# 10.2 设备模型：gendisk、block_device 与 request_queue

> **本节源码**：[`include/linux/blkdev.h`](../../linux/include/linux/blkdev.h)、[`include/linux/blk_types.h`](../../linux/include/linux/blk_types.h)、[`block/genhd.c`](../../linux/block/genhd.c)、[`block/bdev.c`](../../linux/block/bdev.c)、[`block/blk-mq.c`](../../linux/block/blk-mq.c)
> **关键函数**：`__blk_mq_alloc_disk()`、`__alloc_disk_node()`、`device_add_disk()`、`del_gendisk()`、`blk_mq_quiesce_queue()`
> **前置知识**：10.1 块设备层概览、7.2 引用计数、7.9 RCU

## 概述

块层需要同时表示设备能力、可访问地址范围与并发执行状态。把这些信息放进同一个对象，会让分区、设备共享队列和热拔除的关系难以表达。因此，内核用 `gendisk` 描述磁盘，以 `block_device` 表示整盘或分区，以 `request_queue` 管理队列限制和请求路径。设备撤销时，还必须把“禁止新访问”“停止派发”“等待已有使用者退出”分开处理。

## 核心数据结构

### 1. gendisk：磁盘身份与整盘入口

以下裁剪保留字段原名与顺序，中文注释为本书添加。

