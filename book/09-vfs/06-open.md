# 9.6 文件打开、文件描述符与 struct file

> **本节源码**：[`fs/open.c`](../../linux/fs/open.c)、[`fs/namei.c`](../../linux/fs/namei.c)、[`fs/file.c`](../../linux/fs/file.c)、[`fs/file_table.c`](../../linux/fs/file_table.c)、[`include/linux/fdtable.h`](../../linux/include/linux/fdtable.h)、[`include/linux/file.h`](../../linux/include/linux/file.h)、[`include/linux/file_ref.h`](../../linux/include/linux/file_ref.h)、[`fs/internal.h`](../../linux/fs/internal.h)
> **关键函数**：`do_sys_openat2()`、`build_open_flags()`、`alloc_fd()`、`do_filp_open()`、`path_openat()`、`alloc_empty_file()`、`open_last_lookups()`、`lookup_open()`、`do_open()`、`vfs_open()`、`do_dentry_open()`、`fd_install()`、`__fget_light()`、`__fget_files_rcu()`、`fdget_pos()`、`file_close_fd()`、`fput()`、`__fput()`
> **前置知识**：9.3 路径名查找、9.4 目录项缓存、7.9 RCU 原理与使用

## 概述

`open()` 把一个路径变成一个**文件描述符（File Descriptor，fd）**——一个小整数。其背后有三层对象：

```
  进程 A 的 fd 表           struct file（打开文件描述）         inode
  ┌────┐
  │ 0  │──────────────▶ ┌──────────────────┐
  │ 1  │──────┐         │ f_pos = 100      │──────────┐
  │ 3  │──┐   └───────▶ │ f_mode = R       │          │
  └────┘  │             └──────────────────┘          ▼
          │             ┌──────────────────┐     ┌──────────┐
          └───────────▶ │ f_pos = 0        │───▶ │ inode 42 │
  进程 B 的 fd 表         │ f_mode = RW      │     └──────────┘
  ┌────┐   (fork 继承)   └──────────────────┘          ▲
  │ 3  │──────────────▶   （与 A 的 fd 3 共享）       │
  │ 4  │──────────────▶ ┌──────────────────┐          │
  └────┘                │ f_pos = 4096     │──────────┘
                        └──────────────────┘
```

*图 9-6：文件描述符、struct file 与 inode 的关系*

- **文件描述符**只是进程 fd 表的下标。`dup()` 复制下标、`fork()` 复制整张表，复制后的 fd 指向**同一个** `struct file`。
- **`struct file`**（POSIX 称为打开文件描述）记录一次 `open()` 的状态：读写位置、打开模式、操作表。共享它的 fd 共享读写位置，这正是父子进程同时向一个日志文件追加输出时不会互相覆盖的原因。
- **inode** 是文件本身。对同一文件的两次独立 `open()` 得到两个 `struct file`，各自有独立的读写位置。

本节分析 `open()` 的完整实现：分配 fd、路径查找与最后一个分量的处理（可能需要创建文件）、构造 `struct file`、安装到 fd 表；然后分析系统调用如何由 fd 找到 `struct file`，以及 `close()` 如何释放它。

## 核心数据结构

### fd 表

`struct files_struct` 与 `struct fdtable` 的定义已在 9.1 节给出，这里说明它们的组织方式：

```
 files_struct
 ├── fdt ─────────────┐ （RCU 指针，扩容时替换）
 ├── fdtab ◀──────────┘ 初始时 fdt 指向内嵌的 fdtab
 │    ├── max_fds = 64
 │    ├── fd ─────────────▶ fd_array[64]        （内嵌）
 │    ├── open_fds ───────▶ open_fds_init[1]     每位一个 fd：是否已分配
 │    ├── close_on_exec ──▶ close_on_exec_init[1]
 │    └── full_fds_bits ──▶ full_fds_bits_init[1] 每位对应 open_fds 的一个字：是否已满
 ├── file_lock             保护分配与扩容
 └── next_fd               分配提示：小于它的 fd 都已被占用
```

*图 9-7：fd 表的组织（扩容后 `fdt` 指向新分配的 `fdtable`）*

