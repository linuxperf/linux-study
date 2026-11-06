# 9.7 读写路径：kiocb、iov_iter 与缓冲 I/O

> **本节源码**：[`fs/read_write.c`](../../linux/fs/read_write.c)、[`include/linux/fs.h`](../../linux/include/linux/fs.h)、[`include/linux/uio.h`](../../linux/include/linux/uio.h)、[`lib/iov_iter.c`](../../linux/lib/iov_iter.c)、[`mm/filemap.c`](../../linux/mm/filemap.c)、[`fs/libfs.c`](../../linux/fs/libfs.c)
> **关键函数**：`ksys_write()`、`vfs_write()`、`new_sync_write()`、`vfs_readv()`、`do_iter_readv_writev()`、`file_start_write()`、`generic_file_read_iter()`、`generic_file_write_iter()`、`generic_write_checks()`、`__generic_file_write_iter()`、`generic_perform_write()`、`simple_write_begin()`、`simple_write_end()`、`generic_write_sync()`、`copy_page_to_iter()`
> **前置知识**：9.1 VFS 概述、9.6 文件打开、文件描述符与 struct file

## 概述

9.1 节以 `read()` 为例展示了系统调用如何经 `vfs_read()` 分派到文件系统的 `->read_iter`。本节深入这条路径的两端：VFS 层如何把各种形式的读写请求（`read`/`write`、`readv`/`writev`、`pread`/`pwrite`、`preadv2`/`pwritev2`、io_uring、AIO、内核内部读写）统一为一个接口；文件系统层的通用实现如何完成**缓冲 I/O（Buffered I/O）**——读时从 page cache 拷出数据，写时把数据拷入 page cache 并标记为脏，然后立即返回。

统一的关键是两个结构：

- **`struct kiocb`**（kernel I/O control block）：描述一次 I/O 的"上下文"——哪个文件、从哪个位置开始、带什么标志、完成时回调谁。
- **`struct iov_iter`**：描述数据的"来源或去向"——它可以是一个用户缓冲区、一组 iovec、一组内核 kvec、一组 `bio_vec`、一组 folio……文件系统只需调用 `copy_to_iter()`、`copy_from_iter()` 等函数，无需关心数据实际在哪里。

缓冲写入在拷贝完成后就返回成功，此时数据只在内存中。数据何时到达磁盘由脏页写回机制决定（9.10 节）；需要立即持久化的程序要使用 `fsync()` 或 `O_SYNC`。与之对应的**直接 I/O（Direct I/O，`O_DIRECT`）** 绕过 page cache，在用户缓冲区与设备之间直接传输，本节只说明它与 page cache 的交互，块层部分在第 10 章分析。

## 核心数据结构

### struct kiocb

