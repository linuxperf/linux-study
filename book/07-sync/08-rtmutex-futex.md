# 7.8 rt_mutex、优先级继承与 futex

> **本节源码**：`include/linux/rtmutex.h`、`kernel/locking/rtmutex_common.h`、`kernel/locking/rtmutex.c`、`kernel/locking/rtmutex_api.c`、`include/linux/sched.h`、`include/linux/futex.h`、`kernel/futex/futex.h`、`kernel/futex/core.c`、`kernel/futex/waitwake.c`、`kernel/futex/pi.c`、`kernel/futex/syscalls.c`
> **关键函数**：`__rt_mutex_lock()`、`task_blocks_on_rt_mutex()`、`rt_mutex_adjust_prio_chain()`、`rt_mutex_adjust_prio()`、`mark_wakeup_next_waiter()`、`do_futex()`、`futex_wait_setup()`、`__futex_wait()`、`futex_wake()`、`__futex_hash()`、`futex_lock_pi()`
> **前置知识**：3.6 实时调度类（优先级反转与优先级继承）、7.6 互斥锁与乐观自旋

## 概述

本节讨论两个紧密相关的机制。

**rt_mutex** 是支持**优先级继承**（Priority Inheritance，PI）的睡眠锁。3.6 节介绍过优先级反转问题：低优先级任务持锁，高优先级任务等锁，中优先级任务抢占了持锁者，使高优先级任务被无限推迟。rt_mutex 的解决办法是让持锁者临时继承所有等待者中的最高优先级；如果持锁者自己又在等另一把 rt_mutex，提升还要沿着"阻塞链"一路传递下去。普通内核中 rt_mutex 主要服务于 PI futex、RCU 优先级提升和少数驱动；在 PREEMPT_RT 内核中，`spinlock_t`、`mutex`、`rwsem` 全部改由它实现。

**futex**（Fast Userspace muTEX）是用户态同步的内核支撑。glibc 的 `pthread_mutex`、条件变量、信号量都建立在 futex 之上。它的核心思想是：无竞争时完全在用户态用原子指令完成加解锁；只有需要睡眠或唤醒时才进入内核，由内核按"用户态地址"维护等待队列。PI futex 则在内核中为用户态锁创建 rt_mutex，把优先级继承带到用户态。

## rt_mutex

### 核心数据结构

```c
// include/linux/rtmutex.h:23-27（v6.18.60）
struct rt_mutex_base {
	raw_spinlock_t		wait_lock;
	struct rb_root_cached   waiters;
	struct task_struct	*owner;
};
```

`owner` 与 mutex 一样是持锁者指针，最低位 `RT_MUTEX_HAS_WAITERS` 表示有等待者（迫使解锁走慢速路径）。与 mutex 的 FIFO 链表不同，等待者按**优先级**组织在红黑树 `waiters` 中，最左节点就是最高优先级的等待者（"top waiter"）。

```c
// kernel/locking/rtmutex_common.h:32-59（v6.18.60）
struct rt_waiter_node {
	struct rb_node	entry;
	int		prio;
	u64		deadline;
};

/*
 * This is the control structure for tasks blocked on a rt_mutex,
 * which is allocated on the kernel stack on of the blocked task.
 *
 * @tree:		node to enqueue into the mutex waiters tree
 * @pi_tree:		node to enqueue into the mutex owner waiters tree
 * @task:		task reference to the blocked task
 * @lock:		Pointer to the rt_mutex on which the waiter blocks
 * @wake_state:		Wakeup state to use (TASK_NORMAL or TASK_RTLOCK_WAIT)
 * @ww_ctx:		WW context pointer
 *
 * @tree is ordered by @lock->wait_lock
 * @pi_tree is ordered by rt_mutex_owner(@lock)->pi_lock
 */
struct rt_mutex_waiter {
	struct rt_waiter_node	tree;
	struct rt_waiter_node	pi_tree;
	struct task_struct	*task;
	struct rt_mutex_base	*lock;
	unsigned int		wake_state;
	struct ww_acquire_ctx	*ww_ctx;
};
```

每个等待者同时挂在两棵树上：

- `tree`：挂在锁的 `waiters` 树中，由 `lock->wait_lock` 保护；
- `pi_tree`：若它是该锁的 top waiter，还挂在**持锁者**的 `pi_waiters` 树中，由持锁者的 `pi_lock` 保护。

`task_struct` 中与 PI 有关的字段：

```c
// include/linux/sched.h:1228-1239（v6.18.60）
	/* Protection of the PI data structures: */
	raw_spinlock_t			pi_lock;

	struct wake_q_node		wake_q;

#ifdef CONFIG_RT_MUTEXES
	/* PI waiters blocked on a rt_mutex held by this task: */
	struct rb_root_cached		pi_waiters;
	/* Updated under owner's pi_lock and rq lock */
	struct task_struct		*pi_top_task;
	/* Deadlock detection and priority inheritance handling: */
	struct rt_mutex_waiter		*pi_blocked_on;
```

