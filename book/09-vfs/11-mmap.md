# 9.11 文件内存映射：缺页、共享写与截断

> **本节源码**：[`mm/filemap.c`](../../linux/mm/filemap.c)、[`mm/memory.c`](../../linux/mm/memory.c)、[`mm/truncate.c`](../../linux/mm/truncate.c)、[`mm/msync.c`](../../linux/mm/msync.c)、[`fs/ramfs/file-mmu.c`](../../linux/fs/ramfs/file-mmu.c)
> **关键函数**：`generic_file_mmap_prepare()`、`filemap_fault()`、`filemap_map_pages()`、`filemap_page_mkwrite()`、`do_fault()`、`fault_dirty_shared_page()`、`truncate_setsize()`、`truncate_pagecache()`
> **前置知识**：4.7 进程地址空间、4.8 缺页异常、4.9 写时复制与反向映射、[9.8 page cache](08-page-cache.md)、[9.9 文件预读](09-readahead.md)、[9.10 写回与同步](10-writeback.md)

## 概述

文件内存映射（File-backed Memory Mapping）把文件内容放入进程虚拟地址空间，使应用可以通过普通内存访问读写数据。映射建立后，数据通常按访问需求进入 page cache，再由缺页处理建立页表映射，而不是在 `mmap()` 返回前把整个文件读完。这样既能复用文件缓存，也必须处理共享写、私有写时复制、文件截断与既有页表之间的一致性。

