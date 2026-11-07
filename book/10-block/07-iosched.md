# 10.7 I/O 调度器：mq-deadline、Kyber 与 BFQ

> **本节源码**：[`block/elevator.c`](../../linux/block/elevator.c)、[`block/elevator.h`](../../linux/block/elevator.h)、[`block/mq-deadline.c`](../../linux/block/mq-deadline.c)、[`block/kyber-iosched.c`](../../linux/block/kyber-iosched.c)、[`block/bfq-iosched.c`](../../linux/block/bfq-iosched.c)
> **关键函数**：`elv_iosched_store()`、`dd_dispatch_request()`、`__dd_dispatch_request()`、`kyber_dispatch_request()`、`bfq_dispatch_request()`
> **前置知识**：10.5 标签、10.6 合并与派发、3.3 加权调度的基本概念

## 概述

多队列解决并行提交，不会自动解决请求公平、读写干扰和尾延迟（Tail Latency）。I/O 调度器在请求尚未交给设备前控制选择顺序和在途数量，代价是额外的数据结构、锁与计算。mq-deadline 兼顾地址顺序和等待时间，Kyber 通过令牌反馈控制并发，预算公平排队（Budget Fair Queueing，BFQ）则围绕队列预算与权重分配服务。三者的目标不同，不存在不依赖设备与负载的统一最优选择。

## 核心数据结构

