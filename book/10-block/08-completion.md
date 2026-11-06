# 10.8 请求完成、超时与轮询

> **本节源码**：[`block/blk-mq.c`](../../linux/block/blk-mq.c)、[`block/blk-timeout.c`](../../linux/block/blk-timeout.c)、[`block/blk-core.c`](../../linux/block/blk-core.c)、[`block/bio.c`](../../linux/block/bio.c)
> **关键函数**：`blk_mq_start_request()`、`blk_mq_complete_request()`、`blk_update_request()`、`blk_mq_end_request()`、`blk_mq_timeout_work()`、`bio_poll()`
> **前置知识**：10.3 完成链、10.5 标签、5.9 软中断、8.7 时间轮定时器

## 概述

驱动收到设备完成消息后，仍要把协议状态转换为块层结果、更新请求范围、通知各个 bio 并归还资源。完成可能在中断 CPU 直接处理，也可能转移到提交 CPU 或软中断。超时路径与正常完成并发，必须避免请求对象被复用后仍处理旧命令。轮询则改变完成发现方式，不改变完成协议本身。

## 核心数据结构

`request` 的状态包括 `MQ_RQ_IDLE`、`MQ_RQ_IN_FLIGHT`、`MQ_RQ_COMPLETE`，定义见 [`include/linux/blk-mq.h:91-95`](../../linux/include/linux/blk-mq.h#L91-L95)。`deadline` 是 jiffies 时间轴上的超时点，`timeout` 是请求超时长度；`start_time_ns`、`io_start_time_ns` 等用于时延统计，定义见 [`include/linux/blk-mq.h:114-171`](../../linux/include/linux/blk-mq.h#L114-L171)。

`request_queue` 持有共享的 `timeout` 定时器与 `timeout_work`；并不是每个请求各自嵌入一个 `timer_list`。请求只保存自己的截止值，见 [`include/linux/blkdev.h:551-552`](../../linux/include/linux/blkdev.h#L551-L552) 与 [`block/blk-timeout.c:128-167`](../../linux/block/blk-timeout.c#L128-L167)。

## 主流程分析

```text
驱动接收请求
└── blk_mq_start_request()                block/blk-mq.c
    ├── block_rq_issue 事件
    ├── blk_add_timer()                   block/blk-timeout.c
    └── 标记 IN_FLIGHT，建立 tag → request
设备完成（异步触发）
└── 驱动取出完成项
    └── blk_mq_complete_request()          block/blk-mq.c（常用路径）
        ├── 直接调用 mq_ops->complete()
        └── IPI / BLOCK_SOFTIRQ 后调用 mq_ops->complete()
            └── 驱动清理 / 重试决策
                └── blk_mq_end_request()  block/blk-mq.c
                    ├── blk_update_request()
                    │   └── bio_endio()   block/bio.c
                    └── __blk_mq_end_request()
                        └── 请求收尾与资源归还
```

驱动也可以使用批量完成、部分完成或自身优化路径；这棵树不是所有驱动必须逐个调用的固定模板。

### 1. start_request() 建立执行边界

`blk_mq_start_request()` 发出 issue 事件，按需记录 I/O 开始时间和 QoS 信息，设置超时及在途状态，并把请求放入驱动标签索引，见 [`block/blk-mq.c:1381-1407`](../../linux/block/blk-mq.c#L1381-L1407)。

issue 是“驱动声明开始处理”的软件边界，不是硬件介质刚开始读写的时间。驱动可能先调用它，再把命令放入描述符环或写 doorbell。因此，`issue → complete` 包括驱动、传输、设备排队和执行等部分，而不是纯介质服务时间。

### 2. complete_request() 负责完成路由，不等于最终释放

[`block/blk-mq.c:1366-1371`](../../linux/block/blk-mq.c#L1366-L1371)（v6.18.60）

```c
void blk_mq_complete_request(struct request *rq)
{
	if (!blk_mq_complete_request_remote(rq))
		rq->q->mq_ops->complete(rq);
}
EXPORT_SYMBOL(blk_mq_complete_request);
```

`blk_mq_complete_request_remote()` 先标记完成状态，再根据轮询、CPU 关系和队列属性选择就地处理、IPI 或本地 `BLOCK_SOFTIRQ`。轮询请求通常就地完成；是否转移还受共享缓存、CPU 算力和强制中断线程化影响，见 [`block/blk-mq.c:1285-1355`](../../linux/block/blk-mq.c#L1285-L1355)。本配置 `CONFIG_SMP=y`，但不能据此断言每次完成都发送 IPI。

随后调用的是驱动注册的 `complete` 方法，它可能处理重试或故障转移，而不是立刻释放请求。NVMe 的例子见 [10.11](11-drivers.md)。

### 3. 字节完成与请求生命周期结束分开

`blk_update_request()` 按本次完成的 `nr_bytes` 遍历 bio 链，设置错误、推进迭代器，并对已经完全消耗的 bio 调用 `bio_endio()`；返回值表示请求是否仍有剩余数据，见 [`block/blk-mq.c:967-1080`](../../linux/block/blk-mq.c#L967-L1080)。

`blk_mq_end_request()` 是结束整条请求的接口：

[`block/blk-mq.c:1189-1195`](../../linux/block/blk-mq.c#L1189-L1195)（v6.18.60）

```c
void blk_mq_end_request(struct request *rq, blk_status_t error)
{
	if (blk_update_request(rq, error, blk_rq_bytes(rq)))
		BUG();
	__blk_mq_end_request(rq, error);
}
EXPORT_SYMBOL(blk_mq_end_request);
```

最终收尾处理统计、调度器反馈、请求完成回调及标签资源。若存在 `rq->end_io`，其返回值决定是否由块层释放请求，见 [`block/blk-mq.c:1163-1187`](../../linux/block/blk-mq.c#L1163-L1187)。所以 `bio_endio()`、请求 `end_io()` 和请求对象资源归还也不是同一时刻。

### 4. 超时由共享定时器触发工作扫描

`blk_add_timer()` 计算请求 `deadline`，并按需提前队列共享定时器，见 [`block/blk-timeout.c:128-167`](../../linux/block/blk-timeout.c#L128-L167)。定时器回调把 `timeout_work` 交给块层工作队列，见 [`block/blk-core.c:381-386`](../../linux/block/blk-core.c#L381-L386)。

`blk_mq_timeout_work()` 先扫描是否存在超时的在途请求，再等待相应派发临界区退出，随后调用驱动 `timeout()`。`RQF_TIMED_OUT` 避免同一轮重复处理，驱动返回 `BLK_EH_DONE` 表示已经完成或将负责完成，返回 `BLK_EH_RESET_TIMER` 则继续等待，见 [`block/blk-mq.c:1669-1805`](../../linux/block/blk-mq.c#L1669-L1805)。超时不是块层统一立即返回 `-ETIMEDOUT` 的简单分支。

该工作不能像新提交一样阻塞在冻结入口，否则“冻结等待请求退出、超时处理等待冻结解除”会互锁。因此它用 `percpu_ref_tryget()` 保持必要使用引用，源码解释见 [`block/blk-mq.c:1758-1774`](../../linux/block/blk-mq.c#L1758-L1774)。

### 5. 轮询改变发现完成的方式

轮询（Polling）请求带 `REQ_POLLED`。`blk_mq_start_request()` 把 hctx 队列号写到 `bi_cookie`；`bio_poll()` 先冲刷当前 plug，再按 cookie 进入 `blk_mq_poll()` 和驱动 poll 钩子，见 [`block/blk-mq.c:1404-1405`](../../linux/block/blk-mq.c#L1404-L1405)、[`block/blk-core.c:934-974`](../../linux/block/blk-core.c#L934-L974)、[`block/blk-mq.c:5236-5242`](../../linux/block/blk-mq.c#L5236-L5242)。

轮询仍要处理状态、错误和生命周期，也需要设备及驱动支持。普通读写需要拆分时，块层还可能清除轮询标志，见 [`block/blk-merge.c:403-409`](../../linux/block/blk-merge.c#L403-L409)。不能把用户态设置一次轮询选项等同于每个底层请求都走轮询。

## 关键细节 / 设计考量

- 完成回调可能在中断或软中断上下文运行，不能假设可以睡眠，也不能假设是原提交线程。
- 标签会复用，动态观测必须在结束时删除关联；仅用标签数值跨队列匹配会关联错请求。
- 超时可能触发控制器恢复或重试，诊断时要同时看队列重排和驱动日志。
- flush 序列的数据阶段会消耗 bio 数据，但暂不通知上层完成；最终语义见 [10.9](09-flush-discard.md)。

## 可观测性

| 入口 | 解释与注意事项 | 源码 / 配置 |
|---|---|---|
| `block:block_rq_issue` | 驱动开始处理边界 | [`block/blk-mq.c:1385`](../../linux/block/blk-mq.c#L1385)，`CONFIG_TRACING=y` |
| `block:block_rq_complete` | 本次完成部分的扇区数及 errno；可以部分完成 | [`include/trace/events/block.h:118-173`](../../linux/include/trace/events/block.h#L118-L173) |
| `block:block_rq_error` | 错误完成部分，不能用事件条数代替失败系统调用数 | [`include/trace/events/block.h:175-189`](../../linux/include/trace/events/block.h#L175-L189) |
| `/sys/block/<disk>/queue/rq_affinity` | 完成 CPU 路由策略 | [`block/blk-sysfs.c:401-435`](../../linux/block/blk-sysfs.c#L401-L435)，`CONFIG_SYSFS=y` |
| 同目录 `io_timeout`、`io_poll` | 队列超时与轮询相关属性；支持性仍取决于驱动 | [`block/blk-sysfs.c:585-591`](../../linux/block/blk-sysfs.c#L585-L591) |
| `blk_mq_start_request()`、`blk_mq_end_request()`、驱动 timeout 方法探针 | 以请求指针关联执行、结束和恢复 | 依赖 `CONFIG_KPROBES=y` 等运行条件，须确认实际符号 |

普通 tracefs 的 `block_rq_complete` 字段不导出请求指针，不能只凭相同扇区唯一匹配，详见 [10.12](12-observability.md)。

## 小结

- `start_request()` 建立执行统计、超时和标签关联，不代表介质开始动作。
- `complete_request()` 选择完成路由，驱动完成回调才决定结束或重试。
- `blk_update_request()` 推进字节完成，`blk_mq_end_request()` 结束整条请求。
- 超时共享队列定时器、扫描标签，并把恢复决策交给驱动。
- 轮询不绕过完成协议，仍受能力、生命周期和拆分约束。

## 参考

- [`Documentation/block/blk-mq.rst`](../../linux/Documentation/block/blk-mq.rst)
- [`include/linux/blk-mq.h:304-314`](../../linux/include/linux/blk-mq.h#L304-L314)