`pi_waiters` 收集"阻塞在我所持有的各把锁上的 top waiter"，其最左节点就是我应该继承的优先级来源；`pi_blocked_on` 指向我自己正在等待的那个 waiter，用于沿阻塞链向上走。

```
  任务 A（prio 10）          任务 B（prio 50）            任务 C（prio 90）
  pi_blocked_on ──┐        pi_blocked_on ──┐           （持有 L2）
                  ▼                        ▼
              waiter_A                 waiter_B
                  │ tree                   │ tree
                  ▼                        ▼
   L1.waiters ◀───┘        L2.waiters ◀────┘
   L1.owner = B            L2.owner = C
                  │ pi_tree                │ pi_tree
                  ▼                        ▼
   B.pi_waiters = {waiter_A}  C.pi_waiters = {waiter_B}

   结果：B 继承 A 的 10，C 继承 B 的有效优先级 10
```

*图 7-7：两级 PI 阻塞链（数值越小优先级越高）*

### 加锁：快速路径与阻塞

```c
// kernel/locking/rtmutex.c:224-227（v6.18.60）
static __always_inline bool rt_mutex_try_acquire(struct rt_mutex_base *lock)
{
	return rt_mutex_cmpxchg_acquire(lock, NULL, current);
}
```

```c
// kernel/locking/rtmutex.c:1813-1822（v6.18.60）
static __always_inline int __rt_mutex_lock(struct rt_mutex_base *lock,
					   unsigned int state)
{
	lockdep_assert(!current->pi_blocked_on);

	if (likely(rt_mutex_try_acquire(lock)))
		return 0;

	return rt_mutex_slowlock(lock, NULL, state);
}
```

快速路径与 mutex 相同，是一次 `owner: NULL → current` 的 cmpxchg。失败后进入 `rt_mutex_slowlock()`，获取 `wait_lock`，再次尝试后调用 `task_blocks_on_rt_mutex()` 把自己挂入等待树：

```c
// kernel/locking/rtmutex.c:1203-1245（v6.18.60）
static int __sched task_blocks_on_rt_mutex(struct rt_mutex_base *lock,
					   struct rt_mutex_waiter *waiter,
					   struct task_struct *task,
					   struct ww_acquire_ctx *ww_ctx,
					   enum rtmutex_chainwalk chwalk,
					   struct wake_q_head *wake_q)
{
	struct task_struct *owner = rt_mutex_owner(lock);
	struct rt_mutex_waiter *top_waiter = waiter;
	struct rt_mutex_base *next_lock;
	int chain_walk = 0, res;

	lockdep_assert_held(&lock->wait_lock);

	/*
	 * Early deadlock detection. We really don't want the task to
	 * enqueue on itself just to untangle the mess later. It's not
	 * only an optimization. We drop the locks, so another waiter
	 * can come in before the chain walk detects the deadlock. So
	 * the other will detect the deadlock and return -EDEADLOCK,
	 * which is wrong, as the other waiter is not in a deadlock
	 * situation.
	 *
	 * Except for ww_mutex, in that case the chain walk must already deal
	 * with spurious cycles, see the comments at [3] and [6].
	 */
	if (owner == task && !(build_ww_mutex() && ww_ctx))
		return -EDEADLK;

	raw_spin_lock(&task->pi_lock);
	waiter->task = task;
	waiter->lock = lock;
	waiter_update_prio(waiter, task);
	waiter_clone_prio(waiter, task);

	/* Get the top priority waiter on the lock */
	if (rt_mutex_has_waiters(lock))
		top_waiter = rt_mutex_top_waiter(lock);
	rt_mutex_enqueue(lock, waiter);

	task->pi_blocked_on = waiter;

	raw_spin_unlock(&task->pi_lock);
```

```c
// kernel/locking/rtmutex.c:1262-1304（v6.18.60）
	if (!owner)
		return 0;

	raw_spin_lock(&owner->pi_lock);
	if (waiter == rt_mutex_top_waiter(lock)) {
		rt_mutex_dequeue_pi(owner, top_waiter);
		rt_mutex_enqueue_pi(owner, waiter);

		rt_mutex_adjust_prio(lock, owner);
		if (owner->pi_blocked_on)
			chain_walk = 1;
	} else if (rt_mutex_cond_detect_deadlock(waiter, chwalk)) {
		chain_walk = 1;
	}

	/* Store the lock on which owner is blocked or NULL */
	next_lock = task_blocked_on_lock(owner);

	raw_spin_unlock(&owner->pi_lock);
	/*
	 * Even if full deadlock detection is on, if the owner is not
	 * blocked itself, we can avoid finding this out in the chain
	 * walk.
	 */
	if (!chain_walk || !next_lock)
		return 0;

	/*
	 * The owner can't disappear while holding a lock,
	 * so the owner struct is protected by wait_lock.
	 * Gets dropped in rt_mutex_adjust_prio_chain()!
	 */
	get_task_struct(owner);

	raw_spin_unlock_irq_wake(&lock->wait_lock, wake_q);

	res = rt_mutex_adjust_prio_chain(owner, chwalk, lock,
					 next_lock, waiter, task);

	raw_spin_lock_irq(&lock->wait_lock);

	return res;
}
```

