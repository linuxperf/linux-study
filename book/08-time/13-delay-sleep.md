# 8.13 内核中的延时与睡眠

> **本节源码**：[`include/asm-generic/delay.h`](../../linux/include/asm-generic/delay.h)、[`include/linux/delay.h`](../../linux/include/linux/delay.h)、[`arch/x86/lib/delay.c`](../../linux/arch/x86/lib/delay.c)、[`kernel/time/sleep_timeout.c`](../../linux/kernel/time/sleep_timeout.c)、[`include/linux/jiffies.h`](../../linux/include/linux/jiffies.h)、[`include/linux/wait.h`](../../linux/include/linux/wait.h)
> **关键函数**：`udelay()`、`ndelay()`、`mdelay()`、`__const_udelay()`、`delay_tsc()`、`delay_halt()`、`schedule_timeout()`、`msleep()`、`msleep_interruptible()`、`schedule_hrtimeout_range()`、`usleep_range_state()`、`fsleep()`
> **前置知识**：8.3 x86 时钟硬件（TSC 校准、`loops_per_jiffy`）、8.7 时间轮、8.9 高精度定时器、3.2 主调度函数（任务状态与睡眠）

## 概述

驱动程序经常需要"等一会儿"：硬件复位后等待 10 μs 再访问寄存器，固件加载后等待 50 ms 让设备就绪，轮询某个状态位，每隔 1 ms 检查一次。内核为此提供了两类截然不同的接口：

- **忙等延时**（`ndelay()`、`udelay()`、`mdelay()`）：CPU 原地循环直到时间过去，不让出 CPU。可以在任何上下文使用，包括持有自旋锁、关中断、中断处理函数中；代价是延时期间 CPU 什么也做不了。
- **睡眠**（`msleep()`、`usleep_range()`、`fsleep()`、`schedule_timeout()` 等）：把当前任务置为睡眠状态，设置一个定时器后调用调度器，到期时被唤醒。期间 CPU 可以运行其他任务或进入空闲；但只能在可睡眠的进程上下文中使用，并且实际睡眠时长总会比请求的长一些。

选错接口的后果都不轻：在原子上下文中睡眠会导致 "scheduling while atomic" 甚至死锁；在可睡眠的上下文中长时间忙等则浪费 CPU、增加调度延迟。本节分析两类接口在 x86 上的实现，以及它们的精度特性，最后给出选择建议。`linux/Documentation/timers/delay_sleep_functions.rst` 是这部分的官方指南。

## 忙等延时

### udelay() 与 ndelay()

