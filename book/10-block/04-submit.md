# 10.4 提交路径：检查、分区重映射与堆叠设备

> **本节源码**：[`block/blk-core.c`](../../linux/block/blk-core.c)、[`block/blk-mq.c`](../../linux/block/blk-mq.c)、[`block/blk-merge.c`](../../linux/block/blk-merge.c)、[`block/blk.h`](../../linux/block/blk.h)
> **关键函数**：`submit_bio()`、`submit_bio_noacct()`、`blk_partition_remap()`、`submit_bio_noacct_nocheck()`、`__submit_bio()`、`blk_mq_submit_bio()`
> **前置知识**：10.2 设备模型、10.3 bio

## 概述

块层不能把上层构造的任何 `bio` 原样交给驱动：设备可能不支持某种操作，分区地址要转换为整盘地址，数据大小也可能超过硬件能力。堆叠设备还会改写目标设备并向下重提交。公共提交路径把这些工作分层处理，同时把递归重提交变成迭代，限制内核栈使用。理解各入口的职责，才能避免重复记账、漏掉检查或错误复用已经重映射的地址。

## 核心数据结构

| 字段 / 标志 | 用途 | 定义 |
|---|---|---|
| `bio->bi_bdev`、`bi_iter.bi_sector` | 当前目标设备与扇区 | [`include/linux/blk_types.h:210-244`](../../linux/include/linux/blk_types.h#L210-L244) |
| `BIO_REMAPPED` | 分区地址转换已经完成 | [`include/linux/blk_types.h:315`](../../linux/include/linux/blk_types.h#L315) |
| `BD_HAS_SUBMIT_BIO` | 设备使用自己的 bio 提交方法 | [`include/linux/blk_types.h:49-58`](../../linux/include/linux/blk_types.h#L49-L58) |
| `current->bio_list` | 正在提交时暂存递归产生的 bio | [`block/blk-core.c:652-722`](../../linux/block/blk-core.c#L652-L722) 中的使用说明 |
| `queue_limits` | 长度、对齐、段数及操作能力限制 | [`include/linux/blkdev.h:369-430`](../../linux/include/linux/blkdev.h#L369-L430) |

## 主流程分析

```text
submit_bio()                              block/blk-core.c
├── 读写 VM 计数、I/O 优先级初始化
└── submit_bio_noacct()                    block/blk-core.c
    ├── NOWAIT 支持、故障注入与只读提示
    ├── bio_check_eod()                   block/blk-core.c
    ├── blk_partition_remap()             block/blk-core.c
    ├── flush 标志过滤、操作能力检查
    ├── blk_throtl_bio()                  block/blk-throttle.h
    └── submit_bio_noacct_nocheck()        block/blk-core.c
        ├── blkcg 记账、bio 入队事件
        └── bio_list 迭代 / 暂存
            └── __submit_bio()            block/blk-core.c
                ├── blk_crypto_bio_prep()
                ├── blk_mq_submit_bio()   普通请求型设备
                └── disk->fops->submit_bio()  自定义 bio 路径
```

### 1. submit_bio() 是上层入口，noacct 是重提交入口

`submit_bio()` 记录读入 / 写出 VM 事件并初始化 I/O 优先级，随后进入 `submit_bio_noacct()`，见 [`block/blk-core.c:887-920`](../../linux/block/blk-core.c#L887-L920)。文件系统等上层应使用前者；堆叠驱动向下重提交使用后者，以免重复上层记账。

`noacct` 不等于“完全不做统计”或“不做检查”：它仍执行公共验证，并可进入下层设备的 blkcg 统计。接口说明见 [`block/blk-core.c:770-780`](../../linux/block/blk-core.c#L770-L780)。

### 2. 先检查分区范围，再转换地址

`bio_check_eod()` 用当前 `block_device` 容量检查是否越界。检查采用减法形式，避免起始扇区与长度相加溢出，见 [`block/blk-core.c:555-572`](../../linux/block/blk-core.c#L555-L572)。对尚未标记 `BIO_REMAPPED` 的分区 `bio`，再加上分区起始扇区。

[`block/blk-core.c:577-591`](../../linux/block/blk-core.c#L577-L591)（v6.18.60）

```c
static int blk_partition_remap(struct bio *bio)
{
	struct block_device *p = bio->bi_bdev;

	if (unlikely(should_fail_request(p, bio->bi_iter.bi_size)))
		return -EIO;
	if (bio_sectors(bio)) {
		bio->bi_iter.bi_sector += p->bd_start_sect;
		trace_block_bio_remap(bio, p->bd_dev,
				      bio->bi_iter.bi_sector -
				      p->bd_start_sect);
	}
	bio_set_flag(bio, BIO_REMAPPED);
	return 0;
}
```

这也解释了为什么同一个设备号下可能在不同事件中看到相对分区和整盘坐标。堆叠驱动改变设备时，应通过 `bio_set_dev()` 等正式接口更新关联，不能只改一个指针而忽略重映射标志与 blkcg，见 [`include/linux/bio.h:509-516`](../../linux/include/linux/bio.h#L509-L516)。

### 3. 操作能力检查不同于普通数据长度检查

`submit_bio_noacct()` 根据 `bio_op()` 区分普通读写、discard、清零及 zone 操作。设备不支持某操作时返回 `BLK_STS_NOTSUPP`；没有易失写缓存时，flush / FUA 标志可以被过滤，空 flush 直接成功，见 [`block/blk-core.c:782-873`](../../linux/block/blk-core.c#L782-L873)。

一个容易忽略的细节是：此处的 `bio_check_ro()` 对部分内部写路径发出一次性警告，并非所有写入都在这里统一返回只读错误。不能仅凭函数名把它写成完整的只读权限检查，见 [`block/blk-core.c:522-540`](../../linux/block/blk-core.c#L522-L540)。

### 4. 递归改为迭代，优先推进下层

堆叠驱动的 `submit_bio` 可能再次调用 `submit_bio_noacct()`。若每层立刻递归，栈深度会随设备层次和拆分增长。当前路径发现 `current->bio_list` 非空时，把新 `bio` 挂入列表，由最外层循环继续处理，见 [`block/blk-core.c:725-757`](../../linux/block/blk-core.c#L725-L757)。

一般堆叠路径进一步区分“相同队列”和“其他队列”产生的 `bio`，优先处理下层项，再处理同层项和之前遗留项，见 [`block/blk-core.c:674-712`](../../linux/block/blk-core.c#L674-L712)。这是局部提交顺序，不是全局设备拓扑排序，也不意味着完成顺序被固定。

### 5. 到达 blk-mq 后仍要拆分

`blk_mq_submit_bio()` 在持有队列使用引用后检查逻辑块对齐、轮询支持，再调用 `__bio_split_to_limits()`，见 [`block/blk-mq.c:3168-3190`](../../linux/block/blk-mq.c#L3168-L3190)。普通读写按长度、物理段和边界限制计算拆分点；不合法或无法安全拆分时完成错误，而不是无限向下尝试。

拆分助手将原 `bio` 缩为剩余部分并重提交，返回可立即处理的前半段，见 [`block/blk-merge.c:108-140`](../../linux/block/blk-merge.c#L108-L140)。当前迭代框架保证这种重提交不会不断加深栈。

## 关键细节 / 设计考量

- `REQ_NOWAIT` 是对可能阻塞路径的约束，不是“不需要完成通知”。不支持与暂时不能完成分别可能对应 `BLK_STS_NOTSUPP` 和 `BLK_STS_AGAIN`。
- `submit_bio_noacct_nocheck()` 是已经满足相应前提的内部入口，不是绕过校验的通用优化 API。
- `bio` 可能在提交检查中立刻完成；错误路径必须和正常路径遵守同一生命周期规则。

## 可观测性

事件依赖本配置的 `CONFIG_TRACEPOINTS=y`、`CONFIG_TRACING=y`。

| 入口 | 对应边界 | 解读 |
|---|---|---|
| `block:block_bio_remap` | 分区或驱动重映射 | `old_dev`、`old_sector` 与新设备范围共同解释地址；定义见 [`include/trace/events/block.h:518-557`](../../linux/include/trace/events/block.h#L518-L557) |
| `block:block_bio_queue` | 进入未检查提交内部路径 | 位于限速之后，不能据此测得此前在 blk-throttle 的全部等待；调用见 [`block/blk-core.c:725-737`](../../linux/block/blk-core.c#L725-L737) |
| `block:block_getrq` | 新 bio 已取得请求对象 | 可能在 QoS / 标签等待之后，调用见 [`block/blk-mq.c:3198-3220`](../../linux/block/blk-mq.c#L3198-L3220) |
| `submit_bio()`、`submit_bio_noacct()`、`blk_mq_submit_bio()` 探针 | 分层入口 | 需同时记录目标设备与对象身份；只按当前进程名无法还原全部堆叠关系 |

`bio_check_eod()` 等小型静态或内联辅助函数不宜被当成稳定探针入口。探针还应考虑函数是否存在于运行内核符号表，而不是只看源码中有无定义。

## 小结

- 上层使用 `submit_bio()`，堆叠设备重提交使用 `submit_bio_noacct()`。
- 分区先按自身容量校验，再转换为整盘扇区坐标。
- `bio_list` 把递归重提交转换为迭代，限制栈深度。
- 公共操作检查与 blk-mq 的对齐 / 拆分检查各有职责。
- 入队事件并不位于所有等待之前，时延测量必须标明起点。

## 参考

- [`Documentation/block/biovecs.rst`](../../linux/Documentation/block/biovecs.rst)
- [`block/blk.h:408-460`](../../linux/block/blk.h#L408-L460)
