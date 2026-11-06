# 8.2 时钟源框架：clocksource

> **本节源码**：[`include/linux/clocksource.h`](../../linux/include/linux/clocksource.h)、[`kernel/time/clocksource.c`](../../linux/kernel/time/clocksource.c)、[`kernel/time/jiffies.c`](../../linux/kernel/time/jiffies.c)、[`kernel/time/timekeeping_internal.h`](../../linux/kernel/time/timekeeping_internal.h)、[`kernel/time/timekeeping.c`](../../linux/kernel/time/timekeeping.c)
> **关键函数**：`clocks_calc_mult_shift()`、`__clocksource_register_scale()`、`__clocksource_update_freq_scale()`、`clocks_calc_max_nsecs()`、`clocksource_select()`、`timekeeping_notify()`、`clocksource_watchdog()`、`clocksource_mark_unstable()`、`clocksource_delta()`
> **前置知识**：8.1 时间子系统概览、7.5 顺序锁

## 概述

时钟源（Clocksource）是对"一个自由运行的硬件计数器"的抽象：它只提供一个 `read()` 回调，返回当前计数值，计数值以固定频率单调增长，到达位宽上限后回绕。timekeeping 层通过两次读数之差得到经过的周期数，再换算成纳秒。

系统中通常同时存在多个时钟源（TSC、HPET、ACPI PM 定时器……），它们的速度、精度、可靠性各不相同。clocksource 框架负责三件事：一是为每个时钟源预先算好**周期到纳秒的换算参数** `mult`/`shift`，让热路径上只需一次乘法和一次移位；二是按**评级**（rating）从所有已注册的时钟源中选出最好的一个交给 timekeeping；三是用一个可靠的时钟源充当**时钟源看门狗**（Clocksource Watchdog，下文简称 watchdog），持续检验其他时钟源（主要是 TSC）是否可信，发现问题时降级并重新选择。

本节分析这三部分以及作为兜底的 `jiffies` 时钟源。x86 上具体的时钟源驱动留给 8.3 节，timekeeping 如何使用时钟源留给 8.4 节。

## 核心数据结构

### struct clocksource

