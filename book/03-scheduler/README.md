# 第 3 章 调度器

本章分析 Linux v6.18.60 在 x86_64 上的进程调度子系统，源码主要位于 `kernel/sched/`。

调度器决定每个 CPU 在每一时刻运行哪个任务。v6.18 的调度器由一个策略无关的核心层和六个调度类组成：stop、deadline、实时（RT）、公平（fair，自 v6.6 起采用 EEVDF 算法取代 CFS）、sched_ext 和 idle。本章先建立整体框架，再沿着"选择 → 切换 → 唤醒 → 抢占"的主线展开，最后讨论各调度类、负载跟踪与多核负载均衡。

## 阅读建议

- 3.1、3.2 是全章基础，建议按顺序阅读。3.2 先沿 `schedule()` 与 `__schedule()` 连续讲解主调度的五个步骤，区分“进入调度器”和“发生切换”；再展开 `context_switch()` 的地址空间、内核栈与收尾细节。重调度请求传播和 x86 抢占计数编码放在该节的“关键细节 / 设计考量”中。
- 3.3 解释公平类的选择与延迟出队；3.4 从调度器核心层分析首次入队和普通唤醒，按状态匹配、队列修复、执行交接、选核、激活展开，再解释跨 CPU 委托。阅读 3.4 时应区分「唤醒被接收」「恢复可运行」「被选中执行」，不要把它们看成同一时刻。
- 3.5 介绍 rq 时钟、无 tick 模式的依赖判据，以及重调度请求与安全抢占点的关系，并给出观测入口。3.3～3.5 是本章的重点。
- 3.6～3.11 相对独立，可按兴趣选读。3.8 解释 PELT 衰减、实体与队列聚合、频率 / 算力不变性及 `util_est`，为负载均衡和选核决策提供信号基础，并列出观测入口。
- 3.12 以公平调度类的回调为线索，把 3.3、3.8～3.10 的内容串联起来，并分析唤醒与 fork 时的选核策略，建议在读完这几节后阅读。

## 本章配置

本章以 `linux/.config` 为准，与调度相关的主要配置项如下：

| 配置项 | 取值 | 影响 |
|---|---|---|
| `CONFIG_SMP` | y | 多核负载均衡、远程唤醒 |
| `CONFIG_PREEMPT_LAZY` / `CONFIG_PREEMPT_DYNAMIC` | y / y | 默认 lazy，可在启动参数或运行时切换 none / voluntary / full / lazy（3.5 节） |
| `CONFIG_PREEMPT` / `CONFIG_PREEMPT_RT` | n / n | 非 RT；`CONFIG_PREEMPTION=y`，仍支持内核抢占 |
| `CONFIG_NO_HZ_FULL` | y | 支持运行中停 tick，还需 `nohz_full=` 指定 CPU 并满足依赖条件 |
| `CONFIG_HZ` | 1000 | 周期 tick 开启时的目标间隔为 1 ms |
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
| [3.4 任务创建与唤醒](04-wakeup.md) | 已完成（已重组主流程并复核源码、观测口径；实验待实测） |
| [3.5 调度时钟、无 tick 模式与抢占模型](05-tick-preempt.md) | 已完成 |
| [3.6 实时调度类](06-rt.md) | 已完成 |
| [3.7 Deadline 调度类与 dl_server](07-deadline.md) | 已完成 |
| [3.8 PELT 负载跟踪与利用率估计](08-pelt.md) | 已完成（已复核源码，含可观测性） |
| [3.9 调度域与负载均衡](09-load-balance.md) | 已完成 |
| [3.10 组调度与 CPU 带宽控制](10-group-sched.md) | 已完成 |
| [3.11 sched_ext 可扩展调度器](11-sched-ext.md) | 已完成 |
| [3.12 公平调度类：回调实现与唤醒选核](12-fair-class.md) | 已完成 |

## 待完善

- [ ] 当前 `linux/.config` 为 `CONFIG_PREEMPT_LAZY=y`，`CONFIG_PREEMPT`、`CONFIG_PREEMPT_RT`、`CONFIG_SCHED_CLASS_EXT` 均未开启。3.2、3.4、3.5 节与本章配置表已按此复核，其余各节仍需逐节核查；3.3 节“`rq_clock_task()` 不含中断处理时间”在未开启 `CONFIG_IRQ_TIME_ACCOUNTING` 的本书配置下不成立。
- [ ] 3.5 节已补齐 `sched_can_stop_tick()` 的顺序判据；3.3、3.6 节仍需从各调度类角度补充停 tick 的限制（3.7、3.10 节已有相应说明）。
- [ ] 按 AGENTS.md 3.1 节要求，3.3、3.6～3.7、3.9～3.12 尚缺“可观测性”一节（3.2、3.4、3.5、3.8 已补充，3.1 为概述小节可省略）。
- [ ] 3.1～3.12 的"动手实验"尚未在 dev 虚拟机中实测，需补充真实输出示例（注明内核版本与配置）。
- [ ] 3.11 的实验需要开启 `CONFIG_SCHED_CLASS_EXT`（依赖 `CONFIG_DEBUG_INFO_BTF`）的内核，本书默认配置无法直接运行。
- [ ] 各节 Mermaid 图（图 3-1、3-3、3-5、3-7、3-8、3-10、3-12、3-16、3-17、3-18、3-20）需在目标渲染器中确认显示效果。
- [ ] 3.7 引用的 [`kernel/sched/deadline.c:1583-1782`](../../linux/kernel/sched/deadline.c#L1583-L1782) dl_server 状态图注释中的 `dl_defer_idle` 字段在 v6.18.60 结构体中不存在，后续版本若同步修正需更新正文说明。
