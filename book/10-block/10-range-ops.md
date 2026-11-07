# 10.10 范围操作：discard、清零与安全擦除

> **本节源码**：[`block/blk-lib.c`](../../linux/block/blk-lib.c)、[`block/blk-merge.c`](../../linux/block/blk-merge.c)、[`block/bio.c`](../../linux/block/bio.c)、[`block/blk-core.c`](../../linux/block/blk-core.c)、[`block/blk-sysfs.c`](../../linux/block/blk-sysfs.c)
> **关键函数**：`blkdev_issue_discard()`、`blk_alloc_discard_bio()`、`blkdev_issue_zeroout()`、`blkdev_issue_secure_erase()`、`bio_chain_and_submit()`
> **前置知识**：[10.3 bio 与完成链](03-bio.md)、[10.4 提交检查](04-submit.md)、[10.9 持久化语义](09-flush.md)

## 概述

块设备除了读写数据，还需要接受“这个范围不再使用”“这个范围应读出零”等命令。丢弃（Discard）、清零（Write Zeroes）和安全擦除（Secure Erase）解决的问题不同，不能因为它们都作用于一段扇区就互相替代。块层用无数据向量的 bio 表达可由设备执行的范围操作，也能在不支持硬件清零时回退到普通写。本节沿通用助手分析范围构造、分段、完成汇总和错误处理，不提供修改真实磁盘内容的实验命令。

## 核心数据结构

### 1. 没有数据向量，仍然有设备范围

| 操作 | bio 描述 | 语义边界 |
|---|---|---|
| `REQ_OP_DISCARD` | `bi_sector`、`bi_size`，无需普通读写的数据向量 | 通知设备内容不再需要；不保证读回零或不可恢复 |
| `REQ_OP_WRITE_ZEROES` | 设备范围与可选 `REQ_NOUNMAP` | 由设备执行清零；不保证采用哪种物理写入方式 |
| `REQ_OP_SECURE_ERASE` | 设备范围 | 独立操作，须由设备支持；不能从 discard 成功推断此能力 |
| 回退的 `REQ_OP_WRITE` | 设备范围和零页向量 | 用真实的数据传输实现零填充语义 |

