# 1.8 SMP：启动其他处理器

> **本节源码**：[`kernel/smp.c`](../../linux/kernel/smp.c)、[`kernel/cpu.c`](../../linux/kernel/cpu.c)、[`arch/x86/kernel/smpboot.c`](../../linux/arch/x86/kernel/smpboot.c)、[`arch/x86/kernel/head_64.S`](../../linux/arch/x86/kernel/head_64.S)、[`arch/x86/realmode/rm/trampoline_64.S`](../../linux/arch/x86/realmode/rm/trampoline_64.S)
> **关键函数**：`smp_init()`、`bringup_nonboot_cpus()`、`native_kick_ap()`、`do_boot_cpu()`、`start_secondary()`
> **前置知识**：1.5 64 位入口、1.7 可调度执行环境；APIC 基本概念可参阅 5.5

## 概述

对称多处理（Symmetric Multiprocessing，SMP）系统的通用启动前段主要由启动处理器（Bootstrap Processor，BSP）推进，其他应用处理器（Application Processor，AP）不能简单地从 `start_kernel()` 再执行一遍。它们需要自己的 idle 任务、栈、每 CPU 状态和中断环境，再加入已经运行的系统。Linux 把启动过程接入 CPU 热插拔（CPU Hotplug）状态机，以复用准备、同步和上线步骤。当前配置还支持并行唤起优化，但并非所有上线工作都同时完成。

## 核心数据与状态

### 三个 CPU 集合

| 集合 | 含义 | 启动时的用途 |
|---|---|---|
| `cpu_possible_mask` | 内核为其预留支持能力的 CPU 编号集合 | 影响每 CPU 分配与编号范围 |
| `cpu_present_mask` | 当前系统中被识别为存在的 CPU 集合 | 非启动 CPU 的启动候选 |
| `cpu_online_mask` | 已上线、可参与相应运行期活动的 CPU 集合 | 启动过程逐步扩展 |

