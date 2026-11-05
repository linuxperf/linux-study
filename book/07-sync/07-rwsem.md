# 7.7 信号量与读写信号量

> **本节源码**：[`include/linux/semaphore.h`](../../linux/include/linux/semaphore.h)、[`kernel/locking/semaphore.c`](../../linux/kernel/locking/semaphore.c)、[`include/linux/rwsem.h`](../../linux/include/linux/rwsem.h)、[`kernel/locking/rwsem.c`](../../linux/kernel/locking/rwsem.c)、[`include/linux/percpu-rwsem.h`](../../linux/include/linux/percpu-rwsem.h)、[`kernel/locking/percpu-rwsem.c`](../../linux/kernel/locking/percpu-rwsem.c)
> **关键函数**：`down()`、`up()`、`___down_common()`、`down_read()`、`down_write()`、`rwsem_read_trylock()`、`rwsem_down_read_slowpath()`、`rwsem_down_write_slowpath()`、`rwsem_try_write_lock()`、`rwsem_mark_wake()`、`percpu_down_read()`
> **前置知识**：7.6 互斥锁与乐观自旋

## 概述

本节讨论三种可睡眠的锁：

- **计数信号量**（`struct semaphore`）：Dijkstra 意义上的 P/V 操作，允许最多 N 个持有者，且可以由非持有者释放。它是内核中最古老的睡眠锁，如今主要用于少数需要"计数"或"跨任务释放"语义的场合，新代码中的互斥一律应使用 mutex。
- **读写信号量**（`struct rw_semaphore`，rwsem）：可睡眠的读写锁，允许多个读者或一个写者。它是内核中使用最广泛的读写锁，`mmap_lock`、inode 的 `i_rwsem`、文件系统超级块的 `s_umount` 都是 rwsem。
- **per-CPU 读写信号量**（`struct percpu_rw_semaphore`）：读者开销极低、写者极其昂贵，用于几乎从不写的全局状态，如 CPU 热插拔锁、文件系统冻结。

rwsem 在设计上与 mutex 一脉相承：一个原子计数字承载锁状态，`owner` 字段支持乐观自旋，等待队列由 `wait_lock` 保护，并以 `HANDOFF` 位防止饥饿。不同之处在于它要同时处理读者与写者，因此计数字的编码和唤醒策略更为复杂。

## 计数信号量

### 数据结构与实现

