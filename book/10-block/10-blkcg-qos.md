# 10.10 块 I/O 控制组与服务质量

> **本节源码**：[`block/blk-cgroup.c`](../../linux/block/blk-cgroup.c)、[`block/blk-cgroup.h`](../../linux/block/blk-cgroup.h)、[`block/blk-throttle.c`](../../linux/block/blk-throttle.c)、[`block/blk-rq-qos.h`](../../linux/block/blk-rq-qos.h)、[`block/blk-wbt.c`](../../linux/block/blk-wbt.c)、[`block/blk-iocost.c`](../../linux/block/blk-iocost.c)
> **关键函数**：`bio_associate_blkg()`、`blk_cgroup_bio_start()`、`__blk_throtl_bio()`、`rq_qos_throttle()`、`blkcg_print_stat()`
> **前置知识**：10.4 提交路径、10.5 标签、10.7 I/O 调度器、4.14 memcg

## 概述

多个租户共享设备时，提升总吞吐量并不能保证资源隔离。块 I/O 控制组负责给访问标注归属，并为统计、限速和比例分配提供基础；QoS 钩子则允许策略在请求分配、派发与完成等阶段施加控制。绝对带宽上限、权重公平和保护读时延解决的问题不同，也不都依赖 I/O 调度器。本节以 cgroup v2 为主，明确当前配置下哪些机制可用。

## 核心数据结构

### 1. blkcg 与 blkg

`blkcg` 表示 I/O 控制组，`blkcg_gq` 简称 `blkg`，表示控制组与队列的关联。一个控制组访问多个设备时，需要多个 `blkg`。

