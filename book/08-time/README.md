# 第 8 章 时间与定时器

本章分析 Linux v6.18.60 在 x86_64 上的时间子系统，源码主要位于 `kernel/time/`、`arch/x86/kernel/`（`tsc.c`、`tsc_sync.c`、`hpet.c`、`kvmclock.c`、`apic/apic.c`）、`lib/vdso/`、`arch/x86/entry/vdso/` 与 `fs/timerfd.c`。

内核对时间的需求可以归结为三个问题：现在几点（时间维护）、过多久做某事（定时器）、谁按时打断 CPU（时钟事件与 tick）。本章自底向上展开：先看两类时钟硬件抽象——只读的时钟源与可编程的时钟事件设备，以及 x86 上的 TSC、HPET、LAPIC 定时器与 kvm-clock；再看建立在时钟源之上的 timekeeping、NTP 调频与 RTC 同步；然后是两套定时器——以 jiffy 为单位、面向超时的时间轮，与以纳秒为单位、精确到期的 hrtimer，以及让空闲 CPU 把全局定时器交给他人代管的迁移层次；接着分析 tick 如何由 hrtimer 模拟、又如何在空闲或单任务运行时停下；最后是用户态接口：POSIX 时钟与定时器、vDSO 与时间命名空间，以及驱动中常用的延时与睡眠函数。

```
 用户态       8.11 POSIX 时钟 / 定时器 / itimer / timerfd        8.12 vDSO 与时间命名空间        8.13 延时与睡眠
                     │                    │                              │                          │
 ────────────────────┼────────────────────┼──────────────────────────────┼──────────────────────────┼──────
 时间维护     8.4 timekeeper ◀── 8.5 NTP 调频 / 闰秒 / RTC        ▲ update_vsyscall()                 │
                     ▲ update_wall_time()                                                           │
 定时器              │                    8.9 hrtimer（红黑树）◀──────────────────────────────────────┤
                     │                         ▲            8.7 时间轮 timer_list ◀── 8.8 迁移层次 ◀──┘
 tick 层      8.10 tick 模拟 / NO_HZ ──────────┘ sched_timer        ▲ TIMER_SOFTIRQ
                     ▲ event_handler（hrtimer_interrupt / tick_handle_periodic）
 硬件抽象     8.2 clocksource 框架 + watchdog          8.6 clockevents / tick 设备 / tick 广播
                     ▲ read()                                 ▲ set_next_event()
 x86 硬件     8.3 TSC / HPET / kvm-clock                LAPIC 定时器（TSC-deadline）/ HPET
                     └────────────── 8.1 概览：分层、时间表示、jiffies、初始化顺序 ──────────────┘
```

*图 8-0：本章各小节的关系*

## 阅读建议

- 8.1 是全章导读，给出分层结构、各种时间表示与启动时的初始化顺序，建议先读。
- 8.2～8.5 是"读时间"一侧：8.2、8.3 讲时钟源的抽象与 x86 实现，8.4 是全章的核心之一，8.5 依赖 8.4 中的 `mult`、`ntp_error` 等概念。
- 8.6～8.10 是"定时中断"一侧：8.6 讲硬件抽象与 tick 设备，8.7 与 8.9 分别是两套定时器，8.8 是 v6.9 引入的时间轮迁移机制，8.10 把它们串起来，讲 tick 的模拟与停止。8.8 与 8.10 联系紧密，可对照阅读。
- 8.11～8.13 面向接口：8.11、8.12 讲用户态 API 的内核实现，8.13 讲驱动程序中的延时与睡眠，可以在需要时单独查阅。
- 第 3 章 3.5 节从调度器角度分析了 tick、hrtick 与 `NO_HZ_FULL` 的 tick 依赖，第 5 章 5.5 节介绍了 LAPIC 定时器的编程方式，第 7 章 7.5 节分析了 timekeeping 使用的 latch 顺序计数，本章在相应位置引用。

## 本章配置

本章以 `linux/.config` 为准，与时间子系统相关的主要配置项如下：

