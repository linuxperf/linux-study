# 10.6 plug、请求合并与派发

> **本节源码**：[`block/blk-core.c`](../../linux/block/blk-core.c)、[`block/blk-merge.c`](../../linux/block/blk-merge.c)、[`block/blk-mq.c`](../../linux/block/blk-mq.c)、[`block/blk-mq-sched.c`](../../linux/block/blk-mq-sched.c)
> **关键函数**：`blk_start_plug()`、`blk_finish_plug()`、`blk_attempt_plug_merge()`、`blk_mq_flush_plug_list()`、`blk_mq_sched_dispatch_requests()`、`blk_mq_dispatch_rq_list()`
> **前置知识**：10.3 bio、10.5 blk-mq、5.12 工作队列

## 概述

尽快把每个小 I/O 送入设备不一定最有效：相邻访问可以合成更大请求，多条命令也可以共用一次设备通知。plug 提供短期的任务本地批处理窗口，合并减少请求数量，派发机制则把可执行请求交给驱动并处理资源不足。这三者相互配合，但不是同一个概念。尤其是 plug 不等于调度器，`none` 也不意味着完全没有批处理或排队。

## 核心数据结构

- `current->plug` 指向任务当前批次。批次的 `mq_list` 存待提交请求，`cached_rqs` 存预分配缓存请求；初始化见 [`block/blk-core.c:1121-1146`](../../linux/block/blk-core.c#L1121-L1146)。
- `request->bio` / `biotail` 组织同一请求内的 bio 链，`__sector` / `__data_len` 记录设备范围，定义见 [`include/linux/blk-mq.h:103-131`](../../linux/include/linux/blk-mq.h#L103-L131)。
- `hctx->dispatch` 存已脱离普通调度选择、但未能被驱动接收的请求。它不同于 `ctx->rq_lists[]`，也不同于调度器自己的排序结构，见 [`include/linux/blk-mq.h:314-340`](../../linux/include/linux/blk-mq.h#L314-L340)。

## 主流程分析

以下先看选择分支，再看资源不足如何退回等待；“重跑队列”不是重新创建一个 bio。

[`block/blk-mq.c:3035-3056`](../../linux/block/blk-mq.c#L3035-L3056)、[`block/blk-mq.c:3238-3253`](../../linux/block/blk-mq.c#L3238-L3253)、[`block/blk-mq-sched.c:266-315`](../../linux/block/blk-mq-sched.c#L266-L315)（v6.18.60）

```text
blk_mq_submit_bio()                       block/blk-mq.c
├── blk_mq_attempt_bio_merge()             block/blk-mq.c
│   ├── blk_attempt_plug_merge()          block/blk-merge.c
│   └── blk_mq_sched_bio_merge()          block/blk-mq-sched.c
└── 若未合并，准备 request
    ├── blk_add_rq_to_plug()              block/blk-mq.c
    │   └── 稍后 blk_mq_flush_plug_list()
    ├── blk_mq_insert_request()           block/blk-mq.c
    │   └── blk_mq_run_hw_queue()
    │       └── blk_mq_sched_dispatch_requests()  block/blk-mq-sched.c
    │           └── blk_mq_dispatch_rq_list()    block/blk-mq.c
    └── blk_mq_try_issue_directly()        block/blk-mq.c
        └── 驱动 queue_rq()
```

### 1. plug 是有界、可提前冲刷的批次

`blk_start_plug()` 把批次挂到当前任务；嵌套调用不会替换已经存在的批次。`blk_finish_plug()` 只结束自己安装的批次，见 [`block/blk-core.c:1121-1174`](../../linux/block/blk-core.c#L1121-L1174) 与 [`block/blk-core.c:1246-1253`](../../linux/block/blk-core.c#L1246-L1253)。

批次并非必须等到显式结束才提交。请求数达到限制、已有请求足够大，或者任务进入需要冲刷的调度路径，都会推动待提交 I/O，见 [`block/blk-mq.c:1421-1445`](../../linux/block/blk-mq.c#L1421-L1445) 和 [`kernel/sched/core.c:6992-7002`](../../linux/kernel/sched/core.c#L6992-L7002)。任务因内存分配阻塞时若还把回收所需 I/O 留在自己的 plug 中，可能造成前进性问题，因此调度时冲刷也有正确性意义。

`__submit_bio()` 自己也建立局部 plug，见 [`block/blk-core.c:626-650`](../../linux/block/blk-core.c#L626-L650)。所以“没有上层显式 plug 时的直接路径”应理解为不再批量保留请求的逻辑分支；实际普通提交经常先进入局部 plug，再在函数退出时派发。

### 2. 合并先验证属性，再看位置和硬件限制

普通后向合并（Back Merge）要求新 `bio` 紧跟请求末尾；前向合并（Front Merge）要求新 `bio` 正好接到请求前面，判定位于 [`block/blk-merge.c:915-924`](../../linux/block/blk-merge.c#L915-L924)。

相邻不是充分条件。操作类型、blkcg、完整性、加密上下文、写提示、写流和 I/O 优先级等必须兼容，见 [`block/blk-merge.c:889-913`](../../linux/block/blk-merge.c#L889-L913)。随后还要检查合并后的传输长度、段数和边界约束。

后向合并只把新 `bio` 挂到链尾并增加请求长度，不搬动数据：

[`block/blk-merge.c:954-958`](../../linux/block/blk-merge.c#L954-L958)（v6.18.60）

```c
	req->phys_gap_bit = bio_seg_gap(req->q, req->biotail, bio,
					req->phys_gap_bit);
	req->biotail->bi_next = bio;
	req->biotail = bio;
	req->__data_len += bio->bi_iter.bi_size;
```

无调度器时仍可以尝试 plug 或软件上下文链表合并；软件队列的反向扫描有界，不会为了找到一次合并而遍历无限长链表，见 [`block/blk-mq-sched.c:335-368`](../../linux/block/blk-mq-sched.c#L335-L368)。

### 3. 退出 plug 后，也不一定先走软件队列

`blk_mq_flush_plug_list()` 在没有调度器且不是调度触发时，先尝试直接批量派发。支持 `queue_rqs()` 的驱动可一次取得一组请求，其余请求再回到逐请求或排队路径，见 [`block/blk-mq.c:2902-2920`](../../linux/block/blk-mq.c#L2902-L2920) 与 [`block/blk-mq.c:2981-3014`](../../linux/block/blk-mq.c#L2981-L3014)。

有调度器时，请求通常进入调度器的 `insert_requests()`；无调度器但不能直接派发时，进入软件上下文。硬件队列运行时先处理已有 `hctx->dispatch`，再向调度器或软件上下文取新请求，见 [`block/blk-mq-sched.c:266-315`](../../linux/block/blk-mq-sched.c#L266-L315)。优先处理遗留项，也避免过早把大量请求取出调度器而丢失后续合并、排序机会。

### 4. queue_rq() 的返回值决定资源所有权

[`block/blk-mq.c:2157-2173`](../../linux/block/blk-mq.c#L2157-L2173)（v6.18.60）

```c
		bd.rq = rq;
		bd.last = list_empty(list);

		ret = q->mq_ops->queue_rq(hctx, &bd);
		switch (ret) {
		case BLK_STS_OK:
			queued++;
			break;
		case BLK_STS_RESOURCE:
			needs_resource = true;
			fallthrough;
		case BLK_STS_DEV_RESOURCE:
			blk_mq_handle_dev_resource(rq, list);
			goto out;
		default:
			blk_mq_end_request(rq, ret);
		}
```

`BLK_STS_OK` 表示驱动接受请求，不表示设备已经执行完成。`BLK_STS_RESOURCE` 表示更广泛的资源短缺；`BLK_STS_DEV_RESOURCE` 表示设备相关资源暂不可用，驱动需要保证资源恢复后队列能重跑，语义见 [`include/linux/blk_types.h:119-135`](../../linux/include/linux/blk_types.h#L119-L135)。

未能派发的请求进入 `hctx->dispatch`，并根据标签等待、调度重启标志与资源状态安排重跑，见 [`block/blk-mq.c:2182-2255`](../../linux/block/blk-mq.c#L2182-L2255)。这就是背压（Backpressure）：下游不能接收时保留工作，而不是无限分配或把暂时短缺当成永久 I/O 错误。

### 5. 区分“未被接收”与“接收后的重试”

| 边界 | 谁负责下一步 | 不能做的推断 |
|---|---|---|
| `queue_rq()` 返回 `BLK_STS_OK` | 驱动按完成 / 重排协议接续请求 | 不等于已完成，调用者也不能假设请求仍可任意访问 |
| 返回 `BLK_STS_RESOURCE` / `BLK_STS_DEV_RESOURCE` | 驱动清理本次未交接的私有资源，块层保留请求并协调重跑 | 不等于最终 I/O 错误，也不保证此前没有 issue 事件 |
| 驱动接受命令后决定重试 | 驱动与块层重排路径协作 | 不是上层重新提交了一次业务操作 |

例如 virtio-blk 先调用 `blk_mq_start_request()`，再尝试加入 virtqueue；空间不足时仍会解除数据映射、停止相应队列并返回资源不足，见 [`drivers/block/virtio_blk.c:403-453`](../../linux/drivers/block/virtio_blk.c#L403-L453)。所以一次 issue 可能只对应一次未成功发布给设备的尝试。需要把驱动返回值、重排和后续 issue 合起来解释，不能以 issue 次数直接计设备成功接收数。

## 关键细节 / 设计考量

- **插入与 issue 不是同一事件。** 直接派发可以没有普通插入事件；plug 中等待也不等同于调度器等待。
- **批量设备通知需要收尾。** `bd.last` 和 `commit_rqs()` 允许驱动减少 doorbell / kick 次数；批次中途受阻时仍需提交此前累积的命令，见 [`block/blk-mq.c:2177-2180`](../../linux/block/blk-mq.c#L2177-L2180)。
- **资源重试需要避免丢失重启。** 把请求放回派发链与检查重启位之间有内存屏障，不能把这些屏障当成可删的“保守代码”。

## 可观测性

本配置 `CONFIG_TRACEPOINTS=y`、`CONFIG_EVENT_TRACING=y`、`CONFIG_TRACING=y` 下可用的事件：

| 事件 | 对应机制 | 定义 |
|---|---|---|
| `block:block_plug`、`block:block_unplug` | 批次开始与冲刷；unplug 的 `nr_rq` 是本次请求数 | [`include/trace/events/block.h:426-477`](../../linux/include/trace/events/block.h#L426-L477) |
| `block:block_bio_backmerge`、`block:block_bio_frontmerge` | 新 bio 并入已有请求 | [`include/trace/events/block.h:364-384`](../../linux/include/trace/events/block.h#L364-L384) |
| `block:block_rq_insert`、`block:block_rq_issue` | 请求插入与驱动声明开始处理 | [`include/trace/events/block.h:230-258`](../../linux/include/trace/events/block.h#L230-L258) |
| `block:block_rq_requeue` | 请求重新排队，需结合返回状态或驱动事件解释 | [`include/trace/events/block.h:76-116`](../../linux/include/trace/events/block.h#L76-L116) |

`/sys/block/<disk>/queue/nomerges` 控制合并策略，定义见 [`block/blk-sysfs.c:583`](../../linux/block/blk-sysfs.c#L583)（`CONFIG_SYSFS=y`）。debugfs 的 `dispatch` 和 `ctx_map` 可区分遗留派发与软件队列积压，见 [`block/blk-mq-debugfs.c:588-601`](../../linux/block/blk-mq-debugfs.c#L588-L601)（`CONFIG_BLK_DEBUG_FS=y`、`CONFIG_DEBUG_FS=y`）。

## 小结

- plug 提供任务本地批处理窗口，可以因数量、大小或调度提前冲刷。
- 合并要求地址相邻、属性兼容且不突破硬件限制，不复制数据内容。
- 无调度器也有合并、批处理、软件排队和背压。
- 驱动返回成功代表接受请求；资源不足请求必须正确回队并重启，issue 事件也可能早于失败的设备入队尝试。
- `hctx->dispatch` 与调度器队列、软件上下文链表是不同阶段的积压。

## 参考

- [`Documentation/block/blk-mq.rst`](../../linux/Documentation/block/blk-mq.rst)
- [`include/linux/blk-mq.h:574-617`](../../linux/include/linux/blk-mq.h#L574-L617)
