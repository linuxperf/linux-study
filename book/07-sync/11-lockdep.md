# 7.11 lockdep 与锁调试

> **本节源码**：[`include/linux/lockdep_types.h`](../../linux/include/linux/lockdep_types.h)、[`include/linux/lockdep.h`](../../linux/include/linux/lockdep.h)、[`include/linux/mutex.h`](../../linux/include/linux/mutex.h)、[`kernel/locking/lockdep.c`](../../linux/kernel/locking/lockdep.c)、[`kernel/locking/lockdep_internals.h`](../../linux/kernel/locking/lockdep_internals.h)、[`kernel/locking/lockdep_states.h`](../../linux/kernel/locking/lockdep_states.h)、[`kernel/hung_task.c`](../../linux/kernel/hung_task.c)、[`kernel/watchdog.c`](../../linux/kernel/watchdog.c)
> **关键函数**：`lock_acquire()`、`__lock_acquire()`、`validate_chain()`、`check_deadlock()`、`check_prevs_add()`、`check_prev_add()`、`check_noncircular()`、`check_irq_usage()`、`check_wait_context()`
> **前置知识**：7.4～7.8 各类锁的实现

## 概述

死锁类错误有一个共同特点：只在特定的时序下才会发生，测试中可能运行几个月都不出现，一旦出现又难以复现。**lockdep**（锁依赖验证器，Lock Dependency Validator，v2.6.18 引入）把问题从"是否真的发生了死锁"转化为"是否**存在**发生死锁的可能"：它在运行时记录每一次加锁所体现的锁顺序，构建一张全局的锁依赖图，只要某条加锁路径与已有的依赖构成环，或违反了中断安全规则，就立即报告——哪怕这两条路径从未真正并发执行过。

lockdep 的关键抽象是**锁类**（Lock Class）：它不跟踪每个锁实例，而是把"同一处初始化代码创建的锁"视为同一类。内核中有数百万个 `inode->i_rwsem` 实例，但它们只属于一个类，因此只要任意两个 inode 曾以某种顺序加锁，就能验证所有 inode 的加锁顺序。

本书配置**未开启** lockdep（`CONFIG_PROVE_LOCKING`、`CONFIG_DEBUG_LOCK_ALLOC`、`CONFIG_LOCK_STAT` 均未设置），本节分析的代码在默认内核中不参与编译；前面各节看到的 `spin_acquire()`、`mutex_acquire_nest()`、`lock_contended()` 等钩子此时都为空。开发和测试内核时强烈建议打开这些选项。本节最后还介绍几种在生产内核中也可用的锁问题诊断手段。

## 核心数据结构

### 锁类的标识：lock_class_key

每把锁内嵌一个 `struct lockdep_map`（开启 `CONFIG_DEBUG_LOCK_ALLOC` 时），其中的 `key` 指向一个**静态分配**的 `lock_class_key`，锁类就以这个 key 的地址区分：

