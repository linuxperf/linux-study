# 8.4 时间维护：timekeeper 与读时接口

> **本节源码**：[`include/linux/timekeeper_internal.h`](../../linux/include/linux/timekeeper_internal.h)、[`kernel/time/timekeeping.c`](../../linux/kernel/time/timekeeping.c)、[`kernel/time/timekeeping_internal.h`](../../linux/kernel/time/timekeeping_internal.h)、[`include/linux/timekeeping.h`](../../linux/include/linux/timekeeping.h)
> **关键函数**：`ktime_get()`、`ktime_get_real_ts64()`、`ktime_get_with_offset()`、`timekeeping_cycles_to_ns()`、`update_wall_time()`、`timekeeping_advance()`、`logarithmic_accumulation()`、`timekeeping_update_from_shadow()`、`tk_setup_internals()`、`do_settimeofday64()`、`timekeeping_init()`、`timekeeping_suspend()`、`timekeeping_resume()`、`ktime_get_mono_fast_ns()`
> **前置知识**：8.2 时钟源框架、7.5 顺序锁与 latch

## 概述

时钟源只提供一个不断增长的周期计数，把它变成"现在是 2026 年某月某日某时某分某秒某纳秒"，是时间维护（Timekeeping）层的工作。这一层的核心是一个全局结构 `struct timekeeper`，它记录"上一次更新时时钟源读数是多少、那一刻对应的时间是多少"，于是任何时刻的时间都可以由"基准时间 + （当前读数 − 基准读数）× 换算系数"得出。

timekeeping 的设计围绕两个目标：

- **读要极快**：`ktime_get()` 每秒被调用数百万次，读路径不能加锁，只能用顺序锁，并且热数据要挤在一两个缓存行里。
- **写要正确**：tick 中周期性地把累计的周期"折算"进基准，同时应用 NTP 调频；设置时间、切换时钟源、挂起恢复都要保证时间不倒流、各时钟之间关系一致。

本节先分析 timekeeper 的数据结构与各个 POSIX 时钟之间的关系，再分析读路径、tick 中的更新路径、设置时间与挂起恢复，最后介绍 NMI 安全的快速读取接口。NTP 如何通过 `mult` 调频留给 8.5 节。

## 核心数据结构

### tk_read_base：读时间所需的全部信息

