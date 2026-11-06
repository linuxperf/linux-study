# 1.7 start_kernel：通用初始化与执行环境

> **本节源码**：[`init/main.c`](../../linux/init/main.c)、[`mm/mm_init.c`](../../linux/mm/mm_init.c)、[`kernel/fork.c`](../../linux/kernel/fork.c)、[`kernel/sched/core.c`](../../linux/kernel/sched/core.c)
> **关键函数**：`start_kernel()`、`parse_early_param()`、`mm_core_init()`、`sched_init()`、`rest_init()`
> **前置知识**：1.5 内核早期入口、1.6 平台与内存视图

## 概述

架构入口解决了基本取指与启动数据访问，通用内核仍需要建立分配器、调度、中断、时间和各类对象缓存。`start_kernel()` 把这些初始化按依赖关系串起来。它的大部分前段在本地中断关闭的条件下执行，直到能够安全处理异步事件才开放中断。末尾的 `rest_init()` 再把单一启动执行流分成 idle、PID 1 和内核线程管理者。

## 核心数据与状态

| 对象 | 职责 | 位置 |
|---|---|---|
| `boot_command_line` | 保存早期启动命令行 | [`init/main.c:143-158`](../../linux/init/main.c#L143-L158) |
| `saved_command_line` | 保存供输出及后续解析使用的完整命令行 | 同上；构造见 [`init/main.c:605-658`](../../linux/init/main.c#L605-L658) |
| `static_command_line` | 供参数解析器修改的命令行副本 | 同上 |
| `early_boot_irqs_disabled` | 标记早期中断关闭阶段 | [`init/main.c:122-129`](../../linux/init/main.c#L122-L129) |
| `kthreadd_done` | 使 PID 1 等待内核线程管理者就绪的完成量 | [`init/main.c:670-719`](../../linux/init/main.c#L670-L719) |

参数解析会切分字符串，不能让解析器破坏唯一保留副本。bootconfig 还可能提供额外参数，故 `/proc/cmdline` 与最初的 `boot_command_line` 不应无条件视为逐字相同。

## 主流程分析

```text
start_kernel()                         init/main.c
├── local_irq_disable()
├── boot_cpu_init()
├── setup_arch()                       arch/x86/kernel/setup.c
├── setup_boot_config() / setup_command_line()
├── setup_per_cpu_areas()
├── parse_early_param() / parse_args()
├── mm_core_init()                     mm/mm_init.c
├── ftrace_init() / early_trace_init()
├── sched_init()                       kernel/sched/core.c
├── workqueue_init_early() / rcu_init()
├── trace_init()
├── init_IRQ() / timers_init() / timekeeping_init()
├── local_irq_enable()
├── console_init()
├── 进程、VFS、命名空间等基础初始化
└── rest_init()
    ├── user_mode_thread(kernel_init)  kernel/fork.c
    ├── kernel_thread(kthreadd)        kernel/fork.c
    ├── complete(kthreadd_done)
    ├── schedule_preempt_disabled()
    └── cpu_startup_entry()            kernel/sched/idle.c
```

### 命令行不是只解析一次

`setup_arch()` 已可能调用 `parse_early_param()`；通用流程再次调用时，静态 `done` 标志避免重复解析。其实现先复制原始命令行，再只调用标为早期参数的处理函数，见 [`init/main.c:722-756`](../../linux/init/main.c#L722-L756)。

后续 `parse_args()` 处理普通内核参数和未知选项。`--` 之后的内容会转为 init 参数；部分未识别选项也按规则放入 init 的参数或环境，而不是一概被丢弃，见 [`init/main.c:480-563`](../../linux/init/main.c#L480-L563)、[`init/main.c:908-919`](../../linux/init/main.c#L908-L919)。分级 initcall 阶段还会重新按参数级别解析，1.9 再展开。

### 分配器先于大量动态对象初始化

`setup_arch()` 完成早期物理范围准备后，通用流程在 `mm_core_init()` 中推进内存管理的基础初始化，然后才能依靠更完整的分配能力建立大量运行期对象。调用位置见 [`init/main.c:926-938`](../../linux/init/main.c#L926-L938)。其中 `memblock_free_all()` 向伙伴系统交接适合释放的内存，随后执行 `mem_init()`、`kmem_cache_init()` 与 `vmalloc_init()` 等，见 [`mm/mm_init.c:2708-2758`](../../linux/mm/mm_init.c#L2708-L2758)。其内部细节属于 [4.2 内存初始化](../04-memory/02-init.md)，本节重点是它在全局顺序中的位置。

`vfs_caches_init_early()` 与后面的 `vfs_caches_init()` 也不是同一初始化重复执行：前者满足早期缓存准备，后者建立更多依赖分配器的 VFS 对象与挂载基础。

### 调度器先于开放中断

源码直接说明先初始化调度器，是因为随后可能出现定时器中断等异步事件：

[`init/main.c:939-944`](../../linux/init/main.c#L939-L944)（v6.18.60）

```c
	/*
	 * Set up the scheduler prior starting any interrupts (such as the
	 * timer interrupt). Full topology setup happens at smp_init()
	 * time - but meanwhile we still have a functioning scheduler.
	 */
	sched_init();
```

之后建立工作队列（Workqueue）早期框架、RCU、事件跟踪、中断入口、tick、定时器和时间维护设施，才执行 `local_irq_enable()`，见 [`init/main.c:955-1013`](../../linux/init/main.c#L955-L1013)。该操作打开启动 CPU 的本地中断，不意味着每个硬件设备都已配置完，也不意味着其他 CPU 已经启动。

工作队列此时只允许创建队列、提交和取消工作；工作项实际执行还依赖后面的 `workqueue_init()` 与内核线程条件。源码注释见 [`init/main.c:960-965`](../../linux/init/main.c#L960-L965)。

### 普通控制台晚于日志产生

`linux_banner` 在前段就通过 `pr_notice()` 进入日志，而普通 `console_init()` 出现在开放中断之后，见 [`init/main.c:891-892`](../../linux/init/main.c#L891-L892)、[`init/main.c:1017-1022`](../../linux/init/main.c#L1017-L1022)。所以屏幕显示一批日志的时刻不一定是这些日志实际生成的时刻。早期控制台用于缩小这个观测空窗。

### `rest_init()` 建立后续初始化的任务环境

当前版本先用 `user_mode_thread(kernel_init, NULL, CLONE_FS)` 创建 PID 1，再用 `kernel_thread()` 创建 `kthreadd`。PID 1 暂时固定在启动 CPU，避免完整 SMP 调度初始化前迁移，见 [`init/main.c:678-699`](../../linux/init/main.c#L678-L699)。

随后发布 `SYSTEM_SCHEDULING` 并完成 `kthreadd_done`。原始启动任务至少调度一次，再进入启动 CPU 的 idle 循环：

[`init/main.c:708-718`](../../linux/init/main.c#L708-L718)（v6.18.60）

```c
	system_state = SYSTEM_SCHEDULING;

	complete(&kthreadd_done);

	/*
	 * The boot idle thread must execute schedule()
	 * at least once to get things moving:
	 */
	schedule_preempt_disabled();
	/* Call into cpu_idle with preempt disabled */
	cpu_startup_entry(CPUHP_ONLINE);
```

创建 PID 1 在前、允许它继续依赖内核线程管理者的初始化在后，靠完成量明确衔接。不能只凭创建顺序假设调度器绝不会提前运行新任务。

## 关键细节 / 设计考量

### 为什么不把全部启动工作放在 `start_kernel()`

后续初始化需要睡眠、等待设备、启动内核线程与访问文件。把这部分交给可调度的 PID 1，上下文边界更清晰。`kernel_init_freeable()` 会先恢复完整的 GFP 允许掩码，再准备 SMP 和工作队列，见 [`init/main.c:1524-1553`](../../linux/init/main.c#L1524-L1553)。

### 为什么 `rest_init()` 不直接作为普通 `__init` 代码处理

启动任务与 PID 1 已经并发推进。若 PID 1 开始释放初始化段，而启动任务尚未离开其中，就会出现代码被回收的竞态。函数前面的注释和 `noinline`、`__ref` 标记直接说明了这个生命周期问题，见 [`init/main.c:661-672`](../../linux/init/main.c#L661-L672)。

## 可观测性

| 入口 | 适合观察的内容 | 依据与限制 |
|---|---|---|
| `Kernel command line` 日志、`/proc/cmdline` | 参数是否真正传入当前内核；先排除编辑了错误启动项 | [`init/main.c:906-912`](../../linux/init/main.c#L906-L912)、[`fs/proc/cmdline.c:7-24`](../../linux/fs/proc/cmdline.c#L7-L24)；`CONFIG_PRINTK=y`、`CONFIG_PROC_FS=y` |
| `earlyprintk` 或 `earlycon` | 普通控制台注册前的内核阶段日志 | x86 注册见 [`arch/x86/kernel/early_printk.c:449`](../../linux/arch/x86/kernel/early_printk.c#L449)，`earlycon` 用法见 [`Documentation/admin-guide/kernel-parameters.txt`](../../linux/Documentation/admin-guide/kernel-parameters.txt)；当前早期 printk 与串口控制台支持为 y，硬件参数须匹配 |
| `sched:sched_switch` | 调度已运行后的任务切换 | [`include/trace/events/sched.h`](../../linux/include/trace/events/sched.h)；`CONFIG_EVENT_TRACING=y`，必须提前启用，无法覆盖调度器建立之前 |
| ftrace 启动跟踪 | `ftrace_init()` 之后部分可插桩函数 | 初始化位置见 [`init/main.c:931-978`](../../linux/init/main.c#L931-L978)；`CONFIG_FUNCTION_TRACER=y`，不能覆盖所有早期入口 |

`initcall_debug` 只记录经过 initcall 框架的函数。`start_kernel()` 里直接调用的 `mm_core_init()`、`sched_init()` 不会因为加了这个选项就自动各自变成 initcall 记录。

## 小结

- `start_kernel()` 的顺序体现分配器、调度、中断和时间基础之间的依赖。
- 中断开放是启动 CPU 执行条件变化，不是全机所有设备已就绪。
- 命令行有早期、普通和分级参数解析阶段，保留副本与可修改副本分工不同。
- `rest_init()` 创建 PID 1 与 `kthreadd`，通过完成量协调启动，并让原始任务进入 idle。

## 参考

- [`Documentation/admin-guide/bootconfig.rst`](../../linux/Documentation/admin-guide/bootconfig.rst)。
- [`init/main.c`](../../linux/init/main.c)。
- [3.1 调度器框架](../03-scheduler/01-overview.md)、[1.11 PID 1](11-userspace.md)。
