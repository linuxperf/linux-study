# 第 5 章 中断与异常

本章分析 Linux v6.18.60 在 x86_64 上的中断与异常处理，源码主要位于 `arch/x86/entry/`、`arch/x86/kernel/`（`idt.c`、`traps.c`、`nmi.c`、`irq.c`、`smp.c`）、`arch/x86/kernel/apic/`、`kernel/entry/`、`kernel/irq/`、`kernel/softirq.c`、`kernel/smp.c`、`kernel/irq_work.c` 与 `kernel/workqueue.c`。

中断是内核与硬件、内核与自身其他 CPU 之间最基本的异步通信机制，异常则是 CPU 报告"当前指令出了问题"的同步机制。本章沿一个中断"从硬件到驱动、再到延迟处理"的路径展开：先看 x86 硬件如何投递事件、内核如何进出中断上下文、如何处理各类 CPU 异常与 NMI；再看中断控制器和通用中断层如何把设备信号翻译成驱动的处理函数；最后看软中断、tasklet、工作队列等下半部机制，以及处理器间中断与 irq_work。

```
        ┌──────────────────────── 下半部与延迟执行 ────────────────────────┐
        │ 5.9 软中断 ── 5.10 tasklet / BH 工作队列 ── 5.12 工作队列（kworker）  │
        └───────────────▲──────────────────────────────────▲──────────────┘
                        │ irq_exit_rcu()                   │ queue_work()
        ┌───────────────┴──────── 通用中断层 ─────────────────┴──────────────┐
        │ 5.6 irq_desc / irq_chip / irq_domain ── 5.7 request_irq、流控、线程化  │
        │                     5.8 向量分配与亲和性                             │
        └───────────────▲──────────────────────────────────────────────────┘
                        │ common_interrupt() → vector_irq[]
  ┌─────────────────────┴──── x86 入口 ──────────────────┐   ┌─── 跨 CPU ───┐
  │ 5.1 向量 / IDT / IST ── 5.2 idtentry 与 irqentry      │   │ 5.11 IPI 与   │
  │ 5.3 CPU 异常与异常表 ── 5.4 NMI 与机器检查            │   │   irq_work    │
  └─────────────────────▲──────────────────────────────┘   └──────▲──────┘
                        │ 向量                                     │ ICR
               ┌────────┴────────── 5.5 Local APIC / I/O APIC / MSI ──────────┐
               └──────────────────────────────────────────────────────────────┘
```

*图 5-0：本章各小节的关系*

## 阅读建议

- 5.1、5.2 是全章基础，分别回答"CPU 收到事件后跳到哪里"和"进入 C 代码前后内核做了什么"，建议先读。
- 5.3、5.4 讨论异常与 NMI，可在需要时查阅；5.4 中的嵌套 NMI 处理是入口代码中最精巧的部分。
- 5.5～5.8 是设备中断的主线：硬件控制器 → 通用中断层的数据结构 → 驱动接口与线程化 → 向量与亲和性管理。5.6 是后三节的基础。
- 5.9～5.12 是下半部与延迟执行机制，5.9 软中断是 5.10 的基础；5.12 工作队列相对独立。
- 5.11 的 IPI 与 irq_work 与第 3 章（远程唤醒）、第 7 章（RCU、NMI 安全）联系紧密。

## 本章配置

本章以 `linux/.config` 为准，与中断相关的主要配置项如下：