[`block/blk-cgroup.h:56-75`](../../linux/block/blk-cgroup.h#L56-L75)（v6.18.60）

```c
struct blkcg_gq {
	/* Pointer to the associated request_queue */
	struct request_queue		*q;
	struct list_head		q_node;
	struct hlist_node		blkcg_node;
	struct blkcg			*blkcg;

	/* all non-root blkcg_gq's are guaranteed to have access to parent */
	struct blkcg_gq			*parent;
	...
```

`bio->bi_blkg` 记录访问归属，`blkg` 还保存 per-CPU 统计和各策略的私有状态。`CONFIG_BLK_CGROUP=y` 是这些字段及基础机制的前提。

### 2. rq_qos：请求生命周期上的策略链

[`block/blk-rq-qos.h:37-50`](../../linux/block/blk-rq-qos.h#L37-L50)（v6.18.60）

```c
struct rq_qos_ops {
	void (*throttle)(struct rq_qos *, struct bio *);
	void (*track)(struct rq_qos *, struct request *, struct bio *);
	void (*merge)(struct rq_qos *, struct request *, struct bio *);
	void (*issue)(struct rq_qos *, struct request *);
	void (*requeue)(struct rq_qos *, struct request *);
	void (*done)(struct rq_qos *, struct request *);
	void (*done_bio)(struct rq_qos *, struct bio *);
	void (*cleanup)(struct rq_qos *, struct bio *);
	...
```

这些钩子让策略在等待、合并、执行与完成阶段维护一致状态。`cleanup` 尤其重要：节流已记账、请求分配却失败时，也必须撤销对应占用。

## 主流程分析

下面把归属初始化与实际提交流程分开；它们不必发生在同一个函数调用内。

```text
构造或改变 bio 目标设备
└── bio_associate_blkg()                  block/blk-cgroup.c
    └── 按 css 与目标队列建立 / 更新 blkg
提交
submit_bio_noacct()                       block/blk-core.c
├── blk_throtl_bio()                      block/blk-throttle.h
│   └── __blk_throtl_bio()                block/blk-throttle.c
└── submit_bio_noacct_nocheck()            block/blk-core.c
    ├── blk_cgroup_bio_start()            block/blk-cgroup.c
    └── blk_mq_submit_bio()               block/blk-mq.c
        └── blk_mq_get_new_requests()
            ├── rq_qos_throttle()
            └── 分配请求 → rq_qos_track()
驱动开始与最终完成
├── rq_qos_issue()
├── rq_qos_done()
└── rq_qos_done_bio()
```

### 1. 归属不是简单读取 current

`bio_associate_blkg()` 优先保留 bio 已携带的 cgroup 身份，再按当前目标队列重建关联；否则才从当前 I/O 上下文取得身份，见 [`block/blk-cgroup.c:2157-2184`](../../linux/block/blk-cgroup.c#L2157-L2184)。克隆也会传播归属信息。

回写经常由内核线程执行，若只看 `current` 就会把应用产生的脏数据误计到回写线程。cgroup 回写（Cgroup Writeback）通过 inode / 回写上下文把归属传递到 bio，依赖 `CONFIG_CGROUP_WRITEBACK=y` 及文件系统支持。接口与限制见 [`Documentation/admin-guide/cgroup-v2.rst:2146-2197`](../../linux/Documentation/admin-guide/cgroup-v2.rst#L2146-L2197) 和 [`include/linux/writeback.h:247-267`](../../linux/include/linux/writeback.h#L247-L267) 中的 `wbc_init_bio()`。

### 2. io.max：在 bio 层控制绝对上限

`io.max` 支持按设备配置 `rbps`、`wbps`、`riops`、`wiops`，策略注册见 [`block/blk-throttle.c:1628-1638`](../../linux/block/blk-throttle.c#L1628-L1638)。`__blk_throtl_bio()` 从当前组沿祖先检查预算；超过限制就把 bio 放入服务队列，由定时器和工作项后续推进，而不是阻塞硬件完成路径，见 [`block/blk-throttle.c:1751-1845`](../../linux/block/blk-throttle.c#L1751-L1845)。

这条路径在构造 `request` 之前运行，因此无需安装 BFQ 或 mq-deadline 才生效。时间片与分层推进允许短时间突发，不能以极短采样窗口的瞬时速率要求严格等于设置值；接口语义见 [`Documentation/admin-guide/cgroup-v2.rst:2107-2143`](../../linux/Documentation/admin-guide/cgroup-v2.rst#L2107-L2143)。

### 3. WBT：用写侧背压保护时延

回写节流（Writeback Throttling，WBT）通过请求统计感知设备压力，并控制相关写入的在途数量。`get_limit()` 对同步、高优先级、后台与普通写采用不同限制；等待通过 `rq_qos_wait()` 完成，见 [`block/blk-wbt.c:533-595`](../../linux/block/blk-wbt.c#L533-L595)。

WBT 是设备级反馈，不是每个 cgroup 的带宽合同，也不是让所有写无差别等待。当前 `wbt_should_throttle()` 对带特定直接 I/O 标志组合的写有豁免，见 [`block/blk-wbt.c:597-614`](../../linux/block/blk-wbt.c#L597-L614)。即使 `CONFIG_BLK_WBT=y`、`CONFIG_BLK_WBT_MQ=y`，仍要检查队列的运行时开关。

### 4. IOCost：估算设备时间成本，再按权重分配

同样 1 MB 的顺序读和随机小读可能消耗完全不同的设备能力，单按字节或 IOPS 平分未必公平。I/O 成本控制器（I/O Cost Controller，IOCost）用顺序 / 随机基础成本加长度成本估算设备时间，再按层次权重推进各组虚拟时间。反馈环调节设备虚拟时间速率，以适应饱和与负载变化，设计说明见 [`block/blk-iocost.c:9-120`](../../linux/block/blk-iocost.c#L9-L120)。

当前 `CONFIG_BLK_CGROUP_IOCOST=y` 提供实现，但按设备启用、模型与 QoS 设置仍是运行时操作。`io.weight`、`io.cost.qos` 和 `io.cost.model` 的注册见 [`block/blk-iocost.c:3527-3548`](../../linux/block/blk-iocost.c#L3527-L3548)。设置权重本身不能证明设备已启用成本控制。

### 5. 不同权重接口不要混用

BFQ 的 cgroup v2 权重接口是 `io.bfq.weight`，依赖 BFQ 已被选为设备调度器及 `CONFIG_BFQ_GROUP_IOSCHED=y`，定义见 [`block/bfq-cgroup.c:1386-1394`](../../linux/block/bfq-cgroup.c#L1386-L1394)。它不是 IOCost 的 `io.weight`。当前 `CONFIG_BLK_CGROUP_IOLATENCY` 未启用，因此本配置不应期待出现 `io.latency`，即使源码中有完整策略实现。

## 关键细节 / 设计考量

### 统计口径不等于硬件完成计数

非根 cgroup 的 `blk_cgroup_bio_start()` 在 bio 提交侧累加字节和次数，`BIO_CGROUP_ACCT` 防止拆分重复累计字节，但次数可以随拆分增加，见 [`block/blk-cgroup.c:2207-2249`](../../linux/block/blk-cgroup.c#L2207-L2249)。根组又从系统设备统计取数。因此 `io.stat` 的 `rios` / `wios` 与设备完成请求数不必完全相同，不能仅按两者差值判定丢 I/O。

### 控制器叠加需要标明实验条件

`io.max`、IOCost、WBT 与 I/O 调度器作用于不同位置，观测到的总等待可能由多个策略共同产生。分析一个控制器时，应记录其他策略状态，不能只改变一个权重值就把所有现象归因于该权重。

## 可观测性

下列 cgroup 入口要求 cgroup v2 已挂载，且相应父级通过 `cgroup.subtree_control` 启用了 `io` 控制器；编译配置不替代运行时授权与层次设置。

| 入口 | 对应信息 | 源码 / 当前配置 |
|---|---|---|
| `<cgroup>/io.stat` | 每设备 `rbytes`、`wbytes`、`rios`、`wios`、`dbytes`、`dios` | [`block/blk-cgroup.c:1182-1257`](../../linux/block/blk-cgroup.c#L1182-L1257)，`CONFIG_BLK_CGROUP=y` |
| `<cgroup>/io.max` | 绝对限速设置，`max` 表示无限制 | [`block/blk-throttle.c:1560-1638`](../../linux/block/blk-throttle.c#L1560-L1638)，`CONFIG_BLK_DEV_THROTTLING=y` |
| 根组 `io.cost.qos` / `io.cost.model`，子组 `io.weight` | IOCost 按设备启用与参数、组权重 | [`block/blk-iocost.c:3527-3548`](../../linux/block/blk-iocost.c#L3527-L3548)，`CONFIG_BLK_CGROUP_IOCOST=y` |
| `<cgroup>/io.bfq.weight` | BFQ 权重，不代表 BFQ 在该设备必然生效 | [`block/bfq-cgroup.c:1386-1394`](../../linux/block/bfq-cgroup.c#L1386-L1394) |
| `/sys/block/<disk>/queue/wbt_lat_usec` | WBT 目标及运行时开关 | [`block/blk-sysfs.c:620-693`](../../linux/block/blk-sysfs.c#L620-L693)，`CONFIG_SYSFS=y`、`CONFIG_BLK_WBT=y` |
| `wbt:wbt_stat`、`wbt:wbt_step` | 读写时延统计、深度缩放与限制 | [`include/trace/events/wbt.h:15-126`](../../linux/include/trace/events/wbt.h#L15-L126)，`CONFIG_TRACING=y` |

动态探针可在 `__blk_throtl_bio()` 或相应策略的 throttle 方法观察是否进入限流，但仅测函数执行时间不一定包含 bio 在异步服务队列中的全部停留时间。

## 小结

- `blkg` 把控制组与设备队列关联，bio 携带归属穿过异步回写和设备堆叠。
- `io.max` 控制绝对带宽 / IOPS，WBT 通过设备级背压保护时延。
- IOCost 估算设备时间成本并按权重分配，不等于简单均分字节。
- BFQ 的 `io.bfq.weight` 与 IOCost 的 `io.weight` 是不同接口。
- `io.stat` 与磁盘完成统计口径不同；配置启用也不等于策略运行时生效。

## 参考

- [`Documentation/admin-guide/cgroup-v2.rst`](../../linux/Documentation/admin-guide/cgroup-v2.rst)
- [`Documentation/block/bfq-iosched.rst`](../../linux/Documentation/block/bfq-iosched.rst)
- [`block/blk-rq-qos.c`](../../linux/block/blk-rq-qos.c)
