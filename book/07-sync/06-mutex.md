# 7.6 互斥锁与乐观自旋

> **本节源码**：[`include/linux/mutex_types.h`](../../linux/include/linux/mutex_types.h)、[`include/linux/mutex.h`](../../linux/include/linux/mutex.h)、[`kernel/locking/mutex.h`](../../linux/kernel/locking/mutex.h)、[`kernel/locking/mutex.c`](../../linux/kernel/locking/mutex.c)、[`kernel/locking/osq_lock.c`](../../linux/kernel/locking/osq_lock.c)、[`include/linux/osq_lock.h`](../../linux/include/linux/osq_lock.h)、[`include/linux/sched.h`](../../linux/include/linux/sched.h)
> **关键函数**：`mutex_lock()`、`__mutex_trylock_fast()`、`__mutex_lock_common()`、`__mutex_trylock_common()`、`mutex_optimistic_spin()`、`mutex_spin_on_owner()`、`osq_lock()`、`mutex_unlock()`、`__mutex_unlock_slowpath()`、`__mutex_handoff()`
> **前置知识**：7.4 自旋锁与 qspinlock、3.4 任务创建与唤醒（睡眠与唤醒的协议）

## 概述

自旋锁要求临界区不能睡眠，而内核中大量代码需要在持锁期间分配内存、访问用户空间、等待 I/O。**互斥锁**（`struct mutex`）为此提供可睡眠的互斥：获取失败时把任务放入等待队列并调度出去，锁释放时再唤醒。

纯粹的"失败即睡眠"有一个代价：一次睡眠加唤醒涉及两次上下文切换，往往比临界区本身长得多。如果持锁者正在另一个 CPU 上运行，它很可能马上就会释放锁，这时短暂自旋反而更划算。Linux 的 mutex 因此采用**乐观自旋**（Optimistic Spinning）：只要持锁者还在 CPU 上运行，等待者就先自旋；一旦持锁者睡眠或自己需要让出 CPU，再转入睡眠。为防止大量自旋者争抢同一缓存行，自旋者先通过一把 MCS 风格的 **OSQ 锁**排队。

为了兼顾吞吐量与公平性，mutex 还允许新来者"抢锁"，并在等待者等待过久时启用**移交**（Handoff）机制，保证队首不会被无限饿死。

## 核心数据结构

### struct mutex

