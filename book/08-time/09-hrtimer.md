# 8.9 高精度定时器：hrtimer

> **本节源码**：[`include/linux/hrtimer_types.h`](../../linux/include/linux/hrtimer_types.h)、[`include/linux/hrtimer_defs.h`](../../linux/include/linux/hrtimer_defs.h)、[`include/linux/hrtimer.h`](../../linux/include/linux/hrtimer.h)、[`kernel/time/hrtimer.c`](../../linux/kernel/time/hrtimer.c)、[`lib/timerqueue.c`](../../linux/lib/timerqueue.c)
> **关键函数**：`hrtimer_setup()`、`hrtimer_start_range_ns()`、`__hrtimer_start_range_ns()`、`switch_hrtimer_base()`、`enqueue_hrtimer()`、`hrtimer_reprogram()`、`hrtimer_interrupt()`、`__hrtimer_run_queues()`、`__run_hrtimer()`、`hrtimer_run_softirq()`、`hrtimer_try_to_cancel()`、`hrtimer_forward()`、`hrtimer_run_queues()`、`hrtimer_switch_to_hres()`、`clock_was_set()`、`do_nanosleep()`
> **前置知识**：8.4 时间维护、8.6 时钟事件设备与 tick 设备、8.7 时间轮定时器

## 概述

时间轮的粒度是 jiffy，且越远越粗，不适合需要精确定时的场合：`nanosleep()` 要求睡眠 100 μs 就是 100 μs，POSIX 定时器要求周期稳定，调度器的 hrtick、tick 本身的模拟都需要纳秒级的到期时间。高精度定时器（High-Resolution Timer，hrtimer）是另一套独立的定时器框架，它的设计取向与时间轮正好相反：

| | 时间轮 `timer_list` | 高精度定时器 `hrtimer` |
|---|---|---|
| 时间单位 | jiffy | 纳秒（`ktime_t`） |
| 精度 | 随距离降低，最差约 12.5% | 精确到期（加可选的松弛） |
| 数据结构 | 每 CPU 散列桶 | 每 CPU、每时钟一棵红黑树 |
| 插入 / 删除 | O(1) | O(log n) |
| 可选时钟 | jiffies（单调） | MONOTONIC、REALTIME、BOOTTIME、TAI |
| 回调上下文 | 软中断 | 默认硬中断，可选软中断 |
| 典型用途 | 超时 | 睡眠、POSIX 定时器、tick 模拟、调度 |

*表 8-20：时间轮与 hrtimer 的对比*

hrtimer 有两种运行模式：**高精度模式**下，每次编程时钟事件设备都对准下一个到期的 hrtimer，`hrtimer_interrupt()` 就是 tick 设备的中断回调；**低精度模式**（启动早期或 `highres=off`）下，hrtimer 只能在周期 tick 中被检查，精度退化为 1 个 jiffy。本节分析 hrtimer 的数据结构、启动与到期流程、`CLOCK_REALTIME` 被修改时的处理，以及基于 hrtimer 的睡眠。

## 核心数据结构

### struct hrtimer

