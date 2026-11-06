# 10.5 blk-mq：上下文、队列映射与标签

> **本节源码**：[`include/linux/blk-mq.h`](../../linux/include/linux/blk-mq.h)、[`block/blk-mq.h`](../../linux/block/blk-mq.h)、[`block/blk-mq.c`](../../linux/block/blk-mq.c)、[`block/blk-mq-tag.c`](../../linux/block/blk-mq-tag.c)
> **关键函数**：`blk_mq_alloc_tag_set()`、`__blk_mq_alloc_requests()`、`blk_mq_get_tag()`、`blk_mq_rq_ctx_init()`、`blk_mq_start_request()`
> **前置知识**：10.2 设备模型、10.3 bio、per-CPU 数据与位图

## 概述

高速设备能并行处理大量请求，块层若仍依靠单一共享队列，就容易把竞争集中到 CPU 之间。blk-mq 通过 per-CPU 提交上下文与面向驱动的硬件上下文分摊组织成本，再用标签限制并发并快速定位完成请求。标签不仅是编号，也是对有限请求资源的占用凭证。启用 I/O 调度器后，排队资源与设备执行资源还需要分开管理。

## 核心数据结构

### 1. 软件上下文与硬件上下文

`blk_mq_ctx` 是每个队列的 per-CPU 对象，不是每进程一份。