这段代码的关键步骤：

1. **早期死锁检测**：如果锁的持有者就是自己，直接返回 `-EDEADLK`。
2. 在 `task->pi_lock` 保护下把 waiter 加入锁的等待树，并设置 `task->pi_blocked_on`。
3. 如果新 waiter 成为了锁的 top waiter，就在持锁者的 `pi_waiters` 中用它替换旧的 top waiter，再调用 `rt_mutex_adjust_prio()` 重新计算持锁者的优先级。
4. 如果持锁者自己也阻塞在另一把锁上（`owner->pi_blocked_on` 非空），需要**沿链传播**，调用 `rt_mutex_adjust_prio_chain()`。

`rt_mutex_adjust_prio()` 只是取出持锁者 `pi_waiters` 中最高优先级的等待任务，交给调度器：

```c
// kernel/locking/rtmutex.c:527-540（v6.18.60）
static __always_inline void rt_mutex_adjust_prio(struct rt_mutex_base *lock,
						 struct task_struct *p)
{
	struct task_struct *pi_task = NULL;

	lockdep_assert_held(&lock->wait_lock);
	lockdep_assert(rt_mutex_owner(lock) == p);
	lockdep_assert_held(&p->pi_lock);

	if (task_has_pi_waiters(p))
		pi_task = task_top_pi_waiter(p)->task;

	rt_mutex_setprio(p, pi_task);
}
```

`rt_mutex_setprio()`（`kernel/sched/core.c:7352`）把 `p->pi_top_task` 设为该任务，按其有效优先级更新 `p->prio`，必要时把 `p` 从公平调度类迁到实时或 Deadline 调度类并重新入队（3.1、3.6 节）。

### 阻塞链传播

`rt_mutex_adjust_prio_chain()` 是 rt_mutex 中最复杂的函数，它以"每步最多持有两把锁"的方式沿阻塞链向上走。函数前的注释给出了每一步的操作与保护：

```c
// kernel/locking/rtmutex.c:648-673（v6.18.60）
 *	again:
 *	  loop_sanity_check();
 *	retry:
 * [1]	  lock(task->pi_lock);			[R] acquire [P1]
 * [2]	  waiter = task->pi_blocked_on;		[P1]
 * [3]	  check_exit_conditions_1();		[P1]
 * [4]	  lock = waiter->lock;			[P1]
 * [5]	  if (!try_lock(lock->wait_lock)) {	[P1] try to acquire [L]
 *	    unlock(task->pi_lock);		release [P1]
 *	    goto retry;
 *	  }
 * [6]	  check_exit_conditions_2();		[P1] + [L]
 * [7]	  requeue_lock_waiter(lock, waiter);	[P1] + [L]
 * [8]	  unlock(task->pi_lock);		release [P1]
 *	  put_task_struct(task);		release [R]
 * [9]	  check_exit_conditions_3();		[L]
 * [10]	  task = owner(lock);			[L]
 *	  get_task_struct(task);		[L] acquire [R]
 *	  lock(task->pi_lock);			[L] acquire [P2]
 * [11]	  requeue_pi_waiter(tsk, waiters(lock));[P2] + [L]
 * [12]	  check_exit_conditions_4();		[P2] + [L]
 * [13]	  unlock(task->pi_lock);		release [P2]
 *	  unlock(lock->wait_lock);		release [L]
 *	  goto again;
 *
 * Where P1 is the blocking task and P2 is the lock owner; going up one step
```

每一轮处理链上的一个"任务—锁"对：用阻塞任务的新优先级在锁的等待树中重新排序（步骤 7），再在锁持有者的 `pi_waiters` 中更新 top waiter 并调整其优先级（步骤 11），然后把持锁者当作下一轮的"阻塞任务"。由于中途会释放锁，链可能在走的过程中发生变化，四处 `check_exit_conditions` 负责检测并提前结束。

由于整个过程不持有全局锁，深度过大的链会导致长时间遍历。链长超过 `max_lock_depth`（默认 1024，`kernel/locking/rtmutex_api.c:14`，可通过 `/proc/sys/kernel/max_lock_depth` 调整）时放弃传播并报告。开启死锁检测时（futex 路径总是开启），若链回到了起点任务，则返回 `-EDEADLK`。

### 解锁：去提升与唤醒

```c
// kernel/locking/rtmutex.c:1472-1478（v6.18.60）
static __always_inline void __rt_mutex_unlock(struct rt_mutex_base *lock)
{
	if (likely(rt_mutex_cmpxchg_release(lock, current, NULL)))
		return;

	rt_mutex_slowunlock(lock);
}
```

慢速解锁 `rt_mutex_slowunlock()` 调用 `mark_wakeup_next_waiter()`：