| 配置项 | 取值 | 影响 |
|---|---|---|
| `CONFIG_HZ` / `CONFIG_HZ_1000` | 1000 / y | 1 jiffy = 1 ms；时间轮 9 层；`TICK_NSEC` = 1000000 |
| `CONFIG_HIGH_RES_TIMERS` / `CONFIG_TICK_ONESHOT` | y / y | 高精度模式，tick 由 hrtimer 模拟 |
| `CONFIG_NO_HZ_COMMON` / `CONFIG_NO_HZ_FULL` / `CONFIG_NO_HZ_IDLE` | y / y / n | 空闲停 tick；`nohz_full=` 指定的 CPU 运行时也可停 tick |
| `CONFIG_GENERIC_CLOCKEVENTS_BROADCAST` / `_BROADCAST_IDLE` | y / y | 支持 tick 广播，空闲循环可调用 `tick_broadcast_enter()` |
| `CONFIG_GENERIC_CLOCKEVENTS_MIN_ADJUST` | y | 时钟事件设备的最小编程间隔可自适应增大 |
| `CONFIG_CLOCKSOURCE_WATCHDOG` / `_MAX_SKEW_US` | y / 100 | 启用时钟源 watchdog，每个时钟源的误差容限下限为 200 μs |
| `CONFIG_X86_TSC` / `CONFIG_HPET_TIMER` / `CONFIG_X86_PM_TIMER` | y / y / y | TSC、HPET、ACPI PM 定时器 |
| `CONFIG_PARAVIRT_CLOCK` / `CONFIG_KVM_GUEST` / `CONFIG_HYPERV_TIMER` | y / y / y | kvm-clock、Hyper-V 时钟源与 vDSO 支持 |
| `CONFIG_HAVE_UNSTABLE_SCHED_CLOCK` | y | `sched_clock_cpu()` 在 TSC 不稳定时按 CPU 修正（3.5 节） |
| `CONFIG_POSIX_TIMERS` / `CONFIG_POSIX_CPU_TIMERS_TASK_WORK` | y / y | POSIX 定时器；CPU 时间定时器在任务工作中到期 |
| `CONFIG_POSIX_AUX_CLOCKS` | y | v6.17 的辅助时钟（`CLOCK_AUX`） |
| `CONFIG_TIMERFD` / `CONFIG_TIME_NS` | y / y | timerfd 与时间命名空间 |
| `CONFIG_GENERIC_GETTIMEOFDAY` / `CONFIG_GENERIC_VDSO_OVERFLOW_PROTECT` | y / y | 通用 vDSO 实现，带乘法溢出保护 |
| `CONFIG_LEGACY_VSYSCALL_XONLY` | y | 传统 vsyscall 页只能执行，由内核模拟 |
| `CONFIG_COMPAT_32BIT_TIME` | y | 保留 32 位 `time_t` 的兼容系统调用 |
| `CONFIG_NTP_PPS` | n | 内核不直接处理 PPS 信号 |
| `CONFIG_RTC_HCTOSYS` / `CONFIG_RTC_SYSTOHC` / `CONFIG_GENERIC_CMOS_UPDATE` | y / y / y | 启动时从 `rtc0` 读时间；NTP 同步后每 11 分钟写回 RTC |
| `CONFIG_RTC_DRV_CMOS` | y | MC146818 兼容的 CMOS RTC 驱动 |
| `CONFIG_CPU_IDLE` / `CONFIG_INTEL_IDLE` / `CONFIG_CPU_IDLE_GOV_TEO` | y / y / y | cpuidle 管理器参与停 tick 的决策 |
| `CONFIG_RCU_NOCB_CPU` | y | 卸载 RCU 回调的 CPU 更容易长时间停 tick |
| `CONFIG_VIRT_CPU_ACCOUNTING_GEN` / `CONFIG_IRQ_TIME_ACCOUNTING` | y / n | nohz_full CPU 上使用 vtime 精确记账 |
| `CONFIG_SCHED_HRTICK` | y | 调度器可使用 hrtick（默认特性位关闭，3.5 节） |
| `CONFIG_PREEMPT_RT` | n | hrtimer 默认在硬中断中到期；时间轮无到期锁 |

## 小节索引