[`include/linux/mutex_types.h:41-54`](../../linux/include/linux/mutex_types.h#L41-L54)（v6.18.60）

```c
struct mutex {
	atomic_long_t		owner;
	raw_spinlock_t		wait_lock;
#ifdef CONFIG_MUTEX_SPIN_ON_OWNER
	struct optimistic_spin_queue osq; /* Spinner MCS lock */
#endif
	struct list_head	wait_list;
#ifdef CONFIG_DEBUG_MUTEXES
	void			*magic;
#endif
#ifdef CONFIG_DEBUG_LOCK_ALLOC
	struct lockdep_map	dep_map;
#endif
};
```

- `owner`：持锁者的 `task_struct` 指针，0 表示未锁定。由于 `task_struct` 至少按 `L1_CACHE_BYTES` 对齐，指针低 3 位恒为 0，被借用来存放标志。把"是否已锁定"和"谁持有"合并在一个原子字里，使得加锁、解锁的快速路径都是一次 cmpxchg。
- `wait_lock`：保护 `wait_list` 的原始自旋锁。
- `osq`：乐观自旋者的排队锁（依赖 `CONFIG_MUTEX_SPIN_ON_OWNER`，本书配置为 `y`）。
- `wait_list`：睡眠等待者链表，FIFO 顺序。

在 64 位、无调试选项的配置下，`struct mutex` 共 32 字节。

### owner 的标志位

[`kernel/locking/mutex.h:22-36`](../../linux/kernel/locking/mutex.h#L22-L36)（v6.18.60）

```c

/*
 * @owner: contains: 'struct task_struct *' to the current lock owner,
 * NULL means not owned. Since task_struct pointers are aligned at
 * at least L1_CACHE_BYTES, we have low bits to store extra state.
 *
 * Bit0 indicates a non-empty waiter list; unlock must issue a wakeup.
 * Bit1 indicates unlock needs to hand the lock to the top-waiter
 * Bit2 indicates handoff has been done and we're waiting for pickup.
 */
#define MUTEX_FLAG_WAITERS	0x01
#define MUTEX_FLAG_HANDOFF	0x02
#define MUTEX_FLAG_PICKUP	0x04

#define MUTEX_FLAGS		0x07
```

| 标志 | 含义 | 由谁设置 / 清除 |
|---|---|---|
| `MUTEX_FLAG_WAITERS` | 等待队列非空，解锁时必须唤醒 | 第一个等待者入队时设置，队列清空时清除 |
| `MUTEX_FLAG_HANDOFF` | 队首要求解锁者直接把锁交给它 | 被唤醒但仍抢不到锁的队首设置 |
| `MUTEX_FLAG_PICKUP` | 锁已移交给指定任务，等它来"取" | 解锁者在移交时设置，接收者取锁时清除 |

### 等待者

[`kernel/locking/mutex.h:14-21`](../../linux/kernel/locking/mutex.h#L14-L21)（v6.18.60）

```c
struct mutex_waiter {
	struct list_head	list;
	struct task_struct	*task;
	struct ww_acquire_ctx	*ww_ctx;
#ifdef CONFIG_DEBUG_MUTEXES
	void			*magic;
#endif
};
```

`mutex_waiter` 分配在等待者自己的内核栈上，不需要动态内存。

## 主流程分析

### 调用链总览

```
mutex_lock()                                kernel/locking/mutex.c:269
├── __mutex_trylock_fast()                  // cmpxchg owner: 0 → current
└── __mutex_lock_slowpath()                 kernel/locking/mutex.c:1047
    └── __mutex_lock() → __mutex_lock_common()  kernel/locking/mutex.c:562
        ├── __mutex_trylock()               // 再试一次
        ├── mutex_optimistic_spin()         // 乐观自旋
        │   ├── mutex_can_spin_on_owner()
        │   ├── osq_lock()                  kernel/locking/osq_lock.c:93
        │   └── mutex_spin_on_owner()
        └── 加入 wait_list，循环：
            ├── schedule_preempt_disabled()
            ├── __mutex_trylock_or_handoff()
            └── mutex_optimistic_spin()      // 队首被唤醒后再自旋一次

mutex_unlock()                              kernel/locking/mutex.c:530
├── __mutex_unlock_fast()                   // cmpxchg owner: current → 0
└── __mutex_unlock_slowpath()               kernel/locking/mutex.c:915
    ├── 清除 owner（保留标志），或在 HANDOFF 时保留
    ├── wake_q_add(队首)
    └── __mutex_handoff()                   // HANDOFF 时把 owner 设为队首
```

### 快速路径

[`kernel/locking/mutex.c:269-275`](../../linux/kernel/locking/mutex.c#L269-L275)（v6.18.60）

```c
void __sched mutex_lock(struct mutex *lock)
{
	might_sleep();

	if (!__mutex_trylock_fast(lock))
		__mutex_lock_slowpath(lock);
}
```

[`kernel/locking/mutex.c:150-168`](../../linux/kernel/locking/mutex.c#L150-L168)（v6.18.60）

```c
static __always_inline bool __mutex_trylock_fast(struct mutex *lock)
{
	unsigned long curr = (unsigned long)current;
	unsigned long zero = 0UL;

	MUTEX_WARN_ON(lock->magic != lock);

	if (atomic_long_try_cmpxchg_acquire(&lock->owner, &zero, curr))
		return true;

	return false;
}

static __always_inline bool __mutex_unlock_fast(struct mutex *lock)
{
	unsigned long curr = (unsigned long)current;

	return atomic_long_try_cmpxchg_release(&lock->owner, &curr, 0UL);
}
```

无竞争时加锁就是把 `owner` 从 0 改成 `current`，解锁就是把它从 `current` 改回 0。只要 `owner` 带有任何标志位（例如有等待者），解锁的 cmpxchg 就会失败，从而进入慢速路径负责唤醒。快速路径只在未开启 `CONFIG_DEBUG_LOCK_ALLOC` 时存在（本书配置）；开启 lockdep 后所有操作都走慢速路径，以便记录依赖。

注意 `mutex_lock()` 一开始就调用 `might_sleep()`：即使锁空闲、不会真的睡眠，在原子上下文中调用它依然是错误。

### 通用的 trylock

慢速路径中的各处"尝试获取"都调用 `__mutex_trylock_common()`：

[`kernel/locking/mutex.c:87-121`](../../linux/kernel/locking/mutex.c#L87-L121)（v6.18.60）

```c
static inline struct task_struct *__mutex_trylock_common(struct mutex *lock, bool handoff)
{
	unsigned long owner, curr = (unsigned long)current;

	owner = atomic_long_read(&lock->owner);
	for (;;) { /* must loop, can race against a flag */
		unsigned long flags = __owner_flags(owner);
		unsigned long task = owner & ~MUTEX_FLAGS;

		if (task) {
			if (flags & MUTEX_FLAG_PICKUP) {
				if (task != curr)
					break;
				flags &= ~MUTEX_FLAG_PICKUP;
			} else if (handoff) {
				if (flags & MUTEX_FLAG_HANDOFF)
					break;
				flags |= MUTEX_FLAG_HANDOFF;
			} else {
				break;
			}
		} else {
			MUTEX_WARN_ON(flags & (MUTEX_FLAG_HANDOFF | MUTEX_FLAG_PICKUP));
			task = curr;
		}

		if (atomic_long_try_cmpxchg_acquire(&lock->owner, &owner, task | flags)) {
			if (task == curr)
				return NULL;
			break;
		}
	}

	return __owner_task(owner);
}
```

它的逻辑可以按 `owner` 的状态分为三种情况：

1. **无主**（`task == 0`）：直接把 `owner` 设为 `current`，保留现有标志。
2. **已移交给我**（`PICKUP` 置位且 `task == current`）：清除 `PICKUP`，完成接收。
3. **有主且参数 `handoff` 为真**：不获取锁，而是设置 `HANDOFF` 标志，要求持锁者解锁时直接移交。

成功时返回 `NULL`，失败时返回当前持锁者，乐观自旋据此决定在谁身上自旋。

### 慢速路径：先自旋，再睡眠

[`kernel/locking/mutex.c:597-621`](../../linux/kernel/locking/mutex.c#L597-L621)（v6.18.60）

```c
	preempt_disable();
	mutex_acquire_nest(&lock->dep_map, subclass, 0, nest_lock, ip);

	trace_contention_begin(lock, LCB_F_MUTEX | LCB_F_SPIN);
	if (__mutex_trylock(lock) ||
	    mutex_optimistic_spin(lock, ww_ctx, NULL)) {
		/* got the lock, yay! */
		lock_acquired(&lock->dep_map, ip);
		if (ww_ctx)
			ww_mutex_set_context_fastpath(ww, ww_ctx);
		trace_contention_end(lock, 0);
		preempt_enable();
		return 0;
	}

	raw_spin_lock_irqsave(&lock->wait_lock, flags);
	/*
	 * After waiting to acquire the wait_lock, try again.
	 */
	if (__mutex_trylock(lock)) {
		if (ww_ctx)
			__ww_mutex_check_waiters(lock, ww_ctx, &wake_q);

		goto skip_wait;
	}
```

慢速路径一开始就关闭抢占，这有两个目的：自旋期间不能被抢占（否则就是在空转一个被换下的任务）；关抢占区间同时是 RCU 读临界区，保证自旋期间读取的 `owner` 所指向的 `task_struct` 不会被释放（`mutex_spin_on_owner()` 中的注释说明了这一点）。

如果乐观自旋失败，就获取 `wait_lock` 并最后试一次，然后入队睡眠：

[`kernel/locking/mutex.c:630-676`](../../linux/kernel/locking/mutex.c#L630-L676)（v6.18.60）

```c
	if (!use_ww_ctx) {
		/* add waiting tasks to the end of the waitqueue (FIFO): */
		__mutex_add_waiter(lock, &waiter, &lock->wait_list);
	} else {
		/*
		 * Add in stamp order, waking up waiters that must kill
		 * themselves.
		 */
		ret = __ww_mutex_add_waiter(&waiter, lock, ww_ctx, &wake_q);
		if (ret)
			goto err_early_kill;
	}

	__set_task_blocked_on(current, lock);
	set_current_state(state);
	trace_contention_begin(lock, LCB_F_MUTEX);
	for (;;) {
		bool first;

		/*
		 * Once we hold wait_lock, we're serialized against
		 * mutex_unlock() handing the lock off to us, do a trylock
		 * before testing the error conditions to make sure we pick up
		 * the handoff.
		 */
		if (__mutex_trylock(lock))
			goto acquired;

		/*
		 * Check for signals and kill conditions while holding
		 * wait_lock. This ensures the lock cancellation is ordered
		 * against mutex_unlock() and wake-ups do not go missing.
		 */
		if (signal_pending_state(state, current)) {
			ret = -EINTR;
			goto err;
		}

		if (ww_ctx) {
			ret = __ww_mutex_check_kill(lock, &waiter, ww_ctx);
			if (ret)
				goto err;
		}

		raw_spin_unlock_irqrestore_wake(&lock->wait_lock, flags, &wake_q);

		schedule_preempt_disabled();
```

等待循环遵循 3.4 节的睡眠协议：先 `set_current_state()`，再检查条件，最后 `schedule()`。不同之处在于检查条件（`__mutex_trylock()`）是在持有 `wait_lock` 时完成的，而唤醒方（解锁慢速路径）也要获取 `wait_lock`，两者被锁串行化，因此不会丢失唤醒。`raw_spin_unlock_irqrestore_wake()` 在释放 `wait_lock` 后才真正执行积攒在 `wake_q` 中的唤醒，缩短持锁时间。

被唤醒之后：

[`kernel/locking/mutex.c:678-711`](../../linux/kernel/locking/mutex.c#L678-L711)（v6.18.60）

```c
		first = __mutex_waiter_is_first(lock, &waiter);

		/*
		 * As we likely have been woken up by task
		 * that has cleared our blocked_on state, re-set
		 * it to the lock we are trying to acquire.
		 */
		set_task_blocked_on(current, lock);
		set_current_state(state);
		/*
		 * Here we order against unlock; we must either see it change
		 * state back to RUNNING and fall through the next schedule(),
		 * or we must see its unlock and acquire.
		 */
		if (__mutex_trylock_or_handoff(lock, first))
			break;

		if (first) {
			trace_contention_begin(lock, LCB_F_MUTEX | LCB_F_SPIN);
			/*
			 * mutex_optimistic_spin() can call schedule(), so
			 * clear blocked on so we don't become unselectable
			 * to run.
			 */
			clear_task_blocked_on(current, lock);
			if (mutex_optimistic_spin(lock, ww_ctx, &waiter))
				break;
			set_task_blocked_on(current, lock);
			trace_contention_begin(lock, LCB_F_MUTEX);
		}

		raw_spin_lock_irqsave(&lock->wait_lock, flags);
	}
	raw_spin_lock_irqsave(&lock->wait_lock, flags);
```

- 若自己是队首（`first`），调用 `__mutex_trylock_or_handoff(lock, true)`：拿不到锁就设置 `HANDOFF` 标志，要求下一次解锁直接移交。
- 队首还会再做一次乐观自旋（此时以 `waiter` 身份自旋，不必获取 OSQ）。这是因为被唤醒的队首往往需要一点时间才能真正运行，而在此期间锁可能已被别人抢走；自旋可以在持锁者释放的瞬间立即接手。

`set_task_blocked_on()` 等调用记录任务阻塞在哪把锁上，供代理执行（Proxy Execution，`CONFIG_SCHED_PROXY_EXEC`，3.1 节）使用；本书配置未开启该选项，它们只是简单的字段赋值。

### 乐观自旋

[`kernel/locking/mutex.c:428-479`](../../linux/kernel/locking/mutex.c#L428-L479)（v6.18.60）

```c
static __always_inline bool
mutex_optimistic_spin(struct mutex *lock, struct ww_acquire_ctx *ww_ctx,
		      struct mutex_waiter *waiter)
{
	if (!waiter) {
		/*
		 * The purpose of the mutex_can_spin_on_owner() function is
		 * to eliminate the overhead of osq_lock() and osq_unlock()
		 * in case spinning isn't possible. As a waiter-spinner
		 * is not going to take OSQ lock anyway, there is no need
		 * to call mutex_can_spin_on_owner().
		 */
		if (!mutex_can_spin_on_owner(lock))
			goto fail;

		/*
		 * In order to avoid a stampede of mutex spinners trying to
		 * acquire the mutex all at once, the spinners need to take a
		 * MCS (queued) lock first before spinning on the owner field.
		 */
		if (!osq_lock(&lock->osq))
			goto fail;
	}

	for (;;) {
		struct task_struct *owner;

		/* Try to acquire the mutex... */
		owner = __mutex_trylock_or_owner(lock);
		if (!owner)
			break;

		/*
		 * There's an owner, wait for it to either
		 * release the lock or go to sleep.
		 */
		if (!mutex_spin_on_owner(lock, owner, ww_ctx, waiter))
			goto fail_unlock;

		/*
		 * The cpu_relax() call is a compiler barrier which forces
		 * everything in this loop to be re-loaded. We don't need
		 * memory barriers as we'll eventually observe the right
		 * values at the cost of a few extra spins.
		 */
		cpu_relax();
	}

	if (!waiter)
		osq_unlock(&lock->osq);

	return true;
```

自旋分两层：

1. **OSQ 排队**：所有想要自旋的任务先获取 `lock->osq`，只有 OSQ 的持有者才在 `owner` 上自旋。这避免了几十个 CPU 同时轮询 `mutex->owner` 所在缓存行。
2. **在持锁者上自旋**：只要 `owner` 不变且持锁者还在 CPU 上运行，就一直等待：

[`kernel/locking/mutex.c:339-375`](../../linux/kernel/locking/mutex.c#L339-L375)（v6.18.60）

```c
static noinline
bool mutex_spin_on_owner(struct mutex *lock, struct task_struct *owner,
			 struct ww_acquire_ctx *ww_ctx, struct mutex_waiter *waiter)
{
	bool ret = true;

	lockdep_assert_preemption_disabled();

	while (__mutex_owner(lock) == owner) {
		/*
		 * Ensure we emit the owner->on_cpu, dereference _after_
		 * checking lock->owner still matches owner. And we already
		 * disabled preemption which is equal to the RCU read-side
		 * crital section in optimistic spinning code. Thus the
		 * task_strcut structure won't go away during the spinning
		 * period
		 */
		barrier();

		/*
		 * Use vcpu_is_preempted to detect lock holder preemption issue.
		 */
		if (!owner_on_cpu(owner) || need_resched()) {
			ret = false;
			break;
		}

		if (ww_ctx && !ww_mutex_spin_on_owner(lock, ww_ctx, waiter)) {
			ret = false;
			break;
		}

		cpu_relax();
	}

	return ret;
}
```

退出自旋的条件有三个：`owner` 变化（锁被释放或易主，回到循环开头重新 trylock）；持锁者不再运行（它睡眠了，继续自旋没有意义）；本 CPU 需要重新调度（`need_resched()`，不能霸占 CPU）。`owner_on_cpu()` 还检查持锁者所在的 vCPU 是否被宿主机抢占：

[`include/linux/sched.h:2292-2299`](../../linux/include/linux/sched.h#L2292-L2299)（v6.18.60）

```c
static inline bool owner_on_cpu(struct task_struct *owner)
{
	/*
	 * As lock holder preemption issue, we both skip spinning if
	 * task is not on cpu or its cpu is preempted
	 */
	return READ_ONCE(owner->on_cpu) && !vcpu_is_preempted(task_cpu(owner));
}
```

自旋失败后若是因为 `need_resched()`，会先调用 `schedule_preempt_disabled()` 让出 CPU，再回来尝试睡眠路径，以免刚拿到锁就被抢占。

### OSQ 锁

OSQ（Optimistic Spin Queue）是专为乐观自旋设计的 MCS 锁变体，与 qspinlock 一样使用 per-CPU 节点，但多了一个关键能力：**可以中途放弃排队**。

[`kernel/locking/osq_lock.c:15-21`](../../linux/kernel/locking/osq_lock.c#L15-L21)（v6.18.60）

```c
struct optimistic_spin_node {
	struct optimistic_spin_node *next, *prev;
	int locked; /* 1 if lock acquired */
	int cpu; /* encoded CPU # + 1 value */
};

static DEFINE_PER_CPU_SHARED_ALIGNED(struct optimistic_spin_node, osq_node);
```

[`include/linux/osq_lock.h:10-18`](../../linux/include/linux/osq_lock.h#L10-L18)（v6.18.60）

```c
struct optimistic_spin_queue {
	/*
	 * Stores an encoded value of the CPU # of the tail node in the queue.
	 * If the queue is empty, then it's set to OSQ_UNLOCKED_VAL.
	 */
	atomic_t tail;
};

#define OSQ_UNLOCKED_VAL (0)
```

`osq_lock()` 的前半部分是标准的 MCS 入队：

[`kernel/locking/osq_lock.c:93-148`](../../linux/kernel/locking/osq_lock.c#L93-L148)（v6.18.60）

```c
bool osq_lock(struct optimistic_spin_queue *lock)
{
	struct optimistic_spin_node *node = this_cpu_ptr(&osq_node);
	struct optimistic_spin_node *prev, *next;
	int curr = encode_cpu(smp_processor_id());
	int old;

	node->locked = 0;
	node->next = NULL;
	node->cpu = curr;

	/*
	 * We need both ACQUIRE (pairs with corresponding RELEASE in
	 * unlock() uncontended, or fastpath) and RELEASE (to publish
	 * the node fields we just initialised) semantics when updating
	 * the lock tail.
	 */
	old = atomic_xchg(&lock->tail, curr);
	if (old == OSQ_UNLOCKED_VAL)
		return true;

	prev = decode_cpu(old);
	node->prev = prev;

	/*
	 * osq_lock()			unqueue
	 *
	 * node->prev = prev		osq_wait_next()
	 * WMB				MB
	 * prev->next = node		next->prev = prev // unqueue-C
	 *
	 * Here 'node->prev' and 'next->prev' are the same variable and we need
	 * to ensure these stores happen in-order to avoid corrupting the list.
	 */
	smp_wmb();

	WRITE_ONCE(prev->next, node);

	/*
	 * Normally @prev is untouchable after the above store; because at that
	 * moment unlock can proceed and wipe the node element from stack.
	 *
	 * However, since our nodes are static per-cpu storage, we're
	 * guaranteed their existence -- this allows us to apply
	 * cmpxchg in an attempt to undo our queueing.
	 */

	/*
	 * Wait to acquire the lock or cancellation. Note that need_resched()
	 * will come with an IPI, which will wake smp_cond_load_relaxed() if it
	 * is implemented with a monitor-wait. vcpu_is_preempted() relies on
	 * polling, be careful.
	 */
	if (smp_cond_load_relaxed(&node->locked, VAL || need_resched() ||
				  vcpu_is_preempted(node_cpu(node->prev))))
		return true;
```

不同于 qspinlock 的无条件等待，OSQ 的等待条件还包括 `need_resched()` 和前驱 vCPU 被抢占。一旦满足，就执行函数后半部分的"出队"（unqueue）三步：先把前驱的 `next` 指针改回 NULL 以稳定前驱；再等待或确定自己的后继；最后把前驱与后继直接相连。由于节点是静态 per-CPU 存储，即使前驱已经离开，访问它的节点也是安全的。每个 CPU 只需要一个 OSQ 节点，因为睡眠锁不会在中断上下文中获取，而且自旋期间已关抢占。

### 解锁慢速路径与移交

[`kernel/locking/mutex.c:915-966`](../../linux/kernel/locking/mutex.c#L915-L966)（v6.18.60）

```c
static noinline void __sched __mutex_unlock_slowpath(struct mutex *lock, unsigned long ip)
{
	struct task_struct *next = NULL;
	DEFINE_WAKE_Q(wake_q);
	unsigned long owner;
	unsigned long flags;

	mutex_release(&lock->dep_map, ip);

	/*
	 * Release the lock before (potentially) taking the spinlock such that
	 * other contenders can get on with things ASAP.
	 *
	 * Except when HANDOFF, in that case we must not clear the owner field,
	 * but instead set it to the top waiter.
	 */
	owner = atomic_long_read(&lock->owner);
	for (;;) {
		MUTEX_WARN_ON(__owner_task(owner) != current);
		MUTEX_WARN_ON(owner & MUTEX_FLAG_PICKUP);

		if (owner & MUTEX_FLAG_HANDOFF)
			break;

		if (atomic_long_try_cmpxchg_release(&lock->owner, &owner, __owner_flags(owner))) {
			if (owner & MUTEX_FLAG_WAITERS)
				break;

			return;
		}
	}

	raw_spin_lock_irqsave(&lock->wait_lock, flags);
	debug_mutex_unlock(lock);
	if (!list_empty(&lock->wait_list)) {
		/* get the first entry from the wait-list: */
		struct mutex_waiter *waiter =
			list_first_entry(&lock->wait_list,
					 struct mutex_waiter, list);

		next = waiter->task;

		debug_mutex_wake_waiter(lock, waiter);
		__clear_task_blocked_on(next, lock);
		wake_q_add(&wake_q, next);
	}

	if (owner & MUTEX_FLAG_HANDOFF)
		__mutex_handoff(lock, next);

	raw_spin_unlock_irqrestore_wake(&lock->wait_lock, flags, &wake_q);
}
```

解锁者首先尝试**清除 owner、保留标志**——这一步不需要 `wait_lock`，让其他自旋者可以尽快抢到锁。只有存在等待者（`WAITERS`）时才去获取 `wait_lock` 唤醒队首。

如果队首设置了 `HANDOFF`，解锁者就不清除 owner，而是在唤醒队首的同时调用 `__mutex_handoff()`：

[`kernel/locking/mutex.c:219-237`](../../linux/kernel/locking/mutex.c#L219-L237)（v6.18.60）

```c
static void __mutex_handoff(struct mutex *lock, struct task_struct *task)
{
	unsigned long owner = atomic_long_read(&lock->owner);

	for (;;) {
		unsigned long new;

		MUTEX_WARN_ON(__owner_task(owner) != current);
		MUTEX_WARN_ON(owner & MUTEX_FLAG_PICKUP);

		new = (owner & MUTEX_FLAG_WAITERS);
		new |= (unsigned long)task;
		if (task)
			new |= MUTEX_FLAG_PICKUP;

		if (atomic_long_try_cmpxchg_release(&lock->owner, &owner, new))
			break;
	}
}
```

`owner` 被直接改写为"队首任务指针 | `PICKUP` | 保留的 `WAITERS`"。此后任何其他任务的 trylock 都会失败（锁有主且不是 PICKUP 给自己的），只有队首能在被唤醒后通过 `__mutex_trylock_common()` 的第 2 种情况接收它。

## 关键细节 / 设计考量

### 公平性与吞吐量的折中

| 机制 | 偏向 | 效果 |
|---|---|---|
| 快速路径与自旋者抢锁 | 吞吐量 | 正在运行的任务直接拿锁，省去唤醒延迟；但被唤醒的队首可能一次次抢不到 |
| 队首被唤醒后自旋 | 吞吐量 + 公平 | 让队首也有机会在锁释放瞬间接手 |
| `HANDOFF` / `PICKUP` | 公平 | 队首一次抢锁失败后，下一次解锁必定交给它，避免无限饥饿 |

v4.10 之前的 mutex 允许任意次数的抢锁，等待者在极端情况下可能饿死数秒；引入移交机制（Peter Zijlstra，"locking/mutex: Rework mutex::owner"，v4.10）后，队首的等待时间有了上界。

### mutex 的使用约束

- 只能由持锁者解锁，不能在中断上下文中加锁或解锁。
- 不可递归加锁。
- 持锁任务不能退出。
- `mutex_unlock()` 返回前，mutex 本身必须保持有效：解锁慢速路径在 `owner` 被清除后仍会访问 `wait_lock`，因此**不能**用 `mutex_unlock()` 之后立即释放包含它的对象的方式来管理生命周期（`mutex_unlock()` 的注释专门指出这一点，[`kernel/locking/mutex.c:523-526`](../../linux/kernel/locking/mutex.c#L523-L526)）；应使用引用计数。

### ww_mutex

图形驱动等需要同时锁住一组对象，且加锁顺序无法预先确定。`ww_mutex`（Wound/Wait Mutex）为每个加锁事务分配一个"票号"，检测到可能死锁时由较年轻的事务回退。它复用了 `__mutex_lock_common()`（`use_ww_ctx = true`），代码中所有 `ww_ctx` 相关分支即为此服务。详见 `Documentation/locking/ww-mutex-design.rst`。

### PREEMPT_RT 下的 mutex

开启 `CONFIG_PREEMPT_RT` 后，`struct mutex` 改为基于 `rt_mutex_base`（[`include/linux/mutex_types.h:62-67`](../../linux/include/linux/mutex_types.h#L62-L67)），从而获得优先级继承能力（7.8 节）。普通内核中 mutex 不支持优先级继承。

## 动手实验

### 统计 mutex 竞争

```bash
# 以 mutex 类型过滤
sudo perf lock contention -a -b -Y mutex -- sleep 5
```

### 观察乐观自旋的成功率

```bash
sudo bpftrace -e '
kprobe:mutex_spin_on_owner { @spin = count(); }
kretprobe:mutex_spin_on_owner { @ret[retval] = count(); }
kprobe:__mutex_lock_slowpath { @slow = count(); }
interval:s:5 { exit(); }'
```

`@ret[1]` 表示因 `owner` 改变而退出（可以重试 trylock），`@ret[0]` 表示因持锁者睡眠或需要调度而放弃自旋。

## 小结

- `mutex->owner` 同时编码持锁者指针和三个标志位，加锁、解锁快速路径各是一次 cmpxchg。
- 慢速路径先关抢占做乐观自旋：只要持锁者在 CPU 上运行就自旋等待，自旋者通过 OSQ（可中途退出的 MCS 锁）排队，避免缓存行争抢。
- 自旋失败后入队睡眠；条件检查与唤醒都在 `wait_lock` 保护下进行，不会丢失唤醒；唤醒通过 `wake_q` 在释放 `wait_lock` 后执行。
- 新来者可以抢锁以提高吞吐量；队首被唤醒仍抢不到时设置 `HANDOFF`，下一次解锁直接把锁移交给它（`PICKUP`），保证有界等待。
- mutex 只能由持锁者解锁、不可递归、不支持优先级继承（RT 内核除外）。

## 参考

- `linux/Documentation/locking/mutex-design.rst`
- `linux/Documentation/locking/ww-mutex-design.rst`
- Ingo Molnar, "generic mutex subsystem" 补丁系列（v2.6.16 合入）
- Peter Zijlstra, "locking/mutex: Rework mutex::owner"（v4.10，引入 HANDOFF / PICKUP）