这些不是永久相等的集合。用户可见输出直接引用对应掩码，见 [`drivers/base/cpu.c:207-232`](../../linux/drivers/base/cpu.c#L207-L232)。`CONFIG_NR_CPUS=8192` 是编译期容量；dev 虚拟机配置 4 个 vCPU，不会因此启动 8192 个 CPU。

### 每 CPU 上下文与启动同步

`common_cpu_up()` 把目标 CPU 的 `current_task` 指向它自己的 idle 任务，设置栈金丝雀并准备中断栈。`do_boot_cpu()` 设置目标栈和 `initial_code = start_secondary`，见 [`arch/x86/kernel/smpboot.c:814-860`](../../linux/arch/x86/kernel/smpboot.c#L814-L860)。

AP 在早期使用 `cpuhp_ap_sync_alive()` 报告自己已到达同步点，并等待控制 CPU 放行。CPU “有响应”和“已 online”是不同状态，不能把收到启动 IPI 当作完整上线成功。

## 主流程分析

下面把控制 CPU 与 AP 分开列出；热插拔框架中间还有状态推进和回调间接调用，不把它们压成直接函数调用。

```text
PID 1：kernel_init_freeable()           init/main.c
├── smp_prepare_cpus()
├── do_pre_smp_initcalls()
├── smp_init()                         kernel/smp.c
│   ├── idle_threads_init()
│   ├── cpuhp_threads_init()
│   └── bringup_nonboot_cpus()         kernel/cpu.c
│       └── 热插拔状态机准备与唤起 AP
│           └── native_kick_ap()       arch/x86/kernel/smpboot.c
│               ├── common_cpu_up()
│               └── do_boot_cpu()
└── sched_init_smp()                   kernel/sched/core.c

目标 AP：低地址跳板                     arch/x86/realmode/rm/trampoline_64.S
└── secondary_startup_64               arch/x86/kernel/head_64.S
    └── start_secondary()              arch/x86/kernel/smpboot.c
        ├── cpuhp_ap_sync_alive()
        ├── cpu_init()
        ├── ap_starting()
        ├── set_cpu_online()
        └── cpu_startup_entry()        kernel/sched/idle.c
```

### 控制 CPU 先为 AP 准备软件资源

`smp_init()` 先建立 idle 线程和热插拔线程，再启动非启动 CPU，最后报告上线节点和 CPU 数，见 [`kernel/smp.c:992-1010`](../../linux/kernel/smp.c#L992-L1010)。`smp_prepare_cpus()` 以及允许阻塞分配的工作已经在 PID 1 上下文完成，不是在最初的裸入口中动态创建全部资源。

### 常规唤醒使用 INIT 与 STARTUP 消息

`do_boot_cpu()` 优先使用 APIC 驱动提供的 64 位唤醒方法，再考虑普通唤醒方法，最后才回退到 `wakeup_secondary_cpu_via_init()`，见 [`arch/x86/kernel/smpboot.c:842-905`](../../linux/arch/x86/kernel/smpboot.c#L842-L905)。因此“所有 AP 必经同一实模式唤醒方式”并不准确。

普通回退路径先发送 INIT 序列，再按硬件条件发送 STARTUP IPI。启动向量由跳板物理地址右移 12 位形成，见 [`arch/x86/kernel/smpboot.c:695-765`](../../linux/arch/x86/kernel/smpboot.c#L695-L765)。低地址跳板负责进入可用的执行模式，并转到 `secondary_startup_64`；BSP 的压缩镜像不需要在每个 AP 上重新解压。

### AP 不运行 `x86_64_start_kernel()`

`secondary_startup_64` 检查 CPU、清除 BSP 专用的参数指针，使用已经准备好的内核页表。公共汇编根据 CPU 编号选择每 CPU 偏移与栈，最后通过 `initial_code` 进入 `start_secondary()`，见 [`arch/x86/kernel/head_64.S:147-198`](../../linux/arch/x86/kernel/head_64.S#L147-L198)、[`arch/x86/kernel/head_64.S:318-420`](../../linux/arch/x86/kernel/head_64.S#L318-L420)。

AP 的 C 入口执行异常处理基础与微码加载，到达 alive 同步点后才继续每 CPU 状态、RCU、时钟和中断相关准备，见 [`arch/x86/kernel/smpboot.c:232-295`](../../linux/arch/x86/kernel/smpboot.c#L232-L295)。

### online 发布与中断向量必须协调

上线尾段如下：

[`arch/x86/kernel/smpboot.c:303-315`](../../linux/arch/x86/kernel/smpboot.c#L303-L315)（v6.18.60）

```c
	lock_vector_lock();
	set_cpu_online(smp_processor_id(), true);
	lapic_online();
	unlock_vector_lock();
	x86_platform.nmi_init();

	/* enable local interrupts */
	local_irq_enable();

	x86_cpuinit.setup_percpu_clockev();

	wmb();
	cpu_startup_entry(CPUHP_AP_ONLINE_IDLE);
```

把 online 状态与中断向量分配器更新放在同一锁保护下，可避免并发中断设置看到半初始化状态。AP 随后进入 idle 并继续配合热插拔框架完成更高层状态推进。`set_cpu_online()` 是重要发布点，但不应把它误解为所有子系统回调已经无条件结束。

## 关键细节 / 设计考量

### 并行启动不是取消同步

当前 `CONFIG_HOTPLUG_PARALLEL=y`。通用框架可先把多个 AP 推进到唤起阶段，再逐步完成上线；有 SMT 时先处理主线程，以避免微码更新期间的兄弟线程问题，见 [`kernel/cpu.c:1832-1892`](../../linux/kernel/cpu.c#L1832-L1892)。

`cpuhp.parallel=` 可控制是否尝试此优化，架构还可以拒绝。它只改变部分启动时序，不取消 alive 握手、每 CPU 数据准备和最终上线约束。

### CPU 数量参数控制不同层次

`maxcpus=` 主要限制启动时上线的 CPU 数；`nr_cpus=` 影响可支持的 CPU 编号范围；`nosmp` / `maxcpus=0` 还会触发架构相关处理，见 [`kernel/smp.c:935-977`](../../linux/kernel/smp.c#L935-L977)。实验中不要把三个参数视为无副作用的同义词。

## 可观测性

| 入口 | 看什么 | 源码与条件 |
|---|---|---|
| 启动日志 `Bringing up secondary CPUs`、`Brought up ...` | 请求启动阶段和最终上线数；不是所有 possible CPU 必须上线 | [`kernel/smp.c:999-1009`](../../linux/kernel/smp.c#L999-L1009)，`CONFIG_SMP=y`、`CONFIG_PRINTK=y` |
| `/sys/devices/system/cpu/possible`、`present`、`online`、`offline`、`kernel_max` | 比较编译容量、可用编号与实际上线集合 | [`drivers/base/cpu.c:207-275`](../../linux/drivers/base/cpu.c#L207-L275)，`CONFIG_SYSFS=y` |
| `cpuhp:cpuhp_enter` / `cpuhp:cpuhp_multi_enter` / `cpuhp:cpuhp_exit` | `cpu` 是目标 CPU；入口含 `target`、状态下标 `idx` 和回调 `fun`，出口含返回码 `ret` | [`include/trace/events/cpuhp.h:10-90`](../../linux/include/trace/events/cpuhp.h#L10-L90)，`CONFIG_EVENT_TRACING=y` |

启动时的 cpuhp 事件需提前启用，例如在临时启动项中追加 `trace_event=cpuhp:*`；登录以后再启用只能记录后续事件。`start_secondary()` 带 `notrace`，不能把它当作普通 fentry 目标；其定义见 [`arch/x86/kernel/smpboot.c:232`](../../linux/arch/x86/kernel/smpboot.c#L232)。

## 动手实验

在保留旧启动项的 dev 虚拟机中，用一次性 `maxcpus=1` 启动，与默认配置比较下列输出。此实验未实测，且不需要主动下线 CPU：

```bash
uname -r
for name in possible present online offline kernel_max; do
    printf '%s: ' "$name"
    head -n 1 "/sys/devices/system/cpu/$name"
done
dmesg | grep -E 'Bringing up|Brought up|Booting SMP'
```

`possible` 与 `present` 是否变化要依据实际平台和参数处理判断，不预设它们必定与 `online` 一起缩为一个 CPU。

## 小结

- AP 复用已解压内核与共享基础设施，只建立自己的执行状态。
- CPU 唤起、alive 握手、online 发布与最终状态推进是不同步骤。
- 热插拔状态机统一组织启动回调，支持带同步边界的并行优化。
- possible、present、online 和编译期 `NR_CPUS` 不能互相替代。

## 参考

- [`Documentation/core-api/cpu_hotplug.rst`](../../linux/Documentation/core-api/cpu_hotplug.rst)。
- [`arch/x86/realmode/rm/trampoline_64.S`](../../linux/arch/x86/realmode/rm/trampoline_64.S)。
- [5.5 中断控制器](../05-interrupt/05-apic.md)。