```c
// kernel/locking/rtmutex.c:1312-1341（v6.18.60）
static void __sched mark_wakeup_next_waiter(struct rt_wake_q_head *wqh,
					    struct rt_mutex_base *lock)
{
	struct rt_mutex_waiter *waiter;

	lockdep_assert_held(&lock->wait_lock);

	raw_spin_lock(&current->pi_lock);

	waiter = rt_mutex_top_waiter(lock);

	/*
	 * Remove it from current->pi_waiters and deboost.
	 *
	 * We must in fact deboost here in order to ensure we call
	 * rt_mutex_setprio() to update p->pi_top_task before the
	 * task unblocks.
	 */
	rt_mutex_dequeue_pi(current, waiter);
	rt_mutex_adjust_prio(lock, current);

	/*
	 * As we are waking up the top waiter, and the waiter stays
	 * queued on the lock until it gets the lock, this lock
	 * obviously has waiters. Just set the bit here and this has
	 * the added benefit of forcing all new tasks into the
	 * slow path making sure no task of lower priority than
	 * the top waiter can steal this lock.
	 */
	lock->owner = (void *) RT_MUTEX_HAS_WAITERS;
```

解锁者先把 top waiter 从自己的 `pi_waiters` 中移除并**立即去提升**（恢复到剩余 PI 来源或自身的正常优先级），再唤醒 top waiter。`owner` 被设为只有 `HAS_WAITERS` 位的"无主但有等待者"状态，迫使所有新来者走慢速路径，在那里按优先级与 top waiter 比较——只有优先级更高的任务才能偷锁（`rt_mutex_steal()`），从而保证高优先级等待者不会被低优先级的新来者抢先。

被唤醒的 top waiter 在 `rt_mutex_slowlock_block()`（`kernel/locking/rtmutex.c:1610`）的循环中调用 `try_to_take_rt_mutex()` 取得锁。该循环中也有乐观自旋：只有 top waiter 会在持锁者上自旋（`rtmutex_spin_on_owner()`），其他等待者直接睡眠。

## futex

### 基本思想

用户态锁的状态保存在一个 32 位整数（futex word）中。以 glibc 的普通互斥锁为例（伪代码，非源码原文）：

```c
// 伪代码：基于 futex 的简单互斥锁（0 = 空闲，1 = 已锁，2 = 已锁且可能有等待者）
void lock(int *f) {
	int c = cmpxchg(f, 0, 1);
	if (c == 0) return;                     /* 快速路径：用户态完成 */
	if (c != 2) c = xchg(f, 2);
	while (c != 0) {
		futex(f, FUTEX_WAIT, 2, ...);   /* 仅当 *f 仍为 2 时睡眠 */
		c = xchg(f, 2);
	}
}
void unlock(int *f) {
	if (atomic_dec(f) != 1) {               /* 原值为 2：可能有等待者 */
		*f = 0;
		futex(f, FUTEX_WAKE, 1);
	}
}
```

内核只提供两个基本操作：`FUTEX_WAIT(uaddr, val)`——如果 `*uaddr` 仍等于 `val` 就睡眠；`FUTEX_WAKE(uaddr, n)`——唤醒在 `uaddr` 上睡眠的至多 `n` 个任务。"比较后睡眠"必须是原子的，否则用户态检查之后、进入睡眠之前发生的唤醒就会丢失。

### 系统调用入口

```c
// kernel/futex/syscalls.c:112-130（v6.18.60）
long do_futex(u32 __user *uaddr, int op, u32 val, ktime_t *timeout,
		u32 __user *uaddr2, u32 val2, u32 val3)
{
	unsigned int flags = futex_to_flags(op);
	int cmd = op & FUTEX_CMD_MASK;

	if (flags & FLAGS_CLOCKRT) {
		if (cmd != FUTEX_WAIT_BITSET &&
		    cmd != FUTEX_WAIT_REQUEUE_PI &&
		    cmd != FUTEX_LOCK_PI2)
			return -ENOSYS;
	}

	switch (cmd) {
	case FUTEX_WAIT:
		val3 = FUTEX_BITSET_MATCH_ANY;
		fallthrough;
	case FUTEX_WAIT_BITSET:
		return futex_wait(uaddr, flags, val, timeout, val3);
```

经典的 `futex()` 系统调用用 `op` 多路复用十几种操作。v5.16 起增加了 `futex_waitv()`（同时等待多个 futex），v6.7 起又增加了独立的 `futex_wake()`、`futex_wait()`、`futex_requeue()` 系统调用（`kernel/futex/syscalls.c:318-442`），支持 8/16/32/64 位 futex word 与 NUMA 感知。

### 哈希桶与 futex_q

内核不为每个 futex 分配对象，而是把等待者挂在一个全局哈希表中：

```c
// kernel/futex/futex.h:143-147（v6.18.60）
struct futex_hash_bucket {
	atomic_t waiters;
	spinlock_t lock;
	struct plist_head chain;
} ____cacheline_aligned_in_smp;
```