[`include/linux/fs.h:407-432`](../../linux/include/linux/fs.h#L407-L432)（v6.18.60）

```c
struct kiocb {
	struct file		*ki_filp;
	loff_t			ki_pos;
	void (*ki_complete)(struct kiocb *iocb, long ret);
	void			*private;
	int			ki_flags;
	u16			ki_ioprio; /* See linux/ioprio.h */
	u8			ki_write_stream;
	union {
		/*
		 * Only used for async buffered reads, where it denotes the
		 * page waitqueue associated with completing the read. Valid
		 * IFF IOCB_WAITQ is set.
		 */
		struct wait_page_queue	*ki_waitq;
		/*
		 * Can be used for O_DIRECT IO, where the completion handling
		 * is punted back to the issuer of the IO. May only be set
		 * if IOCB_DIO_CALLER_COMP is set by the issuer, and the issuer
		 * must then check for presence of this handler when ki_complete
		 * is invoked. The data passed in to this handler must be
		 * assigned to ->private when dio_complete is assigned.
		 */
		ssize_t (*dio_complete)(void *data);
	};
};
```

- `ki_filp`、`ki_pos`：文件与本次 I/O 的起始位置。文件系统完成 I/O 后负责推进 `ki_pos`，调用者再把它写回 `f_pos`（或丢弃，对于 `pread`）。
- `ki_complete`：异步完成回调。为 `NULL` 时是同步 I/O（`is_sync_kiocb()`），文件系统必须在返回前完成；非空时（io_uring、AIO）文件系统可以返回 `-EIOCBQUEUED` 表示已提交、稍后通过回调报告结果。
- `ki_flags`：`IOCB_*` 标志，一部分由打开标志预先计算（`file->f_iocb_flags`，9.6 节），一部分来自 `preadv2()`/`pwritev2()` 的 `RWF_*` 参数：

[`include/linux/fs.h:351-369`](../../linux/include/linux/fs.h#L351-L369)（v6.18.60）

```c
/* Match RWF_* bits to IOCB bits */
#define IOCB_HIPRI		(__force int) RWF_HIPRI
#define IOCB_DSYNC		(__force int) RWF_DSYNC
#define IOCB_SYNC		(__force int) RWF_SYNC
#define IOCB_NOWAIT		(__force int) RWF_NOWAIT
#define IOCB_APPEND		(__force int) RWF_APPEND
#define IOCB_ATOMIC		(__force int) RWF_ATOMIC
#define IOCB_DONTCACHE		(__force int) RWF_DONTCACHE
#define IOCB_NOSIGNAL		(__force int) RWF_NOSIGNAL

/* non-RWF related bits - start at 16 */
#define IOCB_EVENTFD		(1 << 16)
#define IOCB_DIRECT		(1 << 17)
#define IOCB_WRITE		(1 << 18)
/* iocb->ki_waitq is valid */
#define IOCB_WAITQ		(1 << 19)
#define IOCB_NOIO		(1 << 20)
/* can use bio alloc cache */
#define IOCB_ALLOC_CACHE	(1 << 21)
```

| 标志 | 含义 |
|---|---|
| `IOCB_APPEND` | 追加写，写入位置由文件系统设为文件末尾 |
| `IOCB_DIRECT` | 直接 I/O |
| `IOCB_DSYNC` / `IOCB_SYNC` | 写完后同步数据（及元数据） |
| `IOCB_NOWAIT` | 不允许阻塞，需要阻塞时返回 `-EAGAIN`（io_uring 先用它尝试内联执行） |
| `IOCB_NOIO` | 不允许发起新的 I/O，只读取已缓存的数据 |
| `IOCB_WAITQ` | 缓冲读的异步模式：folio 未就绪时不睡眠，而是登记一个等待项，就绪后回调 |
| `IOCB_DONTCACHE` | 读写完成后尽快从 page cache 中丢弃这些页（v6.14 引入的 `RWF_DONTCACHE`） |
| `IOCB_HIPRI` | 高优先级，配合轮询完成 |

同步 I/O 用 `init_sync_kiocb()` 初始化 kiocb（[`include/linux/fs.h:2594-2601`](../../linux/include/linux/fs.h#L2594-L2601)）。

### struct iov_iter

[`include/linux/uio.h:43-83`](../../linux/include/linux/uio.h#L43-L83)（v6.18.60）

```c
struct iov_iter {
	u8 iter_type;
	bool nofault;
	bool data_source;
	size_t iov_offset;
	/*
	 * Hack alert: overlay ubuf_iovec with iovec + count, so
	 * that the members resolve correctly regardless of the type
	 * of iterator used. This means that you can use:
	 *
	 * &iter->__ubuf_iovec or iter->__iov
	 *
	 * interchangably for the user_backed cases, hence simplifying
	 * some of the cases that need to deal with both.
	 */
	union {
		/*
		 * This really should be a const, but we cannot do that without
		 * also modifying any of the zero-filling iter init functions.
		 * Leave it non-const for now, but it should be treated as such.
		 */
		struct iovec __ubuf_iovec;
		struct {
			union {
				/* use iter_iov() to get the current vec */
				const struct iovec *__iov;
				const struct kvec *kvec;
				const struct bio_vec *bvec;
				const struct folio_queue *folioq;
				struct xarray *xarray;
				void __user *ubuf;
			};
			size_t count;
		};
	};
	union {
		unsigned long nr_segs;
		u8 folioq_slot;
		loff_t xarray_start;
	};
};
```

| 类型 | 数据所在 | 典型来源 |
|---|---|---|
| `ITER_UBUF` | 单个用户缓冲区 | `read()`、`write()`（v6.0 引入，比单段的 `ITER_IOVEC` 少一层间接） |
| `ITER_IOVEC` | 用户空间的 iovec 数组 | `readv()`、`writev()` |
| `ITER_KVEC` | 内核空间的 kvec 数组 | `kernel_read()`、`kernel_write()` |
| `ITER_BVEC` | `bio_vec` 数组（页 + 偏移 + 长度） | 网络栈的 splice、loop 设备、io_uring 固定缓冲区 |
| `ITER_FOLIOQ` / `ITER_XARRAY` | folio 队列 / xarray 中的页 | 网络文件系统（netfs） |
| `ITER_DISCARD` | 丢弃 | 只推进计数，不拷贝数据 |

迭代器类型由 `enum iter_type`（[`include/linux/uio.h:23-32`](../../linux/include/linux/uio.h#L23-L32)）定义，`data_source` 指明方向：`ITER_SOURCE`（数据从 iter 流出，用于写）或 `ITER_DEST`（数据流入 iter，用于读）。`count` 是剩余字节数，`iov_offset` 是当前段内的偏移。迭代器在拷贝过程中自动前进，`iov_iter_revert()` 可以回退。

## 主流程分析

### 写入调用链

```
write(fd, buf, count)
└── ksys_write()                             fs/read_write.c:727
    └── vfs_write()                          fs/read_write.c:666
        ├── rw_verify_area(WRITE)
        ├── file_start_write()               // 冻结保护：sb_start_write()
        ├── new_sync_write()                 fs/read_write.c:583
        │   └── f_op->write_iter()           // 如 generic_file_write_iter()
        │       ├── inode_lock()             // i_rwsem 独占
        │       ├── generic_write_checks()   // O_APPEND、文件大小限制
        │       ├── __generic_file_write_iter()
        │       │   ├── file_remove_privs()  // 清除 suid/sgid
        │       │   ├── file_update_time()   // 更新 mtime/ctime
        │       │   ├── generic_file_direct_write()   // O_DIRECT
        │       │   └── generic_perform_write()       // 缓冲写
        │       │       └── 循环:
        │       │           ├── balance_dirty_pages_ratelimited()  // 9.10 节
        │       │           ├── a_ops->write_begin()   // 获取并锁定 folio
        │       │           ├── copy_folio_from_iter_atomic()
        │       │           └── a_ops->write_end()     // 标脏、更新 i_size
        │       ├── inode_unlock()
        │       └── generic_write_sync()     // O_SYNC/O_DSYNC：vfs_fsync_range()
        └── file_end_write()
```

### vfs_write() 与冻结保护

[`fs/read_write.c:666-696`](../../linux/fs/read_write.c#L666-L696)（v6.18.60）

```c
ssize_t vfs_write(struct file *file, const char __user *buf, size_t count, loff_t *pos)
{
	ssize_t ret;

	if (!(file->f_mode & FMODE_WRITE))
		return -EBADF;
	if (!(file->f_mode & FMODE_CAN_WRITE))
		return -EINVAL;
	if (unlikely(!access_ok(buf, count)))
		return -EFAULT;

	ret = rw_verify_area(WRITE, file, pos, count);
	if (ret)
		return ret;
	if (count > MAX_RW_COUNT)
		count =  MAX_RW_COUNT;
	file_start_write(file);
	if (file->f_op->write)
		ret = file->f_op->write(file, buf, count, pos);
	else if (file->f_op->write_iter)
		ret = new_sync_write(file, buf, count, pos);
	else
		ret = -EINVAL;
	if (ret > 0) {
		fsnotify_modify(file);
		add_wchar(current, ret);
	}
	inc_syscw(current);
	file_end_write(file);
	return ret;
}
```

与 `vfs_read()` 相比，`vfs_write()` 多了一对 `file_start_write()` / `file_end_write()`：

[`include/linux/fs.h:3131-3136`](../../linux/include/linux/fs.h#L3131-L3136)（v6.18.60）

```c
static inline void file_start_write(struct file *file)
{
	if (!S_ISREG(file_inode(file)->i_mode))
		return;
	sb_start_write(file_inode(file)->i_sb);
}
```

它对普通文件调用 `sb_start_write()`，获取超级块的**冻结保护**。文件系统冻结（`fsfreeze -f`，LVM 快照、虚拟机备份时使用）需要保证冻结后不再有新的修改。`sb->s_writers` 中有三级 per-CPU 读写信号量：`SB_FREEZE_WRITE`（普通写入）、`SB_FREEZE_PAGEFAULT`（共享映射上的写缺页）、`SB_FREEZE_FS`（文件系统内部操作，如日志）。写者以读方式获取，冻结者以写方式获取——per-CPU 读写信号量使得写者的开销几乎只是一次 per-CPU 计数（7.7 节）。冻结期间新的写入在此睡眠，直到解冻。

### 向量 I/O 与 preadv2

`readv()`/`writev()` 先用 `import_iovec()` 把用户的 iovec 数组拷入内核并构造 `ITER_IOVEC` 迭代器，再统一交给 `->read_iter`/`->write_iter`；只有旧式的 `->read`/`->write` 才需要逐段循环：

[`fs/read_write.c:990-1026`](../../linux/fs/read_write.c#L990-L1026)（v6.18.60）

```c
static ssize_t vfs_readv(struct file *file, const struct iovec __user *vec,
			 unsigned long vlen, loff_t *pos, rwf_t flags)
{
	struct iovec iovstack[UIO_FASTIOV];
	struct iovec *iov = iovstack;
	struct iov_iter iter;
	size_t tot_len;
	ssize_t ret = 0;

	if (!(file->f_mode & FMODE_READ))
		return -EBADF;
	if (!(file->f_mode & FMODE_CAN_READ))
		return -EINVAL;

	ret = import_iovec(ITER_DEST, vec, vlen, ARRAY_SIZE(iovstack), &iov,
			   &iter);
	if (ret < 0)
		return ret;

	tot_len = iov_iter_count(&iter);
	if (!tot_len)
		goto out;

	ret = rw_verify_area(READ, file, pos, tot_len);
	if (ret < 0)
		goto out;

	if (file->f_op->read_iter)
		ret = do_iter_readv_writev(file, &iter, pos, READ, flags);
	else
		ret = do_loop_readv_writev(file, &iter, pos, READ, flags);
out:
	if (ret >= 0)
		fsnotify_access(file);
	kfree(iov);
	return ret;
}
```

[`fs/read_write.c:812-832`](../../linux/fs/read_write.c#L812-L832)（v6.18.60）

```c
static ssize_t do_iter_readv_writev(struct file *filp, struct iov_iter *iter,
		loff_t *ppos, int type, rwf_t flags)
{
	struct kiocb kiocb;
	ssize_t ret;

	init_sync_kiocb(&kiocb, filp);
	ret = kiocb_set_rw_flags(&kiocb, flags, type);
	if (ret)
		return ret;
	kiocb.ki_pos = (ppos ? *ppos : 0);

	if (type == READ)
		ret = filp->f_op->read_iter(&kiocb, iter);
	else
		ret = filp->f_op->write_iter(&kiocb, iter);
	BUG_ON(ret == -EIOCBQUEUED);
	if (ppos)
		*ppos = kiocb.ki_pos;
	return ret;
}
```

`preadv2()`/`pwritev2()` 的 `flags` 参数经 `kiocb_set_rw_flags()` 转换为 `IOCB_*` 标志，使应用可以针对单次调用要求 `RWF_NOWAIT`、`RWF_DSYNC`、`RWF_APPEND` 等语义，而不必修改文件的打开标志。

### generic_file_write_iter()

大多数基于 page cache 的文件系统使用（或包装）`generic_file_write_iter()`：

[`mm/filemap.c:4406-4422`](../../linux/mm/filemap.c#L4406-L4422)（v6.18.60）

```c
ssize_t generic_file_write_iter(struct kiocb *iocb, struct iov_iter *from)
{
	struct file *file = iocb->ki_filp;
	struct inode *inode = file->f_mapping->host;
	ssize_t ret;

	inode_lock(inode);
	ret = generic_write_checks(iocb, from);
	if (ret > 0)
		ret = __generic_file_write_iter(iocb, from);
	inode_unlock(inode);

	if (ret > 0)
		ret = generic_write_sync(iocb, ret);
	return ret;
}
EXPORT_SYMBOL(generic_file_write_iter);
```

写入在 inode 的 `i_rwsem` **独占**保护下进行。这保证了同一文件的并发写入相互串行，也保证了 `O_APPEND` 的原子性——`generic_write_checks()` 在锁内把写入位置设为当前文件大小：

[`fs/read_write.c:1736-1778`](../../linux/fs/read_write.c#L1736-L1778)（v6.18.60）

```c
int generic_write_checks_count(struct kiocb *iocb, loff_t *count)
{
	struct file *file = iocb->ki_filp;
	struct inode *inode = file->f_mapping->host;

	if (IS_SWAPFILE(inode))
		return -ETXTBSY;

	if (!*count)
		return 0;

	if (iocb->ki_flags & IOCB_APPEND)
		iocb->ki_pos = i_size_read(inode);

	if ((iocb->ki_flags & IOCB_NOWAIT) &&
	    !((iocb->ki_flags & IOCB_DIRECT) ||
	      (file->f_op->fop_flags & FOP_BUFFER_WASYNC)))
		return -EINVAL;

	return generic_write_check_limits(iocb->ki_filp, iocb->ki_pos, count);
}
EXPORT_SYMBOL(generic_write_checks_count);

/*
 * Performs necessary checks before doing a write
 *
 * Can adjust writing position or amount of bytes to write.
 * Returns appropriate error code that caller should return or
 * zero in case that write should be allowed.
 */
ssize_t generic_write_checks(struct kiocb *iocb, struct iov_iter *from)
{
	loff_t count = iov_iter_count(from);
	int ret;

	ret = generic_write_checks_count(iocb, &count);
	if (ret)
		return ret;

	iov_iter_truncate(from, count);
	return iov_iter_count(from);
}
EXPORT_SYMBOL(generic_write_checks);
```

`generic_write_check_limits()`（[`fs/read_write.c:1709-1733`](../../linux/fs/read_write.c#L1709-L1733)）还会根据进程的 `RLIMIT_FSIZE`（超出时发送 `SIGXFSZ`）与文件系统的 `s_maxbytes` 截短写入长度。`IOCB_NOWAIT` 只在直接 I/O 或文件系统声明支持异步缓冲写（`FOP_BUFFER_WASYNC`）时才被接受。

### __generic_file_write_iter()

[`mm/filemap.c:4359-4391`](../../linux/mm/filemap.c#L4359-L4391)（v6.18.60）

```c
ssize_t __generic_file_write_iter(struct kiocb *iocb, struct iov_iter *from)
{
	struct file *file = iocb->ki_filp;
	struct address_space *mapping = file->f_mapping;
	struct inode *inode = mapping->host;
	ssize_t ret;

	ret = file_remove_privs(file);
	if (ret)
		return ret;

	ret = file_update_time(file);
	if (ret)
		return ret;

	if (iocb->ki_flags & IOCB_DIRECT) {
		ret = generic_file_direct_write(iocb, from);
		/*
		 * If the write stopped short of completing, fall back to
		 * buffered writes.  Some filesystems do this for writes to
		 * holes, for example.  For DAX files, a buffered write will
		 * not succeed (even if it did, DAX does not handle dirty
		 * page-cache pages correctly).
		 */
		if (ret < 0 || !iov_iter_count(from) || IS_DAX(inode))
			return ret;
		return direct_write_fallback(iocb, from, ret,
				generic_perform_write(iocb, from));
	}

	return generic_perform_write(iocb, from);
}
EXPORT_SYMBOL(__generic_file_write_iter);
```

写入前的两项通用处理：

- `file_remove_privs()`：非特权用户写入带 setuid / setgid 位的文件时清除这些位，防止通过修改特权程序的内容获得权限。同时移除安全相关的扩展属性（如 `security.capability`）。
- `file_update_time()`：更新 mtime 与 ctime。若当前时间与已记录的时间戳相同（时间粒度内的多次写入）就跳过（[`fs/inode.c:2334-2346`](../../linux/fs/inode.c#L2334-L2346)），避免每次写入都弄脏 inode。

然后根据 `IOCB_DIRECT` 选择直接写或缓冲写。直接写若因文件空洞等原因只完成了一部分，剩余部分回退为缓冲写（`direct_write_fallback()`）。

### generic_perform_write()：缓冲写的核心

缓冲写把数据以 folio 为单位拷入 page cache。每轮循环处理一个 folio：

[`mm/filemap.c:4254-4294`](../../linux/mm/filemap.c#L4254-L4294)（v6.18.60）

```c
	do {
		struct folio *folio;
		size_t offset;		/* Offset into folio */
		size_t bytes;		/* Bytes to write to folio */
		size_t copied;		/* Bytes copied from user */
		void *fsdata = NULL;

		bytes = iov_iter_count(i);
retry:
		offset = pos & (chunk - 1);
		bytes = min(chunk - offset, bytes);
		balance_dirty_pages_ratelimited(mapping);

		if (fatal_signal_pending(current)) {
			status = -EINTR;
			break;
		}

		status = a_ops->write_begin(iocb, mapping, pos, bytes,
						&folio, &fsdata);
		if (unlikely(status < 0))
			break;

		offset = offset_in_folio(folio, pos);
		if (bytes > folio_size(folio) - offset)
			bytes = folio_size(folio) - offset;

		if (mapping_writably_mapped(mapping))
			flush_dcache_folio(folio);

		/*
		 * Faults here on mmap()s can recurse into arbitrary
		 * filesystem code. Lots of locks are held that can
		 * deadlock. Use an atomic copy to avoid deadlocking
		 * in page fault handling.
		 */
		copied = copy_folio_from_iter_atomic(folio, offset, bytes, i);
		flush_dcache_folio(folio);

		status = a_ops->write_end(iocb, mapping, pos, bytes, copied,
						folio, fsdata);
```

每一轮的步骤：

1. `balance_dirty_pages_ratelimited()`：在弄脏更多页之前检查系统的脏页水平，必要时让当前任务睡眠以限制产生脏页的速度（9.10 节）。这是缓冲写的"背压"机制。
2. `->write_begin`：文件系统找到（或创建）该位置的 folio，将其锁定并返回。磁盘文件系统在这里为写入准备好存储：可能要从磁盘读入整页（部分覆盖写时，folio 中未被覆盖的部分必须是正确的旧数据），可能要分配磁盘块，或者像 ext4 的延迟分配那样只预留空间。
3. `copy_folio_from_iter_atomic()`：把用户数据拷入 folio。
4. `->write_end`：文件系统完成收尾——把 folio 标记为脏、更新 `i_size`、解锁并释放 folio。返回实际接受的字节数。

`chunk` 是 mapping 允许的最大 folio 大小（`mapping_max_folio_size()`），支持大 folio 的文件系统一次可以处理多个页。

剩下的部分处理"短拷贝"：

[`mm/filemap.c:4295-4335`](../../linux/mm/filemap.c#L4295-L4335)（v6.18.60）

```c
		if (unlikely(status != copied)) {
			iov_iter_revert(i, copied - max(status, 0L));
			if (unlikely(status < 0))
				break;
		}
		cond_resched();

		if (unlikely(status == 0)) {
			/*
			 * A short copy made ->write_end() reject the
			 * thing entirely.  Might be memory poisoning
			 * halfway through, might be a race with munmap,
			 * might be severe memory pressure.
			 */
			if (chunk > PAGE_SIZE)
				chunk /= 2;
			if (copied) {
				bytes = copied;
				goto retry;
			}

			/*
			 * 'folio' is now unlocked and faults on it can be
			 * handled. Ensure forward progress by trying to
			 * fault it in now.
			 */
			if (fault_in_iov_iter_readable(i, bytes) == bytes) {
				status = -EFAULT;
				break;
			}
		} else {
			pos += status;
			written += status;
		}
	} while (iov_iter_count(i));

	if (!written)
		return status;
	iocb->ki_pos += written;
	return written;
}
```

拷贝使用**原子**版本（不允许处理缺页），是为了避免一种死锁：用户缓冲区本身可能就是同一文件的 `mmap()` 映射，而此时目标 folio 已被 `->write_begin` 锁定；如果拷贝时触发缺页，缺页处理要锁定同一个 folio，于是自己等待自己。原子拷贝遇到缺页时只是拷贝不全并返回。`->write_end` 若因此拒绝本次写入（返回 0），循环先在不持有 folio 锁的情况下用 `fault_in_iov_iter_readable()` 把用户页预先调入，再重试；若确实无法调入则返回 `-EFAULT`。为了避免反复失败，`chunk` 会逐次减半。

### write_begin 与 write_end：以 ramfs 为例

ramfs 的数据只存在于 page cache 中，它的 `write_begin`/`write_end` 是最简单的实现：

[`fs/libfs.c:930-950`](../../linux/fs/libfs.c#L930-L950)（v6.18.60）

```c
int simple_write_begin(const struct kiocb *iocb, struct address_space *mapping,
			loff_t pos, unsigned len,
			struct folio **foliop, void **fsdata)
{
	struct folio *folio;

	folio = __filemap_get_folio(mapping, pos / PAGE_SIZE, FGP_WRITEBEGIN,
			mapping_gfp_mask(mapping));
	if (IS_ERR(folio))
		return PTR_ERR(folio);

	*foliop = folio;

	if (!folio_test_uptodate(folio) && (len != folio_size(folio))) {
		size_t from = offset_in_folio(folio, pos);

		folio_zero_segments(folio, 0, from,
				from + len, folio_size(folio));
	}
	return 0;
}
```

`__filemap_get_folio()` 以 `FGP_WRITEBEGIN`（即 `FGP_LOCK | FGP_WRITE | FGP_CREAT | FGP_STABLE`）查找或创建 folio，返回时 folio 已锁定（9.8 节）。若 folio 不是最新的且本次写入不覆盖整个 folio，就把写入范围之外的部分清零——ramfs 中不存在的数据就是零。

[`fs/libfs.c:976-1005`](../../linux/fs/libfs.c#L976-L1005)（v6.18.60）

```c
static int simple_write_end(const struct kiocb *iocb,
			    struct address_space *mapping,
			    loff_t pos, unsigned len, unsigned copied,
			    struct folio *folio, void *fsdata)
{
	struct inode *inode = folio->mapping->host;
	loff_t last_pos = pos + copied;

	/* zero the stale part of the folio if we did a short copy */
	if (!folio_test_uptodate(folio)) {
		if (copied < len) {
			size_t from = offset_in_folio(folio, pos);

			folio_zero_range(folio, from + copied, len - copied);
		}
		folio_mark_uptodate(folio);
	}
	/*
	 * No need to use i_size_read() here, the i_size
	 * cannot change under us because we hold the i_rwsem.
	 */
	if (last_pos > inode->i_size)
		i_size_write(inode, last_pos);

	folio_mark_dirty(folio);
	folio_unlock(folio);
	folio_put(folio);

	return copied;
}
```

`write_end` 把 folio 标记为 uptodate 与脏、必要时扩大 `i_size`，然后解锁并释放引用。ramfs 的 `dirty_folio` 是 `noop_dirty_folio()`：它只设置 folio 的脏标志，不做任何写回记账——ramfs 没有后备存储，脏页永远不会被写回，也就永远不能被回收（`mapping_set_unevictable()`）。这也是 ramfs 没有容量限制、可能耗尽内存的原因；tmpfs 增加了容量限制并可以把页换出到交换空间。

对于 ext4 这样的磁盘文件系统，`write_end` 调用 `folio_mark_dirty()` 后会经 `filemap_dirty_folio()` 在 page cache 的 xarray 中为该 folio 打上 `PAGECACHE_TAG_DIRTY` 标记，并通过 `__mark_inode_dirty(I_DIRTY_PAGES)` 把 inode 挂上写回链表（9.10 节）。

### generic_write_sync()

[`include/linux/fs.h:3067-3083`](../../linux/include/linux/fs.h#L3067-L3083)（v6.18.60）

```c
static inline ssize_t generic_write_sync(struct kiocb *iocb, ssize_t count)
{
	if (iocb_is_dsync(iocb)) {
		int ret = vfs_fsync_range(iocb->ki_filp,
				iocb->ki_pos - count, iocb->ki_pos - 1,
				(iocb->ki_flags & IOCB_SYNC) ? 0 : 1);
		if (ret)
			return ret;
	} else if (iocb->ki_flags & IOCB_DONTCACHE) {
		struct address_space *mapping = iocb->ki_filp->f_mapping;

		filemap_fdatawrite_range_kick(mapping, iocb->ki_pos - count,
					      iocb->ki_pos - 1);
	}

	return count;
}
```

写入完成并释放 `i_rwsem` 之后，若文件以 `O_DSYNC`/`O_SYNC` 打开（或本次写入带 `RWF_DSYNC`，或 inode 带 `S_SYNC` 属性），就对刚写入的范围调用 `vfs_fsync_range()`，等待数据（与元数据）落盘后才返回（9.10 节）。`IOCB_DONTCACHE` 则只是"踢一脚"写回，不等待完成。

### 读取路径：generic_file_read_iter()

[`mm/filemap.c:2911-2953`](../../linux/mm/filemap.c#L2911-L2953)（v6.18.60）

```c
generic_file_read_iter(struct kiocb *iocb, struct iov_iter *iter)
{
	size_t count = iov_iter_count(iter);
	ssize_t retval = 0;

	if (!count)
		return 0; /* skip atime */

	if (iocb->ki_flags & IOCB_DIRECT) {
		struct file *file = iocb->ki_filp;
		struct address_space *mapping = file->f_mapping;
		struct inode *inode = mapping->host;

		retval = kiocb_write_and_wait(iocb, count);
		if (retval < 0)
			return retval;
		file_accessed(file);

		retval = mapping->a_ops->direct_IO(iocb, iter);
		if (retval >= 0) {
			iocb->ki_pos += retval;
			count -= retval;
		}
		if (retval != -EIOCBQUEUED)
			iov_iter_revert(iter, count - iov_iter_count(iter));

		/*
		 * Btrfs can have a short DIO read if we encounter
		 * compressed extents, so if there was an error, or if
		 * we've already read everything we wanted to, or if
		 * there was a short read because we hit EOF, go ahead
		 * and return.  Otherwise fallthrough to buffered io for
		 * the rest of the read.  Buffered reads will not work for
		 * DAX files, so don't bother trying.
		 */
		if (retval < 0 || !count || IS_DAX(inode))
			return retval;
		if (iocb->ki_pos >= i_size_read(inode))
			return retval;
	}

	return filemap_read(iocb, iter, retval);
}
```

缓冲读直接进入 `filemap_read()`（9.8 节）。与写不同，通用的读路径**不获取** `i_rwsem`，读与写可以并发进行，同一 folio 内的一致性靠 folio 锁与 uptodate 标志保证。因此 Linux 的缓冲读写不保证跨页的原子性：一个读者可能看到另一个跨多页写入的中间状态。XFS 在缓冲读时持共享的 `i_rwsem`（[`fs/xfs/xfs_file.c:272-289`](../../linux/fs/xfs/xfs_file.c#L272-L289)），提供更强的读写原子性，代价是读写之间的并发度降低。

数据最终通过 `copy_folio_to_iter()` → `copy_page_to_iter()` 拷到用户空间：

[`lib/iov_iter.c:349-376`](../../linux/lib/iov_iter.c#L349-L376)（v6.18.60）

```c
size_t copy_page_to_iter(struct page *page, size_t offset, size_t bytes,
			 struct iov_iter *i)
{
	size_t res = 0;
	if (!page_copy_sane(page, offset, bytes))
		return 0;
	if (WARN_ON_ONCE(i->data_source))
		return 0;
	page += offset / PAGE_SIZE; // first subpage
	offset %= PAGE_SIZE;
	while (1) {
		void *kaddr = kmap_local_page(page);
		size_t n = min(bytes, (size_t)PAGE_SIZE - offset);
		n = _copy_to_iter(kaddr + offset, n, i);
		kunmap_local(kaddr);
		res += n;
		bytes -= n;
		if (!bytes || !n)
			break;
		offset += n;
		if (offset == PAGE_SIZE) {
			page++;
			offset = 0;
		}
	}
	return res;
}
EXPORT_SYMBOL(copy_page_to_iter);
```

`kmap_local_page()` 在 x86_64 上只是返回直接映射区的地址（没有高端内存），`_copy_to_iter()` 根据迭代器类型分派到 `copy_to_user()` 或 `memcpy()`。

### 直接 I/O 与 page cache 的一致性

直接 I/O 绕过 page cache，但同一文件可能同时被其他进程以缓冲方式访问，二者之间必须保持一致：

- **直接读之前**，必须先把该范围内的脏页写回，否则会读到磁盘上的旧数据。`generic_file_read_iter()` 中的 `kiocb_write_and_wait()`（[`mm/filemap.c:2838-2851`](../../linux/mm/filemap.c#L2838-L2851)）就是做这件事。
- **直接写之前**，要写回并失效（invalidate）该范围内的缓存页，否则之后的缓冲读会读到 page cache 中的旧数据（`kiocb_invalidate_pages()`，[`mm/filemap.c:2879-2886`](../../linux/mm/filemap.c#L2879-L2886)）。

ext4、XFS 等现代文件系统的直接 I/O 由 iomap 框架（`iomap_dio_rw()`）实现，它把文件偏移映射为磁盘块后直接构造 bio 提交给块层。直接 I/O 通常要求用户缓冲区地址、文件偏移与长度按逻辑块大小对齐（`statx()` 的 `STATX_DIOALIGN` 可以查询具体要求）。

## 关键细节 / 设计考量

### 为什么拆成 write_begin 与 write_end

把一次 folio 写入拆为"准备—拷贝—提交"三步，让通用代码负责与用户空间打交道（拷贝、缺页、短拷贝重试），文件系统只负责与存储打交道（块分配、日志、标脏）。文件系统不必关心数据来自 `write()` 还是 `splice()`，通用代码也不必了解块映射。iomap 框架（`iomap_file_buffered_write()`）把这一模式进一步推广：文件系统一次映射一个范围（extent），然后对范围内的每个 folio 执行通用的写入逻辑，ext4 的部分路径与 XFS 都已转向 iomap。

### 部分写与信号

`generic_perform_write()` 在每轮开始检查 `fatal_signal_pending()`：进程被 `SIGKILL` 时停止写入。只要已经写入了一部分，就返回已写入的字节数（短写），否则返回 `-EINTR`。应用程序因此必须能处理 `write()` 写入少于请求字节数的情况。普通信号不会中断磁盘文件的缓冲写。

### 异步缓冲读写

io_uring 提交缓冲读时先带 `IOCB_NOWAIT` 内联尝试：数据都在 page cache 中就直接完成；否则带 `IOCB_WAITQ` 重新提交，`filemap_read()` 在 folio 未就绪时不睡眠，而是在 folio 的等待队列上登记回调（`ki_waitq`），folio 就绪时唤醒 io_uring 继续。文件系统通过 `FOP_BUFFER_RASYNC`、`FOP_BUFFER_WASYNC` 声明是否支持这种模式（9.1 节 ext4 的 `fop_flags`）。

## 动手实验

### 缓冲写与直接写

```bash
cd /var/tmp
grep -E "^(Dirty|Writeback):" /proc/meminfo
dd if=/dev/zero of=buffered bs=1M count=256          # 很快返回
grep -E "^(Dirty|Writeback):" /proc/meminfo          # Dirty 增加约 256 MB
dd if=/dev/zero of=direct bs=1M count=256 oflag=direct    # 速度取决于磁盘
dd if=/dev/zero of=dsync bs=1M count=256 oflag=dsync      # 每次 write 都等待落盘
rm -f buffered direct dsync
```

### 观察 write_begin / write_end

```bash
sudo bpftrace -e '
kprobe:generic_perform_write { @start[tid] = nsecs; }
kretprobe:generic_perform_write /@start[tid]/ {
	@lat_us = hist((nsecs - @start[tid]) / 1000);
	delete(@start[tid]);
}
kprobe:ext4_da_write_begin { @wb = count(); }
interval:s:10 { exit(); }'
```

在另一个终端运行 `dd if=/dev/zero of=/var/tmp/x bs=1M count=2048`。写入量接近脏页上限后，`generic_perform_write()` 的延迟分布会出现明显的长尾——这是 `balance_dirty_pages()` 在节流写入者。

### O_APPEND 的原子性

```bash
cd /var/tmp && rm -f log
for i in $(seq 8); do (for j in $(seq 1000); do echo "writer-$i-$j" >> log; done) & done; wait
wc -l log                     # 8000 行，没有行被覆盖或交错
```

## 小结

- `struct kiocb` 描述一次 I/O 的上下文（文件、位置、`IOCB_*` 标志、完成回调），`struct iov_iter` 抽象数据的来源或去向；所有读写入口都被统一为 `->read_iter`/`->write_iter`。
- `vfs_write()` 通过 `file_start_write()` 获取超级块的冻结保护；`generic_file_write_iter()` 在 `i_rwsem` 独占下检查与调整写入范围（保证 `O_APPEND` 原子性），清除 setuid 位、更新时间戳。
- `generic_perform_write()` 逐 folio 执行 `balance_dirty_pages_ratelimited()` → `->write_begin` → 原子拷贝 → `->write_end`；原子拷贝与缺页预取避免了 mmap 自引用导致的死锁。
- 缓冲写拷入 page cache、标脏后即返回，持久化依赖写回或 `O_SYNC`/`fsync()`；`generic_write_sync()` 处理同步写语义。
- 通用缓冲读不持 `i_rwsem`，跨页读写不保证原子性；直接 I/O 前需要写回（读）或写回并失效（写）对应范围的 page cache。

## 参考

- `linux/Documentation/filesystems/vfs.rst`（`write_begin`、`write_end`、`read_iter` 的约定）
- `linux/Documentation/filesystems/iomap/`：iomap 框架设计
- `linux/Documentation/filesystems/locking.rst`
- `man 2 preadv2`、`man 2 open`（`O_DIRECT`、`O_SYNC`）、`man 8 fsfreeze`
- Jens Axboe, "Uncached buffered IO"（`RWF_DONTCACHE`，v6.14）