[`include/linux/lockdep_types.h:186-198`](../../linux/include/linux/lockdep_types.h#L186-L198)（v6.18.60）

```c
struct lockdep_map {
	struct lock_class_key		*key;
	struct lock_class		*class_cache[NR_LOCKDEP_CACHING_CLASSES];
	const char			*name;
	u8				wait_type_outer; /* can be taken in this context */
	u8				wait_type_inner; /* presents this context */
	u8				lock_type;
	/* u8				hole; */
#ifdef CONFIG_LOCK_STAT
	int				cpu;
	unsigned long			ip;
#endif
};
```

动态初始化的锁通过初始化宏中的 `static` 变量获得 key：

[`include/linux/mutex.h:60-65`](../../linux/include/linux/mutex.h#L60-L65)（v6.18.60）

```c
#define mutex_init(mutex)						\
do {									\
	static struct lock_class_key __key;				\
									\
	__mutex_init((mutex), #mutex, &__key);				\
} while (0)
```

每个 `mutex_init()` 调用点展开后都有一个独立的 `static struct lock_class_key __key`，因此"同一行初始化代码"天然对应"同一个锁类"。静态定义的锁（`DEFINE_SPINLOCK()` 等）则以锁变量自身的地址作为 key。

### 锁类与依赖图

[`include/linux/lockdep_types.h:98-140`](../../linux/include/linux/lockdep_types.h#L98-L140)（v6.18.60）

```c
struct lock_class {
	/*
	 * class-hash:
	 */
	struct hlist_node		hash_entry;

	/*
	 * Entry in all_lock_classes when in use. Entry in free_lock_classes
	 * when not in use. Instances that are being freed are on one of the
	 * zapped_classes lists.
	 */
	struct list_head		lock_entry;

	/*
	 * These fields represent a directed graph of lock dependencies,
	 * to every node we attach a list of "forward" and a list of
	 * "backward" graph nodes.
	 */
	struct list_head		locks_after, locks_before;

	const struct lockdep_subclass_key *key;
	lock_cmp_fn			cmp_fn;
	lock_print_fn			print_fn;

	unsigned int			subclass;
	unsigned int			dep_gen_id;

	/*
	 * IRQ/softirq usage tracking bits:
	 */
	unsigned long			usage_mask;
	const struct lock_trace		*usage_traces[LOCK_TRACE_STATES];

	const char			*name;
	/*
	 * Generation counter, when doing certain classes of graph walking,
	 * to ensure that we check one node only once:
	 */
	int				name_version;

	u8				wait_type_inner;
	u8				wait_type_outer;
	u8				lock_type;
```

- `locks_after` / `locks_before`：依赖图中的出边与入边。若曾在持有 A 时获取 B，就有一条 A → B 的边。
- `usage_mask`：该类锁被使用的上下文位图，例如"曾在硬中断中获取"（`USED_IN_HARDIRQ`）、"曾在开硬中断时获取"（`ENABLED_HARDIRQ`），每种状态再分读 / 写两种。状态定义在 `kernel/locking/lockdep_states.h`，只有 `HARDIRQ` 与 `SOFTIRQ` 两种。
- `wait_type_inner` / `wait_type_outer`：锁的"等待类型"，用于检查睡眠锁与自旋锁的嵌套是否合法。

### 持有锁栈：held_lock

每个任务在 `task_struct` 中维护一个持有锁的栈 `held_locks[]`（深度 `lockdep_depth`），每项为：

[`include/linux/lockdep_types.h:206-258`](../../linux/include/linux/lockdep_types.h#L206-L258)（v6.18.60）

```c
struct held_lock {
	/*
	 * One-way hash of the dependency chain up to this point. We
	 * hash the hashes step by step as the dependency chain grows.
	 *
	 * We use it for dependency-caching and we skip detection
	 * passes and dependency-updates if there is a cache-hit, so
	 * it is absolutely critical for 100% coverage of the validator
	 * to have a unique key value for every unique dependency path
	 * that can occur in the system, to make a unique hash value
	 * as likely as possible - hence the 64-bit width.
	 *
	 * The task struct holds the current hash value (initialized
	 * with zero), here we store the previous hash value:
	 */
	u64				prev_chain_key;
	unsigned long			acquire_ip;
	struct lockdep_map		*instance;
	struct lockdep_map		*nest_lock;
#ifdef CONFIG_LOCK_STAT
	u64 				waittime_stamp;
	u64				holdtime_stamp;
#endif
	/*
	 * class_idx is zero-indexed; it points to the element in
	 * lock_classes this held lock instance belongs to. class_idx is in
	 * the range from 0 to (MAX_LOCKDEP_KEYS-1) inclusive.
	 */
	unsigned int			class_idx:MAX_LOCKDEP_KEYS_BITS;
	/*
	 * The lock-stack is unified in that the lock chains of interrupt
	 * contexts nest ontop of process context chains, but we 'separate'
	 * the hashes by starting with 0 if we cross into an interrupt
	 * context, and we also keep do not add cross-context lock
	 * dependencies - the lock usage graph walking covers that area
	 * anyway, and we'd just unnecessarily increase the number of
	 * dependencies otherwise. [Note: hardirq and softirq contexts
	 * are separated from each other too.]
	 *
	 * The following field is used to detect when we cross into an
	 * interrupt context:
	 */
	unsigned int irq_context:2; /* bit 0 - soft, bit 1 - hard */
	unsigned int trylock:1;						/* 16 bits */

	unsigned int read:2;        /* see lock_acquire() comment */
	unsigned int check:1;       /* see lock_acquire() comment */
	unsigned int hardirqs_off:1;
	unsigned int sync:1;
	unsigned int references:11;					/* 32 bits */
	unsigned int pin_count;
};

```

`prev_chain_key` 是到此为止整条持锁链的 64 位哈希。lockdep 用它作为**依赖链缓存**的键：同一条加锁序列（如"A → B → C"）第二次出现时直接命中缓存，跳过昂贵的图搜索，这是 lockdep 能在实际内核中运行的关键优化。

## 主流程分析

### 调用链总览

```
spin_lock() / mutex_lock() / ...
└── spin_acquire() / mutex_acquire() → lock_acquire()        kernel/locking/lockdep.c:5861
    └── __lock_acquire()                                    kernel/locking/lockdep.c:5108
        ├── 查找 / 注册锁类（register_lock_class）
        ├── mark_usage()                // 更新 usage_mask，检查中断安全
        ├── check_wait_context()        // 检查等待类型嵌套
        ├── 压入 held_locks[]，更新 chain_key
        └── validate_chain()                                kernel/locking/lockdep.c:3889
            ├── lookup_chain_cache_add()  // 命中缓存则直接返回
            ├── check_deadlock()          // 同类锁递归
            └── check_prevs_add()         // 对栈中每个前驱 prev
                └── check_prev_add(prev, next)              kernel/locking/lockdep.c:3150
                    ├── check_noncircular()   // BFS：next 能否到达 prev？
                    ├── check_irq_usage()     // 中断安全反转
                    └── 添加边 prev → next
```

### validate_chain()：只验证新链

[`kernel/locking/lockdep.c:3889-3948`](../../linux/kernel/locking/lockdep.c#L3889-L3948)（v6.18.60）

```c
static int validate_chain(struct task_struct *curr,
			  struct held_lock *hlock,
			  int chain_head, u64 chain_key)
{
	/*
	 * Trylock needs to maintain the stack of held locks, but it
	 * does not add new dependencies, because trylock can be done
	 * in any order.
	 *
	 * We look up the chain_key and do the O(N^2) check and update of
	 * the dependencies only if this is a new dependency chain.
	 * (If lookup_chain_cache_add() return with 1 it acquires
	 * graph_lock for us)
	 */
	if (!hlock->trylock && hlock->check &&
	    lookup_chain_cache_add(curr, hlock, chain_key)) {
		/*
		 * Check whether last held lock:
		 *
		 * - is irq-safe, if this lock is irq-unsafe
		 * - is softirq-safe, if this lock is hardirq-unsafe
		 *
		 * And check whether the new lock's dependency graph
		 * could lead back to the previous lock:
		 *
		 * - within the current held-lock stack
		 * - across our accumulated lock dependency records
		 *
		 * any of these scenarios could lead to a deadlock.
		 */
		/*
		 * The simple case: does the current hold the same lock
		 * already?
		 */
		int ret = check_deadlock(curr, hlock);

		if (!ret)
			return 0;
		/*
		 * Add dependency only if this lock is not the head
		 * of the chain, and if the new lock introduces no more
		 * lock dependency (because we already hold a lock with the
		 * same lock class) nor deadlock (because the nest_lock
		 * serializes nesting locks), see the comments for
		 * check_deadlock().
		 */
		if (!chain_head && ret != 2) {
			if (!check_prevs_add(curr, hlock))
				return 0;
		}

		graph_unlock();
	} else {
		/* after lookup_chain_cache_add(): */
		if (unlikely(!debug_locks))
			return 0;
	}

	return 1;
}
```

trylock 不会造成等待，因此不添加依赖（但仍压栈）。只有 `lookup_chain_cache_add()` 发现这是一条**新的**依赖链时，才执行后续的检查；绝大多数加锁都命中缓存，开销只是一次哈希查找。

### 检查一：递归加锁

[`kernel/locking/lockdep.c:3085-3125`](../../linux/kernel/locking/lockdep.c#L3085-L3125)（v6.18.60）

```c
check_deadlock(struct task_struct *curr, struct held_lock *next)
{
	struct lock_class *class;
	struct held_lock *prev;
	struct held_lock *nest = NULL;
	int i;

	for (i = 0; i < curr->lockdep_depth; i++) {
		prev = curr->held_locks + i;

		if (prev->instance == next->nest_lock)
			nest = prev;

		if (hlock_class(prev) != hlock_class(next))
			continue;

		/*
		 * Allow read-after-read recursion of the same
		 * lock class (i.e. read_lock(lock)+read_lock(lock)):
		 */
		if ((next->read == 2) && prev->read)
			continue;

		class = hlock_class(prev);

		if (class->cmp_fn &&
		    class->cmp_fn(prev->instance, next->instance) < 0)
			continue;

		/*
		 * We're holding the nest_lock, which serializes this lock's
		 * nesting behaviour.
		 */
		if (nest)
			return 2;

		print_deadlock_bug(curr, prev, next);
		return 0;
	}
	return 1;
}
```

如果栈中已经持有**同一类**的锁，通常意味着递归加锁或同类锁嵌套，会被报告为 "possible recursive locking detected"。两种合法情况被排除：递归读锁（`read == 2`）；通过 `nest_lock` 声明了外层锁串行化的嵌套（如 `mm_take_all_locks()` 在持有 `mmap_lock` 时用 `down_write_nest_lock()` 获取所有 anon_vma 与 `i_mmap_rwsem`，[`mm/vma.c:2157`](../../linux/mm/vma.c#L2157)、`2187`）。确实需要嵌套获取同类锁的代码（如按地址顺序锁住两个 inode）应使用 `mutex_lock_nested(lock, subclass)` 等接口为内层锁指定不同的子类。

### 检查二：依赖环

[`kernel/locking/lockdep.c:3149-3198`](../../linux/kernel/locking/lockdep.c#L3149-L3198)（v6.18.60）

```c
static int
check_prev_add(struct task_struct *curr, struct held_lock *prev,
	       struct held_lock *next, u16 distance,
	       struct lock_trace **const trace)
{
	struct lock_list *entry;
	enum bfs_result ret;

	if (!hlock_class(prev)->key || !hlock_class(next)->key) {
		/*
		 * The warning statements below may trigger a use-after-free
		 * of the class name. It is better to trigger a use-after free
		 * and to have the class name most of the time instead of not
		 * having the class name available.
		 */
		WARN_ONCE(!debug_locks_silent && !hlock_class(prev)->key,
			  "Detected use-after-free of lock class %px/%s\n",
			  hlock_class(prev),
			  hlock_class(prev)->name);
		WARN_ONCE(!debug_locks_silent && !hlock_class(next)->key,
			  "Detected use-after-free of lock class %px/%s\n",
			  hlock_class(next),
			  hlock_class(next)->name);
		return 2;
	}

	if (prev->class_idx == next->class_idx) {
		struct lock_class *class = hlock_class(prev);

		if (class->cmp_fn &&
		    class->cmp_fn(prev->instance, next->instance) < 0)
			return 2;
	}

	/*
	 * Prove that the new <prev> -> <next> dependency would not
	 * create a circular dependency in the graph. (We do this by
	 * a breadth-first search into the graph starting at <next>,
	 * and check whether we can reach <prev>.)
	 *
	 * The search is limited by the size of the circular queue (i.e.,
	 * MAX_CIRCULAR_QUEUE_SIZE) which keeps track of a breadth of nodes
	 * in the graph whose neighbours are to be checked.
	 */
	ret = check_noncircular(next, prev, trace);
	if (unlikely(bfs_error(ret) || ret == BFS_RMATCH))
		return 0;

	if (!check_irq_usage(curr, prev, next))
		return 0;
```

要添加边 `prev → next`，先检查图中是否已有 `next → ... → prev` 的路径。如果有，加上这条边就会成环，意味着存在两条加锁顺序相反的路径——经典的 ABBA 死锁：

```
路径 1（曾经发生）：lock(A); lock(B);       → 边 A → B
路径 2（当前）：    lock(B); lock(A);       → 欲加边 B → A，BFS 从 A 出发找到 B，成环
```

`check_noncircular()` 用广度优先搜索实现：

[`kernel/locking/lockdep.c:2177-2190`](../../linux/kernel/locking/lockdep.c#L2177-L2190)（v6.18.60）

```c
check_noncircular(struct held_lock *src, struct held_lock *target,
		  struct lock_trace **const trace)
{
	enum bfs_result ret;
	struct lock_list *target_entry;
	struct lock_list src_entry;

	bfs_init_root(&src_entry, src);

	debug_atomic_inc(nr_cyclic_checks);

	ret = check_path(target, &src_entry, hlock_conflict, NULL, &target_entry);

	if (unlikely(ret == BFS_RMATCH)) {
```

报告形如 "WARNING: possible circular locking dependency detected"，并打印环上每条边首次建立时的调用栈。注意：这两条路径可能相隔数小时、在不同的任务中执行，lockdep 依然能发现问题。

### 检查三：中断安全

如果锁 L 曾在硬中断中被获取（`USED_IN_HARDIRQ`），而某处又在**开中断**的情况下获取 L（`ENABLED_HARDIRQ`），就存在死锁可能：进程上下文持有 L 时被中断打断，中断处理函数再获取 L，自旋到永远。`mark_usage()` 在每次加锁时更新锁类的 `usage_mask` 并检查这对矛盾状态，报告为 "inconsistent {IN-HARDIRQ-W} -> {HARDIRQ-ON-W} usage"。

`check_irq_usage()` 还会检查跨锁的情况：如果持有一把"中断不安全"的锁 A 时获取了"中断安全"的锁 B（形成 A → B），而 B 可能在中断中被获取，那么任何"中断中获取 B，再间接等待 A"的路径都会死锁。它沿依赖图向前、向后搜索，寻找这种"安全—不安全"的反转，报告为 "HARDIRQ-safe -> HARDIRQ-unsafe lock order detected"。

### 检查四：等待上下文

[`include/linux/lockdep_types.h:17-31`](../../linux/include/linux/lockdep_types.h#L17-L31)（v6.18.60）

```c
enum lockdep_wait_type {
	LD_WAIT_INV = 0,	/* not checked, catch all */

	LD_WAIT_FREE,		/* wait free, rcu etc.. */
	LD_WAIT_SPIN,		/* spin loops, raw_spinlock_t etc.. */

#ifdef CONFIG_PROVE_RAW_LOCK_NESTING
	LD_WAIT_CONFIG,		/* preemptible in PREEMPT_RT, spinlock_t etc.. */
#else
	LD_WAIT_CONFIG = LD_WAIT_SPIN,
#endif
	LD_WAIT_SLEEP,		/* sleeping locks, mutex_t etc.. */

	LD_WAIT_MAX,		/* must be last */
};
```

[`kernel/locking/lockdep.c:4880-4933`](../../linux/kernel/locking/lockdep.c#L4880-L4933)（v6.18.60）

```c
static int check_wait_context(struct task_struct *curr, struct held_lock *next)
{
	u8 next_inner = hlock_class(next)->wait_type_inner;
	u8 next_outer = hlock_class(next)->wait_type_outer;
	u8 curr_inner;
	int depth;

	if (!next_inner || next->trylock)
		return 0;

	if (!next_outer)
		next_outer = next_inner;

	/*
	 * Find start of current irq_context..
	 */
	for (depth = curr->lockdep_depth - 1; depth >= 0; depth--) {
		struct held_lock *prev = curr->held_locks + depth;
		if (prev->irq_context != next->irq_context)
			break;
	}
	depth++;

	curr_inner = task_wait_context(curr);

	for (; depth < curr->lockdep_depth; depth++) {
		struct held_lock *prev = curr->held_locks + depth;
		struct lock_class *class = hlock_class(prev);
		u8 prev_inner = class->wait_type_inner;

		if (prev_inner) {
			/*
			 * We can have a bigger inner than a previous one
			 * when outer is smaller than inner, as with RCU.
			 *
			 * Also due to trylocks.
			 */
			curr_inner = min(curr_inner, prev_inner);

			/*
			 * Allow override for annotations -- this is typically
			 * only valid/needed for code that only exists when
			 * CONFIG_PREEMPT_RT=n.
			 */
			if (unlikely(class->lock_type == LD_LOCK_WAIT_OVERRIDE))
				curr_inner = prev_inner;
		}
	}

	if (next_outer > curr_inner)
		return print_lock_invalid_wait_context(curr, next);

	return 0;
}
```

每个锁类有一个"内层等待类型"：`raw_spinlock_t` 为 `LD_WAIT_SPIN`，`spinlock_t` 为 `LD_WAIT_CONFIG`，`mutex` 为 `LD_WAIT_SLEEP`。`check_wait_context()` 计算当前上下文（中断上下文、已持有的锁）允许的最弱等待类型，若新锁要求更"弱"（数值更大）的上下文就报告 "BUG: Invalid wait context"。典型错误是在持有 `raw_spinlock_t` 时获取 `spinlock_t`：在普通内核中这没有问题，但在 PREEMPT_RT 内核中 `spinlock_t` 会睡眠。开启 `CONFIG_PROVE_RAW_LOCK_NESTING` 后，`LD_WAIT_CONFIG` 与 `LD_WAIT_SPIN` 区分开来，让非 RT 内核也能提前发现这类问题。

## 运行时信息与限制

开启 lockdep 后可以查看：

| 文件 | 内容 |
|---|---|
| `/proc/lockdep` | 所有锁类及其依赖数 |
| `/proc/lockdep_chains` | 已缓存的依赖链 |
| `/proc/lockdep_stats` | 锁类、依赖、链的数量与上限 |
| `/proc/lock_stat` | 需要 `CONFIG_LOCK_STAT`：每个锁类的竞争次数、等待与持有时间 |

lockdep 的所有数据结构都是静态数组，容量由 `MAX_LOCKDEP_KEYS`（8192 个锁类）、`MAX_LOCKDEP_ENTRIES`（`1 << CONFIG_LOCKDEP_BITS` 条依赖，[`kernel/locking/lockdep_internals.h:104`](../../linux/kernel/locking/lockdep_internals.h#L104)）等决定。耗尽时打印 "BUG: MAX_LOCKDEP_ENTRIES too low!" 并**永久关闭** lockdep（`debug_locks = 0`）。同样，lockdep 报告第一个问题后也会自行关闭，后续问题不再报告——修复第一个报告后需要重新测试。

## 生产环境中的锁问题诊断

lockdep 开销较大（加锁路径慢数倍、额外内存数十 MB），通常只在测试内核中使用。生产内核（如本书配置）可以借助以下机制：

### hung task 检测

`CONFIG_DETECT_HUNG_TASK=y` 时，`khungtaskd` 线程定期检查处于 `TASK_UNINTERRUPTIBLE`（D 状态）超过 `kernel.hung_task_timeout_secs`（默认 `CONFIG_DEFAULT_HUNG_TASK_TIMEOUT=120` 秒）且期间没有被调度过的任务，打印 "INFO: task xxx blocked for more than 120 seconds" 及其调用栈。本书配置开启的 `CONFIG_DETECT_HUNG_TASK_BLOCKER` 会额外报告该任务阻塞在哪个 mutex / semaphore / rwsem 上、以及持有者是谁（7.6、7.7 节中看到的 `hung_task_set_blocker()` 即为此记录信息）。

### 软锁死与硬锁死检测

`CONFIG_SOFTLOCKUP_DETECTOR=y`：每个 CPU 上的高精度定时器检查 `watchdog/N`（stopper）任务是否在 `2 × watchdog_thresh`（默认 20 秒，[`kernel/watchdog.c:49`](../../linux/kernel/watchdog.c#L49)）内得到运行；若否，说明该 CPU 长时间在内核态不可调度（如持自旋锁死循环），报告 "BUG: soft lockup"。

`CONFIG_HARDLOCKUP_DETECTOR=y`（本书配置使用基于 perf NMI 的实现）：用 NMI 检查定时器中断是否仍在递增，若 CPU 长时间关中断不响应，报告 "Watchdog detected hard LOCKUP"。

### 锁竞争分析

v5.19 起的 `lock:contention_begin` / `lock:contention_end` 跟踪点无需 lockdep 即可工作，`perf lock contention` 基于它们统计竞争（7.4 节）。配合 BPF（`-b` 选项）可以在内核中聚合，开销很低，适合生产环境短时间采样。

### 其他调试选项

| 选项 | 作用 |
|---|---|
| `CONFIG_DEBUG_ATOMIC_SLEEP` | 在原子上下文中调用可睡眠函数时报告（7.1 节） |
| `CONFIG_DEBUG_SPINLOCK` | 检测未初始化、重复解锁、非持有者解锁等错误 |
| `CONFIG_DEBUG_MUTEXES` | mutex 的各类使用错误检查 |
| `CONFIG_DEBUG_RWSEMS` | rwsem 的使用错误检查 |
| `CONFIG_PROVE_RCU` | 检查 `rcu_dereference()` 是否在合法上下文中调用 |
| `CONFIG_KCSAN` | 基于编译器插桩的数据竞争检测器（7.3 节） |
| `CONFIG_LOCK_TORTURE_TEST` / `CONFIG_RCU_TORTURE_TEST` | 锁与 RCU 的压力测试模块 |

## 关键细节 / 设计考量

### 误报与注解

lockdep 以锁类为单位推理，有时会把实际上不可能并发的路径当成死锁（例如两个 inode 的锁在不同文件系统层级上嵌套）。消除误报的正确方式是为锁提供更精确的类信息：`lockdep_set_class()` 为特定实例指定不同的 key、`*_nested()` 接口指定子类、`lockdep_set_novalidate_class()` 完全跳过验证（应尽量避免）。滥用这些注解会掩盖真实问题。

### lockdep 能覆盖哪些原语

除了自旋锁、读写锁、mutex、rwsem，lockdep 还通过"伪锁"（单独的 `lockdep_map`）跟踪非锁同步：`local_lock`（7.1 节）、`rcu_read_lock()`（`rcu_lock_map`）、工作队列中 `flush_work()` 与 work 函数之间的依赖等。基于 `wait_for_completion()` 等"跨上下文释放"原语的依赖目前不在 lockdep 覆盖范围内。

## 动手实验

以下实验需要在开启了 `CONFIG_PROVE_LOCKING` 的内核上进行（在 dev 虚拟机中自行编译）。

```c
// 实验模块（非内核源码）：制造一个 ABBA 依赖，lockdep 会在第二个函数执行时报告
static DEFINE_MUTEX(a);
static DEFINE_MUTEX(b);

static void path1(void) { mutex_lock(&a); mutex_lock(&b); mutex_unlock(&b); mutex_unlock(&a); }
static void path2(void) { mutex_lock(&b); mutex_lock(&a); mutex_unlock(&a); mutex_unlock(&b); }

static int __init abba_init(void)
{
	path1();
	path2();	/* 单线程顺序执行，不会真正死锁，但 lockdep 会报告 */
	return 0;
}
```

```bash
dmesg | grep -A40 "possible circular locking dependency"
cat /proc/lockdep_stats | head
```

在默认配置内核上，可以观察 hung task 与软锁死检测器的参数：

```bash
sysctl kernel.hung_task_timeout_secs kernel.watchdog_thresh kernel.softlockup_panic
```

## 小结

- lockdep 以"锁类"为单位记录加锁顺序，构建全局依赖图，在死锁真正发生之前就能报告可能的死锁。
- 锁类由静态 `lock_class_key` 的地址标识，每个初始化调用点对应一个类；`held_lock` 栈记录当前持有的锁，链哈希用于缓存已验证的依赖链。
- 主要检查包括：同类锁递归、依赖环（BFS 检测 ABBA）、中断安全（`USED_IN_*` 与 `ENABLED_*` 冲突及其跨锁反转）、等待上下文（防止在自旋锁内获取可睡眠锁）。
- lockdep 在报告第一个问题或资源耗尽后自动关闭；误报应通过子类或独立 key 注解解决。
- 本书配置未开启 lockdep；生产环境可依靠 hung task 检测（含阻塞者信息）、软 / 硬锁死检测和 `perf lock contention` 诊断锁问题。

## 参考

- `linux/Documentation/locking/lockdep-design.rst`
- `linux/Documentation/locking/lockstat.rst`
- `linux/Documentation/locking/locktorture.rst`
- `linux/Documentation/admin-guide/lockup-watchdogs.rst`
- Ingo Molnar, "lock validator" 补丁系列（v2.6.18 合入）
