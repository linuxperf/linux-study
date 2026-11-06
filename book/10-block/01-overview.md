# 10.1 块设备层概览：从文件偏移到设备命令

> **本节源码**：[`block/blk-core.c`](../../linux/block/blk-core.c)、[`block/blk-mq.c`](../../linux/block/blk-mq.c)、[`include/linux/blk_types.h`](../../linux/include/linux/blk_types.h)、[`include/linux/blk-mq.h`](../../linux/include/linux/blk-mq.h)
> **关键函数**：`submit_bio()`、`blk_mq_submit_bio()`、`blk_mq_start_request()`、`blk_mq_end_request()`、`bio_endio()`
> **前置知识**：C 结构体与回调、虚拟内存、文件读写的基本概念

## 概述

块设备层在上层的数据访问与下层的设备命令之间提供统一接口。它既要满足地址对齐、最大传输长度等硬件限制，也要减少请求数量、利用多队列并行，并在资源不足时形成背压。上层通常提交 `bio`，通用多队列路径再把一个或多个 `bio` 组织成 `request`。提交与完成是两条相互关联但可能运行在不同 CPU、不同上下文中的路径。

## 核心数据结构

先用四个问题区分对象，而不是把所有对象都理解成“磁盘请求”。

| 问题 | 对象 | 定义 |
|---|---|---|
| 这是什么磁盘，支持哪些操作？ | `gendisk` | [`include/linux/blkdev.h:143-223`](../../linux/include/linux/blkdev.h#L143-L223) |
| 访问整盘还是某个分区？ | `block_device` | [`include/linux/blk_types.h:41-82`](../../linux/include/linux/blk_types.h#L41-L82) |
| 访问哪个设备范围，数据在什么内存里？ | `bio` | [`include/linux/blk_types.h:210-281`](../../linux/include/linux/blk_types.h#L210-L281) |
| 交给请求型驱动的工作单元是什么？ | `request` | [`include/linux/blk-mq.h:103-217`](../../linux/include/linux/blk-mq.h#L103-L217) |

`request_queue` 保存设备限制、调度器与多队列状态，并不是“一条待执行请求链表”，见 [`include/linux/blkdev.h:469-552`](../../linux/include/linux/blkdev.h#L469-L552)。一个 `request` 可以串接多个 `bio`；反过来，一个较大的 `bio` 也可能先被拆分，再分别形成请求。因此，应用调用次数、`bio` 数、请求数和设备命令数不能直接画等号。

## 主流程分析

下面是普通、非堆叠的读写请求路径。树中完成部分由设备事件另行触发，并非 `submit_bio()` 同步调用的后半段；plug 批处理也可能把实际派发推迟到退出批次时。

```text
提交侧
submit_bio()                              block/blk-core.c
└── submit_bio_noacct()                    block/blk-core.c
    └── submit_bio_noacct_nocheck()        block/blk-core.c
        └── __submit_bio_noacct_mq()       block/blk-core.c
            └── __submit_bio()            block/blk-core.c
                └── blk_mq_submit_bio()   block/blk-mq.c
                    ├── 拆分与合并
                    ├── 分配 / 复用 request
                    └── plug / 排队 / 派发
                        └── mq_ops->queue_rq()   驱动
                            └── blk_mq_start_request()
完成侧（另一次执行）
驱动完成处理
└── blk_mq_end_request()                   block/blk-mq.c
    ├── blk_update_request()              block/blk-mq.c
    │   └── bio_endio()                   block/bio.c
    │       └── bio->bi_end_io()          上层回调
    └── __blk_mq_end_request()            block/blk-mq.c
```

依据分别是 [`block/blk-core.c:626-757`](../../linux/block/blk-core.c#L626-L757)、[`block/blk-mq.c:3136-3260`](../../linux/block/blk-mq.c#L3136-L3260)、[`block/blk-mq.c:967-1080`](../../linux/block/blk-mq.c#L967-L1080) 和 [`block/blk-mq.c:1172-1195`](../../linux/block/blk-mq.c#L1172-L1195)。

### 1. 上层先完成文件地址到设备地址的映射

块层入口接收的 `bio` 已经带有目标 `block_device` 和设备扇区地址。文件偏移到文件系统块、稀疏区间或共享区间的解释属于上层职责；通用块层不能只凭文件偏移推导设备扇区。提交接口契约见 [`block/blk-core.c:895-920`](../../linux/block/blk-core.c#L895-L920)。

缓冲读命中 page cache 时可以根本不产生设备 I/O；缓冲写先改脏内存，也不意味着相同线程立即提交对应写请求。分析块层事件时，应从已经到达 `submit_bio()` 的设备访问开始，而不能把每个 `read()` / `write()` 都当成一次块请求。

### 2. bio 描述数据，request 管理执行

`bio` 记录设备范围、内存向量与上层完成回调；`request` 额外记录队列上下文、标签（Tag）、超时、派发状态及调度器私有数据。`blk_mq_submit_bio()` 先尝试合并，再决定是否需要分配新的请求，见 [`block/blk-mq.c:3183-3221`](../../linux/block/blk-mq.c#L3183-L3221)。

这里的多队列块层（Multi-Queue Block Layer，blk-mq）把提交 CPU 与驱动队列关联起来，但不要求每个 CPU 独占一条硬件队列，也不保证请求一定经过软件排队。

### 3. 完成通知不同于函数返回

`submit_bio()` 返回类型是 `void`。错误通过 `bi_status` 和 `bi_end_io()` 交付；完成可以发生在函数返回之后，也可能因为检查失败或同步设备而发生在返回之前。提交者必须按完成回调管理数据缓冲区与描述符生命周期，不能把“函数已经返回”当作“可以复用数据”。契约见 [`block/blk-core.c:895-907`](../../linux/block/blk-core.c#L895-L907)。

## 关键细节 / 设计考量

### 地址单位始终要标明

块层的扇区（Sector）固定为 512 字节，见 [`include/linux/blk_types.h:25-36`](../../linux/include/linux/blk_types.h#L25-L36)。这与设备的逻辑块大小（Logical Block Size）、物理块大小（Physical Block Size）不是同一个概念：后两者保存在 `queue_limits`，见 [`include/linux/blkdev.h:369-430`](../../linux/include/linux/blkdev.h#L369-L430)。

例如，逻辑块为 4 KB 的设备上，`bi_sector = 8` 仍表示字节偏移 4096，而不是 32768。普通读写的起始偏移与长度必须满足设备逻辑块对齐；检查位于 [`block/blk-mq.c:3112-3121`](../../linux/block/blk-mq.c#L3112-L3121)。

### 并行化不等于取消策略

多队列减少共享热点，但合并、调度、持久化序列和资源控制仍需要协调。请求可以直接派发，也可以进入 plug 或调度器；设备接收多个命令后，完成顺序也未必与提交顺序相同。不能用“先提交”推导“先持久化”，持久化协议见 [10.9](09-flush-discard.md)。

### 不是所有块设备都构造 request

`__submit_bio()` 根据 `BD_HAS_SUBMIT_BIO` 选择通用 blk-mq 或磁盘自己的 `submit_bio` 方法，见 [`block/blk-core.c:636-647`](../../linux/block/blk-core.c#L636-L647)。设备映射、内存模拟等设备可能直接消费或重提交 `bio`；“块设备必有一个请求型驱动”不是接口要求。

## 小结

- `gendisk` 表示磁盘，`block_device` 区分整盘与分区，`bio` 描述数据，`request` 管理请求型设备执行。
- 块层扇区固定为 512 字节，不能与设备逻辑块或文件系统块混用。
- 提交、排队、驱动开始执行、完成和持久化是不同边界。
- `bio` 与 `request` 不是一一对应，堆叠设备还会增加重映射层次。
- 后续分析以普通 blk-mq 路径为主，并单独解释其旁路与特殊操作。

## 参考

- [`Documentation/block/blk-mq.rst`](../../linux/Documentation/block/blk-mq.rst)
- [`Documentation/block/biovecs.rst`](../../linux/Documentation/block/biovecs.rst)
- [`Documentation/block/writeback_cache_control.rst`](../../linux/Documentation/block/writeback_cache_control.rst)
