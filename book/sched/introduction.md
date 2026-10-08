# 调度子系统概述：调度类、运行队列与 `__schedule()`

一台 8 核机器上同时存在几百个线程：编译器进程把 CPU 跑满，`sshd` 大部分时间在等网络包，音频线程每 5 ms 必须拿到一次 CPU，内核的 `kworker` 和 `ksoftirqd` 时不时要处理后台工作。CPU 只有 8 个，内核必须持续回答三个问题：

- **什么时候换人？** 当前任务主动睡眠、时间片用完、或者有更重要的任务被唤醒时。
- **换给谁？** 实时任务优先于普通任务；普通任务之间按 nice 值分配 CPU 时间。
- **在哪个 CPU 上运行？** 被唤醒的任务放到哪个 CPU；某些 CPU 太忙、另一些空闲时，要不要把任务挪过去。

调度子系统（scheduler）负责回答这三个问题，并完成真正的切换：保存当前任务的寄存器和栈，换上下一个任务的地址空间和寄存器。

本章是调度子系统的总览，回答以下问题：

1. 调度器分成哪几层？“核心层”和“调度类”各自负责什么？
2. 有哪些核心对象？任务、运行队列、调度类、调度实体和调度域之间是什么关系？
3. 一次调度怎样选出下一个任务，又怎样完成上下文切换？
4. 任务怎样睡眠、怎样被唤醒？`need_resched` 标志怎样变成一次真正的切换？
5. 不同调度类（stop、deadline、实时、公平、idle）分别用什么算法挑选任务？

每个调度类的内部细节、负载均衡算法放到后续章节展开，本章只建立主线，并给出每个结论对应的源码位置。

## 0. 分析基线