[`include/linux/blkdev.h:143-164`](../../linux/include/linux/blkdev.h#L143-L164)（v6.18.60）

```c
struct gendisk {
	...
	char disk_name[DISK_NAME_LEN];	/* name of major driver */
	...
	struct xarray part_tbl;		/* [注] 分区号到 block_device */
	struct block_device *part0;	/* [注] 整盘 block_device */

	const struct block_device_operations *fops;
	struct request_queue *queue;
	void *private_data;

	struct bio_set bio_split;	/* [注] 拆分 bio 的分配池 */
	...
```

整盘本身也有 `block_device`，由 `part0` 指向；`gendisk` 的引用计数依托 `part0` 内的设备对象，见 [`include/linux/blkdev.h:253-260`](../../linux/include/linux/blkdev.h#L253-L260)。因此，不能把 `gendisk` 当成可在注销后立即 `kfree()` 的普通私有结构。

### 2. block_device：范围与访问状态

[`include/linux/blk_types.h:41-46`](../../linux/include/linux/blk_types.h#L41-L46)（v6.18.60）

```c
struct block_device {
	sector_t		bd_start_sect;
	sector_t		bd_nr_sectors;
	struct gendisk *	bd_disk;
	struct request_queue *	bd_queue;
	struct disk_stats __percpu *bd_stats;
	...
```

`bd_start_sect` 是分区在整盘中的起始位置，`bd_nr_sectors` 是其容量，单位均为 512 字节扇区；`bd_disk` 指向所属磁盘，`bd_queue` 指向其队列。分区有独立地址范围与统计，但不会因此自动获得独立 I/O 调度器（I/O Scheduler）。其余打开者、持有者和地址空间字段见 [`include/linux/blk_types.h:60-82`](../../linux/include/linux/blk_types.h#L60-L82)。

### 3. request_queue：能力与执行入口

| 字段 | 含义 |
|---|---|
| `limits` | 逻辑块、最大扇区数、段数、discard 与功能位 |
| `mq_ops` | 请求型驱动的操作集合 |
| `queue_ctx` / `hctx_table` | per-CPU 软件上下文与硬件上下文集合 |
| `elevator` | 当前 I/O 调度器，无调度器时为空 |
| `q_usage_counter` | 提交 / 请求路径的使用引用，冻结队列依靠它等待 |
| `rq_qos` | 请求 QoS 策略链 |

定义见 [`include/linux/blkdev.h:469-552`](../../linux/include/linux/blkdev.h#L469-L552)。`limits.features` 中的 `BLK_FEAT_*` 描述驱动声明的能力，不能沿用其他版本中所有能力都位于 `queue_flags` 的读法，见 [`include/linux/blkdev.h:300-355`](../../linux/include/linux/blkdev.h#L300-L355)。

## 主流程分析

以驱动建立独立 blk-mq 磁盘为例；这是生命周期关系，不表示所有驱动都采用完全相同的清理封装。

```text
驱动初始化
├── blk_mq_alloc_tag_set()                block/blk-mq.c
├── blk_mq_alloc_disk() 宏
│   └── __blk_mq_alloc_disk()             block/blk-mq.c
│       ├── blk_mq_alloc_queue()          block/blk-mq.c
│       └── __alloc_disk_node()          block/genhd.c
│           └── bdev_alloc(disk, 0)       block/bdev.c
├── 设置磁盘名称、fops、容量与私有数据
└── device_add_disk()                     block/genhd.c
    └── add_disk_fwnode()                 block/genhd.c
        ├── __add_disk()                 block/genhd.c
        └── add_disk_final()             block/genhd.c
```

### 1. 分配不等于发布

`__blk_mq_alloc_disk()` 先分配队列，再建立磁盘，见 [`block/blk-mq.c:4486-4506`](../../linux/block/blk-mq.c#L4486-L4506)。`__alloc_disk_node()` 初始化拆分池、回写设备信息、`part0`、分区表、blkcg 和设备对象，把磁盘与队列双向关联，见 [`block/genhd.c:1454-1500`](../../linux/block/genhd.c#L1454-L1500)。

此时驱动仍要完成自己的初始化。调用 `device_add_disk()` 才进入发布流程：设备模型、队列 sysfs 与后续分区扫描可能使外部使用者开始访问。必须在发布前设置完成路径所需的私有数据，而不能把发布当成最后才会生效的一条日志操作。

### 2. 分区共享磁盘，提交时调整扇区

分区 `bio` 最初携带相对分区的地址；`blk_partition_remap()` 在检查边界后加上 `bd_start_sect`，并设置 `BIO_REMAPPED`，见 [`block/blk-core.c:577-591`](../../linux/block/blk-core.c#L577-L591)。本版本并未在这里把 `bi_bdev` 强制换成整盘设备，分区身份仍可供统计等路径使用。

### 3. 冻结与静止不是同一种等待

队列冻结（Queue Freeze）通过使用计数阻止新的常规入口，并等待已有使用引用退出。开始冻结和等待的实现见 [`block/blk-mq.c:163-209`](../../linux/block/blk-mq.c#L163-L209)，提交入口对冻结的等待见 [`block/blk-core.c:299-367`](../../linux/block/blk-core.c#L299-L367)。驱动已持有的请求可能仍需完成，才能使等待结束。

队列静止（Queue Quiesce）则阻止新的派发，并等待正在执行的派发临界区退出。它不保证设备中所有命令完成，也不禁止完成回调运行；阻塞型驱动借助 SRCU，其他驱动借助 RCU 等待，见 [`block/blk-mq.c:273-306`](../../linux/block/blk-mq.c#L273-L306)。

### 4. 撤销可见性与释放引用分开

`del_gendisk()` 负责撤销磁盘注册并组织退出流程，见 [`block/genhd.c:697-833`](../../linux/block/genhd.c#L697-L833)。实际对象释放还要服从设备和队列引用，驱动应使用与分配方式配套的释放接口。尤其不能在仍有请求引用时先销毁标签集合，否则完成路径将失去请求对象和驱动私有区。

## 关键细节 / 设计考量

- **能力是设备约束，不是性能建议。** `max_sectors`、段数和对齐限制会改变拆分结果；更新限制必须与并发 I/O 协调，而不只是修改显示值。
- **磁盘不必对应一块物理介质。** 虚拟磁盘、堆叠磁盘也使用这些对象；是否经过 blk-mq 取决于提交接口。
- **不要混淆两类引用。** `request_queue` 的对象引用保证结构存在；`q_usage_counter` 参与正在使用的队列路径与冻结协议。两者定义见 [`include/linux/blkdev.h:491-498`](../../linux/include/linux/blkdev.h#L491-L498)。

## 可观测性

下列 sysfs 入口依赖 `CONFIG_SYSFS=y`；`/proc` 依赖 `CONFIG_PROC_FS=y`，均为本配置取值。

| 入口 | 看什么 | 源码 |
|---|---|---|
| `/sys/class/block/<dev>/size` | 容量，单位为 512 字节扇区 | `part_size_show()`，[`block/genhd.c:1060-1064`](../../linux/block/genhd.c#L1060-L1064) |
| `/sys/class/block/<partition>/start` | 分区起始扇区，对应 `bd_start_sect` | `part_start_show()`，[`block/partitions/core.c:182-187`](../../linux/block/partitions/core.c#L182-L187) |
| `/sys/block/<disk>/queue/logical_block_size`、`physical_block_size` | 对齐单位，不是 `sector_t` 的单位 | [`block/blk-sysfs.c:551-552`](../../linux/block/blk-sysfs.c#L551-L552) |
| `/sys/block/<disk>/queue/scheduler` | 整盘队列的调度器 | [`block/blk-sysfs.c:549`](../../linux/block/blk-sysfs.c#L549) |
| `/proc/diskstats` | 设备号、名称及分区 / 整盘计数 | `diskstats_show()`，[`block/genhd.c:1361-1418`](../../linux/block/genhd.c#L1361-L1418) |

动态探针可优先考虑 `device_add_disk()`、`del_gendisk()` 和 `blk_mq_quiesce_queue()`，但应先确认运行内核符号与可挂载列表；它们观测的是生命周期边界，不是每次设备命令。

## 小结

- 整盘与分区都有 `block_device`，同一磁盘的分区共享磁盘队列。
- `gendisk` 组织设备身份与分区，`request_queue` 组织限制、策略与并发执行。
- 分配阶段尚未发布设备；注册后就必须能正确处理 I/O。
- freeze 等待队列使用者，quiesce 等待派发临界区，两者都不能被理解成简单的“暂停磁盘”。

## 参考

- [`Documentation/block/blk-mq.rst`](../../linux/Documentation/block/blk-mq.rst)
- [`Documentation/ABI/stable/sysfs-block`](../../linux/Documentation/ABI/stable/sysfs-block)
