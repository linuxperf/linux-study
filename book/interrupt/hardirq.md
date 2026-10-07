# 硬中断：从向量入口到驱动回调返回

CPU 正在执行某段代码时，设备中断到来。内核要在这次打断里做完三件事：记住自己已经处在硬中断上下文，根据向量找到这条 Linux IRQ 的描述符，再按控制器类型决定屏蔽、确认和调用驱动的顺序。驱动回调返回之后，还要恢复描述符状态，并在最外层硬中断结束时决定要不要接着跑软中断。

[中断子系统概述](overview.md) 已经用 e1000e 把“通知如何找到驱动”串起来。本章不再重复编号体系和 NAPI，只分析**硬中断**这条立即执行的路径：上下文怎样建立，`irq_desc` 上的状态怎样约束流控，以及注册、屏蔽、重发和退出各改哪些字段。

本章回答：

1. 硬中断上下文靠什么标记？处理函数返回时，本地中断必须处于什么状态？
2. x86-64 的 IDT 与 FRED 怎样汇合到同一个 `common_interrupt()`？
3. `irq_desc` 里的禁用深度、屏蔽位、`IRQD_IRQ_INPROGRESS` 和 `IRQS_PENDING` 各表示什么？
4. 电平、边沿和 fasteoi 三种流控为什么顺序不同？x86 上 EOI 落在驱动回调之前还是之后？
5. `request_irq()` 怎样把 `irqaction` 接上并启动 IRQ？`disable_irq()` 为什么可以先不写硬件屏蔽？
6. 硬中断退出时，什么条件下会在当前栈上执行软中断？默认抢占模型会不会在返回内核时抢占？

## 0. 分析基线

