# 9.10 脏页写回与文件同步

> **本节源码**：[`mm/page-writeback.c`](../../linux/mm/page-writeback.c)、[`fs/fs-writeback.c`](../../linux/fs/fs-writeback.c)、[`fs/sync.c`](../../linux/fs/sync.c)、[`mm/filemap.c`](../../linux/mm/filemap.c)、[`include/linux/writeback.h`](../../linux/include/linux/writeback.h)、[`lib/errseq.c`](../../linux/lib/errseq.c)
> **关键函数**：`folio_mark_dirty()`、`filemap_dirty_folio()`、`__mark_inode_dirty()`、`balance_dirty_pages()`、`wb_workfn()`、`do_writepages()`、`vfs_fsync_range()`、`file_check_and_advance_wb_err()`
> **前置知识**：[9.5 inode](05-inode.md)、[9.7 读写路径](07-read-write.md)、[9.8 page cache](08-page-cache.md)、4.14 memcg

## 概述

缓冲写把数据复制到 page cache 后即可返回，这降低了前台延迟，却留下了“数据何时离开内存、失败怎样通知应用”的问题。写回（Writeback）把已修改的数据交给文件系统和存储层，同时通过脏页限速（Dirty Throttling）约束生产速度，避免内存被待写数据耗尽。文件同步则为应用提供主动等待数据及必要元数据持久化的接口，不能与一次后台写回混为一谈。

