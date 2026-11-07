# 10.2 设备模型：gendisk、block_device 与队列生命周期

> **本节源码**：[`include/linux/blkdev.h`](../../linux/include/linux/blkdev.h)、[`include/linux/blk_types.h`](../../linux/include/linux/blk_types.h)、[`block/genhd.c`](../../linux/block/genhd.c)、[`block/bdev.c`](../../linux/block/bdev.c)、[`block/partitions/core.c`](../../linux/block/partitions/core.c)、[`block/blk-core.c`](../../linux/block/blk-core.c)、[`block/blk-mq.c`](../../linux/block/blk-mq.c)
> **关键函数**：`__blk_mq_alloc_disk()`、`__alloc_disk_node()`、`device_add_disk()`、`add_partition()`、`del_gendisk()`、`put_disk()`、`blk_mq_quiesce_queue()`
> **前置知识**：[10.1 块设备层概览](01-overview.md)、[7.2 引用计数](../07-sync/02-atomic.md)、[7.9 RCU](../07-sync/09-rcu-basics.md)

## 概述

同一块磁盘可以提供整盘和多个分区入口，但这些入口不应各自复制一套驱动队列与调度策略。块层用 `gendisk` 组织磁盘身份，用 `block_device` 表示可访问的整盘或分区（Partition）范围，用 `request_queue` 管理共享的能力限制与请求路径。三者是长期存在的设备对象，生命周期不同于一次 I/O 的 `bio` 和 `request`。本节沿“分配对象 → 发布设备 → 撤销访问 → 释放引用”分析，再说明冻结和静止分别等待什么；单次 I/O 的地址检查与重映射留给 [10.4](04-submit.md)。

## 核心数据结构

### 1. gendisk：组织磁盘，而不是保存每一次访问

以下只保留本节相关字段，带 `[注]` 的注释为本书添加。