本章依据仓库内 [Makefile 第 2～4 行](../../linux/Makefile#L2-L4)标记的 **6.18.52** 版本，架构为 **x86-64**。读者需要了解中断与软中断的基本执行路径（[中断子系统概述](../interrupt/overview.md)、[softirq 机制](../interrupt/softirq.md)）、周期性 tick 的来源（[时间子系统概述](../time/introduction.md)），以及自旋锁和内存屏障的基本概念（[锁机制基础](../lock/introduction.md)）。

与本章结论有关的配置如下。它们是编译条件，不代表某次运行一定经过对应路径。

| 配置 | 对本章的影响 | 依据 |
| --- | --- | --- |
| `CONFIG_X86_64=y`、`CONFIG_SMP=y`、`CONFIG_NR_CPUS=512` | 多 CPU；每个 CPU 有一个运行队列 | [.config#L333](../../linux/.config#L333)、[.config#L362](../../linux/.config#L362)、[.config#L431](../../linux/.config#L431) |
| `CONFIG_PREEMPT_DYNAMIC=y`，抢占模型选 `CONFIG_PREEMPT_VOLUNTARY=y` | 抢占基础设施（`CONFIG_PREEMPTION=y`、`CONFIG_PREEMPT_COUNT=y`）全部编入，但**启动后默认按 voluntary 模型运行**；可用启动参数 `preempt=none/voluntary/full/lazy` 改变（`lazy` 依赖 `CONFIG_ARCH_HAS_PREEMPT_LAZY=y`），由于 `CONFIG_DEBUG_FS=y`，运行时还可写 debugfs 的 `sched/preempt` 切换（3.5 节） | [.config#L133-L142](../../linux/.config#L133-L142)、[Kconfig.preempt#L126-L146](../../linux/kernel/Kconfig.preempt#L126-L146)、[sched_dynamic_mode()（core.c#L7671-L7690）](../../linux/kernel/sched/core.c#L7671-L7690)、[debug.c#L503-L505](../../linux/kernel/sched/debug.c#L503-L505)、[.config#L10544](../../linux/.config#L10544) |
| `CONFIG_SCHED_CLASS_EXT` 未出现在 `.config` 中 | 它依赖 `DEBUG_INFO_BTF`；当前 `CONFIG_PAHOLE_VERSION=0` 不满足 BTF 所需的 `PAHOLE_VERSION >= 116`，因此 BTF 和 sched_ext 均未编入；`scx_enabled()` 恒为 `false`，系统中只有 5 个调度类 | [.config#L26](../../linux/.config#L26)、[Kconfig.debug#L377-L383](../../linux/lib/Kconfig.debug#L377-L383)、[Kconfig.preempt#L166-L168](../../linux/kernel/Kconfig.preempt#L166-L168)、[sched.h#L1784-L1785](../../linux/kernel/sched/sched.h#L1784-L1785) |
| `CONFIG_SCHED_PROXY_EXEC` 未设置 | `rq->curr` 和 `rq->donor` 是同一个联合体成员，`task_is_blocked()` 恒为 `false`（`sched_proxy_exec()` 是返回 `false` 的桩函数），本章不讨论代理执行 | [.config#L194](../../linux/.config#L194)、[sched.h#L1171-L1179](../../linux/kernel/sched/sched.h#L1171-L1179)、[sched.h#L2305-L2311](../../linux/kernel/sched/sched.h#L2305-L2311)、[linux/sched.h#L1687-L1692](../../linux/include/linux/sched.h#L1687-L1692) |
| `CONFIG_SCHED_CORE` 未设置 | `pick_next_task()` 直接调用 `__pick_next_task()`，没有 SMT 兄弟核之间的协同选择 | [.config#L143](../../linux/.config#L143)、[core.c#L6518-L6522](../../linux/kernel/sched/core.c#L6518-L6522) |
| `CONFIG_FAIR_GROUP_SCHED=y`、`CONFIG_CFS_BANDWIDTH=y`、`CONFIG_SCHED_AUTOGROUP=y`，`CONFIG_RT_GROUP_SCHED` 未设置 | 公平调度类支持组调度；即使不使用 cgroup，autogroup 也会让任务进入按会话建立的任务组（2.5 节）。本章只指出组调度在数据结构上的位置，细节见 [cgroup v2 的 cpu 控制器](../cgroup2/cpu.md) | [.config#L216-L221](../../linux/.config#L216-L221)、[.config#L245](../../linux/.config#L245) |
| `CONFIG_UCLAMP_TASK` 未设置 | 没有利用率钳制，`uclamp_*()` 调用为空 | [.config#L193](../../linux/.config#L193) |
| `CONFIG_HZ=1000`、`CONFIG_NO_HZ_FULL=y`、`CONFIG_SCHED_HRTICK=y` | 周期 tick 的标称间隔为 1 ms；`nohz_full=` 指定的 CPU 在满足调度类、带宽和其他 tick 依赖条件时可以停 tick，只有一个可运行任务并不充分；HRTICK 虽然编入，但调度特性 `HRTICK` 默认关闭 | [.config#L104-L108](../../linux/.config#L104-L108)、[.config#L505-L507](../../linux/.config#L505-L507)、[sched_can_stop_tick()（core.c#L1350-L1400）](../../linux/kernel/sched/core.c#L1350-L1400)、[can_stop_full_tick()（tick-sched.c#L358-L377）](../../linux/kernel/time/tick-sched.c#L358-L377)、[features.h#L66-L67](../../linux/kernel/sched/features.h#L66-L67) |
| `CONFIG_MMU_LAZY_TLB_REFCOUNT=y` | 借用 `active_mm` 的 lazy TLB 路径实际增加、归还 `mm_count` 引用（4.3 节） | [.config#L899](../../linux/.config#L899)、[sched/mm.h#L88-L112](../../linux/include/linux/sched/mm.h#L88-L112) |
| `CONFIG_SCHED_SMT=y`、`CONFIG_SCHED_CLUSTER=y`、`CONFIG_SCHED_MC=y`、`CONFIG_NUMA=y` | x86 拓扑表为 SMT → CLS → MC → PKG，`sched_init_numa()` 在其后追加 NODE 层和若干 NUMA 层；实际层级在启动时按硬件裁剪，不起作用的层会被删除（2.6 节） | [.config#L835-L837](../../linux/.config#L835-L837)、[.config#L469](../../linux/.config#L469) |
| `CONFIG_PARAVIRT_TIME_ACCOUNTING=y`，`CONFIG_IRQ_TIME_ACCOUNTING` 未设置 | 作为虚拟机运行且开启 steal time 时，被宿主机“偷走”的时间不计入任务运行时间；中断时间不单独扣除 | [.config#L148-L150](../../linux/.config#L148-L150)、[.config#L397](../../linux/.config#L397) |
| `CONFIG_SCHEDSTATS=y`、`CONFIG_PSI=y`、`CONFIG_CPU_FREQ_GOV_SCHEDUTIL=y` | 调度统计、压力统计和 schedutil 调频都挂在调度路径上；本章只提到调用点 | [.config#L10650](../../linux/.config#L10650)、[.config#L158](../../linux/.config#L158)、[.config#L696](../../linux/.config#L696) |

为便于区分同名文件，下文链接文字中的 `sched.h` 指 `kernel/sched/sched.h`，`linux/sched.h` 指 `include/linux/sched.h`，`asm/preempt.h` 指 x86 的 `arch/x86/include/asm/preempt.h`，`linux/preempt.h` 指通用的 `include/linux/preempt.h`。

关于抢占模型需要提前说明一点：`CONFIG_PREEMPT_VOLUNTARY=y` 在 `PREEMPT_DYNAMIC` 下只是“默认值”。[preempt_dynamic_init()（core.c#L7789-L7805）](../../linux/kernel/sched/core.c#L7789-L7805) 在没有 `preempt=` 启动参数时调用 `sched_dynamic_update(preempt_dynamic_voluntary)`；有参数时由 [setup_preempt_mode()（core.c#L7776-L7787）](../../linux/kernel/sched/core.c#L7776-L7787) 提前设好。启动之后，写入 debugfs 的 `sched/preempt` 文件会经 [sched_dynamic_write()（debug.c#L220-L237）](../../linux/kernel/sched/debug.c#L220-L237) 再次调用 `sched_dynamic_update()`，在运行时切换模型。静态分析无法确定某台机器的启动参数和运行时操作，本章以默认的 voluntary 模型为主线。

## 1. 调度子系统要解决什么问题

### 1.1 三个问题，三组机制

| 问题 | 机制 | 主要入口 |
| --- | --- | --- |
| 什么时候换人 | 任务主动阻塞时调用 `schedule()`；其他情况先设置 `TIF_NEED_RESCHED`，等到下一个**抢占点**再进入 `__schedule()` | [schedule()](../../linux/kernel/sched/core.c#L7048-L7060)、[resched_curr()](../../linux/kernel/sched/core.c#L1155-L1158) |
| 换给谁 | 按**调度类**（scheduling class）的优先级从高到低询问，第一个给出任务的类胜出；类内部用各自的算法挑选 | [__pick_next_task()](../../linux/kernel/sched/core.c#L5972-L6023) |
| 在哪个 CPU 上运行 | 唤醒、fork、exec 时由调度类的 `select_task_rq` 选 CPU；运行期间由负载均衡在 CPU 之间迁移任务 | [select_task_rq()](../../linux/kernel/sched/core.c#L3583-L3608)、[sched_balance_trigger()](../../linux/kernel/sched/fair.c#L13257-L13270) |

“什么时候换人”和“换给谁”是分开的。`__schedule()` 源码前的注释（[core.c#L6778-L6815](../../linux/kernel/sched/core.c#L6778-L6815)）列出了进入调度器的三种方式：显式阻塞；在中断返回、返回用户态等路径上检查 `TIF_NEED_RESCHED`；以及唤醒。注释特别指出，唤醒本身并不进入 `schedule()`，它只是把任务放回运行队列，必要时设置 `TIF_NEED_RESCHED`，真正的切换发生在“最近的可能时机”。

### 1.2 调度策略与调度类

用户通过 `sched_setattr()`、`sched_setscheduler()`、`nice()` 等系统调用为任务指定**调度策略**（policy）。内核把策略映射到**调度类**，每个调度类是一组实现了相同接口的函数。当前配置下有 5 个调度类，按优先级从高到低为：

| 调度类 | 对应的用户策略 | 用途 | 类内的选择规则 |
| --- | --- | --- | --- |
| `stop_sched_class` | 无（内核内部） | 每个 CPU 一个 stopper 线程，用于 `stop_machine`、迁移正在运行的任务等，抢占一切且不可被抢占 | 运行队列上只有 `rq->stop` 一个任务 |
| `dl_sched_class` | `SCHED_DEADLINE` | 按运行预算、相对截止时间和周期提供受准入控制约束的预约 | 绝对截止时间最早者（EDF，Earliest Deadline First） |
| `rt_sched_class` | `SCHED_FIFO`、`SCHED_RR` | 固定优先级实时任务 | 最高优先级队列的队首；RR 有时间片 |
| `fair_sched_class` | `SCHED_NORMAL`、`SCHED_BATCH`、`SCHED_IDLE` | 普通任务，按权重分享 CPU | EEVDF：在“应得服务”的任务中选虚拟截止时间最早者 |
| `idle_sched_class` | 无（内核内部） | 每个 CPU 一个 idle 任务，没有其他任务时运行 | 总是返回 `rq->idle` |

这 5 个类分别定义在 [stop_task.c#L96-L117](../../linux/kernel/sched/stop_task.c#L96-L117)、[deadline.c#L3307-L3339](../../linux/kernel/sched/deadline.c#L3307-L3339)、[rt.c#L2578-L2615](../../linux/kernel/sched/rt.c#L2578-L2615)、[fair.c#L14095-L14142](../../linux/kernel/sched/fair.c#L14095-L14142) 和 [idle.c#L546-L568](../../linux/kernel/sched/idle.c#L546-L568)。策略常量定义在 [uapi/linux/sched.h#L114-L121](../../linux/include/uapi/linux/sched.h#L114-L121)，其中 `SCHED_EXT = 7`；由于 sched_ext 没有编入，[normal_policy()（sched.h#L192-L199）](../../linux/kernel/sched/sched.h#L192-L199) 不把 `SCHED_EXT` 当作普通策略，[valid_policy()（sched.h#L216-L220）](../../linux/kernel/sched/sched.h#L216-L220) 因此不接受它。

策略到类的映射由 [__setscheduler_class()（core.c#L7304-L7318）](../../linux/kernel/sched/core.c#L7304-L7318) 完成，它看的是**优先级数值**而不是策略本身：`dl_prio(prio)` 为真则是 deadline 类，`rt_prio(prio)` 为真则是实时类，否则是公平类。

### 1.3 一个统一的优先级数轴

内核用一个整数 `prio` 把三类任务放在同一个数轴上，数值越小优先级越高（[prio.h#L9-L28](../../linux/include/linux/sched/prio.h#L9-L28)）：

```text
  prio:   -1    0 ........... 98   99  100 ........ 120 ........ 139
        |----|------------------|----|-----------------------------|
        DEADLINE   实时（RT）              普通任务（nice -20 .. +19）
              rt_priority 99 .. 1             nice 0 对应 120
```

换算规则在 [__normal_prio()（syscalls.c#L19-L31）](../../linux/kernel/sched/syscalls.c#L19-L31)：

- `SCHED_DEADLINE`：`prio = MAX_DL_PRIO - 1 = -1`；
- `SCHED_FIFO`/`SCHED_RR`：`prio = MAX_RT_PRIO - 1 - rt_priority`，用户设置的 `rt_priority` 越大，`prio` 越小。`rt_priority` 取 1～99，对应 `prio` 98～0；`prio` 99 虽然落在实时区间内，但不会由实时策略算出；
- 其他策略：`prio = NICE_TO_PRIO(nice) = nice + 120`。

判断函数 [rt_prio()（rt.h#L9-L12）](../../linux/include/linux/sched/rt.h#L9-L12) 和 [dl_prio()（deadline.h#L13-L16）](../../linux/include/linux/sched/deadline.h#L13-L16) 只比较数值区间。

### 1.4 分层架构

下面这张图回答“调度器由哪些部分组成、彼此怎样依赖”。实线箭头表示调用路径的概览（可省略中间函数），虚线箭头表示包含或指向的数据。图中省略了统计、PSI、cpufreq 等旁路。

```mermaid
flowchart TB
    subgraph TRIG["触发源"]
        T1["阻塞：wait_event / mutex / sleep"]
        T2["唤醒：wake_up / complete / 信号"]
        T3["tick：update_process_times"]
        T4["抢占点：返回用户态 / 中断返回 / cond_resched"]
        T5["fork / exec / exit / sched_setattr"]
    end

    subgraph CORE["核心层 kernel/sched（core.c / syscalls.c）"]
        C1["schedule / __schedule"]
        C2["try_to_wake_up"]
        C3["sched_tick"]
        C4["wake_up_new_task / sched_exec / do_task_dead"]
        C5["resched_curr：设置 need_resched"]
        C6["context_switch"]
        C7["__sched_setscheduler（syscalls.c）"]
    end

    subgraph SCLS["调度类接口 struct sched_class"]
        S1["stop"]
        S2["deadline"]
        S3["rt"]
        S4["fair"]
        S5["idle"]
    end

    subgraph DATA["每 CPU 数据"]
        RQ["struct rq"]
        Q1["dl_rq"]
        Q2["rt_rq"]
        Q3["cfs_rq"]
        SD["sched_domain 层级"]
    end

    subgraph ARCH["x86 架构层"]
        A1["switch_mm_irqs_off"]
        A2["switch_to → __switch_to_asm"]
    end

    T1 --> C1
    T4 --> C1
    T2 --> C2
    T3 --> C3
    T5 --> C4
    T5 --> C7
    C2 --> C5
    C3 --> C5
    C1 --> SCLS
    C2 --> SCLS
    C3 --> SCLS
    C4 --> SCLS
    C7 --> SCLS
    C1 --> C6
    C6 --> A1
    C6 --> A2
    SCLS -.-> RQ
    RQ -.-> Q1
    RQ -.-> Q2
    RQ -.-> Q3
    RQ -.-> SD
```

图中要注意三点：

- **核心层不懂具体策略**。`__schedule()`、`try_to_wake_up()`、`sched_tick()` 主要通过 `p->sched_class->xxx()` 调用调度类的方法，自己负责的是锁、任务状态、时钟、抢占标志和上下文切换。少数地方为了效率直接调用特定类的函数，例如 `__pick_next_task()` 的快速路径直接调用公平类的 `pick_next_task_fair()` 和 idle 类的 `pick_task_idle()`（[core.c#L5989-L6003](../../linux/kernel/sched/core.c#L5989-L6003)，3.2 节），`sched_tick()` 直接调用公平类的 `sched_balance_trigger()`（[core.c#L5642-L5645](../../linux/kernel/sched/core.c#L5642-L5645)）。
- **调度类不做切换**。调度类只维护自己的队列、记账、回答“下一个是谁”和“要不要抢占”，真正的切换由核心层的 `context_switch()` 完成。
- **所有调度类共享一个每 CPU 的 `struct rq`**。`rq` 中嵌入了各调度类的子队列，由同一把 `rq->__lock` 保护。

与其他子系统的交互点：

| 子系统 | 交互方式 | 依据 |
| --- | --- | --- |
| 时间子系统 | 每个 tick 调用 `sched_tick()` | [timer.c#L2479](../../linux/kernel/time/timer.c#L2479) |
| 中断 / IPI（处理器间中断） | 中断返回时检查是否需要抢占；远程 CPU 通过重调度 IPI 通知 | [entry/common.c#L185-L211](../../linux/kernel/entry/common.c#L185-L211)、[x86 smp.c#L248-L255](../../linux/arch/x86/kernel/smp.c#L248-L255) |
| 等待队列、锁 | 睡眠一方调用 `schedule()`，唤醒一方调用 `try_to_wake_up()` | [wait.h#L302-L331](../../linux/include/linux/wait.h#L302-L331)、[core.c#L7296-L7301](../../linux/kernel/sched/core.c#L7296-L7301) |
| 内存管理 | 切换时按需切换地址空间，`mm == NULL` 的任务借用前一个任务的 `active_mm` | [core.c#L5315-L5341](../../linux/kernel/sched/core.c#L5315-L5341) |
| 进程管理 | fork 时初始化调度状态并首次入队，exit 时最后一次调度 | [fork.c#L2155](../../linux/kernel/fork.c#L2155)、[fork.c#L2642](../../linux/kernel/fork.c#L2642)、[exit.c#L1020](../../linux/kernel/exit.c#L1020) |
| cgroup | `cpu` 控制器把组变成公平类中的调度实体；cpuset 影响任务亲和性和调度域 | [cgroup v2 的 cpu 控制器](../cgroup2/cpu.md) |
| cpuidle | idle 任务在 `do_idle()` 中进入 C 状态 | [idle.c#L276-L383](../../linux/kernel/sched/idle.c#L276-L383) |

### 1.5 触发事件与输入输出

| 触发事件 | 输入 | 输出 / 结果 |
| --- | --- | --- |
| 任务调用 `schedule()` 阻塞 | `current->__state` 非 `TASK_RUNNING` | 尝试阻塞（信号可取消，公平类可延迟出队），选择 `next`；只有 `next != prev` 才切换（4.2 节） |
| `try_to_wake_up(p, state, flags)` | 任务 `p`、允许唤醒的状态掩码 | 状态匹配才认领唤醒；已在队列上的任务恢复状态，否则直接或异步入队；必要时设置目标 CPU 的 `need_resched`（4.4 节） |
| 每个 tick 调用 `sched_tick()` | 当前任务、运行队列时钟 | 更新运行时间记账；时间片用完则设置 `need_resched`；必要时触发负载均衡软中断 |
| 到达抢占点 | `TIF_NEED_RESCHED`（或 `TIF_NEED_RESCHED_LAZY`） | 调用 `__schedule(SM_PREEMPT)` 或 `schedule()` |
| fork 产生新任务 | 父任务的调度属性 | 子任务初始化为 `TASK_NEW`，选 CPU 后首次入队 |
| `sched_setattr()` / `nice()` | 新策略、优先级、参数 | 验证通过后修改属性；原本在队列上才摘下、重新入队，修改策略还可能更换调度类（4.6 节） |

### 1.6 本章边界

本章只讨论以上主线。以下内容只点到为止，留给后续章节：EEVDF 的放置（`place_entity()`）与延迟出队细节、PELT（Per-Entity Load Tracking，按实体的负载跟踪）、负载均衡的具体算法、实时类的 push/pull、deadline 类的 CBS（Constant Bandwidth Server，恒定带宽服务器）与准入控制、NUMA balancing、`nohz_full` 下的 tick 卸载、CPU 热插拔、优先级继承（rt_mutex）。组调度与带宽控制见 [cgroup v2 的 cpu 控制器](../cgroup2/cpu.md)，本章不重复。

## 2. 核心数据结构

本节按“任务 → 运行队列 → 调度类 → 类内队列 → 调度域”的顺序介绍，只解释与主线有关的字段。

### 2.1 结构地图

下图回答“一个任务怎样挂到一个 CPU 的运行队列上”。子图框表示包含关系（框内是嵌入的成员），虚线箭头表示指针或“挂入某个集合”的链接关系。

```mermaid
flowchart LR
    subgraph TASK["struct task_struct"]
        TS["__state / on_rq / on_cpu<br/>prio / policy"]
        SC["sched_class（指针）"]
        SE["se：sched_entity"]
        RTE["rt：sched_rt_entity"]
        DLE["dl：sched_dl_entity"]
    end

    subgraph RQ["struct rq（每 CPU 一个）"]
        LOCK["__lock"]
        CURR["curr / donor（指针）"]
        IDLE["idle / stop（指针）"]
        CFS["cfs：cfs_rq<br/>红黑树 tasks_timeline"]
        RT["rt：rt_rq<br/>位图 + 100 条链表"]
        DL["dl：dl_rq<br/>红黑树 root"]
        FS["fair_server：sched_dl_entity"]
        SDP["sd（RCU 指针）"]
    end

    CLS["fair_sched_class 等<br/>只读函数表"]
    DOM["sched_domain 层级<br/>SMT → CLS → MC → PKG → NODE → NUMA<br/>（按硬件裁剪）"]

    SC -.-> CLS
    SE -.->|"run_node 挂入"| CFS
    RTE -.->|"run_list 挂入"| RT
    DLE -.->|"rb_node 挂入"| DL
    FS -.->|"作为 DL 实体挂入"| DL
    CURR -.-> TASK
    SDP -.-> DOM
```

要注意：

- 一个任务**同时嵌入**三种调度实体（`se`、`rt`、`dl`），所属调度类使用对应实体维护队列；这不表示任务始终挂在排序结构中，睡眠、节流和正在运行等状态都有例外。切换调度类时，对原本入队的任务从旧类摘下、再挂入新类（4.6 节）。stop 和 idle 类只使用特殊任务指针。
- `rq->fair_server` 是 `rq` 中嵌入的一个 deadline 实体，它代表整个公平类参与 deadline 类的竞争（3.4 节）。
- `rq->curr` 指向正在运行的任务。要区分“在运行队列上”（`p->on_rq` 非 0）和“在调度类的排序结构里”：正在运行的公平任务 `on_rq` 仍为 1，但它的 `se` 被 [set_next_entity()（fair.c#L5632-L5653）](../../linux/kernel/sched/fair.c#L5632-L5653) 移出红黑树，由 `cfs_rq->curr` 单独记录，被换下时再由 [put_prev_entity()（fair.c#L5700-L5720）](../../linux/kernel/sched/fair.c#L5700-L5720) 放回。另外，`__schedule()` 中刚被 `block_task()` 出队的 `prev`，在 `rq->curr` 更新为 `next` 之前仍是 `rq->curr`。

### 2.2 `task_struct` 中的调度字段

[`struct task_struct`（linux/sched.h#L815）](../../linux/include/linux/sched.h#L815) 中与调度直接相关的字段集中在开头：

| 字段 | 含义 | 依据 |
| --- | --- | --- |
| `__state` | 任务状态：`TASK_RUNNING`（0）表示可运行；非 0 包括睡眠、停止、`TASK_WAKING`、`TASK_NEW`、`TASK_DEAD` 等，不能一律当成已睡眠 | [linux/sched.h#L823](../../linux/include/linux/sched.h#L823)、[linux/sched.h#L105-L139](../../linux/include/linux/sched.h#L105-L139) |
| `on_cpu` | 任务是否正在某个 CPU 上执行（包括切换过程中） | [linux/sched.h#L844](../../linux/include/linux/sched.h#L844) |
| `wake_entry` | 远程唤醒时挂到目标 CPU 唤醒链表的节点 | [linux/sched.h#L845](../../linux/include/linux/sched.h#L845) |
| `wake_cpu`、`recent_used_cpu` | 唤醒时选择 CPU 的起点与提示 | [linux/sched.h#L857-L858](../../linux/include/linux/sched.h#L857-L858) |
| `on_rq` | 是否在运行队列上：0、`TASK_ON_RQ_QUEUED`（1）、`TASK_ON_RQ_MIGRATING`（2） | [linux/sched.h#L859](../../linux/include/linux/sched.h#L859)、[sched.h#L97-L98](../../linux/kernel/sched/sched.h#L97-L98) |
| `prio` | **有效优先级**，调度器实际使用的值；可被优先级继承临时提升 | [linux/sched.h#L861](../../linux/include/linux/sched.h#L861) |
| `static_prio` | 由 nice 值决定：`nice + 120` | [linux/sched.h#L862](../../linux/include/linux/sched.h#L862) |
| `normal_prio` | 不考虑优先级继承时按策略算出的优先级 | [linux/sched.h#L863](../../linux/include/linux/sched.h#L863) |
| `rt_priority` | 用户设置的实时优先级 1～99 | [linux/sched.h#L864](../../linux/include/linux/sched.h#L864) |
| `se`、`rt`、`dl` | 三种调度实体，嵌入在任务中 | [linux/sched.h#L866-L868](../../linux/include/linux/sched.h#L866-L868) |
| `dl_server` | 若任务是经由 deadline 服务器选中的，指向该服务器 | [linux/sched.h#L869](../../linux/include/linux/sched.h#L869) |
| `sched_class` | 当前所属调度类 | [linux/sched.h#L873](../../linux/include/linux/sched.h#L873) |
| `policy` | 调度策略 | [linux/sched.h#L915](../../linux/include/linux/sched.h#L915) |
| `nr_cpus_allowed`、`cpus_ptr`、`cpus_mask` | 允许运行的 CPU 集合（亲和性） | [linux/sched.h#L917-L920](../../linux/include/linux/sched.h#L917-L920) |
| `migration_disabled` | 非 0 时任务暂时不能迁移 | [linux/sched.h#L922](../../linux/include/linux/sched.h#L922) |
| `pi_lock` | 保护唤醒以及策略、亲和性等属性的修改 | [linux/sched.h#L1229](../../linux/include/linux/sched.h#L1229) |

`prio`、`normal_prio`、`static_prio` 三者的关系由 [effective_prio()（syscalls.c#L52-L63）](../../linux/kernel/sched/syscalls.c#L52-L63) 体现：先由策略算出 `normal_prio`；如果当前 `prio` 不在实时或 deadline 区间，就返回 `normal_prio`，否则保留当前有效值，以免覆盖优先级继承的提升。后一分支也会覆盖本来就是实时优先级的任务，不能仅据此断言任务已被继承提升。

**三个状态变量。** 理解调度器并发协议的关键，是区分 `__state`、`on_rq` 和 `on_cpu` 这三个相互独立的变量。[core.c#L592-L618](../../linux/kernel/sched/core.c#L592-L618) 的注释给出了它们的修改规则：

| 变量 | 回答的问题 | 谁修改 | 保护方式 |
| --- | --- | --- | --- |
| `__state` | 任务处于哪种调度状态？ | 任务自己设置状态；`try_to_wake_up()` 写回 `TASK_RUNNING` | 普通等待用 `set_current_state()` 的屏障；特殊状态用 `set_special_state()` 持 `pi_lock`；唤醒方也用 `pi_lock`（[linux/sched.h#L201-L267](../../linux/include/linux/sched.h#L201-L267)） |
| `on_rq` | 任务是否在运行队列上？ | `activate_task()` 置为 `TASK_ON_RQ_QUEUED`；迁移时 `deactivate_task()` 置为 `TASK_ON_RQ_MIGRATING`；睡眠时 `block_task()` 经 `__block_task()` 清零 | 初始化之外在 `rq->__lock` 下修改，清零用 release 语义 |
| `on_cpu` | 任务是否正在 CPU 上？ | `prepare_task()` 切入前置 1，`finish_task()` 切出后清 0 | `rq->__lock` 下修改，清零用 release 语义 |

`on_rq` 一行需要注意注释与实现的差异：注释（[core.c#L599-L604](../../linux/kernel/sched/core.c#L599-L604)）写的是“由 `deactivate_task()` 清零”，但当前实现中 [deactivate_task()（core.c#L2156-L2169）](../../linux/kernel/sched/core.c#L2156-L2169) 只把 `on_rq` 写为 `TASK_ON_RQ_MIGRATING`，供迁移代码在摘下与挂上之间使用。除了 fork 时 [__sched_fork()（core.c#L4458）](../../linux/kernel/sched/core.c#L4458) 对新任务的初始化，把 `on_rq` 写成 0 的只有 [__block_task()（sched.h#L2810）](../../linux/kernel/sched/sched.h#L2810)，调用者是 [block_task()（core.c#L2171-L2175）](../../linux/kernel/sched/core.c#L2171-L2175) 和延迟出队任务最终出队的路径（[fair.c#L7298-L7311](../../linux/kernel/sched/fair.c#L7298-L7311)）。本章以实现为准。

几种典型组合：

| `__state` | `on_rq` | `on_cpu` | 处境 |
| --- | --- | --- | --- |
| `TASK_RUNNING` | 1 | 1 | 正在运行 |
| `TASK_RUNNING` | 1 | 0 | 可运行，在队列中等待 |
| 非 0 | 1 | 1 | 已调用 `set_current_state()`，但还没在 `__schedule()` 中出队 |
| 非 0 | 1 | 0 | 设置睡眠状态后、主动调用 `schedule()` 前被抢占，仍留在队列中（4.2 节） |
| 非 0 | 0 | 1 | `__schedule()` 已把它出队，但 CPU 还没切换完 |
| 非 0 | 0 | 0 | 完全睡眠 |
| `TASK_WAKING` | 0 | 0 或 1 | 唤醒方已认领，正在选择 CPU、准备入队 |
| `TASK_NEW` | 0 | 0 | fork 中，尚未首次入队 |

此外还有一个特例：公平类的**延迟出队**（delayed dequeue）。任务已睡眠（`__state` 非 0），但 `on_rq` 仍为 1，`p->se.sched_delayed` 为 1，它会在下次被选中时才真正出队（[core.c#L606-L609](../../linux/kernel/sched/core.c#L606-L609)，3.3 节）。

### 2.3 `struct rq`：每 CPU 的运行队列

[`struct rq`（sched.h#L1120-L1323）](../../linux/kernel/sched/sched.h#L1120-L1323) 是调度器最核心的结构。它是一个每 CPU 变量 `runqueues`（[sched.h#L1353](../../linux/kernel/sched/sched.h#L1353)），通过 `cpu_rq(cpu)`、`this_rq()`、`task_rq(p)`、`cpu_curr(cpu)` 访问（[sched.h#L1361-L1364](../../linux/kernel/sched/sched.h#L1361-L1364)）。

| 字段 | 含义 |
| --- | --- |
| `__lock` | 运行队列锁，`raw_spinlock_t`；保护本结构及其中嵌入的各类子队列 |
| `nr_running` | 本 CPU 上各调度类入队任务的总数，不含 idle 任务；处于延迟出队状态的公平任务在真正出队前仍计入（[fair.c#L7233-L7235](../../linux/kernel/sched/fair.c#L7233-L7235)、[fair.c#L7292](../../linux/kernel/sched/fair.c#L7292)） |
| `nr_switches` | 本 CPU 上发生的上下文切换次数 |
| `ttwu_pending` | 远程唤醒链表上是否有待处理的任务 |
| `cfs`、`rt`、`dl` | 三个嵌入的子队列：公平类、实时类、deadline 类 |
| `fair_server` | 代表公平类的 deadline 实体（3.4 节） |
| `nr_uninterruptible` | 处于不可中断睡眠的任务计数，用于计算系统负载；只有各 CPU 之和有意义 |
| `curr`、`donor` | 当前运行的任务；当前配置下二者是同一个联合体成员 |
| `dl_server` | 本次选择是否经由 deadline 服务器 |
| `idle`、`stop` | 本 CPU 的 idle 任务和 stopper 任务 |
| `clock`、`clock_task` | 运行队列时钟，单位纳秒（3.7 节） |
| `rd`、`sd` | 根域（root domain）和最底层调度域，供负载均衡使用 |
| `next_balance` | 下次周期性负载均衡的时刻，单位 jiffies |
| `cpu` | 本运行队列所属 CPU 编号 |

字段依次见 [sched.h#L1122-L1139](../../linux/kernel/sched/sched.h#L1122-L1139)、[sched.h#L1148-L1155](../../linux/kernel/sched/sched.h#L1148-L1155)、[sched.h#L1169-L1189](../../linux/kernel/sched/sched.h#L1169-L1189)、[sched.h#L1208-L1209](../../linux/kernel/sched/sched.h#L1208-L1209) 和 [sched.h#L1226](../../linux/kernel/sched/sched.h#L1226)。

**加锁规则。** 同时锁多个运行队列时必须按固定顺序加锁，以免两个 CPU 以相反顺序加锁而死锁。结构体上方的注释（[sched.h#L1113-L1119](../../linux/kernel/sched/sched.h#L1113-L1119)）写的是按 `rq` 地址升序，但实现并不比较地址：[double_rq_lock()（core.c#L703-L715）](../../linux/kernel/sched/core.c#L703-L715) 用 [rq_order_less()（sched.h#L2929-L2953）](../../linux/kernel/sched/sched.h#L2929-L2953) 决定先后，当前配置（`CONFIG_SCHED_CORE` 未设置）下它比较的是 `rq->cpu`，即**按 CPU 编号升序**加锁。本章以实现为准。

**初始化。** [sched_init()（core.c#L8731-L8741）](../../linux/kernel/sched/core.c#L8731-L8741) 为每个可能的 CPU 初始化锁和三个子队列，在 [core.c#L8803](../../linux/kernel/sched/core.c#L8803) 初始化 `fair_server`。启动 CPU 上当前执行的代码随后被 [init_idle()（core.c#L8844-L8845）](../../linux/kernel/sched/core.c#L8844-L8845) 变成该 CPU 的 idle 任务：`rq->idle` 和 `rq->curr` 都指向它，`sched_class` 设为 `idle_sched_class`（[core.c#L8043-L8060](../../linux/kernel/sched/core.c#L8043-L8060)）。stopper 线程在创建时由 [sched_set_stop_task()（core.c#L3610-L3644）](../../linux/kernel/sched/core.c#L3610-L3644) 设为 `stop_sched_class` 并写入 `rq->stop`。

### 2.4 `struct sched_class`：调度类的接口

[`struct sched_class`（sched.h#L2413-L2484）](../../linux/kernel/sched/sched.h#L2413-L2484) 是一张函数表。按职责可以分成几组：

| 职责 | 方法 | 调用时机 |
| --- | --- | --- |
| 队列维护 | `enqueue_task`、`dequeue_task` | 任务变为可运行 / 不可运行，或属性修改前后 |
| 选择 | `balance`、`pick_task`、`pick_next_task`（可选） | `__schedule()` 选下一个任务 |
| 切换前后 | `put_prev_task`、`set_next_task` | 任务被换下 / 被换上 CPU |
| 抢占判断 | `wakeup_preempt` | 新任务入队后，判断是否应抢占当前任务 |
| 放置与迁移 | `select_task_rq`、`migrate_task_rq`、`task_woken`、`set_cpus_allowed`、`find_lock_rq` | 唤醒、fork、exec 选 CPU；迁移；亲和性变化 |
| 记账 | `task_tick`、`update_curr` | 每个 tick；需要最新运行时间时 |
| 生命周期 | `task_fork`、`task_dead` | fork、任务最后一次切出 |
| 属性变化 | `switching_to`、`switched_from`、`switched_to`、`prio_changed`、`reweight_task` | 更换调度类、优先级或权重 |
| CPU 上下线 | `rq_online`、`rq_offline` | CPU 加入或离开根域 |

`pick_task` 与 `pick_next_task` 的关系由注释（[sched.h#L2428-L2437](../../linux/kernel/sched/sched.h#L2428-L2437)）规定：`pick_next_task` 是可选的优化版本，它等价于“`pick_task()`，若选中则 `put_prev_task(prev)` 再 `set_next_task_first(next)`”。核心层的通用版本就是 [put_prev_set_next_task()（sched.h#L2507-L2520）](../../linux/kernel/sched/sched.h#L2507-L2520)。

**调度类的顺序。** 调度类之间的优先级不是写在某个数组里，而是由**链接器**决定的。[DEFINE_SCHED_CLASS（sched.h#L2532-L2535）](../../linux/kernel/sched/sched.h#L2532-L2535) 把每个类放进独立的段，链接脚本 [SCHED_DATA（vmlinux.lds.h#L136-L145）](../../linux/include/asm-generic/vmlinux.lds.h#L136-L145) 按 stop、dl、rt、fair、ext、idle 的顺序（当前配置下 ext 段为空）把它们排在 `__sched_class_highest` 与 `__sched_class_lowest` 之间。于是：

- 比较优先级就是比较地址：[sched_class_above(a, b)](../../linux/kernel/sched/sched.h#L2575) 定义为 `a < b`；
- 遍历所有类就是指针递增：[for_each_class / for_each_active_class（sched.h#L2563-L2573）](../../linux/kernel/sched/sched.h#L2563-L2573)。

[sched_init()（core.c#L8671-L8675）](../../linux/kernel/sched/core.c#L8671-L8675) 开头用 `BUG_ON` 检查这个顺序。`DEFINE_SCHED_CLASS` 上方的注释写着“laid out in *REVERSE* order”，与当前链接脚本从高到低排列的写法看上去不一致；判断顺序时应以链接脚本、`sched_class_above()` 和 `sched_init()` 中的检查为准。

### 2.5 各类的子队列与调度实体

| 调度类 | 队列结构 | 实体 | 组织方式 | 依据 |
| --- | --- | --- | --- | --- |
| fair | `struct cfs_rq` | `struct sched_entity` | 增强红黑树 `tasks_timeline`，按**虚拟截止时间**排序；每个节点额外维护子树最小 `vruntime` | [sched.h#L676-L698](../../linux/kernel/sched/sched.h#L676-L698)、[linux/sched.h#L570-L616](../../linux/include/linux/sched.h#L570-L616)、[fair.c#L582-L590](../../linux/kernel/sched/fair.c#L582-L590) |
| rt | `struct rt_rq` | `struct sched_rt_entity` | `rt_prio_array`：100 条链表 + 位图，下标即 `prio` | [sched.h#L309-L312](../../linux/kernel/sched/sched.h#L309-L312)、[sched.h#L827-L854](../../linux/kernel/sched/sched.h#L827-L854)、[linux/sched.h#L618-L634](../../linux/include/linux/sched.h#L618-L634) |
| deadline | `struct dl_rq` | `struct sched_dl_entity` | 红黑树 `root`，按绝对截止时间排序 | [sched.h#L862-L877](../../linux/kernel/sched/sched.h#L862-L877)、[linux/sched.h#L639-L713](../../linux/include/linux/sched.h#L639-L713) |
| stop / idle | 无 | 无 | `rq->stop`、`rq->idle` 两个指针 | [sched.h#L1181-L1182](../../linux/kernel/sched/sched.h#L1181-L1182) |

公平类实体中与本章有关的字段：

| 字段 | 含义 |
| --- | --- |
| `load` | 权重，由 nice 值查表得到（3.1 节） |
| `run_node` | 红黑树节点 |
| `on_rq` | 实体是否在 `cfs_rq` 上（与 `task_struct::on_rq` 不是同一个字段） |
| `sched_delayed` | 是否处于延迟出队状态 |
| `exec_start`、`sum_exec_runtime` | 上次记账的时刻、累计实际运行时间，单位纳秒 |
| `vruntime` | 虚拟运行时间：实际运行时间按权重缩放后累加 |
| `deadline` | 虚拟截止时间 |
| `vlag` | 出队时记录的虚拟滞后量 `V − vruntime`（不乘权重，3.3 节），再次入队时用于放置 |
| `slice` | 请求的时间片长度，单位纳秒 |
| `min_vruntime` | 以本节点为根的子树中最小的 `vruntime`，用于剪枝 |
| `parent`、`cfs_rq`、`my_q` | 组调度：父实体、所在队列、组实体自己拥有的队列 |

最后一行的三个字段只在 `CONFIG_FAIR_GROUP_SCHED` 下存在。开启组调度后，`rq->cfs` 是根组的队列，一个 cgroup 在每个 CPU 上都表现为上一层队列中的一个 `sched_entity`，它的 `my_q` 指向组自己的 `cfs_rq`；选择时从根队列逐层向下，直到选中一个任务实体，见 [pick_task_fair()（fair.c#L9104-L9135）](../../linux/kernel/sched/fair.c#L9104-L9135) 中的 `do { ... } while (cfs_rq)` 循环。根任务组 `root_task_group` 的任务直接挂在 `rq->cfs` 上（[core.c#L8761-L8764](../../linux/kernel/sched/core.c#L8761-L8764)），此时循环只执行一次。

要注意，“没有使用 cgroup”并不等于“所有任务都在根任务组”。当前配置 `CONFIG_SCHED_AUTOGROUP=y`，sysctl `kernel.sched_autogroup_enabled` 默认为 1（[autogroup.c#L10](../../linux/kernel/sched/autogroup.c#L10)）。进程成功调用 `setsid()` 建立新会话时，[sys.c#L1298](../../linux/kernel/sys.c#L1298) 调用 `sched_autogroup_create_attach()`，后者经 `autogroup_create()` 以 `root_task_group` 为父创建任务组，并通过 `signal->autogroup` 关联整个线程组（[autogroup.c#L87-L118](../../linux/kernel/sched/autogroup.c#L87-L118)、[autogroup.c#L159-L202](../../linux/kernel/sched/autogroup.c#L159-L202)）。分配失败会回退到 `autogroup_default`，而不是保证创建新组（[autogroup.c#L120-L128](../../linux/kernel/sched/autogroup.c#L120-L128)）。

fork 和任务组变更时，只有 autogroup 开启、任务的 cpu cgroup 是根组且任务未带 `PF_EXITING`，[autogroup_task_group()（autogroup.h#L32-L42）](../../linux/kernel/sched/autogroup.h#L32-L42) 才把它重定向到所在会话的 autogroup（[task_wants_autogroup()（autogroup.c#L131-L152）](../../linux/kernel/sched/autogroup.c#L131-L152)、[core.c#L4780](../../linux/kernel/sched/core.c#L4780)、[core.c#L9214](../../linux/kernel/sched/core.c#L9214)）。若关联的是新建 autogroup，公平任务挂在它自己的 `cfs_rq` 上，选择循环要走两层；仍在根 cpu cgroup 的任务关闭 autogroup 后则直接挂在 `rq->cfs` 上。关闭方式包括启动参数 `noautogroup`（[autogroup.c#L223-L229](../../linux/kernel/sched/autogroup.c#L223-L229)）和 sysctl；默认组 `autogroup_default` 本身也指向 `root_task_group`（[autogroup.c#L35-L40](../../linux/kernel/sched/autogroup.c#L35-L40)）。非根 cpu cgroup 的任务仍遵循自己的组层级，不受这项重定向影响。关联对象用 `kref` 持有，换组及退出时归还引用（[autogroup.c#L175-L191](../../linux/kernel/sched/autogroup.c#L175-L191)、[autogroup.c#L213-L220](../../linux/kernel/sched/autogroup.c#L213-L220)）。层级调度的细节见 [cgroup v2 的 cpu 控制器](../cgroup2/cpu.md)。

### 2.6 `struct sched_domain`：CPU 拓扑

负载均衡需要知道 CPU 之间的拓扑关系，例如 SMT 线程是否共享核内资源、哪些核共享末级缓存（LLC，Last Level Cache）、哪些 CPU 属于同一 NUMA 节点。调度器用这些关系约束搜索范围并表达局部性，但不能仅凭层级给所有工作负载的迁移代价作绝对排序。[`struct sched_domain`（topology.h#L73-L92）](../../linux/include/linux/sched/topology.h#L73-L92) 描述一个层级上的 CPU 集合：

| 字段 | 含义 |
| --- | --- |
| `parent`、`child` | 上一层和下一层调度域，RCU 保护 |
| `groups` | 本域被划分成的若干 `sched_group`，均衡在组之间进行 |
| `min_interval`、`max_interval`、`balance_interval` | 均衡间隔，单位毫秒 |
| `imbalance_pct` | 负载差超过多少百分比才进行均衡 |
| `flags` | `SD_*` 标志，如是否参与唤醒时的亲和选择、是否在 fork/exec 时均衡 |
| `last_balance` | 上次均衡的时刻，单位 jiffies |

x86 的拓扑层级由 [x86_topology（smpboot.c#L481-L491）](../../linux/arch/x86/kernel/smpboot.c#L481-L491) 定义：SMT、CLS（簇）、MC（多核，共享 LLC）、PKG（封装）。这张表在启动时会被裁剪：[build_sched_topology()（smpboot.c#L493-L516）](../../linux/arch/x86/kernel/smpboot.c#L493-L516) 在每核只有一个线程时去掉 SMT 层，在一个封装内含多个 NUMA 节点时清空 PKG 层。随后 `sched_init_numa()` 在表尾追加一层 NODE（本节点），节点间存在多种距离时再追加若干 NUMA 层（[topology.c#L2049-L2060](../../linux/kernel/sched/topology.c#L2049-L2060)）。建立调度域后，[cpu_attach_domain()（topology.c#L721-L745）](../../linux/kernel/sched/topology.c#L721-L745) 还会删除不起作用（退化）的层。`rq->sd` 可为空；非空时指向本 CPU 最底层的调度域，沿 `parent` 向上到当前调度域分区的顶层，不保证覆盖整个系统（[cpu_attach_domain()（topology.c#L711-L788）](../../linux/kernel/sched/topology.c#L711-L788)）。

`rq->sd` 由 [cpu_attach_domain()（topology.c#L771）](../../linux/kernel/sched/topology.c#L771) 用 `rcu_assign_pointer()` 发布，旧树经 [destroy_sched_domains()（topology.c#L633-L647）](../../linux/kernel/sched/topology.c#L633-L647) 的 `call_rcu()` 延迟释放。[for_each_domain()（sched.h#L2022-L2024）](../../linux/kernel/sched/sched.h#L2022-L2024) 用 `rcu_dereference_check()` 读取；上方注释要求任何 CPU 的域树都在关抢占区间内访问（[sched.h#L2015-L2020](../../linux/kernel/sched/sched.h#L2015-L2020)）。**关抢占区间也能保障这里的对象生命周期**：当前普通 RCU 宽限期既等待显式 `rcu_read_lock()`，也等待 `preempt_disable()` 区间（[rcupdate.h#L704-L709](../../linux/include/linux/rcupdate.h#L704-L709)）；当前 `PREEMPT_RCU` 实现在关抢占或禁用软中断的计数位非零时不报告静止状态（[tree_plugin.h#L811-L828](../../linux/kernel/rcu/tree_plugin.h#L811-L828)）。这与“调用过显式 `rcu_read_lock()`”不是同一回事，`rcu_dereference_check()` 的读侧锁检查也须分别看待（[rcupdate.h#L651-L656](../../linux/include/linux/rcupdate.h#L651-L656)）；不能据 `CONFIG_PREEMPT_RCU=y` 就断言关抢占无法保护 `call_rcu()` 延迟释放的域树。

调度域的覆盖范围还受分区影响：重建时针对每个 `doms_new[i]` 分别调用 `build_sched_domains()`，撤销分区时则可以把 CPU 的域设为空（[topology.c#L2823-L2831](../../linux/kernel/sched/topology.c#L2823-L2831)、[topology.c#L2724-L2728](../../linux/kernel/sched/topology.c#L2724-L2728)）。因此结构地图中的拓扑链只是可能的层级，不是每个 CPU 必然拥有的完整链。

### 2.7 锁顺序与保护范围

[core.c#L549-L559](../../linux/kernel/sched/core.c#L549-L559) 给出了调度器的锁顺序：

```text
p->pi_lock
  rq->lock
    hrtimer_cpu_base->lock

rq1->lock
  rq2->lock      （rq1 < rq2）
```

注释中的 `rq1 < rq2` 在当前实现里指 `rq_order_less()` 的结果，即 CPU 编号较小者先加锁（2.3 节）。

[core.c#L576-L590](../../linux/kernel/sched/core.c#L576-L590) 进一步说明：系统调用等“外部”修改者使用 [task_rq_lock()（core.c#L744-L781）](../../linux/kernel/sched/core.c#L744-L781) 同时持有 `p->pi_lock` 和 `rq->lock`，因此它们修改的属性在持有**任意一把**锁时都是稳定的：

| 修改者 | 被保护的字段 |
| --- | --- |
| `sched_setaffinity()` / `set_cpus_allowed_ptr()` | `cpus_ptr`、`nr_cpus_allowed` |
| `set_user_nice()` | `se.load`、各 `prio` |
| `__sched_setscheduler()` | `sched_class`、`policy`、各 `prio`、`se.load`、`rt_priority`、`dl` 参数 |
| `sched_move_task()` | `sched_task_group` |

`task_rq_lock()` 有一个容易忽略的细节：先读 `task_rq(p)` 再加锁，加锁后必须重新检查任务是否还在这个运行队列上、是否处于 `TASK_ON_RQ_MIGRATING`，不满足就释放重试（[core.c#L771-L779](../../linux/kernel/sched/core.c#L771-L779)）。`TASK_ON_RQ_MIGRATING` 允许迁移代码在不同时持有两把 `rq->lock` 的情况下移动任务。

## 3. 关键算法

### 3.1 nice 值与权重

公平类按**权重**分配 CPU 时间。nice 值通过 [sched_prio_to_weight[]（core.c#L10342-L10363）](../../linux/kernel/sched/core.c#L10342-L10363) 查表得到权重：nice 0 对应 1024，相邻两级之比约为 1.25。注释中“约少 10% CPU”的说法须结合竞争集合理解：同组、同一 CPU 上两个持续可运行的 nice 0 任务原本各占 50%；其中一个改为 nice 1 后，权重从 1024 变成 820，理想份额成为 `820 / (1024 + 820) ≈ 44.5%`，比原份额约少 11%。权重比约为 0.8，不代表任意竞争集合中份额固定减少 10%。

[set_load_weight()（core.c#L1448-L1469）](../../linux/kernel/sched/core.c#L1448-L1469) 用 `static_prio - MAX_RT_PRIO` 作为下标查表；`SCHED_IDLE` 任务则使用最小权重 `WEIGHT_IDLEPRIO = 3`（[sched.h#L2351](../../linux/kernel/sched/sched.h#L2351)）。在 64 位内核上，表中的值还要经 `scale_load()` 左移 10 位作为内部权重，以提高定点运算精度（[sched.h#L147-L173](../../linux/kernel/sched/sched.h#L147-L173)）。

权重的作用体现在虚拟运行时间的增长速度上。[calc_delta_fair()（fair.c#L290-L296）](../../linux/kernel/sched/fair.c#L290-L296) 计算：

```text
Δvruntime = Δexec × NICE_0_LOAD / weight
```

一个**概念性例子**（同组、同一 CPU 上只有这两个持续可运行任务，无带宽节流，忽略 EEVDF 的截止时间、保护期等细节）：任务 A 为 nice 0（表权重 1024），任务 B 为 nice 5（表权重 335）。A 实际运行 10 ms，`vruntime` 增加 10 ms；B 实际运行 10 ms，`vruntime` 增加约 30.6 ms。理想公平模型使长期 `vruntime` 增长速率接近，A 得到的 CPU 时间约为 B 的 1024/335 ≈ 3 倍；这不是每次选任务时单纯比较最小 `vruntime` 的算法。

### 3.2 选择下一个任务：按类询问，快速路径直达公平类

`__schedule()` 通过 `pick_next_task()` → [__pick_next_task()（core.c#L5972-L6023）](../../linux/kernel/sched/core.c#L5972-L6023) 选出下一个任务。算法的目标是：在所有调度类中，返回最高优先级的类所选出的任务。

简化逻辑（伪代码，省略了 sched_ext 和 `dl_server` 处理；当前配置下 `rq->donor` 就是 `prev`）：

```text
pick(rq):
    # 快速路径：所有可运行任务都在公平类，且 prev 不属于更高的类
    if prev 的类不高于 fair 且 rq->nr_running == rq->cfs.h_nr_queued:
        p = pick_next_task_fair(rq, prev, rf)  # 内部完成 put_prev/set_next；无任务时做 newidle 均衡
        if p == RETRY_TASK:  goto 慢路径        # newidle 均衡期间来了更高类的任务
        if p == NULL:                           # 公平类也没有任务
            p = pick_task_idle(rq)
            put_prev_set_next_task(prev, p)
        return p

慢路径:
    prev_balance(rq)                            # 从 prev 的类开始，依次调用 class->balance()
    for class in stop, dl, rt, fair, idle:
        if class 实现了 pick_next_task:          # 当前配置下只有公平类
            p = class->pick_next_task(rq, prev) # 内部完成 put_prev/set_next
            if p: return p
        else:
            p = class->pick_task(rq)
            if p: put_prev_set_next_task(prev, p); return p
```

慢路径的循环见 [core.c#L6005-L6022](../../linux/kernel/sched/core.c#L6005-L6022)。公平类实现了可选的 `.pick_next_task`（[fair.c#L14105](../../linux/kernel/sched/fair.c#L14105)），由它自己完成换下 `prev`、换上新任务的工作，核心层不再调用 `put_prev_set_next_task()`。

两个设计要点：

- **快速路径**（[core.c#L5989-L6003](../../linux/kernel/sched/core.c#L5989-L6003)）。绝大多数时间系统里只有公平任务，此时无须逐个询问 stop、dl、rt 类。条件中要求 `prev` 不属于更高的类，注释解释了原因：否则那些类会失去从其他 CPU 拉取任务的机会。
- **`balance` 先于 `put_prev_task`**。[prev_balance()（core.c#L5937-L5967）](../../linux/kernel/sched/core.c#L5937-L5967) 从 `prev` 所属的类开始向下调用各类的 `balance()`，只要某个类报告“有该类或更高类的任务可运行”就停止。`balance()` 可能释放并重新获取 `rq->lock`，从其他 CPU 拉任务：实时类的 [balance_rt()（rt.c#L1594-L1614）](../../linux/kernel/sched/rt.c#L1594-L1614) 调用 `pull_rt_task()`，公平类的 [balance_fair()（fair.c#L8883-L8889）](../../linux/kernel/sched/fair.c#L8883-L8889) 在本类无任务时调用 `sched_balance_newidle()`。注释强调这一步必须在 `put_prev_task()` 之前完成，使锁释放期间 `prev` 的状态与加锁前一致。

idle 类的 [pick_task_idle()（idle.c#L496-L500）](../../linux/kernel/sched/idle.c#L496-L500) 总是返回 `rq->idle`，所以遍历循环一定有结果，末尾的 `BUG()` 不会触发。

### 3.3 公平类：EEVDF 简述

当前版本的公平类使用 **EEVDF**（Earliest Eligible Virtual Deadline First，最早合格虚拟截止时间优先）。它的出发点是“理想公平”：如果 CPU 能无限细分，每个任务按权重比例同时运行，那么任务 i 应得的服务量与实际得到的服务量之差称为**滞后**（lag）。EEVDF 的基本规则是在滞后非负（“还欠着它”）的任务中挑选，再按截止时间决定先后；实现还包含下文的单实体、buddy 和保护期分支。[fair.c#L615-L672](../../linux/kernel/sched/fair.c#L615-L672) 的注释给出了推导，核心量如下：

| 概念 | 定义 | 实现 |
| --- | --- | --- |
| 虚拟运行时间 `v_i` | 实际运行时间按权重缩放后的累计值 | `se->vruntime`，在 [update_curr()（fair.c#L1286-L1333）](../../linux/kernel/sched/fair.c#L1286-L1333) 中累加 |
| 队列虚拟时间 `V` | 所有实体 `vruntime` 的加权平均 | [avg_vruntime()（fair.c#L715-L749）](../../linux/kernel/sched/fair.c#L715-L749)，用 `zero_vruntime`、`sum_w_vruntime`、`sum_weight` 增量维护 |
| 滞后 `lag_i` | `w_i × (V − v_i)` | 定义见 [fair.c#L622-L624](../../linux/kernel/sched/fair.c#L622-L624) 的注释，源码不直接保存它 |
| 虚拟滞后 `vlag_i` | `V − v_i`，并钳制在 ±“队列中最大 slice 加一个 tick、按本实体权重折算的虚拟时间”之内 | [entity_lag()（fair.c#L767-L776）](../../linux/kernel/sched/fair.c#L767-L776)；出队时由 [update_entity_lag()（fair.c#L778-L783）](../../linux/kernel/sched/fair.c#L778-L783) 存入 `se->vlag` |
| 合格（eligible） | `lag_i ≥ 0`，即 `v_i ≤ V` | [vruntime_eligible()（fair.c#L802-L816）](../../linux/kernel/sched/fair.c#L802-L816)，用乘法比较避免除法误差 |
| 虚拟截止时间 `vd_i` | `v_i + slice × NICE_0_LOAD / w_i`，这里 `w_i` 是内部权重，与 `NICE_0_LOAD` 使用同一缩放尺度 | [calc_delta_fair()（fair.c#L290-L296）](../../linux/kernel/sched/fair.c#L290-L296)、[update_deadline()（fair.c#L1117-L1140）](../../linux/kernel/sched/fair.c#L1117-L1140) |

**选择算法。** [pick_eevdf()（fair.c#L996-L1084）](../../linux/kernel/sched/fair.c#L996-L1084) 在合格实体中选虚拟截止时间最早者。红黑树按截止时间排序（[entity_before()（fair.c#L582-L590）](../../linux/kernel/sched/fair.c#L582-L590)），同时每个节点维护子树最小 `vruntime`。要注意，正在运行的实体 `cfs_rq->curr` 不在树里（2.1 节），需要单独处理。查找步骤如下：

1. 队列中只有一个实体（`nr_queued == 1`）时不检查合格性，直接返回它（[fair.c#L1026-L1027](../../linux/kernel/sched/fair.c#L1026-L1027)）；
2. 特性 `PICK_BUDDY` 开启（默认）且 `cfs_rq->next` 指定的 next buddy 合格时，返回它（[fair.c#L1032-L1037](../../linux/kernel/sched/fair.c#L1032-L1037)）；
3. `curr` 若已不在队列上或不合格，就不再参与比较；若它合格、调用者要求保护（`protect` 为真）且仍在**保护期**内（`vruntime` 小于 `se->vprot`，保护长度与 `RUN_TO_PARITY` 特性有关，见 [set_protect_slice()（fair.c#L963-L976）](../../linux/kernel/sched/fair.c#L963-L976)），直接返回 `curr`（[fair.c#L1039-L1043](../../linux/kernel/sched/fair.c#L1039-L1043)）；
4. 若树中最左节点（截止时间最早）合格，取它为候选；
5. 否则从根向下：左子树中若存在合格实体（子树最小 `vruntime` 合格），就进入左子树，因为那里的截止时间更早；否则检查当前节点，合格则取为候选，不合格就进入右子树；
6. 最后让仍参与比较的 `curr` 与候选比较截止时间，取更早者（[fair.c#L1079-L1081](../../linux/kernel/sched/fair.c#L1079-L1081)）。

树中查找只需 O(log n) 次比较。

**时间片。** 每个实体请求的时间片 `slice` 默认取 `sysctl_sched_base_slice`，基准值 0.7 ms（[fair.c#L79-L80](../../linux/kernel/sched/fair.c#L79-L80)）。在缩放方式 `sysctl_sched_tunable_scaling` 取默认值 `SCHED_TUNABLESCALING_LOG` 时（[fair.c#L72](../../linux/kernel/sched/fair.c#L72)），它按 `1 + ilog2(min(在线 CPU 数, 8))` 放大（[fair.c#L192-L221](../../linux/kernel/sched/fair.c#L192-L221)）。例如在线 CPU 不少于 8 个时放大 4 倍，即 2.8 ms。[update_deadline()（fair.c#L1117-L1140）](../../linux/kernel/sched/fair.c#L1117-L1140) 在 `vruntime` 已经不小于 `deadline` 时为它计算新的虚拟截止时间，并返回真。[update_curr()（fair.c#L1326-L1332）](../../linux/kernel/sched/fair.c#L1326-L1332) 并不因此马上重调度：`cfs_rq->nr_queued == 1`（该队列里只有当前实体）时直接返回；否则，时间片已经耗尽，或者当前实体已经不在保护期内（`vruntime` 不再小于 `se->vprot`），才调用 `resched_curr_lazy()`。

**延迟出队。** 睡眠出队时，若特性 `DELAY_DEQUEUE` 开启（默认开启，[features.h#L50-L59](../../linux/kernel/sched/features.h#L50-L59)），这次没有带 `DEQUEUE_SPECIAL`（`__TASK_STOPPED`、`__TASK_TRACED`、`TASK_PARKED`、`TASK_DEAD`、`TASK_FROZEN`）或 `DEQUEUE_THROTTLE`，并且实体不合格（滞后为负，多用了 CPU），[dequeue_entity()（fair.c#L5561-L5576）](../../linux/kernel/sched/fair.c#L5561-L5576) 不做真正的出队：`se->on_rq` 仍为 1，只标记 `sched_delayed`，并返回 `false`。正在运行的实体不在红黑树里（[set_next_entity()（fair.c#L5636-L5644）](../../linux/kernel/sched/fair.c#L5636-L5644)）。

同一次 `__schedule()` 随后挑选下一个任务，两条路径不同：

- 挑选走到这个 `cfs_rq`，并且 [pick_eevdf()（fair.c#L1026-L1027）](../../linux/kernel/sched/fair.c#L1026-L1027) 因 `nr_queued == 1` 跳过合格性检查、直接返回当前实体时，[pick_next_entity()（fair.c#L5687-L5693）](../../linux/kernel/sched/fair.c#L5687-L5693) 发现 `sched_delayed`，立刻完成真正出队并返回 `NULL`。它此前可能因负滞后进入 delayed 状态，但这一层只剩它一个入队实体时，加权平均虚拟时间就是它自身的 `vruntime`，不能据“跳过检查”推断它此时仍不合格（[avg_vruntime()（fair.c#L715-L749）](../../linux/kernel/sched/fair.c#L715-L749)）。上层还有其他组、这次没有走进来时，不会发生这件事。
- 没有选中它时，[put_prev_entity()（fair.c#L5712-L5715）](../../linux/kernel/sched/fair.c#L5712-L5715) 看到 `se->on_rq` 仍为 1，把它放回红黑树。特性注释（[features.h#L50-L54](../../linux/kernel/sched/features.h#L50-L54)）说，这样可以让不合格的任务留在竞争中还清负滞后，被选中时滞后已经为正。这描述的是走了合格性检查的路径；`nr_queued == 1` 会跳过检查，不能当成不变量。以后某次选择选中它时，同样由 `pick_next_entity()` 完成出队。

真正出队时若特性 `DELAY_ZERO` 开启（默认开启），正的 `vlag` 会被清成 0（[fair.c#L5545-L5546](../../linux/kernel/sched/fair.c#L5545-L5546)），负的 `vlag` 保留。若在真正出队之前被唤醒，`ttwu_runnable()` 以 `ENQUEUE_DELAYED` 调用 `requeue_delayed_entity()`，清掉 `sched_delayed` 并恢复为正常入队（4.4 节）。由于这一机制，核心层的 `dequeue_task()` 可能返回 `false`（[core.c#L2119-L2121](../../linux/kernel/sched/core.c#L2119-L2121)），此时任务仍然 `on_rq`。

**唤醒抢占。** [check_preempt_wakeup_fair()（fair.c#L8970-L9102）](../../linux/kernel/sched/fair.c#L8970-L9102) 在新任务入队后判断是否抢占。它先排除同一实体、被唤醒者已节流、当前任务已有立即重调度请求、`WAKEUP_PREEMPTION` 关闭等情况（[fair.c#L8978-L9004](../../linux/kernel/sched/fair.c#L8978-L9004)），再由 `find_matching_se()` 把两个实体对齐到同一层任务组。以下按默认特性说明，`NEXT_BUDDY` 默认关闭（[features.h#L32](../../linux/kernel/sched/features.h#L32)）：

- 当前实体是 `SCHED_IDLE`（任务组则是 idle 组），被唤醒者不是：直接 `resched_curr_lazy()`，不经过 `pick_eevdf()`（[fair.c#L9016-L9022](../../linux/kernel/sched/fair.c#L9016-L9022)）。`SCHED_NORMAL` 和 `SCHED_BATCH` 都会抢占这种当前实体。
- 只有被唤醒者是 idle、当前不是：直接返回，不抢占（[fair.c#L9025-L9026](../../linux/kernel/sched/fair.c#L9025-L9026)）。
- 被唤醒任务的策略不是 `SCHED_NORMAL`：直接返回（[fair.c#L9031-L9032](../../linux/kernel/sched/fair.c#L9031-L9032)）。因此 `SCHED_IDLE` 不抢占别人；`SCHED_BATCH` 不抢占 `SCHED_NORMAL` 或其他 `SCHED_BATCH`，但前面的分支已经让它可以抢占 idle 实体。
- 特性 `PREEMPT_SHORT` 开启（默认开启）且被唤醒者的 `slice` 更短：跳到选择步骤，这次比较不服从当前实体的保护期（[fair.c#L9040-L9042](../../linux/kernel/sched/fair.c#L9040-L9042)、[fair.c#L9078-L9079](../../linux/kernel/sched/fair.c#L9078-L9079)）。这时还没有修改 `vprot`；只有选中被唤醒者、确实请求抢占时才取消保护期。这个分支在 `WF_FORK` 判断之前，`slice` 更短的新任务仍会进入选择。
- 否则，`WF_FORK` 或被唤醒者仍处于 `sched_delayed`：直接返回，不再重新选择（[fair.c#L9051-L9052](../../linux/kernel/sched/fair.c#L9051-L9052)）。因此 slice 并不更短的新任务，不会按 EEVDF 抢走正在运行的普通任务。当前实体若是 idle，前面的强制抢占已经生效，走不到这里。
- 其余情况带保护期进行选择。调用 `pick_next_entity()` 选中的就是被唤醒者时，请求 `resched_curr_lazy()`；更短 `slice` 的分支汇合到同一次选择，但传入 `protect=false`，真正请求抢占前才 `cancel_protect_slice(se)`（[fair.c#L9077-L9101](../../linux/kernel/sched/fair.c#L9077-L9101)）。

EEVDF 的放置规则、保护期、延迟出队和权重变化见[公平调度类](cfs.md)。

### 3.4 实时类、deadline 类与 fair server

**实时类。** [__enqueue_rt_entity()（rt.c#L1326-L1358）](../../linux/kernel/sched/rt.c#L1326-L1358) 把实体挂到 `queue[prio]` 链表（默认尾部，`ENQUEUE_HEAD` 时头部），并在位图中置位。[pick_next_rt_entity()（rt.c#L1671-L1687）](../../linux/kernel/sched/rt.c#L1671-L1687) 用 `sched_find_first_bit()` 找到最小的置位下标，即最高优先级，取该链表的队首。选择时间与任务数无关。

- `SCHED_FIFO` 没有时间片，[task_tick_rt()（rt.c#L2530-L2531）](../../linux/kernel/sched/rt.c#L2530-L2531) 对它直接返回。它在阻塞、主动让出，或被唤醒抢占换下时让出 CPU。
- `SCHED_RR` 在 [task_tick_rt()（rt.c#L2517-L2549）](../../linux/kernel/sched/rt.c#L2517-L2549) 中每个 tick 递减 `time_slice`，减到 0 时重置为 `sched_rr_timeslice`；只有同优先级队列中还有其他实体时，才把它移到队尾并调用 `resched_curr()`（[rt.c#L2536-L2548](../../linux/kernel/sched/rt.c#L2536-L2548)），否则继续运行。默认值 `RR_TIMESLICE` 为 `100 * HZ / 1000` 个 tick（[rt.h#L84](../../linux/include/linux/sched/rt.h#L84)），在 `HZ=1000` 下是 100 ms。
- 唤醒时，[wakeup_preempt_rt()（rt.c#L1620-L1643）](../../linux/kernel/sched/rt.c#L1620-L1643) 看到被唤醒者 `prio` 更小就立即 `resched_curr()`。`prio` 相同、当前任务还能被其他 CPU 接住、而被唤醒者不能时，[check_preempt_equal_prio()（rt.c#L1571-L1591）](../../linux/kernel/sched/rt.c#L1571-L1591) 也会 `resched_curr()`，以便把当前任务推走。`FIFO` 和 `RR` 都走这个函数。

**deadline 类。** [pick_next_dl_entity()（deadline.c#L2556-L2564）](../../linux/kernel/sched/deadline.c#L2556-L2564) 取红黑树最左节点，即绝对截止时间最早的实体。每个 deadline 任务有运行预算、相对截止时间和周期三个参数（`dl_runtime`、`dl_deadline`、`dl_period`），通常在耗尽预算后节流，等待 `dl_timer` 补充（[deadline.c#L1480-L1505](../../linux/kernel/sched/deadline.c#L1480-L1505)）。设置或修改预约时要检查可用带宽，不足则返回 `-EBUSY`（[syscalls.c#L686-L694](../../linux/kernel/sched/syscalls.c#L686-L694)）；不能脱离准入与运行条件，把这些参数理解为任意情况下的执行保证。

**fair server。** 若只按 rt 高于 fair 的类优先级选择，持续可运行的实时任务可能使公平任务得不到 CPU。当前配置没有 RT 组带宽节流：`rt_rq_throttled()` 恒为假，运行预算检查也在 `CONFIG_RT_GROUP_SCHED` 内（[rt.c#L937-L956](../../linux/kernel/sched/rt.c#L937-L956)、[rt.c#L974-L1007](../../linux/kernel/sched/rt.c#L974-L1007)）。当前版本用一个 deadline 实体 `rq->fair_server` 为公平类提供预约：

- 初始参数在 [sched_init_dl_servers()（deadline.c#L1818-L1843）](../../linux/kernel/sched/deadline.c#L1818-L1843) 中设置：运行预算 50 ms、周期 1000 ms，并工作在 defer（推迟）模式。这不是公平类每秒最多只能运行 50 ms，正常 fair 优先级取得的时间也会消耗这份预算；
- 公平类从无任务变为有任务时，[enqueue_task_fair()（fair.c#L7168-L7173）](../../linux/kernel/sched/fair.c#L7168-L7173) 调用 `dl_server_start()`；
- 当 deadline 类选中的实体是服务器时，[__pick_task_dl()（deadline.c#L2583-L2589）](../../linux/kernel/sched/deadline.c#L2583-L2589) 调用它的 `server_pick_task`，即 [fair_server_pick_task()（fair.c#L9228-L9231）](../../linux/kernel/sched/fair.c#L9228-L9231)，从公平类中选任务，并记入 `rq->dl_server`。

defer 模式先把激活时刻推迟到 `deadline - runtime`（[start_dl_timer()（deadline.c#L1091-L1107）](../../linux/kernel/sched/deadline.c#L1091-L1107)）。公平任务正常运行会冲抵预算；预算在后台耗尽时，服务器启动新周期、再次推迟激活（[update_curr_dl_se()（deadline.c#L1446-L1477）](../../linux/kernel/sched/deadline.c#L1446-L1477)）。即使定时器回调已经发生，也可能因后台已消耗的预算而继续后移；确实需要激活时才设置 `dl_defer_running`，将服务器入队并检查抢占（[dl_server_timer()（deadline.c#L1177-L1200）](../../linux/kernel/sched/deadline.c#L1177-L1200)）。因此不能写成“只要公平任务得到过 CPU，定时器就不会触发”。状态机注释可帮助理解整体意图（[deadline.c#L1583-L1781](../../linux/kernel/sched/deadline.c#L1583-L1781)），具体状态和分支以上述实现为准。

正常公平运行如何冲抵预算，可见 [update_curr()（fair.c#L1310-L1321）](../../linux/kernel/sched/fair.c#L1310-L1321)：fair server 活跃时，即使任务不是以 server 身份运行，也调用 `dl_server_update()` 记账。

**stop 与 idle 类** 都只服务于每 CPU 的一个特殊任务，选择逻辑只是返回对应指针。stop 类的 `wakeup_preempt` 是空函数（“we're never preempted”，[stop_task.c#L24-L28](../../linux/kernel/sched/stop_task.c#L24-L28)）；idle 类的 `wakeup_preempt` 无条件 `resched_curr()`（[idle.c#L471-L474](../../linux/kernel/sched/idle.c#L471-L474)）。

### 3.5 何时切换：`need_resched` 与抢占点

调度器把“决定要切换”和“真正切换”分成两步。决定由 [__resched_curr()（core.c#L1113-L1147）](../../linux/kernel/sched/core.c#L1113-L1147) 记录为线程标志：

1. 目标 CPU 上当前运行的是 idle 任务时，LAZY 请求一律升级为 `TIF_NEED_RESCHED`：推迟切换没有意义，应立即让出 CPU 去做实际工作（[core.c#L1121-L1126](../../linux/kernel/sched/core.c#L1121-L1126)）；
2. 目标 CPU 上当前任务已有该标志或更强的 `TIF_NEED_RESCHED`，直接返回；
3. 目标就是本 CPU：设置 `TIF_NEED_RESCHED`（或 `TIF_NEED_RESCHED_LAZY`），对于前者还调用 `set_preempt_need_resched()`；
4. 目标是远程 CPU：原子地设置标志并检查目标 idle 任务是否在轮询（`TIF_POLLING_NRFLAG`）。若没有轮询且标志是 `TIF_NEED_RESCHED`，发送重调度 IPI；若在轮询，idle 循环自己会看到标志，省掉一次 IPI。

这里有两种强度的标志（[thread_info_tif.h#L21-L31](../../linux/include/asm-generic/thread_info_tif.h#L21-L31)）：`resched_curr()` 设置 `TIF_NEED_RESCHED`；`resched_curr_lazy()` 只在 lazy 抢占模型下设置 `TIF_NEED_RESCHED_LAZY`，其他模型下仍设置 `TIF_NEED_RESCHED`（[core.c#L1160-L1184](../../linux/kernel/sched/core.c#L1160-L1184)）。公平类在时间片到期（`update_curr()`，[fair.c#L1330](../../linux/kernel/sched/fair.c#L1330)）和唤醒抢占（[fair.c#L9101](../../linux/kernel/sched/fair.c#L9101)）时使用 lazy 版本；实时类和 deadline 类的唤醒抢占使用立即版本（[rt.c#L1624-L1627](../../linux/kernel/sched/rt.c#L1624-L1627)、[deadline.c#L2507-L2510](../../linux/kernel/sched/deadline.c#L2507-L2510)）。

**x86 的优化。** x86 把“需要重调度”额外折叠进每 CPU 的 `__preempt_count` 的最高位，并且**反向**存储：位为 1 表示不需要（[asm/preempt.h#L13-L19](../../linux/arch/x86/include/asm/preempt.h#L13-L19)）。这样 `preempt_enable()` 只需一条 `decl` 指令并检查结果是否为 0，就同时判断了“抢占计数归零”和“需要重调度”（[asm/preempt.h#L93-L104](../../linux/arch/x86/include/asm/preempt.h#L93-L104)）。远程 CPU 收到重调度 IPI 后，[scheduler_ipi()（linux/sched.h#L2011-L2019）](../../linux/include/linux/sched.h#L2011-L2019) 把线程标志折叠进 `__preempt_count`。

**抢占点与抢占模型。** 标志设置后，任务在以下位置之一进入调度器：

| 抢占点 | 动作 | 依据 |
| --- | --- | --- |
| 返回用户态 | `TIF_NEED_RESCHED` 或 `_LAZY` 任一置位就调用 `schedule()` | [entry/common.c#L19-L31](../../linux/kernel/entry/common.c#L19-L31) |
| 中断返回到内核态 | `irqentry_exit_cond_resched()` → `preempt_schedule_irq()` | [entry/common.c#L160-L170](../../linux/kernel/entry/common.c#L160-L170)、[entry/common.c#L210-L211](../../linux/kernel/entry/common.c#L210-L211)、[core.c#L7276-L7294](../../linux/kernel/sched/core.c#L7276-L7294) |
| `preempt_enable()` 使计数归零 | `__preempt_schedule()` → `preempt_schedule()` | [linux/preempt.h#L230-L235](../../linux/include/linux/preempt.h#L230-L235)、[core.c#L7161-L7170](../../linux/kernel/sched/core.c#L7161-L7170) |
| `cond_resched()`、`might_sleep()` | `__cond_resched()` → `preempt_schedule_common()`；`might_sleep()` 经 `might_resched()` 走同一函数 | [linux/sched.h#L2122-L2125](../../linux/include/linux/sched.h#L2122-L2125)、[kernel.h#L53-L62](../../linux/include/linux/kernel.h#L53-L62)、[kernel.h#L136](../../linux/include/linux/kernel.h#L136)、[core.c#L7493-L7516](../../linux/kernel/sched/core.c#L7493-L7516) |
| 显式调用 `schedule()` | 直接进入 | [core.c#L7048-L7060](../../linux/kernel/sched/core.c#L7048-L7060) |

返回用户态这一项在任何模型下都有效；其余各项是否生效由抢占模型决定。`PREEMPT_DYNAMIC` 用 static call 切换这些入口，切换发生在启动时，也可以在运行时经 debugfs 进行（第 0 节），[core.c#L7620-L7659](../../linux/kernel/sched/core.c#L7620-L7659) 的注释和 [__sched_dynamic_update()（core.c#L7707-L7767）](../../linux/kernel/sched/core.c#L7707-L7767) 给出了对应关系：

| 模型 | `cond_resched()` | `might_sleep()` 处让出 | `preempt_enable()` 抢占 | 中断返回内核态抢占 | 公平类使用 LAZY 标志 |
| --- | --- | --- | --- | --- | --- |
| none | 有效 | 否 | 否 | 否 | 否 |
| **voluntary（本配置默认）** | 有效 | 有效 | 否 | 否 | 否 |
| full | 空操作 | 否 | 是 | 是 | 否 |
| lazy | 空操作 | 否 | 是 | 是 | 是 |

因此在默认的 voluntary 模型下，一个在内核态长时间运行的任务不会在任意位置被抢占，只会在 `cond_resched()`、`might_sleep()` 标注点、显式 `schedule()` 或返回用户态时让出 CPU。lazy 模型下公平类通常设置 LAZY 标志，内核态代码不会因此被立即抢占；若后续本地 tick 到来时该标志仍在，[sched_tick()（core.c#L5621-L5622）](../../linux/kernel/sched/core.c#L5621-L5622) 会把它升级为 `TIF_NEED_RESCHED`。这不是固定 1 ms 内必然切换的保证：tick 可能停止或延迟，实际切换还受关抢占等条件约束。

### 3.6 在哪个 CPU 上运行：放置与负载均衡

调度器在两类时机决定任务的 CPU：

**放置**（placement）发生在任务刚变为可运行时：

- 唤醒：`try_to_wake_up()` 调用 [select_task_rq()（core.c#L3583-L3608）](../../linux/kernel/sched/core.c#L3583-L3608)。只有允许运行在多个 CPU 上且未禁止迁移时才询问调度类；结果若不在任务允许的 CPU 集合中，则用 `select_fallback_rq()` 兜底。
- fork：[wake_up_new_task()（core.c#L4849）](../../linux/kernel/sched/core.c#L4849) 以 `WF_FORK` 选 CPU。
- exec：[sched_exec()（core.c#L5462-L5479）](../../linux/kernel/sched/core.c#L5462-L5479) 以 `WF_EXEC` 选 CPU，若需要换 CPU，就请 stopper 线程把正在运行的自己迁走。

公平类的 [select_task_rq_fair()（fair.c#L8722-L8800）](../../linux/kernel/sched/fair.c#L8722-L8800) 自底向上遍历调度域：唤醒时通常走“快速路径”，在共享缓存的范围内找空闲 CPU（`select_idle_sibling()`）；fork 和 exec 时走“慢路径”，在设置了相应 `SD_BALANCE_*` 标志的调度域中找最空闲组里的最空闲 CPU。`WF_*` 标志的低位与 `SD_BALANCE_*` 数值相同（[sched.h#L2328-L2340](../../linux/kernel/sched/sched.h#L2328-L2340)），所以可以直接拿唤醒标志匹配调度域标志。放置与负载均衡的完整流程见[进程负载均衡](loadbalance.md)。

**负载均衡**（load balancing）在任务运行期间进行：

| 方式 | 触发 | 依据 |
| --- | --- | --- |
| 周期均衡 | `sched_tick()` → `sched_balance_trigger()`：到达 `rq->next_balance` 时触发 `SCHED_SOFTIRQ`，软中断处理函数自底向上遍历调度域 | [fair.c#L13257-L13270](../../linux/kernel/sched/fair.c#L13257-L13270)、[fair.c#L13234-L13252](../../linux/kernel/sched/fair.c#L13234-L13252)、[fair.c#L14194](../../linux/kernel/sched/fair.c#L14194) |
| 新空闲均衡 | CPU 即将空闲时，公平类在选择路径中调用 `sched_balance_newidle()` 拉任务 | [fair.c#L9201-L9218](../../linux/kernel/sched/fair.c#L9201-L9218) |
| NOHZ 空闲均衡 | 停了 tick 的空闲 CPU 无法自己做周期均衡。忙 CPU 在 tick 中调用 `nohz_balancer_kick()`，判断需要时由 `kick_ilb()` 选出一个空闲 CPU 并通过 IPI 唤醒它，由这个空闲 CPU 在 `SCHED_SOFTIRQ` 中替所有停了 tick 的空闲 CPU 做均衡 | [fair.c#L12576-L12582](../../linux/kernel/sched/fair.c#L12576-L12582)、[fair.c#L12651-L12660](../../linux/kernel/sched/fair.c#L12651-L12660)、[fair.c#L12609-L12645](../../linux/kernel/sched/fair.c#L12609-L12645)、[fair.c#L13269](../../linux/kernel/sched/fair.c#L13269)、[fair.c#L13238-L13247](../../linux/kernel/sched/fair.c#L13238-L13247) |
| 实时 / deadline push、pull | 实时类和 deadline 类按优先级或截止时间把任务推给更合适的 CPU，或从其他 CPU 拉过来 | 实时类 [balance_rt()（rt.c#L1594-L1614）](../../linux/kernel/sched/rt.c#L1594-L1614)；deadline 类的 `.balance = balance_dl`（[deadline.c#L3319](../../linux/kernel/sched/deadline.c#L3319)） |

### 3.7 运行队列时钟

调度器的记账使用运行队列自己的时钟，而不是直接读 `ktime_get()`。[update_rq_clock()（core.c#L846-L869）](../../linux/kernel/sched/core.c#L846-L869) 在持有 `rq->lock` 时读取 `sched_clock_cpu()`，把增量加到 `rq->clock`；[update_rq_clock_task()（core.c#L787-L844）](../../linux/kernel/sched/core.c#L787-L844) 再从增量中扣除中断时间（本配置未开启）和虚拟机 steal time（开启 `PARAVIRT_TIME_ACCOUNTING` 且运行时启用了 steal clock 时），结果加到 `rq->clock_task`。任务运行时间都按 `clock_task` 计算（[update_se()（fair.c#L1232-L1271）](../../linux/kernel/sched/fair.c#L1232-L1271)），这样被宿主机偷走的时间不会算到任务头上。

为了避免重复更新时钟，源码有两种相互独立的手段：

- **调用者显式跳过**。`enqueue_task()` 等函数接受 `ENQUEUE_NOCLOCK`/`DEQUEUE_NOCLOCK` 标志，调用者已经更新过时钟时传入，函数就不再更新（[core.c#L2098-L2099](../../linux/kernel/sched/core.c#L2098-L2099)）。
- **跳过请求**。`rq->clock_update_flags` 中的 `RQCF_REQ_SKIP`、`RQCF_ACT_SKIP` 两位（[sched.h#L1656-L1681](../../linux/kernel/sched/sched.h#L1656-L1681)）用于省掉紧挨着的两次更新。例如 `wakeup_preempt()` 已决定重调度时调用 `rq_clock_skip_update()` 设置 `RQCF_REQ_SKIP`（[core.c#L2228-L2233](../../linux/kernel/sched/core.c#L2228-L2233)）；随后 `__schedule()` 把标志左移一位，升级为 `RQCF_ACT_SKIP`，于是紧接着的 `update_rq_clock()` 直接返回（[core.c#L6869-L6872](../../linux/kernel/sched/core.c#L6869-L6872)、[core.c#L853-L854](../../linux/kernel/sched/core.c#L853-L854)）。

同一字段的 `RQCF_UPDATED` 位是调试用的，记录本次持锁以来是否更新过时钟。读取函数 [rq_clock()、rq_clock_task()（sched.h#L1683-L1706）](../../linux/kernel/sched/sched.h#L1683-L1706) 要求持锁，并且时钟已更新或正处于跳过状态，否则告警。

## 4. 实现主线

本节从三条路径展开实现：任务阻塞并切换走、任务被唤醒、tick 驱动抢占；最后补充任务生命周期中的其他调度点。

### 4.1 阻塞：从等待循环到 `schedule()`

内核中典型的睡眠写法是一个循环（见 [linux/sched.h#L201-L237](../../linux/include/linux/sched.h#L201-L237) 的注释）：

```c
/* 简化代码：等待某个条件成立 */
for (;;) {
	set_current_state(TASK_UNINTERRUPTIBLE);
	if (CONDITION)
		break;
	schedule();
}
__set_current_state(TASK_RUNNING);
```

这段只展示“设置状态 → 检查条件 → 调度”的顺序，不是完整的等待队列实现；共享条件还需采用所属对象的同步协议。`wait_event()` 的 [___wait_event（wait.h#L302-L327）](../../linux/include/linux/wait.h#L302-L327) 每轮调用 [prepare_to_wait_event()（wait.c#L290-L320）](../../linux/kernel/sched/wait.c#L290-L320)，在等待队列锁内把自己挂上等待队列并设置状态，然后检查条件，不成立就调用 `schedule()`，结束时 `finish_wait()` 清理；可中断的变体还处理信号返回值。

`set_current_state()` 用 `smp_store_mb()` 写状态（[linux/sched.h#L245-L250](../../linux/include/linux/sched.h#L245-L250)），保证“写状态”先于“读条件”。唤醒方先写条件，再在 `try_to_wake_up()` 中执行完整内存屏障后读 `p->__state`。两边配合，避免出现“等待方读到条件不成立、唤醒方读到状态还是 RUNNING”的丢失唤醒。

[schedule()（core.c#L7048-L7060）](../../linux/kernel/sched/core.c#L7048-L7060) 本身很薄：

1. 若任务确实要睡眠，调用 [sched_submit_work()（core.c#L6992-L7027）](../../linux/kernel/sched/core.c#L6992-L7027)：通知 workqueue 有工作线程要睡了（以便唤醒另一个线程维持并发度），并提交块设备 plug 中积压的 I/O，避免睡眠者持有未提交的请求导致死锁。
2. 调用 [__schedule_loop(SM_NONE)（core.c#L7039-L7046）](../../linux/kernel/sched/core.c#L7039-L7046)：关抢占调用 `__schedule()`，返回后若又被设置了 `need_resched` 就再来一次。

### 4.2 `__schedule()`：主调度函数

[__schedule()（core.c#L6817-L6974）](../../linux/kernel/sched/core.c#L6817-L6974) 的参数 `sched_mode` 取值见 [core.c#L6532-L6535](../../linux/kernel/sched/core.c#L6532-L6535)：`SM_NONE` 表示主动调用，`SM_PREEMPT` 表示抢占，`SM_IDLE` 来自 idle 循环。调用者必须已关抢占。

```mermaid
flowchart TD
    A["__schedule(sched_mode)<br/>prev = rq->curr"] --> B["关中断，rcu_note_context_switch<br/>rq_lock + smp_mb__after_spinlock"]
    B --> C["update_rq_clock"]
    C --> D{"sched_mode?"}
    D -->|"SM_IDLE 且 nr_running == 0"| P["next = prev，跳过选择"]
    D -->|"SM_NONE 且 prev->__state != 0"| E["try_to_block_task"]
    D -->|"其他：SM_PREEMPT、状态为 0 等"| F["不出队"]
    E --> E1{"signal_pending_state<br/>匹配睡眠类型的信号?"}
    E1 -->|"是"| E2["__state = TASK_RUNNING，不出队"]
    E1 -->|"否"| E3["block_task：dequeue_task(DEQUEUE_SLEEP)<br/>成功则 on_rq = 0"]
    E2 --> G
    E3 --> G
    F --> G["next = pick_next_task"]
    G --> H["清除 prev 的 need_resched"]
    P --> H
    H --> I{"prev != next?"}
    I -->|"是"| J["nr_switches++，rq->curr = next<br/>context_switch（其中释放 rq 锁）"]
    I -->|"否"| K["执行 balance callback，释放 rq 锁"]
```

关键步骤：

**加锁与屏障（[core.c#L6846-L6867](../../linux/kernel/sched/core.c#L6846-L6867)）。** 关中断后获取本 CPU 的 `rq->lock`，紧接着 `smp_mb__after_spinlock()`。注释说明这个屏障有两个用途：保证后面的 `signal_pending_state()` 不会与调用者之前的 `set_current_state(TASK_INTERRUPTIBLE)` 重排，避免与 `signal_wake_up()` 竞争；同时满足 `membarrier()` 系统调用对“从用户态进入后、写 `rq->curr` 前有完整屏障”的要求。

**决定是否出队（[core.c#L6883-L6900](../../linux/kernel/sched/core.c#L6883-L6900)）。** `prev->__state` 只读一次。`SM_IDLE` 和 `SM_PREEMPT` 都不会让 `prev` 出队；只有主动调用（当前配置下即 `SM_NONE`，另一个非抢占模式 `SM_RTLOCK_WAIT` 只在 `PREEMPT_RT` 下使用）且状态非 0 时，才调用 [try_to_block_task()（core.c#L6545-L6588）](../../linux/kernel/sched/core.c#L6545-L6588)：

- 若 [signal_pending_state()（signal.h#L408-L416）](../../linux/include/linux/sched/signal.h#L408-L416) 为真（可中断睡眠且有待处理信号，或带 `TASK_WAKEKILL` 的睡眠且有致命信号），把状态改回 `TASK_RUNNING`，不出队，任务会被重新选中并在等待循环中处理信号；
- 否则记录是否计入负载（不可中断、非 `TASK_NOLOAD`、非冻结），调用 [block_task()（core.c#L2171-L2175）](../../linux/kernel/sched/core.c#L2171-L2175)。它调用 `dequeue_task(DEQUEUE_SLEEP)`；若调度类真的出队了（公平类的延迟出队会返回 `false`），再调用 [__block_task()（sched.h#L2770-L2811）](../../linux/kernel/sched/sched.h#L2770-L2811) 更新 `nr_uninterruptible`、`nr_iowait`，最后用 `smp_store_release()` 把 `on_rq` 清零。

注意**抢占（`SM_PREEMPT`）时不出队**，即使 `prev->__state` 非 0。原因可以这样理解：任务可能刚执行完 `set_current_state(TASK_UNINTERRUPTIBLE)`、还没检查条件就被中断抢占，若此时把它出队，它就失去了再次运行、检查条件的机会。切换计数也据此区分：主动调用且状态非 0 时（无论最终是否出队）选择 `prev->nvcsw`，其余情况选择 `prev->nivcsw`（[core.c#L6874](../../linux/kernel/sched/core.c#L6874)、[core.c#L6899](../../linux/kernel/sched/core.c#L6899)）；只有真正换到另一个任务时才递增选中的计数（[core.c#L6919-L6921](../../linux/kernel/sched/core.c#L6919-L6921)、[core.c#L6953](../../linux/kernel/sched/core.c#L6953)）。

`__block_task()` 中的注释（[sched.h#L2782-L2809](../../linux/kernel/sched/sched.h#L2782-L2809)）强调：`on_rq = 0` 一旦写出，其他 CPU 上的 `try_to_wake_up()` 就可能立即把任务迁到别的 CPU，因此之后不能再引用该任务的运行队列状态。

**选择与切换（[core.c#L6902-L6963](../../linux/kernel/sched/core.c#L6902-L6963)）。** `pick_next_task()` 选出 `next`（3.2 节），然后清除 `prev` 的 `TIF_NEED_RESCHED` 与 `TIF_NEED_RESCHED_LAZY`（[linux/sched.h#L2066-L2070](../../linux/include/linux/sched.h#L2066-L2070)）以及 `__preempt_count` 中的对应位。若 `next != prev`，递增 `nr_switches`，用 `RCU_INIT_POINTER()` 更新 `rq->curr`，更新 PSI 统计和 tracepoint，调用 `context_switch()`，后者负责释放 `rq->lock`。

### 4.3 `context_switch()`：换地址空间、换栈

[context_switch()（core.c#L5292-L5353）](../../linux/kernel/sched/core.c#L5292-L5353) 依次完成：

1. [prepare_task_switch()（core.c#L5135-L5147）](../../linux/kernel/sched/core.c#L5135-L5147)：调度统计、perf、rseq、抢占通知等钩子，以及 [prepare_task(next)（core.c#L4956-L4966）](../../linux/kernel/sched/core.c#L4956-L4966) 把 `next->on_cpu` 置 1。
2. **切换地址空间**（[core.c#L5315-L5341](../../linux/kernel/sched/core.c#L5315-L5341)），按注释中的四种情况处理。这里“内核线程”实际按 `mm == NULL` 分支判断；当前 `CONFIG_MMU_LAZY_TLB_REFCOUNT=y`，`mmgrab_lazy_tlb()` 确实增加 `mm_count`，切回带 `mm` 的任务后归还（[sched/mm.h#L88-L112](../../linux/include/linux/sched/mm.h#L88-L112)）：

   | 切换方向 | 处理 |
   | --- | --- |
   | 内核线程 → 内核线程 | 不换页表，`next` 继承 `prev->active_mm`，`prev->active_mm` 清空，借用引用随之转交（lazy TLB：`next->mm` 为 `NULL`，切换到它时不调用 `switch_mm_irqs_off()`） |
   | 用户任务 → 内核线程 | 不换页表，`next` 借用 `prev->active_mm`，并 `mmgrab_lazy_tlb()` 增加引用 |
   | 内核线程 → 用户任务 | 调用 `switch_mm_irqs_off()`；被借用的 mm 记入 `rq->prev_mm`，在 `finish_task_switch()` 中释放引用 |
   | 用户任务 → 用户任务 | 调用 `switch_mm_irqs_off()`；两个任务共享同一个 mm 时不一定重载页表 |

   x86 的 `switch_mm_irqs_off()` 会检查当前已加载的 mm：同一 mm、非 lazy 等条件满足时可以直接返回，不把“调用地址空间切换接口”等同于“必然写 CR3”（[tlb.c#L840-L881](../../linux/arch/x86/mm/tlb.c#L840-L881)）。

3. [prepare_lock_switch()（core.c#L5065-L5080）](../../linux/kernel/sched/core.c#L5065-L5080)：`rq->lock` 由 `prev` 获取、却要由 `next` 释放，这对 lockdep 是非法操作，所以提前做一次 lockdep 层面的释放。
4. `switch_to(prev, next, prev)`：x86 上展开为 [__switch_to_asm（switch_to.h#L49-L52）](../../linux/arch/x86/include/asm/switch_to.h#L49-L52)。[entry_64.S#L177-L217](../../linux/arch/x86/entry/entry_64.S#L177-L217) 中，它把被调用者保存的寄存器压到 `prev` 的栈上，把 `rsp` 保存到 `prev->thread.sp`，从 `next->thread.sp` 恢复 `rsp`，弹出 `next` 当初保存的寄存器，再跳到 C 函数 [__switch_to()（process_64.c#L611）](../../linux/arch/x86/kernel/process_64.c#L611) 处理 FPU、段寄存器、TLS 等。
5. [finish_task_switch(prev)（core.c#L5168-L5256）](../../linux/kernel/sched/core.c#L5168-L5256)。

第 4 步之后执行的已经是 `next` 的栈。从 `next` 的视角看，它是从自己当初调用的 `switch_to()` 中“返回”的；`switch_to` 的第三个参数接收 `__switch_to()` 的返回值，即刚才被换下的任务（[process_64.c#L714](../../linux/arch/x86/kernel/process_64.c#L714)），并写回局部变量 `prev`，所以 `finish_task_switch(prev)` 中的 `prev` 指的是刚被换下的那个任务，而不是 `next` 很久以前保存的旧值。

`finish_task_switch()` 的主要工作：

- 读取 `prev->__state`，**然后**调用 [finish_task(prev)（core.c#L4968-L4982）](../../linux/kernel/sched/core.c#L4968-L4982) 用 `smp_store_release()` 把 `prev->on_cpu` 清零。顺序不能颠倒：一旦 `on_cpu` 清零，`prev` 就可能在另一个 CPU 上运行并改变状态（[core.c#L5199-L5203](../../linux/kernel/sched/core.c#L5199-L5203)）。
- [finish_lock_switch()（core.c#L5082-L5092）](../../linux/kernel/sched/core.c#L5082-L5092) 执行积压的 balance callback，释放 `rq->lock` 并开中断。
- 若 `rq->prev_mm` 非空，释放借用的 mm 引用。
- 若 `prev` 的状态是 `TASK_DEAD`，调用调度类的 `task_dead`，释放任务栈和 `current` 引用（[core.c#L5245-L5253](../../linux/kernel/sched/core.c#L5245-L5253)）。

新创建的任务没有“当初调用的 `switch_to()`”可以返回。x86 上 `copy_thread()` 把它栈上的返回地址设为汇编入口 `ret_from_fork_asm`（[process.c#L186](../../linux/arch/x86/kernel/process.c#L186)），第一次被换上 CPU 时从这里开始执行；`ret_from_fork_asm` 再调用 C 函数 `ret_from_fork()`（[entry_64.S#L228-L245](../../linux/arch/x86/entry/entry_64.S#L228-L245)），后者调用 [schedule_tail()（core.c#L5262-L5287）](../../linux/kernel/sched/core.c#L5262-L5287)，同样经由 `finish_task_switch()` 完成锁和统计的收尾（[process.c#L151-L154](../../linux/arch/x86/kernel/process.c#L151-L154)）。

### 4.4 唤醒：`try_to_wake_up()`

下文主线针对普通等待状态。当前也编入了 freezer：`ttwu_state_match()` 若只匹配 `saved_state`，会把保存状态改成 `TASK_RUNNING` 并报告成功，却不把冻结任务立即入队；真正恢复仍由解冻路径完成（[core.c#L2236-L2245](../../linux/kernel/sched/core.c#L2236-L2245)、[core.c#L4017-L4036](../../linux/kernel/sched/core.c#L4017-L4036)、[.config#L1129](../../linux/.config#L1129)）。因此“成功唤醒”的含义必须结合分支理解。

唤醒的难点在于：被唤醒的任务可能正处在睡眠过程的任何一步，甚至还在另一个 CPU 上执行 `__schedule()`；而唤醒方希望尽量只获取一把 `rq->lock`。[try_to_wake_up()（core.c#L4159-L4316）](../../linux/kernel/sched/core.c#L4159-L4316) 的注释（[core.c#L4139-L4154](../../linux/kernel/sched/core.c#L4139-L4154)）说明，它依靠 `p->pi_lock` 串行化并发唤醒；为了尽量只取一把 `rq->lock`，它在只持有 `p->pi_lock`、不持有任务所在 `rq->lock` 的情况下读取 `on_rq`、`on_cpu` 等状态，因此要靠大量内存屏障与 `__schedule()` 等路径排序。

下面的时序图描述一个跨 CPU 的完整唤醒：任务 X 在 CPU0 上睡眠，CPU1 唤醒它，选中 CPU2 运行，并且没有走唤醒链表。三个 CPU 并发执行，图中只是满足协议约束的一种可能交错，只保留与协议有关的步骤。这里的“没有走唤醒链表”包括第 5 步：X 当时 `on_cpu == 1`，但挂到 CPU0 唤醒链表的条件不成立，CPU1 才会等到切换结束再选 CPU。不走唤醒链表时，对 CPU2 运行队列 `rq2` 的加锁、入队和抢占判断都由 CPU1 自己完成（[core.c#L3983-L3986](../../linux/kernel/sched/core.c#L3983-L3986)、[core.c#L3722-L3725](../../linux/kernel/sched/core.c#L3722-L3725)），CPU2 并不参与；唯一发给 CPU2 的是需要抢占时可能发出的重调度 IPI（图中虚线）。

```mermaid
sequenceDiagram
    participant C0 as CPU0：X 正在睡眠
    participant C1 as CPU1：唤醒方
    participant C2 as CPU2：目标 CPU

    C0->>C0: set_current_state(TASK_UNINTERRUPTIBLE)
    C0->>C0: 检查 CONDITION == 0，准备 schedule
    C1->>C1: 写入 CONDITION = 1
    C0->>C0: schedule()：lock rq0，block_task<br/>smp_store_release(X->on_rq, 0)
    C1->>C1: lock X->pi_lock，smp_mb__after_spinlock<br/>X->__state 匹配，smp_rmb，读 X->on_rq == 0<br/>smp_acquire__after_ctrl_dep
    C1->>C1: X->__state = TASK_WAKING
    C0->>C0: context_switch 到 Y<br/>finish_task：smp_store_release(X->on_cpu, 0)
    C0->>C0: finish_lock_switch：释放 rq0 锁
    C1->>C1: smp_cond_load_acquire(X->on_cpu == 0)
    C1->>C1: select_task_rq → CPU2，set_task_cpu(X, 2)
    C1->>C1: ttwu_queue：lock rq2
    C1->>C1: 在 rq2 上 activate_task：X->on_rq = 1
    C1->>C1: wakeup_preempt：判断是否抢占 CPU2 的当前任务
    C1-->>C2: 需要抢占时 resched_curr 设置标志，必要时发重调度 IPI
    C1->>C1: ttwu_do_wakeup：X->__state = TASK_RUNNING
    C1->>C1: unlock rq2，unlock X->pi_lock
```

逐步说明：

1. **唤醒自己**（[core.c#L4166-L4189](../../linux/kernel/sched/core.c#L4166-L4189)）。`p == current` 说明任务还没进入 `__schedule()` 的出队步骤，只需在状态匹配时写回 `TASK_RUNNING`。
2. **加 `pi_lock` 并匹配状态**（[core.c#L4197-L4200](../../linux/kernel/sched/core.c#L4197-L4200)）。`smp_mb__after_spinlock()` 与等待方 `set_current_state()` 中的屏障配对。`ttwu_state_match()` 检查 `p->__state & state`，不匹配说明任务不在我们要唤醒的状态（例如用 `TASK_INTERRUPTIBLE` 去唤醒一个 `TASK_UNINTERRUPTIBLE` 任务），直接返回 0。
3. **任务仍在队列上**（[core.c#L4226-L4228](../../linux/kernel/sched/core.c#L4226-L4228)）。`on_rq` 非 0 时调用 [ttwu_runnable()（core.c#L3775-L3799）](../../linux/kernel/sched/core.c#L3775-L3799)：锁住任务所在的运行队列，若任务确实还在队列上（等待方还没走到出队，或处于延迟出队状态），就处理延迟出队、必要时检查抢占，写回 `TASK_RUNNING`。这是最便宜的情况：任务从未离开运行队列。
4. **任务已出队**。写入 `TASK_WAKING`（[core.c#L4261](../../linux/kernel/sched/core.c#L4261)），表示这次唤醒已被认领。
5. **任务还没在原 CPU 上切换完**（`on_cpu == 1`）。这一步在 `select_task_rq()` **之前**（[core.c#L4282-L4295](../../linux/kernel/sched/core.c#L4282-L4295)）。先调用 [ttwu_queue_wakelist()（core.c#L3964-L3973）](../../linux/kernel/sched/core.c#L3964-L3973)，CPU 参数是任务仍在执行的 `task_cpu(p)`，不是第 6 步选出的目标 CPU。是否入链用的是第 7 步同一个 `ttwu_queue_cond()`，只是作用在这个原 CPU 上。条件成立就把任务挂到原 CPU 的唤醒链表并返回，由原 CPU 在切换结束后自己入队，唤醒方不必自旋等待。条件不成立时，用 `smp_cond_load_acquire()` 等待 `on_cpu` 变为 0，它与 `finish_task()` 中的 `smp_store_release()` 配对，保证任务在旧 CPU 上的全部操作对唤醒方可见，然后才进入第 6 步。
6. **选择 CPU**（[core.c#L4297-L4307](../../linux/kernel/sched/core.c#L4297-L4307)）。`select_task_rq()` 返回目标 CPU；与当前不同就设置 `WF_MIGRATED` 并 `set_task_cpu()`。`task_cpu` 的修改规则（[core.c#L620-L629](../../linux/kernel/sched/core.c#L620-L629)）允许 `try_to_wake_up()` 在只持有 `p->pi_lock` 时调用 `set_task_cpu()`：任务此时已出队，不属于任何运行队列，而 `pi_lock` 排除了并发唤醒。
7. **入队**。[ttwu_queue()（core.c#L3975-L3987）](../../linux/kernel/sched/core.c#L3975-L3987) 有两种做法：
   - **唤醒链表**。还要特性 `TTWU_QUEUE` 开启；当前不是 `PREEMPT_RT`，它默认开启（[features.h#L82](../../linux/kernel/sched/features.h#L82)）。[ttwu_queue_cond()（core.c#L3915-L3962）](../../linux/kernel/sched/core.c#L3915-L3962) 先排除不适用的情况（stop 类任务、目标 CPU 不处于 active 状态、任务不允许在目标 CPU 上运行），然后在两种情况下选择唤醒链表：目标 CPU 与当前 CPU 不共享 LLC；或二者共享 LLC，但目标不是当前 CPU，且它的 `nr_running` 为 0。这里的目标 CPU 是第 6 步选出的 CPU。此时任务被挂到该 CPU 的链表并（必要时）发送 IPI，目标 CPU 在 [sched_ttwu_pending()（core.c#L3801-L3836）](../../linux/kernel/sched/core.c#L3801-L3836) 中自己加锁入队，避免远程访问目标运行队列造成缓存行来回迁移。发送 IPI 前，[kernel/smp.c#L117](../../linux/kernel/smp.c#L117) 调用 [call_function_single_prep_ipi()（core.c#L3844-L3852）](../../linux/kernel/sched/core.c#L3844-L3852)，若目标 CPU 的 idle 任务正在轮询就省掉 IPI。
   - 否则直接锁目标运行队列，调用 [ttwu_do_activate()（core.c#L3701-L3748）](../../linux/kernel/sched/core.c#L3701-L3748)。

`ttwu_do_activate()` 是两种做法的汇合点：`activate_task(ENQUEUE_WAKEUP)` 调用调度类的 `enqueue_task` 并把 `on_rq` 置为 `TASK_ON_RQ_QUEUED`（[core.c#L2143-L2154](../../linux/kernel/sched/core.c#L2143-L2154)）；[wakeup_preempt()（core.c#L2219-L2234）](../../linux/kernel/sched/core.c#L2219-L2234) 判断是否抢占当前任务——同一调度类交给该类的 `wakeup_preempt`，被唤醒者所属类更高则直接 `resched_curr()`；最后 `ttwu_do_wakeup()` 把状态写为 `TASK_RUNNING`。

`try_to_wake_up()` 返回 1 表示状态匹配、这次唤醒已接受，返回 0 表示没有匹配成功（[core.c#L4197-L4203](../../linux/kernel/sched/core.c#L4197-L4203)、[core.c#L4311-L4315](../../linux/kernel/sched/core.c#L4311-L4315)）。走唤醒链表时，返回 1 的当下任务仍可能是 `TASK_WAKING`，要等目标 CPU 处理链表才变为 `TASK_RUNNING`；返回成功也不代表已开始执行。唤醒**不会**直接调用 `schedule()`。如果需要抢占，目标 CPU 会在可用的抢占点重新选择任务。

### 4.5 tick 驱动的抢占

时间子系统每个 tick 在 [update_process_times()（timer.c#L2467-L2482）](../../linux/kernel/time/timer.c#L2467-L2482) 中调用（调用点 [timer.c#L2479](../../linux/kernel/time/timer.c#L2479)）[sched_tick()（core.c#L5597-L5646）](../../linux/kernel/sched/core.c#L5597-L5646)。它运行在硬中断上下文，主要工作：

1. 加 `rq->lock`，更新运行队列时钟；
2. lazy 模型下，若当前任务已带有 `TIF_NEED_RESCHED_LAZY`（之前的请求至今还没被处理），升级为 `resched_curr()`；
3. 调用当前任务所属调度类的 `task_tick()`；
4. 更新全局负载统计等，释放锁；
5. 记录本 CPU 是否空闲，调用 `sched_balance_trigger()` 判断是否触发负载均衡软中断。

以公平类为例，[task_tick_fair()（fair.c#L13588-L13605）](../../linux/kernel/sched/fair.c#L13588-L13605) 沿实体层级向上调用 `entity_tick()`，其中的 `update_curr()` 累加 `vruntime`，在队列中还有其他实体、且时间片耗尽或已不在保护期时调用 `resched_curr_lazy()`（[fair.c#L1326-L1332](../../linux/kernel/sched/fair.c#L1326-L1332)，3.3 节）。实时类的 `SCHED_RR` 在时间片耗尽且同优先级还有其他任务时调用 `resched_curr()`（3.4 节）。

以 LAPIC 提供本地 tick 为例，下面是一次 tick 抢占的概览，省略 clockevent、hrtimer 等中间分发：LAPIC 中断调用事件处理器（[apic.c#L1041-L1058](../../linux/arch/x86/kernel/apic/apic.c#L1041-L1058)），高精度 tick 的回调再经 `tick_sched_handle()` 调用 `update_process_times()`（[tick-sched.c#L253-L297](../../linux/kernel/time/tick-sched.c#L253-L297)）。编译启用高精度定时器不代表每台机器运行时必然选择该硬件路径。

```text
LAPIC 定时器中断
  → tick 处理 → update_process_times() → sched_tick()
      → task_tick_fair() → entity_tick() → update_curr() → resched_curr_lazy()
          （voluntary、full 模型下设置 TIF_NEED_RESCHED；lazy 模型下只设置 TIF_NEED_RESCHED_LAZY）
  → 中断退出
      → 返回用户态：exit_to_user_mode_loop() → schedule()（两种标志都会触发）
      → 返回内核态：voluntary 模型下不抢占，等到下一个 cond_resched()/might_sleep()/返回用户态
                    full 模型下 preempt_schedule_irq() → __schedule(SM_PREEMPT)
                    lazy 模型下本次不抢占：need_resched() 只检查 TIF_NEED_RESCHED；
                      下一个 tick 的 sched_tick() 把 LAZY 升级为 TIF_NEED_RESCHED 后，
                      那次中断返回时若抢占条件满足，才 preempt_schedule_irq()
```

lazy 一行的依据是：中断返回内核态时 [raw_irqentry_exit_cond_resched()（entry/common.c#L160-L170）](../../linux/kernel/entry/common.c#L160-L170) 用 `need_resched()` 判断，而 [need_resched()（linux/sched.h#L2231-L2234）](../../linux/include/linux/sched.h#L2231-L2234) 经 [tif_need_resched()（thread_info.h#L206-L209）](../../linux/include/linux/thread_info.h#L206-L209) 只测试 `TIF_NEED_RESCHED`；升级发生在 `sched_tick()` 调用 `task_tick()` 之前（[core.c#L5621-L5624](../../linux/kernel/sched/core.c#L5621-L5624)）。上面的链条省略了“返回用户态前先发生其他抢占点”等情况。

### 4.6 任务生命周期中的其他调度点

下图回答“一个任务在调度器眼中经历哪些状态”。这是概念模型：`__state` 与 `on_rq` 被合并成一个状态，省略了延迟出队、迁移中和冻结等中间态。

```mermaid
stateDiagram-v2
    state "TASK_NEW<br/>sched_fork 初始化" as NEW
    state "可运行，在队列中<br/>RUNNING, on_rq=1, on_cpu=0" as READY
    state "运行中<br/>RUNNING, on_rq=1, on_cpu=1" as RUN
    state "睡眠<br/>__state!=0, on_rq=0" as SLEEP
    state "唤醒中<br/>TASK_WAKING" as WAKING
    state "TASK_DEAD" as DEAD

    [*] --> NEW: copy_process
    NEW --> READY: wake_up_new_task
    READY --> RUN: __schedule 选中
    RUN --> READY: 被抢占或让出
    RUN --> SLEEP: set_current_state + schedule
    SLEEP --> WAKING: try_to_wake_up 认领
    WAKING --> READY: ttwu_do_activate
    RUN --> DEAD: do_task_dead
    DEAD --> [*]: finish_task_switch 释放引用
```

**fork。** `copy_process()` 在 [fork.c#L2155](../../linux/kernel/fork.c#L2155) 调用 [sched_fork()（core.c#L4695-L4764）](../../linux/kernel/sched/core.c#L4695-L4764)：把子任务状态设为 `TASK_NEW`，保证此时任何唤醒都不会把它放进队列；`prio` 取父任务的 `normal_prio`，避免把父任务被优先级继承临时提升的优先级传给子任务；若设置了 `sched_reset_on_fork`，把实时/deadline 策略降回 `SCHED_NORMAL`；经过这一步后若 `prio` 仍是 deadline 优先级（即 deadline 任务未设置 reset-on-fork），返回 `-EAGAIN`，fork 失败；否则按优先级设置 `sched_class`。随后在 [fork.c#L2299](../../linux/kernel/fork.c#L2299) 调用 [sched_cgroup_fork()（core.c#L4766-L4795）](../../linux/kernel/sched/core.c#L4766-L4795) 确定任务组并调用调度类的 `task_fork`。最后 [wake_up_new_task()（core.c#L4831-L4867）](../../linux/kernel/sched/core.c#L4831-L4867) 在 [fork.c#L2642](../../linux/kernel/fork.c#L2642) 被调用：写入 `TASK_RUNNING`，选 CPU，以 `ENQUEUE_INITIAL` 首次入队，检查是否抢占。

**修改调度属性。** [__sched_setscheduler()（syscalls.c#L531）](../../linux/kernel/sched/syscalls.c#L531) 用 `task_rq_lock()` 锁住任务（[syscalls.c#L609](../../linux/kernel/sched/syscalls.c#L609)），然后使用 `sched_change` 模式（[sched.h#L3902-L3933](../../linux/kernel/sched/sched.h#L3902-L3933)）：[sched_change_begin()（core.c#L10911-L10931）](../../linux/kernel/sched/core.c#L10911-L10931) 记录任务是否在队列上、是否正在运行，若是则 `dequeue_task()`、`put_prev_task()`；在此期间修改 `sched_class`、`policy`、`prio` 等（[syscalls.c#L713-L738](../../linux/kernel/sched/syscalls.c#L713-L738)）；[sched_change_end()（core.c#L10933-L10944）](../../linux/kernel/sched/core.c#L10933-L10944) 再按原状态 `enqueue_task()`、`set_next_task()`。之后 [check_class_changed()（core.c#L2206-L2217）](../../linux/kernel/sched/core.c#L2206-L2217) 调用旧类的 `switched_from`、新类的 `switched_to` 或 `prio_changed`，让新类判断是否需要抢占。这种“先摘下、改属性、再挂回”的做法保证调度类的队列从不看到处于半修改状态的任务。

**exit。** [do_exit()（exit.c#L1020）](../../linux/kernel/exit.c#L1020) 最后调用 [do_task_dead()（core.c#L6976-L6990）](../../linux/kernel/sched/core.c#L6976-L6990)：用 `set_special_state(TASK_DEAD)` 设置状态，再调用 `__schedule(SM_NONE)`，此后永不返回。任务被出队并切走，下一个任务在 `finish_task_switch()` 中看到 `prev_state == TASK_DEAD`，释放它的栈和引用（4.3 节）。这样安排的原因可以理解为：任务不能在仍在使用的栈上释放自己的栈，所以由下一个任务代为完成。

**idle 循环。** 每个 CPU 启动完成后进入 [cpu_startup_entry()（idle.c#L443-L450）](../../linux/kernel/sched/idle.c#L443-L450)，无限循环调用 [do_idle()](../../linux/kernel/sched/idle.c#L276-L383)：在 `need_resched()` 为假时进入 cpuidle（[idle.c#L298-L354](../../linux/kernel/sched/idle.c#L298-L354)），一旦为真，就处理积压的 IPI 回调（包括唤醒链表），然后调用 `schedule_idle()`（调用点 [idle.c#L379](../../linux/kernel/sched/idle.c#L379)）切换到新任务。[schedule_idle()（core.c#L7073-L7086）](../../linux/kernel/sched/core.c#L7073-L7086) 以 `SM_IDLE` 模式调用 `__schedule()`。

## 5. 执行上下文与并发小结

| 操作 | 执行上下文 | 持有的锁 / 同步方式 |
| --- | --- | --- |
| `__schedule()` | 进程上下文，关抢占、关中断 | 本 CPU 的 `rq->lock`；发生任务切换时由 `prev` 获取、`next` 释放，不切换时本任务释放 |
| `try_to_wake_up()` | 进程、软中断或硬中断等上下文；不能据此推广到任意 NMI 场景 | 通常持 `p->pi_lock`，按分支锁任务原运行队列或目标队列；自唤醒是特例，唤醒链表则由目标 CPU 稍后锁队列；`on_rq`、`on_cpu` 用 acquire/release 和控制依赖排序 |
| 唤醒链表处理 `sched_ttwu_pending()` | 目标 CPU 的 IPI 处理或 idle 循环 | 本 CPU 的 `rq->lock` |
| `sched_tick()` | 硬中断 | 本 CPU 的 `rq->lock` |
| 周期负载均衡 | `SCHED_SOFTIRQ` 软中断 | 公平类先持源运行队列锁摘下任务并标记 `TASK_ON_RQ_MIGRATING`，释放后再持目标运行队列锁挂上（[fair.c#L12104-L12125](../../linux/kernel/sched/fair.c#L12104-L12125)）；调度域用 RCU 读侧遍历 |
| 修改策略、亲和性、nice | 进程上下文（系统调用） | `task_rq_lock()`：`p->pi_lock` → `rq->lock` |
| 迁移正在运行的任务 | 任务所在 CPU 上的 stopper 线程（stop 类，先把该任务抢占下来） | `migration_cpu_stop()` 持 `p->pi_lock` 和 `rq->lock` 后移动任务（[core.c#L2543-L2592](../../linux/kernel/sched/core.c#L2543-L2592)） |
| `rq->curr` 的无锁读取 | 满足对应 RCU 读侧条件的上下文 | RCU 保障所读任务的生命周期，不保证它始终仍是当前任务，也不替代字段自身的同步协议（[core.c#L6922-L6925](../../linux/kernel/sched/core.c#L6922-L6925)） |

几条贯穿全章的不变量：

- **锁顺序**：`p->pi_lock` 先于 `rq->lock`；需要同时持有多个 `rq->lock` 时（如 `double_rq_lock()`、实时类的 push/pull）按 CPU 编号升序（`rq_order_less()`，2.3 节）。实时类 push/pull 使用的 `double_lock_balance()` 在 `CONFIG_PREEMPTION=y` 下先释放本地锁，再调用 `double_rq_lock()` 按序重新加锁（[sched.h#L2957-L2976](../../linux/kernel/sched/sched.h#L2957-L2976)）。
- **单写者**：除未发布的新任务初始化外，`p->on_rq` 在持有任务当前运行队列锁时修改，代码中用 `ASSERT_EXCLUSIVE_WRITER()` 标注（[core.c#L2152-L2153](../../linux/kernel/sched/core.c#L2152-L2153)、[core.c#L4458](../../linux/kernel/sched/core.c#L4458)）。
- **`on_cpu` 的交接点**：`finish_task()` 清零后，仍可运行的 `prev` 可以被迁到其他 CPU，所以旧 CPU 必须在此前完成对其调度状态的读取（[core.c#L4968-L4982](../../linux/kernel/sched/core.c#L4968-L4982)）。注释的“最后引用”不是说后面绝无任何 `prev` 访问：`TASK_DEAD` 不会重新运行，`finish_task_switch()` 仍要调用 `task_dead` 并释放其栈和引用（[core.c#L5245-L5253](../../linux/kernel/sched/core.c#L5245-L5253)）。
- **迁移的程序顺序**：任务迁移后，它在旧 CPU 上的所有操作必须先于在新 CPU 上的执行。对可运行任务的迁移由两把运行队列锁的 release/acquire 链保证；对睡眠后唤醒的任务由 `on_cpu` 的 release/acquire 保证（[core.c#L4039-L4120](../../linux/kernel/sched/core.c#L4039-L4120)）。

## 6. 后续章节路线

按照本章的分层，后续章节建议按以下顺序展开：

1. **核心路径细节**：`__schedule()` 与 `context_switch()` 的完整实现，x86 `__switch_to()`，lazy TLB 与 mm 引用。
2. **唤醒与并发协议**：`try_to_wake_up()` 的全部分支、唤醒链表、`TASK_WAKING`、`set_special_state()` 与信号的竞争、`wake_q`。
3. **公平调度类（EEVDF）**：`place_entity()` 与滞后保持、保护期与 next buddy、延迟出队、`update_curr()` 与记账。已有章节：[公平调度类](cfs.md)。
4. **PELT 负载跟踪与 CPU 容量**：`sched_avg` 的几何级数、`util_est`、与 schedutil 调频的配合。
5. **负载均衡**：调度域的构建、`sched_balance_rq()` 的流程、唤醒放置（`select_idle_sibling()`）、NOHZ 空闲均衡。已有章节：[进程负载均衡](loadbalance.md)。
6. **实时调度类与 deadline 调度类**：优先级数组、push/pull、RT 节流、CBS、准入控制和 fair server。
7. **抢占模型与 `nohz_full`**：`PREEMPT_DYNAMIC` 的 static call 切换、lazy 抢占、tick 依赖与远程 tick。

组调度与带宽控制已在 [cgroup v2 的 cpu 控制器](../cgroup2/cpu.md) 中讨论。

## 7. 回顾

本章建立的主线可以概括为：

- **两层分工**：核心层（`core.c`）负责锁、任务状态、时钟、抢占标志和上下文切换；调度类通过 `struct sched_class` 函数表负责队列、记账、选择和抢占判断。5 个调度类由链接器按 stop > deadline > rt > fair > idle 排列，比较优先级就是比较地址。
- **一个每 CPU 的运行队列**：`struct rq` 中嵌入 `cfs_rq`、`rt_rq`、`dl_rq` 三个子队列和代表公平类的 `fair_server`，由同一把 `rq->__lock` 保护；任务通过嵌入的 `se`、`rt`、`dl` 实体挂到对应子队列上。
- **三个状态变量**：`__state`（想不想运行）、`on_rq`（在不在队列上）、`on_cpu`（在不在 CPU 上）各自独立变化。睡眠与唤醒的正确性依赖它们之间的内存序：`set_current_state()` 与唤醒方的屏障配对防止丢失唤醒，`on_rq` 和 `on_cpu` 的 release/acquire 保证迁移前后的程序顺序。
- **决定与执行分离**：唤醒和 tick 只通过 `resched_curr()` 设置 `TIF_NEED_RESCHED`（或 LAZY 标志），真正的切换发生在抢占点。当前配置默认使用 voluntary 模型，内核态只在 `cond_resched()`、`might_sleep()`、显式 `schedule()` 和返回用户态时切换。
- **一次切换的完整过程**：`__schedule()` 加锁、更新时钟、主动睡眠时尝试出队（信号和延迟出队是例外）→ 选出 `next`（满足条件时走公平类快速路径，否则按类询问）→ 仅在 `next != prev` 时由 `context_switch()` 按需切换地址空间并换栈 → `finish_task_switch()` 清除 `prev->on_cpu`、释放锁，必要时回收已退出任务。

把这些对象放回 4.4 节的跨 CPU 直接唤醒例子里看（不含自唤醒、仍在队列上或唤醒链表分支）：CPU1 上 `wake_up()` → `try_to_wake_up()` 持 `X->pi_lock` 认领任务 → 等 `X->on_cpu` 清零 → `select_task_rq()` 选 CPU2 → CPU1 锁住 CPU2 的运行队列并 `activate_task()` → `wakeup_preempt()` 判断应当抢占时设置 CPU2 当前任务的 `need_resched`（必要时发 IPI）→ CPU2 在可用的抢占点进入 `__schedule()`。只有选择规则最终选中 X 时，才切换到 X；成功唤醒不保证它立即执行。后续各章都是在这条主线的某一段上展开。
