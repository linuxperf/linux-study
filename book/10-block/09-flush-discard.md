# 10.9 持久化与空间操作：flush、FUA、discard 与分区存储

> **本节源码**：[`block/blk-flush.c`](../../linux/block/blk-flush.c)、[`block/blk-lib.c`](../../linux/block/blk-lib.c)、[`block/blk-zoned.c`](../../linux/block/blk-zoned.c)、[`include/uapi/linux/blkzoned.h`](../../linux/include/uapi/linux/blkzoned.h)
> **关键函数**：`blk_insert_flush()`、`blk_flush_complete_seq()`、`blkdev_issue_flush()`、`blkdev_issue_discard()`、`blkdev_issue_zeroout()`、`blk_zone_plug_bio()`
> **前置知识**：10.3 bio、10.6 派发、10.8 请求完成

## 概述

设备报告写完成时，数据可能还在易失缓存中；空间已经不再使用，也不等于读回一定为零。块层因此需要明确表达持久化、丢弃、清零和顺序写入等不同语义，而不能把它们都当成普通写。通用 flush 状态机把上层持久化要求翻译为设备可执行的序列。分区存储设备又引入 zone 写指针约束，需要与普通批处理机制区别对待。

## 核心数据结构

### 1. 操作与标志

- `REQ_PREFLUSH`：数据操作之前，先使先前已完成写入的易失缓存内容持久化。
- 强制单元访问（Force Unit Access，FUA），对应 `REQ_FUA`：本次数据操作只有达到非易失存储后才能向上完成。
- `REQ_OP_DISCARD`、`REQ_OP_WRITE_ZEROES`：分别表达范围不再需要和范围清零，语义不同。
- `REQ_OP_ZONE_APPEND`、`REQ_OP_ZONE_RESET` 等：表达 zone 专用操作。