```c
// kernel/futex/futex.h:200-217（v6.18.60）
struct futex_q {
	struct plist_node list;

	struct task_struct *task;
	spinlock_t *lock_ptr;
	futex_wake_fn *wake;
	void *wake_data;
	union futex_key key;
	struct futex_pi_state *pi_state;
	struct rt_mutex_waiter *rt_waiter;
	union futex_key *requeue_pi_key;
	u32 bitset;
	atomic_t requeue_state;
	struct futex_private_hash *drop_fph;
#ifdef CONFIG_PREEMPT_RT
	struct rcuwait requeue_wait;
#endif
} __randomize_layout;
```

`futex_key` 唯一标识一个 futex：私有 futex（`FUTEX_PRIVATE_FLAG`，同一进程内的线程间同步）用"`mm` 指针 + 虚拟地址"；共享 futex（跨进程，如共享内存中的锁）则用"inode 序号 + 页偏移"或"`mm` 序号 + 地址"，保证不同进程映射同一物理位置时得到相同的键（`get_futex_key()`，`kernel/futex/core.c:516`）。

```c
// kernel/futex/core.c:372-411（v6.18.60）
static struct futex_hash_bucket *
__futex_hash(union futex_key *key, struct futex_private_hash *fph, struct futex_private_hash **fph_p)
{
	int node = key->both.node;
	u32 hash;

#ifdef CONFIG_FUTEX_PRIVATE_HASH
	if (node == FUTEX_NO_NODE && futex_key_is_private(key)) {
		if (!fph)
			fph = rcu_dereference(key->private.mm->futex_phash);
		if (fph && fph->hash_mask) {
			if (fph_p)
				*fph_p = fph;
			return __futex_hash_private(key, fph);
		}
	}
#endif

	hash = jhash2((u32 *)key,
		      offsetof(typeof(*key), both.offset) / sizeof(u32),
		      key->both.offset);

	if (node == FUTEX_NO_NODE) {
		/*
		 * In case of !FLAGS_NUMA, use some unused hash bits to pick a
		 * node -- this ensures regular futexes are interleaved across
		 * the nodes and avoids having to allocate multiple
		 * hash-tables.
		 *
		 * NOTE: this isn't perfectly uniform, but it is fast and
		 * handles sparse node masks.
		 */
		node = (hash >> futex_hashshift) % nr_node_ids;
		if (!node_possible(node)) {
			node = find_next_bit_wrap(node_possible_map.bits,
						  nr_node_ids, node);
		}
	}

	return &futex_queues[node][hash & futex_hashmask];
```

哈希表的大小在启动时确定（`futex_init()`），默认每个 CPU 256 个桶并按 NUMA 节点分配（`kernel/futex/core.c:2017-2051`）。本书配置开启了 `CONFIG_FUTEX_PRIVATE_HASH`（v6.16 引入）：多线程进程在创建线程时（`kernel/fork.c:2307` 调用 `futex_hash_allocate_default()`）会获得一个**进程私有**的哈希表，其私有 futex 不再与其他进程共用全局桶，消除了跨进程的哈希冲突与锁竞争；进程也可以用 `prctl(PR_FUTEX_HASH, ...)` 自行设定桶数。

### FUTEX_WAIT

```c
// kernel/futex/waitwake.c:595-669（v6.18.60）
int futex_wait_setup(u32 __user *uaddr, u32 val, unsigned int flags,
		     struct futex_q *q, union futex_key *key2,
		     struct task_struct *task)
{
	u32 uval;
	int ret;

	/*
	 * Access the page AFTER the hash-bucket is locked.
	 * Order is important:
	 *
	 *   Userspace waiter: val = var; if (cond(val)) futex_wait(&var, val);
	 *   Userspace waker:  if (cond(var)) { var = new; futex_wake(&var); }
	 *
	 * The basic logical guarantee of a futex is that it blocks ONLY
	 * if cond(var) is known to be true at the time of blocking, for
	 * any cond.  If we locked the hash-bucket after testing *uaddr, that
	 * would open a race condition where we could block indefinitely with
	 * cond(var) false, which would violate the guarantee.
	 *
	 * On the other hand, we insert q and release the hash-bucket only
	 * after testing *uaddr.  This guarantees that futex_wait() will NOT
	 * absorb a wakeup if *uaddr does not match the desired values
	 * while the syscall executes.
	 */
retry:
	ret = get_futex_key(uaddr, flags, &q->key, FUTEX_READ);
	if (unlikely(ret != 0))
		return ret;

retry_private:
	if (1) {
		CLASS(hbr, hbr)(&q->key);
		auto hb = hbr.hb;

		futex_q_lock(q, hb);

		ret = futex_get_value_locked(&uval, uaddr);

		if (ret) {
			futex_q_unlock(hb);

			ret = get_user(uval, uaddr);
			if (ret)
				return ret;

			if (!(flags & FLAGS_SHARED))
				goto retry_private;

			goto retry;
		}

		if (uval != val) {
			futex_q_unlock(hb);
			return -EWOULDBLOCK;
		}

		if (key2 && futex_match(&q->key, key2)) {
			futex_q_unlock(hb);
			return -EINVAL;
		}

		/*
		 * The task state is guaranteed to be set before another task can
		 * wake it. set_current_state() is implemented using smp_store_mb() and
		 * futex_queue() calls spin_unlock() upon completion, both serializing
		 * access to the hash list and forcing another memory barrier.
		 */
		if (task == current)
			set_current_state(TASK_INTERRUPTIBLE|TASK_FREEZABLE);
		futex_queue(q, hb, task);
	}

	return ret;
}
```