调度框架通过 `elevator_mq_ops` 暴露 `insert_requests`、`dispatch_request`、`bio_merge`、`completed_request` 等钩子，定义见 [`block/elevator.h:57-89`](../../linux/block/elevator.h#L57-L89)。`request_queue.elevator` 保存当前实例，`hctx.sched_data` 可保存每硬件上下文的调度器数据。

| 调度器 | 核心结构 | 主要状态 |
|---|---|---|
| mq-deadline | `deadline_data`、`dd_per_prio` | 每优先级、每读写方向的红黑树与 FIFO，批次计数、写饥饿计数 |
| Kyber | `kyber_queue_data`、`kyber_hctx_data` | 设备级域令牌、per-CPU 时延直方图、hctx 域队列 |
| BFQ | `bfq_data`、`bfq_queue`、`bfq_entity` | 服务中的队列、预算、已服务量、权重、层次父实体 |

定义分别见 [`block/mq-deadline.c:73-105`](../../linux/block/mq-deadline.c#L73-L105)、[`block/kyber-iosched.c:133-188`](../../linux/block/kyber-iosched.c#L133-L188) 和 [`block/bfq-iosched.h:151-202`](../../linux/block/bfq-iosched.h#L151-L202)。

## 主流程分析

以下表示调度框架的插入与取出关系，而不是一次调用中嵌套完成全部步骤。

[`block/blk-mq.c:2923-2966`](../../linux/block/blk-mq.c#L2923-L2966)、[`block/blk-mq-sched.c:85-188`](../../linux/block/blk-mq-sched.c#L85-L188)、[`block/elevator.h:57-89`](../../linux/block/elevator.h#L57-L89)（v6.18.60）

```text
请求插入
blk_mq_dispatch_list()                    block/blk-mq.c
└── elevator->type->ops.insert_requests()
    ├── dd_insert_requests()              block/mq-deadline.c
    ├── kyber_insert_requests()           block/kyber-iosched.c
    └── bfq_insert_requests()             block/bfq-iosched.c
队列运行
blk_mq_sched_dispatch_requests()          block/blk-mq-sched.c
└── blk_mq_do_dispatch_sched()            block/blk-mq-sched.c
    └── __blk_mq_do_dispatch_sched()      block/blk-mq-sched.c
        ├── elevator->type->ops.dispatch_request()
        │   ├── dd_dispatch_request()    block/mq-deadline.c
        │   ├── kyber_dispatch_request() block/kyber-iosched.c
        │   └── bfq_dispatch_request()   block/bfq-iosched.c
        └── 取得驱动标签，再经 blk_mq_dispatch_rq_list() 派发
```

### 1. mq-deadline：两种排序同时存在

同一请求同时出现在按地址排序的红黑树和按到达顺序组织的 FIFO 中。前者便于连续访问，后者避免只顾地址顺序而让旧请求长期等待。读写分方向，I/O 优先级分 RT、BE、IDLE 三档，见 [`block/mq-deadline.c:42-84`](../../linux/block/mq-deadline.c#L42-L84)。

`__dd_dispatch_request()` 的选择顺序可以概括为：

1. 当前方向仍有后继且批次未满时，继续当前批次。
2. 重新选择方向时倾向读；写被读跳过达到限制后，转向写。
3. 所选方向的 FIFO 到期或没有地址后继时，从 FIFO 最旧项重新开始；否则延续地址顺序。
4. 从树和 FIFO 移除选中请求，交回派发框架。

实现见 [`block/mq-deadline.c:326-414`](../../linux/block/mq-deadline.c#L326-L414)。默认参数如下：

[`block/mq-deadline.c:30-39`](../../linux/block/mq-deadline.c#L30-L39)（v6.18.60）

```c
static const int read_expire = HZ / 2;  /* max time before a read is submitted. */
static const int write_expire = 5 * HZ; /* ditto for writes, these limits are SOFT! */
...
static const int prio_aging_expire = 10 * HZ;
static const int writes_starved = 2;    /* max times reads can starve a write */
static const int fifo_batch = 16;       /* # of sequential requests treated as one
				     by the above parameters. For throughput. */
```

这里的到期是软目标，不承诺请求在 500 ms 或 5 s 内完成；批处理、设备资源与设备内部执行都可能增加等待。优先级之间也有老化处理，见 [`block/mq-deadline.c:416-490`](../../linux/block/mq-deadline.c#L416-L490)。

mq-deadline 的排序状态为整条设备队列共享，而不是每个 hctx 各自独立。`dd_dispatch_request()` 可能返回属于另一个 hctx 的请求，框架随后按实际 hctx 分组派发，见 [`block/mq-deadline.c:445-453`](../../linux/block/mq-deadline.c#L445-L453) 和 [`block/blk-mq-sched.c:156-169`](../../linux/block/blk-mq-sched.c#L156-L169)。

### 2. Kyber：以令牌调节在途深度

Kyber 把操作分成 READ、WRITE、DISCARD、OTHER 域。每个域的设备级令牌限制在途数量，hctx 在派发请求前必须取得对应令牌；没有令牌时注册等待，而不是继续灌满设备。结构与取令牌路径见 [`block/kyber-iosched.c:150-188`](../../linux/block/kyber-iosched.c#L150-L188)、[`block/kyber-iosched.c:693-788`](../../linux/block/kyber-iosched.c#L693-L788)。

完成路径同时采集从请求统计起点算起的总时延和从 `io_start_time_ns` 算起的 I/O 时延，见 [`block/kyber-iosched.c:626-646`](../../linux/block/kyber-iosched.c#L626-L646)。定时聚合直方图后调整域深度，使并发控制适应当前设备压力；默认读 / 写目标分别是 2 ms / 10 ms，见 [`block/kyber-iosched.c:70-74`](../../linux/block/kyber-iosched.c#L70-L74)。目标是反馈参数，不是每条请求的硬截止时间。

### 3. BFQ：用服务预算表达公平

BFQ 不把“占用设备多久”直接作为唯一服务单位，而是为队列分配按扇区计量的预算。调度实体记录 `start`、`finish`、`service`、`budget` 和 `weight`；父实体使组调度能递归分配服务，见 [`block/bfq-iosched.h:151-194`](../../linux/block/bfq-iosched.h#L151-L194)。

`__bfq_dispatch_request()` 选择服务队列，再从中派发请求，并跟踪驱动中的请求数，见 [`block/bfq-iosched.c:5156-5243`](../../linux/block/bfq-iosched.c#L5156-L5243)。其 B-WF2Q+ 引擎、交互队列权重提升、空闲等待以及对低速队列的时间计费修正，共同决定最终行为；不能把 BFQ 简化成“按权重轮询扇区”。总体设计说明见 [`block/bfq-iosched.c:16-94`](../../linux/block/bfq-iosched.c#L16-L94)，时间公平修正见 [`block/bfq-wf2q.c:820-853`](../../linux/block/bfq-wf2q.c#L820-L853)。

## 关键细节 / 设计考量

### 用控制目标对照，而不是把三种算法混成一种

| 策略 | 首要组织单位 | 主要控制方式 | 观测重点 |
|---|---|---|---|
| `none` | 通用 blk-mq 请求 | 不安装 elevator 策略 | 标签与驱动资源等待，不能假设没有排队 |
| mq-deadline | 优先级 / 方向中的请求 | 地址顺序、FIFO 软到期与批次 | 请求等待、读写饥饿和批量派发 |
| Kyber | 操作域中的请求 | 域令牌与时延反馈 | 令牌深度、throttled 事件与时延分布 |
| BFQ | 队列与层次实体 | 服务预算、权重及低延迟启发式 | 各队列 / 组得到的服务与尾延迟 |

本节前面三个算法小节已经给出对应实现。比较时还要记录 [10.12](12-blkcg-qos.md) 的设备级 / cgroup 控制器，避免把进入调度器前的限速误算为调度算法本身的效果。

### none 的准确含义

`none` 表示 `q->elevator` 为空，不安装上述调度策略。它保留 blk-mq 的标签、合并、plug、软件队列与资源重试。当前调度器输出在 [`block/elevator.c:834-865`](../../linux/block/elevator.c#L834-L865) 中通过方括号标识。

### 不要把选择策略写成介质类型定律

高速、并行度高的设备可能更在意调度 CPU 成本；交互负载可能更在意尾延迟；混合读写或多个租户还要考虑干扰与公平。应固定负载、并发度和其他控制器设置后比较，而不是只按 HDD / SSD 名称决定。

### 调度器不掌控设备内部全部顺序

驱动接受请求后，设备仍可能重排执行。BFQ 的 `strict_guarantees` 会限制同时交给驱动的请求以强化顺序，但可能损失吞吐量，源码明确说明了这一权衡，见 [`block/bfq-iosched.c:5213-5226`](../../linux/block/bfq-iosched.c#L5213-L5226)。

## 可观测性

配置为 `CONFIG_MQ_IOSCHED_DEADLINE=y`、`CONFIG_MQ_IOSCHED_KYBER=m`、`CONFIG_IOSCHED_BFQ=m`；BFQ 组调度依赖本配置的 `CONFIG_BFQ_GROUP_IOSCHED=y`。sysfs 还依赖 `CONFIG_SYSFS=y`；事件接口依赖 `CONFIG_TRACEPOINTS=y`、`CONFIG_EVENT_TRACING=y`、`CONFIG_TRACING=y`。

| 入口 | 看什么 | 实现 |
|---|---|---|
| `/sys/block/<disk>/queue/scheduler` | 当前策略与已注册策略；模块未加载不代表未编译 | [`block/elevator.c:785-865`](../../linux/block/elevator.c#L785-L865) |
| `/sys/block/<disk>/queue/iosched/read_expire`、`write_expire`、`fifo_batch` | mq-deadline 软到期与批次设置 | [`block/mq-deadline.c:761-816`](../../linux/block/mq-deadline.c#L761-L816) |
| `kyber:kyber_latency`、`kyber:kyber_adjust`、`kyber:kyber_throttled` | 域时延、令牌深度调整、令牌短缺 | [`include/trace/events/kyber.h:14-92`](../../linux/include/trace/events/kyber.h#L14-L92)，另依赖 `CONFIG_TRACING=y` |
| BFQ 的 `queue/iosched` 属性 | `low_latency`、`slice_idle` 等启发式设置 | [`block/bfq-iosched.c:7588-7600`](../../linux/block/bfq-iosched.c#L7588-L7600) 中的 `bfq_attrs` |

切换策略会协调队列冻结与调度器状态，不能在生产设备上把频繁切换当成无影响观测。实验只在 [10.14](14-observability.md) 的专用 `null_blk` 上进行；固定延时的模拟设备只验证路径差异，不用于给三种策略排通用名次。

## 小结

- mq-deadline 同时维护地址树与到达 FIFO，在吞吐、读优先和防饥饿之间折中。
- Kyber 用域令牌与时延反馈调节并发，目标时延不等于完成保证。
- BFQ 以服务预算和权重组织队列及组，并包含低延迟与时间公平修正。
- `none` 只是不安装 I/O 调度器，不删除通用块层机制。
- 性能比较必须明确设备、负载、并发度以及同时启用的控制策略。

## 参考

- [`Documentation/block/deadline-iosched.rst`](../../linux/Documentation/block/deadline-iosched.rst)
- [`Documentation/block/kyber-iosched.rst`](../../linux/Documentation/block/kyber-iosched.rst)
- [`Documentation/block/bfq-iosched.rst`](../../linux/Documentation/block/bfq-iosched.rst)
- [`Documentation/block/switching-sched.rst`](../../linux/Documentation/block/switching-sched.rst)