[`include/linux/hrtimer_types.h:39-48`](../../linux/include/linux/hrtimer_types.h#L39-L48)（v6.18.60）

```c
struct hrtimer {
	struct timerqueue_node		node;		// [注] 红黑树节点 + 硬到期时间 node.expires
	ktime_t				_softexpires;	// [注] 软到期时间：最早允许到期的时刻
	enum hrtimer_restart		(*__private function)(struct hrtimer *);
	struct hrtimer_clock_base	*base;		// [注] 所属的每 CPU、每时钟基
	u8				state;		// [注] INACTIVE（0）或 ENQUEUED（1）
	u8				is_rel;
	u8				is_soft;	// [注] 在软中断中到期
	u8				is_hard;	// [注] 即使在 PREEMPT_RT 下也在硬中断中到期
};
```

每个 hrtimer 有两个到期时间——软到期（Soft Expiry）与硬到期（Hard Expiry），构成一个区间 `[_softexpires, node.expires]`：

[`include/linux/hrtimer.h:109-113`](../../linux/include/linux/hrtimer.h#L109-L113)（v6.18.60）

```c
static inline void hrtimer_set_expires_range_ns(struct hrtimer *timer, ktime_t time, u64 delta)
{
	timer->_softexpires = time;
	timer->node.expires = ktime_add_safe(time, ns_to_ktime(delta));
}
```

红黑树按硬到期时间 `node.expires` 排序，决定何时编程硬件；到期处理时，只要软到期时间已过就执行回调。中间的松弛（slack）`delta` 让到期时间相近的定时器可以在同一次中断中一并处理，减少唤醒次数。用户态任务的睡眠默认带 50 μs 松弛（`timer_slack_ns`，[`init/init_task.c:146`](../../linux/init/init_task.c#L146)），可用 `prctl(PR_SET_TIMERSLACK)` 修改；实时调度类任务的松弛为 0。

回调返回 `HRTIMER_RESTART` 表示定时器已用 `hrtimer_forward()` 推后到期时间、需要重新入队，返回 `HRTIMER_NORESTART` 表示一次性定时器到此结束。`function` 字段带 `__private` 标注，只能通过 `hrtimer_setup()` 设置，v6.15 起不再允许直接赋值。

`state` 只有"在树中"与"不在树中"两种取值。回调是否正在执行不记录在定时器本身，而是记录在 `base->running` 中——注释解释说，若在回调结束后还要修改 `timer->state`，回调就不能释放定时器本身（[`include/linux/hrtimer.h:58-83`](../../linux/include/linux/hrtimer.h#L58-L83)）。

### 模式

[`include/linux/hrtimer.h:35-56`](../../linux/include/linux/hrtimer.h#L35-L56)（v6.18.60，节选）

```c
enum hrtimer_mode {
	HRTIMER_MODE_ABS	= 0x00,
	HRTIMER_MODE_REL	= 0x01,
	HRTIMER_MODE_PINNED	= 0x02,
	HRTIMER_MODE_SOFT	= 0x04,
	HRTIMER_MODE_HARD	= 0x08,
	...
};
```

| 位 | 含义 |
|---|---|
| `ABS` / `REL` | 到期时间是绝对时间还是相对于现在 |
| `PINNED` | 固定在当前 CPU，不参与迁移 |
| `SOFT` | 回调在 `HRTIMER_SOFTIRQ` 软中断中执行 |
| `HARD` | 回调在硬中断中执行；`PREEMPT_RT` 下未标记 `HARD` 的定时器一律改为软中断执行 |

*表 8-21：hrtimer 的模式位*

### 时钟基与 CPU 基

[`include/linux/hrtimer_defs.h:46-66`](../../linux/include/linux/hrtimer_defs.h#L46-L66)（v6.18.60）

```c
struct hrtimer_clock_base {
	struct hrtimer_cpu_base	*cpu_base;
	unsigned int		index;
	clockid_t		clockid;
	seqcount_raw_spinlock_t	seq;		// [注] 配合 hrtimer_active() 的无锁判断
	struct hrtimer		*running;	// [注] 正在执行回调的定时器
	struct timerqueue_head	active;		// [注] 红黑树（缓存最左节点）
	ktime_t			offset;		// [注] 本时钟相对于 MONOTONIC 的偏移
} __hrtimer_clock_base_align;

enum  hrtimer_base_type {
	HRTIMER_BASE_MONOTONIC,
	HRTIMER_BASE_REALTIME,
	HRTIMER_BASE_BOOTTIME,
	HRTIMER_BASE_TAI,
	HRTIMER_BASE_MONOTONIC_SOFT,
	HRTIMER_BASE_REALTIME_SOFT,
	HRTIMER_BASE_BOOTTIME_SOFT,
	HRTIMER_BASE_TAI_SOFT,
	HRTIMER_MAX_CLOCK_BASES,
};
```

每个 CPU 有 8 个时钟基：4 种时钟 × 硬 / 软两种到期上下文。每个时钟基一棵红黑树，树中定时器的到期时间以**本时钟**表示（例如 REALTIME 时钟基中存放的是墙上时间），而编程硬件时统一换算为单调时间：`expires − offset`。`timerqueue_head` 是带最左节点缓存的红黑树（`rb_root_cached`，[`include/linux/timerqueue_types.h:8-15`](../../linux/include/linux/timerqueue_types.h#L8-L15)），取最早到期的定时器为 O(1)。

[`include/linux/hrtimer_defs.h:101-127`](../../linux/include/linux/hrtimer_defs.h#L101-L127)（v6.18.60）

```c
struct hrtimer_cpu_base {
	raw_spinlock_t			lock;		// [注] 保护本 CPU 所有时钟基
	unsigned int			cpu;
	unsigned int			active_bases;	// [注] 位图：哪些时钟基非空
	unsigned int			clock_was_set_seq;
	unsigned int			hres_active		: 1,	// [注] 高精度模式已启用
					in_hrtirq		: 1,	// [注] 正在 hrtimer_interrupt() 中
					hang_detected		: 1,
					softirq_activated       : 1,	// [注] 已触发 HRTIMER_SOFTIRQ
					online			: 1;
#ifdef CONFIG_HIGH_RES_TIMERS
	unsigned int			nr_events;
	unsigned short			nr_retries;
	unsigned short			nr_hangs;
	unsigned int			max_hang_time;
#endif
#ifdef CONFIG_PREEMPT_RT
	spinlock_t			softirq_expiry_lock;
	atomic_t			timer_waiters;
#endif
	ktime_t				expires_next;		// [注] 硬件当前被编程的到期时间
	struct hrtimer			*next_timer;
	ktime_t				softirq_expires_next;	// [注] 软定时器中最早的到期时间
	struct hrtimer			*softirq_next_timer;
	struct hrtimer_clock_base	clock_base[HRTIMER_MAX_CLOCK_BASES];
	call_single_data_t		csd;			// [注] 远程重新编程用的 IPI
} ____cacheline_aligned;
```

每 CPU 的 `hrtimer_bases` 在 [`kernel/time/hrtimer.c:72-111`](../../linux/kernel/time/hrtimer.c#L72-L111) 中静态初始化。一把 `lock` 保护本 CPU 的全部 8 棵树，`active_bases` 的低 4 位对应硬时钟基、高 4 位对应软时钟基（`HRTIMER_ACTIVE_HARD` / `HRTIMER_ACTIVE_SOFT`，[`kernel/time/hrtimer.c:56-59`](../../linux/kernel/time/hrtimer.c#L56-L59)），遍历时只访问非空的时钟基。

```
 hrtimer_bases（每 CPU）
 ├── lock / expires_next / softirq_expires_next / active_bases
 ├── clock_base[0] MONOTONIC        active: ●───●──●────●     （红黑树，最左 = 最早）
 ├── clock_base[1] REALTIME         offset = offs_real
 ├── clock_base[2] BOOTTIME         offset = offs_boot
 ├── clock_base[3] TAI              offset = offs_tai
 ├── clock_base[4] MONOTONIC_SOFT
 ├── ...
 └── clock_base[7] TAI_SOFT
 硬件编程时刻 = min(各硬时钟基最左节点 expires − offset, softirq_expires_next)
```

*图 8-14：每 CPU 的 hrtimer 数据结构*

## 初始化：hrtimer_setup()

[`kernel/time/hrtimer.c:1612-1652`](../../linux/kernel/time/hrtimer.c#L1612-L1652)（v6.18.60）

```c
static void __hrtimer_setup(struct hrtimer *timer,
			    enum hrtimer_restart (*function)(struct hrtimer *),
			    clockid_t clock_id, enum hrtimer_mode mode)
{
	bool softtimer = !!(mode & HRTIMER_MODE_SOFT);
	struct hrtimer_cpu_base *cpu_base;
	int base;

	/*
	 * On PREEMPT_RT enabled kernels hrtimers which are not explicitly
	 * marked for hard interrupt expiry mode are moved into soft
	 * interrupt context for latency reasons and because the callbacks
	 * can invoke functions which might sleep on RT, e.g. spin_lock().
	 */
	if (IS_ENABLED(CONFIG_PREEMPT_RT) && !(mode & HRTIMER_MODE_HARD))
		softtimer = true;

	memset(timer, 0, sizeof(struct hrtimer));

	cpu_base = raw_cpu_ptr(&hrtimer_bases);

	/*
	 * POSIX magic: Relative CLOCK_REALTIME timers are not affected by
	 * clock modifications, so they needs to become CLOCK_MONOTONIC to
	 * ensure POSIX compliance.
	 */
	if (clock_id == CLOCK_REALTIME && mode & HRTIMER_MODE_REL)
		clock_id = CLOCK_MONOTONIC;

	base = softtimer ? HRTIMER_MAX_CLOCK_BASES / 2 : 0;
	base += hrtimer_clockid_to_base(clock_id);
	timer->is_soft = softtimer;
	timer->is_hard = !!(mode & HRTIMER_MODE_HARD);
	timer->base = &cpu_base->clock_base[base];
	timerqueue_init(&timer->node);

	if (WARN_ON_ONCE(!function))
		ACCESS_PRIVATE(timer, function) = hrtimer_dummy_timeout;
	else
		ACCESS_PRIVATE(timer, function) = function;
}
```

初始化时就确定了定时器使用哪个时钟基，其中有一条 POSIX 语义的特殊规则：**相对的 `CLOCK_REALTIME` 定时器改用 `CLOCK_MONOTONIC`**。"10 秒后到期"不应因为有人把墙上时间调快一小时而提前到期；只有"在某个墙上时刻到期"的绝对定时器才需要跟随墙上时间的调整。`timer->base` 初始指向当前 CPU，真正入队时可能迁移到其他 CPU。

## 启动：hrtimer_start_range_ns()

```
hrtimer_start(timer, tim, mode) → hrtimer_start_range_ns(timer, tim, 0, mode)
└── lock_hrtimer_base()                       // 锁住定时器当前所在的 CPU 基
    └── __hrtimer_start_range_ns()
        ├── remove_hrtimer()                  // 若已在树中，先摘下
        ├── REL → ABS：tim += 本时钟的当前时间
        ├── hrtimer_set_expires_range_ns()    // 软 / 硬到期时间
        ├── switch_hrtimer_base()             // 选择目标 CPU（可能迁移）
        ├── enqueue_hrtimer()                 // 插入红黑树，返回是否成为最左节点
        └── 是最左节点 → hrtimer_reprogram()   // 必要时重新编程本 CPU 的时钟事件设备
```

[`kernel/time/hrtimer.c:1204-1264`](../../linux/kernel/time/hrtimer.c#L1204-L1264)（v6.18.60，节选）

```c
static int __hrtimer_start_range_ns(struct hrtimer *timer, ktime_t tim,
				    u64 delta_ns, const enum hrtimer_mode mode,
				    struct hrtimer_clock_base *base)
{
	struct hrtimer_cpu_base *this_cpu_base = this_cpu_ptr(&hrtimer_bases);
	struct hrtimer_clock_base *new_base;
	bool force_local, first, was_armed;

	/*
	 * If the timer is on the local cpu base and is the first expiring
	 * timer then this might end up reprogramming the hardware twice
	 * (on removal and on enqueue). To avoid that prevent the reprogram
	 * on removal, keep the timer local to the current CPU and enforce
	 * reprogramming after it is queued no matter whether it is the new
	 * first expiring timer again or not.
	 */
	force_local = base->cpu_base == this_cpu_base;
	force_local &= base->cpu_base->next_timer == timer;
	...
	was_armed = remove_hrtimer(timer, base, true, force_local);

	if (mode & HRTIMER_MODE_REL)
		tim = ktime_add_safe(tim, __hrtimer_cb_get_time(base->clockid));

	tim = hrtimer_update_lowres(timer, tim, mode);

	hrtimer_set_expires_range_ns(timer, tim, delta_ns);

	/* Switch the timer base, if necessary: */
	if (!force_local) {
		new_base = switch_hrtimer_base(timer, base,
					       mode & HRTIMER_MODE_PINNED);
	} else {
		new_base = base;
	}

	first = enqueue_hrtimer(timer, new_base, mode, was_armed);

	/*
	 * If the hrtimer interrupt is running, then it will reevaluate the
	 * clock bases and reprogram the clock event device.
	 */
	if (new_base->cpu_base->in_hrtirq)
		return false;
	...
```

几处值得注意的优化与约束：

- **避免两次编程**：若被重新启动的定时器正是本 CPU 当前硬件所对准的那个（`next_timer`），摘下时就重新编程、插入后再编程一次是浪费。此时强制留在本地（`force_local`），摘下时不编程，插入后统一用 `hrtimer_force_reprogram()` 重新计算（[`kernel/time/hrtimer.c:1288-1295`](../../linux/kernel/time/hrtimer.c#L1288-L1295)）。这是 `hrtimer_forward()` + 重新启动这一常见周期定时器模式的快路径。
- **中断处理中不编程**：若目标 CPU 正在执行 `hrtimer_interrupt()`（`in_hrtirq`），它在处理完所有到期定时器后会重新计算并编程，这里无需操心。
- **插入**：`enqueue_hrtimer()`（[`kernel/time/hrtimer.c:1071-1083`](../../linux/kernel/time/hrtimer.c#L1071-L1083)）设置 `active_bases` 中的对应位、把状态改为 `ENQUEUED`，再调用 `timerqueue_add()` 插入红黑树，返回值表示新节点是否成为最左节点。

### 选择目标 CPU

[`kernel/time/hrtimer.c:215-228`](../../linux/kernel/time/hrtimer.c#L215-L228)（v6.18.60）

```c
static inline struct hrtimer_cpu_base *get_target_base(struct hrtimer_cpu_base *base, int pinned)
{
	if (!hrtimer_base_is_online(base)) {
		int cpu = cpumask_any_and(cpu_online_mask, housekeeping_cpumask(HK_TYPE_TIMER));

		return &per_cpu(hrtimer_bases, cpu);
	}

#if defined(CONFIG_SMP) && defined(CONFIG_NO_HZ_COMMON)
	if (static_branch_likely(&timers_migration_enabled) && !pinned)
		return &per_cpu(hrtimer_bases, get_nohz_timer_target());
#endif
	return base;
}
```

与时间轮不同，hrtimer 仍沿用"插入时选择目标"的策略：非固定定时器经 `get_nohz_timer_target()` 被放到一个非空闲的 housekeeping CPU 上（受 8.8 节提到的 `kernel.timer_migration` sysctl 控制）。`switch_hrtimer_base()`（[`kernel/time/hrtimer.c:242-289`](../../linux/kernel/time/hrtimer.c#L242-L289)）实现迁移：先把 `timer->base` 指向一个特殊的 `migration_base` 作为"正在迁移"的标记，释放旧锁、获取新锁，再写入新的时钟基；`lock_hrtimer_base()` 看到 `migration_base` 就自旋等待（[`kernel/time/hrtimer.c:154-172`](../../linux/kernel/time/hrtimer.c#L154-L172)），与时间轮的 `TIMER_MIGRATING` 思路相同。

迁移有一个硬性约束，体现在 `hrtimer_suitable_target()`（[`kernel/time/hrtimer.c:188-213`](../../linux/kernel/time/hrtimer.c#L188-L213)）中：**定时器不能成为远程 CPU 上最早到期的定时器**。每 CPU 的时钟事件设备只能由本 CPU 编程，若新定时器比远程 CPU 当前编程的时刻还早，就必须发 IPI 让对方重新编程，而 IPI 未必来得及。因此这种情况下定时器留在本地。只有本 CPU 已下线时才例外，此时通过 `csd` 发 IPI 让目标 CPU 重新编程。

### hrtimer_reprogram()

[`kernel/time/hrtimer.c:796-855`](../../linux/kernel/time/hrtimer.c#L796-L855)（v6.18.60，节选）

```c
static void hrtimer_reprogram(struct hrtimer *timer, bool reprogram)
{
	struct hrtimer_cpu_base *cpu_base = this_cpu_ptr(&hrtimer_bases);
	struct hrtimer_clock_base *base = timer->base;
	ktime_t expires = ktime_sub(hrtimer_get_expires(timer), base->offset);
	...
	if (timer->is_soft) {
		...
		struct hrtimer_cpu_base *timer_cpu_base = base->cpu_base;

		if (timer_cpu_base->softirq_activated)
			return;

		if (!ktime_before(expires, timer_cpu_base->softirq_expires_next))
			return;

		timer_cpu_base->softirq_next_timer = timer;
		timer_cpu_base->softirq_expires_next = expires;

		if (!ktime_before(expires, timer_cpu_base->expires_next) ||
		    !reprogram)
			return;
	}

	/*
	 * If the timer is not on the current cpu, we cannot reprogram
	 * the other cpus clock event device.
	 */
	if (base->cpu_base != cpu_base)
		return;

	if (expires >= cpu_base->expires_next)
		return;
	...
	if (cpu_base->in_hrtirq)
		return;

	cpu_base->next_timer = timer;

	__hrtimer_reprogram(cpu_base, timer, expires);
}
```

把定时器的到期时间换算成单调时间（减去时钟基的 `offset`），若早于硬件当前编程的 `expires_next` 就重新编程。软定时器多一步：先更新 `softirq_expires_next`，只有当它也早于硬件时刻时才编程——软定时器同样需要一次硬件中断来触发软中断。`__hrtimer_reprogram()`（[`kernel/time/hrtimer.c:645-672`](../../linux/kernel/time/hrtimer.c#L645-L672)）在低精度模式或检测到"挂起"（见下文）时只记录不编程，否则调用 `tick_program_event()`（8.6 节）。

## 到期处理

### hrtimer_interrupt()

高精度模式下，tick 设备的 `event_handler` 就是 `hrtimer_interrupt()`：

[`kernel/time/hrtimer.c:1878-1918`](../../linux/kernel/time/hrtimer.c#L1878-L1918)（v6.18.60）

```c
void hrtimer_interrupt(struct clock_event_device *dev)
{
	struct hrtimer_cpu_base *cpu_base = this_cpu_ptr(&hrtimer_bases);
	ktime_t expires_next, now, entry_time, delta;
	unsigned long flags;
	int retries = 0;

	BUG_ON(!cpu_base->hres_active);
	cpu_base->nr_events++;
	dev->next_event = KTIME_MAX;

	raw_spin_lock_irqsave(&cpu_base->lock, flags);
	entry_time = now = hrtimer_update_base(cpu_base);
retry:
	cpu_base->in_hrtirq = 1;
	/*
	 * We set expires_next to KTIME_MAX here with cpu_base->lock
	 * held to prevent that a timer is enqueued in our queue via
	 * the migration code. This does not affect enqueueing of
	 * timers which run their callback and need to be requeued on
	 * this CPU.
	 */
	cpu_base->expires_next = KTIME_MAX;

	if (!ktime_before(now, cpu_base->softirq_expires_next)) {
		cpu_base->softirq_expires_next = KTIME_MAX;
		cpu_base->softirq_activated = 1;
		raise_timer_softirq(HRTIMER_SOFTIRQ);
	}

	__hrtimer_run_queues(cpu_base, now, flags, HRTIMER_ACTIVE_HARD);

	/* Reevaluate the clock bases for the [soft] next expiry */
	expires_next = hrtimer_update_next_event(cpu_base);
	/*
	 * Store the new expiry value so the migration code can verify
	 * against it.
	 */
	cpu_base->expires_next = expires_next;
	cpu_base->in_hrtirq = 0;
	raw_spin_unlock_irqrestore(&cpu_base->lock, flags);
```

1. `hrtimer_update_base()` 读取当前单调时间，并在时间被设置过时刷新 REALTIME、BOOTTIME、TAI 时钟基的 `offset`（见下文"时钟被修改"）。
2. 把 `expires_next` 设为 `KTIME_MAX`：期间其他 CPU 迁移定时器过来时，`hrtimer_suitable_target()` 会认为本 CPU"没有更早的事件"而接受。
3. 若有软定时器到期，触发 `HRTIMER_SOFTIRQ`，并置 `softirq_activated`，避免在软中断执行前重复计算软定时器。
4. 在硬中断中运行所有到期的硬定时器。
5. 重新计算下一个最早事件（硬定时器与尚未触发的软定时器取较早者）。

[`kernel/time/hrtimer.c:1920-1967`](../../linux/kernel/time/hrtimer.c#L1920-L1967)（v6.18.60，节选）

```c
	/* Reprogramming necessary ? */
	if (!tick_program_event(expires_next, 0)) {
		cpu_base->hang_detected = 0;
		return;
	}
	...
	raw_spin_lock_irqsave(&cpu_base->lock, flags);
	now = hrtimer_update_base(cpu_base);
	cpu_base->nr_retries++;
	if (++retries < 3)
		goto retry;
	/*
	 * Give the system a chance to do something else than looping
	 * here. We stored the entry time, so we know exactly how long
	 * we spent here. We schedule the next event this amount of
	 * time away.
	 */
	cpu_base->nr_hangs++;
	cpu_base->hang_detected = 1;
	raw_spin_unlock_irqrestore(&cpu_base->lock, flags);

	delta = ktime_sub(now, entry_time);
	if ((unsigned int)delta > cpu_base->max_hang_time)
		cpu_base->max_hang_time = (unsigned int) delta;
	/*
	 * Limit it to a sensible value as we enforce a longer
	 * delay. Give the CPU at least 100ms to catch up.
	 */
	if (delta > 100 * NSEC_PER_MSEC)
		expires_next = ktime_add_ns(now, 100 * NSEC_PER_MSEC);
	else
		expires_next = ktime_add(now, delta);
	tick_program_event(expires_next, 1);
	pr_warn_once("hrtimer: interrupt took %llu ns\n", ktime_to_ns(delta));
}
```

编程失败意味着下一个定时器在处理过程中已经到期（回调太慢、被跟踪打断、虚拟机中被调度走），此时回到 `retry` 再处理一轮，最多 3 轮。仍然失败就判定为"挂起"：本次中断已经耗时 `delta`，就把下一次中断推迟同样长的时间（不超过 100 ms），并置 `hang_detected`，在此期间 `__hrtimer_reprogram()` 不再缩短这个延迟。这是一道保险：防止定时器风暴把 CPU 永远困在中断处理中，以牺牲部分定时器的精度换取系统可以前进。`/proc/timer_list` 中的 `nr_hangs`、`max_hang_time` 统计了这类事件。

### __hrtimer_run_queues()

[`kernel/time/hrtimer.c:1814-1851`](../../linux/kernel/time/hrtimer.c#L1814-L1851)（v6.18.60）

```c
static void __hrtimer_run_queues(struct hrtimer_cpu_base *cpu_base, ktime_t now,
				 unsigned long flags, unsigned int active_mask)
{
	struct hrtimer_clock_base *base;
	unsigned int active = cpu_base->active_bases & active_mask;

	for_each_active_base(base, cpu_base, active) {
		struct timerqueue_node *node;
		ktime_t basenow;

		basenow = ktime_add(now, base->offset);

		while ((node = timerqueue_getnext(&base->active))) {
			struct hrtimer *timer;

			timer = container_of(node, struct hrtimer, node);

			/*
			 * The immediate goal for using the softexpires is
			 * minimizing wakeups, not running timers at the
			 * earliest interrupt after their soft expiration.
			 * This allows us to avoid using a Priority Search
			 * Tree, which can answer a stabbing query for
			 * overlapping intervals and instead use the simple
			 * BST we already have.
			 * We don't add extra wakeups by delaying timers that
			 * are right-of a not yet expired timer, because that
			 * timer will have to trigger a wakeup anyway.
			 */
			if (basenow < hrtimer_get_softexpires_tv64(timer))
				break;

			__run_hrtimer(cpu_base, base, timer, &basenow, flags);
			if (active_mask == HRTIMER_ACTIVE_SOFT)
				hrtimer_sync_wait_running(cpu_base, flags);
		}
	}
}
```

对每个非空时钟基，把单调时间换算到本时钟（`now + offset`），从最左节点开始依次执行软到期时间已过的定时器，遇到第一个尚未软到期的就停止。注释解释了软/硬到期区间的取舍：严格地说，应找出所有区间与当前时刻相交的定时器，那需要优先搜索树；这里只用按硬到期时间排序的普通红黑树，遇到一个未软到期的节点就停止，其右侧可能有软到期已过的定时器被推迟——但不会因此增加唤醒，因为那个未到期的节点反正要触发一次中断。

### __run_hrtimer()

[`kernel/time/hrtimer.c:1739-1812`](../../linux/kernel/time/hrtimer.c#L1739-L1812)（v6.18.60，节选）

```c
static void __run_hrtimer(struct hrtimer_cpu_base *cpu_base,
			  struct hrtimer_clock_base *base,
			  struct hrtimer *timer, ktime_t *now,
			  unsigned long flags) __must_hold(&cpu_base->lock)
{
	enum hrtimer_restart (*fn)(struct hrtimer *);
	bool expires_in_hardirq;
	int restart;

	lockdep_assert_held(&cpu_base->lock);

	debug_hrtimer_deactivate(timer);
	base->running = timer;

	/*
	 * Separate the ->running assignment from the ->state assignment.
	 *
	 * As with a regular write barrier, this ensures the read side in
	 * hrtimer_active() cannot observe base->running == NULL &&
	 * timer->state == INACTIVE.
	 */
	raw_write_seqcount_barrier(&base->seq);

	__remove_hrtimer(timer, base, HRTIMER_STATE_INACTIVE, 0);
	fn = ACCESS_PRIVATE(timer, function);
	...
	raw_spin_unlock_irqrestore(&cpu_base->lock, flags);
	trace_hrtimer_expire_entry(timer, now);
	expires_in_hardirq = lockdep_hrtimer_enter(timer);

	restart = fn(timer);

	lockdep_hrtimer_exit(expires_in_hardirq);
	trace_hrtimer_expire_exit(timer);
	raw_spin_lock_irq(&cpu_base->lock);
	...
	if (restart != HRTIMER_NORESTART &&
	    !(timer->state & HRTIMER_STATE_ENQUEUED))
		enqueue_hrtimer(timer, base, HRTIMER_MODE_ABS, false);
	...
	raw_write_seqcount_barrier(&base->seq);

	WARN_ON_ONCE(base->running != timer);
	base->running = NULL;
}
```

- 先设置 `running`，再从树中摘下（状态变为 `INACTIVE`），中间用 `raw_write_seqcount_barrier()` 分隔。这样无锁的 `hrtimer_active()`（[`kernel/time/hrtimer.c:1701-1718`](../../linux/kernel/time/hrtimer.c#L1701-L1718)）在任何时刻都不会同时看到"不在树中"且"不在运行"，不会误判一个正在运行的定时器为非活动——注释称"不能有假阴性"。
- 执行回调时释放 CPU 基锁，回调可以重新启动自己或操作其他定时器。
- 回调返回 `HRTIMER_RESTART` 且定时器尚未被（其他 CPU）重新入队时，以绝对模式重新入队。此时不编程硬件，由 `hrtimer_interrupt()` 末尾统一处理。

### 软中断到期

[`kernel/time/hrtimer.c:1853-1870`](../../linux/kernel/time/hrtimer.c#L1853-L1870)（v6.18.60）

```c
static __latent_entropy void hrtimer_run_softirq(void)
{
	struct hrtimer_cpu_base *cpu_base = this_cpu_ptr(&hrtimer_bases);
	unsigned long flags;
	ktime_t now;

	hrtimer_cpu_base_lock_expiry(cpu_base);
	raw_spin_lock_irqsave(&cpu_base->lock, flags);

	now = hrtimer_update_base(cpu_base);
	__hrtimer_run_queues(cpu_base, now, flags, HRTIMER_ACTIVE_SOFT);

	cpu_base->softirq_activated = 0;
	hrtimer_update_softirq_timer(cpu_base, true);

	raw_spin_unlock_irqrestore(&cpu_base->lock, flags);
	hrtimer_cpu_base_unlock_expiry(cpu_base);
}
```

`HRTIMER_SOFTIRQ`（5.9 节表 5-17 中的第 8 号软中断）运行软时钟基中到期的定时器，结束时清除 `softirq_activated` 并按剩余软定时器的最早时间重新编程。软定时器适合回调较重、或需要与软中断上下文的代码同步的场景，例如网络协议栈中的部分 hrtimer。

## 取消

[`kernel/time/hrtimer.c:1342-1369`](../../linux/kernel/time/hrtimer.c#L1342-L1369)（v6.18.60，节选）

```c
int hrtimer_try_to_cancel(struct hrtimer *timer)
{
	struct hrtimer_clock_base *base;
	unsigned long flags;
	int ret = -1;

	/*
	 * Check lockless first. If the timer is not active (neither
	 * enqueued nor running the callback, nothing to do here.  The
	 * base lock does not serialize against a concurrent enqueue,
	 * so we can avoid taking it.
	 */
	if (!hrtimer_active(timer))
		return 0;

	base = lock_hrtimer_base(timer, &flags);

	if (!hrtimer_callback_running(timer)) {
		ret = remove_hrtimer(timer, base, false, false);
		if (ret)
			trace_hrtimer_cancel(timer);
	}

	unlock_hrtimer_base(timer, &flags);

	return ret;

}
```

返回 0 表示定时器本就不活跃，1 表示成功摘下，−1 表示回调正在执行、无法取消。`hrtimer_cancel()` 在返回 −1 时调用 `hrtimer_cancel_wait_running()` 等待后重试（[`kernel/time/hrtimer.c:1481-1492`](../../linux/kernel/time/hrtimer.c#L1481-L1492)），非 RT 内核中等待就是 `cpu_relax()`。与 `timer_delete_sync()` 一样，不能在持有回调所需的锁时调用 `hrtimer_cancel()`。

摘下的若是本 CPU 当前编程的那个定时器，`__remove_hrtimer()` 会调用 `hrtimer_force_reprogram()` 把硬件改为下一个定时器的时间（[`kernel/time/hrtimer.c:1095-1120`](../../linux/kernel/time/hrtimer.c#L1095-L1120)），避免一次空中断；远程 CPU 上的定时器则不重新编程，对方到时会收到一次空中断，注释认为这比发 IPI 更便宜。

## 周期定时器：hrtimer_forward()

[`kernel/time/hrtimer.c:1028-1060`](../../linux/kernel/time/hrtimer.c#L1028-L1060)（v6.18.60）

```c
u64 hrtimer_forward(struct hrtimer *timer, ktime_t now, ktime_t interval)
{
	u64 orun = 1;
	ktime_t delta;

	delta = ktime_sub(now, hrtimer_get_expires(timer));

	if (delta < 0)
		return 0;

	if (WARN_ON(timer->state & HRTIMER_STATE_ENQUEUED))
		return 0;

	if (interval < hrtimer_resolution)
		interval = hrtimer_resolution;

	if (unlikely(delta >= interval)) {
		s64 incr = ktime_to_ns(interval);

		orun = ktime_divns(delta, incr);
		hrtimer_add_expires_ns(timer, incr * orun);
		if (hrtimer_get_expires_tv64(timer) > now)
			return orun;
		/*
		 * This (and the ktime_add() below) is the
		 * correction for exact:
		 */
		orun++;
	}
	hrtimer_add_expires(timer, interval);

	return orun;
}
```

把到期时间以 `interval` 为步长向后推，直到超过 `now`，返回推进的步数——即"错过了几个周期"（overrun）。到期时间以原来的到期时间为基准累加，而不是以 `now` 为基准，因此周期不会因中断延迟而累积漂移。8.10 节的 tick 模拟定时器每次都 `hrtimer_forward(timer, now, TICK_NSEC)`；POSIX 定时器则把返回值累加到 `it_overrun` 中，供 `timer_getoverrun()` 查询（8.11 节）。

## 时钟被修改：clock_was_set()

REALTIME、BOOTTIME、TAI 时钟基中存放的是本时钟的绝对时间。当这些时钟相对于 MONOTONIC 的偏移改变时（设置时间、闰秒、挂起恢复），树中定时器换算成单调时间后的到期时刻也随之改变，必须重新计算硬件编程时刻。

偏移的获取发生在每次中断开始时：

[`kernel/time/hrtimer.c:620-634`](../../linux/kernel/time/hrtimer.c#L620-L634)（v6.18.60）

```c
static inline ktime_t hrtimer_update_base(struct hrtimer_cpu_base *base)
{
	ktime_t *offs_real = &base->clock_base[HRTIMER_BASE_REALTIME].offset;
	ktime_t *offs_boot = &base->clock_base[HRTIMER_BASE_BOOTTIME].offset;
	ktime_t *offs_tai = &base->clock_base[HRTIMER_BASE_TAI].offset;

	ktime_t now = ktime_get_update_offsets_now(&base->clock_was_set_seq,
					    offs_real, offs_boot, offs_tai);

	base->clock_base[HRTIMER_BASE_REALTIME_SOFT].offset = *offs_real;
	base->clock_base[HRTIMER_BASE_BOOTTIME_SOFT].offset = *offs_boot;
	base->clock_base[HRTIMER_BASE_TAI_SOFT].offset = *offs_tai;

	return now;
}
```

`ktime_get_update_offsets_now()`（8.4、8.5 节）在读当前时间的同时比较 `clock_was_set_seq`：只有 timekeeper 的序号与本 CPU 记录的不同时才复制三个偏移，平时只是一次比较；它还处理闰秒插入时刻的 REALTIME 回拨。

但如果某个 CPU 正在长时间空闲、硬件编程在一个很远的时刻，它不会有中断来刷新偏移。`clock_was_set()`（[`kernel/time/hrtimer.c:933-969`](../../linux/kernel/time/hrtimer.c#L933-L969)）负责主动通知：它遍历所有在线 CPU，在对方的 CPU 基锁内调用 `update_needs_ipi()`（[`kernel/time/hrtimer.c:857-917`](../../linux/kernel/time/hrtimer.c#L857-L917)）刷新偏移，并检查受影响的时钟基中最早的定时器是否因此早于对方当前编程的时刻；只有需要时才把对方加入 IPI 掩码，最后对这些 CPU 执行 `retrigger_next_event()`（[`kernel/time/hrtimer.c:759-787`](../../linux/kernel/time/hrtimer.c#L759-L787)）重新编程。函数末尾还调用 `timerfd_clock_was_set()` 唤醒带 `TFD_TIMER_CANCEL_ON_SET` 的 timerfd（8.11 节）。`clock_was_set()` 可能分配内存、发 IPI，所以 tick 中发现闰秒时只能用 `clock_was_set_delayed()` 交给工作队列执行（8.4 节）。

## 低精度模式与模式切换

启动时所有 CPU 的 hrtimer 都处于低精度模式：tick 设备工作在周期模式，`hrtimer_run_queues()` 在每个 tick 中被 `run_local_timers()` 调用：

[`kernel/time/hrtimer.c:1973-2005`](../../linux/kernel/time/hrtimer.c#L1973-L2005)（v6.18.60）

```c
void hrtimer_run_queues(void)
{
	struct hrtimer_cpu_base *cpu_base = this_cpu_ptr(&hrtimer_bases);
	unsigned long flags;
	ktime_t now;

	if (hrtimer_hres_active(cpu_base))
		return;

	/*
	 * This _is_ ugly: We have to check periodically, whether we
	 * can switch to highres and / or nohz mode. The clocksource
	 * switch happens with xtime_lock held. Notification from
	 * there only sets the check bit in the tick_oneshot code,
	 * otherwise we might deadlock vs. xtime_lock.
	 */
	if (tick_check_oneshot_change(!hrtimer_is_hres_enabled())) {
		hrtimer_switch_to_hres();
		return;
	}

	raw_spin_lock_irqsave(&cpu_base->lock, flags);
	now = hrtimer_update_base(cpu_base);

	if (!ktime_before(now, cpu_base->softirq_expires_next)) {
		cpu_base->softirq_expires_next = KTIME_MAX;
		cpu_base->softirq_activated = 1;
		raise_timer_softirq(HRTIMER_SOFTIRQ);
	}

	__hrtimer_run_queues(cpu_base, now, flags, HRTIMER_ACTIVE_HARD);
	raw_spin_unlock_irqrestore(&cpu_base->lock, flags);
}
```

它在每个 tick 中检查一次能否切换模式。`tick_check_oneshot_change()`（[`kernel/time/tick-sched.c:1654-1672`](../../linux/kernel/time/tick-sched.c#L1654-L1672)）只在 `check_clocks` 位被置上时（时钟源或时钟事件设备有变化，8.2、8.6 节）才真正检查：时钟源必须 `VALID_FOR_HRES`，tick 设备必须能工作在单次模式。条件满足且高精度未被禁用（`highres=off`），就调用：

[`kernel/time/hrtimer.c:723-738`](../../linux/kernel/time/hrtimer.c#L723-L738)（v6.18.60）

```c
static void hrtimer_switch_to_hres(void)
{
	struct hrtimer_cpu_base *base = this_cpu_ptr(&hrtimer_bases);

	if (tick_init_highres()) {
		pr_warn("Could not switch to high resolution mode on CPU %u\n",
			base->cpu);
		return;
	}
	base->hres_active = 1;
	hrtimer_resolution = HIGH_RES_NSEC;

	tick_setup_sched_timer(true);
	/* "Retrigger" the interrupt to get things going */
	retrigger_next_event(NULL);
}
```

三步完成切换：`tick_init_highres()` 把 tick 设备切到单次模式并把回调换成 `hrtimer_interrupt()`（8.6 节）；`hrtimer_resolution` 从 1 个 jiffy 变为 1 ns，`clock_getres()` 此后报告 1 ns；`tick_setup_sched_timer(true)` 创建模拟周期 tick 的 hrtimer（8.10 节），否则 jiffies 与 `update_process_times()` 将无人驱动。切换是每 CPU 独立进行的，各 CPU 在各自的下一个 tick 中完成。若高精度被禁用但 NO_HZ 可用，`tick_check_oneshot_change()` 则直接切换到低精度 NO_HZ 模式。

`CONFIG_TIME_LOW_RES` 只在没有高精度时钟的架构上开启，x86 上不使能，`hrtimer_update_lowres()` 为空操作。

## 基于 hrtimer 的睡眠

`nanosleep()`、`clock_nanosleep()`、`usleep_range()`（8.13 节）、`select()`/`poll()`/`epoll_wait()` 的超时都基于 hrtimer。睡眠者使用一个携带任务指针的包装结构：

[`include/linux/hrtimer.h:92-95`](../../linux/include/linux/hrtimer.h#L92-L95)（v6.18.60）

```c
struct hrtimer_sleeper {
	struct hrtimer timer;
	struct task_struct *task;
};
```

其回调 `hrtimer_wakeup()`（[`kernel/time/hrtimer.c:2010-2021`](../../linux/kernel/time/hrtimer.c#L2010-L2021)）把 `task` 置为 NULL 后唤醒任务。`task` 是否为 NULL 就成了"定时器是否已经到期"的标志：

[`kernel/time/hrtimer.c:2112-2145`](../../linux/kernel/time/hrtimer.c#L2112-L2145)（v6.18.60）

```c
static int __sched do_nanosleep(struct hrtimer_sleeper *t, enum hrtimer_mode mode)
{
	struct restart_block *restart;

	do {
		set_current_state(TASK_INTERRUPTIBLE|TASK_FREEZABLE);
		hrtimer_sleeper_start_expires(t, mode);

		if (likely(t->task))
			schedule();

		hrtimer_cancel(&t->timer);
		mode = HRTIMER_MODE_ABS;

	} while (t->task && !signal_pending(current));

	__set_current_state(TASK_RUNNING);

	if (!t->task)
		return 0;

	restart = &current->restart_block;
	if (restart->nanosleep.type != TT_NONE) {
		ktime_t rem = hrtimer_expires_remaining(&t->timer);
		struct timespec64 rmt;

		if (rem <= 0)
			return 0;
		rmt = ktime_to_timespec64(rem);

		return nanosleep_copyout(restart, &rmt);
	}
	return -ERESTART_RESTARTBLOCK;
}
```

先设置任务状态再启动定时器，避免"定时器在 `schedule()` 之前就到期"导致的丢失唤醒（3.2 节）。被唤醒后若 `task` 仍非空，说明是被信号等其他原因唤醒的：没有信号就再睡（第二轮起改为绝对模式，到期时刻不变）；有信号则把剩余时间写回用户空间的 `rmtp`，返回 `-ERESTART_RESTARTBLOCK`。若信号处理函数返回后系统调用需要重启，内核会调用 `restart_block` 中登记的 `hrtimer_nanosleep_restart()`，以原来的绝对到期时间继续睡眠——这样重启不会延长总的睡眠时间（[`kernel/time/hrtimer.c:2159-2185`](../../linux/kernel/time/hrtimer.c#L2159-L2185)）。`nanosleep(2)` 系统调用本身以 `CLOCK_MONOTONIC`、相对模式调用 `hrtimer_nanosleep()`，并把当前任务的 `timer_slack_ns` 作为松弛（[`kernel/time/hrtimer.c:2189-2205`](../../linux/kernel/time/hrtimer.c#L2189-L2205)）。

## 关键细节 / 设计考量

### 为什么用红黑树

hrtimer 需要的操作是"按精确时间插入"与"取最早者"，这是优先队列的典型场景。红黑树的插入、删除是 O(log n)，取最左节点借助 `rb_root_cached` 是 O(1)。之所以不用堆，是因为 hrtimer 经常被取消或修改，红黑树删除任意节点同样是 O(log n)，而堆需要额外的索引。hrtimer 的数量通常远少于时间轮定时器（每个 CPU 几个到几百个），O(log n) 的代价可以接受。

### 每 CPU、每时钟的设计

把定时器分散到每个 CPU，使绝大多数操作只涉及本地锁与本地缓存行；每种时钟一棵树，使同一棵树内的到期时间可以直接比较，设置墙上时间时也无需重新排序——只需修改这棵树的 `offset`。软 / 硬时钟基分开，使硬中断只需遍历硬定时器，软定时器的处理被推迟到软中断。

### 回调的执行上下文

硬定时器的回调在关中断的硬中断上下文中执行，必须简短，不能睡眠，也不能获取普通 `spinlock_t`（在 `PREEMPT_RT` 下它会睡眠）。这正是 RT 内核默认把未标记 `HARD` 的定时器移到软中断的原因（[`kernel/time/hrtimer.c:1620-1627`](../../linux/kernel/time/hrtimer.c#L1620-L1627)）；tick 模拟定时器、hrtick 等必须在硬中断中执行的定时器显式使用 `HRTIMER_MODE_*_HARD`（3.5 节）。本书配置未开启 `PREEMPT_RT`。

## 动手实验

```bash
# 查看每个 CPU 上排队的 hrtimer、各时钟基的 offset 以及统计数据
sudo grep -E '^cpu:|clock [0-9]|\.offset|nr_events|nr_retries|nr_hangs|max_hang_time|expires_next|#[0-9]' /proc/timer_list | head -60

# 统计到期最频繁的 hrtimer 回调
sudo bpftrace -e 'tracepoint:timer:hrtimer_expire_entry { @[ksym(args->function)] = count(); } interval:s:5 { exit(); }'

# 观察 timer slack 对睡眠精度的影响：每次 nanosleep(100us) 的实际时长
cat > /tmp/slack.c <<'EOF'
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <sys/prctl.h>
int main(int argc, char **argv) {
    struct timespec a, b, req = { 0, 100000 };
    if (argc > 1) prctl(PR_SET_TIMERSLACK, atol(argv[1]));
    long sum = 0;
    for (int i = 0; i < 1000; i++) {
        clock_gettime(CLOCK_MONOTONIC, &a);
        nanosleep(&req, NULL);
        clock_gettime(CLOCK_MONOTONIC, &b);
        sum += (b.tv_sec - a.tv_sec) * 1000000000L + b.tv_nsec - a.tv_nsec;
    }
    printf("avg sleep = %ld ns\n", sum / 1000);
}
EOF
gcc -O2 -o /tmp/slack /tmp/slack.c && /tmp/slack && /tmp/slack 1 && /tmp/slack 1000000
```

`/proc/timer_list` 中每个 CPU 的 `tick_sched_timer` 几乎总是排在 MONOTONIC 时钟基的第一位（8.10 节）；`clock 1`（REALTIME）的 `.offset` 即 `offs_real`。slack 实验中，默认 50 μs 松弛下平均睡眠时长约为 150 μs 左右，设为 1 ns 后接近 100 μs 加上唤醒延迟，设为 1 ms 后则接近 1.1 ms——松弛越大，内核越有机会把多个定时器合并到同一次中断。

## 小结

- hrtimer 以纳秒为单位、按每 CPU 每时钟（4 种时钟 × 硬 / 软）组织成 8 棵红黑树，取最早者为 O(1)；树中存放本时钟的绝对时间，编程硬件时减去时钟基的 `offset` 换算为单调时间。
- 每个定时器有软 / 硬两个到期时间，红黑树按硬到期时间排序，到期处理按软到期时间判断，松弛让相近的定时器合并唤醒；相对的 `CLOCK_REALTIME` 定时器被改为 `CLOCK_MONOTONIC`。
- 启动时可能迁移到非空闲 CPU，但不能成为远程 CPU 上最早到期的定时器；成为本 CPU 最早者时由 `hrtimer_reprogram()` 重新编程时钟事件设备。
- 高精度模式下 `hrtimer_interrupt()` 刷新时钟偏移、触发软定时器软中断、执行到期的硬定时器、重新编程，连续 3 次赶不上时判定挂起并强制推迟下一次中断。
- 低精度模式下 hrtimer 在 tick 中被检查，并在时钟源与 tick 设备就绪后经 `hrtimer_switch_to_hres()` 切换到高精度模式，同时用 hrtimer 模拟周期 tick。
- `clock_was_set()` 在时钟偏移改变时按需 IPI 其他 CPU 重新编程；`nanosleep()` 基于 `hrtimer_sleeper`，被信号打断后以原绝对时刻重启。

## 参考

- `linux/Documentation/timers/hrtimers.rst`（hrtimer 的设计动机与时间轮的对比）
- `linux/Documentation/timers/highres.rst`
- Thomas Gleixner, Douglas Niehaus, "Hrtimers and Beyond: Transforming the Linux Time Subsystems", OLS 2006
- LWN, "The high-resolution timer API"（2006）
- `man 2 nanosleep`、`man 2 prctl`（`PR_SET_TIMERSLACK`）
