# 第 3 章 调度器

本章分析 Linux v6.18.60 在 x86_64 上的进程调度子系统，源码主要位于 `kernel/sched/`。

调度器决定每个 CPU 在每一时刻运行哪个任务。v6.18 的调度器由一个策略无关的核心层和六个调度类组成：stop、deadline、实时（RT）、公平（fair，自 v6.6 起采用 EEVDF 算法取代 CFS）、sched_ext 和 idle。本章先建立整体框架，再沿着"选择 → 切换 → 唤醒 → 抢占"的主线展开，最后讨论各调度类、负载跟踪与多核负载均衡。

## 阅读建议

- 3.1、3.2 是全章基础，建议按顺序阅读。
- 3.3、3.4 构成公平调度的完整生命周期；3.5 介绍与调度类无关的调度时钟、无 tick 模式和抢占模型。三节是本章的重点。
- 3.6～3.11 相对独立，可按兴趣选读。
- 3.12 以公平调度类的回调为线索，把 3.3、3.8～3.10 的内容串联起来，并分析唤醒与 fork 时的选核策略，建议在读完这几节后阅读。

## 本章配置

本章以 `linux/.config` 为准，与调度相关的主要配置项如下：

| 配置项 | 取值 | 影响 |
|---|---|---|
| `CONFIG_SMP` | y | 多核负载均衡、远程唤醒 |
| `CONFIG_PREEMPT` / `CONFIG_PREEMPT_DYNAMIC` / `CONFIG_PREEMPT_RT` | y / y / y | 实时内核，默认 full 抢占模型，可在启动参数或运行时切换为 lazy（3.5 节） |
| `CONFIG_NO_HZ_FULL` | y | 无 tick 运行 |
| `CONFIG_HZ` | 1000 | tick 周期 1 ms |
| `CONFIG_FAIR_GROUP_SCHED` / `CONFIG_CFS_BANDWIDTH` | y / y | 公平类组调度与带宽控制 |
| `CONFIG_RT_GROUP_SCHED` | n | 无 RT 组调度 |
| `CONFIG_SCHED_CORE` | y | 核心调度（SMT 隔离） |
| `CONFIG_UCLAMP_TASK` | y | 利用率钳制 |
| `CONFIG_SCHED_CLASS_EXT` | n | sched_ext 未编译 |
| `CONFIG_SCHED_PROXY_EXEC` | n | 代理执行未开启，`rq->curr` 与 `rq->donor` 等价 |

## 小节索引

| 小节 | 状态 |
|---|---|
| [3.1 调度器框架：调度策略、调度类与运行队列](01-overview.md) | 已完成 |
| [3.2 主调度函数与上下文切换](02-schedule.md) | 已完成 |
| [3.3 EEVDF 公平调度算法](03-eevdf.md) | 已完成 |
| [3.4 任务创建与唤醒](04-wakeup.md) | 已完成 |
| [3.5 调度时钟与抢占模型](05-tick-preempt.md) | 已完成 |
| [3.6 实时调度类](06-rt.md) | 已完成 |
| [3.7 Deadline 调度类与 dl_server](07-deadline.md) | 已完成 |
| [3.8 PELT 负载跟踪](08-pelt.md) | 已完成 |
| [3.9 调度域与负载均衡](09-load-balance.md) | 已完成 |
| [3.10 组调度与 CPU 带宽控制](10-group-sched.md) | 已完成 |
| [3.11 sched_ext 可扩展调度器](11-sched-ext.md) | 已完成 |
| [3.12 公平调度类：回调实现与唤醒选核](12-fair-class.md) | 已完成 |

## 待完善

- [ ] 当前 `linux/.config` 为 `CONFIG_PREEMPT_LAZY=y`，`CONFIG_PREEMPT`、`CONFIG_PREEMPT_RT`、`CONFIG_SCHED_CLASS_EXT` 均未开启。3.2 节已按此复核；上方"本章配置"表中抢占一行（y / y / y）与 3.5 节仍按 `CONFIG_PREEMPT=y`、`CONFIG_PREEMPT_RT=y` 编写，其余各节也需逐节复核；3.3 节"`rq_clock_task()` 不含中断处理时间"在未开启 `CONFIG_IRQ_TIME_ACCOUNTING` 的本书配置下不成立。
- [ ] 原 3.5 节中各调度类对 `sched_can_stop_tick()` 的具体判据（Deadline、`SCHED_RR` / `SCHED_FIFO`、多个公平任务）已移出，需在 3.3、3.6 节补充（3.7、3.10 节已有相应说明）。
- [ ] 按 AGENTS.md 3.1 节要求，3.3～3.12 尚缺"可观测性"一节（3.2 已补充，3.1 为概述小节可省略）。
- [ ] 3.1～3.12 的"动手实验"尚未在 dev 虚拟机中实测，需补充真实输出示例（注明内核版本与配置）。
- [ ] 3.11 的实验需要开启 `CONFIG_SCHED_CLASS_EXT`（依赖 `CONFIG_DEBUG_INFO_BTF`）的内核，本书默认配置无法直接运行。
- [ ] 各节 Mermaid 图（图 3-1、3-3、3-5、3-7、3-8、3-10、3-12、3-16、3-17、3-18、3-20）需在目标渲染器中确认显示效果。
- [ ] 3.7 引用的 [`kernel/sched/deadline.c:1583-1782`](../../linux/kernel/sched/deadline.c#L1583-L1782) dl_server 状态图注释中的 `dl_defer_idle` 字段在 v6.18.60 结构体中不存在，后续版本若同步修正需更新正文说明。