`futex_wait_setup()` 实现了"比较后睡眠"的原子性：**先锁住哈希桶，再读用户态的值**。如果值已改变，返回 `-EWOULDBLOCK` 让用户态重试；否则在仍持有桶锁的情况下设置任务状态并入队（`futex_queue()` 最后释放桶锁）。唤醒方要唤醒必须先拿到同一个桶锁，因此不可能出现"值已改变、但等待者未看到也未被唤醒"的情况。

用户态内存可能不在内存中（缺页），而持有自旋锁时不能处理缺页。`futex_get_value_locked()` 在关闭缺页处理的情况下读取；若失败就释放桶锁，用普通的 `get_user()` 触发缺页把页面调入，再重试。

```c
// kernel/futex/waitwake.c:671-709（v6.18.60）
int __futex_wait(u32 __user *uaddr, unsigned int flags, u32 val,
		 struct hrtimer_sleeper *to, u32 bitset)
{
	struct futex_q q = futex_q_init;
	int ret;

	if (!bitset)
		return -EINVAL;

	q.bitset = bitset;

retry:
	/*
	 * Prepare to wait on uaddr. On success, it holds hb->lock and q
	 * is initialized.
	 */
	ret = futex_wait_setup(uaddr, val, flags, &q, NULL, current);
	if (ret)
		return ret;

	/* futex_queue and wait for wakeup, timeout, or a signal. */
	futex_do_wait(&q, to);

	/* If we were woken (and unqueued), we succeeded, whatever. */
	if (!futex_unqueue(&q))
		return 0;

	if (to && !to->task)
		return -ETIMEDOUT;

	/*
	 * We expect signal_pending(current), but we might be the
	 * victim of a spurious wakeup as well.
	 */
	if (!signal_pending(current))
		goto retry;

	return -ERESTARTSYS;
}
```

```c
// kernel/futex/waitwake.c:344-364（v6.18.60）
void futex_do_wait(struct futex_q *q, struct hrtimer_sleeper *timeout)
{
	/* Arm the timer */
	if (timeout)
		hrtimer_sleeper_start_expires(timeout, HRTIMER_MODE_ABS);

	/*
	 * If we have been removed from the hash list, then another task
	 * has tried to wake us, and we can skip the call to schedule().
	 */
	if (likely(!plist_node_empty(&q->list))) {
		/*
		 * If the timer has already expired, current will already be
		 * flagged for rescheduling. Only call schedule if there
		 * is no timeout, or if it has yet to expire.
		 */
		if (!timeout || timeout->task)
			schedule();
	}
	__set_current_state(TASK_RUNNING);
}
```

醒来后用 `futex_unqueue()` 判断是谁把自己移出了队列：如果已被唤醒者移除，返回 0；否则是超时或信号（或伪唤醒，此时重试）。

### FUTEX_WAKE 与"无等待者"优化

```c
// kernel/futex/waitwake.c:155-201（v6.18.60）
int futex_wake(u32 __user *uaddr, unsigned int flags, int nr_wake, u32 bitset)
{
	struct futex_q *this, *next;
	union futex_key key = FUTEX_KEY_INIT;
	DEFINE_WAKE_Q(wake_q);
	int ret;

	if (!bitset)
		return -EINVAL;

	ret = get_futex_key(uaddr, flags, &key, FUTEX_READ);
	if (unlikely(ret != 0))
		return ret;

	if ((flags & FLAGS_STRICT) && !nr_wake)
		return 0;

	CLASS(hbr, hbr)(&key);
	auto hb = hbr.hb;

	/* Make sure we really have tasks to wakeup */
	if (!futex_hb_waiters_pending(hb))
		return ret;

	spin_lock(&hb->lock);

	plist_for_each_entry_safe(this, next, &hb->chain, list) {
		if (futex_match (&this->key, &key)) {
			if (this->pi_state || this->rt_waiter) {
				ret = -EINVAL;
				break;
			}

			/* Check if one of the bits is set in both bitsets */
			if (!(this->bitset & bitset))
				continue;

			this->wake(&wake_q, this);
			if (++ret >= nr_wake)
				break;
		}
	}

	spin_unlock(&hb->lock);
	wake_up_q(&wake_q);
	return ret;
}
```