本节分析普通缓存文件的通用路径，不把 DAX、设备映射、hugetlbfs 或 tmpfs 的专用回调混进同一调用链。依赖 `CONFIG_MMU=y`，本配置已启用，见 [`linux/.config:353`](../../linux/.config#L353)；通用实现的配置分支见 [`mm/filemap.c:3205`](../../linux/mm/filemap.c#L3205) 与 [`mm/filemap.c:3976-3995`](../../linux/mm/filemap.c#L3976-L3995)。

## 核心数据结构

### 文件、VMA 与 mapping 的分工

| 对象或字段 | 在本节中的作用 | 源码 |
|---|---|---|
| `vm_area_struct->vm_file` | 持有打开文件引用，把 VMA 连接到文件与 mapping | [`include/linux/mm_types.h:874-876`](../../linux/include/linux/mm_types.h#L874-L876) |
| `vm_area_struct->vm_pgoff` | 映射起点对应的文件页索引，不能把虚拟地址直接当文件偏移 | [`include/linux/mm_types.h:874-876`](../../linux/include/linux/mm_types.h#L874-L876) |
| `vm_area_struct->vm_ops` | 缺页、批量映射、可写准备等回调表 | [`include/linux/mm.h:676-715`](../../linux/include/linux/mm.h#L676-L715) |
| `vm_fault->pgoff` | 当前缺页对应的文件页索引，`filemap_fault()` 据此查询缓存和 EOF | [`mm/filemap.c:3459-3473`](../../linux/mm/filemap.c#L3459-L3473) |
| `address_space->i_pages` | 文件数据 folio 的共享索引，不是进程页表 | [`include/linux/fs.h:506-526`](../../linux/include/linux/fs.h#L506-L526) |
| `address_space->i_mmap` | 文件映射的区间树，供反向查找映射范围、撤销页表等操作使用 | [`include/linux/fs.h:506-526`](../../linux/include/linux/fs.h#L506-L526) |
| `address_space->invalidate_lock` | 协调缓存填充与文件块映射失效；不能代替 folio 锁或页表锁 | [`include/linux/fs.h:506-526`](../../linux/include/linux/fs.h#L506-L526) |

对落在 VMA 中的虚拟地址，文件偏移可按“VMA 内偏移 + `vm_pgoff` 换算的字节数”理解。`msync()` 使用的换算可直接参照 [`mm/msync.c:90-98`](../../linux/mm/msync.c#L90-L98)。

### 通用文件映射操作表

[`mm/filemap.c:3929-3933`](../../linux/mm/filemap.c#L3929-L3933)（v6.18.60）

```c
const struct vm_operations_struct generic_file_vm_ops = {
	.fault		= filemap_fault,
	.map_pages	= filemap_map_pages,
	.page_mkwrite	= filemap_page_mkwrite,
};
```

`fault()` 获取需要的文件页；`map_pages()` 尝试顺带建立邻近页表映射；`page_mkwrite()` 在共享文件页变成可写前完成准备。它们都不是 `read()` 的用户缓冲区复制接口。

## 主流程分析

### 调用链总览

下面把建立映射与随后发生的缺页分开；通用回调安装见 [`mm/filemap.c:3937-3959`](../../linux/mm/filemap.c#L3937-L3959)，缺页分流见 [`mm/memory.c:5711-5878`](../../linux/mm/memory.c#L5711-L5878)。

```text
建立映射：文件系统 f_op->mmap_prepare()
└── 例如 generic_file_mmap_prepare()         mm/filemap.c
    └── 设置 generic_file_vm_ops

文件缺页：do_fault()                         mm/memory.c
├── 读：do_read_fault()                      mm/memory.c
│   ├── do_fault_around() → vm_ops->map_pages()
│   │   └── filemap_map_pages()              mm/filemap.c
│   └── __do_fault() → vm_ops->fault()
│       └── filemap_fault()                  mm/filemap.c
├── 私有写：do_cow_fault()                   mm/memory.c
│   └── 获取文件页，再复制到匿名页
└── 共享写：do_shared_fault()                mm/memory.c
    ├── __do_fault() → vm_ops->fault()
    ├── do_page_mkwrite() → vm_ops->page_mkwrite()
    └── fault_dirty_shared_page()           mm/memory.c
        └── 标脏并进行必要的脏页限速
```

*图 9-12：文件映射的建立与首次缺页分流*

这张图描述缺少页表项时的主要分支。已经映射为只读的共享文件页，在写保护异常时经 `do_wp_page()`、`wp_page_shared()` 进入相应可写准备，而不是重新走首次缺页分支，见 [`mm/memory.c:3886-3922`](../../linux/mm/memory.c#L3886-L3922) 与 [`mm/memory.c:4104-4124`](../../linux/mm/memory.c#L4104-L4124)。

### 1. 建立映射：安装操作表而非立即搬运全部数据

本版本既有接受 `vm_area_struct` 的 `generic_file_mmap()`，也有接受 `vm_area_desc` 的 `generic_file_mmap_prepare()`。二者检查 mapping 是否提供 `read_folio()`，更新访问状态，然后安装通用操作表，见 [`mm/filemap.c:3937-3959`](../../linux/mm/filemap.c#L3937-L3959)。

不要机械地假定所有文件系统都使用旧式 `f_op->mmap()`。本版本 ramfs 的 MMU 操作表就使用 `.mmap_prepare = generic_file_mmap_prepare`，见 [`fs/ramfs/file-mmu.c:41-50`](../../linux/fs/ramfs/file-mmu.c#L41-L50)。具体文件系统也可能替换缺页或共享写回调，以处理自己的块分配与日志事务。

### 2. 读缺页：查缓存、读入、验证，再交给页表层

`filemap_fault()` 的第一步是以 `i_size` 计算合法页索引范围。超出范围返回 `VM_FAULT_SIGBUS`，不是自动扩展文件，见 [`mm/filemap.c:3459-3473`](../../linux/mm/filemap.c#L3459-L3473)。之后分两种情况：

- **缓存命中**：尝试异步映射预读；若 folio 尚未就绪，则准备在 `invalidate_lock` 读锁保护下完成读取。
- **缓存未命中**：记录主缺页事件，尝试同步映射预读，再查找或创建 folio。预读没有提供目标数据时仍有单独读取的兜底路径。

这两条分支见 [`mm/filemap.c:3477-3520`](../../linux/mm/filemap.c#L3477-L3520)。映射预读还会考虑 `VM_RAND_READ`、`VM_SEQ_READ` 和 `mmap_miss` 等状态，不能照搬缓冲读的窗口推进方式，见 [`mm/filemap.c:3260-3392`](../../linux/mm/filemap.c#L3260-L3392)。

获得 folio 锁后，处理器必须重新验证 `folio->mapping`；如果它已被截断移出原 mapping，就重新查找。数据就绪后，还要在 folio 锁下再次检查 `i_size`，因为第一次检查与锁定之间可能发生截断。最后把 `vmf->page` 设为所需子页，并返回 `VM_FAULT_LOCKED`，见 [`mm/filemap.c:3522-3587`](../../linux/mm/filemap.c#L3522-L3587)。

`filemap_fault()` 获取文件页，但通常不独自完成 PTE 安装。上层缺页流程通过 `finish_fault()` 完成页表与反向映射记账。等待 I/O 时若释放了保护 VMA 的锁，则返回 `VM_FAULT_RETRY`，让上层重新查找和验证 VMA，而不是继续使用失去保护的旧指针，见 [`mm/filemap.c:3609-3627`](../../linux/mm/filemap.c#L3609-L3627)。

### 3. 批量建立页表不等于预读

`do_read_fault()` 优先尝试邻近页映射（Fault Around），再回退到 `fault()`。`filemap_map_pages()` 遍历附近已经就绪的缓存 folio，尝试一次安装多页映射，见 [`mm/memory.c:5711-5741`](../../linux/mm/memory.c#L5711-L5741) 与 [`mm/filemap.c:3818-3901`](../../linux/mm/filemap.c#L3818-L3901)。

两种优化跨越的是不同边界：预读把数据从文件系统送入 page cache，批量映射把已有 page cache 页接入进程页表。数据已经在缓存中仍可能缺 PTE；一次缺页也可能顺带建立多个 PTE，所以不能把缺页次数当作读取页数或设备 I/O 次数。

### 4. 私有写：文件是初始数据源，不是修改目标

私有映射（Private Mapping）对未映射页发生写缺页时，`do_cow_fault()` 准备匿名 folio，调用文件 `fault()` 取得原数据，然后复制并安装私有页，见 [`mm/memory.c:5743-5783`](../../linux/mm/memory.c#L5743-L5783)。若此前已经读映射了文件页，后续写保护异常由相应 COW 路径处理。

因此 `MAP_PRIVATE` 可以共享初始的文件缓存，但写入后的私有匿名页不属于该文件的普通 page cache 写回。对原文件调用 `fsync()` 不会把这份私有修改保存成文件内容。

### 5. 共享写：先让文件系统准备，再允许用户态写入

共享映射（Shared Mapping）的 `do_shared_fault()` 获取文件页后，若有 `page_mkwrite()`，先调用它，再完成页表安装和标脏。文件系统可以在这个阶段分配块、处理日志或拒绝写入，见 [`mm/memory.c:5785-5825`](../../linux/mm/memory.c#L5785-L5825)。

通用的 `filemap_page_mkwrite()` 协调文件系统冻结，更新时间，锁定 folio 并复核 mapping，然后提前标脏并等待需要的稳定写条件，见 [`mm/filemap.c:3903-3927`](../../linux/mm/filemap.c#L3903-L3927)。它只是一个通用实现，不能认为 ext4 等文件系统的所有共享写都直接调用它。

`fault_dirty_shared_page()` 最终把 folio 标脏，并在合适条件下调用脏页限速。它在解锁前保存 mapping 指针，必要时释放 VMA 相关锁后等待 I/O，见 [`mm/memory.c:3525-3571`](../../linux/mm/memory.c#L3525-L3571)。与 [9.10 节](10-writeback.md) 对照可知：共享映射写不绕过写回，也不保证永远不会因存储压力阻塞。

### 6. 截断：既要删除缓存，也要撤销页表

`truncate_setsize()` 先更新 `i_size`，必要时处理扩展，再调用 `truncate_pagecache()`，见 [`mm/truncate.c:804-825`](../../linux/mm/truncate.c#L804-L825)。缩短文件不只改变 inode 中的一个数字：旧的文件页和用户页表必须同步失效。

[`mm/truncate.c:783-800`](../../linux/mm/truncate.c#L783-L800)（v6.18.60）

```c
void truncate_pagecache(struct inode *inode, loff_t newsize)
{
	struct address_space *mapping = inode->i_mapping;
	loff_t holebegin = round_up(newsize, PAGE_SIZE);
...
	unmap_mapping_range(mapping, holebegin, 0, 1);
	truncate_inode_pages(mapping, newsize);
	unmap_mapping_range(mapping, holebegin, 0, 1);
}
```

第一次撤销映射减少后续逐页处理的工作；但在缓存截断期间仍可能产生私有 COW 页，所以第二次撤销映射是正确性要求，不能简单当作冗余扫描。调用中的 `even_cows=1` 也说明越过新 EOF 的私有副本不能依赖旧映射继续使用。源码解释见 [`mm/truncate.c:788-799`](../../linux/mm/truncate.c#L788-L799)。

## 关键细节 / 设计考量

### 锁的责任边界

- **VMA 相关锁**保证地址空间布局与 VMA 的生命周期，不直接保证文件大小不变。
- **`invalidate_lock`**协调缓存填充与块映射失效。需要改变磁盘块布局的文件系统通常在截断或打洞时持写锁；例如 ext4 缩短文件的处理见 [`fs/ext4/inode.c:6009-6052`](../../linux/fs/ext4/inode.c#L6009-L6052)。这不能反推所有内存文件系统的通用截断助手都会自行获取该锁。
- **folio 锁**保护读取、共享写准备和缓存删除的关键状态，取得锁后仍须验证 mapping 与 EOF。
- **页表锁**序列化 PTE 更新；page cache 命中不意味着可以无锁安装页表。

这些锁各有作用域。文件系统自己的截断入口还必须序列化写入与大小变化；`truncate_setsize()` 对调用者锁约束的说明见 [`mm/truncate.c:804-815`](../../linux/mm/truncate.c#L804-L815)。

### EOF、munmap 与同步

EOF 检查以页为边界：最后一个不完整页仍可能被映射，不能把页内尾部与整页超出 EOF 混为一谈。应用不能靠向映射末尾写入来扩展文件，应先通过文件接口设置所需长度；整页超出 EOF 的缺页由 `filemap_fault()` 拒绝。

`munmap()` 撤销地址映射不是同步持久化操作。对共享文件映射，`msync(MS_SYNC)` 把虚拟范围换算为文件范围，并调用 `vfs_fsync_range(..., datasync=1)`；该版本 `MS_ASYNC` 不主动启动写回 I/O，见 [`mm/msync.c:18-30`](../../linux/mm/msync.c#L18-L30) 与 [`mm/msync.c:88-114`](../../linux/mm/msync.c#L88-L114)。

## 可观测性

事件依赖本配置的 `CONFIG_TRACEPOINTS=y`、`CONFIG_EVENT_TRACING=y`，见 [`linux/.config:320`](../../linux/.config#L320)、[`linux/.config:12937`](../../linux/.config#L12937)。`/proc` 依赖 `CONFIG_PROC_FS=y`，`smaps` 还依赖 `CONFIG_PROC_PAGE_MONITOR=y`，见 [`linux/.config:11777-11782`](../../linux/.config#L11777-L11782)。

| 入口 | 看什么、怎样解释 | 源码依据 |
|---|---|---|
| `filemap:mm_filemap_fault` | 设备号、inode、页索引；记录进入文件页缺页处理的活动，不等于主缺页或设备读取 | [`include/trace/events/filemap.h:114-142`](../../linux/include/trace/events/filemap.h#L114-L142)、[`mm/filemap.c:3475`](../../linux/mm/filemap.c#L3475) |
| `filemap:mm_filemap_map_pages` | 邻近页映射处理范围；输出按索引换算为字节范围，不是实际安装页数的完成计数 | [`include/trace/events/filemap.h:61-112`](../../linux/include/trace/events/filemap.h#L61-L112)、[`mm/filemap.c:3889`](../../linux/mm/filemap.c#L3889) |
| `/proc/<pid>/maps` | 地址范围、权限、文件偏移和路径；只能证明映射存在，不能证明全部驻留 | [`fs/proc/task_mmu.c:463-499`](../../linux/fs/proc/task_mmu.c#L463-L499) |
| `/proc/<pid>/smaps` | `Rss`、`Anonymous`、`Shared_Dirty`、`Private_Dirty` 等；脏页分类依据当前映射共享程度，不是简单对应 `MAP_SHARED` / `MAP_PRIVATE` 参数 | [`fs/proc/task_mmu.c:886-990`](../../linux/fs/proc/task_mmu.c#L886-L990)、[`fs/proc/task_mmu.c:1331-1370`](../../linux/fs/proc/task_mmu.c#L1331-L1370) |
| `/proc/<pid>/stat` 的 `minflt`、`majflt` | 任务次缺页与主缺页累计计数；包含其他地址空间访问，不只实验文件 | [`fs/proc/array.c:598-601`](../../linux/fs/proc/array.c#L598-L601)、[`mm/memory.c:6380-6412`](../../linux/mm/memory.c#L6380-L6412) |
| 动态探针 `filemap_page_mkwrite()` | 通用共享写准备；只覆盖使用此回调的文件系统，需要先核对实际 `vm_ops` | [`mm/filemap.c:3903-3933`](../../linux/mm/filemap.c#L3903-L3933) |

探针支持依赖本配置的 `CONFIG_KPROBES=y`、`CONFIG_KPROBE_EVENTS=y`，见 [`linux/.config:881`](../../linux/.config#L881)、[`linux/.config:12970`](../../linux/.config#L12970)。实际函数能否探测仍应检查运行内核的符号和可探测列表，不能以源码存在代替验证。

## 动手实验

以下 Python 3 程序在虚拟机本地临时目录中创建两页文件，验证共享写、私有写与截断边界。它在子进程中故意触发一次 `SIGBUS`，先禁用 core dump，且不影响其他文件。应在隔离实验环境中执行。

```bash
python3 - <<'PY'
import mmap, os, resource, signal, tempfile
resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
page = os.sysconf("SC_PAGE_SIZE")
with tempfile.TemporaryFile(dir="/var/tmp") as f:
    f.write(b"A" * (2 * page))
    f.flush()
    shared = mmap.mmap(f.fileno(), 2 * page, flags=mmap.MAP_SHARED,
                       prot=mmap.PROT_READ | mmap.PROT_WRITE)
    private = mmap.mmap(f.fileno(), 2 * page, flags=mmap.MAP_PRIVATE,
                        prot=mmap.PROT_READ | mmap.PROT_WRITE)
    shared[0] = ord("S")
    private[0] = ord("P")
    print("file/private:", os.pread(f.fileno(), 1, 0), private[:1], flush=True)
    shared.flush()
    os.ftruncate(f.fileno(), page)
    pid = os.fork()
    if pid == 0:
        _ = shared[page]
        os._exit(0)
    _, status = os.waitpid(pid, 0)
    print("child got SIGBUS:", os.WIFSIGNALED(status) and
          os.WTERMSIG(status) == signal.SIGBUS)
    private.close()
    shared.close()
PY
```

预期文件读取得到 `S`，私有映射得到 `P`；截断后访问原第二页的子进程被 `SIGBUS` 终止。这是源码推导的预期，不是已采集的运行输出。若增加 `trace-cmd` 记录 `filemap:mm_filemap_fault` 与 `filemap:mm_filemap_map_pages`，注意创建文件后数据已在缓存中，不能据此实验声称发生了磁盘主缺页。以上步骤尚未在项目虚拟机执行。

## 小结

- 文件映射复用 page cache，但缓存、VMA 与页表属于不同层次。
- `filemap_fault()` 获取并验证文件页，上层缺页代码负责通常的页表安装。
- 邻近页映射减少缺页次数，预读减少等待，两者不能用同一计数衡量。
- 私有写产生匿名副本，共享写则进入文件系统的可写准备与脏页协议。
- 截断必须协调文件大小、缓存和既有页表；两次撤销映射有不同目的。
- 撤销映射不代替同步，持久化仍须使用合适的文件同步接口。

## 参考

- [`Documentation/mm/page_cache.rst`](../../linux/Documentation/mm/page_cache.rst)：page cache 与文件访问的关系。
- [`Documentation/mm/page_tables.rst`](../../linux/Documentation/mm/page_tables.rst)：页表层次与缺页背景。
- [`Documentation/filesystems/locking.rst`](../../linux/Documentation/filesystems/locking.rst)：文件系统与地址空间操作的锁约定。
- [`mm/msync.c`](../../linux/mm/msync.c)：映射范围同步的实际实现。
- [9.12 目录操作与 ramfs](12-directory-ramfs.md)：使用通用文件映射接口的最小内存文件系统。