操作定义见 [`include/linux/blk_types.h:340-374`](../../linux/include/linux/blk_types.h#L340-L374)；构造实例见 [`block/blk-lib.c:38-61`](../../linux/block/blk-lib.c#L38-L61)、[`block/blk-lib.c:121-146`](../../linux/block/blk-lib.c#L121-L146)、[`block/blk-lib.c:197-230`](../../linux/block/blk-lib.c#L197-L230)、[`block/blk-lib.c:330-371`](../../linux/block/blk-lib.c#L330-L371)。这里的 `bi_size` 是受操作影响的**字节长度**，不等于“必有同样长度的内存数据待传输”。

### 2. 限制与回退标志各管一件事

`queue_limits` 分别保存 `max_discard_sectors`、`discard_granularity`、`discard_alignment`、`max_write_zeroes_sectors`、`max_secure_erase_sectors`，见 [`include/linux/blkdev.h:369-430`](../../linux/include/linux/blkdev.h#L369-L430)。不同操作不能共用一个最大长度假设。

清零助手的 `BLKDEV_ZERO_NOFALLBACK` 禁止用普通写零页回退；`BLKDEV_ZERO_NOUNMAP` 要求不释放底层逻辑配置空间，硬件清零路径将其转换为 `REQ_NOUNMAP`。契约见 [`block/blk-lib.c:259-272`](../../linux/block/blk-lib.c#L259-L272)，标志转换见 [`block/blk-lib.c:134-140`](../../linux/block/blk-lib.c#L134-L140)。这两个标志都不是持久化标志。

## 主流程分析

[`block/blk-lib.c:38-104`](../../linux/block/blk-lib.c#L38-L104)、[`block/blk-lib.c:121-255`](../../linux/block/blk-lib.c#L121-L255)、[`block/blk-lib.c:311-371`](../../linux/block/blk-lib.c#L311-L371)、[`block/bio.c:359-372`](../../linux/block/bio.c#L359-L372)（v6.18.60）

```text
discard
blkdev_issue_discard()                    block/blk-lib.c
├── __blkdev_issue_discard()
│   ├── blk_alloc_discard_bio()
│   └── bio_chain_and_submit()            block/bio.c
└── submit_bio_wait() 等待最后一个 bio 及其完成链
清零
blkdev_issue_zeroout()                    block/blk-lib.c
├── 检查逻辑块对齐与只读状态
├── blkdev_issue_write_zeroes()           有硬件能力时
│   └── __blkdev_issue_write_zeroes()
└── blkdev_issue_zero_pages()             无能力或返回 EOPNOTSUPP 时
    └── __blkdev_issue_zero_pages()
安全擦除
blkdev_issue_secure_erase()               block/blk-lib.c
├── 检查能力、对齐与只读状态
├── blk_next_bio()                        block/bio.c
└── submit_bio_wait()
```

### 1. discard 构造与设备限制拆分是两层工作

`blk_alloc_discard_bio()` 通过 `bio_alloc(bdev, 0, REQ_OP_DISCARD, ...)` 分配无向量的描述符，再设置扇区和字节长度。它限制单个 bio 可表示的长度，并让后续段尽量按 discard 粒度开始，见 [`block/blk-lib.c:13-61`](../../linux/block/blk-lib.c#L13-L61)。分区的起始偏移也参与粒度计算，不能只按分区内扇区做整除。

助手构造成功不代表已经满足队列的每个限制。进入 blk-mq 后，`bio_split_discard()` 再按队列的最大 discard 扇区数、粒度和对齐计算拆分点；secure erase 选择自己的长度上限，见 [`block/blk-merge.c:160-207`](../../linux/block/blk-merge.c#L160-L207)。公共提交还会检查操作是否受支持，见 [`block/blk-core.c:840-847`](../../linux/block/blk-core.c#L840-L847)。

### 2. 完成链把多个范围的结果汇总到最后一个 bio

`bio_chain_and_submit(prev, new)` 把旧 bio 链到新 bio，然后提交旧项并返回新项：

[`block/bio.c:359-366`](../../linux/block/bio.c#L359-L366)（v6.18.60）

```c
struct bio *bio_chain_and_submit(struct bio *prev, struct bio *new)
{
	if (prev) {
		bio_chain(prev, new);
		submit_bio(prev);
	}
	return new;
}
```

最后一个 bio 成为完成依赖的汇总端。同步助手对它调用 `submit_bio_wait()`，等待的不只是最后一段自身，而是整条完成链；较早段的错误会沿父关系传播，见 [`block/bio.c:314-347`](../../linux/block/bio.c#L314-L347)。提交顺序不要求设备按相同顺序完成。

当前 `blkdev_issue_discard()` 把同步等待返回的 `-EOPNOTSUPP` 转为成功，见 [`block/blk-lib.c:92-102`](../../linux/block/blk-lib.c#L92-L102)。所以助手返回 0 不能作为“设备确实擦除了数据”的证据，甚至不能证明它执行过 discard。

### 3. 清零优先硬件卸载，但不吞掉所有错误

`blkdev_issue_zeroout()` 先检查逻辑块对齐和只读状态。有硬件清零能力时调用 `blkdev_issue_write_zeroes()`；只有返回 `-EOPNOTSUPP` 才进入普通写回退，其余返回值直接上交，见 [`block/blk-lib.c:311-328`](../../linux/block/blk-lib.c#L311-L328)。

能力也可能在尝试后撤销。若清零失败且设备已把 `max_write_zeroes_sectors` 清为 0，内部助手会将错误解释为“不支持”，从而允许同步外层回退，见 [`block/blk-lib.c:172-183`](../../linux/block/blk-lib.c#L172-L183)。不能据此推广为“任何 I/O 错误都重试写零页”。

回退路径通过 `largest_zero_folio()` 取得零内容，循环把其范围加入普通 WRITE bio，见 [`block/blk-lib.c:197-230`](../../linux/block/blk-lib.c#L197-L230)。若指定 `BLKDEV_ZERO_NOFALLBACK`，则返回不支持而不写这些数据页，见 [`block/blk-lib.c:232-241`](../../linux/block/blk-lib.c#L232-L241)。

同步接口 `blkdev_issue_zeroout()` 与仅构造、链入 bio 的 `__blkdev_issue_zeroout()` 不能混读。后者不等待设备执行，自然也不能在该次调用里依据稍后的完成错误决定重新写零页，见 [`block/blk-lib.c:273-298`](../../linux/block/blk-lib.c#L273-L298)。

### 4. 安全擦除必须走独立能力检查

`blkdev_issue_secure_erase()` 依据 `max_secure_erase_sectors` 分段，检查对齐、只读状态并构造 `REQ_OP_SECURE_ERASE`，没有把“不支持”退化成 discard 或普通清零，见 [`block/blk-lib.c:330-371`](../../linux/block/blk-lib.c#L330-L371)。通用层只负责传递协议语义，实际是否满足数据处置要求还取决于设备实现和验证方法。

## 关键细节 / 设计考量

- **范围大，不代表内存数据大。** 无向量 bio 的长度限制仍需处理，但不能按 `bi_size` 估算它占用的数据页。
- **清零不自动保证掉电后仍为零。** 本节助手没有把普通范围操作统一变成 FUA 写；持久化边界仍按 [10.9](09-flush.md) 分析。
- **逻辑空间释放与物理数据抹除分开。** discard 成功、清零可读回零，都不应直接作为安全擦除的证明。
- **语义相似不代表控制接口通用。** discard 的限制不能用于推导 WRITE ZEROES 或 zone reset 的允许范围。

## 可观测性

sysfs 依赖 `CONFIG_SYSFS=y`；事件依赖 `CONFIG_TRACEPOINTS=y`、`CONFIG_EVENT_TRACING=y`、`CONFIG_TRACING=y`，见 [`linux/.config`](../../linux/.config)。以下路径均相对于 `/sys/block/<disk>/`。

| 入口 | 看什么 | 源码与解释 |
|---|---|---|
| `queue/discard_max_bytes`、`discard_max_hw_bytes` | 软件生效上限与硬件报告上限，单位为字节 | [`block/blk-sysfs.c:557-560`](../../linux/block/blk-sysfs.c#L557-L560)；值为 0 时不能据此期待支持 discard |
| `queue/discard_granularity` | discard 粒度，单位为字节 | [`block/blk-sysfs.c:189`](../../linux/block/blk-sysfs.c#L189)、[`block/blk-sysfs.c:558`](../../linux/block/blk-sysfs.c#L558) |
| `queue/write_zeroes_max_bytes` | 硬件清零路径的长度上限 | [`block/blk-sysfs.c:570`](../../linux/block/blk-sysfs.c#L570)；不表示普通写零页回退也不可用 |
| `block:block_rq_issue`、`block:block_rq_complete` | 实际进入请求层的操作与错误 | [`include/trace/events/block.h:118-258`](../../linux/include/trace/events/block.h#L118-L258)；清零回退后可能看到普通 WRITE |
| `stat` 第 12～15 项 | discard 完成、合并、扇区与累计时间 | [`block/genhd.c:1066-1107`](../../linux/block/genhd.c#L1066-L1107)；不是通用的安全擦除完成计数 |

可在 `blkdev_issue_zeroout()` 的入口和返回处记录范围、标志与结果，再结合请求事件辨认回退。动态探针依赖 `CONFIG_KPROBES=y`、`CONFIG_KPROBE_EVENTS=y` 及实际符号可挂载性；事件发生次数不能直接等同于一次助手调用。

本节只建议读取能力与已有事件。`blkdiscard`、清零、secure erase 都会改变存储内容，不应为了观测而对系统盘执行；[10.14](14-observability.md) 的只读 `null_blk` 实验也不验证这些数据语义。

## 小结

- discard、清零与安全擦除有不同契约，不能互相替代。
- 范围 bio 可以没有数据向量，`bi_size` 仍表示操作覆盖的字节数。
- 构造阶段分段与提交阶段按队列限制拆分分别处理不同约束。
- 完成链把多段结果汇总给同步等待者，discard 助手还会忽略不支持错误。
- 清零回退有明确条件和禁用标志；异步构造接口不等于同步失败后的回退接口。

## 参考

- [`block/blk-lib.c`](../../linux/block/blk-lib.c)
- [`Documentation/ABI/stable/sysfs-block`](../../linux/Documentation/ABI/stable/sysfs-block)
- [`Documentation/block/stat.rst`](../../linux/Documentation/block/stat.rst)