本章依据仓库 [Makefile 第 2～6 行](../../linux/Makefile#L2-L6) 标记的 **6.18.52**，架构为 **x86-64**。读者需要了解可屏蔽中断、中断门和“不能在原子上下文睡眠”这些概念，并读过概述章对 Linux IRQ 号、`irq_desc` 与 `irqaction` 的介绍。

下面的配置决定本章走哪条实现。它们是编译条件，不表示某次运行一定经过对应分支。

| 配置 | 对本章的影响 | 依据 |
| --- | --- | --- |
| `CONFIG_X86_64=y`、`CONFIG_SMP=y` | 使用 64 位入口和每 CPU 的 `vector_irq` | [.config 第 333 行](../../linux/.config#L333)、[.config 第 362 行](../../linux/.config#L362) |
| `CONFIG_SPARSE_IRQ=y` | `irq_to_desc()` 从 maple tree `sparse_irqs` 查找描述符，描述符动态分配 | [.config 第 84 行](../../linux/.config#L84)、[irqdesc.c 第 168 行](../../linux/kernel/irq/irqdesc.c#L168)、[irqdesc.c 第 414～417 行](../../linux/kernel/irq/irqdesc.c#L414-L417) |
| `CONFIG_IRQ_DOMAIN=y`、`CONFIG_IRQ_DOMAIN_HIERARCHY=y` | 一个 Linux IRQ 可以有多层 `irq_data`，最外层嵌入 `irq_desc` | [.config 第 78～79 行](../../linux/.config#L78-L79) |
| `CONFIG_X86_LOCAL_APIC=y`、`CONFIG_X86_IO_APIC=y`、`CONFIG_PCI_MSI=y` | 设备向量由 vector 域分配；I/O APIC 与 MSI 使用不同的 `irq_chip` | [.config 第 433 行](../../linux/.config#L433)、[.config 第 435 行](../../linux/.config#L435)、[.config 第 2190 行](../../linux/.config#L2190) |
| `CONFIG_X86_FRED=y` | FRED 入口被编译进来。CPU 不具备该特性时，启动代码仍安装传统 IDT 设备门 | [.config 第 368 行](../../linux/.config#L368)、[irqinit.c 第 100～105 行](../../linux/arch/x86/kernel/irqinit.c#L100-L105) |
| `CONFIG_IRQ_REMAP=y` | 中断重映射芯片被编译进来。I/O APIC 的父域是 vector 域时用普通芯片，否则用重映射芯片 | [.config 第 8721 行](../../linux/.config#L8721)、[io_apic.c 第 2889～2890 行](../../linux/arch/x86/kernel/apic/io_apic.c#L2889-L2890) |
| `CONFIG_PREEMPT_RT` 未设置，`CONFIG_PREEMPT_VOLUNTARY=y`，`CONFIG_PREEMPT_DYNAMIC=y`，`CONFIG_PREEMPTION=y`，`CONFIG_PREEMPT_COUNT=y` | 硬中断计数在 `preempt_count` 里。默认动态抢占模型是 voluntary，中断返回内核时的条件抢占调用被改成空操作 | [.config 第 136～142 行](../../linux/.config#L136-L142)、[core.c 第 7636～7641 行](../../linux/kernel/sched/core.c#L7636-L7641)、[core.c 第 7791～7795 行](../../linux/kernel/sched/core.c#L7791-L7795) |
| `CONFIG_IRQ_FORCED_THREADING=y` | 编入强制线程化。默认静态键关闭，只有启动参数 `threadirqs` 才打开 | [.config 第 83 行](../../linux/.config#L83)、[manage.c 第 27～35 行](../../linux/kernel/irq/manage.c#L27-L35) |
| `CONFIG_HARDIRQS_SW_RESEND=y` | 硬件 retrigger 失败时，可以编入 tasklet 重发。经过 vector 域的 IRQ 带 `IRQD_HANDLE_ENFORCE_IRQCTX`，会拒绝这条路径，见第 7.4 节 | [.config 第 76 行](../../linux/.config#L76) |
| `CONFIG_GENERIC_IRQ_EFFECTIVE_AFF_MASK=y`、`CONFIG_GENERIC_PENDING_IRQ=y` | 有效 affinity 与推迟的 affinity 迁移被编译进来 | [.config 第 72～73 行](../../linux/.config#L72-L73) |
| `CONFIG_HAVE_IRQ_EXIT_ON_IRQ_STACK=y` | 硬中断退出时若要跑软中断，直接在当前栈上调用 `__do_softirq()`；`threadirqs` 且 `ksoftirqd` 已存在时改为唤醒它 | [.config 第 940 行](../../linux/.config#L940)、[softirq.c 第 487～507 行](../../linux/kernel/softirq.c#L487-L507) |
| `CONFIG_VMAP_STACK=y`、`CONFIG_PAGE_SHIFT=12`、`CONFIG_KASAN` 未设置 | 每 CPU 中断栈经 vmap 映射，带保护页。`IRQ_STACK_ORDER` 为 2，栈大小为 16 KiB | [.config 第 951 行](../../linux/.config#L951)、[.config 第 954 行](../../linux/.config#L954)、[.config 第 967 行](../../linux/.config#L967)、[.config 第 10607 行](../../linux/.config#L10607)、[page_64_types.h 第 21～22 行](../../linux/arch/x86/include/asm/page_64_types.h#L21-L22)、[irq_64.c 第 32～54 行](../../linux/arch/x86/kernel/irq_64.c#L32-L54) |
| `CONFIG_DEBUG_SHIRQ`、`CONFIG_PROVE_LOCKING` 未设置 | 释放共享 IRQ 时不会额外假调一次处理函数。`lockdep_assert_irqs_disabled()` 在本配置中是空操作 | [.config 第 10618 行](../../linux/.config#L10618)、[.config 第 10659 行](../../linux/.config#L10659)、[lockdep.h 第 630 行](../../linux/include/linux/lockdep.h#L630) |
| `CONFIG_X86_POSTED_MSI` 未设置 | 不分析 posted MSI 通知向量。`FIRST_SYSTEM_VECTOR` 仍取 `0xeb`，它只是系统向量区的起点 | [.config 第 365 行](../../linux/.config#L365)、[irq_vectors.h 第 104～109 行](../../linux/arch/x86/include/asm/irq_vectors.h#L104-L109) |

运行时还有两个默认开关。`force_irqthreads()` 在非 RT 上是静态键，初值为假，`threadirqs` 才把它打开（[manage.c 第 27～35 行](../../linux/kernel/irq/manage.c#L27-L35)）。动态抢占在未指定 `preempt=` 时因为 `CONFIG_PREEMPT_VOLUNTARY` 进入 voluntary，此时 `irqentry_exit_cond_resched` 被关掉（[core.c 第 7732～7737 行](../../linux/kernel/sched/core.c#L7732-L7737)）。本章以这两个默认值为准。

NMI、系统向量（本地 APIC 定时器、IPI）和软中断回调本身不在本章展开。系统向量有自己的入口，不经过设备 IRQ 的 `irq_desc` 流控。

## 1. 硬中断要在被打断的上下文里做完

### 1.1 它解决的是“现在就要处理”的那一段

设备把通知送到某个 CPU 的某个向量之后，这段工作还没有任务可调度。内核直接打断当前执行，在硬中断上下文里识别来源、操作控制器、调用已经注册的主处理函数。主处理函数要自己完成必须立刻做的设备操作，或者把后续工作标记出去。

通用层把这种上下文写成一条明确的约束。[`preempt.h` 的注释](../../linux/include/linux/preempt.h#L21-L25)说明：处理函数在中断关闭时运行，因此正常情况下硬中断不会嵌套；仍保留 4 位计数，是因为少数处理函数会重新打开中断。处理函数一旦打开中断，[`__handle_irq_event_percpu()`](../../linux/kernel/irq/handle.c#L208-L210)会发出警告，并立刻再执行 `local_irq_disable()`。

因此硬中断主处理函数不能睡眠，不能获取 mutex，也不能调用可能睡眠的分配或等待。`current` 仍是被打断的任务，这个指针不表示现在处于该任务的进程上下文。

### 1.2 三层函数各管一段顺序

概述章已经区分过这三层。本章后面的算法都围绕它们：

| 层 | 代码里的位置 | 这一层决定什么 |
| --- | --- | --- |
| 架构入口 | `common_interrupt()` | 建立入口状态、硬中断计数，用向量找到 `irq_desc` |
| 流控函数 | `desc->handle_irq` | 屏蔽、ACK、EOI 与调用 `irqaction` 的先后，以及处理过程中又来一次请求时记在哪里 |
| 驱动回调 | `action->handler` | 判断是否是自己的设备，并处理设备状态 |

[`generic_handle_irq_desc()`](../../linux/include/linux/irqdesc.h#L171-L174)只有一句 `desc->handle_irq(desc)`。x86-64 的 [`handle_irq()`](../../linux/arch/x86/kernel/irq.c#L250-L257)在 64 位配置下直接调用它。

### 1.3 本章停在哪里

向量号、Linux IRQ 号和 MSI 消息怎样在准备阶段对应起来，见概述章第 2 节。本章从“描述符和向量映射已经建立”开始，分析一次中断到达之后的状态变化。线程化 IRQ 的线程函数、软中断的分类回调，只在它们与硬中断退出或 `IRQ_WAKE_THREAD` 相接的地方说明。

## 2. 一次设备中断的主路径

下面按 x86-64 上一次已经注册好的设备 IRQ 来看。图中的流控以边沿为例；电平与 fasteoi 的差别在第 5 节。FRED 与 IDT 在进入 `common_interrupt()` 之前不同，返回被打断上下文时用的指令也不同。函数内部的硬中断记账相同。第 4.2 节和第 8.2 节分别说明这两处。

```text
CPU 接受外部中断，向量落在设备向量范围
  → IDT：irq_entries_start 压入向量，进入 asm_common_interrupt
    或 FRED：fred_extint() 直接调用 common_interrupt(regs, vector)
    → irqentry_enter()
    → 必要时换到每 CPU 硬中断栈
    → irq_enter_rcu()                 preempt_count 加上 HARDIRQ_OFFSET
    → __common_interrupt()
      → 本 CPU 的 vector_irq[vector] 得到 irq_desc
      → desc->handle_irq(desc)
        → 流控函数操作 irq_chip，遍历 action->handler
    → irq_exit_rcu()                  减去 HARDIRQ_OFFSET，条件满足时执行软中断
    → irqentry_exit()                 返回用户态可调度；返回内核时先看 exit_rcu，再按抢占模型决定
```

这是概念顺序，省略了失败分支和锁。依据是 [idtentry.h 的 `DEFINE_IDTENTRY_IRQ`](../../linux/arch/x86/include/asm/idtentry.h#L206-L222)、[irq.c 的 `common_interrupt()`](../../linux/arch/x86/kernel/irq.c#L318-L329)和 [entry_fred.c 的 `fred_extint()`](../../linux/arch/x86/entry/entry_fred.c#L159-L176)。

设备向量的范围由 vector 分配器限定。[`arch_early_irq_init()` 的矩阵](../../linux/arch/x86/kernel/apic/vector.c#L812-L817)在 `FIRST_EXTERNAL_VECTOR`（`0x20`，[irq_vectors.h 第 36 行](../../linux/arch/x86/include/asm/irq_vectors.h#L36)）与 `FIRST_SYSTEM_VECTOR` 之间分配。本配置里后者是 `0xeb`（[irq_vectors.h 第 104～109 行](../../linux/arch/x86/include/asm/irq_vectors.h#L104-L109)）。`0xeb` 及以上是系统向量区，本地定时器和 IPI 走那里的专用入口，不进入上面的 `desc->handle_irq`。

## 3. 描述符把“这条 IRQ 现在处于哪一步”记下来

### 3.1 `irq_desc` 是流控和注册的汇合点

[`struct irq_desc`](../../linux/include/linux/irqdesc.h#L67-L120)里，本章主要使用这些成员：

| 成员 | 作用 |
| --- | --- |
| `irq_data` | 最外层控制器数据，内嵌在描述符里。`irq` 是 Linux IRQ 号，`chip` 是这层的 `irq_chip`，`parent_data` 指向父层 |
| `handle_irq` | 流控函数 |
| `action` | `irqaction` 链表 |
| `lock` | `raw_spinlock_t`。流控拿着它修改状态；调用驱动前会放开，避免驱动再去拿同一把锁时死锁 |
| `depth` | 软件禁用的嵌套次数。0 表示软件希望这条 IRQ 处于启用状态 |
| `istate` | 内核内部状态。字段本名是 `core_internal_state__do_not_mess_with_it`，`kernel/irq` 内部用宏把它写成 `istate`（[internals.h 第 20 行](../../linux/kernel/irq/internals.h#L20)） |
| `kstat_irqs`、`tot_count` | 每 CPU 计数和描述符上的累计计数 |
| `threads_oneshot`、`threads_active` | 线程化处理尚未结束时的掩码和计数。非线程化主路径里前者保持 0 |
| `request_mutex` | 串行化 `request_irq()` / `free_irq()`，不在中断处理热路径上使用 |

`irq_data.common` 指向同一个描述符里的 `irq_common_data`（[desc_set_defaults() 第 123 行](../../linux/kernel/irq/irqdesc.c#L123-L124)）。父层 `irq_data` 是另外分配的对象，通过 `parent_data` 连接，创建时把 `common` 指到子层这一份。保存父指针不等于父对象嵌在描述符里。状态字怎样被各层读到，见第 3.2 节。

新描述符的初始状态是“还不能接中断”：`handle_irq` 为 `handle_bad_irq`，`depth` 为 1，同时置上 `IRQD_IRQ_DISABLED` 和 `IRQD_IRQ_MASKED`（[irqdesc.c 第 128～131 行](../../linux/kernel/irq/irqdesc.c#L128-L131)）。`request_irq()` 找不到描述符会直接失败，所以分配 Linux IRQ 和注册处理函数是两步。

本配置使用稀疏 IRQ，[`irq_to_desc()`](../../linux/kernel/irq/irqdesc.c#L414-L417)在 maple tree [`sparse_irqs`](../../linux/kernel/irq/irqdesc.c#L168)里查找。热路径上的 x86 设备中断不调用它，而是读每 CPU 的 `vector_irq`。

### 3.2 三套状态不能混用

一条 IRQ 的状态分开放在三个字里，再加一个嵌套计数：

| 位置 | 代表位 | 谁修改 | 表示什么 |
| --- | --- | --- | --- |
| `irq_common_data` 的状态字，用 `irqd_*()` 访问 | `IRQD_IRQ_DISABLED`、`IRQD_IRQ_MASKED`、`IRQD_IRQ_INPROGRESS`、`IRQD_IRQ_STARTED`、`IRQD_ACTIVATED` | 通用流控和启动/关闭路径 | 软件眼里的禁用、硬件屏蔽是否已经做过、处理函数是否正在运行、是否已经 startup、中断域是否已经激活 |
| `desc->istate` | `IRQS_PENDING`、`IRQS_REPLAY`、`IRQS_ONESHOT`、`IRQS_SPURIOUS_DISABLED` | 流控、重发和伪中断检测 | 处理期间又来过请求、这次是重放、线程结束前不要 unmask、因为无人处理而被关掉 |
| `desc->status_use_accessors` | 其中的电平标志，由 `irq_settings_is_level()` 读取 | 注册触发类型和 `irq_set_status_flags()` | 重发时用来区分电平与边沿。它和 `IRQD_LEVEL` 由 [`irq_modify_status()`](../../linux/kernel/irq/chip.c#L1080-L1081)保持一致 |
| `desc->depth` | 无符号计数 | `__disable_irq()` / `__enable_irq()` / `irq_startup()` | 嵌套的软件禁用次数 |

`IRQD_*` 的定义在 [irq.h 第 223～247 行](../../linux/include/linux/irq.h#L223-L247)，`IRQS_*` 在 [internals.h 第 61～73 行](../../linux/kernel/irq/internals.h#L61-L73)。

这些 `IRQD_*` 位放在 `irq_common_data` 里。结构注释写明，这份数据由一条 IRQ 上的各层芯片共享（[irq.h 第 131～146 行](../../linux/include/linux/irq.h#L131-L146)）。父层插入时执行 `irq_data->common = child->common`（[irqdomain.c 第 1325～1327 行](../../linux/kernel/irq/irqdomain.c#L1325-L1327)）。[`__irqd_to_state()`](../../linux/include/linux/irq.h#L249)从这份 `common` 读写状态，所以 vector 域上置的位，最外层 `desc->irq_data` 读到的是同一位。各层各自持有的是 `chip`、`hwirq`、`chip_data` 和 `parent_data`。有效 affinity 也在这份 `common` 里（[irq.h 第 155～157 行](../../linux/include/linux/irq.h#L155-L157)、[第 905～907 行](../../linux/include/linux/irq.h#L905-L907)）。

这几个名字接近的位，含义不同：

- `IRQD_IRQ_DISABLED`：通用层认为这条 IRQ 已被软件禁用。流控看到它就不再调用 `action`。
- `IRQD_IRQ_MASKED`：通用层认为已经调用过 `irq_mask`（或等价路径）并且还没有 `irq_unmask`。它记录的是软件追踪到的屏蔽状态。
- `depth`：禁用请求的嵌套层数。从 0 变成 1 时才真正执行禁用；减回 0 时才重新启动。

懒禁用会让前两个位暂时不一致：`IRQD_IRQ_DISABLED` 已经置上，硬件却还没屏蔽，`IRQD_IRQ_MASKED` 仍为假。下一次中断进入流控后才会补上屏蔽。第 7.2 节按代码说明这个过程。

`IRQD_IRQ_INPROGRESS` 在调用驱动之前置上，驱动返回并重新拿到 `desc->lock` 之后清除（[handle.c 第 249～261 行](../../linux/kernel/irq/handle.c#L249-L261)）。置位和清位都在持锁时完成，所以另一个 CPU 上的流控要么看到“还没开始”，要么看到“正在处理”。

### 3.3 `irqaction` 是一次注册

[`struct irqaction`](../../linux/include/linux/interrupt.h#L122-L136)挂在 `desc->action` 链表上。共享 IRQ 有多项，[遍历宏](../../linux/kernel/irq/internals.h#L146-L147)顺着 `next` 走。热路径使用的字段是 `handler`、`dev_id` 和 `flags`。`thread_fn` 与 `thread` 只在注册了线程处理函数、或强制线程化改写了注册时才有意义。

`request_irq()` 是 [`request_threaded_irq()` 的封装](../../linux/include/linux/interrupt.h#L168-L173)，它传入空的 `thread_fn`，并带上 `IRQF_COND_ONESHOT`。在没有已存在的 oneshot 共享者时，这个条件标志不会把 IRQ 变成 oneshot。第 6.2 节给出判断代码。

处理函数的返回值是位标志（[irqreturn.h 第 11～15 行](../../linux/include/linux/irqreturn.h#L11-L15)）：

| 值 | 数值 | 硬中断路径上的含义 |
| --- | --- | --- |
| `IRQ_NONE` | 0 | 这项注册没有处理这次中断 |
| `IRQ_HANDLED` | bit 0 | 这项注册已经处理 |
| `IRQ_WAKE_THREAD` | bit 1 | 还要唤醒这项注册的 IRQ 线程 |

返回值按位或合并成一个结果。循环仍会调用链表上的每一项，一项返回 `IRQ_NONE` 只表示这项没有处理。合并结果里只要带有 `IRQ_HANDLED` 或 `IRQ_WAKE_THREAD`，它就不再是 `IRQ_NONE`。

### 3.4 硬中断上下文记在 `preempt_count` 里

[`preempt_count` 的布局](../../linux/include/linux/preempt.h#L27-L52)把硬中断放在 bit 16～19，`HARDIRQ_OFFSET` 是 `1 << 16`。`in_hardirq()` 检查这块掩码是否非零（[第 109～127 行](../../linux/include/linux/preempt.h#L109-L127)）。NMI 使用更高的 4 位，并额外再加上一份 `HARDIRQ_OFFSET`；本章的设备 IRQ 不走 `nmi_enter()`。

进入时 [`__irq_enter_raw()`](../../linux/include/linux/hardirq.h#L46-L50)把计数加上 `HARDIRQ_OFFSET`，并调用 `lockdep_hardirq_enter()`。这个宏由 `CONFIG_TRACE_IRQFLAGS` 控制：打开时增加 per-CPU 的 `hardirq_context`，否则是空操作（[irqflags.h 第 42～60 行](../../linux/include/linux/irqflags.h#L42-L60)、[第 108～117 行](../../linux/include/linux/irqflags.h#L108-L117)）。本配置没有选上它。会 `select` 它的是 `PROVE_LOCKING`（[Kconfig.debug 第 1378 行](../../linux/lib/Kconfig.debug#L1378)）、`IRQSOFF_TRACER`（[trace/Kconfig 第 395 行](../../linux/kernel/trace/Kconfig#L395)）和 sleep monitor（[sleep/Kconfig 第 8 行](../../linux/kernel/trace/rv/monitors/sleep/Kconfig#L8)）。前两项未设置（[.config 第 10659 行](../../linux/.config#L10659)、[.config 第 10751 行](../../linux/.config#L10751)），sleep monitor 依赖的 `CONFIG_RV` 也未编入（[.config 第 10789 行](../../linux/.config#L10789)）。`lockdep_assert_irqs_disabled()` 才是直接跟着 `CONFIG_PROVE_LOCKING` 变成空操作。能回答“现在是不是硬中断”的，是 `preempt_count` 里的这 4 位。

### 3.5 向量表把入口接到描述符

[`vector_irq`](../../linux/arch/x86/include/asm/hw_irq.h#L130-L131)是每 CPU 数组，元素类型是 `struct irq_desc *`。三个哨兵值（[第 126～128 行](../../linux/arch/x86/include/asm/hw_irq.h#L126-L128)）：

| 值 | 含义 |
| --- | --- |
| `VECTOR_UNUSED`（`NULL`） | 这个 CPU 上的该向量没有处理程序 |
| `VECTOR_SHUTDOWN`（`(void *)-1`） | 向量已经关闭，描述符指针已被拿掉 |
| `VECTOR_RETRIGGERED`（`(void *)-2`） | 迁移过程中的重触发标记 |

查找时要同时看 CPU 和向量。同一个向量号在不同 CPU 上可以指向不同描述符。概述章说明了映射如何在激活时写入；本章第 4.6 节只分析到达时如何读它。

## 4. 进入硬中断上下文

### 4.1 没有 FRED 时，IDT 用中断门进入公共桩

[`native_init_IRQ()`](../../linux/arch/x86/kernel/irqinit.c#L95-L105)在 `X86_FEATURE_FRED` 不可用时调用 `idt_setup_apic_and_irq_gates()`。该函数把尚未标记为系统向量、且位于 `FIRST_SYSTEM_VECTOR` 之下的项设成中断门，入口地址是 `irq_entries_start` 里对应的桩（[idt.c 第 284～294 行](../../linux/arch/x86/kernel/idt.c#L284-L294)）。[`set_intr_gate()`](../../linux/arch/x86/kernel/idt.c#L206-L212)经 [`init_idt_data()`](../../linux/arch/x86/include/asm/desc.h#L405-L415)把门类型写成 `GATE_INTERRUPT`。同文件第 32～34 行的 `INTG` 宏也使用这个类型，设备向量门不走那张静态表。从 `FIRST_SYSTEM_VECTOR` 到 255，尚未占用的项指向 `spurious_entries_start`（[idt.c 第 296～305 行](../../linux/arch/x86/kernel/idt.c#L296-L305)）。

每个设备向量桩的长度固定，内容是压入自己的向量再跳到 `asm_common_interrupt`（[idtentry.h 第 531～557 行](../../linux/arch/x86/include/asm/idtentry.h#L531-L557)）。压入使用 `.byte 0x6a, vector`，也就是 `push imm8`。注释说明这条立即数会按符号扩展，所以 C 入口只保留低 8 位：

```c
u32 vector = (u32)(u8)error_code;
```

宏在 [idtentry.h 第 206～222 行](../../linux/arch/x86/include/asm/idtentry.h#L206-L222)。这里的 `error_code` 参数名来自异常入口的复用，普通设备 IRQ 没有异常错误码，这个槽位放的是向量。

`asm_common_interrupt` 由 [`idtentry_irq`](../../linux/arch/x86/entry/entry_64.S#L376-L379)生成，按“带错误码”的 [`idtentry`](../../linux/arch/x86/entry/entry_64.S#L329-L361)展开。[`idtentry_body`](../../linux/arch/x86/entry/entry_64.S#L289-L316)调用 `error_entry()` 保存寄存器、在需要时切换到内核 CR3 和任务栈，再以 `pt_regs` 和向量调用 C 函数 `common_interrupt()`。

### 4.2 有 FRED 时，设备向量直接调用同一个 C 函数

`CONFIG_X86_FRED=y` 时，`native_init_IRQ()` 总会调用 `fred_complete_exception_setup()`；只有 CPU 不具备 FRED 时才安装上一节的 IDT 设备门。两种入口不会在同一次中断里串起来。

FRED 的用户态和内核态入口在事件类型为外部中断时进入 [`fred_extint()`](../../linux/arch/x86/entry/entry_fred.c#L159-L176)。向量大于等于 `FIRST_SYSTEM_VECTOR` 时查系统向量表；否则调用：

```c
common_interrupt(regs, vector);
```

FRED 传入的向量已经是 8 位事件号，C 包装里的 `(u8)` 截断不会再丢掉有效位。随后仍然执行 `irqentry_enter()`、中断栈切换、`irq_enter_rcu()` 和 `irqentry_exit()`。FRED 改变的是如何到达这个函数，不改变函数内部的硬中断记账。

### 4.3 `irqentry_enter()` 区分用户态、空闲任务和普通内核态

[`irqentry_enter()`](../../linux/kernel/entry/common.c#L78-L143)在加上硬中断计数之前运行：

- 被打断的是用户态：调用 `irqentry_enter_from_user_mode()`，返回值里的 `exit_rcu` 为假。
- 被打断的是空闲任务，或者当前处于 RCU 扩展静止状态：调用 `ct_irq_enter()`，让 RCU 看到这段内核执行，并把 `exit_rcu` 设为真。源码注释说明，嵌套进来的定时器中断也必须走这条路径，否则可能把嵌套 tick 误当成第一层中断并过早报告静止状态。
- 其他内核态：RCU 已经在观察。这里只做 `rcu_irq_enter_check_tick()`，处理 NO_HZ 全系统下是否需要重启 tick。

`common_interrupt()` 在进入查表之前还检查 `rcu_is_watching()`（[irq.c 第 322～323 行](../../linux/arch/x86/kernel/irq.c#L322-L323)）。这个警告用来抓住“入口没有把 RCU 叫醒”的错误。

### 4.4 何时换到硬中断栈

[`call_on_irqstack_cond()`](../../linux/arch/x86/include/asm/irq_stack.h#L132-L153)的条件是：

- 从用户态进入，或者本 CPU 的 `hardirq_stack_inuse` 已经为真：不换栈。直接调用 `irq_enter_rcu()`、处理函数和 `irq_exit_rcu()`。注释说明，用户态进入时任务栈上还没有内核调用帧。
- 其他情况：先把 `hardirq_stack_inuse` 写成真，再把当前 `rsp` 存进中断栈顶端并切换过去（[第 81～99 行](../../linux/arch/x86/include/asm/irq_stack.h#L81-L99)）。汇编调用序列内部同样是 `irq_enter_rcu`、处理函数、`irq_exit_rcu`（[第 189～204 行](../../linux/arch/x86/include/asm/irq_stack.h#L189-L204)）。调用返回后才把 `hardirq_stack_inuse` 写成假。

因此 `irq_exit_rcu()` 执行时，如果这次切换过栈，CPU 仍在中断栈上，而且处理函数的栈帧已经返回。`inuse` 仍为真。软中断循环在调用回调前会执行 `local_irq_enable()`（[softirq.c 第 606 行](../../linux/kernel/softirq.c#L606)）。这个窗口里新进来的设备 IRQ 看到 `inuse` 为真，就留在当前中断栈上，不再切换。

栈对象是每 CPU 的 `irq_stack`。`map_irq_stack()` 在 vmap 之后把 `hardirq_stack_ptr` 指到栈顶减 8 的位置，热路径不用再计算栈顶（[irq_64.c 第 49～54 行](../../linux/arch/x86/kernel/irq_64.c#L49-L54)）。`IRQ_STACK_SIZE` 是 `PAGE_SIZE << IRQ_STACK_ORDER`。`IRQ_STACK_ORDER` 等于 `2 + KASAN_STACK_ORDER`；本配置未启用 KASAN，后者为 0，阶数就是 2（[page_64_types.h 第 9～13 行](../../linux/arch/x86/include/asm/page_64_types.h#L9-L13)、[第 21～22 行](../../linux/arch/x86/include/asm/page_64_types.h#L21-L22)）。`PAGE_SHIFT` 为 12，页大小 4 KiB，栈大小是 16 KiB。

### 4.5 `irq_enter_rcu()` 加上硬中断计数

[`irq_enter_rcu()`](../../linux/kernel/softirq.c#L662-L670)调用 `__irq_enter_raw()`，然后在 NO_HZ 全系统 CPU 上，或者被打断的是空闲任务且当前中断计数刚好等于一层硬中断时，调用 `tick_irq_enter()`。最后 `account_hardirq_enter()` 给被打断的任务记硬中断时间。

这一层计数是后面 `in_hardirq()` 为真的原因。流控函数和驱动回调都跑在它里面。

### 4.6 向量查不到描述符时必须 EOI

[`call_irq_handler()`](../../linux/arch/x86/kernel/irq.c#L273-L311)先无锁读取 `this_cpu` 的 `vector_irq[vector]`。读到普通描述符指针就调用 `handle_irq()`。

读到 `NULL` 或哨兵时，它拿 `vector_lock` 再读一次。注释写出了要防的交错：一个 CPU 正在 `free_irq()` 把目标 CPU 的表项写成 `VECTOR_SHUTDOWN`，另一个 CPU 已经接受了这个向量但还没处理；与此同时新的 `request_irq()` 可能把表项写成新描述符。第二次查找若仍不是有效描述符，就把 `VECTOR_SHUTDOWN` 收成 `VECTOR_UNUSED` 并返回假。

返回假时，[`common_interrupt()`](../../linux/arch/x86/kernel/irq.c#L325-L326)调用 `apic_eoi()`。没有对应描述符就不会进入流控，也就不会有人向 Local APIC 写 EOI。这里的 EOI 用来结束这次无法投递的中断。`apic_eoi()` 本身是间接调用（[apic.h 第 412～415 行](../../linux/arch/x86/include/asm/apic.h#L412-L415)），具体写哪个寄存器由 APIC 驱动安装。

查表成功后，64 位路径不在这里 EOI。EOI 由后面的 `irq_chip` 回调完成，时机取决于流控函数。

## 5. 流控函数决定屏蔽和确认的顺序

### 5.1 调用驱动之前先问描述符能不能处理

三种主要流控都会调用 [`irq_can_handle()`](../../linux/kernel/irq/chip.c#L555-L560)。它可以分成两步。

[`irq_can_handle_pm()`](../../linux/kernel/irq/chip.c#L477-L541)在 `IRQD_IRQ_INPROGRESS` 和 `IRQD_WAKEUP_ARMED` 都没置上时直接返回真。唤醒武装属于睡眠电源管理，本章不展开；它会标记挂起并返回假。已经在处理时，只有两种情况会等待：

- 描述符处于中断轮询（`IRQS_POLL_INPROGRESS`），并且轮询不是发生在当前 CPU 上。
- 同时满足：编译了有效 affinity、最外层 `irq_data` 带 `IRQD_SINGLE_TARGET`、流控函数正好是 `handle_edge_irq`，而且有效 affinity 的第一个 CPU 就是当前 CPU。这时它忙等 `IRQD_IRQ_INPROGRESS` 清掉。注释说明，这是为了避免 affinity 刚刚迁到本 CPU、旧 CPU 还在跑边沿循环时，新 CPU 把中断记成 pending 并屏蔽，旧 CPU 又 unmask 后再次进来，形成空转。

x86 的 [`x86_vector_alloc_irqs()`](../../linux/arch/x86/kernel/apic/vector.c#L548-L588)用 [`irq_domain_get_irq_data()`](../../linux/arch/x86/kernel/apic/vector.c#L568)拿到 vector 域这一层，再调用 `irqd_set_single_target()` 和 `irqd_set_handle_enforce_irqctx()`（[第 582～588 行](../../linux/arch/x86/kernel/apic/vector.c#L582-L588)、[irq.h 第 306～323 行](../../linux/include/linux/irq.h#L306-L323)）。第 3.2 节说过，这两位写在共享状态字上，`irq_can_handle_pm()` 读 `desc->irq_data` 时看得到。有效 affinity 也由 vector 域写进同一份 `common`（[vector.c 第 140 行](../../linux/arch/x86/kernel/apic/vector.c#L140)）。

因此层次化的边沿 IRQ 在“正在处理”时，只要流控仍是 `handle_edge_irq`，并且有效 affinity 的第一个 CPU 就是当前 CPU，就会忙等。这段忙等包在 `CONFIG_SMP` 里，本配置会编进来（[chip.c 第 463～472 行](../../linux/kernel/irq/chip.c#L463-L472)）。它在锁外自旋，回到锁内发现位已清除后，还要软件没有禁用且 `action` 仍在，才返回真。电平 I/O APIC 的流控是 `handle_fasteoi_irq`，函数指针条件不成立，`irq_can_handle_pm()` 直接返回假；这一支是否置 pending、是否立刻 EOI，按第 5.4 节的芯片标志处理。边沿 IRQ 的有效 affinity 第一个 CPU 不是当前 CPU 时，`irq_can_handle()` 也返回假，`handle_edge_irq()` 记下 `IRQS_PENDING` 并 `mask_ack_irq()`。

[`irq_can_handle_actions()`](../../linux/kernel/irq/chip.c#L544-L552)会清掉 `IRQS_REPLAY` 和 `IRQS_WAITING`。没有 `action` 或已经 `IRQD_IRQ_DISABLED` 时，它置上 `IRQS_PENDING` 并返回假。置 pending 是为了启用或重新注册之后还能重放边沿；电平的重放规则不同，见第 7.4 节。

### 5.2 电平：先屏蔽，处理完再按条件打开

[`handle_level_irq()`](../../linux/kernel/irq/chip.c#L685-L697)在持有 `desc->lock` 的前提下：

```c
mask_ack_irq(desc);
if (!irq_can_handle(desc))
    return;
kstat_incr_irqs_this_cpu(desc);
handle_irq_event(desc);
cond_unmask_irq(desc);
```

`guard(raw_spinlock)` 在函数返回时放锁，包括上面的提前返回。`handle_irq_event()` 中间会临时放开这把锁，返回前再拿回来；函数末尾的 guard 再放一次。锁的持有次数是平衡的：驱动运行期间不持有 `desc->lock`。

[`mask_ack_irq()`](../../linux/kernel/irq/chip.c#L416-L425)优先调用 `irq_mask_ack`。没有这个回调时，先 `mask_irq()`，再在存在 `irq_ack` 时调用它。电平信号在处理期间一直有效，所以要先挡住这条线，等设备状态把电平撤掉之后再打开。

若 `irq_can_handle()` 失败，函数直接返回，**不会**执行 `cond_unmask_irq()`。线保持屏蔽。失败原因如果是禁用或没有 action，`irq_can_handle_actions()` 已经记下 `IRQS_PENDING`。

[`cond_unmask_irq()`](../../linux/kernel/irq/chip.c#L662-L673)只在三个条件同时成立时 unmask：软件没有禁用、当前记录为已屏蔽、`threads_oneshot` 为 0。普通非线程化注册不会置 `threads_oneshot`，所以处理完就会 unmask。oneshot 并且线程被唤醒时，这个掩码非零，线保持屏蔽，直到线程侧清掉对应位。

### 5.3 边沿：ACK 之后用 pending 记住处理期间的下一次请求

边沿可能在处理函数还没返回时再次发生。控制器通常只锁存“发生过”，不会把次数排队。[`handle_edge_irq()`](../../linux/kernel/irq/chip.c#L823-L858)用软件位补上这个信息。简化后的控制流是：

```text
持有 desc->lock
若 irq_can_handle() 为假：
    istate |= IRQS_PENDING
    mask_ack_irq()
    返回
增加本 CPU 中断计数
调用 chip->irq_ack()          // 这里不做空指针判断
do:
    若 action 已变为空：mask_irq() 并返回
    若 IRQS_PENDING 且未禁用且记录为已屏蔽：unmask_irq()
    handle_irq_event()         // 清 IRQS_PENDING，置 INPROGRESS，放锁，调用 action，拿锁，清 INPROGRESS
while IRQS_PENDING 且未禁用
```

`chip->irq_ack` 被直接调用，使用这个流控的芯片必须提供该回调。x86 的 MSI 与边沿 I/O APIC 都把它设成 `irq_chip_ack_parent`。这个函数只调用直接父层的 `irq_ack`（[chip.c 第 1333～1337 行](../../linux/kernel/irq/chip.c#L1333-L1337)），发生在驱动回调之前。父层是 vector 域时，回调是 `apic_ack_edge()`，它再调用 `apic_ack_irq()`，由后者执行 `apic_eoi()`（[vector.c 第 1014～1024 行](../../linux/arch/x86/kernel/apic/vector.c#L1014-L1024)）。父层是中断重映射域时，Intel 的 `intel_ir_chip` 和 AMD 的 `amd_ir_chip` 把 `irq_ack` 设成 `apic_ack_irq`（[irq_remapping.c 第 1279～1281 行](../../linux/drivers/iommu/intel/irq_remapping.c#L1279-L1281)、[iommu.c 第 4063～4065 行](../../linux/drivers/iommu/amd/iommu.c#L4063-L4065)），同样在回调前 EOI，调用链里没有 `apic_ack_edge()`。本配置没有 `CONFIG_X86_POSTED_MSI`。`enable_posted_msi` 只在该选项打开时才能被启动参数置真，否则保持假，[`posted_msi_supported()`](../../linux/arch/x86/include/asm/irq_remapping.h#L70-L73)也就为假（[irq_remapping.c 第 75～76 行](../../linux/drivers/iommu/irq_remapping.c#L75-L76)）。Intel 重映射域分配 MSI / MSI-X 时因此装上 `intel_ir_chip`，不会选到省略这次 EOI 的 posted 芯片（[irq_remapping.c 第 1463～1468 行](../../linux/drivers/iommu/intel/irq_remapping.c#L1463-L1468)）。

处理期间又一次进入时，`IRQD_IRQ_INPROGRESS` 已经置上。第 5.1 节的忙等条件不成立时，`irq_can_handle()` 失败，新的进入者只做两件事：置 `IRQS_PENDING`，并 `mask_ack_irq()`。正在跑的循环从 `handle_irq_event()` 回来后看到 pending，若软件没禁用就先 unmask，再处理一轮。屏蔽的作用是让处理过程中连续到来的边沿合成“至少又来了一次”，而不是在驱动还没返回时反复重入。忙等条件成立时，新的进入者在锁外等到该位清掉；描述符仍可处理就继续这次 ACK 和回调，不走 pending 分支。

`handle_irq_event()` 一进门就清 `IRQS_PENDING`（[handle.c 第 253 行](../../linux/kernel/irq/handle.c#L253)），然后才放锁。清位之后、驱动返回之前若又来一次，并且这次没有忙等成功，新的进入者会重新置位。外层 `do-while` 因此还能再跑一轮。

同一 CPU 上要在回调返回前再次进入，处理函数必须先打开本地中断。正常路径不这样做。若回调打开了中断，而当前 CPU 又正好是有效 affinity 的第一个 CPU，新的进入会在锁外等待 `IRQD_IRQ_INPROGRESS`。这一位要等外层 `handle_irq_event()` 重新拿锁后才清除，外层却停在被这次进入打断的回调里，所以这次等待结束不了。核心在回调返回后若发现中断被打开，会警告并重新关闭。另一个 CPU 收到同一条边沿 IRQ 时：它是有效 affinity 的第一个 CPU 就忙等，否则走 pending 并屏蔽。

### 5.4 fasteoi：默认在回调之后 EOI，没有 action 时先屏蔽

[`handle_fasteoi_irq()`](../../linux/kernel/irq/chip.c#L736-L773)把流控交给会在软件 EOI 时自己收尾的控制器。持锁后的主要分支：

| 条件 | 行为 |
| --- | --- |
| `irq_can_handle_pm()` 为假 | 若最外层带 `IRQD_RESEND_WHEN_IN_PROGRESS`，置 `IRQS_PENDING`。然后按芯片标志决定是否立刻 EOI，返回 |
| 没有 action 或已禁用 | `mask_irq()`，再按同样规则 EOI，返回 |
| 可以处理 | 计数；若 `IRQS_ONESHOT` 则先屏蔽；`handle_irq_event()`；`cond_unmask_eoi_irq()`；若此时仍有 `IRQS_PENDING`，调用 `check_irq_resend()` |

非 oneshot 的 [`cond_unmask_eoi_irq()`](../../linux/kernel/irq/chip.c#L700-L718)直接调用 `chip->irq_eoi`。oneshot 则在“没有禁用、仍屏蔽、没有线程还占着线”时先 EOI 再 unmask；否则若芯片没有 `IRQCHIP_EOI_THREADED`，仍然 EOI，把 unmask 留给线程结束之后。

提前返回上的 [`cond_eoi_irq()`](../../linux/kernel/irq/chip.c#L721-L724)在芯片**没有** `IRQCHIP_EOI_IF_HANDLED` 时也 EOI。也就是说，默认即使这次没有调用驱动，也要通知控制器结束这一次。没有 action 的那条分支还会先屏蔽，避免电平线在无人处理时 EOI 之后立刻再次进来。

`IRQD_RESEND_WHEN_IN_PROGRESS` 由部分非 x86 控制器设置。本章的 x86 MSI 与 I/O APIC 路径不设置它。x86 边沿用的是 `handle_edge_irq()` 自己的 pending 循环，不依赖这个标志。

### 5.5 x86 怎样选择流控，以及 EOI 落在回调的哪一侧

I/O APIC 按触发电平选择流控。[`mp_register_handler()`](../../linux/arch/x86/kernel/apic/io_apic.c#L845-L859)对电平设置 `IRQ_LEVEL` 并安装 `handle_fasteoi_irq`，对边沿安装 `handle_edge_irq`。[`init_ISA_irqs()`](../../linux/arch/x86/kernel/irqinit.c#L69-L71)给每个编号小于 `nr_legacy_irqs()` 的 IRQ 装上 `handle_level_irq`。8259A 存在时这个上限是 `NR_IRQS_LEGACY`，定义为 16（[irq_vectors.h 第 128 行](../../linux/arch/x86/include/asm/irq_vectors.h#L128)、[i8259.c 第 432～433 行](../../linux/arch/x86/kernel/i8259.c#L432-L433)）。`null_legacy_pic` 报告 0，循环不会执行（[i8259.c 第 419～420 行](../../linux/arch/x86/kernel/i8259.c#L419-L420)）。I/O APIC 随后接管时，`mp_register_handler()` 会按触发电平改掉流控函数。

MSI / MSI-X 域在 x86 上固定使用边沿流控。[`x86_init_dev_msi_info()`](../../linux/arch/x86/kernel/apic/msi.c#L248-L254)把 `irq_ack` 设为 `irq_chip_ack_parent`，把 `handler` 设为 `handle_edge_irq`。

Local APIC 的 EOI 写在 [`apic_ack_irq()`](../../linux/arch/x86/kernel/apic/vector.c#L1014-L1018)里。vector 域芯片 [`lapic_controller`](../../linux/arch/x86/kernel/apic/vector.c#L1032-L1039)的 `irq_ack` 是 [`apic_ack_edge()`](../../linux/arch/x86/kernel/apic/vector.c#L1020-L1024)，它先做 `irq_complete_move()`，再调用 `apic_ack_irq()`。[`irq_chip_ack_parent()`](../../linux/kernel/irq/chip.c#L1333-L1337)只走一层 `parent_data`。直接父域是 vector 域时才会进入 `apic_ack_edge()`；直接父域是重映射域时，进入的是该域芯片自己的 `irq_ack`。

电平 I/O APIC 虽然也把 `irq_ack` 设成 `irq_chip_ack_parent`（[io_apic.c 第 1859 行](../../linux/arch/x86/kernel/apic/io_apic.c#L1859)、[第 1873 行](../../linux/arch/x86/kernel/apic/io_apic.c#L1873)），`handle_fasteoi_irq()` 不调用 `irq_ack`。它的 EOI 走 `irq_eoi`，在驱动回调之后。

因此这几条路径的 EOI 时机是：

| 路径 | 流控 | EOI 发生在 |
| --- | --- | --- |
| MSI，以及边沿 I/O APIC，直接父域是 vector 域 | `handle_edge_irq` | 驱动回调之前。`irq_ack` 进入 `apic_ack_edge()`，再由 `apic_ack_irq()` 写 Local APIC EOI |
| MSI，以及边沿 I/O APIC，直接父域是重映射域 | `handle_edge_irq` | 驱动回调之前。直接父层的 `irq_ack` 就是 `apic_ack_irq()` |
| 电平 I/O APIC，父域就是 vector 域 | `handle_fasteoi_irq` | 驱动回调之后。`irq_eoi` 是 [`ioapic_ack_level()`](../../linux/arch/x86/kernel/apic/io_apic.c#L1657-L1720)：先读 Local APIC 的 TMR，再 `apic_eoi()`；若 TMR 显示这个向量不像电平触发，再把 Local APIC 向量写进 I/O APIC 的 EOI 寄存器 |
| 电平 I/O APIC，父域不是 vector 域 | 同一个 fasteoi | [`ioapic_ir_ack_level()`](../../linux/arch/x86/kernel/apic/io_apic.c#L1723-L1734)：先 `apic_ack_irq()`，再把 `entry.vector` 交给 `eoi_ioapic_pin()`。函数自己的注释把这个数说成 pin。写入 RTE 时，Intel DMAR 填的是 pin，AMD IR 填的是 8 位 IRTE index（[io_apic.c 第 1761～1766 行](../../linux/arch/x86/kernel/apic/io_apic.c#L1761-L1766)）。本配置两种 IOMMU 都编了进来（[.config 第 8712 行](../../linux/.config#L8712)、[.config 第 8714 行](../../linux/.config#L8714)），运行时父域决定填哪一个 |

父域的选择在分配 IRQ 时完成：父域等于 `x86_vector_domain` 就用 `ioapic_chip`，否则用 `ioapic_ir_chip`（[io_apic.c 第 2889～2890 行](../../linux/arch/x86/kernel/apic/io_apic.c#L2889-L2890)）。`CONFIG_IRQ_REMAP=y` 只说明后一种芯片已被编译，不说明当前每条 IRQ 都经过重映射。

向 Local APIC 写 EOI，表示这颗 CPU 的中断控制器可以结束对应向量的服务阶段。设备自己的状态寄存器、屏蔽位和后续工作仍由驱动回调处理。

### 5.6 每 CPU 流控不使用这套锁和 pending

[`handle_percpu_irq()`](../../linux/kernel/irq/chip.c#L867-L883)不拿 `desc->lock`，不设置 `IRQD_IRQ_INPROGRESS`，也不修改 `tot_count`。它在当前 CPU 上 ACK、调用 `handle_irq_event_percpu()`、再 EOI。这类 IRQ 的注册带 `IRQF_PERCPU`，每个 CPU 各自处理自己的那一次，不与其他 CPU 争用同一把描述符锁。

[`handle_simple_irq()`](../../linux/kernel/irq/chip.c#L610-L625)则假定调用者已经处理完 ACK 和屏蔽。通过检查之后，它增加本 CPU 计数并调用 `handle_irq_event()`。复用中断的下一层分发会用到它。设备 IRQ 的热路径一般不从这里开始。

## 6. 驱动回调怎样被逐个调用

### 6.1 放锁、调用、再把返回值并起来

[`handle_irq_event()`](../../linux/kernel/irq/handle.c#L249-L261)在持锁时清 `IRQS_PENDING`、置 `IRQD_IRQ_INPROGRESS`，然后放锁。`handle_irq_event_percpu()` 调用 [`__handle_irq_event_percpu()`](../../linux/kernel/irq/handle.c#L177-L233)，回来之后再贡献一次中断随机性，并在描述符允许调试时调用 `note_interrupt()`。

回调循环对每个 `irqaction` 执行：

```c
res = action->handler(irq, action->dev_id);
```

`irq` 来自 `desc->irq_data.irq`，是 Linux IRQ 号，不是 x86 向量。`dev_id` 是注册时传入的指针。返回后若本地中断已被打开，核心打印一次警告并执行 `local_irq_disable()`。

`switch` 只对 `IRQ_WAKE_THREAD` 做额外动作：必须已有 `thread_fn`，否则只警告。有线程时 [`__irq_wake_thread()`](../../linux/kernel/irq/handle.c#L61-L137)在 `thread_flags` 上置 `IRQTF_RUNTHREAD`，把 `thread_mask` 或进 `threads_oneshot`，增加 `threads_active`，再 `wake_up_process()`。线程函数不在这条硬中断栈上运行。其他返回值在 `switch` 里没有分支。

无论返回哪一种，循环在 `switch` 之后都执行 `retval |= res`（[handle.c 第 230 行](../../linux/kernel/irq/handle.c#L230)）。每一项都会被调用，是因为 [`for_each_action_of_desc()`](../../linux/kernel/irq/internals.h#L146-L147)顺着链表走。某个设备返回 `IRQ_NONE`，只说明**它**没有处理。只要有一项返回 `IRQ_HANDLED` 或 `IRQ_WAKE_THREAD`，合并结果就不再是 `IRQ_NONE`。

`kstat_incr_irqs_this_cpu()` 在进入 `handle_irq_event()` 之前由流控调用。它增加本 CPU 的 `kstat_irqs->cnt`、全局 `kstat.irqs_sum`，以及 `desc->tot_count`（[internals.h 第 252～261 行](../../linux/kernel/irq/internals.h#L252-L261)）。`/proc/interrupts` 里的每 CPU 计数来自前者。每 CPU 流控使用不碰 `tot_count` 的 `__kstat_incr_irqs_this_cpu()`。

### 6.2 oneshot 只影响屏蔽何时恢复

注册时若带 `IRQF_ONESHOT`，`__setup_irq()` 给该 action 分配一个 `thread_mask` 位（[manage.c 第 1619～1648 行](../../linux/kernel/irq/manage.c#L1619-L1648)），并给描述符置 `IRQS_ONESHOT`（[第 1712～1713 行](../../linux/kernel/irq/manage.c#L1712-L1713)）。硬中断侧唤醒线程时把这一位置进 `threads_oneshot`。电平和 fasteoi 的 unmask 条件都要求这个字段为 0，所以线程还没结束时线保持屏蔽。

`request_irq()` 附加的 `IRQF_COND_ONESHOT` 自己不会置 `IRQS_ONESHOT`。只有共享时发现旧 action 已经是 oneshot，才把新 action 也标成 oneshot（[第 1589～1591 行](../../linux/kernel/irq/manage.c#L1589-L1591)）。

`handler == NULL` 且提供了 `thread_fn` 时，核心安装 [`irq_default_primary_handler()`](../../linux/kernel/irq/manage.c#L986-L989)，它只返回 `IRQ_WAKE_THREAD`。这种注册若没有 oneshot，并且芯片也没有 `IRQCHIP_ONESHOT_SAFE`，`__setup_irq()` 直接返回 `-EINVAL`（[第 1650～1669 行](../../linux/kernel/irq/manage.c#L1650-L1669)）。注释说明原因：默认主处理函数不清除设备状态，电平线若马上 unmask 会立刻再次进入。

强制线程化默认关闭。`threadirqs` 打开后，[`irq_setup_forced_threading()`](../../linux/kernel/irq/manage.c#L1307-L1343)把原来的主处理函数改成线程函数，主处理函数换成默认的“只唤醒线程”，并加上 `IRQF_ONESHOT`。`IRQF_NO_THREAD`、`IRQF_PERCPU` 和已经是 oneshot 的注册不改。本章默认路径不经过这个改写，驱动传入的 `handler` 仍在硬中断里运行。

## 7. 注册、禁用和重发怎样改状态

### 7.1 `__setup_irq()` 把 action 接上并决定要不要启动

[`request_threaded_irq()`](../../linux/kernel/irq/manage.c#L2076-L2164)先拒绝 `IRQ_NOTCONNECTED`、共享却没有 `dev_id`、以及共享同时要求 `IRQF_NO_AUTOEN` 这几类参数，再 `irq_to_desc()`。描述符不存在、不允许请求、或是 per-CPU devid IRQ 时返回错误。然后分配 `irqaction`，保存函数指针、标志、名字和 `dev_id`，交给 `__setup_irq()`。

[`__setup_irq()`](../../linux/kernel/irq/manage.c#L1445-L1775)的锁顺序写在函数注释里：`request_mutex` 对上并发的 `free_irq()`，可选的芯片总线锁对上慢速总线，`desc->lock` 对上硬中断。热路径只拿最后一把。

和硬中断状态直接相关的步骤是：

1. 调用者没指定触发类型时，沿用 `irq_data` 里已有的类型。
2. 若允许线程化，先做第 6.2 节的强制线程化改写；需要线程时创建 `irq/<号>-<名字>` 内核线程。
3. 芯片带 `IRQCHIP_ONESHOT_SAFE` 时去掉 `IRQF_ONESHOT`，避免线程结束时多做一次 unmask。
4. 已有 action 时检查共享条件：双方都有 `IRQF_SHARED`，触发类型一致，oneshot 一致，per-CPU 属性一致。NMI 不允许共享。
5. 第一项 action 负责 `irq_request_resources()`、按标志设置触发类型，并调用 `irq_activate()`。
6. 没有 `IRQF_NO_AUTOEN` 且允许自动启用时调用 `irq_startup()`。否则把 `depth` 留在 1，等以后的 `enable_irq()`。共享 IRQ 若要求不自动启用，会触发 `WARN_ON_ONCE`。
7. 把新 action 接到链表尾，清零伪中断计数。若这条线先前因为伪中断被关掉，并且这次是共享注册，则清 `IRQS_SPURIOUS_DISABLED` 并 `__enable_irq()`。

`irq_activate()` 调用 [`irq_domain_activate_irq()`](../../linux/kernel/irq/irqdomain.c#L1967-L1975)，沿域层次执行 `activate`，成功后置 `IRQD_ACTIVATED`。x86 vector 域的 [`x86_vector_activate()`](../../linux/arch/x86/kernel/apic/vector.c#L461-L481)在这里分配或启用已经预留的 CPU 向量。激活的是投递资源。把 `depth` 清零并调用芯片启动回调的是下一步 `irq_startup()`。

[`irq_startup()`](../../linux/kernel/irq/chip.c#L269-L300)先把 `depth` 写成 0。已经处于 started 状态时只 `irq_enable()`。否则在调用芯片的 `irq_startup` 或 `irq_enable` 前后设置 affinity，并置 `IRQD_IRQ_STARTED`。`resend` 参数为真时接着 `check_irq_resend()`。`__setup_irq()` 传入的就是要重发。

I/O APIC 的启动回调是 [`startup_ioapic_irq()`](../../linux/arch/x86/kernel/apic/io_apic.c#L1564-L1575)。它解开该 pin 的屏蔽；IRQ 号仍落在 legacy PIC 范围内时，还会屏蔽 8259A 上的对应线，并查询 PIC 是否已有挂起请求。这个返回值会从 `irq_startup()` 传出，但 `request` / `enable` 的调用点不检查它。边沿是否重放，看的是 `IRQS_PENDING`，不是这个返回值。

注册注释要求驱动假定处理函数可能在 `request_irq()` 返回前就运行（[manage.c 第 2049～2053 行](../../linux/kernel/irq/manage.c#L2049-L2053)）。回调使用的数据要在注册前准备好。

### 7.2 `depth` 嵌套，硬件屏蔽可以推迟到下一次中断

[`__disable_irq()`](../../linux/kernel/irq/manage.c#L664-L667)先做 `depth++`，只有从 0 变成 1 时才调用 `irq_disable()`。再禁用一次只增加计数。

[`irq_disable()`](../../linux/kernel/irq/chip.c#L373-L396)的注释把没有 `irq_disable` 回调的情况定义成懒禁用：

- 已经处于 `IRQD_IRQ_DISABLED`：若调用者要求屏蔽，再执行一次 `mask_irq()`。
- 尚未禁用：置 `IRQD_IRQ_DISABLED`。芯片有 `irq_disable` 时调用它，并置 `IRQD_IRQ_MASKED`。否则只有设置了 `IRQ_DISABLE_UNLAZY` 才 `mask_irq()`。没有这个标志时，硬件保持未屏蔽。

懒禁用避开了“禁用之后往往不再来中断，却仍然写一次屏蔽寄存器”的开销。下一次中断仍会进来，流控在 `irq_can_handle_actions()` 里看到禁用，置 `IRQS_PENDING` 并返回假。电平流控在这之前已经 `mask_ack_irq()`，并且不会 unmask。边沿流控在失败分支里 `mask_ack_irq()`。fasteoi 在“没有 action 或已禁用”分支里 `mask_irq()` 然后 EOI。硬件屏蔽被补上，软件留下 pending，供以后启用时决定要不要重放。

[`__enable_irq()`](../../linux/kernel/irq/manage.c#L758-L787)按 `depth` 分支。大于 1 时只减一。等于 1 时调用 `irq_startup(..., IRQ_RESEND, ...)`，由它把 `depth` 写成 0 并启用。等于 0 时说明启用次数多于禁用次数，打印不平衡警告。

对外的三个接口差别是等不等待：

| 接口 | 行为 | 能否睡眠 |
| --- | --- | --- |
| [`disable_irq_nosync()`](../../linux/kernel/irq/manage.c#L690-L693) | 只增加 `depth` 并按上面的规则禁用 | 不睡眠，可在硬中断里调用 |
| [`disable_irq()`](../../linux/kernel/irq/manage.c#L710-L715) | 先做 nosync，再 `synchronize_irq()` | `might_sleep()`，不能在硬中断里调用 |
| [`enable_irq()`](../../linux/kernel/irq/manage.c#L800-L809) | 嵌套减一，减到启用时 startup | 芯片带慢速总线锁时不能在硬中断里调用 |

`disable_irq()` 若在持有处理函数需要的锁时调用，会和正在运行的处理函数互相等待。这是接口注释里写明的死锁条件。

### 7.3 同步等到调用当时已经在途的中断

[`__synchronize_hardirq()`](../../linux/kernel/irq/manage.c#L40-L71)先在锁外观察 `IRQD_IRQ_INPROGRESS`，看到为假后再拿 `desc->lock` 确认一次。注释说明第一次观察没有额外的内存屏障，所以必须以持锁后的结果为准。

`synchronize_hardirq()` 传入 `sync_chip = false`，硬中断侧的等待到此为止（[manage.c 第 95～101 行](../../linux/kernel/irq/manage.c#L95-L101)）。它的注释写明，这样调用不查询硬件里尚未被服务的在途中断。`synchronize_irq()` 走 [`__synchronize_irq()`](../../linux/kernel/irq/manage.c#L108-L116)，把 `sync_chip` 设为真。沿层次找到 `irq_get_irqchip_state` 时，会查询 `IRQCHIP_STATE_ACTIVE`：中断已经送到某颗 CPU，还在等待服务和确认（[manage.c 第 57～68 行](../../linux/kernel/irq/manage.c#L57-L68)、[第 2641～2660 行](../../linux/kernel/irq/manage.c#L2641-L2660)）。I/O APIC 对电平线把 Remote IRR 为 1 当成 active；边沿的 Remote IRR 无定义，查询结果保持假（[io_apic.c 第 1825～1849 行](../../linux/arch/x86/kernel/apic/io_apic.c#L1825-L1849)）。芯片没有这个回调时，返回码被忽略，`inprogress` 保持刚才读到的假。查完硬中断侧之后，`wait_event` 直到 `threads_active` 为 0（[第 108～138 行](../../linux/kernel/irq/manage.c#L108-L138)）。等待线程可以睡眠，所以 `synchronize_irq()` 要求可睡眠上下文。

`IRQD_IRQ_INPROGRESS` 只在驱动回调运行期间为真。芯片报告的 active 覆盖的是这次调用时已经送出、服务还没结束的那一次。同步返回之后设备才新产生的中断，不在这次等待里。

在本 IRQ 的硬中断处理函数里调用 `synchronize_irq()` 或 `disable_irq()` 会等自己的 `IRQD_IRQ_INPROGRESS`。该位要等处理函数返回后才清除，所以这样调用不会完成。

[`__free_irq()`](../../linux/kernel/irq/manage.c#L1818-L1947)按 `dev_id` 摘掉对应 action。摘掉最后一项时 `irq_shutdown()`：增加 `depth`，调用芯片的 `irq_shutdown` 或带屏蔽的禁用，并清 `IRQD_IRQ_STARTED`（[chip.c 第 322～341 行](../../linux/kernel/irq/chip.c#L322-L341)）。然后放锁，再 `__synchronize_irq()`。最后一项还要在同步完成之后 `irq_domain_deactivate_irq()`，释放域在激活时准备的向量等资源。`free_irq()` 的注释要求它不能从中断上下文调用；函数开头对 `in_interrupt()` 发出警告。

共享 IRQ 上，其他设备的 action 还在。调用者必须先让自己的设备不再产生中断，再 `free_irq()`。摘掉 action 也不会取消这个回调已经排进软中断、工作队列或定时器的后续工作。

### 7.4 边沿用重发补回被屏蔽吃掉的请求，电平交给硬件

[`check_irq_resend()`](../../linux/kernel/irq/resend.c#L122-L150)在持有 `desc->lock` 且本地中断关闭时调用。电平 IRQ 直接清掉 `IRQS_PENDING` 并返回。注释说明：电平若仍然有效，unmask 之后硬件会再次送出请求，软件不需要另做一次。边沿的硬件锁存可能已经在 `mask_ack` 时被确认掉，所以要靠软件补一次。

边沿路径上，没有 `IRQS_PENDING` 且不是强制注入时什么也不做。有 pending 时先清掉它，再 [`try_retrigger()`](../../linux/kernel/irq/resend.c#L105-L115)。芯片提供 `irq_retrigger` 就调用它；否则沿父层寻找。成功后置 `IRQS_REPLAY`。下一次真正进入 `irq_can_handle_actions()` 时会清掉 replay，避免同一次请求被反复重发。

x86 的 vector 芯片把 [`apic_retrigger_irq()`](../../linux/arch/x86/kernel/apic/vector.c#L1002-L1011)配成 `irq_retrigger`：向该 IRQ 当前记录的目标 CPU 发送这个向量的 IPI，并返回 1。MSI 和 I/O APIC 的 retrigger 都是 `irq_chip_retrigger_hierarchy()`，从父层找到这个实现。返回非 0 表示成功，`check_irq_resend()` 就不再走软件重发。

软件重发是后备。[`irq_sw_resend()`](../../linux/kernel/irq/resend.c#L49-L82)看到最外层 `irq_data` 带 `IRQD_HANDLE_ENFORCE_IRQCTX` 就返回 `-EINVAL`（[第 55～56 行](../../linux/kernel/irq/resend.c#L55-L56)）。第 3.2 节说过，vector 域置上的这一位在共享状态字里，最外层看得到。因此经过 vector 域的 MSI 和 I/O APIC，即使 `try_retrigger()` 返回 0，也不会改走 tasklet。`IRQS_PENDING` 在 retrigger 之前已经清掉；两边都失败时不会置 `IRQS_REPLAY`。没有这个标志时，函数把描述符挂到 `irq_resend_list` 并调度 `resend_tasklet`。[`resend_irqs()`](../../linux/kernel/irq/resend.c#L31-L44)在 tasklet 里、本地中断关闭的状态下直接调用 `desc->handle_irq(desc)`。这条路径没有经过 `irq_enter_rcu()`，调用时的上下文是软中断里的 tasklet。

同一位还被 [`handle_irq_desc()`](../../linux/kernel/irq/irqdesc.c#L658-L670)检查：不在硬中断里就返回 `-EPERM`。`generic_handle_irq()` 走这道检查，所以在进程上下文里对这种 IRQ 调用它会失败。普通设备入口调用的是 `generic_handle_irq_desc()`，不经过这道检查。热路径的 EOI 顺序由流控函数决定。

### 7.5 连续无人处理会把 IRQ 关掉

[`note_interrupt()`](../../linux/kernel/irq/spurious.c#L222-L379)在每次 `handle_irq_event_percpu()` 之后调用，除非描述符设置了不调试。返回值含有非法位时只报告，不累计。合并结果为 `IRQ_NONE` 时增加 `irqs_unhandled`；若距上次未处理已经超过 `HZ/10`，计数改从 1 重新开始，避免很稀的单次伪中断把工作正常的共享线累进死。

`irq_count` 到达 100000 时清零并检查。`irqs_unhandled` 大于 99900 就认为中断卡住：打印处理函数，置 `IRQS_SPURIOUS_DISABLED`，`depth++`，再 `irq_disable()`。随后启动周期为 `HZ/10` 的轮询定时器（[spurious.c 第 19 行](../../linux/kernel/irq/spurious.c#L19)）。启动参数 `noirqdebug` 会把整段检测关掉。

`IRQ_WAKE_THREAD` 且主处理函数没有返回 `IRQ_HANDLED` 时，检测会推迟到下一次硬件中断，用 `threads_handled` 是否变化来判断线程是否处理过。共享线上只要有一项主处理函数返回 `IRQ_HANDLED`，就不会因为其他项返回 `IRQ_NONE` 而累计未处理。

本配置没有 `CONFIG_DEBUG_SHIRQ`，`free_irq()` 里那段“释放后再假调一次共享处理函数”的代码不会编译进来。

## 8. 离开硬中断

### 8.1 先去掉硬中断计数，再决定是否执行软中断

[`irq_exit_rcu()`](../../linux/kernel/softirq.c#L737-L741)调用 [`__irq_exit_rcu()`](../../linux/kernel/softirq.c#L713-L729)。x86 没有定义 `__ARCH_IRQ_EXIT_IRQS_DISABLED`，所以函数先执行 `local_irq_disable()`，不依赖入口门已经把中断关掉。随后 `account_hardirq_exit()`，再减去 `HARDIRQ_OFFSET`。

减去之后若 `in_interrupt()` 为假且本 CPU 有软中断 pending，就 `invoke_softirq()`（[softirq.c 第 722～723 行](../../linux/kernel/softirq.c#L722-L723)）。本配置没有 `CONFIG_PREEMPT_RT`，`in_interrupt()` 就是 `irq_count()`，掩码含有 `NMI_MASK`、`HARDIRQ_MASK` 和整个 `SOFTIRQ_MASK`（[preempt.h 第 113～115 行](../../linux/include/linux/preempt.h#L113-L115)、[第 143 行](../../linux/include/linux/preempt.h#L143)）。`local_bh_disable()` 传入 `SOFTIRQ_DISABLE_OFFSET`，它等于两份 `SOFTIRQ_OFFSET`（[preempt.h 第 55 行](../../linux/include/linux/preempt.h#L55)）。本配置没有 `CONFIG_TRACE_IRQFLAGS`，这个调用走头文件里的内联，把该偏移加进 `preempt_count`，因此也落在 `SOFTIRQ_MASK` 里（[bottom_half.h 第 8～20 行](../../linux/include/linux/bottom_half.h#L8-L20)）。内层硬中断退出时外层硬中断计数还在，这里不会跑软中断。被打断的上下文关着 bottom half 时，最外层硬中断计数减掉之后 `in_interrupt()` 仍为真，同样不会跑。硬中断、软中断、NMI 和 bottom half 禁用都已经清掉，并且本 CPU 有 pending，才会调用 `invoke_softirq()`。

本配置有 `CONFIG_HAVE_IRQ_EXIT_ON_IRQ_STACK`，且默认没有打开 `force_irqthreads()`。[`invoke_softirq()`](../../linux/kernel/softirq.c#L487-L507)因此直接调用 `__do_softirq()`，就在当前栈上执行。第 4.4 节说过，此时要么仍在即将清空的硬中断栈上，要么在用户态进入时使用的任务栈上。从任务上下文打开 bottom half 时走的是另一条 `do_softirq_own_stack()`，那一次会主动换到中断栈。

`threadirqs` 打开并且本 CPU 的 `ksoftirqd` 已存在时，这个函数改为唤醒 `ksoftirqd`，不在退出路径里执行软中断回调。默认不是这样。

软中断回调的分类、时间限制和 `ksoftirqd` 不在本章展开。这里只需要看到：它们可能紧接在硬中断计数归零之后、返回被打断上下文之前运行；运行时 `in_hardirq()` 已经为假。返回指令见下一节。

### 8.2 返回用户态可以调度，返回内核时默认不抢占

[`irqentry_exit()`](../../linux/kernel/entry/common.c#L185-L223)按被保存的上下文分三支：

- `user_mode(regs)`：进入返回用户态的准备，其中会在需要时 `schedule()`。这与抢占模型无关，只要线程标志里有需要重新调度的位。
- 返回内核，且被打断时 EFLAGS.IF 是打开的：`regs_irqs_disabled()` 读的是 `pt_regs->flags` 里的 IF（[ptrace.h 第 312～315 行](../../linux/arch/x86/include/asm/ptrace.h#L312-L315)）。`state.exit_rcu` 为真时，进入时是空闲任务或 RCU 扩展静止状态，见第 4.3 节。这里做完 `ct_irq_exit()` 就返回，不调用条件抢占（[common.c 第 198～206 行](../../linux/kernel/entry/common.c#L198-L206)）。标志为假时，`CONFIG_PREEMPTION` 才会调用 `irqentry_exit_cond_resched()`。voluntary 模型把这个调用改成空操作，所以默认的普通内核态返回也不会抢占被打断的代码。`preempt=full` 或 `preempt=lazy` 才会在 `need_resched()` 且 `preempt_count` 为 0 时调用 `preempt_schedule_irq()`。`exit_rcu` 那一支在这两种模型下仍然提前返回。
- 返回内核，且被打断时 IF 本来就是关闭的：不调用条件抢占。源码注释说明，此时 IRQ 标志状态已经正确，只在 `exit_rcu` 为真时调用 `ct_irq_exit()`（[common.c 第 216～222 行](../../linux/kernel/entry/common.c#L216-L222)）。

`irqentry_exit()` 开头的 `lockdep_assert_irqs_disabled()` 在本配置里是空操作。真正在退出软中断判断前关上本地中断的，是上一节的 `local_irq_disable()`。IDT 路径从 [`error_return`](../../linux/arch/x86/entry/entry_64.S#L1095-L1100)按 CS 的特权级回到内核返回或用户态返回，原生路径最后执行 `iretq`（[entry_64.S 第 651～659 行](../../linux/arch/x86/entry/entry_64.S#L651-L659)）。CPU 启用 FRED 时，用户态事件在 `asm_fred_exit_user` 执行 `ERETU`，内核态事件执行 `ERETS`（[entry_64_fred.S 第 41～43 行](../../linux/arch/x86/entry/entry_64_fred.S#L41-L43)、[第 54～58 行](../../linux/arch/x86/entry/entry_64_fred.S#L54-L58)）。

## 9. 回顾

硬中断路径把一次设备向量变成一次有状态的回调，再把 CPU 交回去。可以把整章收成下面几条：

1. 硬中断上下文是 `preempt_count` 里的 `HARDIRQ_OFFSET`。它在 `irq_enter_rcu()` 加上，在 `irq_exit_rcu()` 减去。主处理函数运行时本地中断应当保持关闭；若回调打开了中断，核心在返回后警告并重新关闭。
2. 没有 FRED 时，设备向量经中断门和 `irq_entries_start` 进入 `common_interrupt()`，返回时执行 `iretq`。有 FRED 时，`fred_extint()` 调用同一个函数；用户态返回执行 `ERETU`，内核态返回执行 `ERETS`。两边都会做入口状态、中断栈和硬中断计数。
3. `vector_irq[vector]` 在当前 CPU 上给出 `irq_desc`。查不到时入口自己 `apic_eoi()`。查到之后，EOI 由流控调用的 `irq_chip` 完成。
4. `depth`、`IRQD_IRQ_DISABLED`、`IRQD_IRQ_MASKED`、`IRQD_IRQ_INPROGRESS` 和 `IRQS_PENDING` 回答的是不同问题。懒禁用可以先只置禁用位，等下一次中断再屏蔽硬件。
5. 电平流控先 `mask_ack` 再调用驱动，结束后再 unmask。边沿流控先 ACK，用 `IRQS_PENDING` 记住处理期间的下一次请求。fasteoi 默认在驱动返回后 EOI。x86 MSI 走边沿，Local APIC 的 EOI 在驱动回调之前：直接父域是 vector 域时经过 `apic_ack_edge()`，是重映射域时直接调用 `apic_ack_irq()`。
6. `request_irq()` 把 `irqaction` 挂到描述符上，并在允许自动启用时 `irq_startup()`。`disable_irq()` 用 `depth` 嵌套，还会等待当前正在运行的处理。`synchronize_irq()` 在芯片支持时还会等到它报告的在途中断结束。`free_irq()` 摘掉 action，最后一项还要同步并 deactivate。
7. 边沿在屏蔽中丢掉的请求，x86 上通过 vector 芯片发送 IPI 来重放。硬件 retrigger 失败时，共享状态字上的 `IRQD_HANDLE_ENFORCE_IRQCTX` 会拒绝 tasklet 重发。电平清掉 pending，把“线是否仍然有效”交给硬件。
8. 最外层硬中断退出、`in_interrupt()` 已经为假、且本 CPU 有软中断 pending 时，默认在当前栈上执行 `__do_softirq()`。关着 bottom half 时这里不会跑。返回用户态可以调度。返回内核时，`exit_rcu` 为真就直接离开；其余路径在默认 voluntary 模型下也不会调用 `preempt_schedule_irq()`。没有 FRED 时最后用 `iretq` 返回，启用 FRED 时用 `ERETU` 或 `ERETS`。

可以用下面的问题检查这一章，而不必背下每个芯片寄存器：

1. 为什么 `in_hardirq()` 为真时，`current` 仍可能是被打断的用户进程？
2. 同一条 MSI IRQ 在驱动回调运行之前，Local APIC 的 EOI 已经发出。设备的中断原因寄存器由谁清除？
3. 边沿 IRQ 的处理函数还没返回，另一个 CPU 又进来一次。什么条件下第二次只置 `IRQS_PENDING` 并屏蔽？什么条件下它会忙等？
4. `disable_irq_nosync()` 返回之后，硬件屏蔽位为什么可能还没置上？下一次中断谁来补上？
5. `synchronize_irq()` 返回，为什么还不能说明设备不会再访问驱动的缓冲区？
6. 默认启动参数下，硬中断从普通内核态返回时为什么不调用 `preempt_schedule_irq()`？改成 `preempt=full` 之后，哪一个调用被重新接上？进入时空闲任务的那一支为什么仍然不接上？