| 配置项 | 取值 | 影响 |
|---|---|---|
| `CONFIG_X86_LOCAL_APIC` / `CONFIG_X86_IO_APIC` / `CONFIG_X86_X2APIC` | y / y / y | 本地 APIC、I/O APIC、x2APIC 支持；`FIRST_SYSTEM_VECTOR = 0xeb` |
| `CONFIG_PCI_MSI` / `CONFIG_IRQ_REMAP` | y / y | MSI / MSI-X 与中断重映射 |
| `CONFIG_X86_FRED` | y | 编译 FRED 支持，但默认不启用，需 `fred=on` |
| `CONFIG_MITIGATION_PAGE_TABLE_ISOLATION` | y | 入口 / 出口切换 CR3，`sp0` 指向入口栈 |
| `CONFIG_X86_KERNEL_IBT` | n | `IDT_ALIGN = 8`，入口桩无 `ENDBR` |
| `CONFIG_VMAP_STACK` / `CONFIG_KASAN` | y / n | 任务栈与中断栈带保护页；栈大小不翻倍 |
| `CONFIG_GENERIC_ENTRY` / `CONFIG_GENERIC_IRQ_ENTRY` | y / y | 使用 `kernel/entry/` 的通用入口代码 |
| `CONFIG_HAVE_IRQ_EXIT_ON_IRQ_STACK` / `CONFIG_SOFTIRQ_ON_OWN_STACK` | y / y | 软中断在硬中断栈上执行 |
| `CONFIG_CONTEXT_TRACKING_USER` / `CONFIG_NO_HZ_FULL` | y / y | 入口代码通知上下文跟踪与 RCU |
| `CONFIG_SPARSE_IRQ` | y | 中断描述符动态分配，存放于枫树 |
| `CONFIG_IRQ_DOMAIN_HIERARCHY` / `CONFIG_GENERIC_IRQ_MATRIX_ALLOCATOR` | y / y | 分层中断域与 `irq_matrix` 向量分配器 |
| `CONFIG_GENERIC_IRQ_RESERVATION_MODE` / `CONFIG_GENERIC_PENDING_IRQ` | y / y | 保留模式；延迟到中断上下文的亲和性迁移 |
| `CONFIG_IRQ_FORCED_THREADING` | y | 支持 `threadirqs` 强制线程化 |
| `CONFIG_PREEMPT_RT` | n | 软中断、tasklet、irq_work 使用非 RT 语义 |
| `CONFIG_GENERIC_IRQ_DEBUGFS` | n | 无 `/sys/kernel/debug/irq/` |
| `CONFIG_HARDLOCKUP_DETECTOR_PERF` | y | 基于 perf NMI 的硬锁死检测 |
| `CONFIG_X86_MCE` / `CONFIG_X86_MCE_INTEL` / `CONFIG_X86_MCE_AMD` | y / y / y | 机器检查异常处理 |
| `CONFIG_IRQ_WORK` | y | irq_work 与 `IRQ_WORK_VECTOR` |
| `CONFIG_WQ_POWER_EFFICIENT_DEFAULT` / `CONFIG_WQ_CPU_INTENSIVE_REPORT` | y / y | 省电工作队列默认 unbound；报告 CPU 密集工作项 |
| `CONFIG_WQ_WATCHDOG` / `CONFIG_CSD_LOCK_WAIT_DEBUG` | n / n | 无工作队列看门狗、无 CSD 锁超时诊断 |
| `CONFIG_HZ` | 1000 | `MAX_SOFTIRQ_TIME` 为 2 个 jiffy |

## 小节索引

| 小节 | 状态 |
|---|---|
| [5.1 x86_64 中断与异常体系：向量、IDT 与中断栈](01-x86-idt.md) | 已完成 |
| [5.2 中断入口与出口：idtentry 与 irqentry 框架](02-entry.md) | 已完成 |
| [5.3 CPU 异常处理：陷阱、故障与异常修复](03-exceptions.md) | 已完成 |
| [5.4 NMI 与机器检查](04-nmi.md) | 已完成 |
| [5.5 中断控制器：Local APIC、I/O APIC 与 MSI](05-apic.md) | 已完成 |
| [5.6 通用中断层：irq_desc、irq_chip 与 irq_domain](06-irq-core.md) | 已完成 |
| [5.7 中断申请、流控处理与线程化中断](07-request-thread.md) | 已完成 |
| [5.8 中断向量分配与亲和性](08-vector-affinity.md) | 已完成 |
| [5.9 软中断](09-softirq.md) | 已完成 |
| [5.10 tasklet 与 BH 工作队列](10-tasklet-bh-wq.md) | 已完成 |
| [5.11 处理器间中断与 irq_work](11-ipi-irq-work.md) | 已完成 |
| [5.12 工作队列](12-workqueue.md) | 已完成 |

## 待完善

- [ ] 各小节"动手实验"尚未在 dev 虚拟机中实测，需补充真实输出示例（注明内核版本、vCPU 数与配置）；部分实验依赖 PMU 直通、多 vCPU 或 UMIP 关闭等虚拟机条件。
- [ ] 5.1 的 `sidt` 实验受 UMIP 影响，需确认 dev 虚拟机的 CPU 特性后再给出预期输出。
- [ ] 5.2、5.4 中 FRED 部分仅基于源码分析，尚未在支持 FRED 的硬件上验证 `fred=on` 的行为。
- [ ] 5.4 的机器检查部分只覆盖入口与可恢复路径，MCA bank 解析、CMCI 与 `mce_severity()` 的细节可另行展开。
- [ ] 5.5 未展开 Intel VT-d / AMD-Vi 中断重映射驱动（`drivers/iommu/`）与 posted interrupt（`CONFIG_X86_POSTED_MSI` 未开启）。
- [ ] 部分历史版本号（如 IDT 重构 v4.14、x86 分层 irqdomain v3.19～v4.1、清理 IPI 改为定时器 v6.6、BH 工作队列 v6.9）依据社区资料整理，后续可补充对应提交号以便核对。
- [ ] 若干代码块（如 `arch/x86/entry/entry_64.S:1207-1247`、`kernel/irq/manage.c:1553-1612`、`kernel/smp.c:811-879`、`kernel/workqueue.c:187-236`）超过 40 行建议值，可进一步拆分讲解。
- [ ] 图 5-3、5-7、5-9、5-10、5-14 的 Mermaid 时序图与图 5-4、5-12 的状态图需在目标渲染器中确认显示效果。