进程最初的 64 个 fd 直接使用内嵌在 `files_struct` 中的数组与位图，不需要额外分配。打开更多文件时，`expand_fdtable()`（[`fs/file.c:252-279`](../../linux/fs/file.c#L252-L279)）分配更大的 `fdtable`，复制旧内容，然后用 `rcu_assign_pointer()` 发布新表，旧表经 `call_rcu()` 释放——这使得读者可以在 RCU 下无锁访问 fd 表。fd 上限受 `RLIMIT_NOFILE`（`ulimit -n`）与全局的 `fs.nr_open` 共同约束。

### open_flags

`open()` 的参数先被翻译成内核内部使用的 `struct open_flags`：

[`fs/internal.h:181-187`](../../linux/fs/internal.h#L181-L187)（v6.18.60）

```c
struct open_flags {
	int open_flag;
	umode_t mode;
	int acc_mode;
	int intent;
	int lookup_flags;
};
```

| 字段 | 含义 |
|---|---|
| `open_flag` | 规范化后的 `O_*` 标志 |
| `mode` | 创建文件时的权限位 |
| `acc_mode` | 需要检查的权限：`MAY_READ`、`MAY_WRITE`、`MAY_APPEND` 等 |
| `intent` | 告诉文件系统查找的意图：`LOOKUP_OPEN`、`LOOKUP_CREATE`、`LOOKUP_EXCL` |
| `lookup_flags` | 路径查找标志：`LOOKUP_FOLLOW`、`LOOKUP_DIRECTORY`，以及 `openat2()` 的 `RESOLVE_*` 对应的作用域标志 |

`build_open_flags()`（[`fs/open.c:1238-1361`](../../linux/fs/open.c#L1238-L1361)）完成翻译并检查参数组合的合法性。例如 `O_SYNC` 被实现为 `__O_SYNC | O_DSYNC`，`O_TRUNC` 隐含写权限检查，`O_CREAT | O_EXCL` 隐含 `O_NOFOLLOW`（防止通过符号链接在意外的位置创建文件），`O_PATH` 只允许与少数标志组合：

[`fs/open.c:1311-1340`](../../linux/fs/open.c#L1311-L1340)（v6.18.60）

```c
	if (flags & __O_SYNC)
		flags |= O_DSYNC;

	op->open_flag = flags;

	/* O_TRUNC implies we need access checks for write permissions */
	if (flags & O_TRUNC)
		acc_mode |= MAY_WRITE;

	/* Allow the LSM permission hook to distinguish append
	   access from general write access. */
	if (flags & O_APPEND)
		acc_mode |= MAY_APPEND;

	op->acc_mode = acc_mode;

	op->intent = flags & O_PATH ? 0 : LOOKUP_OPEN;

	if (flags & O_CREAT) {
		op->intent |= LOOKUP_CREATE;
		if (flags & O_EXCL) {
			op->intent |= LOOKUP_EXCL;
			flags |= O_NOFOLLOW;
		}
	}

	if (flags & O_DIRECTORY)
		lookup_flags |= LOOKUP_DIRECTORY;
	if (!(flags & O_NOFOLLOW))
		lookup_flags |= LOOKUP_FOLLOW;
```

### f_mode

`struct file` 的 `f_mode` 汇集了打开后不再改变的属性，热路径上只检查它而不去解析 `f_flags`：

| 标志 | 含义 | 设置时机 |
|---|---|---|
| `FMODE_READ` / `FMODE_WRITE` | 以读 / 写方式打开 | `init_file()` 中由 `O_ACCMODE` 换算 |
| `FMODE_LSEEK` / `FMODE_PREAD` / `FMODE_PWRITE` | 支持 `lseek()` / `pread()` / `pwrite()` | `do_dentry_open()` 默认设置，`->open` 可清除 |
| `FMODE_ATOMIC_POS` | 读写位置的更新需要原子性 | 普通文件与目录 |
| `FMODE_CAN_READ` / `FMODE_CAN_WRITE` | 操作表提供了读 / 写方法 | `do_dentry_open()` 根据 `f_op` 计算 |
| `FMODE_OPENED` | 打开已完成，释放时需要调用 `->release` | `do_dentry_open()` |
| `FMODE_CREATED` | 本次 `open()` 创建了文件 | `lookup_open()` |
| `FMODE_PATH` | `O_PATH` 打开，只代表位置，不能读写 | `do_dentry_open()` |
| `FMODE_STREAM` | 流式文件，没有位置概念 | 管道、套接字等的 `->open` |
| `FMODE_WRITER` | 已获取 inode 与挂载的写访问权 | `do_dentry_open()` |

### file_ref_t

`struct file` 的引用计数 `f_ref` 是 v6.13 引入的专用类型 `file_ref_t`（Christian Brauner，借鉴了 Thomas Gleixner 的 `rcuref`），取代了此前的 `atomic_long_t f_count`：

[`include/linux/file_ref.h:9-28`](../../linux/include/linux/file_ref.h#L9-L28)（v6.18.60）

```c
/*
 * file_ref is a reference count implementation specifically for use by
 * files. It takes inspiration from rcuref but differs in key aspects
 * such as support for SLAB_TYPESAFE_BY_RCU type caches.
 *
 * FILE_REF_ONEREF                FILE_REF_MAXREF
 * 0x0000000000000000UL      0x7FFFFFFFFFFFFFFFUL
 * <-------------------valid ------------------->
 *
 *                       FILE_REF_SATURATED
 * 0x8000000000000000UL 0xA000000000000000UL 0xBFFFFFFFFFFFFFFFUL
 * <-----------------------saturation zone---------------------->
 *
 * FILE_REF_RELEASED                   FILE_REF_DEAD
 * 0xC000000000000000UL         0xE000000000000000UL
 * <-------------------dead zone------------------->
 *
 * FILE_REF_NOREF
 * 0xFFFFFFFFFFFFFFFFUL
 */
```

`filp` slab 缓存带有 `SLAB_TYPESAFE_BY_RCU` 标志（[`fs/file_table.c:625-641`](../../linux/fs/file_table.c#L625-L641)）：`struct file` 被释放后，其内存可以**立即**被重新分配为另一个 `struct file`，只是在 RCU 宽限期内不会被归还给伙伴系统或用作其他类型的对象。于是 RCU 读者在 fd 表中拿到一个 `struct file` 指针时，它可能已经被释放并重用了。`file_ref_t` 把计数的"死亡"与"已释放"编码为特殊值，使得 `file_ref_get()` 在对象已死亡时必然失败，读者再通过重新检查 fd 表项确认拿到的仍是原来那个文件（见后文 `__fget_files_rcu()`）。计数值 0 表示"一个引用"（`FILE_REF_ONEREF`），负值区间留作饱和区与死亡区。`file_ref_get()`（[`include/linux/file_ref.h:78-109`](../../linux/include/linux/file_ref.h#L78-L109)）因此只需一条无条件的原子加法 `atomic_long_add_negative()`，根据结果的符号位判断是否成功，不需要 cmpxchg 循环；对已死亡对象的误加不会让它"复活"，因为死亡区离有效区有 2^61 之遥。

## 主流程分析

### 调用链总览

```
openat(dfd, path, flags, mode)                 fs/open.c:1463
└── do_sys_open() → do_sys_openat2()           fs/open.c:1420
    ├── build_open_flags()                     // O_* → open_flags
    ├── getname()                              // 拷入路径名
    ├── get_unused_fd_flags() → alloc_fd()     fs/file.c:570
    ├── do_filp_open()                         fs/namei.c:4185
    │   └── path_openat()                      // RCU / REF / REVAL 三次尝试
    │       ├── alloc_empty_file()             fs/file_table.c:239
    │       ├── path_init() + link_path_walk() // 9.3 节
    │       ├── open_last_lookups()            // 最后一个分量
    │       │   ├── lookup_fast_for_open()
    │       │   └── lookup_open()              // 未命中或需要创建
    │       │       ├── d_alloc_parallel()
    │       │       ├── dir->i_op->atomic_open() // 网络文件系统
    │       │       ├── dir->i_op->lookup()
    │       │       └── dir->i_op->create()    // O_CREAT 且不存在
    │       └── do_open()
    │           ├── may_open()                 // 权限检查
    │           ├── vfs_open() → do_dentry_open()
    │           │   └── f_op->open()
    │           └── handle_truncate()          // O_TRUNC
    └── fd_install()                           fs/file.c:652
```

### do_sys_openat2()

[`fs/open.c:1420-1447`](../../linux/fs/open.c#L1420-L1447)（v6.18.60）

```c
static int do_sys_openat2(int dfd, const char __user *filename,
			  struct open_how *how)
{
	struct open_flags op;
	struct filename *tmp;
	int err, fd;

	err = build_open_flags(how, &op);
	if (unlikely(err))
		return err;

	tmp = getname(filename);
	if (IS_ERR(tmp))
		return PTR_ERR(tmp);

	fd = get_unused_fd_flags(how->flags);
	if (likely(fd >= 0)) {
		struct file *f = do_filp_open(dfd, tmp, &op);
		if (IS_ERR(f)) {
			put_unused_fd(fd);
			fd = PTR_ERR(f);
		} else {
			fd_install(fd, f);
		}
	}
	putname(tmp);
	return fd;
}
```

注意顺序：**先分配 fd，再打开文件，最后安装**。先分配 fd 是为了在 fd 耗尽（`-EMFILE`）时尽早失败，不必白做一次路径查找。分配后的 fd 在位图中被标记为已占用，但 fd 表项仍为 `NULL`，其他线程此时对这个 fd 的访问会得到 `-EBADF`；直到 `fd_install()` 才真正可见。打开失败时 `put_unused_fd()` 归还 fd。

### 分配 fd：alloc_fd()

POSIX 要求 `open()` 返回当前**最小**的可用 fd：

[`fs/file.c:570-613`](../../linux/fs/file.c#L570-L613)（v6.18.60）

```c
static int alloc_fd(unsigned start, unsigned end, unsigned flags)
{
	struct files_struct *files = current->files;
	unsigned int fd;
	int error;
	struct fdtable *fdt;

	spin_lock(&files->file_lock);
repeat:
	fdt = files_fdtable(files);
	fd = start;
	if (fd < files->next_fd)
		fd = files->next_fd;

	if (likely(fd < fdt->max_fds))
		fd = find_next_fd(fdt, fd);

	/*
	 * N.B. For clone tasks sharing a files structure, this test
	 * will limit the total number of files that can be opened.
	 */
	error = -EMFILE;
	if (unlikely(fd >= end))
		goto out;

	if (unlikely(fd >= fdt->max_fds)) {
		error = expand_files(files, fd);
		if (error < 0)
			goto out;

		goto repeat;
	}

	if (start <= files->next_fd)
		files->next_fd = fd + 1;

	__set_open_fd(fd, fdt, flags & O_CLOEXEC);
	error = fd;
	VFS_BUG_ON(rcu_access_pointer(fdt->fd[fd]) != NULL);

out:
	spin_unlock(&files->file_lock);
	return error;
}
```

`next_fd` 是一个提示：所有小于它的 fd 都已被占用，搜索可以从它开始。释放 fd 时若被释放的 fd 更小，就把 `next_fd` 回退（`__put_unused_fd()`）。若找到的 fd 超出当前表的大小，就调用 `expand_files()` 扩容后重试。

查找空闲位使用两级位图：

[`fs/file.c:544-565`](../../linux/fs/file.c#L544-L565)（v6.18.60）

```c
static unsigned int find_next_fd(struct fdtable *fdt, unsigned int start)
{
	unsigned int maxfd = fdt->max_fds; /* always multiple of BITS_PER_LONG */
	unsigned int maxbit = maxfd / BITS_PER_LONG;
	unsigned int bitbit = start / BITS_PER_LONG;
	unsigned int bit;

	/*
	 * Try to avoid looking at the second level bitmap
	 */
	bit = find_next_zero_bit(&fdt->open_fds[bitbit], BITS_PER_LONG,
				 start & (BITS_PER_LONG - 1));
	if (bit < BITS_PER_LONG)
		return bit + bitbit * BITS_PER_LONG;

	bitbit = find_next_zero_bit(fdt->full_fds_bits, maxbit, bitbit) * BITS_PER_LONG;
	if (bitbit >= maxfd)
		return maxfd;
	if (bitbit > start)
		start = bitbit;
	return find_next_zero_bit(fdt->open_fds, maxfd, start);
}
```

先在起始位置所在的那个字里找空闲位；找不到时借助 `full_fds_bits` 一次跳过所有已满的字（`full_fds_bits` 的每一位表示 `open_fds` 中对应的 64 位字是否全满）。对于打开了数十万个文件的服务器进程，这避免了线性扫描整个位图。

### path_openat()：查找与打开

`do_filp_open()`（[`fs/namei.c:4185-4200`](../../linux/fs/namei.c#L4185-L4200)）与 9.3 节的 `filename_lookup()` 结构相同，依次以 RCU、REF、REVAL 模式调用 `path_openat()`：

[`fs/namei.c:4146-4183`](../../linux/fs/namei.c#L4146-L4183)（v6.18.60）

```c
static struct file *path_openat(struct nameidata *nd,
			const struct open_flags *op, unsigned flags)
{
	struct file *file;
	int error;

	file = alloc_empty_file(op->open_flag, current_cred());
	if (IS_ERR(file))
		return file;

	if (unlikely(file->f_flags & __O_TMPFILE)) {
		error = do_tmpfile(nd, flags, op, file);
	} else if (unlikely(file->f_flags & O_PATH)) {
		error = do_o_path(nd, flags, file);
	} else {
		const char *s = path_init(nd, flags);
		while (!(error = link_path_walk(s, nd)) &&
		       (s = open_last_lookups(nd, file, op)) != NULL)
			;
		if (!error)
			error = do_open(nd, file, op);
		terminate_walk(nd);
	}
	if (likely(!error)) {
		if (likely(file->f_mode & FMODE_OPENED))
			return file;
		WARN_ON(1);
		error = -EINVAL;
	}
	fput_close(file);
	if (error == -EOPENSTALE) {
		if (flags & LOOKUP_RCU)
			error = -ECHILD;
		else
			error = -ESTALE;
	}
	return ERR_PTR(error);
}
```

`alloc_empty_file()`（[`fs/file_table.c:239-279`](../../linux/fs/file_table.c#L239-L279)）从 `filp` 缓存分配 `struct file`，检查系统打开文件总数是否超过 `fs.file-max`（特权进程不受此限），由 `init_file()` 初始化字段并设置 `f_ref` 为一个引用。`O_TMPFILE`（在目录中创建无名文件）与 `O_PATH`（只获取位置）走各自的特殊路径；一般情况则先由 `link_path_walk()` 解析到最后一个分量的父目录，再循环调用 `open_last_lookups()` 处理最后一个分量（若它是符号链接，返回链接内容继续循环）。

### open_last_lookups()：最后一个分量

打开的最后一个分量比普通查找复杂，因为可能需要创建文件。快速路径仍尝试在 dcache 中查找：

[`fs/namei.c:3876-3909`](../../linux/fs/namei.c#L3876-L3909)（v6.18.60）

```c
static const char *open_last_lookups(struct nameidata *nd,
		   struct file *file, const struct open_flags *op)
{
	struct dentry *dir = nd->path.dentry;
	int open_flag = op->open_flag;
	bool got_write = false;
	struct dentry *dentry;
	const char *res;

	nd->flags |= op->intent;

	if (nd->last_type != LAST_NORM) {
		if (nd->depth)
			put_link(nd);
		return handle_dots(nd, nd->last_type);
	}

	/* We _can_ be in RCU mode here */
	dentry = lookup_fast_for_open(nd, open_flag);
	if (IS_ERR(dentry))
		return ERR_CAST(dentry);

	if (likely(dentry))
		goto finish_lookup;

	if (!(open_flag & O_CREAT)) {
		if (WARN_ON_ONCE(nd->flags & LOOKUP_RCU))
			return ERR_PTR(-ECHILD);
	} else {
		if (nd->flags & LOOKUP_RCU) {
			if (!try_to_unlazy(nd))
				return ERR_PTR(-ECHILD);
		}
	}
```

`lookup_fast_for_open()`（[`fs/namei.c:3846-3874`](../../linux/fs/namei.c#L3846-L3874)）对 `O_CREAT | O_EXCL` 直接放弃 dcache（结果必然是"创建"，查到了也只能报 `-EEXIST`），对 `O_CREAT` 时找到的负 dentry 也视为未命中。命中正 dentry 时直接跳到 `finish_lookup`。否则进入慢速路径：

[`fs/namei.c:3911-3953`](../../linux/fs/namei.c#L3911-L3953)（v6.18.60）

```c
	if (open_flag & (O_CREAT | O_TRUNC | O_WRONLY | O_RDWR)) {
		got_write = !mnt_want_write(nd->path.mnt);
		/*
		 * do _not_ fail yet - we might not need that or fail with
		 * a different error; let lookup_open() decide; we'll be
		 * dropping this one anyway.
		 */
	}
	if (open_flag & O_CREAT)
		inode_lock(dir->d_inode);
	else
		inode_lock_shared(dir->d_inode);
	dentry = lookup_open(nd, file, op, got_write);
	if (!IS_ERR(dentry)) {
		if (file->f_mode & FMODE_CREATED)
			fsnotify_create(dir->d_inode, dentry);
		if (file->f_mode & FMODE_OPENED)
			fsnotify_open(file);
	}
	if (open_flag & O_CREAT)
		inode_unlock(dir->d_inode);
	else
		inode_unlock_shared(dir->d_inode);

	if (got_write)
		mnt_drop_write(nd->path.mnt);

	if (IS_ERR(dentry))
		return ERR_CAST(dentry);

	if (file->f_mode & (FMODE_OPENED | FMODE_CREATED)) {
		dput(nd->path.dentry);
		nd->path.dentry = dentry;
		return NULL;
	}

finish_lookup:
	if (nd->depth)
		put_link(nd);
	res = step_into(nd, WALK_TRAILING, dentry);
	if (unlikely(res))
		nd->flags &= ~(LOOKUP_OPEN|LOOKUP_CREATE|LOOKUP_EXCL);
	return res;
```

慢速路径的要点：

- 需要创建或写入时，先 `mnt_want_write()` 获取挂载的写权限（只读挂载上会失败，但此时先不报错，留给 `lookup_open()` 判断该返回哪个错误码）。
- 需要创建时对父目录持**独占**的 `i_rwsem`（创建会修改目录），否则持共享锁。
- `lookup_open()` 完成查找、必要时创建。

### lookup_open()

[`fs/namei.c:3743-3766`](../../linux/fs/namei.c#L3743-L3766)（v6.18.60）

```c
	file->f_mode &= ~FMODE_CREATED;
	dentry = d_lookup(dir, &nd->last);
	for (;;) {
		if (!dentry) {
			dentry = d_alloc_parallel(dir, &nd->last, &wq);
			if (IS_ERR(dentry))
				return dentry;
		}
		if (d_in_lookup(dentry))
			break;

		error = d_revalidate(dir_inode, &nd->last, dentry, nd->flags);
		if (likely(error > 0))
			break;
		if (error)
			goto out_dput;
		d_invalidate(dentry);
		dput(dentry);
		dentry = NULL;
	}
	if (dentry->d_inode) {
		/* Cached positive dentry: will open in f_op->open */
		return dentry;
	}
```

先在 dcache 中查找，未命中则用 `d_alloc_parallel()` 分配查找中的 dentry（9.4 节）。如果得到正 dentry，直接返回，后续由 `do_open()` 打开。否则准备创建：

[`fs/namei.c:3795-3834`](../../linux/fs/namei.c#L3795-L3834)（v6.18.60）

```c
	if (dir_inode->i_op->atomic_open) {
		dentry = atomic_open(nd, dentry, file, open_flag, mode);
		if (unlikely(create_error) && dentry == ERR_PTR(-ENOENT))
			dentry = ERR_PTR(create_error);
		return dentry;
	}

	if (d_in_lookup(dentry)) {
		struct dentry *res = dir_inode->i_op->lookup(dir_inode, dentry,
							     nd->flags);
		d_lookup_done(dentry);
		if (unlikely(res)) {
			if (IS_ERR(res)) {
				error = PTR_ERR(res);
				goto out_dput;
			}
			dput(dentry);
			dentry = res;
		}
	}

	/* Negative dentry, just create the file */
	if (!dentry->d_inode && (open_flag & O_CREAT)) {
		file->f_mode |= FMODE_CREATED;
		audit_inode_child(dir_inode, dentry, AUDIT_TYPE_CHILD_CREATE);
		if (!dir_inode->i_op->create) {
			error = -EACCES;
			goto out_dput;
		}

		error = dir_inode->i_op->create(idmap, dir_inode, dentry,
						mode, open_flag & O_EXCL);
		if (error)
			goto out_dput;
	}
	if (unlikely(create_error) && !dentry->d_inode) {
		error = create_error;
		goto out_dput;
	}
	return dentry;
```

- 文件系统若实现了 `->atomic_open`（NFS、CIFS、FUSE 等网络 / 用户态文件系统），就把"查找 + 创建 + 打开"作为一次原子操作交给它，可以节省多次网络往返并避免竞争。
- 否则先调用 `->lookup` 完成查找。
- 若查找结果是负 dentry 且带 `O_CREAT`，设置 `FMODE_CREATED`，调用 `->create`（如 ramfs 的 `ramfs_create()`，9.12 节）在磁盘上创建 inode 并 `d_instantiate()`。

### do_open()：权限检查与真正打开

[`fs/namei.c:3968-4015`](../../linux/fs/namei.c#L3968-L4015)（v6.18.60）

```c
	if (!(file->f_mode & (FMODE_OPENED | FMODE_CREATED))) {
		error = complete_walk(nd);
		if (error)
			return error;
	}
	...
	if (open_flag & O_CREAT) {
		if ((open_flag & O_EXCL) && !(file->f_mode & FMODE_CREATED))
			return -EEXIST;
		if (d_is_dir(nd->path.dentry))
			return -EISDIR;
	...
	do_truncate = false;
	acc_mode = op->acc_mode;
	if (file->f_mode & FMODE_CREATED) {
		/* Don't check for write permission, don't truncate */
		open_flag &= ~O_TRUNC;
		acc_mode = 0;
	} else if (d_is_reg(nd->path.dentry) && open_flag & O_TRUNC) {
		error = mnt_want_write(nd->path.mnt);
		if (error)
			return error;
		do_truncate = true;
	}
	error = may_open(idmap, &nd->path, acc_mode, open_flag);
	if (!error && !(file->f_mode & FMODE_OPENED))
		error = vfs_open(&nd->path, file);
	if (!error)
		error = security_file_post_open(file, op->acc_mode);
	if (!error && do_truncate)
		error = handle_truncate(idmap, file);
	if (unlikely(error > 0)) {
		WARN_ON(1);
		error = -EINVAL;
	}
	if (do_truncate)
		mnt_drop_write(nd->path.mnt);
	return error;
}
```

`do_open()` 先调用 `complete_walk()` 结束路径查找（RCU 模式下转为持有引用），处理 `O_CREAT | O_EXCL` 的已存在检查、`O_DIRECTORY` 检查等，然后由 `may_open()` 检查访问权限：设备文件在 `nodev` 挂载上不能打开、目录不能以写方式打开、`append-only` 文件只能追加，以及最终的 `inode_permission()`。新创建的文件跳过权限检查（`acc_mode = 0`），即使创建时指定的权限位不允许当前用户写，`open(O_CREAT | O_WRONLY, 0444)` 返回的 fd 依然可写，这是 POSIX 规定的行为。

### do_dentry_open()：构造 struct file

`vfs_open()` 把路径填入 `file->f_path` 后调用 `do_dentry_open()`：

[`fs/open.c:903-944`](../../linux/fs/open.c#L903-L944)（v6.18.60）

```c
static int do_dentry_open(struct file *f,
			  int (*open)(struct inode *, struct file *))
{
	static const struct file_operations empty_fops = {};
	struct inode *inode = f->f_path.dentry->d_inode;
	int error;

	path_get(&f->f_path);
	f->f_inode = inode;
	f->f_mapping = inode->i_mapping;
	f->f_wb_err = filemap_sample_wb_err(f->f_mapping);
	f->f_sb_err = file_sample_sb_err(f);

	if (unlikely(f->f_flags & O_PATH)) {
		f->f_mode = FMODE_PATH | FMODE_OPENED;
		file_set_fsnotify_mode(f, FMODE_NONOTIFY);
		f->f_op = &empty_fops;
		return 0;
	}

	if ((f->f_mode & (FMODE_READ | FMODE_WRITE)) == FMODE_READ) {
		i_readcount_inc(inode);
	} else if (f->f_mode & FMODE_WRITE && !special_file(inode->i_mode)) {
		error = file_get_write_access(f);
		if (unlikely(error))
			goto cleanup_file;
		f->f_mode |= FMODE_WRITER;
	}

	/* POSIX.1-2008/SUSv4 Section XSI 2.9.7 */
	if (S_ISREG(inode->i_mode) || S_ISDIR(inode->i_mode))
		f->f_mode |= FMODE_ATOMIC_POS;

	f->f_op = fops_get(inode->i_fop);
	if (WARN_ON(!f->f_op)) {
		error = -ENODEV;
		goto cleanup_all;
	}

	error = security_file_open(f);
	if (error)
		goto cleanup_all;
```

这里有几处关键的步骤：

- `path_get()` 为 `f_path` 获取引用：**`struct file` 通过持有 dentry 与挂载的引用，间接保证了 inode 与超级块在文件打开期间有效**。
- `f_wb_err` 记录打开时 `address_space` 的写回错误序号，此后 `fsync()` 只报告打开之后发生的写回错误（9.10 节）。
- 以写方式打开普通文件时，`file_get_write_access()` 递增 inode 的 `i_writecount` 并获取挂载的写访问权。若文件正在被执行（`i_writecount` 为负，`execve()` 时由 `exe_file_deny_write_access()` 设置），这里失败返回 `-ETXTBSY`。
- `f_op` 取自 `inode->i_fop`。对于字符设备，`i_fop` 是 `def_chr_fops`，其 `->open`（`chrdev_open()`）再根据设备号把 `f_op` 替换为具体驱动的操作表——这就是设备文件"转发"到驱动的方式（第 12 章）。

[`fs/open.c:960-987`](../../linux/fs/open.c#L960-L987)（v6.18.60）

```c
	/* normally all 3 are set; ->open() can clear them if needed */
	f->f_mode |= FMODE_LSEEK | FMODE_PREAD | FMODE_PWRITE;
	if (!open)
		open = f->f_op->open;
	if (open) {
		error = open(inode, f);
		if (error)
			goto cleanup_all;
	}
	f->f_mode |= FMODE_OPENED;
	if ((f->f_mode & FMODE_READ) &&
	     likely(f->f_op->read || f->f_op->read_iter))
		f->f_mode |= FMODE_CAN_READ;
	if ((f->f_mode & FMODE_WRITE) &&
	     likely(f->f_op->write || f->f_op->write_iter))
		f->f_mode |= FMODE_CAN_WRITE;
	if ((f->f_mode & FMODE_LSEEK) && !f->f_op->llseek)
		f->f_mode &= ~FMODE_LSEEK;
	if (f->f_mapping->a_ops && f->f_mapping->a_ops->direct_IO)
		f->f_mode |= FMODE_CAN_ODIRECT;

	f->f_flags &= ~(O_CREAT | O_EXCL | O_NOCTTY | O_TRUNC);
	f->f_iocb_flags = iocb_flags(f);

	file_ra_state_init(&f->f_ra, f->f_mapping->host->i_mapping);

	if ((f->f_flags & O_DIRECT) && !(f->f_mode & FMODE_CAN_ODIRECT))
		return -EINVAL;
```

调用文件系统的 `->open` 之后，根据操作表设置 `FMODE_CAN_READ` 等能力位，清除只在打开时有意义的 `O_CREAT`、`O_EXCL`、`O_NOCTTY`、`O_TRUNC`，由 `iocb_flags()` 预先计算出 I/O 时使用的 `IOCB_*` 标志（`O_APPEND` → `IOCB_APPEND`、`O_DIRECT` → `IOCB_DIRECT` 等），并初始化预读状态（9.9 节）。

### 安装 fd：fd_install()

[`fs/file.c:652-677`](../../linux/fs/file.c#L652-L677)（v6.18.60）

```c
void fd_install(unsigned int fd, struct file *file)
{
	struct files_struct *files = current->files;
	struct fdtable *fdt;

	if (WARN_ON_ONCE(unlikely(file->f_mode & FMODE_BACKING)))
		return;

	rcu_read_lock_sched();

	if (unlikely(files->resize_in_progress)) {
		rcu_read_unlock_sched();
		spin_lock(&files->file_lock);
		fdt = files_fdtable(files);
		VFS_BUG_ON(rcu_access_pointer(fdt->fd[fd]) != NULL);
		rcu_assign_pointer(fdt->fd[fd], file);
		spin_unlock(&files->file_lock);
		return;
	}
	/* coupled with smp_wmb() in expand_fdtable() */
	smp_rmb();
	fdt = rcu_dereference_sched(files->fdt);
	VFS_BUG_ON(rcu_access_pointer(fdt->fd[fd]) != NULL);
	rcu_assign_pointer(fdt->fd[fd], file);
	rcu_read_unlock_sched();
}
```

`fd_install()` 只需把 `struct file` 指针写入 fd 表的对应槽位，用 `rcu_assign_pointer()` 保证读者看到指针时也能看到 `struct file` 的完整初始化。唯一的复杂性来自并发扩容：如果另一个线程正在扩容 fd 表（`resize_in_progress`），写入旧表的指针可能在复制完成后丢失，因此改为在 `file_lock` 下写入新表。`expand_fdtable()` 中的 `synchronize_rcu()` 与这里的 `rcu_read_lock_sched()` 配合，保证扩容复制开始前，所有已经进入无锁路径的 `fd_install()` 都已完成。

### 由 fd 找到 struct file

几乎每个以 fd 为参数的系统调用都从 `fdget()` 开始。它的快速路径利用了一个观察：**如果 fd 表没有被其他线程共享，就不可能有人并发关闭这个 fd**，因此连引用计数都不必增加：

[`fs/file.c:1153-1183`](../../linux/fs/file.c#L1153-L1183)（v6.18.60）

```c
static inline struct fd __fget_light(unsigned int fd, fmode_t mask)
{
	struct files_struct *files = current->files;
	struct file *file;

	/*
	 * If another thread is concurrently calling close_fd() followed
	 * by put_files_struct(), we must not observe the old table
	 * entry combined with the new refcount - otherwise we could
	 * return a file that is concurrently being freed.
	 *
	 * atomic_read_acquire() pairs with atomic_dec_and_test() in
	 * put_files_struct().
	 */
	if (likely(atomic_read_acquire(&files->count) == 1)) {
		file = files_lookup_fd_raw(files, fd);
		if (!file || unlikely(file->f_mode & mask))
			return EMPTY_FD;
		return BORROWED_FD(file);
	} else {
		file = __fget_files(files, fd, mask);
		if (!file)
			return EMPTY_FD;
		return CLONED_FD(file);
	}
}
struct fd fdget(unsigned int fd)
{
	return __fget_light(fd, FMODE_PATH);
}
EXPORT_SYMBOL(fdget);
```

`files->count == 1` 时返回的是"借用"的指针（`BORROWED_FD`），`fdput()` 什么也不做；否则走 `__fget_files()`，获取真正的引用（`CLONED_FD`），`fdput()` 时释放。单线程进程的 `read()`、`write()` 因此完全没有对 `f_ref` 的原子操作。

多线程情况下，查找在 RCU 下进行：

[`fs/file.c:989-1063`](../../linux/fs/file.c#L989-L1063)（v6.18.60）

```c
static inline struct file *__fget_files_rcu(struct files_struct *files,
       unsigned int fd, fmode_t mask)
{
	for (;;) {
		struct file *file;
		struct fdtable *fdt = rcu_dereference_raw(files->fdt);
		struct file __rcu **fdentry;
		unsigned long nospec_mask;

		/* Mask is a 0 for invalid fd's, ~0 for valid ones */
		nospec_mask = array_index_mask_nospec(fd, fdt->max_fds);

		/*
		 * fdentry points to the 'fd' offset, or fdt->fd[0].
		 * Loading from fdt->fd[0] is always safe, because the
		 * array always exists.
		 */
		fdentry = fdt->fd + (fd & nospec_mask);

		/* Do the load, then mask any invalid result */
		file = rcu_dereference_raw(*fdentry);
		file = (void *)(nospec_mask & (unsigned long)file);
		if (unlikely(!file))
			return NULL;
		...
		if (unlikely(!file_ref_get(&file->f_ref)))
			continue;
		...
		if (unlikely(file != rcu_dereference_raw(*fdentry)) ||
		    unlikely(rcu_dereference_raw(files->fdt) != fdt)) {
			fput(file);
			continue;
		}
		...
		if (unlikely(file->f_mode & mask)) {
			fput(file);
			return NULL;
		}
		...
		return file;
	}
}
```

1. `array_index_mask_nospec()` 生成一个掩码，防止越界的 fd 在推测执行中被用作数组下标（Spectre v1 缓解）。
2. 读出 `struct file` 指针后，`file_ref_get()` 尝试增加引用。若最后一个引用已被释放（文件已死亡），获取失败，重新读取表项。
3. 获取引用成功后，**再读一次** fd 表项与 `fdt` 指针，确认没有变化。由于 `SLAB_TYPESAFE_BY_RCU`，`file` 指向的内存可能在第 1 步和第 2 步之间被释放并分配给了另一个刚打开的文件；只有确认 fd 表中仍是同一个指针，才能断定拿到的是想要的那个文件。
4. 最后按 `mask` 过滤：`fget()`、`fdget()` 传入 `FMODE_PATH`，因此用 `O_PATH` 打开的文件不能被 `read()` 等普通系统调用使用。

### 读写位置的锁：fdget_pos()

`read()`、`write()`、`lseek()` 使用 `fdget_pos()`，在必要时获取 `f_pos_lock`：

[`fs/file.c:1200-1235`](../../linux/fs/file.c#L1200-L1235)（v6.18.60）

```c
static inline bool file_needs_f_pos_lock(struct file *file)
{
	if (!(file->f_mode & FMODE_ATOMIC_POS))
		return false;
	if (__file_ref_read_raw(&file->f_ref) != FILE_REF_ONEREF)
		return true;
	if (file->f_op->iterate_shared)
		return true;
	return false;
}
...
struct fd fdget_pos(unsigned int fd)
{
	struct fd f = fdget(fd);
	struct file *file = fd_file(f);

	if (likely(file) && file_needs_f_pos_lock(file)) {
		f.word |= FDPUT_POS_UNLOCK;
		mutex_lock(&file->f_pos_lock);
	}
	return f;
}
```

POSIX 要求对共享 `struct file` 的读写位置更新是原子的，例如两个线程同时 `read()` 同一个 fd 不能读到重复的数据。但加锁有成本，因此只在可能共享时才加：文件带有 `FMODE_ATOMIC_POS`（普通文件、目录）且引用计数不只一个（fd 表被共享或 `struct file` 被 `dup()`、`fork()` 共享），或者是目录（`readdir` 的位置必须正确维护）。

### 关闭：close() 与 fput()

[`fs/open.c:1574-1602`](../../linux/fs/open.c#L1574-L1602)（v6.18.60）

```c
SYSCALL_DEFINE1(close, unsigned int, fd)
{
	int retval;
	struct file *file;

	file = file_close_fd(fd);
	if (!file)
		return -EBADF;

	retval = filp_flush(file, current->files);

	/*
	 * We're returning to user space. Don't bother
	 * with any delayed fput() cases.
	 */
	fput_close_sync(file);

	if (likely(retval == 0))
		return 0;

	/* can't restart close syscall because file table entry was cleared */
	if (retval == -ERESTARTSYS ||
	    retval == -ERESTARTNOINTR ||
	    retval == -ERESTARTNOHAND ||
	    retval == -ERESTART_RESTARTBLOCK)
		retval = -EINTR;

	return retval;
}
```

`file_close_fd()` 在 `file_lock` 下把 fd 表项清空并释放 fd 号（[`fs/file.c:692-709`](../../linux/fs/file.c#L692-L709)），拿到原来的 `struct file`。`filp_flush()`（[`fs/open.c:1538-1556`](../../linux/fs/open.c#L1538-L1556)）调用 `->flush`（NFS 在此把脏数据写回服务器，以便 `close()` 能报告写错误）并释放该进程在此文件上持有的 POSIX 记录锁——注意，**POSIX 锁在进程关闭该文件的任何一个 fd 时都会释放**，这是 POSIX 锁的一个著名缺陷。

最后释放 `struct file` 的引用。`close()` 系统调用很快就要返回用户态，可以直接同步执行最终清理（`fput_close_sync()`）。一般情况下的 `fput()` 则把最终清理推迟：

[`fs/file_table.c:555-583`](../../linux/fs/file_table.c#L555-L583)（v6.18.60）

```c
static void __fput_deferred(struct file *file)
{
	struct task_struct *task = current;

	if (unlikely(!(file->f_mode & (FMODE_BACKING | FMODE_OPENED)))) {
		file_free(file);
		return;
	}

	if (likely(!in_interrupt() && !(task->flags & PF_KTHREAD))) {
		init_task_work(&file->f_task_work, ____fput);
		if (!task_work_add(task, &file->f_task_work, TWA_RESUME))
			return;
		/*
		 * After this task has run exit_task_work(),
		 * task_work_add() will fail.  Fall through to delayed
		 * fput to avoid leaking *file.
		 */
	}

	if (llist_add(&file->f_llist, &delayed_fput_list))
		schedule_delayed_work(&delayed_fput_work, 1);
}

void fput(struct file *file)
{
	if (unlikely(file_ref_put(&file->f_ref)))
		__fput_deferred(file);
}
```

最后一个引用释放时，若当前是普通进程，通过 `task_work_add()` 把 `____fput()` 挂到当前任务上，在返回用户态之前执行；中断上下文或内核线程则放入全局链表，由工作队列处理。推迟的原因是 `fput()` 可能在任意深度的调用栈、持有任意锁的情况下被调用，而最终清理可能执行很重的操作（`->release` 可能睡眠、`dput()` 可能触发 inode 驱逐与磁盘 I/O、`mntput()` 可能触发卸载）。在返回用户态前执行，栈很浅、不持有任何锁，最为安全。

[`fs/file_table.c:479-519`](../../linux/fs/file_table.c#L479-L519)（v6.18.60）

```c
static void __fput(struct file *file)
{
	struct dentry *dentry = file->f_path.dentry;
	struct vfsmount *mnt = file->f_path.mnt;
	struct inode *inode = file->f_inode;
	fmode_t mode = file->f_mode;

	if (unlikely(!(file->f_mode & FMODE_OPENED)))
		goto out;

	might_sleep();

	fsnotify_close(file);
	/*
	 * The function eventpoll_release() should be the first called
	 * in the file cleanup chain.
	 */
	eventpoll_release(file);
	locks_remove_file(file);

	security_file_release(file);
	if (unlikely(file->f_flags & FASYNC)) {
		if (file->f_op->fasync)
			file->f_op->fasync(-1, file, 0);
	}
	if (file->f_op->release)
		file->f_op->release(inode, file);
	if (unlikely(S_ISCHR(inode->i_mode) && inode->i_cdev != NULL &&
		     !(mode & FMODE_PATH))) {
		cdev_put(inode->i_cdev);
	}
	fops_put(file->f_op);
	file_f_owner_release(file);
	put_file_access(file);
	dput(dentry);
	if (unlikely(mode & FMODE_NEED_UNMOUNT))
		dissolve_on_fput(mnt);
	mntput(mnt);
out:
	file_free(file);
}
```

`__fput()` 按依赖关系的逆序释放资源：通知 fsnotify（`IN_CLOSE_WRITE` 等事件）、从 epoll 中移除、释放文件锁与租约、调用 `->release`、释放写访问权（`put_file_access()`）、释放 dentry 与挂载引用，最后把 `struct file` 还给 slab。

## 关键细节 / 设计考量

### fork() 与 exec() 中的 fd 表

`fork()` 时，若未指定 `CLONE_FILES`，`dup_fd()`（[`fs/file.c:383`](../../linux/fs/file.c#L383)）复制一份 fd 表，并为其中每个 `struct file` 增加引用——父子进程的 fd 共享同一组 `struct file`，也就共享读写位置。`execve()` 时，内核先确保 fd 表不被其他线程共享（必要时复制一份），再关闭所有设置了 `close_on_exec` 位的 fd。`O_CLOEXEC` 让设置该位与打开文件成为一个原子操作，避免了多线程程序中"另一个线程恰好在 `open()` 与 `fcntl(FD_CLOEXEC)` 之间执行了 `fork()` + `exec()`"导致 fd 泄漏的竞态。

### 一个 fd 被关闭时另一个线程正在使用它

线程 A 正在 `read(fd)` 中等待数据，线程 B 执行了 `close(fd)`。由于 A 在 `fdget()` 时（多线程情况下）已经获取了 `struct file` 的引用，B 的 `close()` 只清除 fd 表项并释放自己那一个引用，`struct file` 依然存活，A 的 `read()` 不受影响；A 返回、`fdput()` 后，最后一个引用才释放。对套接字这类可能永久阻塞的文件，应用若想打断 A，需要使用 `shutdown()` 而不是 `close()`。

### 系统级限制

| 限制 | 默认值 / 来源 | 作用 |
|---|---|---|
| `RLIMIT_NOFILE` | 通常软限制 1024、硬限制更高 | 单个进程的 fd 上限，`alloc_fd()` 的 `end` 参数 |
| `fs.nr_open` | 1048576 | `RLIMIT_NOFILE` 能设置的最大值，也是 fd 表扩容上限 |
| `fs.file-max` | 按内存计算（`files_maxfiles_init()`，[`fs/file_table.c:647-657`](../../linux/fs/file_table.c#L647-L657)） | 全系统 `struct file` 总数上限（`CAP_SYS_ADMIN` 不受限） |

## 动手实验

### 查看进程的 fd 与 struct file 状态

```bash
exec 3</etc/passwd
ls -l /proc/$$/fd
cat /proc/$$/fdinfo/3        # pos、flags（八进制 O_*）、mnt_id、ino
read -n 10 line <&3; cat /proc/$$/fdinfo/3   # pos 前进
exec 3<&-
```

### 验证共享读写位置

```bash
python3 - <<'EOF'
import os
fd = os.open("/etc/passwd", os.O_RDONLY)
fd2 = os.dup(fd)                     # 共享 struct file
os.read(fd, 10)
print("dup:", os.lseek(fd2, 0, os.SEEK_CUR))     # 10
fd3 = os.open("/etc/passwd", os.O_RDONLY)        # 独立的 struct file
print("open:", os.lseek(fd3, 0, os.SEEK_CUR))    # 0
EOF
```

### 跟踪 open 的慢速路径

```bash
sudo bpftrace -e '
kprobe:do_dentry_open {
	$f = (struct file *)arg0;
	@[comm, str($f->f_path.dentry->d_sb->s_type->name)] = count();
}
kretprobe:do_filp_open /(int64)retval < 0 && (int64)retval > -4096/ {
	@err[comm, -(int64)retval] = count();
}
interval:s:10 { exit(); }'
```

`@err` 中大量的 2（`ENOENT`）通常来自动态链接器与 `PATH` 搜索。

## 小结

- fd 是进程 fd 表的下标，指向 `struct file`（打开文件描述），后者指向 inode；`dup()`、`fork()` 共享 `struct file` 及其读写位置。
- fd 表初始内嵌 64 项，按需以 RCU 方式扩容；`alloc_fd()` 借助 `next_fd` 提示与两级位图快速找到最小可用 fd。
- `do_sys_openat2()` 先分配 fd、再 `do_filp_open()`、最后 `fd_install()`；最后一个分量由 `open_last_lookups()` / `lookup_open()` 处理，必要时在父目录独占锁下调用 `->create`（或 `->atomic_open`）。
- `do_dentry_open()` 为 `f_path` 取引用、获取写访问权、设置 `f_op` 并调用 `->open`，再计算 `FMODE_CAN_*` 与 `IOCB_*` 标志。
- `fdget()` 在 fd 表未共享时不增加引用；共享时在 RCU 下 `file_ref_get()` 并二次检查表项，以应对 `SLAB_TYPESAFE_BY_RCU` 带来的对象重用。
- `fput()` 把最终清理通过 `task_work` 推迟到返回用户态之前；`__fput()` 依次调用 `->release`、释放写访问权、dentry 与挂载引用。

## 参考

- `linux/Documentation/filesystems/files.rst`：fd 表的 RCU 设计
- `linux/Documentation/filesystems/vfs.rst`（struct file_operations 一节）
- `man 2 open`、`man 2 openat2`、`man 2 close`、`man 2 dup`
- Christian Brauner, "fs: add file_ref"（v6.13）