唤醒方在哈希桶中查找键匹配（且 `bitset` 有交集）的等待者，调用 `this->wake()`（默认为 `futex_wake_mark()`）把它们移出队列并加入 `wake_q`，释放桶锁后统一唤醒。

注意 `kernel/futex/waitwake.c:176` 的 `futex_hb_waiters_pending()`：如果桶中没有等待者，就直接返回而**不获取桶锁**。这是一个重要的优化（大多数 `FUTEX_WAKE` 调用实际上没有等待者），但它引入了一个典型的存储缓冲（SB）竞态，文件开头的注释给出了完整分析：

```c
// kernel/futex/waitwake.c:54-96（v6.18.60）
 * The correct serialization ensures that a waiter either observes
 * the changed user space value before blocking or is woken by a
 * concurrent waker:
 *
 * CPU 0                                 CPU 1
 * val = *futex;
 * sys_futex(WAIT, futex, val);
 *   futex_wait(futex, val);
 *
 *   waiters++; (a)
 *   smp_mb(); (A) <-- paired with -.
 *                                  |
 *   lock(hash_bucket(futex));      |
 *                                  |
 *   uval = *futex;                 |
 *                                  |        *futex = newval;
 *                                  |        sys_futex(WAKE, futex);
 *                                  |          futex_wake(futex);
 *                                  |
 *                                  `--------> smp_mb(); (B)
 *   if (uval == val)
 *     queue();
 *     unlock(hash_bucket(futex));
 *     schedule();                         if (waiters)
 *                                           lock(hash_bucket(futex));
 *   else                                    wake_waiters(futex);
 *     waiters--; (b)                        unlock(hash_bucket(futex));
 *
 * Where (A) orders the waiters increment and the futex value read through
 * atomic operations (see futex_hb_waiters_inc) and where (B) orders the write
 * to futex and the waiters read (see futex_hb_waiters_pending()).
 *
 * This yields the following case (where X:=waiters, Y:=futex):
 *
 *	X = Y = 0
 *
 *	w[X]=1		w[Y]=1
 *	MB		MB
 *	r[Y]=y		r[X]=x
 *
 * Which guarantees that x==0 && y==0 is impossible; which translates back into
 * the guarantee that we cannot both miss the futex variable change and the
 * enqueue.
```

等待方先增加 `waiters` 计数再读 futex 值，唤醒方先写 futex 值再读 `waiters`。两侧各有一个完整屏障（`futex_hb_waiters_inc()` 中的 `smp_mb__after_atomic()` 与 `futex_hb_waiters_pending()` 中的 `smp_mb()`），保证二者至少有一方看到对方的写——要么等待方看到新值不睡眠，要么唤醒方看到计数非零去加锁唤醒。这正是 7.3 节 SB 模式的实际应用：

```c
// kernel/futex/futex.h:387-420（v6.18.60）
static inline void futex_hb_waiters_inc(struct futex_hash_bucket *hb)
{
#ifdef CONFIG_SMP
	atomic_inc(&hb->waiters);
	/*
	 * Full barrier (A), see the ordering comment above.
	 */
	smp_mb__after_atomic();
#endif
}

/*
 * Reflects a waiter being removed from the waitqueue by wakeup
 * paths.
 */
static inline void futex_hb_waiters_dec(struct futex_hash_bucket *hb)
{
#ifdef CONFIG_SMP
	atomic_dec(&hb->waiters);
#endif
}

static inline int futex_hb_waiters_pending(struct futex_hash_bucket *hb)
{
#ifdef CONFIG_SMP
	/*
	 * Full barrier (B), see the ordering comment above.
	 */
	smp_mb();
	return atomic_read(&hb->waiters);
#else
	return 1;
#endif
}
```

### PI futex

普通 futex 不知道锁的持有者是谁，无法做优先级继承。PI futex 约定 futex word 中存放持有者的线程 ID（TID）：

- 用户态加锁：cmpxchg `0 → TID`，成功即持锁，不进内核；
- 失败时调用 `FUTEX_LOCK_PI`：内核 `futex_lock_pi()`（`kernel/futex/pi.c:983`）在 word 中置位 `FUTEX_WAITERS`（`0x80000000`），根据 TID 找到持有者任务，为该用户态锁创建（或找到）一个 `futex_pi_state`，其中内嵌一个 `rt_mutex_base`，并以持有者的身份"代理"加锁，然后当前任务阻塞在这个 rt_mutex 上——于是本节前半部分的整套 PI 机制自动生效，持有者被提升；
- 用户态解锁：cmpxchg `TID → 0`；若因 `FUTEX_WAITERS` 置位而失败，调用 `FUTEX_UNLOCK_PI`，内核 `futex_unlock_pi()`（`kernel/futex/pi.c:1187`）把锁直接移交给 rt_mutex 的 top waiter，并把其 TID 写入 futex word。

```c
// kernel/futex/futex.h:152-168（v6.18.60）
struct futex_pi_state {
	/*
	 * list of 'owned' pi_state instances - these have to be
	 * cleaned up in do_exit() if the task exits prematurely:
	 */
	struct list_head list;