[`include/linux/timekeeper_internal.h:50-59`](../../linux/include/linux/timekeeper_internal.h#L50-L59)（v6.18.60）

```c
struct tk_read_base {
	struct clocksource	*clock;		// [注] 当前时钟源
	u64			mask;		// [注] 从时钟源复制，避免多访问一个缓存行
	u64			cycle_last;	// [注] 上次更新时的时钟源读数
	u32			mult;		// [注] 经 NTP 调整后的乘数（可能不等于 clock->mult）
	u32			shift;
	u64			xtime_nsec;	// [注] 上次更新时的亚秒部分，左移 shift 位保存
	ktime_t			base;		// [注] 上次更新时的单调时间（纳秒）
	u64			base_real;	// [注] base + offs_real，供 NMI 安全接口读 REALTIME
};
```

注释指出这个结构在 64 位上占 56 字节，与前置的 4 字节顺序计数正好放进一个 64 字节缓存行。读单调时间只需要这一个缓存行。

`xtime_nsec` 以"左移 `shift` 位的纳秒"为单位保存，称为 shifted nanoseconds。这样每次累加 `Δcycles × mult` 时不必右移，亚纳秒的余数也不会丢失，只有在需要输出纳秒时才右移。

### struct timekeeper

[`include/linux/timekeeper_internal.h:140-159`](../../linux/include/linux/timekeeper_internal.h#L140-L159)（v6.18.60）

```c
struct timekeeper {
	/* Cacheline 0 (together with prepended seqcount of timekeeper core): */
	struct tk_read_base	tkr_mono;		// [注] CLOCK_MONOTONIC 的读取基准

	/* Cacheline 1: */
	u64			xtime_sec;		// [注] CLOCK_REALTIME 的秒数
	unsigned long		ktime_sec;		// [注] CLOCK_MONOTONIC 的秒数
	struct timespec64	wall_to_monotonic;	// [注] REALTIME → MONOTONIC 的偏移（通常为负）
	ktime_t			offs_real;		// [注] MONOTONIC → REALTIME 的偏移，= -wall_to_monotonic
	ktime_t			offs_boot;		// [注] MONOTONIC → BOOTTIME 的偏移，= 累计挂起时间
	union {
		ktime_t		offs_tai;		// [注] MONOTONIC → TAI 的偏移
		ktime_t		offs_aux;		// [注] 辅助时钟复用此字段
	};
	u32			coarse_nsec;		// [注] COARSE 时钟的纳秒部分
	enum timekeeper_ids	id;

	/* Cacheline 2: */
	struct tk_read_base	tkr_raw;		// [注] CLOCK_MONOTONIC_RAW 的读取基准
	u64			raw_sec;
```

[`include/linux/timekeeper_internal.h:161-183`](../../linux/include/linux/timekeeper_internal.h#L161-L183)（v6.18.60）

```c
	/* Cachline 3 and 4 (timekeeping internal variables): */
	unsigned int		clock_was_set_seq;	// [注] 时间被设置的次数，hrtimer 据此刷新偏移
	u8			cs_was_changed_seq;	// [注] 时钟源切换的次数
	u8			clock_valid;

	union {
		struct timespec64	monotonic_to_boot;
		struct timespec64	monotonic_to_aux;
	};

	u64			cycle_interval;		// [注] 一个 NTP 间隔（一个 tick）对应的周期数
	u64			xtime_interval;		// [注] 一个间隔对应的 shifted ns（随 NTP 调频变化）
	s64			xtime_remainder;	// [注] 周期数取整造成的误差
	u64			raw_interval;		// [注] 一个间隔对应的原始 shifted ns

	ktime_t			next_leap_ktime;	// [注] 下一个闰秒的单调时间
	u64			ntp_tick;
	s64			ntp_error;		// [注] 实际累计时间与 NTP 期望时间的误差（8.5 节）
	u32			ntp_error_shift;
	u32			ntp_err_mult;
	u32			skip_second_overflow;
	s32			tai_offset;		// [注] TAI − UTC 的秒数
};
```

字段排列按缓存行精心设计（[`include/linux/timekeeper_internal.h:124-138`](../../linux/include/linux/timekeeper_internal.h#L124-L138)）：第 0 行是顺序计数与 `tkr_mono`，第 1 行是 REALTIME/BOOTTIME/TAI 需要的秒数与偏移，第 2 行是 RAW 时钟，第 3、4 行是只在 tick 更新时访问的内部变量。读 MONOTONIC 只碰第 0 行，读 REALTIME 再加第 1 行。

### tk_data：真实副本、影子副本与锁

[`kernel/time/timekeeping.c:48-62`](../../linux/kernel/time/timekeeping.c#L48-L62)（v6.18.60）

```c
/*
 * The most important data for readout fits into a single 64 byte
 * cache line.
 */
struct tk_data {
	seqcount_raw_spinlock_t	seq;			// [注] 读者用的顺序计数
	struct timekeeper	timekeeper;		// [注] 读者读取的副本
	struct timekeeper	shadow_timekeeper;	// [注] 写者修改的影子副本
	raw_spinlock_t		lock;			// [注] 写者之间互斥
} ____cacheline_aligned;

static struct tk_data timekeeper_data[TIMEKEEPERS_MAX];

/* The core timekeeper */
#define tk_core		(timekeeper_data[TIMEKEEPER_CORE])
```

系统时间由 `tk_core` 维护；开启 `CONFIG_POSIX_AUX_CLOCKS` 后，数组中还有 8 个辅助时钟的 timekeeper（8.1 节）。

每个 `tk_data` 中有**两份** timekeeper。所有修改都先在 `shadow_timekeeper` 上进行，可能耗时较长（例如循环累加周期、计算 NTP 调整）；修改完成后，才在顺序计数的写区间内用一次 `memcpy()` 把影子复制到 `timekeeper`。读者只读 `timekeeper`，因此顺序计数处于"写中"状态的时间只有一次结构体拷贝那么长，读者重试的概率大大降低。源码注释也解释了为什么不用指针切换代替拷贝：那样会多一次间接访问，并破坏为读者优化的缓存行布局（[`kernel/time/timekeeping.c:745-753`](../../linux/kernel/time/timekeeping.c#L745-L753)）。

## 各时钟之间的关系

timekeeper 中只有两条"时间线"真正由时钟源推进：`tkr_mono`（受 NTP 调频）与 `tkr_raw`（不受调频）。其余时钟都是在单调时间上加偏移：

```
                              Δ = (cycles_now − cycle_last) & mask
 CLOCK_MONOTONIC     = tkr_mono.base + ((Δ × tkr_mono.mult + tkr_mono.xtime_nsec) >> shift)
 CLOCK_REALTIME      = CLOCK_MONOTONIC + offs_real          （offs_real = −wall_to_monotonic）
 CLOCK_BOOTTIME      = CLOCK_MONOTONIC + offs_boot          （offs_boot = 累计挂起时长）
 CLOCK_TAI           = CLOCK_MONOTONIC + offs_tai           （offs_tai = offs_real + tai_offset 秒）
 CLOCK_MONOTONIC_RAW = tkr_raw.base  + ((Δ × tkr_raw.mult  + tkr_raw.xtime_nsec)  >> shift)
 *_COARSE            = 上次更新时的秒数 + coarse_nsec（不读时钟源）
```

*图 8-5：各 POSIX 时钟与 timekeeper 字段的关系*

`tkr_mono.base` 由 REALTIME 的秒数与 `wall_to_monotonic` 计算得出：

[`kernel/time/timekeeping.c:669-697`](../../linux/kernel/time/timekeeping.c#L669-L697)（v6.18.60）

```c
static inline void tk_update_ktime_data(struct timekeeper *tk)
{
	u64 seconds;
	u32 nsec;

	/*
	 * The xtime based monotonic readout is:
	 *	nsec = (xtime_sec + wtm_sec) * 1e9 + wtm_nsec + now();
	 * The ktime based monotonic readout is:
	 *	nsec = base_mono + now();
	 * ==> base_mono = (xtime_sec + wtm_sec) * 1e9 + wtm_nsec
	 */
	seconds = (u64)(tk->xtime_sec + tk->wall_to_monotonic.tv_sec);
	nsec = (u32) tk->wall_to_monotonic.tv_nsec;
	tk->tkr_mono.base = ns_to_ktime(seconds * NSEC_PER_SEC + nsec);

	/*
	 * The sum of the nanoseconds portions of xtime and
	 * wall_to_monotonic can be greater/equal one second. Take
	 * this into account before updating tk->ktime_sec.
	 */
	nsec += (u32)(tk->tkr_mono.xtime_nsec >> tk->tkr_mono.shift);
	if (nsec >= NSEC_PER_SEC)
		seconds++;
	tk->ktime_sec = seconds;

	/* Update the monotonic raw base */
	tk->tkr_raw.base = ns_to_ktime(tk->raw_sec * NSEC_PER_SEC);
}
```

注意 `base` 只包含整秒部分与 `wall_to_monotonic` 的纳秒部分，REALTIME 的亚秒部分仍留在 `xtime_nsec` 中，读时间时与 `Δ × mult` 一起右移。这个设计让 `xtime_nsec` 成为"REALTIME 与 MONOTONIC 共享的亚秒累加器"，两者天然保持一致。

由此可以理解各种时间操作的效果：

- **设置时间**（`settimeofday()`）：修改 `xtime_sec`/`xtime_nsec`，并反向修改 `wall_to_monotonic`，使 MONOTONIC 不变，只有 `offs_real` 改变。
- **NTP 调频**：修改 `tkr_mono.mult`，MONOTONIC 与 REALTIME 的流速一起变化，RAW 不受影响。
- **闰秒**：`xtime_sec` 加减 1 秒，`wall_to_monotonic` 反向调整，`tai_offset` 同步调整，TAI 与 MONOTONIC 不变（8.5 节）。
- **挂起恢复**：`xtime` 与 `offs_boot` 加上挂起时长，`wall_to_monotonic` 减去同样的值，于是 REALTIME 与 BOOTTIME 跳过挂起期间，MONOTONIC 不变——这正是"MONOTONIC 不包含挂起时间"的实现方式。

## 读路径

### 周期到纳秒

[`kernel/time/timekeeping.c:378-405`](../../linux/kernel/time/timekeeping.c#L378-L405)（v6.18.60）

```c
static inline u64 timekeeping_cycles_to_ns(const struct tk_read_base *tkr, u64 cycles)
{
	/* Calculate the delta since the last update_wall_time() */
	u64 mask = tkr->mask, delta = (cycles - tkr->cycle_last) & mask;

	/*
	 * This detects both negative motion and the case where the delta
	 * overflows the multiplication with tkr->mult.
	 */
	if (unlikely(delta > tkr->clock->max_cycles)) {
		/*
		 * Handle clocksource inconsistency between CPUs to prevent
		 * time from going backwards by checking for the MSB of the
		 * mask being set in the delta.
		 */
		if (delta & ~(mask >> 1))
			return tkr->xtime_nsec >> tkr->shift;

		return delta_to_ns_safe(tkr, delta);
	}

	return ((delta * tkr->mult) + tkr->xtime_nsec) >> tkr->shift;
}

static __always_inline u64 timekeeping_get_ns(const struct tk_read_base *tkr)
{
	return timekeeping_cycles_to_ns(tkr, tk_clock_read(tkr));
}
```

正常情况只是一次减法、一次乘法、一次加法、一次移位。异常分支处理两种情况，靠同一个比较 `delta > max_cycles` 筛出：

- **负向运动**：当前 CPU 读到的 TSC 略小于另一个 CPU 写入的 `cycle_last`，差值的最高位被置位。此时视差值为 0，返回基准时间，保证时间不倒流。
- **差值过大**：tick 长时间没有更新 timekeeper（例如在虚拟机中长时间停顿），`delta × mult` 会溢出 64 位，此时改用 `mul_u64_u32_add_u64_shr()` 做 128 位运算（[`kernel/time/timekeeping.c:373-376`](../../linux/kernel/time/timekeeping.c#L373-L376)）。

`tk_clock_read()`（[`kernel/time/timekeeping.c:291-296`](../../linux/kernel/time/timekeeping.c#L291-L296)）用 `READ_ONCE()` 取一次时钟源指针，再用这个指针调用它自己的 `read()`。顺序锁只保证读到的数据最终一致，不保证读的过程中不崩溃：若时钟源恰好在读取期间被切换，分两次读取 `tkr->clock` 可能把旧时钟源的 `read` 函数与新时钟源的结构体搭配调用。

### ktime_get() 与 ktime_get_real_ts64()

[`kernel/time/timekeeping.c:814-831`](../../linux/kernel/time/timekeeping.c#L814-L831)（v6.18.60）

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
```

[`kernel/time/timekeeping.c:793-811`](../../linux/kernel/time/timekeeping.c#L793-L811)（v6.18.60）

```c
void ktime_get_real_ts64(struct timespec64 *ts)
{
	struct timekeeper *tk = &tk_core.timekeeper;
	unsigned int seq;
	u64 nsecs;

	WARN_ON(timekeeping_suspended);

	do {
		seq = read_seqcount_begin(&tk_core.seq);

		ts->tv_sec = tk->xtime_sec;
		nsecs = timekeeping_get_ns(&tk->tkr_mono);

	} while (read_seqcount_retry(&tk_core.seq, seq));

	ts->tv_nsec = 0;
	timespec64_add_ns(ts, nsecs);
}
```

两者结构完全相同：在顺序锁读区间内读基准与时钟源，循环直到没有并发写者。`ktime_get_real_ts64()` 直接以 `xtime_sec` 为整秒、以 `timekeeping_get_ns()`（含 `xtime_nsec`）为纳秒，`timespec64_add_ns()` 负责把可能超过一秒的纳秒进位。挂起期间读时间会触发 `WARN_ON`：此时时钟源可能已经停止或不可访问。

### 带偏移的读取与其他接口

REALTIME、BOOTTIME、TAI 的 `ktime_t` 接口共用 `ktime_get_with_offset()`：

[`kernel/time/timekeeping.c:851-875`](../../linux/kernel/time/timekeeping.c#L851-L875)（v6.18.60）

```c
static ktime_t *offsets[TK_OFFS_MAX] = {
	[TK_OFFS_REAL]	= &tk_core.timekeeper.offs_real,
	[TK_OFFS_BOOT]	= &tk_core.timekeeper.offs_boot,
	[TK_OFFS_TAI]	= &tk_core.timekeeper.offs_tai,
};

ktime_t ktime_get_with_offset(enum tk_offsets offs)
{
	struct timekeeper *tk = &tk_core.timekeeper;
	unsigned int seq;
	ktime_t base, *offset = offsets[offs];
	u64 nsecs;

	WARN_ON(timekeeping_suspended);

	do {
		seq = read_seqcount_begin(&tk_core.seq);
		base = ktime_add(tk->tkr_mono.base, *offset);
		nsecs = timekeeping_get_ns(&tk->tkr_mono);

	} while (read_seqcount_retry(&tk_core.seq, seq));

	return ktime_add_ns(base, nsecs);

}
```

`ktime_get_real()`、`ktime_get_boottime()`、`ktime_get_clocktai()` 都是它的内联包装（[`include/linux/timekeeping.h:86-122`](../../linux/include/linux/timekeeping.h#L86-L122)）。内核中常用的读时接口归纳如下：

| 接口 | 时钟 | 读时钟源 | 说明 |
|---|---|---|---|
| `ktime_get()` / `ktime_get_ns()` | MONOTONIC | 是 | 最常用 |
| `ktime_get_real()` / `ktime_get_real_ts64()` | REALTIME | 是 | 墙上时间 |
| `ktime_get_boottime()` | BOOTTIME | 是 | |
| `ktime_get_clocktai()` | TAI | 是 | |
| `ktime_get_raw()` / `ktime_get_raw_ts64()` | MONOTONIC_RAW | 是 | 读 `tkr_raw` |
| `ktime_get_coarse_ts64()` / `ktime_get_coarse_real_ts64()` | *_COARSE | 否 | 精度为一个 tick，极快 |
| `ktime_get_seconds()` / `ktime_get_real_seconds()` | 秒 | 否 | 无锁读一个字段 |
| `ktime_get_mono_fast_ns()` 等 | MONOTONIC 等 | 是 | NMI 安全，见下文 |
| `ktime_mono_to_any()` | — | 否 | 把已有的单调时间换算成其他时钟 |

*表 8-10：常用的内核读时接口*

COARSE 接口用的是 `coarse_nsec` 而不是 `xtime_nsec`：NTP 调频时 `xtime_nsec` 可能被减去一个小量（8.5 节），直接用它会使 COARSE 时钟轻微倒退；`coarse_nsec` 只在设置时间或确实累加了时间后才更新（[`kernel/time/timekeeping.c:220-232`](../../linux/kernel/time/timekeeping.c#L220-L232)）。

## 更新路径

### update_wall_time()

tick 中负责计时的 CPU（8.10 节）在推进 jiffies 后调用 `update_wall_time()`：

[`kernel/time/timekeeping.c:2389-2405`](../../linux/kernel/time/timekeeping.c#L2389-L2405)（v6.18.60）

```c
static bool timekeeping_advance(enum timekeeping_adv_mode mode)
{
	guard(raw_spinlock_irqsave)(&tk_core.lock);
	return __timekeeping_advance(&tk_core, mode);
}

/**
 * update_wall_time - Uses the current clocksource to increment the wall time
 *
 * It also updates the enabled auxiliary clock timekeepers
 */
void update_wall_time(void)
{
	if (timekeeping_advance(TK_ADV_TICK))
		clock_was_set_delayed();
	tk_aux_advance();
}
```

### __timekeeping_advance()

[`kernel/time/timekeeping.c:2328-2387`](../../linux/kernel/time/timekeeping.c#L2328-L2387)（v6.18.60，节选）

```c
static bool __timekeeping_advance(struct tk_data *tkd, enum timekeeping_adv_mode mode)
{
	struct timekeeper *tk = &tkd->shadow_timekeeper;
	struct timekeeper *real_tk = &tkd->timekeeper;
	unsigned int clock_set = 0;
	int shift = 0, maxshift;
	u64 offset, orig_offset;

	/* Make sure we're fully resumed: */
	if (unlikely(timekeeping_suspended))
		return false;

	offset = clocksource_delta(tk_clock_read(&tk->tkr_mono),
				   tk->tkr_mono.cycle_last, tk->tkr_mono.mask,
				   tk->tkr_mono.clock->max_raw_delta);
	orig_offset = offset;
	/* Check if there's really nothing to do */
	if (offset < real_tk->cycle_interval && mode == TK_ADV_TICK)
		return false;
	...
	shift = ilog2(offset) - ilog2(tk->cycle_interval);
	shift = max(0, shift);
	/* Bound shift to one less than what overflows tick_length */
	maxshift = (64 - (ilog2(ntp_tick_length(tk->id)) + 1)) - 1;
	shift = min(shift, maxshift);
	while (offset >= tk->cycle_interval) {
		offset = logarithmic_accumulation(tk, offset, shift, &clock_set);
		if (offset < tk->cycle_interval<<shift)
			shift--;
	}

	/* Adjust the multiplier to correct NTP error */
	timekeeping_adjust(tk, offset);

	/*
	 * Finally, make sure that after the rounding
	 * xtime_nsec isn't larger than NSEC_PER_SEC
	 */
	clock_set |= accumulate_nsecs_to_secs(tk);
	...
	if (orig_offset != offset)
		tk_update_coarse_nsecs(tk);

	timekeeping_update_from_shadow(tkd, clock_set);

	return !!clock_set;
}
```

更新以 `cycle_interval` 为单位进行。`cycle_interval` 是一个 NTP 间隔（`NTP_INTERVAL_LENGTH`，即一个 tick，1 ms）对应的周期数，在 `tk_setup_internals()` 中按时钟源频率算出（[`kernel/time/timekeeping.c:309-370`](../../linux/kernel/time/timekeeping.c#L309-L370)）。函数的步骤：

1. 读时钟源，算出自上次更新以来的周期差 `offset`；不足一个间隔就什么都不做——这意味着 timekeeper 的基准总是落在整数个间隔的边界上。
2. 把 `offset` 中整数个间隔累加进影子 timekeeper，剩余不足一个间隔的周期留到下次。
3. `timekeeping_adjust()` 根据 NTP 误差微调 `mult`（8.5 节）。
4. 把 `xtime_nsec` 中满一秒的部分进位到 `xtime_sec`，并在每次跨秒时调用 NTP 的 `second_overflow()` 处理闰秒等（8.5 节）。
5. 把影子 timekeeper 发布给读者。

### 对数累加

NO_HZ 下 tick 可能停止很久，一次 `update_wall_time()` 可能要累加成千上万个间隔。逐个累加太慢，`logarithmic_accumulation()` 每次累加 `2^shift` 个间隔：

[`kernel/time/timekeeping.c:2290-2322`](../../linux/kernel/time/timekeeping.c#L2290-L2322)（v6.18.60）

```c
static u64 logarithmic_accumulation(struct timekeeper *tk, u64 offset,
				    u32 shift, unsigned int *clock_set)
{
	u64 interval = tk->cycle_interval << shift;
	u64 snsec_per_sec;

	/* If the offset is smaller than a shifted interval, do nothing */
	if (offset < interval)
		return offset;

	/* Accumulate one shifted interval */
	offset -= interval;
	tk->tkr_mono.cycle_last += interval;
	tk->tkr_raw.cycle_last  += interval;

	tk->tkr_mono.xtime_nsec += tk->xtime_interval << shift;
	*clock_set |= accumulate_nsecs_to_secs(tk);

	/* Accumulate raw time */
	tk->tkr_raw.xtime_nsec += tk->raw_interval << shift;
	snsec_per_sec = (u64)NSEC_PER_SEC << tk->tkr_raw.shift;
	while (tk->tkr_raw.xtime_nsec >= snsec_per_sec) {
		tk->tkr_raw.xtime_nsec -= snsec_per_sec;
		tk->raw_sec++;
	}

	/* Accumulate error between NTP and clock interval */
	tk->ntp_error += tk->ntp_tick << shift;
	tk->ntp_error -= (tk->xtime_interval + tk->xtime_remainder) <<
						(tk->ntp_error_shift + shift);

	return offset;
}
```

`shift` 初值取 `log2(offset) − log2(cycle_interval)`，即"`offset` 大约是间隔的 2 的几次方倍"，然后从大到小依次累加：能减就减，减不了就把 `shift` 减 1。整个过程是 O(log n) 次循环，类似二进制分解。`maxshift` 限制 `ntp_tick << shift` 不溢出 64 位。

每累加一块，`cycle_last` 前进 `interval` 个周期，`xtime_nsec` 前进 `xtime_interval << shift` 个 shifted ns。由于 `xtime_interval = cycle_interval × mult`，这等价于用当前的 `mult` 把这些周期换算成时间。之所以按块累加而不是直接 `xtime_nsec += offset × mult`，一是要在每次跨秒时调用 `second_overflow()`，二是要以间隔为单位统计 `ntp_error`。

### 发布：timekeeping_update_from_shadow()

[`kernel/time/timekeeping.c:708-755`](../../linux/kernel/time/timekeeping.c#L708-L755)（v6.18.60，节选）

```c
static void timekeeping_update_from_shadow(struct tk_data *tkd, unsigned int action)
{
	struct timekeeper *tk = &tkd->shadow_timekeeper;

	lockdep_assert_held(&tkd->lock);

	/*
	 * Block out readers before running the updates below because that
	 * updates VDSO and other time related infrastructure. Not blocking
	 * the readers might let a reader see time going backwards when
	 * reading from the VDSO after the VDSO update and then reading in
	 * the kernel from the timekeeper before that got updated.
	 */
	write_seqcount_begin(&tkd->seq);

	if (action & TK_CLEAR_NTP) {
		tk->ntp_error = 0;
		ntp_clear(tk->id);
	}

	tk_update_leap_state(tk);
	tk_update_ktime_data(tk);
	tk->tkr_mono.base_real = tk->tkr_mono.base + tk->offs_real;

	if (tk->id == TIMEKEEPER_CORE) {
		update_vsyscall(tk);
		update_pvclock_gtod(tk, action & TK_CLOCK_WAS_SET);

		update_fast_timekeeper(&tk->tkr_mono, &tk_fast_mono);
		update_fast_timekeeper(&tk->tkr_raw,  &tk_fast_raw);
	} else if (tk_is_aux(tk)) {
		vdso_time_update_aux(tk);
	}

	if (action & TK_CLOCK_WAS_SET)
		tk->clock_was_set_seq++;
	...
	memcpy(&tkd->timekeeper, tk, sizeof(*tk));
	write_seqcount_end(&tkd->seq);
}
```

这是所有修改的唯一出口，它在顺序计数写区间内依次：

- 重新计算派生量：闰秒时刻、`tkr_mono.base`、`ktime_sec`、`tkr_raw.base`、`base_real`。
- 更新 vDSO 数据页（`update_vsyscall()`，8.12 节）。注释特别说明为什么要先阻塞读者再更新 vDSO：否则用户态可能先从 vDSO 读到新时间，再经系统调用从内核读到旧时间，看到时间倒流。
- 通知 pvclock 链（KVM 宿主用它把时间同步给客户机）。
- 更新两个 NMI 安全的快速 timekeeper（见下文）。
- 若时间被设置过，递增 `clock_was_set_seq`，hrtimer 据此判断是否需要刷新 REALTIME 等时钟的偏移（8.9 节）。
- 最后把影子复制给读者副本。

`update_wall_time()` 的返回值 `clock_set` 只在闰秒插入或删除时为真，此时调用 `clock_was_set_delayed()`，经工作队列通知所有 CPU 的 hrtimer 重新计算 REALTIME 定时器的到期时间（8.9 节）。

## 设置时间

[`kernel/time/timekeeping.c:1434-1465`](../../linux/kernel/time/timekeeping.c#L1434-L1465)（v6.18.60）

```c
int do_settimeofday64(const struct timespec64 *ts)
{
	struct timespec64 ts_delta, xt;

	if (!timespec64_valid_settod(ts))
		return -EINVAL;

	scoped_guard (raw_spinlock_irqsave, &tk_core.lock) {
		struct timekeeper *tks = &tk_core.shadow_timekeeper;

		timekeeping_forward_now(tks);

		xt = tk_xtime(tks);
		ts_delta = timespec64_sub(*ts, xt);

		if (timespec64_compare(&tks->wall_to_monotonic, &ts_delta) > 0) {
			timekeeping_restore_shadow(&tk_core);
			return -EINVAL;
		}

		tk_set_wall_to_mono(tks, timespec64_sub(tks->wall_to_monotonic, ts_delta));
		tk_set_xtime(tks, ts);
		timekeeping_update_from_shadow(&tk_core, TK_UPDATE_ALL);
	}

	/* Signal hrtimers about time change */
	clock_was_set(CLOCK_SET_WALL);

	audit_tk_injoffset(ts_delta);
	add_device_randomness(ts, sizeof(*ts));
	return 0;
}
```

步骤如下：

1. `timekeeping_forward_now()`（[`kernel/time/timekeeping.c:765-785`](../../linux/kernel/time/timekeeping.c#L765-L785)）先把自上次更新以来的所有周期（不只是整数个间隔）累加进影子 timekeeper，使 `cycle_last` 等于此刻的读数——这样后续修改不会与"尚未累加的那一小段时间"混在一起。
2. 计算新旧 REALTIME 之差 `ts_delta`，并检查它不会让 `wall_to_monotonic` 变为正值。`wall_to_monotonic` 为正意味着单调时间比墙上时间还大，即把时间设置到了启动之前。检查失败时用 `timekeeping_restore_shadow()` 撤销对影子的修改。
3. 设置新的 `xtime`，反向调整 `wall_to_monotonic`，MONOTONIC 保持不变。
4. 以 `TK_UPDATE_ALL` 发布：清除 NTP 状态（时间被手动设置，之前的同步结果失效），递增 `clock_was_set_seq`。
5. `clock_was_set()` 通知 hrtimer 与 timerfd（8.9、8.11 节）：基于 REALTIME 的绝对定时器需要重新计算到期时刻，`TFD_TIMER_CANCEL_ON_SET` 的 timerfd 需要被唤醒。

`adjtimex(ADJ_SETOFFSET)` 走的 `timekeeping_inject_offset()`（[`kernel/time/timekeeping.c:1480-1533`](../../linux/kernel/time/timekeeping.c#L1480-L1533)）与此类似，只是给出的是偏移量而非绝对时间。

## 初始化

[`kernel/time/timekeeping.c:1801-1844`](../../linux/kernel/time/timekeeping.c#L1801-L1844)（v6.18.60，节选）

```c
void __init timekeeping_init(void)
{
	struct timespec64 wall_time, boot_offset, wall_to_mono;
	struct timekeeper *tks = &tk_core.shadow_timekeeper;
	struct clocksource *clock;

	tkd_basic_setup(&tk_core, TIMEKEEPER_CORE, true);
	tk_aux_setup();

	read_persistent_wall_and_boot_offset(&wall_time, &boot_offset);
	...
	/*
	 * We want set wall_to_mono, so the following is true:
	 * wall time + wall_to_mono = boot time
	 */
	wall_to_mono = timespec64_sub(boot_offset, wall_time);

	clock = clocksource_default_clock();
	if (clock->enable)
		clock->enable(clock);

	guard(raw_spinlock_irqsave)(&tk_core.lock);

	ntp_init();

	tk_setup_internals(tks, clock);

	tk_set_xtime(tks, &wall_time);
	tks->raw_sec = 0;

	tk_set_wall_to_mono(tks, wall_to_mono);

	timekeeping_update_from_shadow(&tk_core, TK_CLOCK_WAS_SET);
}
```

- **墙上时间**来自"持久时钟"（persistent clock）：x86 上 `read_persistent_clock64()` 调用 `x86_platform.get_wallclock()`，默认读 CMOS RTC（`mach_get_cmos_time()`，[`arch/x86/kernel/rtc.c:57-78`](../../linux/arch/x86/kernel/rtc.c#L57-L78)），KVM 客户机中读宿主提供的 wall clock（8.3 节）。CMOS RTC 只有秒级精度。
- **启动偏移** `boot_offset` 默认取 `local_clock()`，即 TSC 版 `sched_clock()` 自上电以来的纳秒数（[`kernel/time/timekeeping.c:1764-1770`](../../linux/kernel/time/timekeeping.c#L1764-L1770)）。令 `wall_to_mono = boot_offset − wall_time`，于是此刻 MONOTONIC 等于 `boot_offset`，而不是 0——启动早期已经过去的时间被计入了单调时间，与 printk 时间戳对齐。
- **时钟源**是 `jiffies`（8.2 节），之后由 clocksource 框架切换。

## 挂起与恢复

`timekeeping_suspend()`（[`kernel/time/timekeeping.c:2003-2068`](../../linux/kernel/time/timekeeping.c#L2003-L2068)）作为 syscore 回调在系统挂起的最后阶段运行：它先读一次持久时钟记下挂起时刻，用 `timekeeping_forward_now()` 把时间推进到此刻，置位 `timekeeping_suspended`，并让快速 timekeeper 停在当前值（`halt_fast_timekeeper()`），因为挂起后时钟源可能不可访问。

`timekeeping_resume()`（[`kernel/time/timekeeping.c:1938-1996`](../../linux/kernel/time/timekeeping.c#L1938-L1996)）需要知道挂起了多久。源码注释列出了三种来源及其优先级：

1. **挂起期间不停的时钟源**（`SUSPEND_NONSTOP`，如支持 `nonstop_tsc_s3` 的 TSC）：直接用周期差计算，精度最高。
2. **持久时钟**：用恢复时与挂起时读数之差，精度为秒级。
3. **RTC 子系统**：前两者都不可用时，由 `rtc_resume()` 调用 `timekeeping_inject_sleeptime64()` 注入。

得到挂起时长后，`__timekeeping_inject_sleeptime()`（[`kernel/time/timekeeping.c:1857-1870`](../../linux/kernel/time/timekeeping.c#L1857-L1870)）把它加到 `xtime` 与 `offs_boot` 上并从 `wall_to_monotonic` 中减去。持久时钟只有秒级精度，反复挂起恢复会累积误差，`timekeeping_suspend()` 中用 `old_delta` 记录系统时间与持久时钟之差，尽量让这个差值保持恒定（[`kernel/time/timekeeping.c:2037-2057`](../../linux/kernel/time/timekeeping.c#L2037-L2057)）。

## NMI 安全的快速访问

`ktime_get()` 的顺序锁读区间在 NMI 中不安全：如果 NMI 恰好打断了本 CPU 上处于写区间内的 `timekeeping_update_from_shadow()`，NMI 中的读者会看到奇数序号并永远自旋。perf、ftrace、BPF 等需要在任意上下文打时间戳，为此 timekeeping 额外维护两个 latch 结构：

[`kernel/time/timekeeping.c:104-107`](../../linux/kernel/time/timekeeping.c#L104-L107)（v6.18.60）

```c
struct tk_fast {
	seqcount_latch_t	seq;
	struct tk_read_base	base[2];
};
```

`update_fast_timekeeper()`（[`kernel/time/timekeeping.c:422-440`](../../linux/kernel/time/timekeeping.c#L422-L440)）每次发布时把 `tkr_mono` 与 `tkr_raw` 分别复制到 `tk_fast_mono`、`tk_fast_raw` 的两个副本中，读者 `__ktime_get_fast_ns()`（[`kernel/time/timekeeping.c:442-456`](../../linux/kernel/time/timekeeping.c#L442-L456)）按序号最低位选择副本，永不等待。latch 的原理已在 7.5 节详细分析。

代价是快速接口不保证跨更新单调：写者正在更新副本 0 时，读者被引导到尚未更新的副本 1，仍用旧的 `mult` 计算，可能比稍早一个用新 `mult` 的读数略大或略小。注释中的 ASCII 图描述了这种情况（[`kernel/time/timekeeping.c:458-489`](../../linux/kernel/time/timekeeping.c#L458-L489)）。`ktime_get_raw_fast_ns()` 不受 NTP 调频影响，因此总是单调。

| 接口 | 时钟 | 单调 | 典型用户 |
|---|---|---|---|
| `ktime_get_mono_fast_ns()` | MONOTONIC | 跨更新可能有纳秒级误差 | 跟踪、perf |
| `ktime_get_raw_fast_ns()` | MONOTONIC_RAW | 是 | |
| `ktime_get_boot_fast_ns()` | BOOTTIME | 同上 | BPF `bpf_ktime_get_boot_ns()` |
| `ktime_get_tai_fast_ns()` | TAI | 同上 | |
| `ktime_get_real_fast_ns()` | REALTIME | 同上 | pstore 记录的时间戳 |

*表 8-11：NMI 安全的快速读时接口*

`ktime_get_real_fast_ns()` 读的是 `tk_read_base` 中的 `base_real` 字段（[`kernel/time/timekeeping.c:562-577`](../../linux/kernel/time/timekeeping.c#L562-L577)），这也是该字段存在的原因。

在 timekeeping 尚未初始化的早期，快速 timekeeper 指向一个返回 `local_clock()` 的 `dummy_clock`，`mult = 1`、`shift = 0`（[`kernel/time/timekeeping.c:112-148`](../../linux/kernel/time/timekeeping.c#L112-L148)），保证启动最早期的跟踪时间戳也有意义。

## 关键细节 / 设计考量

### 为什么以 tick 为间隔累加

timekeeper 完全可以每次直接把"当前读数 − `cycle_last`"一次性换算进去，为什么要拆成整数个 `cycle_interval`？原因在于 NTP。NTP 的调整量以"每个 tick 应该前进多少纳秒"（`ntp_tick_length()`）表达，误差 `ntp_error` 也以间隔为单位累计和校正；按固定间隔累加使"实际前进量"与"NTP 期望前进量"可以逐间隔比较（8.5 节）。另一个好处是 `cycle_last` 总是对齐到间隔边界，余下不足一个间隔的周期留在下一次，不会因为 tick 抖动而引入误差。

### 多粒度时间戳

文件系统的 `ctime`/`mtime` 传统上使用 COARSE 时间，同一个 tick 内的多次修改得到相同时间戳，NFS 等依赖时间戳判断文件是否变化的场景因此出问题；但每次都用精确时间又会让 inode 频繁标脏。v6.13 引入的多粒度时间戳（Multigrain Timestamp）折中处理：维护一个全局"已发出的最大精确时间戳" `mg_floor`（[`kernel/time/timekeeping.c:173-188`](../../linux/kernel/time/timekeeping.c#L173-L188)），文件系统平时取 COARSE 时间与 `mg_floor` 的较大者（`ktime_get_coarse_real_ts64_mg()`），只有在有人观察过时间戳、需要区分时才取精确时间并尝试推高 `mg_floor`（`ktime_get_real_ts64_mg()`，[`kernel/time/timekeeping.c:2487-2526`](../../linux/kernel/time/timekeeping.c#L2487-L2526)）。

### 辅助时钟

v6.17 的辅助时钟为每个 `CLOCK_AUX` 维护独立的 `tk_data`，复用同一个时钟源，但有自己的 `mult` 调整、NTP 状态与偏移 `offs_aux`。它们在 `update_wall_time()` 末尾的 `tk_aux_advance()` 中随核心 timekeeper 一起推进（[`kernel/time/timekeeping.c:2851-2864`](../../linux/kernel/time/timekeeping.c#L2851-L2864)），通过 `/sys/kernel/time/aux_clocks/<n>/aux_clock_enable` 启用（属性名由 `__ATTR_RW(aux_clock_enable)` 决定，[`kernel/time/timekeeping.c:3074`](../../linux/kernel/time/timekeeping.c#L3074)；ABI 文档 `Documentation/ABI/stable/sysfs-kernel-time-aux-clocks` 中写作 `enable`，应以代码为准），可用 `clock_adjtime()` 独立调频。典型用途是让 PTP 守护进程把某个辅助时钟与外部时间域同步，而不影响系统时间。

## 动手实验

```bash
# 统计 update_wall_time 的调用频率与耗时（需 bpftrace）
sudo bpftrace -e '
kprobe:update_wall_time { @start[cpu] = nsecs; @cpu[cpu] = count(); }
kretprobe:update_wall_time /@start[cpu]/ { @ns = hist(nsecs - @start[cpu]); delete(@start[cpu]); }
interval:s:5 { exit(); }'

# 观察 NO_HZ 下一次累加的间隔数：offset 越大，说明 tick 停得越久
sudo bpftrace -e 'kprobe:logarithmic_accumulation { @shift = lhist(arg2, 0, 16, 1); } interval:s:10 { exit(); }'

# 比较 REALTIME 与 MONOTONIC 之差（即 offs_real），以及 BOOTTIME 与 MONOTONIC 之差（累计挂起时间）
python3 -c 'import time; m=time.clock_gettime(time.CLOCK_MONOTONIC); print(time.clock_gettime(time.CLOCK_REALTIME)-m, time.clock_gettime(time.CLOCK_BOOTTIME)-m)'
```

第一个脚本中，`@cpu` 只有少数 CPU 的计数显著（负责计时的 CPU 可能随 NO_HZ 而轮换，8.10 节）；空闲系统上调用频率明显低于 1000 次/秒。第二个脚本中 `logarithmic_accumulation()` 的第三个参数就是 `shift`，空闲系统上能看到较大的值；它是 `static` 函数，若编译器将其内联则无法挂 kprobe，可先用 `grep logarithmic_accumulation /proc/kallsyms` 确认符号存在。

## 小结

- timekeeper 记录"上次更新时的时钟源读数与对应时间"，任一时刻的时间 = 基准 + （读数 − `cycle_last`）× `mult` >> `shift`；`xtime_nsec` 以 shifted ns 保存亚秒部分以保留精度。
- 只有 `tkr_mono`（受 NTP 调频）与 `tkr_raw` 两条时间线由时钟源推进，REALTIME、BOOTTIME、TAI 都是在单调时间上加偏移；设置时间、闰秒、挂起只改变偏移，不改变 MONOTONIC。
- 读路径是无锁的顺序计数读区间，热数据按缓存行布局；异常分支处理跨 CPU 的负向运动与长时间未更新导致的乘法溢出。
- `update_wall_time()` 以 tick 为间隔、按 2 的幂次对数累加周期，应用 NTP 调整后，在 `timekeeping_update_from_shadow()` 中同步更新 vDSO、快速 timekeeper，并一次性把影子副本拷给读者。
- 设置时间前先 `timekeeping_forward_now()`，修改后经 `clock_was_set()` 通知 hrtimer 与 timerfd；挂起时长优先由不停的时钟源计算，其次是持久时钟，最后是 RTC。
- `ktime_get_*_fast_ns()` 用 latch 双副本提供 NMI 安全的读取，代价是跨更新的轻微不单调。

## 参考

- `linux/Documentation/core-api/timekeeping.rst`
- `linux/Documentation/timers/timekeeping.rst`
- `include/linux/timekeeper_internal.h` 中 `struct timekeeper` 的缓存行布局注释
- `kernel/time/timekeeping.c` 中 `mg_floor` 前的注释（多粒度时间戳）
- `linux/Documentation/ABI/stable/sysfs-kernel-time-aux-clocks`（辅助时钟的 sysfs 接口）
