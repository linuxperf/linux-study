# 7.5 读写锁与顺序锁

> **本节源码**：[`include/linux/rwlock_types.h`](../../linux/include/linux/rwlock_types.h)、[`include/linux/rwlock_api_smp.h`](../../linux/include/linux/rwlock_api_smp.h)、[`include/asm-generic/qrwlock_types.h`](../../linux/include/asm-generic/qrwlock_types.h)、[`include/asm-generic/qrwlock.h`](../../linux/include/asm-generic/qrwlock.h)、[`kernel/locking/qrwlock.c`](../../linux/kernel/locking/qrwlock.c)、[`include/linux/seqlock_types.h`](../../linux/include/linux/seqlock_types.h)、[`include/linux/seqlock.h`](../../linux/include/linux/seqlock.h)、[`kernel/time/timekeeping.c`](../../linux/kernel/time/timekeeping.c)
> **关键函数**：`read_lock()`、`write_lock()`、`queued_read_lock()`、`queued_read_lock_slowpath()`、`queued_write_lock_slowpath()`、`read_seqcount_begin()`、`read_seqcount_retry()`、`write_seqlock()`、`write_seqcount_latch()`
> **前置知识**：7.3 内存屏障与 Linux 内核内存模型、7.4 自旋锁与 qspinlock

## 概述

许多数据结构读远多于写。用普通自旋锁保护它们时，读者之间也互相排斥，白白浪费了并行度。内核为此提供了两类自旋式的"读者友好"同步原语：

- **读写锁**（Reader-Writer Lock，`rwlock_t`）：允许多个读者同时持锁，写者独占。读者仍要修改锁字，读多时锁所在的缓存行会在 CPU 之间来回传递。
- **顺序锁**（Sequence Lock，`seqcount_t` / `seqlock_t`）：读者完全不写共享内存，只在读前后比较一个序号；若期间有写者修改过数据，读者丢弃结果重试。它对读者极其友好，代价是读者可能重试，且读到的中间数据可能不一致，只适用于能够容忍"读到垃圾再丢弃"的简单数据。

x86_64 上的 `rwlock_t` 由 qrwlock 实现，它借用一把 qspinlock 作为等待队列；`seqcount_t` 则只是一个 32 位整数配合屏障。本节分别分析二者，并介绍用于 NMI 安全读取的 latch 变体。对于读者需要睡眠的场景，应使用 7.7 节的读写信号量；对于读多写少且能延迟释放的场景，RCU（7.9 节）通常是更好的选择。

## 读写锁：qrwlock

### 核心数据结构