	/*
	 * The PI object:
	 */
	struct rt_mutex_base pi_mutex;

	struct task_struct *owner;
	refcount_t refcount;

	union futex_key key;
} __randomize_layout;
```

glibc 中使用 `PTHREAD_PRIO_INHERIT` 协议的 `pthread_mutex` 即基于 PI futex。持有者线程意外退出时，内核通过 robust futex 链表（`set_robust_list()`）在 word 中置 `FUTEX_OWNER_DIED` 位并唤醒等待者。

## 关键细节 / 设计考量

### 为什么普通 mutex 不做优先级继承

PI 需要在每次阻塞时遍历并修改阻塞链、可能改变多个任务的调度类，开销显著，而且要求锁一定有明确的持有者。对于吞吐量优先的普通内核，mutex 选择了乐观自旋和移交来优化平均性能；对确定性要求高的场景，则使用 PREEMPT_RT 内核，让所有睡眠锁都变成 rt_mutex。

### futex 的"比较后睡眠"与 ABA

`FUTEX_WAIT` 只保证"睡眠时值等于 `val`"，并不保证"醒来时值仍为某个状态"。用户态必须在醒来后重新检查条件并重试，内核也可能伪唤醒。

### FUTEX_REQUEUE

条件变量的广播（`pthread_cond_broadcast()`）若一次唤醒所有等待者，它们会立即争抢同一把互斥锁，只有一个能成功，其余又要睡下（"惊群"）。`FUTEX_CMP_REQUEUE` 只唤醒一个等待者，把其余的直接从条件变量的 futex 搬到互斥锁的 futex 上排队（`kernel/futex/requeue.c`）。

## 动手实验

### 观察 futex 系统调用

```bash
# 统计每个进程的 futex 操作类型（op & 0x7f 去掉 PRIVATE/CLOCK_REALTIME 标志）
sudo bpftrace -e 'tracepoint:syscalls:sys_enter_futex { @[comm, args->op & 0x7f] = count(); } interval:s:5 { exit(); }'
```

`op` 为 0 表示 `FUTEX_WAIT`，1 为 `FUTEX_WAKE`，9 为 `FUTEX_WAIT_BITSET`（glibc 的条件变量与带超时的等待常用它）。

### 观察优先级继承

```bash
# 编写一个使用 PTHREAD_PRIO_INHERIT 互斥锁的程序：低优先级 SCHED_FIFO 线程持锁，
# 高优先级线程等待；同时运行：
sudo bpftrace -e 'kprobe:rt_mutex_setprio { printf("%s/%d -> pi_task %d\n", comm, pid, ((struct task_struct *)arg1)->pid); }'
```

可以看到持锁线程在高优先级线程阻塞时被提升、解锁时被恢复（`arg1` 为 `NULL` 时表示去提升）。

### 查看私有 futex 哈希

```bash
# 对一个多线程进程（在其内部）查询私有哈希桶数
# prctl(PR_FUTEX_HASH, PR_FUTEX_HASH_GET_SLOTS)，返回值为桶数，0 表示使用全局哈希
dmesg | grep "futex hash table entries"
```

## 小结

- rt_mutex 按优先级组织等待者；每个锁的 top waiter 还挂在持锁者的 `pi_waiters` 树中，持锁者据此继承最高的等待者优先级。
- 阻塞时若持锁者自身也在等锁，`rt_mutex_adjust_prio_chain()` 以每步至多两把锁的方式沿链传播优先级，并检测死锁，链长受 `max_lock_depth` 限制。
- 解锁时先去提升再唤醒 top waiter，并让锁处于"无主但有等待者"状态，只有优先级更高的任务才能偷锁。
- futex 让无竞争的用户态加解锁完全不进内核；内核以 `futex_key` 哈希维护等待队列，"先锁桶、再读值、再入队"保证比较后睡眠的原子性。
- `FUTEX_WAKE` 的无等待者快速返回依赖一对完整屏障，是存储缓冲模式的实例；v6.16 起多线程进程默认使用私有 futex 哈希表。
- PI futex 在 futex word 中存放持有者 TID，内核为其创建内嵌 rt_mutex 的 `futex_pi_state`，把优先级继承带到用户态。

## 参考

- `linux/Documentation/locking/rt-mutex-design.rst`
- `linux/Documentation/locking/rt-mutex.rst`
- `linux/Documentation/locking/pi-futex.rst`
- `linux/Documentation/locking/robust-futexes.rst`
- `linux/Documentation/locking/futex-requeue-pi.rst`
- Hubertus Franke, Rusty Russell, Matthew Kirkwood, "Fuss, Futexes and Furwocks: Fast Userlevel Locking in Linux", Ottawa Linux Symposium, 2002
- Ulrich Drepper, "Futexes Are Tricky", 2011