[`include/linux/semaphore.h:15-23`](../../linux/include/linux/semaphore.h#L15-L23)（v6.18.60）

```c
struct semaphore {
	raw_spinlock_t		lock;
	unsigned int		count;
	struct list_head	wait_list;

#ifdef CONFIG_DETECT_HUNG_TASK_BLOCKER
	unsigned long		last_holder;
#endif
};
```

信号量的实现非常直接——所有操作都在 `sem->lock` 保护下进行，`count` 只是普通整数：

[`kernel/locking/semaphore.c:91-102`](../../linux/kernel/locking/semaphore.c#L91-L102)（v6.18.60）

```c
void __sched down(struct semaphore *sem)
{
	unsigned long flags;

	might_sleep();
	raw_spin_lock_irqsave(&sem->lock, flags);
	if (likely(sem->count > 0))
		__sem_acquire(sem);
	else
		__down(sem);
	raw_spin_unlock_irqrestore(&sem->lock, flags);
}
```

[`kernel/locking/semaphore.c:220-237`](../../linux/kernel/locking/semaphore.c#L220-L237)（v6.18.60）

```c
void __sched up(struct semaphore *sem)
{
	unsigned long flags;
	DEFINE_WAKE_Q(wake_q);

	raw_spin_lock_irqsave(&sem->lock, flags);

	hung_task_sem_clear_if_holder(sem);

	if (likely(list_empty(&sem->wait_list)))
		sem->count++;
	else
		__up(sem, &wake_q);
	raw_spin_unlock_irqrestore(&sem->lock, flags);
	if (!wake_q_empty(&wake_q))
		wake_up_q(&wake_q);
}
EXPORT_SYMBOL(up);
```

`count > 0` 时 `down()` 直接减一返回；否则进入 `__down()` 排队睡眠。`up()` 在没有等待者时加一，有等待者时**不增加计数**，而是直接把"一个名额"交给队首：

[`kernel/locking/semaphore.c:252-283`](../../linux/kernel/locking/semaphore.c#L252-L283)（v6.18.60）

```c
static inline int __sched ___down_common(struct semaphore *sem, long state,
								long timeout)
{
	struct semaphore_waiter waiter;

	list_add_tail(&waiter.list, &sem->wait_list);
	waiter.task = current;
	waiter.up = false;

	for (;;) {
		if (signal_pending_state(state, current))
			goto interrupted;
		if (unlikely(timeout <= 0))
			goto timed_out;
		__set_current_state(state);
		raw_spin_unlock_irq(&sem->lock);
		timeout = schedule_timeout(timeout);
		raw_spin_lock_irq(&sem->lock);
		if (waiter.up) {
			hung_task_sem_set_holder(sem);
			return 0;
		}
	}

 timed_out:
	list_del(&waiter.list);
	return -ETIME;

 interrupted:
	list_del(&waiter.list);
	return -EINTR;
}
```

[`kernel/locking/semaphore.c:321-329`](../../linux/kernel/locking/semaphore.c#L321-L329)（v6.18.60）

```c
static noinline void __sched __up(struct semaphore *sem,
				  struct wake_q_head *wake_q)
{
	struct semaphore_waiter *waiter = list_first_entry(&sem->wait_list,
						struct semaphore_waiter, list);
	list_del(&waiter->list);
	waiter->up = true;
	wake_q_add(wake_q, waiter->task);
}
```

`__up()` 把等待者从队列移除并设置 `waiter.up = true`，等待者醒来后检查这个标志即知道自己已获得信号量。这是一种严格的 FIFO 移交，不存在抢锁。`hung_task_*` 系列调用（依赖 `CONFIG_DETECT_HUNG_TASK_BLOCKER`，本书配置为 `y`）记录阻塞在哪个信号量上，供 hung task 检测器在报告时打印持有者。

### 为什么新代码不应使用 semaphore 做互斥

- 没有乐观自旋，每次竞争都要睡眠与唤醒；
- 不记录持有者，无法做优先级继承，lockdep 也难以追踪；
- 每次操作都要获取 `raw_spinlock`，即使无竞争也比 mutex 的一次 cmpxchg 慢。

`DEFINE_SEMAPHORE(name, n)` 在 v6.4 起要求显式给出初值，以区分"计数信号量"与"当作互斥使用的二值信号量"。

## 读写信号量

### 核心数据结构

[`include/linux/rwsem.h:48-67`](../../linux/include/linux/rwsem.h#L48-L67)（v6.18.60）

```c
struct rw_semaphore {
	atomic_long_t count;
	/*
	 * Write owner or one of the read owners as well flags regarding
	 * the current state of the rwsem. Can be used as a speculative
	 * check to see if the write owner is running on the cpu.
	 */
	atomic_long_t owner;
#ifdef CONFIG_RWSEM_SPIN_ON_OWNER
	struct optimistic_spin_queue osq; /* spinner MCS lock */
#endif
	raw_spinlock_t wait_lock;
	struct list_head wait_list;
#ifdef CONFIG_DEBUG_RWSEMS
	void *magic;
#endif
#ifdef CONFIG_DEBUG_LOCK_ALLOC
	struct lockdep_map	dep_map;
#endif
};
```

64 位系统上 `count` 的编码：

[`kernel/locking/rwsem.c:82-129`](../../linux/kernel/locking/rwsem.c#L82-L129)（v6.18.60）

```c
/*
 * On 64-bit architectures, the bit definitions of the count are:
 *
 * Bit  0    - writer locked bit
 * Bit  1    - waiters present bit
 * Bit  2    - lock handoff bit
 * Bits 3-7  - reserved
 * Bits 8-62 - 55-bit reader count
 * Bit  63   - read fail bit
 *
 * On 32-bit architectures, the bit definitions of the count are:
 *
 * Bit  0    - writer locked bit
 * Bit  1    - waiters present bit
 * Bit  2    - lock handoff bit
 * Bits 3-7  - reserved
 * Bits 8-30 - 23-bit reader count
 * Bit  31   - read fail bit
 *
 * It is not likely that the most significant bit (read fail bit) will ever
 * be set. This guard bit is still checked anyway in the down_read() fastpath
 * just in case we need to use up more of the reader bits for other purpose
 * in the future.
 *
 * atomic_long_fetch_add() is used to obtain reader lock, whereas
 * atomic_long_cmpxchg() will be used to obtain writer lock.
 *
 * There are three places where the lock handoff bit may be set or cleared.
 * 1) rwsem_mark_wake() for readers		-- set, clear
 * 2) rwsem_try_write_lock() for writers	-- set, clear
 * 3) rwsem_del_waiter()			-- clear
 *
 * For all the above cases, wait_lock will be held. A writer must also
 * be the first one in the wait_list to be eligible for setting the handoff
 * bit. So concurrent setting/clearing of handoff bit is not possible.
 */
#define RWSEM_WRITER_LOCKED	(1UL << 0)
#define RWSEM_FLAG_WAITERS	(1UL << 1)
#define RWSEM_FLAG_HANDOFF	(1UL << 2)
#define RWSEM_FLAG_READFAIL	(1UL << (BITS_PER_LONG - 1))

#define RWSEM_READER_SHIFT	8
#define RWSEM_READER_BIAS	(1UL << RWSEM_READER_SHIFT)
#define RWSEM_READER_MASK	(~(RWSEM_READER_BIAS - 1))
#define RWSEM_WRITER_MASK	RWSEM_WRITER_LOCKED
#define RWSEM_LOCK_MASK		(RWSEM_WRITER_MASK|RWSEM_READER_MASK)
#define RWSEM_READ_FAILED_MASK	(RWSEM_WRITER_MASK|RWSEM_FLAG_WAITERS|\
				 RWSEM_FLAG_HANDOFF|RWSEM_FLAG_READFAIL)
```

```
 63  62                                         8 7     3   2   1   0
┌───┬────────────────────────────────────────────┬───────┬───┬───┬───┐
│ F │          读者计数（每个读者 +0x100）          │ 保留  │ H │ W │ L │
└───┴────────────────────────────────────────────┴───────┴───┴───┴───┘
  │                                                        │   │   └─ RWSEM_WRITER_LOCKED
  └─ RWSEM_FLAG_READFAIL（保护位）                          │   └─ RWSEM_FLAG_WAITERS
                                                           └─ RWSEM_FLAG_HANDOFF
```

*图 7-6：rwsem `count` 字段布局（高位在左）*

`owner` 字段与 mutex 类似，存放写者的 `task_struct` 指针，低位用作标志：

[`kernel/locking/rwsem.c:64-66`](../../linux/kernel/locking/rwsem.c#L64-L66)（v6.18.60）

```c
#define RWSEM_READER_OWNED	(1UL << 0)
#define RWSEM_NONSPINNABLE	(1UL << 1)
#define RWSEM_OWNER_FLAGS_MASK	(RWSEM_READER_OWNED | RWSEM_NONSPINNABLE)
```

- `RWSEM_READER_OWNED`：锁当前由读者持有，`owner` 中的指针只是"最后一个获取读锁的任务"，仅供调试参考。
- `RWSEM_NONSPINNABLE`：禁止乐观自旋，例如读者持锁过久时由自旋者设置。

### 快速路径

[`kernel/locking/rwsem.c:249-274`](../../linux/kernel/locking/rwsem.c#L249-L274)（v6.18.60）

```c
static inline bool rwsem_read_trylock(struct rw_semaphore *sem, long *cntp)
{
	*cntp = atomic_long_add_return_acquire(RWSEM_READER_BIAS, &sem->count);

	if (WARN_ON_ONCE(*cntp < 0))
		rwsem_set_nonspinnable(sem);

	if (!(*cntp & RWSEM_READ_FAILED_MASK)) {
		rwsem_set_reader_owned(sem);
		return true;
	}

	return false;
}

static inline bool rwsem_write_trylock(struct rw_semaphore *sem)
{
	long tmp = RWSEM_UNLOCKED_VALUE;

	if (atomic_long_try_cmpxchg_acquire(&sem->count, &tmp, RWSEM_WRITER_LOCKED)) {
		rwsem_set_owner(sem);
		return true;
	}

	return false;
}
```

**读者**无条件地给 `count` 加一个 `RWSEM_READER_BIAS`，然后检查 `RWSEM_READ_FAILED_MASK`（写者持锁、有等待者、有移交请求或保护位）。只要这些位都为零就成功。注意"有等待者"也会让读者失败：一旦有写者在排队，新读者不再插队，这是防止写者饥饿的第一道防线。

**写者**只有在 `count` 为 0 时才能用 cmpxchg 置位 `RWSEM_WRITER_LOCKED`。

[`kernel/locking/rwsem.c:1254-1269`](../../linux/kernel/locking/rwsem.c#L1254-L1269)（v6.18.60）

```c
static __always_inline int __down_read_common(struct rw_semaphore *sem, int state)
{
	int ret = 0;
	long count;

	preempt_disable();
	if (!rwsem_read_trylock(sem, &count)) {
		if (IS_ERR(rwsem_down_read_slowpath(sem, count, state))) {
			ret = -EINTR;
			goto out;
		}
		DEBUG_RWSEMS_WARN_ON(!is_rwsem_reader_owned(sem), sem);
	}
out:
	preempt_enable();
	return ret;
```

[`kernel/locking/rwsem.c:1311-1322`](../../linux/kernel/locking/rwsem.c#L1311-L1322)（v6.18.60）

```c
static __always_inline int __down_write_common(struct rw_semaphore *sem, int state)
{
	int ret = 0;

	preempt_disable();
	if (unlikely(!rwsem_write_trylock(sem))) {
		if (IS_ERR(rwsem_down_write_slowpath(sem, state)))
			ret = -EINTR;
	}
	preempt_enable();
	return ret;
}
```

与 mutex 一样，rwsem 的加锁过程关闭抢占，以便在乐观自旋时安全地访问 `owner` 指向的任务结构。

### 解锁

[`kernel/locking/rwsem.c:1349-1389`](../../linux/kernel/locking/rwsem.c#L1349-L1389)（v6.18.60）

```c
static inline void __up_read(struct rw_semaphore *sem)
{
	long tmp;

	DEBUG_RWSEMS_WARN_ON(sem->magic != sem, sem);
	DEBUG_RWSEMS_WARN_ON(!is_rwsem_reader_owned(sem), sem);

	preempt_disable();
	rwsem_clear_reader_owned(sem);
	tmp = atomic_long_add_return_release(-RWSEM_READER_BIAS, &sem->count);
	DEBUG_RWSEMS_WARN_ON(tmp < 0, sem);
	if (unlikely((tmp & (RWSEM_LOCK_MASK|RWSEM_FLAG_WAITERS)) ==
		      RWSEM_FLAG_WAITERS)) {
		clear_nonspinnable(sem);
		rwsem_wake(sem);
	}
	preempt_enable();
}

/*
 * unlock after writing
 */
static inline void __up_write(struct rw_semaphore *sem)
{
	long tmp;

	DEBUG_RWSEMS_WARN_ON(sem->magic != sem, sem);
	/*
	 * sem->owner may differ from current if the ownership is transferred
	 * to an anonymous writer by setting the RWSEM_NONSPINNABLE bits.
	 */
	DEBUG_RWSEMS_WARN_ON((rwsem_owner(sem) != current) &&
			    !rwsem_test_oflags(sem, RWSEM_NONSPINNABLE), sem);

	preempt_disable();
	rwsem_clear_owner(sem);
	tmp = atomic_long_fetch_add_release(-RWSEM_WRITER_LOCKED, &sem->count);
	if (unlikely(tmp & RWSEM_FLAG_WAITERS))
		rwsem_wake(sem);
	preempt_enable();
}
```

- 读者解锁减去 `RWSEM_READER_BIAS`。只有当它是**最后一个读者**（`RWSEM_LOCK_MASK` 部分为零）且有等待者时才调用 `rwsem_wake()`。
- 写者解锁减去 `RWSEM_WRITER_LOCKED`，若有等待者则唤醒。

### 读者慢速路径

[`kernel/locking/rwsem.c:992-1028`](../../linux/kernel/locking/rwsem.c#L992-L1028)（v6.18.60）

```c
static struct rw_semaphore __sched *
rwsem_down_read_slowpath(struct rw_semaphore *sem, long count, unsigned int state)
{
	long adjustment = -RWSEM_READER_BIAS;
	long rcnt = (count >> RWSEM_READER_SHIFT);
	struct rwsem_waiter waiter;
	DEFINE_WAKE_Q(wake_q);

	/*
	 * To prevent a constant stream of readers from starving a sleeping
	 * writer, don't attempt optimistic lock stealing if the lock is
	 * very likely owned by readers.
	 */
	if ((atomic_long_read(&sem->owner) & RWSEM_READER_OWNED) &&
	    (rcnt > 1) && !(count & RWSEM_WRITER_LOCKED))
		goto queue;

	/*
	 * Reader optimistic lock stealing.
	 */
	if (!(count & (RWSEM_WRITER_LOCKED | RWSEM_FLAG_HANDOFF))) {
		rwsem_set_reader_owned(sem);
		lockevent_inc(rwsem_rlock_steal);

		/*
		 * Wake up other readers in the wait queue if it is
		 * the first reader.
		 */
		if ((rcnt == 1) && (count & RWSEM_FLAG_WAITERS)) {
			raw_spin_lock_irq(&sem->wait_lock);
			if (!list_empty(&sem->wait_list))
				rwsem_mark_wake(sem, RWSEM_WAKE_READ_OWNED,
						&wake_q);
			raw_spin_unlock_irq(&sem->wait_lock);
			wake_up_q(&wake_q);
		}
		return sem;
```

快速路径失败时读者已经给 `count` 加上了偏置，慢速路径先判断能否"顺手"拿到读锁：如果锁没有被写者持有、也没有移交请求（只是因为 `WAITERS` 位使快速路径失败），读者就**偷锁**（lock stealing）直接返回，并在自己是第一个读者时顺带唤醒队列中的其他读者。但若锁已经被多个读者持有（`rcnt > 1`），就不偷锁而直接排队，避免源源不断的读者饿死正在睡眠的写者。

否则入队等待：

[`kernel/locking/rwsem.c:1030-1097`](../../linux/kernel/locking/rwsem.c#L1030-L1097)（v6.18.60，节选）

```c

queue:
	waiter.task = current;
	waiter.type = RWSEM_WAITING_FOR_READ;
	waiter.timeout = jiffies + RWSEM_WAIT_TIMEOUT;
	waiter.handoff_set = false;

	raw_spin_lock_irq(&sem->wait_lock);
	if (list_empty(&sem->wait_list)) {
		/*
		 * In case the wait queue is empty and the lock isn't owned
		 * by a writer, this reader can exit the slowpath and return
		 * immediately as its RWSEM_READER_BIAS has already been set
		 * in the count.
		 */
		if (!(atomic_long_read(&sem->count) & RWSEM_WRITER_MASK)) {
			/* Provide lock ACQUIRE */
			smp_acquire__after_ctrl_dep();
			raw_spin_unlock_irq(&sem->wait_lock);
			rwsem_set_reader_owned(sem);
			lockevent_inc(rwsem_rlock_fast);
			return sem;
		}
		adjustment += RWSEM_FLAG_WAITERS;
	}
	rwsem_add_waiter(sem, &waiter);

	/* we're now waiting on the lock, but no longer actively locking */
	count = atomic_long_add_return(adjustment, &sem->count);

	rwsem_cond_wake_waiter(sem, count, &wake_q);
	raw_spin_unlock_irq(&sem->wait_lock);
...
	/* wait to be given the lock */
	for (;;) {
		if (!smp_load_acquire(&waiter.task)) {
			/* Matches rwsem_mark_wake()'s smp_store_release(). */
			break;
		}
		if (signal_pending_state(state, current)) {
			raw_spin_lock_irq(&sem->wait_lock);
			if (waiter.task)
				goto out_nolock;
			raw_spin_unlock_irq(&sem->wait_lock);
			/* Ordered by sem->wait_lock against rwsem_mark_wake(). */
			break;
		}
		schedule_preempt_disabled();
		lockevent_inc(rwsem_sleep_reader);
		set_current_state(state);
	}

	if (state == TASK_UNINTERRUPTIBLE)
		hung_task_clear_blocker();

	__set_current_state(TASK_RUNNING);
	lockevent_inc(rwsem_rlock);
	trace_contention_end(sem, 0);
	return sem;
```

入队时把快速路径加上的偏置减掉（`adjustment = -RWSEM_READER_BIAS`），若队列原本为空还要加上 `WAITERS`。等待循环的退出条件是 `waiter.task` 被清空——唤醒者在**唤醒前就已经替读者加好了读者计数**，读者醒来时已经持有读锁，无需再竞争。`smp_load_acquire()` 与唤醒者的 `smp_store_release()` 配对。

### 唤醒策略：rwsem_mark_wake()

`rwsem_mark_wake()` 根据队首等待者的类型决定唤醒谁：

[`kernel/locking/rwsem.c:424-440`](../../linux/kernel/locking/rwsem.c#L424-L440)（v6.18.60）

```c
	waiter = rwsem_first_waiter(sem);

	if (waiter->type == RWSEM_WAITING_FOR_WRITE) {
		if (wake_type == RWSEM_WAKE_ANY) {
			/*
			 * Mark writer at the front of the queue for wakeup.
			 * Until the task is actually later awoken later by
			 * the caller, other writers are able to steal it.
			 * Readers, on the other hand, will block as they
			 * will notice the queued writer.
			 */
			wake_q_add(wake_q, waiter->task);
			lockevent_inc(rwsem_wake_writer);
		}

		return;
	}
```

- **队首是写者**：只唤醒它，由它醒来后自己去竞争（期间其他写者可以抢锁）。
- **队首是读者**：唤醒队列中**所有**读者（最多 `MAX_READERS_WAKEUP` 即 256 个），无论它们是否排在写者之后。这是"阶段公平"（phase-fair）读写锁的思想：读阶段让所有读者一起进入，写者按队列顺序进入。

[`kernel/locking/rwsem.c:508-567`](../../linux/kernel/locking/rwsem.c#L508-L567)（v6.18.60，节选）

```c
	INIT_LIST_HEAD(&wlist);
	list_for_each_entry_safe(waiter, tmp, &sem->wait_list, list) {
		if (waiter->type == RWSEM_WAITING_FOR_WRITE)
			continue;

		woken++;
		list_move_tail(&waiter->list, &wlist);

		/*
		 * Limit # of readers that can be woken up per wakeup call.
		 */
		if (unlikely(woken >= MAX_READERS_WAKEUP))
			break;
	}

	adjustment = woken * RWSEM_READER_BIAS - adjustment;
...
	if (adjustment)
		atomic_long_add(adjustment, &sem->count);

	/* 2nd pass */
	list_for_each_entry_safe(waiter, tmp, &wlist, list) {
		struct task_struct *tsk;

		tsk = waiter->task;
		get_task_struct(tsk);

		/*
		 * Ensure calling get_task_struct() before setting the reader
		 * waiter to nil such that rwsem_down_read_slowpath() cannot
		 * race with do_exit() by always holding a reference count
		 * to the task to wakeup.
		 */
		smp_store_release(&waiter->task, NULL);
		/*
		 * Ensure issuing the wakeup (either by us or someone else)
		 * after setting the reader waiter to nil.
		 */
		wake_q_add_safe(wake_q, tsk);
	}
}
```

唤醒分两遍进行：第一遍把要唤醒的读者移到临时链表并一次性加好读者计数；第二遍逐个清空 `waiter->task` 并加入 `wake_q`。之所以必须先加计数再清 `task`，是因为被唤醒的读者可能根本还没睡着——它看到 `task` 为 NULL 就会进入临界区并很快解锁，如果此时计数还没加上，解锁就会把计数减成错误的值。

### 写者慢速路径

[`kernel/locking/rwsem.c:1110-1149`](../../linux/kernel/locking/rwsem.c#L1110-L1149)（v6.18.60）

```c
static struct rw_semaphore __sched *
rwsem_down_write_slowpath(struct rw_semaphore *sem, int state)
{
	struct rwsem_waiter waiter;
	DEFINE_WAKE_Q(wake_q);

	/* do optimistic spinning and steal lock if possible */
	if (rwsem_can_spin_on_owner(sem) && rwsem_optimistic_spin(sem)) {
		/* rwsem_optimistic_spin() implies ACQUIRE on success */
		return sem;
	}

	/*
	 * Optimistic spinning failed, proceed to the slowpath
	 * and block until we can acquire the sem.
	 */
	waiter.task = current;
	waiter.type = RWSEM_WAITING_FOR_WRITE;
	waiter.timeout = jiffies + RWSEM_WAIT_TIMEOUT;
	waiter.handoff_set = false;

	raw_spin_lock_irq(&sem->wait_lock);
	rwsem_add_waiter(sem, &waiter);

	/* we're now waiting on the lock */
	if (rwsem_first_waiter(sem) != &waiter) {
		rwsem_cond_wake_waiter(sem, atomic_long_read(&sem->count),
				       &wake_q);
		if (!wake_q_empty(&wake_q)) {
			/*
			 * We want to minimize wait_lock hold time especially
			 * when a large number of readers are to be woken up.
			 */
			raw_spin_unlock_irq(&sem->wait_lock);
			wake_up_q(&wake_q);
			raw_spin_lock_irq(&sem->wait_lock);
		}
	} else {
		atomic_long_or(RWSEM_FLAG_WAITERS, &sem->count);
	}
```

写者先尝试乐观自旋（`rwsem_optimistic_spin()`，[`kernel/locking/rwsem.c:816`](../../linux/kernel/locking/rwsem.c#L816) 起）：与 mutex 一样通过 OSQ 排队后在 `owner` 上自旋。不同的是，rwsem 的持有者可能是读者，而读者的身份无法确知、也无法判断它们是否在运行，所以对读者持有的锁只自旋有限的时间（`rspin_threshold`），超时后设置 `RWSEM_NONSPINNABLE`。

自旋失败则入队，在 `wait_lock` 保护下循环调用 `rwsem_try_write_lock()`：

[`kernel/locking/rwsem.c:603-664`](../../linux/kernel/locking/rwsem.c#L603-L664)（v6.18.60）

```c
static inline bool rwsem_try_write_lock(struct rw_semaphore *sem,
					struct rwsem_waiter *waiter)
{
	struct rwsem_waiter *first = rwsem_first_waiter(sem);
	long count, new;

	lockdep_assert_held(&sem->wait_lock);

	count = atomic_long_read(&sem->count);
	do {
		bool has_handoff = !!(count & RWSEM_FLAG_HANDOFF);

		if (has_handoff) {
			/*
			 * Honor handoff bit and yield only when the first
			 * waiter is the one that set it. Otherwisee, we
			 * still try to acquire the rwsem.
			 */
			if (first->handoff_set && (waiter != first))
				return false;
		}

		new = count;

		if (count & RWSEM_LOCK_MASK) {
			/*
			 * A waiter (first or not) can set the handoff bit
			 * if it is an RT task or wait in the wait queue
			 * for too long.
			 */
			if (has_handoff || (!rt_or_dl_task(waiter->task) &&
					    !time_after(jiffies, waiter->timeout)))
				return false;

			new |= RWSEM_FLAG_HANDOFF;
		} else {
			new |= RWSEM_WRITER_LOCKED;
			new &= ~RWSEM_FLAG_HANDOFF;

			if (list_is_singular(&sem->wait_list))
				new &= ~RWSEM_FLAG_WAITERS;
		}
	} while (!atomic_long_try_cmpxchg_acquire(&sem->count, &count, new));

	/*
	 * We have either acquired the lock with handoff bit cleared or set
	 * the handoff bit. Only the first waiter can have its handoff_set
	 * set here to enable optimistic spinning in slowpath loop.
	 */
	if (new & RWSEM_FLAG_HANDOFF) {
		first->handoff_set = true;
		lockevent_inc(rwsem_wlock_handoff);
		return false;
	}

	/*
	 * Have rwsem_try_write_lock() fully imply rwsem_del_waiter() on
	 * success.
	 */
	list_del(&waiter->list);
	rwsem_set_owner(sem);
	return true;
```

- 锁空闲时置位 `WRITER_LOCKED` 并清除 `HANDOFF`；若自己是最后一个等待者，同时清除 `WAITERS`。
- 锁被占用时，若等待者是实时 / Deadline 任务，或者等待已超过 `RWSEM_WAIT_TIMEOUT`，就设置 `HANDOFF`。

[`kernel/locking/rwsem.c:353-358`](../../linux/kernel/locking/rwsem.c#L353-L358)（v6.18.60）

```c
/*
 * The typical HZ value is either 250 or 1000. So set the minimum waiting
 * time to at least 4ms or 1 jiffy (if it is higher than 4ms) in the wait
 * queue before initiating the handoff protocol.
 */
#define RWSEM_WAIT_TIMEOUT	DIV_ROUND_UP(HZ, 250)
```

`HANDOFF` 置位后，读者快速路径会失败（它属于 `RWSEM_READ_FAILED_MASK`），其他写者在 `rwsem_try_write_lock()` 中也会让步（只有设置它的队首才能获取），从而保证队首写者在锁释放后一定能拿到锁。设置了 `HANDOFF` 的队首还会在 `owner` 上自旋（`rwsem_spin_on_owner()`），以便在前任写者释放的瞬间接手，而不必等待一次唤醒。

## per-CPU 读写信号量

对于"读者极多、写者极罕见"的全局锁，即使 rwsem 读者的一次原子加法也会成为瓶颈——所有 CPU 都在修改同一缓存行。`percpu_rw_semaphore` 把读者计数分散到每个 CPU：

[`include/linux/percpu-rwsem.h:13-22`](../../linux/include/linux/percpu-rwsem.h#L13-L22)（v6.18.60）

```c
struct percpu_rw_semaphore {
	struct rcu_sync		rss;
	unsigned int __percpu	*read_count;
	struct rcuwait		writer;
	wait_queue_head_t	waiters;
	atomic_t		block;
#ifdef CONFIG_DEBUG_LOCK_ALLOC
	struct lockdep_map	dep_map;
#endif
};
```

[`include/linux/percpu-rwsem.h:48-73`](../../linux/include/linux/percpu-rwsem.h#L48-L73)（v6.18.60）

```c
static inline void percpu_down_read_internal(struct percpu_rw_semaphore *sem,
					     bool freezable)
{
	might_sleep();

	rwsem_acquire_read(&sem->dep_map, 0, 0, _RET_IP_);

	preempt_disable();
	/*
	 * We are in an RCU-sched read-side critical section, so the writer
	 * cannot both change sem->state from readers_fast and start checking
	 * counters while we are here. So if we see !sem->state, we know that
	 * the writer won't be checking until we're past the preempt_enable()
	 * and that once the synchronize_rcu() is done, the writer will see
	 * anything we did within this RCU-sched read-size critical section.
	 */
	if (likely(rcu_sync_is_idle(&sem->rss)))
		this_cpu_inc(*sem->read_count);
	else
		__percpu_down_read(sem, false, freezable); /* Unconditional memory barrier */
	/*
	 * The preempt_enable() prevents the compiler from
	 * bleeding the critical section out.
	 */
	preempt_enable();
}
```

没有写者时（`rcu_sync_is_idle()`），读者只在本 CPU 的计数器上加一，完全不涉及共享缓存行，也没有内存屏障。写者 `percpu_down_write()`（`kernel/locking/percpu-rwsem.c`）先通过 `rcu_sync_enter()` 把锁切换到"慢速模式"并等待一个 RCU 宽限期，确保此后所有读者都能看到这一切换、转而走 `__percpu_down_read()` 慢速路径；然后等待各 CPU 计数之和归零。写者的代价是至少一个 RCU 宽限期（毫秒级），因此只适合极少写的场景。

典型使用者：CPU 热插拔锁 `cpu_hotplug_lock`（`cpus_read_lock()`）、文件系统冻结 `sb->s_writers`、cgroup 的 `cgroup_threadgroup_rwsem`。

## 关键细节 / 设计考量

### 三种睡眠锁的比较

| 特性 | `semaphore` | `mutex` | `rw_semaphore` | `percpu_rw_semaphore` |
|---|---|---|---|---|
| 持有者数量 | ≤ N | 1 | 多读 / 单写 | 多读 / 单写 |
| 记录持有者 | 否（仅调试） | 是 | 写者是 | 否 |
| 乐观自旋 | 否 | 是 | 是（写者；对读者有限） | 否 |
| 无竞争读开销 | 自旋锁 + 计数 | — | 一次原子加 | per-CPU 加一 |
| 可由非持有者释放 | 是 | 否 | 否（`up_read_non_owner()` 例外） | 否 |
| 防饥饿 | FIFO 移交 | HANDOFF | HANDOFF + 写者排队阻挡新读者 | 写者等待宽限期 |

*表 7-6：内核睡眠锁对比*

### mmap_lock 与 per-VMA 锁

`mm_struct->mmap_lock` 是最著名的 rwsem，缺页时读、`mmap`/`munmap` 时写，长期以来是多线程程序的主要扩展性瓶颈。v6.4 引入的 per-VMA 锁（`CONFIG_PER_VMA_LOCK`，4.7、4.8 节）让大多数缺页只锁单个 VMA，而 `mm->mm_lock_seq` 顺序计数用于检测 `mmap_lock` 写者是否修改过 VMA（7.5 节）。这是"缩小锁粒度 + 顺序计数验证"的典型优化。

### downgrade_write

rwsem 支持把写锁原子地降级为读锁（`downgrade_write()`），降级时唤醒排在前面的读者。这允许"先独占地修改，再与其他读者共享地读取"而不必释放锁再重新竞争。

## 动手实验

### 观察 mmap_lock 竞争

```bash
# 多线程程序频繁 mmap/munmap 时
sudo perf lock contention -a -b -Y rwsem -- sleep 5
```

内核为 `mmap_lock` 专门提供了跟踪点：

```bash
sudo bpftrace -e '
tracepoint:mmap_lock:mmap_lock_start_locking { @start[tid] = nsecs; }
tracepoint:mmap_lock:mmap_lock_acquire_returned /@start[tid]/ {
	@wait_us[args->write ? "write" : "read"] = hist((nsecs - @start[tid]) / 1000);
	delete(@start[tid]);
}'
```

### 查看阻塞在 rwsem 上的任务

`CONFIG_DETECT_HUNG_TASK_BLOCKER=y` 时，若任务在 `TASK_UNINTERRUPTIBLE` 状态阻塞超过 `kernel.hung_task_timeout_secs`（默认 120 秒），hung task 报告会附带它所等待的 mutex / semaphore / rwsem 及其持有者信息。

## 小结

- 计数信号量在自旋锁保护下维护计数，`up()` 有等待者时直接把名额移交队首；新代码中应以 mutex 代替二值信号量。
- rwsem 的 `count` 编码写者位、等待者位、移交位与 55 位读者计数；读者快速路径是一次原子加，写者快速路径是一次 cmpxchg。
- 读者遇到等待者时不再插队；读者慢速路径可在锁未被写者持有时偷锁，但锁已被多个读者持有时直接排队，防止写者饥饿。
- 唤醒时若队首为读者，就唤醒所有读者（阶段公平），并在唤醒前替它们加好读者计数；写者等待超时或为实时任务时设置 `HANDOFF`，保证锁移交给队首。
- `percpu_rw_semaphore` 把读者计数分散到 per-CPU 变量，读端几乎零开销，写端需等待 RCU 宽限期。

## 参考

- `linux/Documentation/locking/locktypes.rst`
- `linux/Documentation/locking/percpu-rw-semaphore.rst`
- Waiman Long, "locking/rwsem: Rwsem rearchitecture part 2" 补丁系列，2019（读者乐观自旋与 owner 编码）
- `linux/include/trace/events/mmap_lock.h`（mmap_lock 跟踪点）
