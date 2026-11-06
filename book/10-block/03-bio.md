# 10.3 bio：数据描述、分配、拆分与完成链

> **本节源码**：[`include/linux/blk_types.h`](../../linux/include/linux/blk_types.h)、[`include/linux/bvec.h`](../../linux/include/linux/bvec.h)、[`block/bio.c`](../../linux/block/bio.c)、[`block/blk-merge.c`](../../linux/block/blk-merge.c)
> **关键函数**：`bio_alloc_bioset()`、`bio_add_page()`、`bio_alloc_clone()`、`bio_split()`、`bio_chain()`、`bio_endio()`
> **前置知识**：10.1 块设备层概览、4.1 page 与 folio、7.2 引用计数

## 概述

一次设备访问的内存数据未必连续，但设备地址范围通常是连续的。`bio` 用内存向量描述数据位置，用扇区与长度描述设备范围，避免为了描述 I/O 而先复制整份数据。迭代器把“原始向量数组”与“当前尚未处理的范围”分离，使克隆、拆分和部分完成不必改写共享向量。描述符、数据页以及完成依赖又有不同生命周期，必须分别管理。

## 核心数据结构

### 1. bio_vec 与 bvec_iter

[`include/linux/bvec.h:28-32`](../../linux/include/linux/bvec.h#L28-L32)（v6.18.60）

```c
struct bio_vec {
	struct page	*bv_page;
	unsigned int	bv_len;
	unsigned int	bv_offset;
};
```

`bv_page` 是物理连续范围的起始页，`bv_offset` 是相对起始页的偏移，`bv_len` 是字节数。一个向量可以跨越多个连续页，不能假设一个 `bio_vec` 恰好对应一页，定义前的说明见 [`include/linux/bvec.h:20-27`](../../linux/include/linux/bvec.h#L20-L27)。

[`include/linux/bvec.h:77-86`](../../linux/include/linux/bvec.h#L77-L86)（v6.18.60）

```c
struct bvec_iter {
	sector_t		bi_sector;	/* device address in 512 byte
						   sectors */
	unsigned int		bi_size;	/* residual I/O count */

	unsigned int		bi_idx;		/* current index into bvl_vec */

	unsigned int            bi_bvec_done;	/* number of bytes completed in
						   current bvec */
} __packed __aligned(4);
```

### 2. bio 的三个维度

| 维度 | 关键字段 | 含义 |
|---|---|---|
| 设备范围 | `bi_bdev`、`bi_opf`、`bi_iter` | 设备、操作及标志、当前范围 |
| 内存描述 | `bi_io_vec`、`bi_vcnt`、`bi_max_vecs` | 向量数组、已用向量数、容量 |
| 生命周期 | `bi_end_io`、`bi_private`、`__bi_remaining`、`__bi_cnt` | 完成回调、私有上下文、完成依赖、描述符引用 |

定义见 [`include/linux/blk_types.h:210-281`](../../linux/include/linux/blk_types.h#L210-L281)。`bi_vcnt` 并不随每次部分完成等比例减少，剩余有效范围应由 `bi_iter` 解读。`__bi_remaining` 也不是描述符引用计数，不能用 `bio_get()` / `bio_put()` 代替完成链计数。

## 主流程分析

```text
构造 bio
├── bio_alloc_bioset()                    block/bio.c
├── bio_add_page() / bio_add_folio()       block/bio.c
└── submit_bio()                          block/blk-core.c
    └── blk_mq_submit_bio()               block/blk-mq.c
        └── __bio_split_to_limits()       block/blk.h
            └── bio_split_rw() 等         block/blk-merge.c
                └── bio_submit_split_bioset()
                    ├── bio_split()      block/bio.c
                    │   └── bio_alloc_clone()
                    ├── bio_chain()      block/bio.c
                    └── 重提交原 bio 的剩余范围
完成时
bio_endio()                              block/bio.c
├── 等待 __bi_remaining 归零
├── 若为完成链：释放子 bio，继续处理父 bio
└── 调用上层 bi_end_io()
```

### 1. 内存池保障前进，但有使用约束

`bio_alloc_bioset()` 从 `bio_set` 的内存池（Memory Pool，mempool）分配描述符，向量少时可使用内联空间，较多时另行分配向量数组，见 [`block/bio.c:508-597`](../../linux/block/bio.c#L508-L597)。这些操作分配的是描述信息，不会替调用者分配或复制待传输数据页。

内存池保证依赖正确的使用方式：先提交前一个 `bio`，再分配下一个，避免把有限储备全部握在手里。提交路径会把递归转换为延后处理，因此带救援工作队列的 `bio_set` 在必要时把待提交 `bio` 移交救援线程；不是任意多个内存池分配组合都能自动避免死锁，见 [`block/bio.c:475-506`](../../linux/block/bio.c#L475-L506)。

### 2. 加入向量不等于满足所有硬件限制

`bio_add_page()` 尝试把页范围并入相邻向量，或新增一个向量，返回实际加入字节数；调用者不能忽略失败。实现见 [`block/bio.c:1024-1049`](../../linux/block/bio.c#L1024-L1049)。此时构造成功只说明描述符装得下；设备最大传输、散集段数和边界限制仍要在提交路径检查。

### 3. 克隆共享向量，拆分移动迭代器

`bio_alloc_clone()` 创建独立描述符，但共享原始 `bi_io_vec`，见 [`block/bio.c:831-884`](../../linux/block/bio.c#L831-L884)。`bio_split()` 再把新描述符限制为前半段，并通过 `bio_advance()` 推进原描述符，使原 `bio` 表示剩余部分，见 [`block/bio.c:1692-1734`](../../linux/block/bio.c#L1692-L1734)。

因此，拆分不是把数据搬成两份。共享向量及数据页必须一直有效到所有相关 I/O 完成；单独释放一个克隆不会替上层解除全部数据页生命周期约束。

### 4. bio_chain() 建立“都完成后再通知”的关系

[`block/bio.c:340-347`](../../linux/block/bio.c#L340-L347)（v6.18.60）

```c
void bio_chain(struct bio *bio, struct bio *parent)
{
	BUG_ON(bio->bi_private || bio->bi_end_io);

	bio->bi_private = parent;
	bio->bi_end_io	= bio_chain_endio;
	bio_inc_remaining(parent);
}
```

子 `bio` 的 `bi_private` 与 `bi_end_io` 被完成链占用，调用者不能再用这两个字段安放自己的回调。子完成时把错误传播到父对象、释放子描述符，并推进父完成依赖；父自身和全部子项都完成后，才通知原提交者，见 [`block/bio.c:314-326`](../../linux/block/bio.c#L314-L326)。

`bio_endio()` 用循环展开标准完成链，避免大量拆分造成无界递归。它还处理完整性、zone、QoS 和跟踪，再调用最终上层回调，见 [`block/bio.c:1631-1677`](../../linux/block/bio.c#L1631-L1677)。

## 关键细节 / 设计考量

- **不要直接调用别人的 `bi_end_io()`。** `bio_endio()` 才会处理完成依赖及公共收尾，接口约定见 [`block/bio.c:1619-1630`](../../linux/block/bio.c#L1619-L1630)。
- **不要修改克隆共享的向量数组。** 应通过自己的迭代器表示区间，并使用适合当前范围的迭代接口；完整向量遍历与剩余数据遍历用途不同，见 [`Documentation/block/biovecs.rst`](../../linux/Documentation/block/biovecs.rst)。
- **不是每种操作都能拆分。** `bio_split()` 拒绝 zone append 和 `REQ_ATOMIC`，见 [`block/bio.c:1702-1708`](../../linux/block/bio.c#L1702-L1708)。普通读写的 `REQ_NOWAIT` 若必须拆分，当前实现返回 `-EAGAIN`，见 [`block/blk-merge.c:375-384`](../../linux/block/blk-merge.c#L375-L384)。

## 可观测性

事件依赖 `CONFIG_TRACEPOINTS=y`、`CONFIG_TRACING=y`，本配置已启用；操作方法见 [10.12](12-observability.md)。

| 事件 / 函数 | 观察内容 | 定义或调用位置 |
|---|---|---|
| `block:block_split` | 拆分点 `new_sector`，观察单个大 I/O 如何受限 | [`include/trace/events/block.h:479-516`](../../linux/include/trace/events/block.h#L479-L516)、[`block/blk-merge.c:119-138`](../../linux/block/blk-merge.c#L119-L138) |
| `block:block_bio_queue` | 到达块层的设备、范围和操作 | [`include/trace/events/block.h:386-395`](../../linux/include/trace/events/block.h#L386-L395) |
| `block:block_bio_complete` | 被标记为需要 bio 级完成跟踪的对象 | [`block/bio.c:1643-1646`](../../linux/block/bio.c#L1643-L1646) |
| `bio_split()`、`bio_endio()` 动态探针 | 描述符范围、完成链及错误；挂载前检查符号可用性 | [`block/bio.c:1631-1734`](../../linux/block/bio.c#L1631-L1734) |

请求完成路径可能清除 `BIO_TRACE_COMPLETION`，改由请求级事件反映完成，见 [`block/blk-mq.c:1018-1033`](../../linux/block/blk-mq.c#L1018-L1033)。因此，不应假定每条 `block_bio_queue` 都能在普通 tracefs 输出中找到同数量的 `block_bio_complete`。

## 小结

- `bio_vec` 描述内存范围，`bvec_iter` 描述当前设备范围与向量进度。
- 分配 `bio` 不等于分配数据页；向量和页的生命周期由调用者协议保证。
- 克隆与拆分主要复制描述符、共享向量，不复制数据。
- 完成链计数与描述符引用计数独立；所有子 I/O 完成后才通知父提交者。
- 跟踪事件有不同层级，不能仅靠数量相等判断完成是否丢失。

## 参考

- [`Documentation/block/biovecs.rst`](../../linux/Documentation/block/biovecs.rst)
- [`include/linux/bio.h`](../../linux/include/linux/bio.h)
