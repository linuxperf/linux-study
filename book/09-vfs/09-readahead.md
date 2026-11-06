# 9.9 文件预读：访问模式与预读窗口

> **本节源码**：[`mm/readahead.c`](../../linux/mm/readahead.c)、[`mm/filemap.c`](../../linux/mm/filemap.c)、[`mm/fadvise.c`](../../linux/mm/fadvise.c)、[`include/linux/fs.h`](../../linux/include/linux/fs.h)、[`include/linux/pagemap.h`](../../linux/include/linux/pagemap.h)
> **关键函数**：`page_cache_sync_ra()`、`page_cache_async_ra()`、`page_cache_ra_order()`、`page_cache_ra_unbounded()`、`force_page_cache_ra()`、`generic_fadvise()`
> **前置知识**：[9.7 读写路径](07-read-write.md)、[9.8 page cache](08-page-cache.md)、4.1 folio

## 概述

逐页等待读取完成会把存储延迟直接暴露给应用。文件预读（File Readahead）根据访问历史，提前把尚未请求的数据读入 page cache，让应用处理当前数据时，存储设备准备后续数据。它必须在吞吐收益与无效 I/O、缓存占用之间权衡，因此预读范围不是固定常数，而是随访问模式变化的窗口。

本节分析普通文件的通用预读路径。预读策略保存在打开文件中，预读结果却进入同一 inode 的共享 page cache；文件系统再通过 `address_space_operations` 提交具体读取。入口与协议见 [`mm/readahead.c:11-114`](../../linux/mm/readahead.c#L11-L114)，文件映射触发预读的部分留到 [9.11 节](11-mmap.md)。

## 核心数据结构

### 每次打开的历史：struct file_ra_state

[`include/linux/fs.h:1164-1172`](../../linux/include/linux/fs.h#L1164-L1172)（v6.18.60）

```c
struct file_ra_state {
	pgoff_t start;
	unsigned int size;
	unsigned int async_size;
	unsigned int ra_pages;
	unsigned short order;
	unsigned short mmap_miss;
	loff_t prev_pos;
};
```

| 字段 | 本节中的含义 |
|---|---|
| `start` | 最近一次预读窗口的起始页索引，不是字节偏移 |
| `size` | 窗口包含的基础页数，不是 folio 数量 |
| `async_size` | 窗口中可提前获取、当前请求不急需的尾部页数 |
| `ra_pages` | 常规窗口上限，初始化时复制后备设备的信息；并非所有路径的绝对硬上限 |
| `order` | 预读期望采用的 folio 阶，仍受 mapping 能力、对齐和 EOF 限制 |
| `mmap_miss` | 文件映射预读使用的缓存未命中反馈 |
| `prev_pos` | 调用者维护的上次读取位置，单位为字节，用于近似判断连续性 |

该状态内嵌在 `struct file` 的 `f_ra` 中，见 [`include/linux/fs.h:1241-1249`](../../linux/include/linux/fs.h#L1241-L1249)。因此 `dup()` 后的描述符共享预读历史，独立 `open()` 则有各自的历史。初始化函数把 `bdi->ra_pages` 复制给打开文件，并把 `prev_pos` 设为 `-1`，见 [`mm/readahead.c:141-147`](../../linux/mm/readahead.c#L141-L147)。

### 一批读取的描述：struct readahead_control

`readahead_control` 把 `file`、`mapping`、`ra` 和一批连续页的游标组合起来；文件系统应使用 `readahead_folio()` 等访问器消费它，而不是直接修改私有游标。定义见 [`include/linux/pagemap.h:1314-1350`](../../linux/include/linux/pagemap.h#L1314-L1350)。

注意区分两个层面：`file_ra_state` 描述策略窗口，`readahead_control` 描述本次交付给文件系统的工作。窗口中已经有缓存的部分不需要再次读入，因此窗口大小不等于最终提交的 I/O 大小。

## 主流程分析

### 调用链总览

以下按缓冲读的两个触发条件分支；实际源码见 [`mm/filemap.c:2608-2679`](../../linux/mm/filemap.c#L2608-L2679) 与 [`mm/readahead.c:464-699`](../../linux/mm/readahead.c#L464-L699)。

```text
filemap_get_pages()                         mm/filemap.c
├── 缓存未命中：page_cache_sync_ra()         mm/readahead.c
│   ├── 强制读取：force_page_cache_ra()      mm/readahead.c
│   └── 常规窗口：page_cache_ra_order()      mm/readahead.c
└── 命中预读标记：filemap_readahead()        mm/filemap.c
    └── page_cache_async_ra()               mm/readahead.c
        └── page_cache_ra_order()           mm/readahead.c
            ├── 分配 folio、加入 page cache
            └── read_pages()               mm/readahead.c
                ├── a_ops->readahead()
                └── 无该回调时：a_ops->read_folio()
```

*图 9-10：缓冲读的预读触发与文件系统分派*

这里的“同步预读”指由当前缓存未命中触发，“异步预读”指消费旧窗口时提前触发下一窗口。两者都在触发者上下文执行策略和提交工作，**不表示一个必然等完全部 I/O、另一个由独立线程执行**；需要的数据是否就绪，由后续 folio 等待和读取路径判断。

### 1. 缓存未命中：建立窗口或仅满足当前请求

`filemap_get_pages()` 先尝试批量取得缓存；批次为空时，调用 `page_cache_sync_ra()`，然后重新查找。如果预读仍未提供数据，再进入单独创建 folio 的路径，见 [`mm/filemap.c:2641-2660`](../../linux/mm/filemap.c#L2641-L2660)。

`page_cache_sync_ra()` 依次处理三类情况，见 [`mm/readahead.c:554-628`](../../linux/mm/readahead.c#L554-L628)：

1. 设置了 `FMODE_RANDOM` 时采用强制路径，不建立推测性的顺序窗口。若 `ra_pages` 为零或块控制组拥塞，则把本次需求缩至一页后走强制路径；这不等于禁止读取当前必需的数据。
2. 从文件开头读取、请求大于常规上限，或当前页与 `prev_pos` 所在页相同或相邻时，按请求大小建立初始窗口。
3. 其余情况向前查询 page cache 的连续历史。历史不足时只读取当前请求，且不污染已有预读状态；历史足够长时才据此构建窗口。

第二类情况的核心代码如下。

[`mm/readahead.c:590-596`](../../linux/mm/readahead.c#L590-L596)（v6.18.60）

```c
	if (!index || req_count > max_pages || index - prev_index <= 1UL) {
		ra->start = index;
		ra->size = get_init_ra_size(req_count, max_pages);
		ra->async_size = ra->size > req_count ? ra->size - req_count :
							ra->size >> 1;
		goto readit;
	}
```

`get_init_ra_size()` 先把需求向上取整为 2 的幂，再根据它与上限的比例扩大或直接取上限；不是“所有首次读取都预读 128 KB”。初始化和后续增长公式分别见 [`mm/readahead.c:373-401`](../../linux/mm/readahead.c#L373-L401)。另外，`ractl_max_pages()` 在需求较大且 `bdi->io_pages` 更大时允许扩大本次上限，见 [`mm/readahead.c:539-552`](../../linux/mm/readahead.c#L539-L552)。

### 2. 命中标记：在窗口耗尽前推进

预读在窗口的异步部分起点附近设置 `PG_readahead`。应用读到带该标记的 folio 后，`page_cache_async_ra()` 检查 `ra_pages` 与写回状态，清除标记，再判断是否应继续预读。写回状态检查不能省略，因为 `PG_readahead` 与 `PG_reclaim` 复用位，见 [`mm/readahead.c:630-652`](../../linux/mm/readahead.c#L630-L652)。

如果触发索引等于按 folio 大小向下对齐后的预期位置 `start + size - async_size`，便认为顺序流仍在继续：新窗口从旧窗口末尾开始，并扩大 `size`。否则从 page cache 向后寻找空缺，尝试恢复被交错访问打乱的历史，见 [`mm/readahead.c:654-688`](../../linux/mm/readahead.c#L654-L688)。

以基础页为单位，假设上限为 32 页、首个请求为一页、没有其他缓存或大 folio 干扰：初始窗口可为 `[0, 4)`，第 1 页带预读标记。消费第 1 页时，下一窗口可以推进到 `[4, 12)`，其 `async_size` 等于窗口大小。此时应用仍在处理旧窗口，新的读取已经提交。这个例子只演示算法，不是实际运行结果。

### 3. 把窗口变成文件系统请求

`page_cache_ra_order()` 尝试按期望阶分配 folio，限制在 mapping 支持的阶范围，并处理索引对齐和文件尾边界。若 mapping 不支持大 folio，或分配、插入遇到问题，则由常规路径补齐尚未处理的范围，见 [`mm/readahead.c:464-537`](../../linux/mm/readahead.c#L464-L537)。

常规的 `page_cache_ra_unbounded()` 只分配尚未缓存的页；遇到已有页时先提交当前连续批次，再继续查找。分配期间使用禁止回收递归进入文件系统的上下文，并持有 `invalidate_lock` 读锁，见 [`mm/readahead.c:210-309`](../../linux/mm/readahead.c#L210-L309)。通常由 `do_page_cache_ra()` 先把范围限制到 EOF；`unbounded` 名称本身并不承诺这一检查。

最终由 `read_pages()` 调用 `a_ops->readahead()`，缺少该回调时逐个调用 `read_folio()`。文件系统没有消费的 folio 会被通用层移出缓存并解锁。预读失败本身不是应用的最终读错误：当前需求仍可经 `read_folio()` 重试，见 [`mm/readahead.c:149-180`](../../linux/mm/readahead.c#L149-L180)。

## 关键细节 / 设计考量

### 访问提示不是统一的“开关”

普通文件的 `generic_fadvise()` 行为见 [`mm/fadvise.c:31-185`](../../linux/mm/fadvise.c#L31-L185)。系统调用支持依赖 `CONFIG_ADVISE_SYSCALLS=y`，见 [`linux/.config:297`](../../linux/.config#L297)。

| 提示 | 该版本的通用实现 |
|---|---|
| `POSIX_FADV_NORMAL` | 恢复设备的 `ra_pages`，清除随机和不复用标志 |
| `POSIX_FADV_RANDOM` | 设置 `FMODE_RANDOM`，抑制同步未命中路径的推测扩张；不清空已有缓存或窗口标记 |
| `POSIX_FADV_SEQUENTIAL` | 把该打开文件的 `ra_pages` 设为设备值的两倍，清除随机标志 |
| `POSIX_FADV_WILLNEED` | 对给定范围请求强制预读，不保证整个范围一次驻留 |
| `POSIX_FADV_NOREUSE` | 设置 `FMODE_NOREUSE`，不等同于关闭预读 |
| `POSIX_FADV_DONTNEED` | 尝试启动范围写回并失效缓存；不会保证脏页立即被丢弃 |

文件系统可用自己的 `fadvise()` 覆盖通用实现；DAX 和使用 `noop_backing_dev_info` 的对象会忽略这些合法提示，见 [`mm/fadvise.c:51-65`](../../linux/mm/fadvise.c#L51-L65)。`readahead()` 系统调用也转到 `POSIX_FADV_WILLNEED`，见 [`mm/readahead.c:701-735`](../../linux/mm/readahead.c#L701-L735)。

### 共享缓存不等于共享预测状态

两个独立打开者共享缓存，却可能各自判断访问方向。预读代码因此既利用每个打开文件的历史，也查询 page cache 中留下的连续痕迹。共享一个 `struct file` 的多个流则可能相互覆盖状态；源码明确把标记与缓存扫描作为恢复线索，而不是保证每个流都被准确识别，见 [`mm/readahead.c:408-439`](../../linux/mm/readahead.c#L408-L439)。

## 可观测性

下列 tracepoint 依赖本配置的 `CONFIG_TRACEPOINTS=y`、`CONFIG_EVENT_TRACING=y`，见 [`linux/.config:320`](../../linux/.config#L320) 与 [`linux/.config:12937`](../../linux/.config#L12937)。使用 tracefs 通常需要管理员权限。

| 入口 | 看什么、怎样解释 | 源码依据 |
|---|---|---|
| `readahead:page_cache_sync_ra` | `index`、`req_count` 是本次需求；`size`、`async_size`、`prev_pos` 是进入函数时的状态，不应误当作更新后的窗口 | [`include/trace/events/readahead.h:78-121`](../../linux/include/trace/events/readahead.h#L78-L121)、[`mm/readahead.c:563`](../../linux/mm/readahead.c#L563) |
| `readahead:page_cache_async_ra` | 标记触发时的历史；结合设备号和 inode 区分同一文件的活动 | [`include/trace/events/readahead.h:123-127`](../../linux/include/trace/events/readahead.h#L123-L127)、[`mm/readahead.c:648`](../../linux/mm/readahead.c#L648) |
| `readahead:page_cache_ra_order` | 已选窗口的 `size`、`async_size`、期望 `order`；触发点仍早于实际阶限制和分配，不能据此认定分配成功 | [`include/trace/events/readahead.h:43-76`](../../linux/include/trace/events/readahead.h#L43-L76)、[`mm/readahead.c:478-490`](../../linux/mm/readahead.c#L478-L490) |
| `readahead:page_cache_ra_unbounded` | 常规预读请求的起点、页数与异步尾部；不是所有大 folio 路径都会经过它 | [`include/trace/events/readahead.h:14-41`](../../linux/include/trace/events/readahead.h#L14-L41) |
| `/sys/class/bdi/<设备名>/read_ahead_kb` | 设备的 `ra_pages` 换算成 KB；修改它不会自动更新已经打开文件的副本 | [`mm/backing-dev.c:260-287`](../../linux/mm/backing-dev.c#L260-L287)、[`mm/backing-dev.c:473-490`](../../linux/mm/backing-dev.c#L473-L490) |

上述 sysfs 接口依赖 `CONFIG_SYSFS=y`，见 [`linux/.config:11787`](../../linux/.config#L11787)。若使用动态探针，优先考虑实际的 `page_cache_sync_ra()`、`page_cache_async_ra()`，不要假定 `static inline` 的 `page_cache_sync_readahead()` 包装函数存在独立可探测符号；包装定义见 [`include/linux/pagemap.h:1373-1402`](../../linux/include/linux/pagemap.h#L1373-L1402)。

## 动手实验

在 `dev` 虚拟机内的本地磁盘文件系统上运行，安装 `trace-cmd` 与 Python 3。不要使用仓库共享挂载、tmpfs 或 ramfs，以免实际路径偏离本节。以下只失效实验文件的缓存，不修改全局预读参数或执行全局缓存回收。

```bash
work=$(mktemp -d /var/tmp/vfs-ra.XXXXXX)
dd if=/dev/zero of="$work/data" bs=1M count=32 conv=fsync status=none
for mode in NORMAL RANDOM SEQUENTIAL; do
    sudo trace-cmd record -o "$work/$mode.dat" -e readahead \
        python3 - "$work/data" "$mode" <<'PY'
import os, sys
fd = os.open(sys.argv[1], os.O_RDONLY)
os.posix_fadvise(fd, 0, 0, os.POSIX_FADV_DONTNEED)
os.posix_fadvise(fd, 0, 0, getattr(os, "POSIX_FADV_" + sys.argv[2]))
page = os.sysconf("SC_PAGE_SIZE")
for _ in range(256):
    if not os.read(fd, page):
        break
os.close(fd)
PY
done
sudo trace-cmd report -i "$work/NORMAL.dat" | head -60
# 对照 RANDOM.dat 与 SEQUENTIAL.dat；保存所需结果后删除整个 work 目录。
```

三次访问顺序相同，只改变访问提示，避免同时改变多个变量。观察 `NORMAL` 的窗口推进、`RANDOM` 对推测扩张的抑制，以及 `SEQUENTIAL` 的 `ra_pages`；不保证短序列一定增长到上限。实验可能夹杂其他进程事件，应按设备号和 inode 筛选；失效缓存也可能失败，因此必须结合实际事件解释，不能仅用耗时推断命中率。以上步骤尚未在本项目虚拟机中执行，不提供预设输出。

## 小结

- 预读结果属于共享 page cache，预测历史主要属于一次打开的 `struct file`。
- 同步与异步预读描述触发条件，不是阻塞 API 与后台线程的划分。
- `start`、`size`、`async_size` 决定窗口，预读标记让下一批读取提前开始。
- 窗口、实际分配的 folio 和最终存储 I/O 是三个不同层面的数量。
- 访问提示、设备参数和缓存历史共同影响策略，应结合 tracepoint 验证。

## 参考

- [`mm/readahead.c:11-114`](../../linux/mm/readahead.c#L11-L114)：内核自带的预读设计说明与文件系统回调协议。
- [`Documentation/core-api/mm-api.rst`](../../linux/Documentation/core-api/mm-api.rst)：内存管理 API 与预读接口文档入口。
- [`mm/fadvise.c`](../../linux/mm/fadvise.c)：文件访问提示的通用实现。
- [9.11 文件内存映射](11-mmap.md)：缺页触发预读及其与页表建立的关系。