[`include/asm-generic/delay.h:56-66`](../../linux/include/asm-generic/delay.h#L56-L66)（v6.18.60）

```c
static __always_inline void udelay(unsigned long usec)
{
	if (__builtin_constant_p(usec)) {
		if (usec >= DELAY_CONST_MAX)
			__bad_udelay();
		else
			__const_udelay(usec * UDELAY_CONST_MULT);
	} else {
		__udelay(usec);
	}
}
```

- 参数为编译期常量时，直接乘以 `UDELAY_CONST_MULT = ⌈2³² / 10⁶⌉`（[`include/asm-generic/delay.h:22-23`](../../linux/include/asm-generic/delay.h#L22-L23)），把微秒换算成以 2⁻³² 秒为单位的定点数，省去运行时乘法。
- 常量参数达到 `DELAY_CONST_MAX`（20000，即 20 ms）时引用一个不存在的函数 `__bad_udelay()`，在链接阶段报错——这么长的忙等应改用 `mdelay()` 或睡眠。
- `mdelay(n)` 在 `n ≤ MAX_UDELAY_MS`（5）时直接调用 `udelay(n * 1000)`，否则循环 `n` 次 `udelay(1000)`，避免定点数乘法溢出（[`include/linux/delay.h:44-46`](../../linux/include/linux/delay.h#L44-L46)）。

### 换算为循环次数

[`arch/x86/lib/delay.c:201-231`](../../linux/arch/x86/lib/delay.c#L201-L231)（v6.18.60）

```c
void __delay(unsigned long loops)
{
	delay_fn(loops);
}
EXPORT_SYMBOL(__delay);

noinline void __const_udelay(unsigned long xloops)
{
	unsigned long lpj = this_cpu_read(cpu_info.loops_per_jiffy) ? : loops_per_jiffy;
	int d0;

	xloops *= 4;
	asm("mull %%edx"
		:"=d" (xloops), "=&a" (d0)
		:"1" (xloops), "0" (lpj * (HZ / 4)));

	__delay(++xloops);
}
EXPORT_SYMBOL(__const_udelay);

void __udelay(unsigned long usecs)
{
	__const_udelay(usecs * 0x000010c7); /* 2**32 / 1000000 (rounded up) */
}
EXPORT_SYMBOL(__udelay);

void __ndelay(unsigned long nsecs)
{
	__const_udelay(nsecs * 0x00005); /* 2**32 / 1000000000 (rounded up) */
}
EXPORT_SYMBOL(__ndelay);
```

`loops_per_jiffy × HZ` 是每秒的"循环"数，乘以以 2⁻³² 秒为单位的延时，取 64 位乘积的高 32 位（`mull` 指令把结果的高半部分放在 `edx` 中），就得到需要的循环数。两边各乘、除以 4 是为了防止 `lpj × HZ` 在 32 位乘法中溢出。最后加 1 保证不会少等。

"循环"的含义取决于 `__delay()` 的具体实现。8.3 节的 `tsc_enable_sched_clock()` 把 `loops_per_jiffy` 设为 `tsc_khz × 1000 / HZ`，即每个 jiffy 的 TSC 周期数，并调用 `use_tsc_delay()`；此后"一个循环"就是"一个 TSC 周期"。

### 三种实现

[`arch/x86/lib/delay.c:62-97`](../../linux/arch/x86/lib/delay.c#L62-L97)（v6.18.60）

```c
/* TSC based delay: */
static void delay_tsc(u64 cycles)
{
	u64 bclock, now;
	int cpu;

	preempt_disable();
	cpu = smp_processor_id();
	bclock = rdtsc_ordered();
	for (;;) {
		now = rdtsc_ordered();
		if ((now - bclock) >= cycles)
			break;

		/* Allow RT tasks to run */
		preempt_enable();
		native_pause();
		preempt_disable();

		/*
		 * It is possible that we moved to another CPU, and
		 * since TSC's are per-cpu we need to calculate
		 * that. The delay must guarantee that we wait "at
		 * least" the amount of time. Being moved to another
		 * CPU could make the wait longer but we just need to
		 * make sure we waited long enough. Rebalance the
		 * counter for this CPU.
		 */
		if (unlikely(cpu != smp_processor_id())) {
			cycles -= (now - bclock);
			cpu = smp_processor_id();
			bclock = rdtsc_ordered();
		}
	}
	preempt_enable();
}
```

`delay_fn` 指针在启动时被设置为以下三者之一（[`arch/x86/lib/delay.c:174-190`](../../linux/arch/x86/lib/delay.c#L174-L190)）：

| 实现 | 启用条件 | 等待方式 |
|---|---|---|
| `delay_loop()` | 默认，TSC 不可用时 | 对齐的 `dec`/`jnz` 空循环，依赖 `calibrate_delay()` 测得的 BogoMIPS |
| `delay_tsc()` | `tsc_enable_sched_clock()` 中 `use_tsc_delay()` | 轮询 TSC，每次循环执行 `pause` |
| `delay_halt()` + `delay_halt_tpause()` | CPU 支持 `WAITPKG`（`x86_late_time_init()` 中 `use_tpause_delay()`） | `tpause` 指令进入 C0.2 轻度省电状态直到 TSC 达到目标值 |
| `delay_halt()` + `delay_halt_mwaitx()` | AMD 支持 `MWAITX`（`use_mwaitx_delay()`） | `mwaitx` 带 TSC 超时 |

*表 8-27：x86 上 `__delay()` 的实现*

`delay_tsc()` 有两处值得注意。一是**允许抢占**：每次循环之间短暂开启抢占，让高优先级任务有机会运行——这意味着在可抢占内核中，`udelay()` 本身并不禁止抢占，实际延时可能远长于请求值（调用者若持有自旋锁，抢占本来就是关闭的）。二是**处理 CPU 迁移**：若被抢占后迁移到另一个 CPU，各 CPU 的 TSC 可能略有差异，于是先扣除已等待的周期，再以新 CPU 的 TSC 为基准继续等待，保证"至少"等足请求的时间。

`tpause`（Intel，v5.8 起）与 `mwaitx`（AMD）让 CPU 在等待期间进入低功耗状态，并允许超线程兄弟获得更多执行资源，比 `pause` 自旋更节能。`delay_halt()` 用一个循环反复调用它们，因为这些指令可能被中断提前唤醒（[`arch/x86/lib/delay.c:149-172`](../../linux/arch/x86/lib/delay.c#L149-L172)）。

### 精度

注释中列出了忙等延时可能**提前**返回的几个原因（[`include/asm-generic/delay.h:48-54`](../../linux/include/asm-generic/delay.h#L48-L54)）：`loops_per_jiffy` 测得偏低、缓存行为影响循环速度、CPU 频率变化。这些主要针对 `delay_loop()`；基于 TSC 的实现直接以时间为准，在不变 TSC 的现代 CPU 上不会提前，但可能因中断、抢占而延后。

## 基于时间轮的睡眠

### schedule_timeout()

[`kernel/time/sleep_timeout.c:61-109`](../../linux/kernel/time/sleep_timeout.c#L61-L109)（v6.18.60，节选）

```c
signed long __sched schedule_timeout(signed long timeout)
{
	struct process_timer timer;
	unsigned long expire;

	switch (timeout) {
	case MAX_SCHEDULE_TIMEOUT:
		...
		schedule();
		goto out;
	default:
		...
		if (timeout < 0) {
			pr_err("%s: wrong timeout value %lx\n", __func__, timeout);
			dump_stack();
			__set_current_state(TASK_RUNNING);
			goto out;
		}
	}

	expire = timeout + jiffies;

	timer.task = current;
	timer_setup_on_stack(&timer.timer, process_timeout, 0);
	timer.timer.expires = expire;
	add_timer(&timer.timer);
	schedule();
	timer_delete_sync(&timer.timer);

	/* Remove the timer from the object tracker */
	timer_destroy_on_stack(&timer.timer);

	timeout = expire - jiffies;

 out:
	return timeout < 0 ? 0 : timeout;
}
```

这是内核中最基础的带超时睡眠：在栈上建立一个 `timer_list`，回调 `process_timeout()` 只做一件事——唤醒任务（[`kernel/time/sleep_timeout.c:18-28`](../../linux/kernel/time/sleep_timeout.c#L18-L28)）；然后调用 `schedule()`。被唤醒后用 `timer_delete_sync()` 确保定时器已删除且回调已结束（栈上的定时器即将失效），返回剩余的 jiffies。

调用者必须**先设置任务状态**再调用它，注释列出了三种情况（[`kernel/time/sleep_timeout.c:30-60`](../../linux/kernel/time/sleep_timeout.c#L30-L60)）：`TASK_RUNNING` 时 `schedule()` 不会让任务睡眠，相当于立即返回；`TASK_UNINTERRUPTIBLE` 时保证至少睡够时间（除非被显式唤醒）；`TASK_INTERRUPTIBLE` 时信号也能提前唤醒。`schedule_timeout_interruptible()`、`_killable()`、`_uninterruptible()`、`_idle()` 是设置好状态的便捷包装（[`kernel/time/sleep_timeout.c:125-177`](../../linux/kernel/time/sleep_timeout.c#L125-L177)）。`TASK_IDLE` 状态的睡眠不计入负载均值，适合长期休眠的内核线程。

`wait_event_timeout()`、`wait_for_completion_timeout()` 等带超时的等待原语，底层都是在循环中调用 `schedule_timeout()`（[`include/linux/wait.h:393-396`](../../linux/include/linux/wait.h#L393-L396)）。

### msleep()

[`kernel/time/sleep_timeout.c:313-319`](../../linux/kernel/time/sleep_timeout.c#L313-L319)（v6.18.60）

```c
void msleep(unsigned int msecs)
{
	unsigned long timeout = msecs_to_jiffies(msecs);

	while (timeout)
		timeout = schedule_timeout_uninterruptible(timeout);
}
```

`msleep()` 把毫秒向上取整为 jiffies（`HZ=1000` 时 1 ms = 1 jiffy，[`include/linux/jiffies.h:461-464`](../../linux/include/linux/jiffies.h#L461-L464)），以不可中断状态循环睡眠直到时间耗尽——即使被显式唤醒也会接着睡。`msleep_interruptible()` 在有信号待处理时提前返回剩余毫秒数（[`kernel/time/sleep_timeout.c:334-341`](../../linux/kernel/time/sleep_timeout.c#L334-L341)）。

由于底层是时间轮定时器，`msleep()` 的精度受 8.7 节分析的粒度影响。函数的文档注释专门给出了计算方法（[`kernel/time/sleep_timeout.c:282-312`](../../linux/kernel/time/sleep_timeout.c#L282-L312)）：

- 落入时间轮第 1 层及以上（`HZ=1000` 时约 64 ms 以上）的睡眠，最大额外延迟（slack）为 12.5%；
- 第 0 层的睡眠，slack 约为 `1 个 tick / msecs`，且不低于 12.5%。例如 `HZ=1000` 时，要求 slack 不超过 25% 的调用者应保证 `msecs ≥ 4`。

短时间的 `msleep()` 尤其不准：`msleep(1)` 换算为 1 jiffy，`calc_index()` 为保证不早到又加 1（8.7 节），定时器在下一个或再下一个 tick 到期，实际睡眠 1～2 ms；在 `HZ=100` 的内核上，`msleep(1)` 可能睡 10～20 ms。

## 基于 hrtimer 的睡眠

### schedule_hrtimeout_range()

[`kernel/time/sleep_timeout.c:189-224`](../../linux/kernel/time/sleep_timeout.c#L189-L224)（v6.18.60）

```c
int __sched schedule_hrtimeout_range_clock(ktime_t *expires, u64 delta,
					   const enum hrtimer_mode mode, clockid_t clock_id)
{
	struct hrtimer_sleeper t;

	/*
	 * Optimize when a zero timeout value is given. It does not
	 * matter whether this is an absolute or a relative time.
	 */
	if (expires && *expires == 0) {
		__set_current_state(TASK_RUNNING);
		return 0;
	}

	/*
	 * A NULL parameter means "infinite"
	 */
	if (!expires) {
		schedule();
		return -EINTR;
	}

	hrtimer_setup_sleeper_on_stack(&t, clock_id, mode);
	hrtimer_set_expires_range_ns(&t.timer, *expires, delta);
	hrtimer_sleeper_start_expires(&t, mode);

	if (likely(t.task))
		schedule();

	hrtimer_cancel(&t.timer);
	destroy_hrtimer_on_stack(&t.timer);

	__set_current_state(TASK_RUNNING);

	return !t.task ? 0 : -EINTR;
}
```

与 `schedule_timeout()` 结构相同，只是把时间轮定时器换成了 8.9 节的 `hrtimer_sleeper`，并可指定松弛 `delta`：定时器的软到期时间为 `expires`、硬到期时间为 `expires + delta`，内核可以在这个区间内任何合适的时刻唤醒任务，以便与其他已安排的中断合并（[`kernel/time/sleep_timeout.c:227-259`](../../linux/kernel/time/sleep_timeout.c#L227-L259)）。`select()`、`poll()`、`epoll_wait()` 的超时也走这个函数，松弛取自任务的 `timer_slack_ns`。

### usleep_range()

[`kernel/time/sleep_timeout.c:362-376`](../../linux/kernel/time/sleep_timeout.c#L362-L376)（v6.18.60）

```c
void __sched usleep_range_state(unsigned long min, unsigned long max, unsigned int state)
{
	ktime_t exp = ktime_add_us(ktime_get(), min);
	u64 delta = (u64)(max - min) * NSEC_PER_USEC;

	if (WARN_ON_ONCE(max < min))
		delta = 0;

	for (;;) {
		__set_current_state(state);
		/* Do not return before the requested sleep time has elapsed */
		if (!schedule_hrtimeout_range(&exp, delta, HRTIMER_MODE_ABS))
			break;
	}
}
```

`usleep_range(min, max)` 至少睡 `min` 微秒、最多 `max` 微秒，区间就是 hrtimer 的松弛。以绝对时间计算到期点，若被提前唤醒则继续睡到同一个时刻，保证总时长不少于 `min`。设置一个合理的区间（而不是 `min == max`）让内核有机会合并唤醒，文档注释特别强调这一点（[`kernel/time/sleep_timeout.c:344-361`](../../linux/kernel/time/sleep_timeout.c#L344-L361)）。`usleep_range_idle()` 以 `TASK_IDLE` 睡眠，不计入负载均值（[`include/linux/delay.h:90-93`](../../linux/include/linux/delay.h#L90-L93)）。

## fsleep()：自动选择

v5.9 引入、v6.13 重写的 `fsleep()` 根据时长自动选择最合适的机制，目标是 slack 不超过 25%：

[`include/linux/delay.h:106-135`](../../linux/include/linux/delay.h#L106-L135)（v6.18.60，节选）

```c
static const unsigned int max_slack_shift = 2;
#define USLEEP_RANGE_UPPER_BOUND	((TICK_NSEC << max_slack_shift) / NSEC_PER_USEC)
...
static inline void fsleep(unsigned long usecs)
{
	if (usecs <= 10)
		udelay(usecs);
	else if (usecs < USLEEP_RANGE_UPPER_BOUND)
		usleep_range(usecs, usecs + (usecs >> max_slack_shift));
	else
		msleep(DIV_ROUND_UP(usecs, USEC_PER_MSEC));
}
```

- **不超过 10 μs**：忙等。这么短的时间，设置 hrtimer、调度出去再调度回来的开销反而更大。
- **不足 4 个 tick**（`HZ=1000` 时为 4000 μs）：`usleep_range(usecs, usecs × 1.25)`，hrtimer 提供精确唤醒，允许 25% 松弛。
- **更长**：`msleep()`。时间轮在 4 个 tick 以上时 slack 不超过 25%，而且时间轮定时器比 hrtimer 开销更低。

注意 `fsleep()` 可能睡眠，只能在进程上下文中使用。

## 如何选择

| 场景 | 推荐接口 | 说明 |
|---|---|---|
| 原子上下文（持自旋锁、关中断、中断处理） | `ndelay()` / `udelay()` / `mdelay()` | 唯一选择；尽量缩短，考虑重构以避免长忙等 |
| 进程上下文，≤ 10 μs | `udelay()` 或 `fsleep()` | 睡眠的开销大于等待本身 |
| 进程上下文，10 μs ～ 约 20 ms | `usleep_range()` 或 `fsleep()` | 给出合理区间，让内核合并唤醒 |
| 进程上下文，≥ 约 20 ms，精度要求不高 | `msleep()` / `ssleep()` 或 `fsleep()` | 时间轮开销低，slack ≤ 12.5% |
| 需要被信号打断 | `msleep_interruptible()`、`schedule_timeout_interruptible()` | 返回剩余时间 |
| 等待条件成立，带超时 | `wait_event_timeout()`、`wait_for_completion_timeout()`、`read_poll_timeout()` | 不要自己写"睡一会儿再检查"的循环 |
| 不希望计入负载均值的长期等待 | `schedule_timeout_idle()`、`usleep_range_idle()` | `TASK_IDLE` |

*表 8-28：延时与睡眠接口的选择*

需要强调的是，**不确定是否处于原子上下文时，不能用睡眠接口**。开启 `CONFIG_DEBUG_ATOMIC_SLEEP` 后，`might_sleep()` 会在睡眠函数中捕获这类错误（7.1 节）；本书配置未开启该选项。

## 关键细节 / 设计考量

### 忙等与睡眠的分界

为什么分界点大约在 10 μs？一次完整的睡眠需要：设置定时器、调用 `schedule()` 切换到其他任务或 idle、到期中断、唤醒、再次调度回来。这些操作在现代 CPU 上合计数微秒，再加上 CPU 进入空闲后的唤醒延迟（深度 C-state 可达数十到上百微秒），对于很短的等待，睡眠反而让实际等待时间变得更长、更不确定。`udelay()` 注释中的"所有延迟不超过 10 微秒"就是对这一分界的经验估计（[`include/asm-generic/delay.h:38-42`](../../linux/include/asm-generic/delay.h#L38-L42)）。

### 睡眠总是"至少"，从不"恰好"

无论哪种睡眠接口，内核都只保证"不早于"请求的时间唤醒，晚多少取决于定时器粒度、松弛、中断延迟以及唤醒后能否立即获得 CPU。实时任务（`SCHED_FIFO`/`SCHED_RR`）的 hrtimer 松弛为 0，且唤醒后能立即抢占普通任务，延迟最小；普通任务在负载较重时可能晚数毫秒才真正运行。驱动程序若需要严格的时序，应依赖硬件（DMA、硬件定时器）而不是软件睡眠。

### 轮询的推荐写法

"写寄存器 → 循环等待状态位，每次睡一会儿 → 超时报错"是驱动中极常见的模式。`include/linux/iopoll.h` 中的 `read_poll_timeout()`、`readx_poll_timeout()` 系列宏封装了这一模式：内部用 `usleep_range()`（或原子版本中的 `udelay()`）实现间隔，用 `ktime_get()` 判断超时，比手写循环更不容易出错。

## 动手实验

以下内核模块在加载时测量几种延时与睡眠接口的实际耗时（需要在 dev 虚拟机中安装与运行内核匹配的头文件后编译）：

```c
// delay_test.c
#include <linux/module.h>
#include <linux/delay.h>
#include <linux/ktime.h>

#define MEASURE(desc, stmt) do {				\
	ktime_t t0 = ktime_get();				\
	stmt;							\
	pr_info("%-28s %8lld us\n", desc,			\
		ktime_us_delta(ktime_get(), t0));		\
} while (0)

static int __init delay_test_init(void)
{
	MEASURE("udelay(100)", udelay(100));
	MEASURE("msleep(1)", msleep(1));
	MEASURE("msleep(10)", msleep(10));
	MEASURE("usleep_range(1000, 1100)", usleep_range(1000, 1100));
	MEASURE("usleep_range(1000, 2000)", usleep_range(1000, 2000));
	MEASURE("fsleep(500)", fsleep(500));
	MEASURE("fsleep(20000)", fsleep(20000));
	return 0;
}

static void __exit delay_test_exit(void) { }

module_init(delay_test_init);
module_exit(delay_test_exit);
MODULE_LICENSE("GPL");
```

```bash
cat > Makefile <<'EOF'
obj-m += delay_test.o
EOF
make -C /lib/modules/$(uname -r)/build M=$PWD modules
sudo insmod delay_test.ko && sudo rmmod delay_test && dmesg | tail -n 7

# 观察系统中谁在频繁调用 schedule_timeout 及其超时分布
sudo bpftrace -e 'kprobe:schedule_timeout { @to[kstack(3)] = lhist(arg0, 0, 1000, 50); } interval:s:10 { exit(); }'
```

典型结果是：`udelay(100)` 约为 100 μs；`msleep(1)` 介于 1000～2000 μs；`usleep_range(1000, 1100)` 接近 1000～1100 μs；`usleep_range(1000, 2000)` 若系统中恰好有其他定时器可以合并，可能在 1000 μs 稍后就返回，否则接近 2000 μs；`fsleep(20000)` 走 `msleep(20)`，结果略大于 20 ms。虚拟机中受 vCPU 调度影响，抖动会更大。

## 小结

- 忙等延时 `ndelay()`/`udelay()`/`mdelay()` 可用于任何上下文，在 x86 上以 `loops_per_jiffy`（即每 jiffy 的 TSC 周期数）把时间换算为循环数，再由 `delay_tsc()`、`tpause` 或 `mwaitx` 等实现等待；常量参数过大会在链接时报错。
- `delay_tsc()` 在循环间允许抢占并正确处理 CPU 迁移，保证"至少"等足时间，但可能被抢占而延长。
- `schedule_timeout()` 用栈上的时间轮定时器实现带超时睡眠，是 `msleep()`、`wait_event_timeout()` 等的基础；受时间轮粒度影响，`msleep()` 的最大 slack 约为 12.5%，短时间睡眠尤其不准。
- `schedule_hrtimeout_range()` 与 `usleep_range()` 基于 hrtimer，以 `[min, max]` 区间作为松弛以合并唤醒，适合 10 μs 至约 20 ms 的睡眠。
- `fsleep()` 按时长自动在 `udelay()`、`usleep_range()`、`msleep()` 之间选择，使 slack 不超过 25%；所有睡眠接口都只能在进程上下文使用，且只保证不早于请求的时间唤醒。

## 参考

- `linux/Documentation/timers/delay_sleep_functions.rst`（延时与睡眠函数的官方使用指南）
- `kernel/time/sleep_timeout.c` 中 `msleep()` 的文档注释（slack 的计算方法）
- `include/linux/iopoll.h`（`read_poll_timeout()` 系列宏）
- Intel® SDM, "TPAUSE—Timed PAUSE"；AMD APM, "MWAITX"