[`block/blk-mq.h:19-32`](../../linux/block/blk-mq.h#L19-L32)（v6.18.60）

```c
struct blk_mq_ctx {
	struct {
		spinlock_t		lock;
		struct list_head	rq_lists[HCTX_MAX_TYPES];
	} ____cacheline_aligned_in_smp;

	unsigned int		cpu;
	unsigned short		index_hw[HCTX_MAX_TYPES];
	struct blk_mq_hw_ctx 	*hctxs[HCTX_MAX_TYPES];
	...
```

`blk_mq_hw_ctx` 简称 `hctx`。其中 `dispatch` 保存已经准备派发却暂时受阻的请求，`ctxs` / `ctx_map` 组织关联的软件上下文，`driver_data` 联系驱动队列，`tags` / `sched_tags` 分别指向驱动与调度器标签集合，见 [`include/linux/blk-mq.h:314-440`](../../linux/include/linux/blk-mq.h#L314-L440)。`hctx` 本身仍是软件结构，不是设备直接消费的 DMA 环。

### 2. 标签集合与请求对象

[`include/linux/blk-mq.h:772-790`](../../linux/include/linux/blk-mq.h#L772-L790)（v6.18.60）

```c
struct blk_mq_tags {
	unsigned int nr_tags;
	unsigned int nr_reserved_tags;
	unsigned int active_queues;

	struct sbitmap_queue bitmap_tags;
	struct sbitmap_queue breserved_tags;

	struct request **rqs;
	struct request **static_rqs;
	...
```

`bitmap_tags` 管普通标签，`breserved_tags` 管保留标签（Reserved Tag）；后者不是无限应急容量，只能由显式申请保留资源的路径使用。`static_rqs` 指向预分配请求对象，`rqs` 用于驱动标签到当前请求的关联。

`blk_mq_tag_set` 则描述整个驱动的队列资源：`nr_hw_queues`、`queue_depth`、`reserved_tags`、`cmd_size`、`map[]` 和 `ops`，见 [`include/linux/blk-mq.h:495-557`](../../linux/include/linux/blk-mq.h#L495-L557)。一个 tag set 可以被多个 `request_queue` 共享，不能假定“一块磁盘恰好一个独占 tag set”。

## 主流程分析

```text
blk_mq_submit_bio()                       block/blk-mq.c
└── blk_mq_get_new_requests()              block/blk-mq.c
    ├── rq_qos_throttle()                 block/blk-rq-qos.h
    └── __blk_mq_alloc_requests()          block/blk-mq.c
        ├── blk_mq_get_ctx()              block/blk-mq.h
        ├── blk_mq_map_queue()            block/blk-mq.h
        ├── blk_mq_limit_depth()          block/blk-mq.c
        ├── blk_mq_get_tag()              block/blk-mq-tag.c
        ├── blk_mq_rq_ctx_init()          block/blk-mq.c
        └── blk_mq_rq_late_init()         block/blk-mq.c
```

### 1. 由 CPU 与操作类型选择 hctx

队列映射（Queue Mapping）通过 `blk_mq_queue_map.mq_map[cpu]` 把 CPU 关联到硬件队列下标。类型包括默认、读与轮询，定义见 [`include/linux/blk-mq.h:464-492`](../../linux/include/linux/blk-mq.h#L464-L492)。驱动可以共享类型映射，也可以提供独立队列组，不保证每类都有独立物理队列。

常用路径通过 `ctx->hctxs[type]` 取得目标 `hctx`，见 [`block/blk-mq.h:90-113`](../../linux/block/blk-mq.h#L90-L113)。取得 `ctx` 后任务可能迁移，内核依靠上下文对象长期存在保证安全，并不把“保存的提交 CPU”当成之后每条指令的当前 CPU，见 [`block/blk-mq.h:148-158`](../../linux/block/blk-mq.h#L148-L158)。

### 2. 无调度器：请求分配时取得驱动标签

没有调度器时，分配使用 `hctx->tags`；取得标签后，直接从 `tags->static_rqs[tag]` 取出预分配对象，而不是每个 I/O 都临时 `kmalloc()` 一个 `request`，见 [`block/blk-mq.c:410-451`](../../linux/block/blk-mq.c#L410-L451)。

驱动私有区的大小由 `cmd_size` 预先指定，和请求对象共同分配。驱动可通过 `blk_mq_rq_to_pdu()` 获取该空间，不必为每条命令再分配一份基本上下文；分配布局见 [`block/blk-mq.c:3609-3682`](../../linux/block/blk-mq.c#L3609-L3682)。

### 3. 有调度器：先内部标签，后驱动标签

启用调度器时，分配路径设置 `RQF_SCHED_TAGS`，使用 `hctx->sched_tags`，见 [`block/blk-mq.c:504-531`](../../linux/block/blk-mq.c#L504-L531)。初始化区分两种编号：

[`block/blk-mq.c:427-433`](../../linux/block/blk-mq.c#L427-L433)（v6.18.60）

```c
	if (data->rq_flags & RQF_SCHED_TAGS) {
		rq->tag = BLK_MQ_NO_TAG;
		rq->internal_tag = tag;
	} else {
		rq->tag = tag;
		rq->internal_tag = BLK_MQ_NO_TAG;
	}
```

内部标签（Internal Tag）让请求可以先在调度器等待；驱动标签（Driver Tag）在派发阶段取得，才占用设备执行侧资源。驱动标签在结束时与内部标签分别归还，随后重启有需要的队列，见 [`block/blk-mq.c:809-845`](../../linux/block/blk-mq.c#L809-L845)。调度器取出请求后若无法取得驱动标签，会停止继续向下取，避免制造“设备仍有容量”的错误反馈，见 [`block/blk-mq-sched.c:140-149`](../../linux/block/blk-mq-sched.c#L140-L149)。

`RQF_SCHED_TAGS` 与 `RQF_USE_SCHED` 也不是同一个标志。flush 或透传命令可以来自调度器标签池，却不走普通调度策略，见 [`block/blk-mq.c:493-550`](../../linux/block/blk-mq.c#L493-L550)。

### 4. 标签不足时，等待本身构成背压

普通分配失败后，`blk_mq_get_tag()` 先尝试推动队列，再注册等待并重试，必要时进入 `io_schedule()`；醒来后重新取得 CPU / `hctx` 映射，见 [`block/blk-mq-tag.c:137-229`](../../linux/block/blk-mq-tag.c#L137-L229)。`BLK_MQ_REQ_NOWAIT` 则在无可用标签时直接失败。

这意味着“设备没有更多 issue 事件”不一定是调度器没有派发；提交线程可能尚在等待请求资源。只测 `issue → complete` 会漏掉这段等待。

## 关键细节 / 设计考量

- `nr_requests`、`queue_depth`、保留标签数、应用 `iodepth` 与控制器深度有不同作用域，不能当成同一数值。
- 标签通常只在其标签集合内唯一；跨磁盘、跨 `hctx` 追踪时至少应带上队列上下文。
- 共享标签集合还需要活跃队列间的资源协调，不能简单用“每个 hctx 标签数 × hctx 数”计算全设备可用深度。
- 批量分配和 plug 的 `cached_rqs` 可以复用已占用的请求；探针看到分配对象与实际 `bio` 绑定的时刻可能不同，见 [`block/blk-mq.c:453-485`](../../linux/block/blk-mq.c#L453-L485)。

## 可观测性

| 入口 | 内容与解释 | 源码 / 配置 |
|---|---|---|
| `/sys/block/<disk>/mq/<n>/cpu_list` | 此 hctx 关联的 CPU 集合，不是当前正在执行的 CPU | [`block/blk-mq-sysfs.c:82-115`](../../linux/block/blk-mq-sysfs.c#L82-L115)，`CONFIG_SYSFS=y` |
| 同目录 `nr_tags`、`nr_reserved_tags` | 驱动标签容量与保留数量，不是实时剩余量 | [`block/blk-mq-sysfs.c:70-110`](../../linux/block/blk-mq-sysfs.c#L70-L110) |
| `/sys/kernel/debug/block/<disk>/hctx<n>/tags`、`sched_tags` | 两种标签资源及位图状态 | [`block/blk-mq-debugfs.c:588-601`](../../linux/block/blk-mq-debugfs.c#L588-L601)，`CONFIG_BLK_DEBUG_FS=y`、`CONFIG_DEBUG_FS=y` |
| `blk_mq_get_tag()` 动态探针 | 观察入口到返回的时间，区分分配等待与设备耗时 | [`block/blk-mq-tag.c:137-229`](../../linux/block/blk-mq-tag.c#L137-L229)，`CONFIG_KPROBES=y`，须检查运行时可挂载性 |

`blk_mq_map_queue()`、`blk_mq_get_ctx()` 是内联函数，不应默认它们存在可挂载的独立符号。

## 小结

- `ctx` 面向提交 CPU，`hctx` 面向驱动队列，但都属于块层软件对象。
- 映射可按默认、读、轮询类型区分，也可共享队列。
- 无调度器时分配驱动标签；有调度器时先分配内部标签，派发再拿驱动标签。
- 标签限制并发、关联预分配请求，并为完成路径提供快速索引。
- 资源等待可能发生在请求入队事件之前，必须纳入性能分析。

## 参考

- [`Documentation/block/blk-mq.rst`](../../linux/Documentation/block/blk-mq.rst)
- [`block/blk-mq-cpumap.c`](../../linux/block/blk-mq-cpumap.c)