[`include/linux/rwlock_types.h:25-34`](../../linux/include/linux/rwlock_types.h#L25-L34)（v6.18.60）

```c
typedef struct {
	arch_rwlock_t raw_lock;
#ifdef CONFIG_DEBUG_SPINLOCK
	unsigned int magic, owner_cpu;
	void *owner;
#endif
#ifdef CONFIG_DEBUG_LOCK_ALLOC
	struct lockdep_map dep_map;
#endif
} rwlock_t;
```

[`include/asm-generic/qrwlock_types.h:13-27`](../../linux/include/asm-generic/qrwlock_types.h#L13-L27)（v6.18.60）

```c
typedef struct qrwlock {
	union {
		atomic_t cnts;
		struct {
#ifdef __LITTLE_ENDIAN
			u8 wlocked;	/* Locked for write? */
			u8 __lstate[3];
#else
			u8 __lstate[3];
			u8 wlocked;	/* Locked for write? */
#endif
		};
	};
	arch_spinlock_t		wait_lock;
} arch_rwlock_t;
```

[`include/asm-generic/qrwlock.h:24-31`](../../linux/include/asm-generic/qrwlock.h#L24-L31)（v6.18.60）

```c
/*
 * Writer states & reader shift and bias.
 */
#define	_QW_WAITING	0x100		/* A writer is waiting	   */
#define	_QW_LOCKED	0x0ff		/* A writer holds the lock */
#define	_QW_WMASK	0x1ff		/* Writer mask		   */
#define	_QR_SHIFT	9		/* Reader count shift	   */
#define _QR_BIAS	(1U << _QR_SHIFT)
```

`cnts` 的布局如下：

```
 31                                   9   8   7          0
┌─────────────────────────────────────┬───┬────────────┐
│          读者计数（每个读者 +0x200）   │ W │ wlocked    │
└─────────────────────────────────────┴───┴────────────┘
                                        │      └─ 0xff：写者持锁
                                        └─ 0x100：有写者在等待
```

*图 7-5：qrwlock `cnts` 字段布局（高位在左）*

`wait_lock` 是一把普通的 qspinlock，所有进入慢速路径的读者和写者都在它上面排队，从而继承了 qspinlock 的 FIFO 公平性。

### 快速路径

`read_lock()` / `write_lock()` 的通用层与自旋锁相同：先关抢占，再调用架构实现（[`include/linux/rwlock_api_smp.h:147-152`](../../linux/include/linux/rwlock_api_smp.h#L147-L152)、`206-211`）。架构层的快速路径：

[`include/asm-generic/qrwlock.h:78-123`](../../linux/include/asm-generic/qrwlock.h#L78-L123)（v6.18.60）

```c
static inline void queued_read_lock(struct qrwlock *lock)
{
	int cnts;

	cnts = atomic_add_return_acquire(_QR_BIAS, &lock->cnts);
	if (likely(!(cnts & _QW_WMASK)))
		return;

	/* The slowpath will decrement the reader count, if necessary. */
	queued_read_lock_slowpath(lock);
}

/**
 * queued_write_lock - acquire write lock of a queued rwlock
 * @lock : Pointer to queued rwlock structure
 */
static inline void queued_write_lock(struct qrwlock *lock)
{
	int cnts = 0;
	/* Optimize for the unfair lock case where the fair flag is 0. */
	if (likely(atomic_try_cmpxchg_acquire(&lock->cnts, &cnts, _QW_LOCKED)))
		return;

	queued_write_lock_slowpath(lock);
}

/**
 * queued_read_unlock - release read lock of a queued rwlock
 * @lock : Pointer to queued rwlock structure
 */
static inline void queued_read_unlock(struct qrwlock *lock)
{
	/*
	 * Atomically decrement the reader count
	 */
	(void)atomic_sub_return_release(_QR_BIAS, &lock->cnts);
}

/**
 * queued_write_unlock - release write lock of a queued rwlock
 * @lock : Pointer to queued rwlock structure
 */
static inline void queued_write_unlock(struct qrwlock *lock)
{
	smp_store_release(&lock->wlocked, 0);
}
```

- **读者**：无条件地给 `cnts` 加 `_QR_BIAS`（一次 `lock xadd`），若返回值中写者位（`_QW_WMASK`，包括"持锁"和"等待"）为 0 就成功。先加后查可以只用一条原子指令，失败时由慢速路径负责撤销。
- **写者**：只有 `cnts` 为 0（无读者、无写者）时才能用 cmpxchg 置为 `_QW_LOCKED`。
- **读者解锁**：减去 `_QR_BIAS`，带 release 语义。
- **写者解锁**：对 `wlocked` 字节做一次 `smp_store_release()`，与 qspinlock 一样只需一次字节写。

### 慢速路径

[`kernel/locking/qrwlock.c:21-46`](../../linux/kernel/locking/qrwlock.c#L21-L46)（v6.18.60）

```c
void __lockfunc queued_read_lock_slowpath(struct qrwlock *lock)
{
	/*
	 * Readers come here when they cannot get the lock without waiting
	 */
	if (unlikely(in_interrupt())) {
		/*
		 * Readers in interrupt context will get the lock immediately
		 * if the writer is just waiting (not holding the lock yet),
		 * so spin with ACQUIRE semantics until the lock is available
		 * without waiting in the queue.
		 */
		atomic_cond_read_acquire(&lock->cnts, !(VAL & _QW_LOCKED));
		return;
	}
	atomic_sub(_QR_BIAS, &lock->cnts);

	trace_contention_begin(lock, LCB_F_SPIN | LCB_F_READ);

	/*
	 * Put the reader into the wait queue
	 */
	arch_spin_lock(&lock->wait_lock);
	atomic_add(_QR_BIAS, &lock->cnts);

	/*
```

读者慢速路径有一个重要的例外：**中断上下文中的读者**不排队，只要写者尚未真正持锁（哪怕已有写者在等待），就直接自旋等待 `_QW_LOCKED` 清除后进入。考虑这样的场景：进程上下文已持有读锁，随后另一 CPU 上的写者开始排队并设置了 `_QW_WAITING`，接着本 CPU 被中断打断，中断处理函数再次申请同一把读锁。如果中断中的读者也老老实实排在写者后面，就会出现"中断读者等写者、写者等进程读者、进程读者等中断返回"的循环等待。这条例外保证了读锁在中断中可以递归获取，这也是 `tasklist_lock` 等锁能在进程上下文用 `read_lock()`、在中断上下文中同样读取的前提。

普通读者先撤销刚才加上的偏置，到 `wait_lock` 上排队；轮到自己时重新加上偏置，并等待写者释放（`_QW_LOCKED` 清除）。注意它只等待"持锁"位，而不理会"等待"位——队首读者释放 `wait_lock` 后，后面排队的读者也会依次进来，使得一批连续的读者可以同时持锁。

[`kernel/locking/qrwlock.c:52-77`](../../linux/kernel/locking/qrwlock.c#L52-L77)（v6.18.60）

```c

	/*
	 * Signal the next one in queue to become queue head
	 */
	arch_spin_unlock(&lock->wait_lock);

	trace_contention_end(lock, 0);
}
EXPORT_SYMBOL(queued_read_lock_slowpath);

/**
 * queued_write_lock_slowpath - acquire write lock of a queued rwlock
 * @lock : Pointer to queued rwlock structure
 */
void __lockfunc queued_write_lock_slowpath(struct qrwlock *lock)
{
	int cnts;

	trace_contention_begin(lock, LCB_F_SPIN | LCB_F_WRITE);

	/* Put the writer into the wait queue */
	arch_spin_lock(&lock->wait_lock);

	/* Try to acquire the lock directly if no reader is present */
	if (!(cnts = atomic_read(&lock->cnts)) &&
	    atomic_try_cmpxchg_acquire(&lock->cnts, &cnts, _QW_LOCKED))
```

写者在 `wait_lock` 上排队，成为队首后先尝试直接获取；否则设置 `_QW_WAITING` 阻止新读者在快速路径上进入，然后等待现有读者全部离开（`cnts` 恰好等于 `_QW_WAITING`），再用 cmpxchg 换成 `_QW_LOCKED`。因为写者一直持有 `wait_lock`，排在它后面的读者和写者都进不来。

### 使用场景与局限

`rwlock_t` 最著名的使用者是 `tasklist_lock`（[`kernel/fork.c:156`](../../linux/kernel/fork.c#L156)），保护全局任务链表。但读写锁有两个固有问题：

1. **读者也要写锁字**。每个读者都对 `cnts` 做原子加减，CPU 越多，缓存行争抢越严重，读者"并行"的收益会被一致性流量抵消。
2. **写者饥饿与延迟**。尽管 qrwlock 让写者在排队后阻挡新读者，但中断上下文读者仍可插队。

因此新代码很少再引入 `rwlock_t`，读多写少的数据优先考虑 RCU 或顺序锁。

## 顺序锁：seqcount 与 seqlock

### 思想

顺序锁用一个序号 `sequence` 标记数据版本：写者开始修改时把序号加一（变为奇数），修改完成后再加一（变为偶数）。读者在读之前记录序号，读完后再比较：

- 若开始时序号为奇数，说明写者正在修改，等待它完成；
- 若结束时序号与开始时不同，说明读的过程中发生了写，结果作废，重试。

读者不修改任何共享状态，因此任意多个 CPU 并发读取都不会产生缓存行争抢；写者也不会被读者阻塞。

### 核心数据结构

[`include/linux/seqlock_types.h:33-38`](../../linux/include/linux/seqlock_types.h#L33-L38)（v6.18.60）

```c
typedef struct seqcount {
	unsigned sequence;
#ifdef CONFIG_DEBUG_LOCK_ALLOC
	struct lockdep_map dep_map;
#endif
} seqcount_t;
```

[`include/linux/seqlock_types.h:62-72`](../../linux/include/linux/seqlock_types.h#L62-L72)（v6.18.60）

```c
#define SEQCOUNT_LOCKNAME(lockname, locktype, preemptible, lockbase)	\
typedef struct seqcount_##lockname {					\
	seqcount_t		seqcount;				\
	__SEQ_LOCK(locktype	*lock);					\
} seqcount_##lockname##_t;

SEQCOUNT_LOCKNAME(raw_spinlock, raw_spinlock_t,  false,    raw_spin)
SEQCOUNT_LOCKNAME(spinlock,     spinlock_t,      __SEQ_RT, spin)
SEQCOUNT_LOCKNAME(rwlock,       rwlock_t,        __SEQ_RT, read)
SEQCOUNT_LOCKNAME(mutex,        struct mutex,    true,     mutex)
#undef SEQCOUNT_LOCKNAME
```

[`include/linux/seqlock_types.h:84-91`](../../linux/include/linux/seqlock_types.h#L84-L91)（v6.18.60）

```c
typedef struct {
	/*
	 * Make sure that readers don't starve writers on PREEMPT_RT: use
	 * seqcount_spinlock_t instead of seqcount_t. Check __SEQ_LOCK().
	 */
	seqcount_spinlock_t seqcount;
	spinlock_t lock;
} seqlock_t;
```

三种形态：

| 类型 | 组成 | 写者串行化 |
|---|---|---|
| `seqcount_t` | 只有序号 | 由调用者自行保证（外部锁或单一写者），且写者须关抢占 |
| `seqcount_LOCKNAME_t` | 序号 + 指向关联锁的指针 | 关联锁；lockdep 会检查写者持有该锁 |
| `seqlock_t` | `seqcount_spinlock_t` + 内嵌 `spinlock_t` | 内嵌自旋锁 |

`seqcount_LOCKNAME_t` 记录关联的锁有两个作用：开启 lockdep 时验证写者确实持有该锁；在 PREEMPT_RT 下，读者若发现序号为奇数，会先获取再释放这把锁，从而等待被抢占的写者完成，而不是空转（这正是 `__SEQ_RT` 参数控制的行为）。

### 读端

[`include/linux/seqlock.h:271-300`](../../linux/include/linux/seqlock.h#L271-L300)（v6.18.60）

```c
#define __read_seqcount_begin(s)					\
({									\
	unsigned __seq;							\
									\
	while (unlikely((__seq = seqprop_sequence(s)) & 1))		\
		cpu_relax();						\
									\
	kcsan_atomic_next(KCSAN_SEQLOCK_REGION_MAX);			\
	__seq;								\
})

/**
 * raw_read_seqcount_begin() - begin a seqcount_t read section w/o lockdep
 * @s: Pointer to seqcount_t or any of the seqcount_LOCKNAME_t variants
 *
 * Return: count to be passed to read_seqcount_retry()
 */
#define raw_read_seqcount_begin(s) __read_seqcount_begin(s)

/**
 * read_seqcount_begin() - begin a seqcount_t read critical section
 * @s: Pointer to seqcount_t or any of the seqcount_LOCKNAME_t variants
 *
 * Return: count to be passed to read_seqcount_retry()
 */
#define read_seqcount_begin(s)						\
({									\
	seqcount_lockdep_reader_access(seqprop_const_ptr(s));		\
	raw_read_seqcount_begin(s);					\
})
```

`seqprop_sequence()` 对普通 `seqcount_t` 是 `smp_load_acquire(&s->sequence)`（[`include/linux/seqlock.h:209-212`](../../linux/include/linux/seqlock.h#L209-L212)），保证后续对数据的读取不会提前到读取序号之前。序号为奇数时用 `cpu_relax()` 自旋等待。

[`include/linux/seqlock.h:385-412`](../../linux/include/linux/seqlock.h#L385-L412)（v6.18.60）

```c
#define __read_seqcount_retry(s, start)					\
	do___read_seqcount_retry(seqprop_const_ptr(s), start)

static inline int do___read_seqcount_retry(const seqcount_t *s, unsigned start)
{
	kcsan_atomic_next(0);
	return unlikely(READ_ONCE(s->sequence) != start);
}

/**
 * read_seqcount_retry() - end a seqcount_t read critical section
 * @s: Pointer to seqcount_t or any of the seqcount_LOCKNAME_t variants
 * @start: count, from read_seqcount_begin()
 *
 * read_seqcount_retry closes the read critical section of given
 * seqcount_t.  If the critical section was invalid, it must be ignored
 * (and typically retried).
 *
 * Return: true if a read section retry is required, else false
 */
#define read_seqcount_retry(s, start)					\
	do_read_seqcount_retry(seqprop_const_ptr(s), start)

static inline int do_read_seqcount_retry(const seqcount_t *s, unsigned start)
{
	smp_rmb();
	return do___read_seqcount_retry(s, start);
}
```

结束时先 `smp_rmb()`，保证对数据的读取都完成后再重读序号，与写端的 `smp_wmb()` 配对。在 x86 上两者都只是编译器屏障。

典型的读者写法是一个 do-while 循环，例如获取单调时钟：

[`kernel/time/timekeeping.c:814-832`](../../linux/kernel/time/timekeeping.c#L814-L832)（v6.18.60）

```c
ktime_t ktime_get(void)
{
	struct timekeeper *tk = &tk_core.timekeeper;
	unsigned int seq;
	ktime_t base;
	u64 nsecs;

	WARN_ON(timekeeping_suspended);

	do {
		seq = read_seqcount_begin(&tk_core.seq);
		base = tk->tkr_mono.base;
		nsecs = timekeeping_get_ns(&tk->tkr_mono);

	} while (read_seqcount_retry(&tk_core.seq, seq));

	return ktime_add_ns(base, nsecs);
}
EXPORT_SYMBOL_GPL(ktime_get);
```

### 写端

[`include/linux/seqlock.h:420-454`](../../linux/include/linux/seqlock.h#L420-L454)（v6.18.60）

```c
#define raw_write_seqcount_begin(s)					\
do {									\
	if (seqprop_preemptible(s))					\
		preempt_disable();					\
									\
	do_raw_write_seqcount_begin(seqprop_ptr(s));			\
} while (0)

static inline void do_raw_write_seqcount_begin(seqcount_t *s)
{
	kcsan_nestable_atomic_begin();
	s->sequence++;
	smp_wmb();
}

/**
 * raw_write_seqcount_end() - end a seqcount_t write section w/o lockdep
 * @s: Pointer to seqcount_t or any of the seqcount_LOCKNAME_t variants
 *
 * Context: check write_seqcount_end()
 */
#define raw_write_seqcount_end(s)					\
do {									\
	do_raw_write_seqcount_end(seqprop_ptr(s));			\
									\
	if (seqprop_preemptible(s))					\
		preempt_enable();					\
} while (0)

static inline void do_raw_write_seqcount_end(seqcount_t *s)
{
	smp_wmb();
	s->sequence++;
	kcsan_nestable_atomic_end();
}
```

写者的两次 `sequence++` 不是原子操作——顺序锁假定写者已经被串行化。两个 `smp_wmb()` 保证"序号变奇"先于数据修改可见，"数据修改"先于"序号变偶"可见。

`raw_write_seqcount_begin()` 会根据 `seqprop_preemptible()` 决定是否关抢占：如果关联的锁本身已经关了抢占（自旋锁），就不需要重复关闭；对于关联 `mutex` 的 `seqcount_mutex_t`，写者可能被抢占，此时必须显式关抢占。原因是：**写者在序号为奇数时被抢占**，同一 CPU 上的读者会无限自旋，直到写者再次被调度——在单核或绑核场景下就是活锁。

`seqlock_t` 把写端的加锁与序号操作打包在一起：

[`include/linux/seqlock.h:874-891`](../../linux/include/linux/seqlock.h#L874-L891)（v6.18.60）

```c
static inline void write_seqlock(seqlock_t *sl)
{
	spin_lock(&sl->lock);
	do_write_seqcount_begin(&sl->seqcount.seqcount);
}

/**
 * write_sequnlock() - end a seqlock_t write side critical section
 * @sl: Pointer to seqlock_t
 *
 * write_sequnlock closes the (serialized and non-preemptible) write side
 * critical section of given seqlock_t.
 */
static inline void write_sequnlock(seqlock_t *sl)
{
	do_write_seqcount_end(&sl->seqcount.seqcount);
	spin_unlock(&sl->lock);
}
```

### 写者优先与读者重试

顺序锁天然偏向写者：写者从不等待读者。若写操作频繁，读者可能不断重试而"饿死"。为此 `seqlock_t` 提供了 `read_seqbegin_or_lock()` / `need_seqretry()`（[`include/linux/seqlock.h:1127-1145`](../../linux/include/linux/seqlock.h#L1127-L1145)）：调用者第一次传入偶数（通常为 0），以无锁方式读；若 `need_seqretry()` 返回真，调用者把 `seq` 置为奇数（通常为 1）再循环一次，此时 `read_seqbegin_or_lock()` 直接获取内嵌自旋锁做"加锁读"，保证进展。

### 限制

- 读者在临界区中**可能读到不一致的数据**。因此读者不能解引用读到的指针（指针可能已失效），也不能根据中间结果做有副作用的操作。顺序锁适合保护少量标量数据，如时间戳、统计值、`dentry` 的名字长度和哈希。
- 读者若在中断或 NMI 中，而写者在同一 CPU 的进程上下文中被打断于"序号为奇数"的状态，读者会永远自旋。普通顺序锁要求这种情况下写者关中断；NMI 中读取则需要下面的 latch 变体。

### Latch：NMI 安全的顺序锁

`seqcount_latch_t` 维护数据的**两份副本**，用序号的最低位选择读者应该读哪一份：

[`include/linux/seqlock.h:696-701`](../../linux/include/linux/seqlock.h#L696-L701)（v6.18.60）

```c
static __always_inline void raw_write_seqcount_latch(seqcount_latch_t *s)
{
	smp_wmb();	/* prior stores before incrementing "sequence" */
	s->seqcount.sequence++;
	smp_wmb();      /* increment "sequence" before following stores */
}
```

时间子系统的 NMI 安全时钟 `ktime_get_mono_fast_ns()` 就是这样实现的：

[`kernel/time/timekeeping.c:422-456`](../../linux/kernel/time/timekeeping.c#L422-L456)（v6.18.60）

```c
static void update_fast_timekeeper(const struct tk_read_base *tkr,
				   struct tk_fast *tkf)
{
	struct tk_read_base *base = tkf->base;

	/* Force readers off to base[1] */
	write_seqcount_latch_begin(&tkf->seq);

	/* Update base[0] */
	memcpy(base, tkr, sizeof(*base));

	/* Force readers back to base[0] */
	write_seqcount_latch(&tkf->seq);

	/* Update base[1] */
	memcpy(base + 1, base, sizeof(*base));

	write_seqcount_latch_end(&tkf->seq);
}

static __always_inline u64 __ktime_get_fast_ns(struct tk_fast *tkf)
{
	struct tk_read_base *tkr;
	unsigned int seq;
	u64 now;

	do {
		seq = read_seqcount_latch(&tkf->seq);
		tkr = tkf->base + (seq & 0x01);
		now = ktime_to_ns(tkr->base);
		now += timekeeping_get_ns(tkr);
	} while (read_seqcount_latch_retry(&tkf->seq, seq));

	return now;
}
```

写者先把序号加一（变奇），引导读者去读 `base[1]`，此时安全地更新 `base[0]`；再把序号加一（变偶），引导读者回到 `base[0]`，再更新 `base[1]`。任何时刻总有一份副本是完整的，读者永远不必等待写者，即使写者被 NMI 打断、NMI 中的读者也能拿到一个一致（虽然可能稍旧）的值。

## 关键细节 / 设计考量

### 读写锁与顺序锁的对比

| 特性 | `rwlock_t` | `seqcount_t` / `seqlock_t` | RCU |
|---|---|---|---|
| 读者是否写共享内存 | 是（读者计数） | 否 | 否 |
| 读者是否被写者阻塞 | 是 | 写入期间自旋并重试 | 否 |
| 写者是否被读者阻塞 | 是 | 否 | 否（但释放需等宽限期） |
| 读者能否读指针并解引用 | 能 | 不能 | 能 |
| 读者可否在 NMI 中 | 否 | 仅 latch 变体 | 能 |
| 典型用途 | `tasklist_lock` | 时间戳、`mm->mm_lock_seq`（[`include/linux/mm_types.h:1080`](../../linux/include/linux/mm_types.h#L1080)） | 链表、哈希表、配置指针 |

*表 7-5：三种读者友好原语的对比*

### 读写锁的写者偏好

qrwlock 采用"排队公平 + 中断读者优先"的折中策略。如果读者在进程上下文中长时间占据锁，写者会一直等待；反过来，写者一旦设置 `_QW_WAITING`，新的进程上下文读者就会进入慢速路径排队。整体上接近 FIFO 顺序。

## 动手实验

### 观察 tasklist_lock 的竞争

```bash
# 在 dev 虚拟机中并发创建大量进程，同时统计锁竞争
sudo perf lock contention -a -b -- bash -c 'for i in $(seq 2000); do /bin/true & done; wait'
```

输出中 `tasklist_lock` 通常以 `rwlock:W` 类型出现，调用者为 `copy_process()` 或 `release_task()`。

### 统计顺序锁读者重试

`read_seqcount_retry()` 是内联函数，无法直接挂探针；可以观察使用 latch 的快速时钟被调用的频率：

```bash
sudo bpftrace -e 'kprobe:ktime_get_mono_fast_ns { @[comm] = count(); } interval:s:5 { exit(); }'
```

## 小结

- qrwlock 用 32 位 `cnts`（读者计数 + 写者等待位 + 写者持锁字节）加一把 qspinlock 等待队列实现读写锁，快速路径各需一条原子指令。
- 中断上下文中的读者在写者尚未持锁时直接进入，这保证了读锁的递归性，避免"进程读者—等待写者—中断读者"死锁。
- 读写锁的读者仍要修改锁字，多核扩展性有限；新代码倾向使用 RCU 或顺序锁。
- 顺序锁的读者只读序号、不写共享内存，通过"奇数等待、前后不等则重试"保证读到一致的快照；写者须已串行化并关抢占。
- `seqcount_latch_t` 维护两份数据副本，让读者永不等待写者，适用于 NMI 中的读取，如快速时钟。

## 参考

- `linux/Documentation/locking/seqlock.rst`
- `linux/Documentation/locking/spinlocks.rst`（读写锁部分）
- Waiman Long, "qrwlock: Introducing queue read/write lock" 补丁系列（v3.16 合入）
- Ahmed S. Darwish, "seqlock: Extend seqcount API with associated locks" 补丁系列，2020（引入 `seqcount_LOCKNAME_t`）