本节聚焦可写回的普通文件：先从 folio 的脏状态追到 inode 队列，再分析后台工作、前台限速、`fsync()` 分派与错误游标。ramfs 没有后备存储，不走这种常规磁盘写回；其对比见 [9.12 节](12-directory-ramfs.md)。是否具备写回能力由 `mapping_can_writeback()` 检查 `BDI_CAP_WRITEBACK`，见 [`include/linux/backing-dev.h:134-137`](../../linux/include/linux/backing-dev.h#L134-L137)。

## 核心数据结构

### 三层状态不能互相替代

| 层次 | 状态 | 用途与源码 |
|---|---|---|
| folio | `PG_dirty`、`PG_writeback` | 数据待写与一次写回在途是独立状态；开始、完成协议见 [`mm/page-writeback.c:2985-3083`](../../linux/mm/page-writeback.c#L2985-L3083) |
| mapping | `PAGECACHE_TAG_DIRTY`、`PAGECACHE_TAG_WRITEBACK`、`PAGECACHE_TAG_TOWRITE` | 让扫描器跳过无关范围；标记定义见 [`include/linux/fs.h:533-536`](../../linux/include/linux/fs.h#L533-L536) |
| inode | `I_DIRTY_PAGES`、元数据脏位、`i_io_list` | 表示文件需要处理，并加入写回队列；更新见 [`fs/fs-writeback.c:2579-2722`](../../linux/fs/fs-writeback.c#L2579-L2722) |

`PG_dirty` 与 `PG_writeback` 可以同时为真：上一轮写回尚未完成时，文件又被修改，后续仍需写回。因此“写回完成”不等于“这个 folio 从此干净”。

### 写回范围与策略：struct writeback_control

以下只摘录本节需要的公开字段；完整定义见链接。

[`include/linux/writeback.h:43-91`](../../linux/include/linux/writeback.h#L43-L91)（v6.18.60）

```c
struct writeback_control {
	/* public fields that can be set and/or consumed by the caller: */
	long nr_to_write;		/* Write this many pages, and decrement
					   this for each page written */
	long pages_skipped;		/* Pages which were not written */
...
	loff_t range_start;
	loff_t range_end;

	enum writeback_sync_modes sync_mode;
...
};
```

- `nr_to_write` 是基础页数量预算；文件系统迭代器消耗它，不是字节数。
- `range_start`、`range_end` 是字节范围，结束位置包含在内。
- `sync_mode` 区分 `WB_SYNC_NONE` 与 `WB_SYNC_ALL`。它影响扫描和等待策略，不直接代表整个设备已经达到掉电持久化状态。
- 未在摘录中展示的 `for_background`、`for_sync`、`range_cyclic` 等位域分别说明触发原因和扫描方式，见 [`include/linux/writeback.h:60-68`](../../linux/include/linux/writeback.h#L60-L68)。

### 谁组织写回：struct bdi_writeback

后备设备信息（Backing Device Information，BDI）描述存储侧能力；`bdi_writeback` 组织具体写回域。它包含 `b_dirty`、`b_io`、`b_more_io`、`b_dirty_time` 等 inode 链表，以及吞吐估计和 `dirty_ratelimit`，见 [`include/linux/backing-dev-defs.h:102-143`](../../linux/include/linux/backing-dev-defs.h#L102-L143)。

本配置为 `CONFIG_MEMCG=y`、`CONFIG_CGROUP_WRITEBACK=y`，见 [`linux/.config:216-219`](../../linux/.config#L216-L219)。支持控制组写回的文件系统可把写回归入相应域；不能仅凭配置打开，就认定所有文件系统和所有 inode 都支持这种归属。`writeback_control` 中相关字段也受该配置控制，见 [`include/linux/writeback.h:83-91`](../../linux/include/linux/writeback.h#L83-L91)。

## 主流程分析

### 调用链总览

下面展示通用的弄脏与后台写回分支；文件系统可实现自己的 `dirty_folio()`、`writepages()`，不要求所有文件都采用同一个具体函数。

```text
folio_mark_dirty()                         mm/page-writeback.c
└── a_ops->dirty_folio()
    └── 例如 filemap_dirty_folio()          mm/page-writeback.c
        ├── __folio_mark_dirty()           mm/page-writeback.c
        │   └── folio_account_dirtied()     mm/page-writeback.c
        └── __mark_inode_dirty()           fs/fs-writeback.c
            └── 加入 wb 队列，必要时安排延迟工作

wb_workfn()                                fs/fs-writeback.c
└── wb_do_writeback()                       fs/fs-writeback.c
    └── wb_writeback()                     fs/fs-writeback.c
        └── writeback_sb_inodes()          fs/fs-writeback.c
            └── __writeback_single_inode() fs/fs-writeback.c
                ├── do_writepages()        mm/page-writeback.c
                │   └── a_ops->writepages()
                └── 按需要写 inode 元数据
```

*图 9-11：从脏 folio 到后台写回的两条执行路径*

分派与后台入口分别见 [`mm/page-writeback.c:2743-2832`](../../linux/mm/page-writeback.c#L2743-L2832)、[`fs/fs-writeback.c:2373-2439`](../../linux/fs/fs-writeback.c#L2373-L2439)。两条路径不是同一个前台调用栈。

### 1. 第一次弄脏：记账、索引与排队

`folio_mark_dirty()` 把操作交给 mapping 的 `dirty_folio()`。以不使用 buffer head 的通用实现 `filemap_dirty_folio()` 为例，只有干净到脏的转换才继续记账：设置 folio 脏位，给 xarray 添加 `PAGECACHE_TAG_DIRTY`，再把 inode 标为 `I_DIRTY_PAGES`，见 [`mm/page-writeback.c:2703-2757`](../../linux/mm/page-writeback.c#L2703-L2757)。

`folio_account_dirtied()` 对可写回 mapping 增加 `NR_FILE_DIRTY`、`NR_DIRTIED` 和相应 wb 计数，并累计当前任务的 `nr_dirtied`。同一 folio 已经脏时重复修改，不会按写入次数反复增加这些页计数，见 [`mm/page-writeback.c:2647-2672`](../../linux/mm/page-writeback.c#L2647-L2672)。

`__mark_inode_dirty()` 在 inode 第一次变脏时记录时间并放入适当链表；已在脏队列中的 inode 不随每次修改重新排队，以免破坏时间顺序。首个脏 inode 还可能安排延迟工作，见 [`fs/fs-writeback.c:2637-2712`](../../linux/fs/fs-writeback.c#L2637-L2712)。

### 2. 后台写回：先选 inode，再选 folio

`wb_workfn()` 是工作队列回调，不应把源码中的 flusher 一概理解为永久独占的专用线程。`wb_do_writeback()` 先消费显式工作，再检查全量请求、周期性旧数据和后台阈值，见 [`fs/fs-writeback.c:2373-2439`](../../linux/fs/fs-writeback.c#L2373-L2439)。

选中 inode 后，`__writeback_single_inode()` 调用 `do_writepages()`。在需要的数据完整性同步场景下，它还等待数据，然后处理 inode 元数据脏位。清除 inode 脏状态后会重新检查 mapping 的脏标记，避免并发写入丢失唤醒或漏写，见 [`fs/fs-writeback.c:1746-1830`](../../linux/fs/fs-writeback.c#L1746-L1830)。

`do_writepages()` 在预算有效时调用文件系统的 `writepages()`；没有回调便返回零，**不是在这里自动逐页调用 `writepage()`**，见 [`mm/page-writeback.c:2593-2610`](../../linux/mm/page-writeback.c#L2593-L2610)。分析具体文件系统时，应继续追它自己的实现。

采用通用 `writeback_iter()` 的实现，在 `WB_SYNC_ALL` 或显式要求标记扫描时先建立 `TOWRITE` 集合，避免持续新增脏页使本轮同步永远结束不了；循环扫描还通过 `mapping->writeback_index` 保存进度，见 [`mm/page-writeback.c:2504-2590`](../../linux/mm/page-writeback.c#L2504-L2590)。

### 3. 一次写回的 folio 协议

提交前，`folio_clear_dirty_for_io()` 在持 folio 锁的条件下清除脏状态并调整计数，但暂时保留 xarray 脏标记，使并发同步扫描仍能发现它。该函数还用 `folio_mkclean()` 处理映射页表中的脏状态，见 [`mm/page-writeback.c:2886-2956`](../../linux/mm/page-writeback.c#L2886-L2956)。

开始写回时，`__folio_start_writeback()` 设置 `PG_writeback` 和写回标记；若 folio 没被重新弄脏，再清除脏标记。完成时，`__folio_end_writeback()` 清除写回状态、调整 `NR_WRITEBACK` 并增加 `NR_WRITTEN`，上层完成路径再负责唤醒等待者，见 [`mm/page-writeback.c:2985-3083`](../../linux/mm/page-writeback.c#L2985-L3083)。

这套协议协调的是“哪一轮数据正在处理”。持久化顺序、日志提交以及设备缓存刷新仍由文件系统和块层继续保证，见下文同步路径与第 10 章。

### 4. 前台限速：让生产速度接近消耗速度

缓冲写和共享映射写都可能调用 `balance_dirty_pages_ratelimited()`。它按任务累计的脏页数量批量检查，不必每弄脏一页都进行完整控制，见 [`mm/page-writeback.c:2026-2107`](../../linux/mm/page-writeback.c#L2026-L2107)。

`balance_dirty_pages()` 主要做三件事，见 [`mm/page-writeback.c:1810-2010`](../../linux/mm/page-writeback.c#L1810-L2010)：

1. 计算全局域和适用的 memcg 域阈值；通常越过较低后台阈值时启动后台写回，仍处于自由运行区间时直接返回。
2. 根据 wb 的脏页位置和写回吞吐估计，计算当前任务允许的弄脏速度；有 memcg 约束时综合两层域选择控制条件。
3. 根据本批弄脏数量换算应等待的时间，在可睡眠路径调用 `io_schedule_timeout()`。带 `BDP_ASYNC` 的调用可返回 `-EAGAIN`，而不是强制睡眠。

所以写入可能在 `write()` 中变慢，不必等到 `fsync()` 才等待 I/O。`dirty_ratio` 也不是“达到百分比才第一次发生任何限速”的单一开关。

### 5. 文件同步：VFS 分派，文件系统兑现语义

[`fs/sync.c:179-189`](../../linux/fs/sync.c#L179-L189)（v6.18.60）

```c
int vfs_fsync_range(struct file *file, loff_t start, loff_t end, int datasync)
{
	struct inode *inode = file->f_mapping->host;

	if (!file->f_op->fsync)
		return -EINVAL;
	if (!datasync && (inode->i_state & I_DIRTY_TIME))
		mark_inode_dirty_sync(inode);
	return file->f_op->fsync(file, start, end, datasync);
}
```

`fsync()` 传入 `datasync=0`，`fdatasync()` 传入 `1`，后者仍必须同步正确访问已修改数据所需的元数据，例如文件大小；不是“完全不写元数据”。入口见 [`fs/sync.c:199-223`](../../linux/fs/sync.c#L199-L223)。`O_SYNC`、`O_DSYNC` 与相应写标志在 `generic_write_sync()` 中也可进入范围同步，见 [`include/linux/fs.h:3067-3078`](../../linux/include/linux/fs.h#L3067-L3078)。

以本配置支持的 ext4（`CONFIG_EXT4_FS=y`，见 [`linux/.config:11642`](../../linux/.config#L11642)）为例：有日志时，`ext4_sync_file()` 先写出并等待数据，再提交所需日志事务，必要时调用 `blkdev_issue_flush()`。无日志则走另一套元数据同步分支。实现见 [`fs/ext4/fsync.c:82-189`](../../linux/fs/ext4/fsync.c#L82-L189)。是否启用日志、屏障取决于实际文件系统和挂载条件，不能从编译配置推出。

## 关键细节 / 设计考量

### 错误是游标协议，不是错误事件队列

异步写回失败时，`mapping_set_error()` 把错误记录到 `mapping->wb_err`，见 [`include/linux/pagemap.h:228-264`](../../linux/include/linux/pagemap.h#L228-L264)。打开文件时采样到 `f_wb_err`，见 [`fs/open.c:910-914`](../../linux/fs/open.c#L910-L914)。同步路径通过 `file_check_and_advance_wb_err()` 比较并推进游标，见 [`mm/filemap.c:722-771`](../../linux/mm/filemap.c#L722-L771)。

必须避免三种误读：

- 多个描述符共享同一个 `struct file` 时，也共享错误游标；不是每个 fd 都独立收到一遍错误。
- `errseq_t` 不为每次失败保存记录。错误会合并或覆盖，序号也不是每次设置都无条件递增，见 [`lib/errseq.c:49-110`](../../linux/lib/errseq.c#L49-L110)。
- 不能绝对地说“打开之前的错误永远不可见”：`errseq_sample()` 对尚未被观察的错误有特殊处理，见 [`lib/errseq.c:113-133`](../../linux/lib/errseq.c#L113-L133)。

应用应检查每次同步调用的返回值，并把错误理解为可能覆盖一批较早写入的失败，而非某一次 `write()` 的精确回执。

### 写回、同步与命名空间持久化

`sync_file_range()` 主要控制页缓存范围的提交与等待，不能代替文件系统的完整 `fsync()` 语义；其源码注释明确警告它不负责完整的元数据和磁盘缓存同步，见 [`fs/sync.c:303-350`](../../linux/fs/sync.c#L303-L350)。同样，`close()` 成功不等于已经执行文件同步。

文件内容的同步与父目录中名字的持久化也是两件事。依赖崩溃恢复语义的创建或重命名流程，通常还需要同步涉及的父目录，并核对所用文件系统的保证；不能从 `vfs_fsync_range()` 的接口推导所有目录项都已持久化。ext4 对目录与普通文件日志提交的区分见 [`fs/ext4/fsync.c:109-128`](../../linux/fs/ext4/fsync.c#L109-L128)。

## 可观测性

本节的 `/proc` 入口依赖 `CONFIG_PROC_FS=y`，sysctl 依赖 `CONFIG_SYSCTL=y`；分别见 [`linux/.config:11777`](../../linux/.config#L11777) 与 [`linux/.config:269`](../../linux/.config#L269)。事件跟踪依赖 `CONFIG_TRACEPOINTS=y`、`CONFIG_EVENT_TRACING=y`，见 [`linux/.config:320`](../../linux/.config#L320)、[`linux/.config:12937`](../../linux/.config#L12937)。

| 入口 | 看什么、怎样解释 | 源码依据 |
|---|---|---|
| `/proc/meminfo` 的 `Dirty`、`Writeback` | 分别对应 `NR_FILE_DIRTY`、`NR_WRITEBACK`，输出单位为 KB，不是某个文件独占的计数 | [`fs/proc/meminfo.c:97-100`](../../linux/fs/proc/meminfo.c#L97-L100) |
| `/proc/vmstat` 的 `nr_dirty`、`nr_writeback`、`nr_dirtied`、`nr_written` | 前两者是当前页数，后两者是累计页计数；弄脏和完成时的更新位置见主流程，不应直接把差值当成设备在途字节 | [`mm/vmstat.c:1254-1265`](../../linux/mm/vmstat.c#L1254-L1265)、[`mm/page-writeback.c:2647-2672`](../../linux/mm/page-writeback.c#L2647-L2672)、[`mm/page-writeback.c:2985-3020`](../../linux/mm/page-writeback.c#L2985-L3020) |
| `/proc/sys/vm/dirty_background_ratio`、`dirty_background_bytes`、`dirty_ratio`、`dirty_bytes` | 后台触发与前台控制的比例或字节配置；同组比例与字节设置互斥，不能把两个都相加 | [`mm/page-writeback.c:487-547`](../../linux/mm/page-writeback.c#L487-L547)、[`mm/page-writeback.c:2269-2303`](../../linux/mm/page-writeback.c#L2269-L2303) |
| `/proc/sys/vm/dirty_writeback_centisecs`、`dirty_expire_centisecs` | 周期性检查间隔与旧脏数据年龄，单位为百分之一秒；不是应用数据的掉电安全期限 | [`mm/page-writeback.c:103-110`](../../linux/mm/page-writeback.c#L103-L110)、[`mm/page-writeback.c:2304-2318`](../../linux/mm/page-writeback.c#L2304-L2318) |
| `writeback:balance_dirty_pages` | `dirty`、`wb_dirty` 为页数，速率字段转为 KB/s，`pause`、`paused`、`period` 转为毫秒；存在事件不等于一定真的睡眠 | [`include/trace/events/writeback.h:654-732`](../../linux/include/trace/events/writeback.h#L654-L732) |
| `writeback:writeback_single_inode_start`、`writeback:writeback_single_inode` | 一次 inode 写回工作的前后状态和预算，不等于介质完成时间 | [`include/trace/events/writeback.h:766-824`](../../linux/include/trace/events/writeback.h#L766-L824) |
| `filemap:file_check_and_advance_wb_err` | `old`、`new` 展示一次打开文件的错误游标推进；正常无变化的快路径不会产生该事件 | [`include/trace/events/filemap.h:168-198`](../../linux/include/trace/events/filemap.h#L168-L198)、[`mm/filemap.c:745-761`](../../linux/mm/filemap.c#L745-L761) |

## 动手实验

在虚拟机的本地磁盘文件系统上做一个有限大小的实验，不修改全局脏页阈值。不使用 tmpfs、ramfs 或仓库共享挂载。需要 Python 3、`trace-cmd` 和足够空间。

```bash
work=$(mktemp -d /var/tmp/vfs-wb.XXXXXX)
sudo trace-cmd record -o "$work/writeback.dat" -e writeback \
    -e filemap:file_check_and_advance_wb_err \
    python3 - "$work/data" <<'PY'
import os, sys, time

def state():
    with open("/proc/meminfo") as f:
        return [s.strip() for s in f if s.startswith(("Dirty:", "Writeback:"))]

fd = os.open(sys.argv[1], os.O_CREAT | os.O_TRUNC | os.O_WRONLY, 0o600)
chunk = b"x" * (1024 * 1024)
t = time.monotonic()
for _ in range(64):
    view = memoryview(chunk)
    while view:
        view = view[os.write(fd, view):]
print("write seconds:", time.monotonic() - t, state(), flush=True)
t = time.monotonic()
os.fsync(fd)
print("fsync seconds:", time.monotonic() - t, state(), flush=True)
os.close(fd)
PY
sudo trace-cmd report -i "$work/writeback.dat" | head -80
# 保存需要的结果后删除 work 目录。
```

这只验证观测方法，不保证 64 MB 写入足以触发前台限速。后台写回可能在 `fsync()` 前已消化大部分脏页，其他进程也会影响全局统计。若没有错误游标事件，通常只是没有发生相关写回错误；不要为了观察它而在有数据的设备上注入故障。上述实验尚未在项目虚拟机中执行。

## 小结

- folio 脏位、mapping 标记和 inode 队列分别解决状态、查找与调度问题。
- 后台工作通过 `writepages()` 交给文件系统，前台限速则约束新脏数据的产生。
- `PG_dirty` 与 `PG_writeback` 可以并存，完成一轮不代表后续修改已经写出。
- `fsync()` 是文件系统语义入口；等待数据、提交日志与刷新设备缓存不能混为一层。
- 写回错误按打开文件的游标报告，不是逐次写入、逐个描述符的可靠事件队列。

## 参考

- [`Documentation/admin-guide/sysctl/vm.rst`](../../linux/Documentation/admin-guide/sysctl/vm.rst)：脏页阈值、周期与过期参数。
- [`Documentation/filesystems/vfs.rst`](../../linux/Documentation/filesystems/vfs.rst)：地址空间操作与文件同步接口。
- [`Documentation/core-api/errseq.rst`](../../linux/Documentation/core-api/errseq.rst)：错误序列的采样、设置与观察语义。
- [`fs/ext4/fsync.c`](../../linux/fs/ext4/fsync.c)：数据、元数据、日志与设备刷新衔接的具体实例。
- [第 10 章块设备层](../10-block/README.md)：flush、FUA 与设备命令的持久化边界。