[`include/linux/clocksource.h:101-138`](../../linux/include/linux/clocksource.h#L101-L138)（v6.18.60）

```c
struct clocksource {
	u64			(*read)(struct clocksource *cs);	// [注] 读取当前计数值
	u64			mask;		// [注] 计数器有效位掩码，用于回绕时的二进制补码减法
	u32			mult;		// [注] 周期 → 纳秒的乘数
	u32			shift;		// [注] 周期 → 纳秒的右移位数：ns = (cyc * mult) >> shift
	u64			max_idle_ns;	// [注] 两次读取之间允许的最长间隔，NO_HZ 停 tick 的上限
	u32			maxadj;		// [注] NTP 调整 mult 时允许的最大偏差（约 11%）
	u32			uncertainty_margin;	// [注] watchdog 比对时允许的误差
#ifdef CONFIG_ARCH_CLOCKSOURCE_DATA
	struct arch_clocksource_data archdata;
#endif
	u64			max_cycles;	// [注] 与 mult 相乘不溢出 64 位的最大周期数
	u64			max_raw_delta;	// [注] 超过此值的差值视为"时间倒流"
	const char		*name;
	struct list_head	list;		// [注] 挂在按 rating 降序排列的 clocksource_list 上
	u32			freq_khz;
	int			rating;		// [注] 评级，越高越优先
	enum clocksource_ids	id;
	enum vdso_clock_mode	vdso_clock_mode;	// [注] vDSO 能否直接读取（8.12 节）
	unsigned long		flags;
	struct clocksource_base *base;

	int			(*enable)(struct clocksource *cs);
	void			(*disable)(struct clocksource *cs);
	void			(*suspend)(struct clocksource *cs);
	void			(*resume)(struct clocksource *cs);
	void			(*mark_unstable)(struct clocksource *cs);	// [注] 被 watchdog 判为不稳定时回调
	void			(*tick_stable)(struct clocksource *cs);		// [注] 每次 watchdog 检查通过时回调

	/* private: */
#ifdef CONFIG_CLOCKSOURCE_WATCHDOG
	/* Watchdog related data, used by the framework */
	struct list_head	wd_list;	// [注] 挂在 watchdog_list 上
	u64			cs_last;	// [注] 上次检查时本时钟源的读数
	u64			wd_last;	// [注] 上次检查时 watchdog 的读数
#endif
	struct module		*owner;
};
```

结构体注释特别说明它**不在** timekeeping 的热路径上：timekeeper 会把 `read`、`mask`、`mult`、`shift` 等热字段复制到自己的 `struct tk_read_base` 中（8.4 节），因此这里不需要缓存行对齐。

### 标志位

[`include/linux/clocksource.h:143-153`](../../linux/include/linux/clocksource.h#L143-L153)（v6.18.60）

```c
#define CLOCK_SOURCE_IS_CONTINUOUS		0x01
#define CLOCK_SOURCE_MUST_VERIFY		0x02

#define CLOCK_SOURCE_WATCHDOG			0x10
#define CLOCK_SOURCE_VALID_FOR_HRES		0x20
#define CLOCK_SOURCE_UNSTABLE			0x40
#define CLOCK_SOURCE_SUSPEND_NONSTOP		0x80
#define CLOCK_SOURCE_RESELECT			0x100
#define CLOCK_SOURCE_VERIFY_PERCPU		0x200
/* simplify initialization of mask field */
#define CLOCKSOURCE_MASK(bits) GENMASK_ULL((bits) - 1, 0)
```

| 标志 | 设置者 | 含义 |
|---|---|---|
| `IS_CONTINUOUS` | 驱动 | 计数连续、不跳变，可作为高精度时钟源的候选 |
| `MUST_VERIFY` | 驱动 | 需要被 watchdog 检验（TSC 默认设置）；没有此标志的时钟源才能充当 watchdog |
| `WATCHDOG` | 框架 | watchdog 已为它记录了基准读数 |
| `VALID_FOR_HRES` | 驱动或框架 | 可用于高精度定时器与 NO_HZ（单次触发模式的前提） |
| `UNSTABLE` | 框架 | 已被判定不稳定，rating 将被降为 0 |
| `SUSPEND_NONSTOP` | 驱动 | 系统挂起期间仍在计数，可用于计算挂起时长 |
| `RESELECT` | 框架 | 需要重新选择时钟源 |
| `VERIFY_PERCPU` | 驱动 | 被判不稳定时额外检查各 CPU 之间是否同步 |

*表 8-5：时钟源标志位*

### rating 的约定

头文件注释给出了 rating 的分档建议（[`include/linux/clocksource.h:56-70`](../../linux/include/linux/clocksource.h#L56-L70)）：1～99 仅用于启动与测试，100～199 可用但不理想，200～299 正确可用，300～399 快速且准确，400～499 理想。x86 上常见时钟源的评级如下：

| 时钟源 | rating | 定义位置 | 备注 |
|---|---|---|---|
| `jiffies` | 1 | [`kernel/time/jiffies.c:32-41`](../../linux/kernel/time/jiffies.c#L32-L41) | 兜底，启动早期使用 |
| `refined-jiffies` | 2 | [`kernel/time/jiffies.c:77-100`](../../linux/kernel/time/jiffies.c#L77-L100) | 以 PIT 频率修正 mult |
| `pit` | 110 | [`drivers/clocksource/i8253.c:90-92`](../../linux/drivers/clocksource/i8253.c#L90-L92) | |
| `acpi_pm` | 200 | [`drivers/clocksource/acpi_pm.c:96-98`](../../linux/drivers/clocksource/acpi_pm.c#L96-L98) | 需要读三次校验的芯片组降为 120 |
| `hpet` | 250 | [`arch/x86/kernel/hpet.c:852-859`](../../linux/arch/x86/kernel/hpet.c#L852-L859) | |
| `tsc-early` | 299 | [`arch/x86/kernel/tsc.c:1167-1182`](../../linux/arch/x86/kernel/tsc.c#L1167-L1182) | 粗校准阶段的 TSC |
| `tsc` | 300 | [`arch/x86/kernel/tsc.c:1189-1205`](../../linux/arch/x86/kernel/tsc.c#L1189-L1205) | |
| `kvm-clock` | 400 | [`arch/x86/kernel/kvmclock.c:157-165`](../../linux/arch/x86/kernel/kvmclock.c#L157-L165) | 宿主提供不变 TSC 时降为 299，让位于 `tsc` |
| `hyperv_clocksource_tsc_page` | 500 | [`drivers/clocksource/hyperv_timer.c:494-496`](../../linux/drivers/clocksource/hyperv_timer.c#L494-L496) | Hyper-V 虚拟机 |

*表 8-6：x86 上常见时钟源的评级*

## 周期到纳秒的换算

### mult 与 shift

把周期数换算成纳秒，数学上是 `ns = cycles × 10⁹ / freq`。为避免热路径上的除法，内核把系数 `10⁹ / freq` 表示成定点数 `mult / 2^shift`：

[`include/linux/clocksource.h:212-215`](../../linux/include/linux/clocksource.h#L212-L215)（v6.18.60）

```c
static inline s64 clocksource_cyc2ns(u64 cycles, u32 mult, u32 shift)
{
	return ((u64) cycles * mult) >> shift;
}
```

`shift` 越大，`mult` 的有效位越多，换算越精确；但 `cycles × mult` 不能溢出 64 位，`shift` 越大，允许的 `cycles` 就越小。`clocks_calc_mult_shift()` 在二者之间折中：

[`kernel/time/clocksource.c:58-87`](../../linux/kernel/time/clocksource.c#L58-L87)（v6.18.60）

```c
void
clocks_calc_mult_shift(u32 *mult, u32 *shift, u32 from, u32 to, u32 maxsec)
{
	u64 tmp;
	u32 sft, sftacc= 32;

	/*
	 * Calculate the shift factor which is limiting the conversion
	 * range:
	 */
	tmp = ((u64)maxsec * from) >> 32;
	while (tmp) {
		tmp >>=1;
		sftacc--;
	}

	/*
	 * Find the conversion shift/mult pair which has the best
	 * accuracy and fits the maxsec conversion range:
	 */
	for (sft = 32; sft > 0; sft--) {
		tmp = (u64) to << sft;
		tmp += from / 2;
		do_div(tmp, from);
		if ((tmp >> sftacc) == 0)
			break;
	}
	*mult = tmp;
	*shift = sft;
}
```

算法分两步：

1. `maxsec × from` 是 `maxsec` 秒内的最大周期数，它超出 32 位的位数记为 *k*，则 `mult` 最多只能有 `32 − k` 位（`sftacc`），这样两者相乘才不会超过 64 位。
2. 从 `shift = 32` 开始向下尝试，取第一个使 `mult = (to << shift) / from` 不超过 `sftacc` 位的值——这就是满足溢出约束下精度最高的组合。

以 3 GHz 的 TSC 为例：时钟源掩码为 64 位，`__clocksource_update_freq_scale()` 把 `maxsec` 限制为 600 秒（见下文），600 × 3×10⁹ ≈ 1.8×10¹²，右移 32 位后约为 419，需要 9 位，于是 `sftacc = 23`；`mult ≈ 2^shift / 3` 不超过 23 位的最大 `shift` 是 24，最终 `mult = 5592405`、`shift = 24`。`mult` 每变化 1，换算系数变化约 1/5592405 ≈ 0.18 ppm，这也就是 NTP 调频（8.5 节）的最小步长。

### 注册时计算的派生参数

[`kernel/time/clocksource.c:1144-1172`](../../linux/kernel/time/clocksource.c#L1144-L1172)（v6.18.60）

```c
void __clocksource_update_freq_scale(struct clocksource *cs, u32 scale, u32 freq)
{
	u64 sec;

	/*
	 * Default clocksources are *special* and self-define their mult/shift.
	 * But, you're not special, so you should specify a freq value.
	 */
	if (freq) {
		/*
		 * Calc the maximum number of seconds which we can run before
		 * wrapping around. For clocksources which have a mask > 32-bit
		 * we need to limit the max sleep time to have a good
		 * conversion precision. 10 minutes is still a reasonable
		 * amount. That results in a shift value of 24 for a
		 * clocksource with mask >= 40-bit and f >= 4GHz. That maps to
		 * ~ 0.06ppm granularity for NTP.
		 */
		sec = cs->mask;
		do_div(sec, freq);
		do_div(sec, scale);
		if (!sec)
			sec = 1;
		else if (sec > 600 && cs->mask > UINT_MAX)
			sec = 600;

		clocks_calc_mult_shift(&cs->mult, &cs->shift, freq,
				       NSEC_PER_SEC / scale, sec * scale);
	}
	...
```

`maxsec` 取"计数器回绕一圈的时间"，对 64 位计数器则封顶 600 秒——回绕要数百年，若照此计算 `shift` 会小到失去精度。函数随后计算三个派生量：

- `uncertainty_margin`：未指定时按"一个时钟周期"计算，但不低于 `2 × WATCHDOG_MAX_SKEW`（本书配置 `CONFIG_CLOCKSOURCE_WATCHDOG_MAX_SKEW_US=100`，即 200 μs）。
- `maxadj`：`mult` 的 11%（`clocksource_max_adjustment()`，[`kernel/time/clocksource.c:925-934`](../../linux/kernel/time/clocksource.c#L925-L934)），NTP 调频不得超过这个范围；若 `mult ± maxadj` 会溢出 32 位，则把 `mult` 与 `shift` 同时减半直到不溢出。
- `max_cycles`、`max_idle_ns`、`max_raw_delta`：由 `clocksource_update_max_deferment()` 计算。

[`kernel/time/clocksource.c:951-1000`](../../linux/kernel/time/clocksource.c#L951-L1000)（v6.18.60，节选）

```c
u64 clocks_calc_max_nsecs(u32 mult, u32 shift, u32 maxadj, u64 mask, u64 *max_cyc)
{
	u64 max_nsecs, max_cycles;

	max_cycles = ULLONG_MAX;
	do_div(max_cycles, mult+maxadj);	// [注] 乘以调频后的最大 mult 也不溢出

	max_cycles = min(max_cycles, mask);	// [注] 也不能超过计数器回绕
	max_nsecs = clocksource_cyc2ns(max_cycles, mult - maxadj, shift);
	...
	/* Return 50% of the actual maximum, so we can detect bad values */
	max_nsecs >>= 1;

	return max_nsecs;
}
...
static inline void clocksource_update_max_deferment(struct clocksource *cs)
{
	cs->max_idle_ns = clocks_calc_max_nsecs(cs->mult, cs->shift,
						cs->maxadj, cs->mask,
						&cs->max_cycles);
	...
	cs->max_raw_delta = (cs->mask >> 1) + (cs->mask >> 2) + (cs->mask >> 3);
}
```

`max_idle_ns` 的含义是"timekeeper 至少要每隔这么久读一次时钟源"，否则差值可能溢出乘法或越过回绕。它只取理论上限的一半，留出 50% 余量来检测异常。NO_HZ 停 tick 时，负责计时的 CPU 的睡眠时长不能超过它（`timekeeping_max_deferment()`，8.10 节）。以上述 3 GHz TSC 为例，`max_idle_ns` 约 440 秒，启动日志中形如 `clocksource: tsc: mask: 0xffffffffffffffff max_cycles: 0x..., max_idle_ns: 440795... ns` 的输出就来自这里（[`kernel/time/clocksource.c:1220-1221`](../../linux/kernel/time/clocksource.c#L1220-L1221)）。

`max_raw_delta` 取掩码的 7/8，用于在 `clocksource_delta()` 中识别"时间倒流"：

[`kernel/time/timekeeping_internal.h:33-42`](../../linux/kernel/time/timekeeping_internal.h#L33-L42)（v6.18.60）

```c
static inline u64 clocksource_delta(u64 now, u64 last, u64 mask, u64 max_delta)
{
	u64 ret = (now - last) & mask;

	/*
	 * Prevent time going backwards by checking the result against
	 * @max_delta. If greater, return 0.
	 */
	return ret > max_delta ? 0 : ret;
}
```

`(now − last) & mask` 能正确处理计数器回绕：例如 24 位的 ACPI PM 定时器从 `0xfffff0` 回绕到 `0x000010`，差值仍是 `0x20`。但如果 `now` 因为 CPU 之间 TSC 微小不同步而略小于 `last`，无符号差值会变成一个接近掩码的巨大数字，导致时间向前跳跃数百年。这里把超过掩码 7/8 的差值当作负数处理并返回 0，代价是极长时间不读时钟源（超过回绕周期的 7/8）会被误判——这在 `max_idle_ns` 的约束下不会发生。

## 主流程分析

### 调用链总览

```
clocksource_register_khz() / clocksource_register_hz()   include/linux/clocksource.h
└── __clocksource_register_scale()                       kernel/time/clocksource.c
    ├── clocksource_arch_init()                          arch/x86/kernel/time.c    // vDSO 要求 64 位掩码
    ├── __clocksource_update_freq_scale()                                          // mult/shift/maxadj/max_idle_ns
    ├── clocksource_enqueue()                                                      // 按 rating 降序插入
    ├── clocksource_enqueue_watchdog()                                             // MUST_VERIFY → watchdog_list
    ├── clocksource_select()
    │   └── __clocksource_select()
    │       ├── clocksource_find_best()
    │       └── timekeeping_notify()                     kernel/time/timekeeping.c
    │           ├── stop_machine(change_clocksource)                               // 在所有 CPU 停顿时切换
    │           └── tick_clock_notify()                  kernel/time/tick-sched.c  // 通知 tick 层检查能否进入单次模式
    ├── clocksource_select_watchdog()
    └── __clocksource_suspend_select()
```

### 注册

[`kernel/time/clocksource.c:1236-1267`](../../linux/kernel/time/clocksource.c#L1236-L1267)（v6.18.60）

```c
int __clocksource_register_scale(struct clocksource *cs, u32 scale, u32 freq)
{
	unsigned long flags;

	clocksource_arch_init(cs);

	if (WARN_ON_ONCE((unsigned int)cs->id >= CSID_MAX))
		cs->id = CSID_GENERIC;
	if (cs->vdso_clock_mode < 0 ||
	    cs->vdso_clock_mode >= VDSO_CLOCKMODE_MAX) {
		pr_warn("clocksource %s registered with invalid VDSO mode %d. Disabling VDSO support.\n",
			cs->name, cs->vdso_clock_mode);
		cs->vdso_clock_mode = VDSO_CLOCKMODE_NONE;
	}

	/* Initialize mult/shift and max_idle_ns */
	__clocksource_update_freq_scale(cs, scale, freq);

	/* Add clocksource to the clocksource list */
	mutex_lock(&clocksource_mutex);

	clocksource_watchdog_lock(&flags);
	clocksource_enqueue(cs);
	clocksource_enqueue_watchdog(cs);
	clocksource_watchdog_unlock(&flags);

	clocksource_select();
	clocksource_select_watchdog(false);
	__clocksource_suspend_select(cs);
	mutex_unlock(&clocksource_mutex);
	return 0;
}
```

驱动通常调用 `clocksource_register_hz()` 或 `clocksource_register_khz()`，它们只是以 `scale = 1` 或 `1000` 包装本函数。`clocksource_enqueue()`（[`kernel/time/clocksource.c:1118-1130`](../../linux/kernel/time/clocksource.c#L1118-L1130)）把时钟源插到第一个 rating 比它小的节点之前，于是 `clocksource_list` 始终按 rating 降序排列，"选最好的"只需取第一个满足条件的节点。

x86 的 `clocksource_arch_init()`（[`arch/x86/kernel/time.c:101-111`](../../linux/arch/x86/kernel/time.c#L101-L111)）检查：声明支持 vDSO 的时钟源必须是 64 位掩码，否则取消其 vDSO 支持——x86 的 vDSO 换算代码不做掩码运算（8.12 节）。

### 选择

[`kernel/time/clocksource.c:1002-1022`](../../linux/kernel/time/clocksource.c#L1002-L1022)（v6.18.60）

```c
static struct clocksource *clocksource_find_best(bool oneshot, bool skipcur)
{
	struct clocksource *cs;

	if (!finished_booting || list_empty(&clocksource_list))
		return NULL;

	/*
	 * We pick the clocksource with the highest rating. If oneshot
	 * mode is active, we pick the highres valid clocksource with
	 * the best rating.
	 */
	list_for_each_entry(cs, &clocksource_list, list) {
		if (skipcur && cs == curr_clocksource)
			continue;
		if (oneshot && !(cs->flags & CLOCK_SOURCE_VALID_FOR_HRES))
			continue;
		return cs;
	}
	return NULL;
}
```

两个限制条件：

- `finished_booting` 为假时不做任何选择。启动早期注册的时钟源只是进入链表，等到 `fs_initcall` 阶段的 `clocksource_done_booting()`（[`kernel/time/clocksource.c:1100-1113`](../../linux/kernel/time/clocksource.c#L1100-L1113)）先运行一次 watchdog 清理掉已知不稳定的时钟源，再统一选择。源码注释称之为"避免启动时大量切换时钟源的 hack"。
- 一旦 tick 层进入单次触发模式（高精度或 NO_HZ，`tick_oneshot_mode_active()`），就只能选 `VALID_FOR_HRES` 的时钟源。单次模式依赖时钟源计算"距下一事件还有多久"，换成 `jiffies` 这类依赖 tick 本身推进的时钟源会形成循环依赖。

`__clocksource_select()`（[`kernel/time/clocksource.c:1024-1073`](../../linux/kernel/time/clocksource.c#L1024-L1073)）在此基础上处理用户指定的时钟源：启动参数 `clocksource=`（[`kernel/time/clocksource.c:1505-1514`](../../linux/kernel/time/clocksource.c#L1505-L1514)）或写 sysfs 的 `current_clocksource`（[`kernel/time/clocksource.c:1373-1388`](../../linux/kernel/time/clocksource.c#L1373-L1388)）都会填写 `override_name`。若指定的时钟源不满足单次模式的要求，内核会打印"cannot switch while in HRT/NOHZ mode"或"deferring"，而不是盲目切换。最终调用 `timekeeping_notify()`：

[`kernel/time/timekeeping.c:1628-1637`](../../linux/kernel/time/timekeeping.c#L1628-L1637)（v6.18.60）

```c
int timekeeping_notify(struct clocksource *clock)
{
	struct timekeeper *tk = &tk_core.timekeeper;

	if (tk->tkr_mono.clock == clock)
		return 0;
	stop_machine(change_clocksource, clock, NULL);
	tick_clock_notify();
	return tk->tkr_mono.clock == clock ? 0 : -1;
}
```

切换时钟源使用 `stop_machine()`：在所有 CPU 都停在一个已知点、关中断的状态下执行 `change_clocksource()`。原因是 timekeeper 中的 `cycle_last`、`mult`、`shift` 与时钟源一一对应，切换必须原子地完成"用旧时钟源把时间推进到此刻 → 换新时钟源并读取新的 `cycle_last`"（[`kernel/time/timekeeping.c:1583-1619`](../../linux/kernel/time/timekeeping.c#L1583-L1619)，8.4 节）。时钟源切换非常罕见，这种重量级手段的代价可以接受。

## clocksource watchdog

### 为什么需要 watchdog

TSC 读取快、分辨率高，但历史上问题很多：早期 CPU 的 TSC 频率随 P-state 变化、在深度 C-state 停止计数，多路服务器上各 CPU 的 TSC 可能不同步，BIOS/SMM 还可能偷偷改写 TSC。这些问题通常无法在启动时一次性检测出来。clocksource watchdog 的思路是：拿一个慢但可靠的时钟源（HPET 或 ACPI PM 定时器）作参照，每隔 0.5 秒同时读一次参照时钟和被检时钟，比较两者在这段间隔内走过的时间；若偏差超出允许范围，就把被检时钟标为不稳定并降级。

```
时间 →        t0                       t0 + 0.5s
watchdog:     wd_last ───── wd_nsec ─────▶ wdnow
被检时钟:     cs_last ───── cs_nsec ─────▶ csnow
判据：|cs_nsec − wd_nsec| ≤ cs->uncertainty_margin + watchdog->uncertainty_margin
```

*图 8-3：watchdog 的比对方式*

哪些时钟源被检、谁当 watchdog，由 `MUST_VERIFY` 标志决定（[`kernel/time/clocksource.c:623-636`](../../linux/kernel/time/clocksource.c#L623-L636)、[`kernel/time/clocksource.c:638-673`](../../linux/kernel/time/clocksource.c#L638-L673)）：带此标志的进入 `watchdog_list` 等待检验；不带此标志的时钟源中 rating 最高者被选为 watchdog。TSC 带此标志，HPET 与 ACPI PM 不带，因此通常是 HPET 检验 TSC。

### 检查流程

watchdog 是一个时间轮定时器（8.7 节），每 `WATCHDOG_INTERVAL` 即 0.5 秒触发一次，并且每次换到下一个在线 CPU 上运行，以便顺带发现 CPU 之间的不一致：

[`kernel/time/clocksource.c:110-119`](../../linux/kernel/time/clocksource.c#L110-L119)（v6.18.60）

```c
/*
 * Interval: 0.5sec.
 */
#define WATCHDOG_INTERVAL (HZ >> 1)
#define WATCHDOG_INTERVAL_MAX_NS ((2 * WATCHDOG_INTERVAL) * (NSEC_PER_SEC / HZ))

/*
 * Threshold: 0.0312s, when doubled: 0.0625s.
 */
#define WATCHDOG_THRESHOLD (NSEC_PER_SEC >> 5)
```

每个被检时钟源的处理分为几步（[`kernel/time/clocksource.c:424-598`](../../linux/kernel/time/clocksource.c#L424-L598)）：

1. **读取**：`cs_watchdog_read()`（[`kernel/time/clocksource.c:253-302`](../../linux/kernel/time/clocksource.c#L253-L302)）在关中断状态下按"watchdog → 被检时钟 → watchdog → watchdog"的顺序读四次。若前两次 watchdog 读数间隔过大（被 SMI、NMI 或 vCPU 抢占打断），说明这次采样本身不可靠，最多重试 `ilog2(在线 CPU 数)/2 + 1` 次；如果连续两次读 watchdog 也很慢，说明系统负载极重，跳过本次检查并暂停 5 分钟（`WD_READ_SKIP`）。
2. **建立基准**：首次检查或刚恢复（挂起唤醒后 `watchdog_reset_pending`）时只记录 `cs_last`、`wd_last`。
3. **比对**：

[`kernel/time/clocksource.c:484-489`](../../linux/kernel/time/clocksource.c#L484-L489)（v6.18.60）

```c
		wd_nsec = cycles_to_nsec_safe(watchdog, cs->wd_last, wdnow);
		cs_nsec = cycles_to_nsec_safe(cs, cs->cs_last, csnow);
		wdlast = cs->wd_last; /* save these in case we print them */
		cslast = cs->cs_last;
		cs->cs_last = csnow;
		cs->wd_last = wdnow;
```

   若间隔超过 `WATCHDOG_INTERVAL_MAX_NS`（1 秒，通常因为定时器软中断被延迟），比对结果不可信，跳过；否则比较 `|cs_nsec − wd_nsec|` 与两者 `uncertainty_margin` 之和，超出即打印 "Marking clocksource 'tsc' as unstable because the skew is too large" 并调用 `__clocksource_unstable()`。
4. **晋升**：检查通过且两者都是连续时钟源时，为被检时钟源置上 `VALID_FOR_HRES`：

[`kernel/time/clocksource.c:545-572`](../../linux/kernel/time/clocksource.c#L545-L572)（v6.18.60）

```c
		if (!(cs->flags & CLOCK_SOURCE_VALID_FOR_HRES) &&
		    (cs->flags & CLOCK_SOURCE_IS_CONTINUOUS) &&
		    (watchdog->flags & CLOCK_SOURCE_IS_CONTINUOUS)) {
			/* Mark it valid for high-res. */
			cs->flags |= CLOCK_SOURCE_VALID_FOR_HRES;

			/*
			 * clocksource_done_booting() will sort it if
			 * finished_booting is not set yet.
			 */
			if (!finished_booting)
				continue;

			/*
			 * If this is not the current clocksource let
			 * the watchdog thread reselect it. Due to the
			 * change to high res this clocksource might
			 * be preferred now. If it is the current
			 * clocksource let the tick code know about
			 * that change.
			 */
			if (cs != curr_clocksource) {
				cs->flags |= CLOCK_SOURCE_RESELECT;
				schedule_work(&watchdog_work);
			} else {
				tick_clock_notify();
			}
		}
```

这就是 8.1 节所说的"被 watchdog 认可后才能切换到高精度模式"：`tsc-early` 刚注册时没有 `VALID_FOR_HRES`（[`arch/x86/kernel/tsc.c:1167-1182`](../../linux/arch/x86/kernel/tsc.c#L1167-L1182)），第一次检查通过后才获得它，并经 `tick_clock_notify()` 通知 tick 层可以尝试切换模式。正式的 `tsc` 时钟源在定义时就带有 `VALID_FOR_HRES`，源码注释解释说这是为了在注销 `tsc-early` 时能立即接替，而且只有 TSC 未被判定为不稳定时才会注册它（[`arch/x86/kernel/tsc.c:1184-1188`](../../linux/arch/x86/kernel/tsc.c#L1184-L1188)）；它仍带有 `MUST_VERIFY`，继续接受 watchdog 检验（8.3 节）。

### 降级

[`kernel/time/clocksource.c:202-222`](../../linux/kernel/time/clocksource.c#L202-L222)（v6.18.60）

```c
static void __clocksource_unstable(struct clocksource *cs)
{
	cs->flags &= ~(CLOCK_SOURCE_VALID_FOR_HRES | CLOCK_SOURCE_WATCHDOG);
	cs->flags |= CLOCK_SOURCE_UNSTABLE;

	/*
	 * If the clocksource is registered clocksource_watchdog_kthread() will
	 * re-rate and re-select.
	 */
	if (list_empty(&cs->list)) {
		cs->rating = 0;
		return;
	}

	if (cs->mark_unstable)
		cs->mark_unstable(cs);

	/* kick clocksource_watchdog_kthread() */
	if (finished_booting)
		schedule_work(&watchdog_work);
}
```

watchdog 定时器运行在软中断上下文，不能直接切换时钟源（`timekeeping_notify()` 要调用 `stop_machine()`，后者可能睡眠）。因此它只标记、回调驱动的 `mark_unstable()`（TSC 驱动借此停用 TSC 版的 `sched_clock` 稳定标记，8.3 节），再调度一个工作项。工作项又不直接干活，而是创建一个临时内核线程 `kwatchdog`——注释解释说，从工作队列中调用 `stop_machine()` 会与 CPU 热插拔形成锁反转（[`kernel/time/clocksource.c:177-193`](../../linux/kernel/time/clocksource.c#L177-L193)）。线程在 `__clocksource_watchdog_kthread()`（[`kernel/time/clocksource.c:687-716`](../../linux/kernel/time/clocksource.c#L687-L716)）中把不稳定时钟源的 rating 改为 0 并重新入队，然后 `clocksource_select()` 选出新的时钟源，内核日志中出现 "clocksource: Switched to clocksource hpet"。

若当前时钟源带有 `VERIFY_PERCPU`（TSC 带有），线程还会先调用 `clocksource_verify_percpu()`（[`kernel/time/clocksource.c:359-412`](../../linux/kernel/time/clocksource.c#L359-L412)）：在本 CPU 读一次、经 IPI 让其他 CPU 读一次、再在本 CPU 读一次，检查其他 CPU 的读数是否落在前后两次本地读数之间，并打印"CPUs ... ahead/behind of CPU ..."，帮助定位是哪些 CPU 不同步。默认抽查 8 个 CPU（模块参数 `verify_n_cpus`）。

除 watchdog 外，驱动自己也可以调用 `clocksource_mark_unstable()`（[`kernel/time/clocksource.c:231-242`](../../linux/kernel/time/clocksource.c#L231-L242)），例如 TSC 同步检查失败时（8.3 节）。

## jiffies 时钟源

[`kernel/time/jiffies.c:16-41`](../../linux/kernel/time/jiffies.c#L16-L41)（v6.18.60）

```c
static u64 jiffies_read(struct clocksource *cs)
{
	return (u64) jiffies;
}

/*
 * The Jiffies based clocksource is the lowest common
 * denominator clock source which should function on
 * all systems. It has the same coarse resolution as
 * the timer interrupt frequency HZ and it suffers
 * inaccuracies caused by missed or lost timer
 * interrupts and the inability for the timer
 * interrupt hardware to accurately tick at the
 * requested HZ value. It is also not recommended
 * for "tick-less" systems.
 */
static struct clocksource clocksource_jiffies = {
	.name			= "jiffies",
	.rating			= 1, /* lowest valid rating*/
	.uncertainty_margin	= 32 * NSEC_PER_MSEC,
	.read			= jiffies_read,
	.mask			= CLOCKSOURCE_MASK(32),
	.mult			= TICK_NSEC << JIFFIES_SHIFT, /* details above */
	.shift			= JIFFIES_SHIFT,
	.max_cycles		= 10,
};
```

`jiffies` 时钟源把 jiffies 本身当作计数器，每"周期"等于 `TICK_NSEC` 纳秒。它是 `clocksource_default_clock()` 的返回值（[`kernel/time/jiffies.c:66-73`](../../linux/kernel/time/jiffies.c#L66-L73)），`timekeeping_init()` 用它启动 timekeeper。这里存在一个看似循环的依赖：jiffies 由 tick 推进，tick 又调用 `update_wall_time()` 读取 jiffies 时钟源。在周期 tick 模式下这没有问题，但在单次模式下不可行，所以它没有 `VALID_FOR_HRES`，也就是注释所说的"不推荐用于无 tick 系统"。

x86 在 `setup_arch()` 中还注册了一个 `refined-jiffies`（[`arch/x86/kernel/setup.c:1280`](../../linux/arch/x86/kernel/setup.c#L1280)），它以 PIT 的实际频率 `CLOCK_TICK_RATE` 修正 `mult`，补偿 PIT 无法精确产生 1000 Hz 带来的误差，rating 为 2。

## 关键细节 / 设计考量

### TSC 也可以反过来充当 watchdog

在 TSC 可信度很高的现代平台上，watchdog 反而可能误伤：HPET 读取慢、在虚拟机中受 vCPU 抢占影响，偶尔一次延迟就可能让 TSC 被错误地判为不稳定。因此 TSC 驱动在满足"恒定频率、C-state 不停、有 `TSC_ADJUST` 寄存器、不超过 4 个封装"时会清除 TSC 的 `MUST_VERIFY`，完全不检验 TSC（8.3 节）。此时如果用户指定 `tsc=watchdog`，`tsc_clocksource_watchdog_disabled()` 为真，HPET 与 ACPI PM 反而被加上 `MUST_VERIFY`（[`arch/x86/kernel/hpet.c:1085-1086`](../../linux/arch/x86/kernel/hpet.c#L1085-L1086)、[`drivers/clocksource/acpi_pm.c:246-247`](../../linux/drivers/clocksource/acpi_pm.c#L246-L247)），由 TSC 去检验它们。

### 误差容限的演进

watchdog 的阈值经历过多次调整。早期固定为 `WATCHDOG_THRESHOLD`（两倍约 62.5 ms），对于 0.5 秒的间隔意味着 12.5% 的容差，几乎只能发现 TSC 停止或频率剧变这类粗大问题。2021 年前后的一组补丁（v5.14 起陆续合入）引入了 `uncertainty_margin` 与 `WATCHDOG_MAX_SKEW`，把默认容差收紧到约 500 ppm，并加入了"读 watchdog 延迟过大则重试"的逻辑以过滤 SMI 等干扰（源码注释见 [`kernel/time/clocksource.c:121-150`](../../linux/kernel/time/clocksource.c#L121-L150)）。未设置 `CONFIG_CLOCKSOURCE_WATCHDOG_MAX_SKEW_US` 时 `MAX_SKEW_USEC` 为 62 μs，两个时钟源的 `uncertainty_margin` 之和至少约 250 μs，相对 0.5 秒间隔即约 500 ppm；本书配置把它设为 100 μs，和至少为 400 μs，约 800 ppm。

### 挂起期间的时钟源

`__clocksource_suspend_select()`（[`kernel/time/clocksource.c:757-778`](../../linux/kernel/time/clocksource.c#L757-L778)）另外选出一个"挂起时钟源"：带有 `SUSPEND_NONSTOP` 标志、在挂起期间继续计数的时钟源。系统恢复时 timekeeping 用它计算挂起了多久，比读 RTC（只有秒级精度）更准确。支持 `X86_FEATURE_NONSTOP_TSC_S3` 的 CPU 上 TSC 带有此标志（[`arch/x86/kernel/tsc.c:1421-1422`](../../linux/arch/x86/kernel/tsc.c#L1421-L1422)）。

## 动手实验

在 dev 虚拟机中执行（需要 root 权限）：

```bash
cd /sys/devices/system/clocksource/clocksource0
cat available_clocksource current_clocksource

# 手动切换时钟源，观察 clock_gettime 开销的变化
echo hpet > current_clocksource      # 若 hpet 可用
dmesg | tail -n 2                     # clocksource: Switched to clocksource hpet
python3 -c 'import time,timeit; print(timeit.timeit(lambda: time.clock_gettime(time.CLOCK_MONOTONIC), number=1000000))'
echo tsc > current_clocksource

# 查看各时钟源注册时计算出的参数
dmesg | grep 'max_idle_ns'
```

切换到 `hpet` 后，`clock_gettime()` 不再能走 vDSO 中的 TSC 快速路径（HPET 的 `vdso_clock_mode` 为 `NONE`，退化为系统调用并在内核中做 MMIO 读取），百万次调用的耗时通常会增加一到两个数量级。

若要观察 watchdog 降级，可在启动参数中加入 `tsc=unstable`，或在虚拟机中长时间暂停后恢复，再检查 `dmesg | grep -i 'unstable'`。

## 小结

- 时钟源是自由运行计数器的抽象，只提供 `read()`；`mult`/`shift` 把周期换算成纳秒，`clocks_calc_mult_shift()` 在不溢出的前提下选取精度最高的组合，64 位时钟源的换算区间封顶 600 秒。
- 注册时还会计算 `maxadj`（NTP 调频上限 11%）、`max_cycles`、`max_idle_ns`（两次读取最长间隔，取理论值一半）和 `max_raw_delta`（掩码 7/8，用于识别时间倒流）。
- `clocksource_list` 按 rating 降序排列；`fs_initcall` 之前不做选择；进入单次模式后只能选 `VALID_FOR_HRES` 的时钟源；切换经 `timekeeping_notify()` 在 `stop_machine()` 中原子完成。
- watchdog 用不带 `MUST_VERIFY` 的时钟源（通常是 HPET）每 0.5 秒检验带此标志的时钟源（通常是 TSC），通过则授予 `VALID_FOR_HRES`，失败则经 `kwatchdog` 线程把 rating 降为 0 并重选。
- `jiffies` 时钟源是启动时的兜底，依赖 tick 本身推进，不能用于高精度与 NO_HZ。

## 参考

- `linux/Documentation/timers/timekeeping.rst`
- `linux/Documentation/admin-guide/kernel-parameters.txt`（`clocksource=`、`tsc=` 参数）
- John Stultz, Nishanth Aravamudan, Darren Hart, "We Are Not Getting Any Younger: A New Approach to Time and Timers", OLS 2005（通用时间维护与 clocksource 框架的设计）
- `kernel/time/clocksource.c` 中 `WATCHDOG_MAX_SKEW` 前后的注释（watchdog 误差容限的设计说明）