| 小节 | 状态 |
|---|---|
| [8.1 时间子系统概览：硬件、分层与时间表示](01-overview.md) | 已完成 |
| [8.2 时钟源框架：clocksource](02-clocksource.md) | 已完成 |
| [8.3 x86 时钟硬件：TSC、HPET 与 kvm-clock](03-x86-clock-hw.md) | 已完成 |
| [8.4 时间维护：timekeeper 与读时接口](04-timekeeping.md) | 已完成 |
| [8.5 NTP 调频、闰秒与 RTC 同步](05-ntp-rtc.md) | 已完成 |
| [8.6 时钟事件设备与 tick 设备](06-clockevents.md) | 已完成 |
| [8.7 时间轮定时器：timer_list](07-timer-wheel.md) | 已完成 |
| [8.8 定时器迁移层次：空闲 CPU 的全局定时器](08-timer-migration.md) | 已完成 |
| [8.9 高精度定时器：hrtimer](09-hrtimer.md) | 已完成 |
| [8.10 tick 模拟与无 tick 模式](10-tick-nohz.md) | 已完成 |
| [8.11 POSIX 时钟与定时器接口](11-posix-timers.md) | 已完成 |
| [8.12 vDSO 与时间命名空间](12-vdso-timens.md) | 已完成 |
| [8.13 内核中的延时与睡眠](13-delay-sleep.md) | 已完成 |

## 待完善

- [ ] 各小节"动手实验"尚未在 dev 虚拟机中实测，需补充真实输出示例（注明内核版本、vCPU 数、虚拟化平台与当前时钟源）；VirtualBox 下可用的时钟源（`tsc`、`kvm-clock`、`hpet`、`acpi_pm`）取决于虚拟机的半虚拟化接口设置。
- [ ] 8.3 的 TSC 同步测试、TSC_ADJUST 修正与 tick 广播（8.6）需要多路物理机或不支持 ARAT 的 CPU 才能观察，虚拟机中难以复现。
- [ ] 8.5 未展开 `CONFIG_NTP_PPS` 下的 `hardpps()` 与 PPS 内核消费者，以及 PTP 硬件时钟（`drivers/ptp/`）与 `get_device_system_crosststamp()` 的交叉时间戳机制。
- [ ] 8.6 未展开 HPET 每通道 MSI 时钟事件设备与 `tick_oneshot_wakeup_device` 机制，以及 `tick_freeze()`/`tick_unfreeze()` 在 suspend-to-idle 中的作用。
- [ ] 8.11 对 POSIX CPU 时间定时器的 `handle_posix_cpu_timers()` 内部流程、`RLIMIT_CPU`/`RLIMIT_RTTIME` 的检查与 SIG_IGN 定时器的处理只做了概述，可另行展开。
- [ ] 部分历史版本号（如非级联时间轮 v4.8、定时器迁移层次 v6.9、cpuidle 参与停 tick 决策 v4.17、多粒度时间戳 v6.13、辅助时钟 v6.17、`fsleep()` 重写 v6.13、watchdog 误差容限调整 v5.14 前后）依据社区资料整理，后续可补充对应的提交号以便核对。
- [ ] 若干代码块（如 [`kernel/time/timer.c:1017-1095`](../../linux/kernel/time/timer.c#L1017-L1095)、[`kernel/time/timekeeping.c:2328-2387`](../../linux/kernel/time/timekeeping.c#L2328-L2387)、[`kernel/time/tick-sched.c:970-1071`](../../linux/kernel/time/tick-sched.c#L970-L1071)、[`kernel/time/hrtimer.c:1612-1652`](../../linux/kernel/time/hrtimer.c#L1612-L1652)、[`kernel/time/posix-timers.c:466-572`](../../linux/kernel/time/posix-timers.c#L466-L572)）超过 40 行建议值，可进一步拆分讲解。
- [ ] 图 8-2、8-4、8-6、8-7、8-8、8-10、8-12、8-13、8-15、8-16、8-18 的 Mermaid 图需在目标渲染器中确认显示效果。
- [ ] `Documentation/ABI/stable/sysfs-kernel-time-aux-clocks` 记载的属性名（`enable`）与代码（`aux_clock_enable`）不一致，若上游修正需同步更新 8.4 节。