[`include/linux/blkdev.h:143-235`](../../linux/include/linux/blkdev.h#L143-L235)（v6.18.60）

```c
struct gendisk {
	...
	char disk_name[DISK_NAME_LEN];	/* name of major driver */
	/* [注] 磁盘名，用于设备模型中的命名 */
	...
	struct xarray part_tbl;		/* [注] 分区号到 block_device 的索引 */
	struct block_device *part0;	/* [注] 整盘入口，也位于 part_tbl[0] */

	const struct block_device_operations *fops; /* [注] 磁盘操作表 */
	struct request_queue *queue;	/* [注] 块层队列 */
	void *private_data;		/* [注] 驱动私有数据 */
	...
};
```

这里最重要的关系是：**整盘也有一个 `block_device`，不是只有分区才有。** `part0` 指向它，分区号为 0；其他分区通过 `part_tbl` 索引。整盘入口和索引 0 的建立见 [`block/genhd.c:1472-1481`](../../linux/block/genhd.c#L1472-L1481)。

`gendisk` 没有另放一个独立的 `struct device`。`disk_to_dev(disk)` 返回 `&disk->part0->bd_device`，磁盘的设备模型身份和引用计数都依托这个对象，见 [`include/linux/blkdev.h:253-260`](../../linux/include/linux/blkdev.h#L253-L260)。因此，“磁盘已注销”不能直接推导出“可以释放 `gendisk` 内存”。

### 2. block_device：一个范围、一份访问状态

[`include/linux/blk_types.h:41-83`](../../linux/include/linux/blk_types.h#L41-L83)（v6.18.60）

```c
struct block_device {
	sector_t		bd_start_sect;	/* [注] 相对整盘的起始扇区 */
	sector_t		bd_nr_sectors;	/* [注] 本范围的扇区数 */
	struct gendisk *	bd_disk;	/* [注] 所属磁盘 */
	struct request_queue *	bd_queue;	/* [注] 所属磁盘的队列 */
	struct disk_stats __percpu *bd_stats; /* [注] 本入口的 per-CPU 统计 */
	...
	dev_t			bd_dev;		/* [注] 本入口的设备号 */
	struct address_space	*bd_mapping;	/* page cache */

	atomic_t		bd_openers;	/* [注] 打开计数，不是对象引用计数 */
	...
	struct device		bd_device;	/* [注] 设备模型对象 */
} __randomize_layout;
```

`bd_start_sect` 和 `bd_nr_sectors` 均以 **512 字节扇区**为单位，与设备逻辑块大小无关；约定见 [`include/linux/blk_types.h:25-35`](../../linux/include/linux/blk_types.h#L25-L35)。`bdev_alloc()` 为每个入口分配统计，将 `bd_disk` 设为所属磁盘，将 `bd_queue` 设为 `disk->queue`，见 [`block/bdev.c:451-480`](../../linux/block/bdev.c#L451-L480)。

以一块普通的分区磁盘为例，关系如下；这是对象关系，不是内存布局。

| 入口 | `gendisk` 中的位置 | 地址范围 | 队列关系 |
|---|---|---|---|
| 整盘 `vda` | `part0`，同时为 `part_tbl[0]` | 从扇区 0 开始，容量由整盘入口记录 | `bd_queue == disk->queue` |
| 分区 `vda1` | `part_tbl[1]` | 从自身 `bd_start_sect` 开始，共 `bd_nr_sectors` 个扇区 | 仍为同一个 `disk->queue` |
| 分区 `vda2` | `part_tbl[2]` | 另一段整盘范围 | 仍为同一个 `disk->queue` |

`block_device` 也不是每次 `open()` 新建的文件对象。打开路径按设备号查找已有对象并取得设备引用，再更新打开和持有状态；`struct file` 使用它的 `bd_mapping`。对应实现见 [`block/bdev.c:817-839`](../../linux/block/bdev.c#L817-L839)、[`block/bdev.c:921-989`](../../linux/block/bdev.c#L921-L989)。不要把 `bd_openers`、设备对象引用和文件引用混成一个计数。

### 3. request_queue：共享约束与执行管理，不是硬件命令环

| 字段 | 本节关注的职责 |
|---|---|
| `limits` | 长度、逻辑块大小、段数和操作能力等设备约束 |
| `mq_ops` | blk-mq 驱动的请求操作表，区别于 `gendisk->fops` |
| `queue_ctx` / `hctx_table` | per-CPU 软件上下文与硬件上下文集合，详见 [10.5](05-blk-mq.md) |
| `elevator` | 队列的 I/O 调度器（I/O Scheduler）；未安装时为 `NULL` |
| `refs` | 队列对象引用计数，保证 `request_queue` 结构仍然存在 |
| `q_usage_counter` | 队列使用引用，参与提交、请求分配与冻结协议 |
| `queue_flags` / `quiesce_depth` | 队列运行状态与静止嵌套深度 |
| `disk` | 关联的磁盘对象 |

字段定义见 [`include/linux/blkdev.h:469-519`](../../linux/include/linux/blkdev.h#L469-L519)。冻结深度另存于 `mq_freeze_depth`，其更新见 [`block/blk-mq.c:163-178`](../../linux/block/blk-mq.c#L163-L178)。

由这些关系可以直接得出：**分区有各自的范围、设备号和统计，但没有因此获得各自的 `request_queue` 或调度器。** 另外，不能把能力位和运行状态位混读：本版本的写缓存、FUA、轮询等能力位于 `limits.features`，见 [`include/linux/blkdev.h:300-355`](../../linux/include/linux/blkdev.h#L300-L355)，而不是全部放在 `queue_flags` 中。

## 主流程分析

下面以 `blk_mq_alloc_disk()` 创建并拥有队列的普通、非隐藏磁盘为主线。这是驱动初始化的阶段图，省略驱动自身的资源准备与错误回滚。

[`block/blk-mq.c:4486-4506`](../../linux/block/blk-mq.c#L4486-L4506)、[`block/genhd.c:405-632`](../../linux/block/genhd.c#L405-L632)、[`block/genhd.c:1454-1500`](../../linux/block/genhd.c#L1454-L1500)（v6.18.60）

```text
驱动初始化
├── blk_mq_alloc_tag_set()                 block/blk-mq.c
├── blk_mq_alloc_disk() 宏                 include/linux/blk-mq.h
│   └── __blk_mq_alloc_disk()              block/blk-mq.c
│       ├── blk_mq_alloc_queue()           block/blk-mq.c
│       └── __alloc_disk_node()           block/genhd.c
│           └── bdev_alloc(disk, 0)        block/bdev.c
├── 设置名称、fops、私有数据和容量；准备好驱动 I/O 路径
└── device_add_disk()                      block/genhd.c
    └── add_disk_fwnode()                  block/genhd.c
        ├── __add_disk()                  block/genhd.c
        │   ├── device_add()              drivers/base/core.c
        │   └── blk_register_queue()      block/blk-sysfs.c
        └── add_disk_final()              block/genhd.c
            ├── bdev_add()                block/bdev.c
            ├── disk_scan_partitions()    block/genhd.c（容量非零时）
            └── disk_uevent()             block/genhd.c
```

### 1. 分配：先有队列，再建立整盘对象

`blk_mq_alloc_disk()` 是注入锁类信息的宏，其实际工作由 `__blk_mq_alloc_disk()` 完成，见 [`include/linux/blk-mq.h:725-733`](../../linux/include/linux/blk-mq.h#L725-L733)。

[`block/blk-mq.c:4486-4505`](../../linux/block/blk-mq.c#L4486-L4505)（v6.18.60）

```c
struct gendisk *__blk_mq_alloc_disk(struct blk_mq_tag_set *set,
		struct queue_limits *lim, void *queuedata,
		struct lock_class_key *lkclass)
{
	struct request_queue *q;
	struct gendisk *disk;

	q = blk_mq_alloc_queue(set, lim, queuedata);
	if (IS_ERR(q))
		return ERR_CAST(q);

	disk = __alloc_disk_node(q, set->numa_node, lkclass);
	if (!disk) {
		blk_mq_destroy_queue(q);
		blk_put_queue(q);
		return ERR_PTR(-ENOMEM);
	}
	set_bit(GD_OWNS_QUEUE, &disk->state);
	return disk;
}
```

先创建队列，是因为后续 `bdev_alloc()` 要把它写入 `bd_queue`。`__alloc_disk_node()` 建立拆分池、后备设备信息、整盘入口和分区索引，并调用 `device_initialize()`；最后设置 `q->disk`。这些是内部对象初始化，还不是设备注册，见 [`block/genhd.c:1454-1500`](../../linux/block/genhd.c#L1454-L1500)。其中块 I/O 控制组初始化依赖 `CONFIG_BLK_CGROUP`，本配置为 `y`，取值见 [`linux/.config`](../../linux/.config)。

`GD_OWNS_QUEUE` 表示磁盘负责队列退出。不能把它当作所有磁盘的恒定属性：`blk_mq_alloc_disk_for_queue()` 使用调用者已有的队列，只增加队列对象引用，不设置该位，见 [`block/blk-mq.c:4508-4520`](../../linux/block/blk-mq.c#L4508-L4520)。后面的撤销路径会据此区分清理责任。

### 2. 发布：注册过程中就可能发生 I/O

驱动必须先设置磁盘名、`fops`、容量及回调所需的私有状态，再调用 `device_add_disk()`。后者不是一次原子的“显示设备”操作，而是分阶段开放入口：

1. `__add_disk()` 检查提交接口、分配或验证设备号，通过 `device_add()` 加入设备模型。新驱动无需自行指定主次设备号的约定见 [`include/linux/blkdev.h:144-150`](../../linux/include/linux/blkdev.h#L144-L150)；注册实现见 [`block/genhd.c:427-512`](../../linux/block/genhd.c#L427-L512)。
2. `blk_register_queue()` 建立磁盘下的 `queue/` 属性，注册 blk-mq 相关对象并设置默认调度器，见 [`block/blk-sysfs.c:907-961`](../../linux/block/blk-sysfs.c#L907-L961)。
3. `__add_disk()` 成功后，`add_disk_final()` 才把整盘入口加入设备号查找结构，按条件扫描分区，再发送磁盘及分区的添加事件，见 [`block/genhd.c:405-425`](../../linux/block/genhd.c#L405-L425)、[`block/genhd.c:608-614`](../../linux/block/genhd.c#L608-L614)。

**I/O 路径必须在调用注册函数之前可用，而不是等注册返回后才启用。** 分区扫描本身就可能读取设备；添加事件的延迟只约束通知顺序，不保证整个注册过程没有访问。virtio-blk 在 `device_add_disk()` 之前设置操作表、更新容量并调用 `virtio_device_ready()`，可作为具体对照，见 [`drivers/block/virtio_blk.c:1510-1537`](../../linux/drivers/block/virtio_blk.c#L1510-L1537)。

### 3. 分区：新增范围对象，不复制驱动队列

`add_partition()` 先取得整盘设备引用，再调用 `bdev_alloc(disk, partno)`，设置起始扇区与长度，将分区设备的父对象设为整盘，最后插入 `part_tbl` 并调用 `bdev_add()`。对应实现见 [`block/partitions/core.c:295-393`](../../linux/block/partitions/core.c#L295-L393)。这些步骤只新建分区入口，没有另行分配队列。

这也建立了明确的生命期依赖：分区存在时需要保住整盘；分区设备的释放回调 `part_release()` 通过 `put_disk()` 归还这一引用，见 [`block/partitions/core.c:247-251`](../../linux/block/partitions/core.c#L247-L251)。仅把分区从索引和 sysfs 撤下，不等于所有持有它的访问者都已消失。

提交时，分区地址还需要加上 `bd_start_sect`，但这不会创建新队列，也不会在该处把 `bi_bdev` 换成整盘。实现见 [`block/blk-core.c:577-591`](../../linux/block/blk-core.c#L577-L591)；边界检查和坐标转换在 [10.4](04-submit.md) 展开。

### 4. 撤销：先关闭入口，再等待队列使用者退出

`del_gendisk()` 不只是删除 sysfs 目录。其内部的 `__del_gendisk()` 同时处理访问入口、上层持有者、分区与队列退出，见 [`block/genhd.c:697-783`](../../linux/block/genhd.c#L697-L783)。按本节的队列归磁盘所有的路径，可分为以下阶段：

| 顺序 | 主要动作 | 目的 |
|---|---|---|
| 1 | 对分区表中的入口执行 `bdev_unhash()` | 从设备号查找结构移除，阻止新的打开者 |
| 2 | 尚未标死时调用 `blk_report_disk_dead(disk, false)` | 通知上层持有者执行有序退出；此时尚未开始下述队列排空 |
| 3 | 设置 `GD_DEAD`，设置队列 `DYING`，容量清零并启动排空，删除分区 | 让新的 I/O 失败，而不是等待将来恢复 |
| 4 | 撤销队列注册与设备可见性 | 移除 sysfs 等外部管理入口 |
| 5 | 等待 `q_usage_counter` 归零，取消相关定时器和工作，退出 blk-mq 队列 | 收束已经进入队列的工作及相关异步活动 |

标死和启动排空的实现见 [`block/genhd.c:660-679`](../../linux/block/genhd.c#L660-L679)、[`block/blk-core.c:283-297`](../../linux/block/blk-core.c#L283-L297)。已有请求仍需由驱动完成或终止，使使用引用能够归还；标死并不会替驱动生成设备完成。

这个顺序也说明，不宜把“先静止队列，再删除磁盘”当作通用清理模板：若停止派发后仍留下持有使用引用的待发请求，排空就可能无法结束。驱动必须先明确哪些请求还能正常完成、哪些需要终止。若磁盘不拥有队列，撤销路径还会重新允许直通请求使用队列，而不是永久关闭整个队列，见 [`block/genhd.c:767-779`](../../linux/block/genhd.c#L767-L779)。

### 5. 释放：注销和最后一次引用归还不是同一时刻

`put_disk()` 本质上是 `put_device(disk_to_dev(disk))`。最后一个设备引用归还后，`disk_release()` 清理磁盘资源、调用 `blk_put_queue()` 归还队列对象引用，并通过 `bdev_drop(part0)` 归还整盘 inode 引用，见 [`block/genhd.c:1280-1322`](../../linux/block/genhd.c#L1280-L1322)、[`block/genhd.c:1538-1555`](../../linux/block/genhd.c#L1538-L1555)。`gendisk` 的内存最终在整盘的 `bdev_free_inode()` 中释放，见 [`block/bdev.c:377-395`](../../linux/block/bdev.c#L377-L395)。

因此要分别检查三套协议：

| 保护对象 / 工作 | 引用或状态 | 退出边界 |
|---|---|---|
| 磁盘及分区设备对象 | `bd_device` 的设备引用 | 设备释放回调，随后归还对应 inode 引用 |
| `request_queue` 结构 | `refs` | `blk_put_queue()` 减到 0 后进入队列回收 |
| 已进入队列的使用者 | `q_usage_counter` | 使用者归还引用，冻结等待条件才成立 |

队列对象引用的释放与使用引用的归还分别见 [`block/blk-core.c:248-281`](../../linux/block/blk-core.c#L248-L281)、[`block/blk-core.c:369-380`](../../linux/block/blk-core.c#L369-L380)。**对象仍存在，不代表它仍接受 I/O；使用计数为零，也不代表可以绕过对象引用直接释放结构。**

标签集合的销毁还要区分失败路径和正常移除。注册前的失败回滚需要先 `put_disk()` 再释放标签集合，因为 `disk_release()` 可能还要退出 blk-mq 队列；成功注册后的移除已在 `del_gendisk()` 中完成这部分退出。virtio-blk 的两条路径正好展示了不同顺序，见 [`drivers/block/virtio_blk.c:1542-1581`](../../linux/drivers/block/virtio_blk.c#L1542-L1581)，不能仅凭初始化顺序反推所有释放步骤。

## 关键细节 / 设计考量

### 冻结与静止：等待的不是同一批工作

队列冻结（Queue Freeze）控制的是**队列使用入口**。第一次开始冻结时，`__blk_freeze_queue_start()` 增加冻结深度，并对 `q_usage_counter` 执行 `percpu_ref_kill()`；随后还会推动 blk-mq 队列运行，让已有请求能够继续前进。`blk_mq_freeze_queue_wait()` 才负责等待计数为零，见 [`block/blk-mq.c:163-209`](../../linux/block/blk-mq.c#L163-L209)。所以“开始冻结”和“冻结等待完成”必须分开。

新入口通过 `percpu_ref_tryget_live_rcu()` 尝试取得使用引用，见 [`block/blk.h:60-94`](../../linux/block/blk.h#L60-L94)。尝试失败后，普通调用者等待解冻；NOWAIT 路径不会睡眠；设备已死或队列正在退出时则返回错误。具体返回值与 bio 完成方式见 [`block/blk-core.c:304-367`](../../linux/block/blk-core.c#L304-L367)。

队列静止（Queue Quiesce）控制的是**请求派发临界区**。`blk_mq_quiesce_queue()` 设置 `QUEUE_FLAG_QUIESCED` 并等待已进入临界区的派发退出：声明 `BLK_MQ_F_BLOCKING` 的驱动使用 SRCU，其他驱动使用 RCU。它不等待设备内所有命令完成，也不阻止请求结束回调运行，见 [`block/blk-mq.c:261-306`](../../linux/block/blk-mq.c#L261-L306)。

| 机制 | 等待完成后可以依赖的条件 | 不能据此推导的结论 |
|---|---|---|
| freeze | 已有使用引用耗尽，常规新入口不能取得使用引用 | 队列对象已释放；它本身也不是设备持久化命令 |
| quiesce | 已有派发临界区退出，解静止前不再开始派发 | 在途命令全部完成、完成回调不会运行 |
| dead / dying 与排空 | 永久退出路径使新的访问失败，并等待已有使用者退出 | 标记状态的一刻就释放所有资源 |

静止期间仍可有请求加入等待派发的集合，`blk_mq_unquiesce_queue()` 在静止深度降到 0 后重新运行队列，见 [`block/blk-mq.c:315-333`](../../linux/block/blk-mq.c#L315-L333)。这与冻结等待使用引用耗尽的目标不同。

本版本还有一个接口细节：`blk_mq_freeze_queue()` 返回保存的内存分配上下文标志，配对的 `blk_mq_unfreeze_queue()` 需要该返回值，见 [`include/linux/blk-mq.h:949-962`](../../linux/include/linux/blk-mq.h#L949-L962)。不要直接套用其他版本的单参数解冻示例。

### 限制更新：不能只保护写字段的瞬间

能力限制会影响当前 I/O 如何拆分与构造，不只是 sysfs 显示值。`queue_limits_start_update()` 取得 `limits_lock` 并返回限制快照，但这个锁本身不排空 I/O，见 [`include/linux/blkdev.h:1056-1072`](../../linux/include/linux/blkdev.h#L1056-L1072)。

`queue_limits_commit_update()` 要求调用者已冻结队列，或以其他方式保证没有未结束 I/O；它验证新限制后才写回。需要封装冻结与解冻时可使用 `queue_limits_commit_update_frozen()`，见 [`block/blk-settings.c:522-581`](../../linux/block/blk-settings.c#L522-L581)。这是一处把“对象仍存在”和“当前可以安全改变其约束”明确分开的实例。

## 可观测性

sysfs 入口依赖 `CONFIG_SYSFS=y`，`/proc` 依赖 `CONFIG_PROC_FS=y`；动态探针依赖 `CONFIG_KPROBES=y`、`CONFIG_KPROBE_EVENTS=y`。这些均为仓库 [`linux/.config`](../../linux/.config) 的取值，实验前仍须核对实际运行内核。

### 从属性反推对象关系

| 入口 | 看什么、怎么解读 | 输出实现 |
|---|---|---|
| `/sys/class/block/<dev>/dev` | 整盘与分区各自的 `major:minor`，不要把不同设备号误读成不同队列 | `dev_show()`，[`drivers/base/core.c:3049-3054`](../../linux/drivers/base/core.c#L3049-L3054) |
| `/sys/class/block/<dev>/size` | 对应 `bd_nr_sectors`，乘 512 才是字节容量 | `part_size_show()`，[`block/genhd.c:1060-1064`](../../linux/block/genhd.c#L1060-L1064) |
| `/sys/class/block/<partition>/partition`、`start` | 分区号与 `bd_start_sect`；后者是整盘坐标中的起点 | `part_partition_show()`、`part_start_show()`，[`block/partitions/core.c:176-186`](../../linux/block/partitions/core.c#L176-L186) |
| `/sys/block/<disk>/queue/logical_block_size`、`physical_block_size` | `limits` 中的块大小，单位是字节，不改变 `size` 和 `start` 的扇区单位 | 宏生成的显示函数及属性，[`block/blk-sysfs.c:172-185`](../../linux/block/blk-sysfs.c#L172-L185)、[`block/blk-sysfs.c:551-552`](../../linux/block/blk-sysfs.c#L551-L552) |
| `/sys/block/<disk>/queue/scheduler` | 队列调度器，方括号标明当前选择；`[none]` 表示未安装调度器，不是没有队列 | `elv_iosched_show()`，[`block/elevator.c:834-861`](../../linux/block/elevator.c#L834-L861) |
| `/proc/diskstats` | 按 `part_tbl` 遍历整盘与分区，汇总各自的 `bd_stats`；多行统计不表示多个调度器 | `diskstats_show()` 及接口注册，[`block/genhd.c:1361-1436`](../../linux/block/genhd.c#L1361-L1436) |

只读检查时，先选一个分区，读取其 `partition`、`start`、`size`，再查看父磁盘的 `queue/` 属性。这样可以把“独立范围与统计、共享队列能力”对应到运行时视图。统计的累计口径与完整实验见 [10.14](14-observability.md)，不应把整盘和分区计数直接相加当作独立设备总量。

### 从探针区分生命周期边界

- `device_add_disk()`：同时观察入口与返回值，区分“尝试发布”和“发布成功”；它不是每条设备命令的入口，见 [`block/genhd.c:627-632`](../../linux/block/genhd.c#L627-L632)。
- `del_gendisk()`：观察撤销开始与返回；其耗时可能包含等待已有使用引用退出，不能直接解释为 sysfs 删除耗时，见 [`block/genhd.c:697-832`](../../linux/block/genhd.c#L697-L832)。
- `blk_mq_freeze_queue_wait()` 与 `blk_mq_quiesce_queue()`：分别观察使用引用等待和派发临界区等待，不把两者当作同一种“暂停时间”，见 [`block/blk-mq.c:189-193`](../../linux/block/blk-mq.c#L189-L193)、[`block/blk-mq.c:291-306`](../../linux/block/blk-mq.c#L291-L306)。

记录事件时应带上目标磁盘或队列身份，并确认运行内核确实存在可挂载符号。`blk_mq_freeze_queue()` 是 `static inline` 包装，不能直接当作稳定探针入口；需要观测整个冻结操作时，可核查其非内联实现 `blk_mq_freeze_queue_nomemsave()`，见 [`block/blk-mq.c:204-209`](../../linux/block/blk-mq.c#L204-L209)。

## 小结

- `gendisk` 组织磁盘，`block_device` 表示整盘或分区范围，`request_queue` 提供共享约束与执行管理。
- `part0` 既是整盘入口，也是磁盘设备模型身份和引用计数的依托；分区另有设备对象并持有整盘引用。
- 分配、设备注册和磁盘添加事件是不同阶段；注册过程中就可能发生分区扫描 I/O。
- `del_gendisk()` 关闭入口并组织排空，`put_disk()` 归还对象引用；两者不能互相替代。
- freeze 等待队列使用引用，quiesce 等待派发临界区；对象引用还决定内存何时回收。
- 队列所有权、是否成功注册以及驱动在途工作，共同决定清理顺序，不能机械地倒放初始化步骤。

## 参考

- [`Documentation/block/blk-mq.rst`](../../linux/Documentation/block/blk-mq.rst)
- [`Documentation/ABI/stable/sysfs-block`](../../linux/Documentation/ABI/stable/sysfs-block)
- [`block/genhd.c`](../../linux/block/genhd.c)：磁盘分配、注册、撤销与设备释放回调。
- [`block/partitions/core.c`](../../linux/block/partitions/core.c)：分区设备的建立与释放。
- [`drivers/block/virtio_blk.c:1492-1581`](../../linux/drivers/block/virtio_blk.c#L1492-L1581)：注册失败与正常移除两条清理路径。