操作枚举见 [`include/linux/blk_types.h:340-374`](../../linux/include/linux/blk_types.h#L340-L374)，flush / FUA 契约见 [`Documentation/block/writeback_cache_control.rst`](../../linux/Documentation/block/writeback_cache_control.rst)。

### 2. flush 序列

`blk_flush_queue` 使用两个待 flush 队列，定义见 [`block/blk.h:34-44`](../../linux/block/blk.h#L34-L44)。`request.flush.seq` 标记已经完成的阶段，并保存原请求完成方法。阶段定义为 PREFLUSH、DATA、POSTFLUSH、DONE，见 [`block/blk-flush.c:79-94`](../../linux/block/blk-flush.c#L79-L94)。`RQF_FLUSH_SEQ` 使普通数据完成暂不结束上层 bio。

### 3. zone 描述符

分区存储设备（Zoned Block Device，ZBD）中的 zone 不是磁盘分区表中的 partition。`blk_zone` 的 `start`、`len`、`capacity`、`wp` 都用 512 字节扇区表示；`wp` 是写指针（Write Pointer），`type` 和 `cond` 表示类型与状态，见 [`include/uapi/linux/blkzoned.h:86-118`](../../linux/include/uapi/linux/blkzoned.h#L86-L118)。

## 主流程分析

```text
持久化请求
blk_mq_submit_bio()                       block/blk-mq.c
└── blk_insert_flush()                    block/blk-flush.c
    ├── 计算 PREFLUSH / DATA / POSTFLUSH 策略
    └── blk_flush_complete_seq()          block/blk-flush.c
        ├── flush 阶段：加入 pending 队列，触发 blk_kick_flush()
        ├── data 阶段：进入重提交队列
        └── done 阶段：恢复请求，最终完成
纯 flush 助手
blkdev_issue_flush()                      block/blk-flush.c
└── REQ_OP_WRITE | REQ_PREFLUSH 的空 bio → submit_bio_wait()
```

### 1. PREFLUSH 与 FUA 各自解决一个方向的问题

PREFLUSH 约束的是本次数据操作之前的缓存状态；FUA 约束的是本次写操作何时可以完成。只有 PREFLUSH 的数据写，并不因此保证自己的数据在该写完成时已经持久化；只有 FUA，也不等同于先把所有旧写缓存刷完。两者可组合使用，契约与能力映射见 [`block/blk-flush.c:8-26`](../../linux/block/blk-flush.c#L8-L26)。

普通写的完成、`REQ_SYNC` 提示以及文件系统的 `fsync()` 语义也不能混用。块层只执行上层已经编码进 bio 的设备级要求；文件系统还要负责数据与元数据、日志和恢复协议。

### 2. 按硬件能力构造序列

对带数据的 `REQ_PREFLUSH | REQ_FUA` 写，可按下表理解 `blk_insert_flush()` 的决策：

| 设备声明 | 块层执行策略 |
|---|---|
| 无易失写缓存 | 普通 DATA，不需要额外 flush |
| 有写缓存、支持原生 FUA | PREFLUSH → 带 FUA 的 DATA |
| 有写缓存、不支持原生 FUA | PREFLUSH → DATA → POSTFLUSH |

实际策略还取决于请求是否有数据、标志是否齐全，代码见 [`block/blk-flush.c:382-461`](../../linux/block/blk-flush.c#L382-L461)。原生 FUA 省掉的是用来模拟本次持久化的后置 flush，不是无条件省掉前置 flush。

纯 flush 在 bio 层仍采用 WRITE 操作加 PREFLUSH 标志：

[`block/blk-flush.c:468-474`](../../linux/block/blk-flush.c#L468-L474)（v6.18.60）

```c
int blkdev_issue_flush(struct block_device *bdev)
{
	struct bio bio;

	bio_init(&bio, bdev, NULL, 0, REQ_OP_WRITE | REQ_PREFLUSH);
	return submit_bio_wait(&bio);
}
```

`REQ_OP_FLUSH` 由请求级状态机合成，不能直接把它当成通用 bio 操作提交，见 [`block/blk-core.c:833-839`](../../linux/block/blk-core.c#L833-L839)。

### 3. 数据完成不一定是整条序列完成

多个请求可以共享一次 flush。状态机以双缓冲队列组织等待项，控制 flush 执行与推进下一阶段，见 [`block/blk-flush.c:28-64`](../../linux/block/blk-flush.c#L28-L64)。

数据阶段走到 `blk_update_request()` 时，如果带 `RQF_FLUSH_SEQ`，只推进数据范围，不调用最终 `bio_endio()`，见 [`block/blk-mq.c:1026-1033`](../../linux/block/blk-mq.c#L1026-L1033)。直到必要的后置 flush 也结束，才恢复原请求并向上通知。过早结束 bio 会把“数据送达缓存”错误地报告成“持久化完成”。

### 4. discard 不是写零，更不是安全擦除

丢弃（Discard）告诉设备某范围的数据不再需要。构造的 bio 可以没有数据向量，只带设备范围与操作类型，见 [`block/blk-lib.c:38-61`](../../linux/block/blk-lib.c#L38-L61)。设备是否回收物理空间、后续读回什么，不能仅由通用 discard 请求推导；安全擦除还有独立 `REQ_OP_SECURE_ERASE`。

清零（Write Zeroes）保证的是相应零填充语义。`blkdev_issue_zeroout()` 优先尝试硬件清零，不支持时可回退到显式写零页，见 [`block/blk-lib.c:297-328`](../../linux/block/blk-lib.c#L297-L328)。限制是可分别配置的，不应拿 `discard_max_bytes` 代替 `write_zeroes_max_bytes`。

另一个接口细节是，当前 `blkdev_issue_discard()` 会把同步提交得到的 `-EOPNOTSUPP` 转为成功，见 [`block/blk-lib.c:85-104`](../../linux/block/blk-lib.c#L85-L104)。因此，助手返回成功不能证明硬件实际执行了介质擦除。

### 5. zone 写入需要独立的串行化协议

顺序写 zone 具有写指针与容量边界。普通写必须遵守位置约束；zone append 让设备选择实际写入位置，完成时返回该位置。zone append 不允许普通拆分，否则不同片段的落点可能不连续，检查见 [`block/blk-core.c:594-624`](../../linux/block/blk-core.c#L594-L624) 与 [`block/bio.c:1702-1708`](../../linux/block/bio.c#L1702-L1708)。

当前块层使用 zone 写入暂存（Zone Write Plugging）处理需要串行化的写、清零和模拟 append。它是按 zone 协调的机制，不是 `current->plug` 的任务批处理，见 [`block/blk-zoned.c:1163-1218`](../../linux/block/blk-zoned.c#L1163-L1218)。请求结束后推动同一 zone 的后续 bio；reset、finish 等操作仍要求使用者正确协调在途写入，块层不替应用消除全部语义冲突。

## 关键细节 / 设计考量

- 持久化能力依赖设备正确报告写缓存与 FUA 支持，虚拟化后端也必须正确实现协议。
- 不应为了性能实验在生产设备上谎报“没有易失缓存”；这可能直接破坏掉电一致性。
- flush 共享与多个阶段会改变请求事件数量，不能一条上层同步操作对应一条 `block_rq_issue`。
- zone 管理、discard、清零都可能改变存储内容或可用范围，不能作为无副作用的诊断命令。

## 可观测性

sysfs 依赖本配置的 `CONFIG_SYSFS=y`，zone 依赖 `CONFIG_BLK_DEV_ZONED=y`，事件依赖 `CONFIG_TRACING=y`。

| 入口 | 观察内容 | 实现 |
|---|---|---|
| `queue/write_cache`、`queue/fua` | 设备声明的缓存与 FUA 能力；不是实时缓存占用 | [`block/blk-sysfs.c:588-589`](../../linux/block/blk-sysfs.c#L588-L589) |
| `queue/discard_max_bytes`、`discard_granularity`、`write_zeroes_max_bytes` | 范围操作的限制，单位按属性名解释 | [`block/blk-sysfs.c:557-575`](../../linux/block/blk-sysfs.c#L557-L575) |
| `queue/zoned`、`nr_zones`、`max_open_zones`、`max_active_zones` | zone 模型与资源限制 | [`block/blk-sysfs.c:578-581`](../../linux/block/blk-sysfs.c#L578-L581) |
| `BLKREPORTZONE` ioctl | zone 的写指针、容量与状态，只读报告 | [`block/blk-zoned.c:277-304`](../../linux/block/blk-zoned.c#L277-L304) |
| `block:block_rq_issue`、`block:block_rq_complete` | flush / 数据阶段与操作标志 | [`include/trace/events/block.h:118-258`](../../linux/include/trace/events/block.h#L118-L258) |
| `block:blkdev_zone_mgmt` | zone 管理操作类型与范围；该事件不带最终完成状态 | [`include/trace/events/block.h:611-635`](../../linux/include/trace/events/block.h#L611-L635) |

整盘 `stat` 的 flush 计数来自实际 flush 请求的专门记账，不等于上层 PREFLUSH / FUA bio 数，见 [`block/blk-flush.c:128-138`](../../linux/block/blk-flush.c#L128-L138)。

## 小结

- PREFLUSH 约束先前写缓存，FUA 约束本次数据完成时的持久化。
- 块层按设备能力选择前置 flush、数据和后置 flush，最终完成须等待完整序列。
- discard、清零、安全擦除是不同操作，不能互相替代。
- zone 的地址和容量仍用 512 字节扇区表示，zone 不等于磁盘 partition。
- zone 写入暂存与任务 plug 不同，用于维护顺序写入协议。

## 参考

- [`Documentation/block/writeback_cache_control.rst`](../../linux/Documentation/block/writeback_cache_control.rst)
- [`include/uapi/linux/blkzoned.h`](../../linux/include/uapi/linux/blkzoned.h)
- [`Documentation/ABI/stable/sysfs-block`](../../linux/Documentation/ABI/stable/sysfs-block)
