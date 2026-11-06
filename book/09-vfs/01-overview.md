# 9.1 VFS 概述：通用文件模型与核心对象

> **本节源码**：[`include/linux/fs.h`](../../linux/include/linux/fs.h)、[`include/linux/dcache.h`](../../linux/include/linux/dcache.h)、[`include/linux/mount.h`](../../linux/include/linux/mount.h)、[`fs/mount.h`](../../linux/fs/mount.h)、[`include/linux/path.h`](../../linux/include/linux/path.h)、[`include/linux/fs_struct.h`](../../linux/include/linux/fs_struct.h)、[`include/linux/fdtable.h`](../../linux/include/linux/fdtable.h)、[`fs/read_write.c`](../../linux/fs/read_write.c)、[`fs/dcache.c`](../../linux/fs/dcache.c)、[`fs/ramfs/inode.c`](../../linux/fs/ramfs/inode.c)
> **关键函数**：`ksys_read()`、`vfs_read()`、`new_sync_read()`、`vfs_caches_init_early()`、`vfs_caches_init()`
> **前置知识**：4.5 SLUB 分配器、4.7 进程地址空间、7.9 RCU 原理与使用

## 概述

Unix 的设计信条是"一切皆文件"：普通文件、目录、设备、管道、套接字都通过 `open()`、`read()`、`write()`、`close()` 这一组统一接口访问。而在内核内部，这些"文件"背后的实现千差万别：ext4、XFS 把数据存放在块设备上，tmpfs 存放在内存里，procfs、sysfs 的内容由内核即时生成，NFS、FUSE 则要跨越网络或回到用户态。

**虚拟文件系统（Virtual File System，VFS）** 是夹在系统调用与具体文件系统之间的一层抽象。它定义了一套**通用文件模型**：所有文件系统都用超级块、inode、目录项、文件这几类对象来描述自己，并通过一组函数指针表（操作表）向 VFS 提供具体实现。系统调用只与通用模型打交道，具体文件系统只需实现自己关心的方法，其余交给 VFS 的通用代码。

VFS 不仅是一层接口转换。路径名查找、目录项缓存、inode 缓存、page cache、脏页写回、挂载树管理这些对所有文件系统都通用的机制都实现在 VFS 及与之紧密协作的 `mm/` 代码中，这正是本章要分析的内容。本节先建立全局图景：VFS 的分层位置、四大核心对象及其关系、操作表的分派方式，并以一次 `read()` 调用为例看它如何穿过 VFS。

```
            用户进程： open / read / write / mmap / stat / mount ...
  ──────────────────────────────── 系统调用 ────────────────────────────────
  ┌─────────────────────────────────── VFS ───────────────────────────────────┐
  │ 文件描述符表(9.6)  路径名查找(9.3)  dcache(9.4)  inode 缓存(9.5)  挂载树(9.2) │
  │ 读写分派(9.7)  page cache(9.8)  预读(9.9)  写回(9.10)  文件映射(9.11)        │
  └──────┬──────────────┬──────────────┬──────────────┬──────────────┬────────┘
         │ 操作表        │              │              │              │
     ext4 / XFS      tmpfs/ramfs    procfs/sysfs     NFS / FUSE    pipefs/sockfs
         │                                              │
     块设备层（第 10 章）                              网络栈（第 11 章）
```

*图 9-1：VFS 在内核中的位置（括号内为本章对应小节）*

## 通用文件模型

VFS 的通用文件模型由四类核心对象组成，另有两类对象把它们与挂载树、进程联系起来：

| 对象 | 结构体 | 代表什么 | 生命周期 | 操作表 |
|---|---|---|---|---|
| 超级块（Superblock） | `struct super_block` | 一个已挂载的文件系统实例 | 挂载时创建，最后一个挂载消失后销毁 | `super_operations` |
| inode | `struct inode` | 文件系统中的一个对象（文件、目录、符号链接、设备……）的元数据 | 首次访问时读入，缓存在内存中，回收时释放 | `inode_operations`、`file_operations`（默认） |
| 目录项（Directory Entry，dentry） | `struct dentry` | 路径中的一个名字分量，把名字与 inode 关联起来 | 查找时创建，缓存在 dcache 中 | `dentry_operations` |
| 文件 | `struct file` | 一次"打开"的结果：读写位置、打开模式、所用操作表 | `open()` 时创建，最后一个引用释放时销毁 | `file_operations` |
| 挂载 | `struct mount` / `struct vfsmount` | 某个文件系统（的某棵子树）挂在挂载树的某个位置 | `mount()` 时创建，`umount()` 后释放 | — |
| 路径 | `struct path` | （挂载，dentry）二元组，唯一确定挂载树中的一个位置 | 临时对象，随引用计数 | — |

此外，每个 inode 内嵌一个 `struct address_space`，用来组织该文件在内存中的数据页（page cache），其操作表 `address_space_operations` 负责页与存储介质之间的读写（9.8 节）。

需要特别区分 inode 与 dentry：inode 描述"对象本身"，dentry 描述"对象在目录树中的名字"。一个文件有多个硬链接时，只有一个 inode，却可以有多个 dentry；一个已删除但仍被打开的文件，inode 依然存在，却已没有可被查找到的名字。目录项的存在使得路径查找可以完全在内存中完成，而不必每次都去读磁盘上的目录。

## 核心数据结构

以下结构体都很大，这里只摘录与本章主线相关的字段。它们的定义上都带有 `__randomize_layout` 标注，开启 `CONFIG_RANDSTRUCT` 时编译器会打乱字段顺序以增加攻击难度；本书配置 `CONFIG_RANDSTRUCT_NONE=y`，字段按源码顺序排列。

### struct super_block

[`include/linux/fs.h:1446-1463`](../../linux/include/linux/fs.h#L1446-L1463)（v6.18.60）

```c
struct super_block {
	struct list_head	s_list;		/* Keep this first */
	dev_t			s_dev;		/* search index; _not_ kdev_t */
	unsigned char		s_blocksize_bits;
	unsigned long		s_blocksize;
	loff_t			s_maxbytes;	/* Max file size */
	struct file_system_type	*s_type;
	const struct super_operations	*s_op;
	const struct dquot_operations	*dq_op;
	const struct quotactl_ops	*s_qcop;
	const struct export_operations *s_export_op;
	unsigned long		s_flags;
	unsigned long		s_iflags;	/* internal SB_I_* flags */
	unsigned long		s_magic;
	struct dentry		*s_root;
	struct rw_semaphore	s_umount;
	int			s_count;
	atomic_t		s_active;
```

- `s_list`：挂入全局链表 `super_blocks`；`s_instances` 挂入所属 `file_system_type` 的 `fs_supers` 链表，同一类型的多个实例由此串在一起。
- `s_type`、`s_op`：所属文件系统类型和超级块操作表。
- `s_root`：该文件系统的根 dentry。
- `s_umount`：保护挂载、卸载、重新挂载等操作的读写信号量。
- `s_count` 与 `s_active`：两种引用计数。`s_active` 是"活跃引用"，每个挂载实例持有一个，降为 0 时文件系统被关闭；`s_count` 是"被动引用"，只保证结构体本身不被释放（9.2 节）。

后半部分还有几个重要字段：

[`include/linux/fs.h:1489-1587`](../../linux/include/linux/fs.h#L1489-L1587)（v6.18.60）

```c
	struct sb_writers	s_writers;

	/*
	 * Keep s_fs_info, s_time_gran, s_fsnotify_mask, and
	 * s_fsnotify_info together for cache efficiency. They are frequently
	 * accessed and rarely modified.
	 */
	void			*s_fs_info;	/* Filesystem private info */
	...
	struct user_namespace *s_user_ns;

	/*
	 * The list_lru structure is essentially just a pointer to a table
	 * of per-node lru lists, each of which has its own spinlock.
	 * There is no need to put them into separate cachelines.
	 */
	struct list_lru		s_dentry_lru;
	struct list_lru		s_inode_lru;
	struct rcu_head		rcu;
	struct work_struct	destroy_work;

	struct mutex		s_sync_lock;	/* sync serialisation lock */

	/*
	 * Indicates how deep in a filesystem stack this SB is
	 */
	int s_stack_depth;

	/* s_inode_list_lock protects s_inodes */
	spinlock_t		s_inode_list_lock ____cacheline_aligned_in_smp;
	struct list_head	s_inodes;	/* all inodes */

	spinlock_t		s_inode_wblist_lock;
	struct list_head	s_inodes_wb;	/* writeback inodes */
} __randomize_layout;
```

- `s_fs_info`：具体文件系统的私有数据，如 ext4 的 `struct ext4_sb_info`。
- `s_dentry_lru`、`s_inode_lru`：本文件系统的未使用 dentry 与 inode 的 LRU 链表，由超级块的 shrinker（`s_shrink`，[`include/linux/fs.h:1542`](../../linux/include/linux/fs.h#L1542)）在内存紧张时回收（9.4、9.5 节）。
- `s_inodes`：本文件系统的全部内存 inode。

### struct inode

[`include/linux/fs.h:788-812`](../../linux/include/linux/fs.h#L788-L812)（v6.18.60）

```c
/*
 * Keep mostly read-only and often accessed (especially for
 * the RCU path lookup and 'stat' data) fields at the beginning
 * of the 'struct inode'
 */
struct inode {
	umode_t			i_mode;
	unsigned short		i_opflags;
	kuid_t			i_uid;
	kgid_t			i_gid;
	unsigned int		i_flags;

#ifdef CONFIG_FS_POSIX_ACL
	struct posix_acl	*i_acl;
	struct posix_acl	*i_default_acl;
#endif

	const struct inode_operations	*i_op;
	struct super_block	*i_sb;
	struct address_space	*i_mapping;

#ifdef CONFIG_SECURITY
	void			*i_security;
#endif

```

结构体开头的注释说明了布局的意图：路径查找（尤其是 RCU 模式查找）与 `stat()` 频繁访问的只读字段集中放在前面，以减少缓存行访问。`i_mode` 记录文件类型与权限位，`i_op` 是 inode 操作表，`i_sb` 指回所属超级块，`i_mapping` 指向承载文件数据的 `address_space`（通常就是内嵌的 `i_data`）。

[`include/linux/fs.h:813-848`](../../linux/include/linux/fs.h#L813-L848)（v6.18.60）

```c
	/* Stat data, not accessed from path walking */
	unsigned long		i_ino;
	/*
	 * Filesystems may only read i_nlink directly.  They shall use the
	 * following functions for modification:
	 *
	 *    (set|clear|inc|drop)_nlink
	 *    inode_(inc|dec)_link_count
	 */
	union {
		const unsigned int i_nlink;
		unsigned int __i_nlink;
	};
	dev_t			i_rdev;
	loff_t			i_size;
	...
	/* Misc */
	enum inode_state_flags_t	i_state;
	/* 32-bit hole */
	struct rw_semaphore	i_rwsem;
```

- `i_ino`：inode 号，在同一文件系统内唯一。
- `i_nlink`：硬链接数，降为 0 意味着已无名字指向它。注释要求文件系统只读不写，修改必须通过 `set_nlink()` 等辅助函数，以便 VFS 维护超级块上的 `s_remove_count`。
- `i_size`：文件长度（字节）。
- `i_state`：inode 状态位，如 `I_NEW`、`I_DIRTY_*`、`I_FREEING`，受 `i_lock` 保护（9.5 节）。
- `i_rwsem`：inode 级的读写信号量，写文件、修改目录时持有。

[`include/linux/fs.h:850-883`](../../linux/include/linux/fs.h#L850-L883)（v6.18.60）

```c
	unsigned long		dirtied_when;	/* jiffies of first dirtying */
	unsigned long		dirtied_time_when;

	struct hlist_node	i_hash;
	struct list_head	i_io_list;	/* backing dev IO list */
#ifdef CONFIG_CGROUP_WRITEBACK
	struct bdi_writeback	*i_wb;		/* the associated cgroup wb */

	/* foreign inode detection, see wbc_detach_inode() */
	int			i_wb_frn_winner;
	u16			i_wb_frn_avg_time;
	u16			i_wb_frn_history;
#endif
	struct list_head	i_lru;		/* inode LRU list */
	struct list_head	i_sb_list;
	struct list_head	i_wb_list;	/* backing dev writeback list */
	union {
		struct hlist_head	i_dentry;
		struct rcu_head		i_rcu;
	};
	atomic64_t		i_version;
	atomic64_t		i_sequence; /* see futex */
	atomic_t		i_count;
	atomic_t		i_dio_count;
	atomic_t		i_writecount;
#if defined(CONFIG_IMA) || defined(CONFIG_FILE_LOCKING)
	atomic_t		i_readcount; /* struct files open RO */
#endif
	union {
		const struct file_operations	*i_fop;	/* former ->i_op->default_file_ops */
		void (*free_inode)(struct inode *);
	};
	struct file_lock_context	*i_flctx;
	struct address_space	i_data;
```

- `i_hash`：挂入全局 inode 哈希表，按（超级块，inode 号）查找（9.5 节）。
- `i_io_list`：挂入写回链表（9.10 节）。
- `i_lru`、`i_sb_list`：分别挂入超级块的 inode LRU 与全部 inode 链表。
- `i_dentry`：引用该 inode 的所有 dentry（"别名"）组成的链表。
- `i_count`：引用计数；`i_writecount` 记录写者数量（负数表示禁止写，例如正在执行的程序文件）。
- `i_fop`：默认文件操作表，`open()` 时拷贝到 `file->f_op`。
- `i_data`：内嵌的 `address_space`，即该文件的 page cache。

### struct dentry

[`include/linux/dcache.h:92-132`](../../linux/include/linux/dcache.h#L92-L132)（v6.18.60）

```c
struct dentry {
	/* RCU lookup touched fields */
	unsigned int d_flags;		/* protected by d_lock */
	seqcount_spinlock_t d_seq;	/* per dentry seqlock */
	struct hlist_bl_node d_hash;	/* lookup hash list */
	struct dentry *d_parent;	/* parent directory */
	union {
	struct qstr __d_name;		/* for use ONLY in fs/dcache.c */
	const struct qstr d_name;
	};
	struct inode *d_inode;		/* Where the name belongs to - NULL is
					 * negative */
	union shortname_store d_shortname;
	/* --- cacheline 1 boundary (64 bytes) was 32 bytes ago --- */

	/* Ref lookup also touches following */
	const struct dentry_operations *d_op;
	struct super_block *d_sb;	/* The root of the dentry tree */
	unsigned long d_time;		/* used by d_revalidate */
	void *d_fsdata;			/* fs-specific data */
	/* --- cacheline 2 boundary (128 bytes) --- */
	struct lockref d_lockref;	/* per-dentry lock and refcount
					 * keep separate from RCU lookup area if
					 * possible!
					 */

	union {
		struct list_head d_lru;		/* LRU list */
		wait_queue_head_t *d_wait;	/* in-lookup ones only */
	};
	struct hlist_node d_sib;	/* child of parent list */
	struct hlist_head d_children;	/* our children */
	/*
	 * d_alias and d_rcu can share memory
	 */
	union {
		struct hlist_node d_alias;	/* inode alias list */
		struct hlist_bl_node d_in_lookup_hash;	/* only for in-lookup ones */
	 	struct rcu_head d_rcu;
	} d_u;
};
```

- 前半部分是 RCU 路径查找要访问的字段：`d_flags`、用于无锁一致性检查的顺序计数 `d_seq`、哈希链节点 `d_hash`、父目录 `d_parent`、名字 `d_name`、指向 inode 的 `d_inode`（为 `NULL` 表示**负目录项**，即"这个名字不存在"的缓存）。
- `d_shortname`：短名字（x86_64 上不超过 39 字节）直接内嵌在 dentry 中，较长的名字另行分配。
- `d_lockref`：自旋锁与引用计数合二为一的 `struct lockref`，在无竞争时可以用一次 cmpxchg 同时完成"检查未加锁 + 计数加一"（7.2 节）。
- `d_lru`：未被使用时挂入超级块的 dentry LRU。
- `d_sib`、`d_children`：兄弟链表与子节点链表，组成内存中的目录树。
- `d_u.d_alias`：挂入 inode 的 `i_dentry` 链表。

注释中的缓存行标记表明，在 64 位配置下 `struct dentry` 恰好是 192 字节（3 个缓存行）。

### struct file

[`include/linux/fs.h:1211-1252`](../../linux/include/linux/fs.h#L1211-L1252)（v6.18.60）

```c
struct file {
	spinlock_t			f_lock;
	fmode_t				f_mode;
	const struct file_operations	*f_op;
	struct address_space		*f_mapping;
	void				*private_data;
	struct inode			*f_inode;
	unsigned int			f_flags;
	unsigned int			f_iocb_flags;
	const struct cred		*f_cred;
	struct fown_struct		*f_owner;
	/* --- cacheline 1 boundary (64 bytes) --- */
	union {
		const struct path	f_path;
		struct path		__f_path;
	};
	union {
		/* regular files (with FMODE_ATOMIC_POS) and directories */
		struct mutex		f_pos_lock;
		/* pipes */
		u64			f_pipe;
	};
	loff_t				f_pos;
#ifdef CONFIG_SECURITY
	void				*f_security;
#endif
	/* --- cacheline 2 boundary (128 bytes) --- */
	errseq_t			f_wb_err;
	errseq_t			f_sb_err;
#ifdef CONFIG_EPOLL
	struct hlist_head		*f_ep;
#endif
	union {
		struct callback_head	f_task_work;
		struct llist_node	f_llist;
		struct file_ra_state	f_ra;
		freeptr_t		f_freeptr;
	};
	file_ref_t			f_ref;
	/* --- cacheline 3 boundary (192 bytes) --- */
} __randomize_layout
  __attribute__((aligned(4)));	/* lest something weird decides that 2 is OK */
```

- `f_mode`：`FMODE_READ`、`FMODE_WRITE`、`FMODE_CAN_READ` 等模式位，热路径上判断能否读写只看它。
- `f_op`：文件操作表，`open()` 时从 `inode->i_fop` 取得，之后 `read()`、`write()` 等都经它分派。
- `f_mapping`：文件数据所在的 `address_space`。
- `f_path`：打开时的（挂载，dentry），保证文件打开期间所在的挂载与 dentry 都不会被释放。
- `f_pos` 与 `f_pos_lock`：文件读写位置及保护它的互斥锁（9.7 节）。
- `f_ra`：预读状态（9.9 节），与 `f_task_work`、`f_llist` 共用一个 union——后两者只在文件即将释放时使用。
- `f_ref`：引用计数，类型为 v6.13 引入的 `file_ref_t`（9.6 节）。

`struct file` 与 inode 是多对一关系：同一文件每打开一次就有一个新的 `struct file`，它们共享 inode，却各有独立的读写位置。POSIX 称之为**打开文件描述（Open File Description）**；文件描述符只是进程 fd 表中指向它的下标。

### vfsmount 与 path

[`include/linux/mount.h:58-63`](../../linux/include/linux/mount.h#L58-L63)（v6.18.60）

```c
struct vfsmount {
	struct dentry *mnt_root;	/* root of the mounted tree */
	struct super_block *mnt_sb;	/* pointer to superblock */
	int mnt_flags;
	struct mnt_idmap *mnt_idmap;
} __randomize_layout;
```

[`include/linux/path.h:8-11`](../../linux/include/linux/path.h#L8-L11)（v6.18.60）

```c
struct path {
	struct vfsmount *mnt;
	struct dentry *dentry;
} __randomize_layout;
```

`struct vfsmount` 是挂载对外公开的部分，完整的 `struct mount`（定义在 VFS 私有头文件 `fs/mount.h` 中）把它内嵌为 `mnt` 字段，再通过 `real_mount()` 用 `container_of()` 取回（9.2 节）。`struct path` 由（挂载，dentry）组成：同一个 dentry 可以通过绑定挂载出现在挂载树的多个位置，只有加上挂载才能唯一确定一个位置。

### 进程视角：fs_struct 与 files_struct

[`include/linux/fs_struct.h:9-15`](../../linux/include/linux/fs_struct.h#L9-L15)（v6.18.60）

```c
struct fs_struct {
	int users;
	seqlock_t seq;
	int umask;
	int in_exec;
	struct path root, pwd;
} __randomize_layout;
```

[`include/linux/fdtable.h:26-57`](../../linux/include/linux/fdtable.h#L26-L57)（v6.18.60）

```c
struct fdtable {
	unsigned int max_fds;
	struct file __rcu **fd;      /* current fd array */
	unsigned long *close_on_exec;
	unsigned long *open_fds;
	unsigned long *full_fds_bits;
	struct rcu_head rcu;
};

/*
 * Open file table structure
 */
struct files_struct {
  /*
   * read mostly part
   */
	atomic_t count;
	bool resize_in_progress;
	wait_queue_head_t resize_wait;

	struct fdtable __rcu *fdt;
	struct fdtable fdtab;
  /*
   * written part on a separate cache line in SMP
   */
	spinlock_t file_lock ____cacheline_aligned_in_smp;
	unsigned int next_fd;
	unsigned long close_on_exec_init[1];
	unsigned long open_fds_init[1];
	unsigned long full_fds_bits_init[1];
	struct file __rcu * fd_array[NR_OPEN_DEFAULT];
};
```

`task_struct` 中的 `fs` 指向 `fs_struct`，记录进程的根目录 `root`、当前工作目录 `pwd` 与 `umask`，路径查找以它们为起点（9.3 节）；`files` 指向 `files_struct`，其中的 `fdtable` 把文件描述符映射到 `struct file`（9.6 节）。两者都可以在线程间共享（`clone()` 的 `CLONE_FS`、`CLONE_FILES`）。

把以上对象连起来，就得到图 9-2：

```
 task_struct
  ├── fs ────▶ fs_struct { root, pwd }        // 路径查找的起点（struct path）
  └── files ─▶ files_struct ─▶ fdtable
                                └── fd[N] ─▶ struct file
                                              ├── f_op ─────────▶ file_operations
                                              ├── f_path ───────▶ struct path { mnt, dentry }
                                              ├── f_inode ──┐                  │      │
                                              └── f_mapping │     ┌────────────┘      │
                                                     │      │     ▼                   ▼
                                                     │      │  struct mount      struct dentry ◀── dcache 哈希表
                                                     │      │   ├── mnt_root      ├── d_parent ──▶ 父目录 dentry
                                                     │      │   └── mnt_sb ─┐     ├── d_op
                                                     │      │               │     └── d_inode ──┐
                                                     │      ▼               │                   │
                                                     │   struct inode ◀─────┼───────────────────┘   ◀── inode 哈希表
                                                     │    ├── i_op / i_fop  │
                                                     │    ├── i_dentry ─────┼──▶ 所有别名 dentry
                                                     │    ├── i_sb ─────────┤
                                                     └───▶└── i_data        ▼
                                                          (address_space)  struct super_block
                                                           ├── a_ops        ├── s_root ──▶ 根 dentry
                                                           └── i_pages      ├── s_op
                                                              (page cache)  └── s_type ──▶ file_system_type
```

*图 9-2：VFS 核心对象之间的关系*

## 操作表：用 C 实现的多态

VFS 采用"结构体 + 函数指针表"的方式实现多态：每类对象都有一个 `xxx_operations` 指针，具体文件系统在创建对象时填入自己的表。

### 五张操作表

| 操作表 | 挂在 | 典型方法 | 由谁调用 |
|---|---|---|---|
| `super_operations` | `sb->s_op` | `alloc_inode`、`write_inode`、`evict_inode`、`sync_fs`、`statfs` | inode 生命周期管理、写回、`sync`、`statfs()` |
| `inode_operations` | `inode->i_op` | `lookup`、`create`、`mkdir`、`unlink`、`rename`、`get_link`、`permission`、`setattr`、`getattr` | 路径查找、目录操作、属性操作 |
| `file_operations` | `file->f_op`（默认来自 `inode->i_fop`） | `read_iter`、`write_iter`、`mmap_prepare`、`open`、`release`、`fsync`、`iterate_shared`、`unlocked_ioctl` | 对已打开文件的操作 |
| `dentry_operations` | `dentry->d_op` | `d_revalidate`、`d_hash`、`d_compare`、`d_delete`、`d_release` | dcache 查找与回收 |
| `address_space_operations` | `mapping->a_ops` | `read_folio`、`readahead`、`writepages`、`write_begin`、`write_end`、`dirty_folio` | page cache 与存储之间的数据传输 |

以 `file_operations` 为例（只摘录常用方法）：

[`include/linux/fs.h:2271-2315`](../../linux/include/linux/fs.h#L2271-L2315)（v6.18.60）

```c
struct file_operations {
	struct module *owner;
	fop_flags_t fop_flags;
	loff_t (*llseek) (struct file *, loff_t, int);
	ssize_t (*read) (struct file *, char __user *, size_t, loff_t *);
	ssize_t (*write) (struct file *, const char __user *, size_t, loff_t *);
	ssize_t (*read_iter) (struct kiocb *, struct iov_iter *);
	ssize_t (*write_iter) (struct kiocb *, struct iov_iter *);
	int (*iopoll)(struct kiocb *kiocb, struct io_comp_batch *,
			unsigned int flags);
	int (*iterate_shared) (struct file *, struct dir_context *);
	__poll_t (*poll) (struct file *, struct poll_table_struct *);
	long (*unlocked_ioctl) (struct file *, unsigned int, unsigned long);
	long (*compat_ioctl) (struct file *, unsigned int, unsigned long);
	int (*mmap) (struct file *, struct vm_area_struct *);
	int (*open) (struct inode *, struct file *);
	int (*flush) (struct file *, fl_owner_t id);
	int (*release) (struct inode *, struct file *);
	int (*fsync) (struct file *, loff_t, loff_t, int datasync);
	...
	int (*mmap_prepare)(struct vm_area_desc *);
} __randomize_layout;
```

`->read`/`->write` 是早期的接口，只能处理单个用户缓冲区；`->read_iter`/`->write_iter` 基于 `iov_iter`，是现代文件系统应当实现的接口（9.7 节）。类似地，`->mmap` 已被标记为弃用（见 `Documentation/filesystems/porting.rst`），由新近引入的 `->mmap_prepare` 取代：后者在 VMA 真正创建之前调用，只能修改一个描述符 `struct vm_area_desc`，而不是直接操作 VMA，两者不可同时提供（`can_mmap_file()`，[`include/linux/fs.h:2384-2396`](../../linux/include/linux/fs.h#L2384-L2396)）。

### 以 ramfs 和 ext4 为例

ramfs 是内核中最简单的可读写文件系统（[`fs/ramfs/inode.c`](../../linux/fs/ramfs/inode.c) 只有约 330 行），源码开头的注释特意指出它更适合作为"如何编写一个虚拟文件系统"的示例。它在创建 inode 时按文件类型填入不同的操作表：

[`fs/ramfs/inode.c:57-91`](../../linux/fs/ramfs/inode.c#L57-L91)（v6.18.60）

```c
struct inode *ramfs_get_inode(struct super_block *sb,
				const struct inode *dir, umode_t mode, dev_t dev)
{
	struct inode * inode = new_inode(sb);

	if (inode) {
		inode->i_ino = get_next_ino();
		inode_init_owner(&nop_mnt_idmap, inode, dir, mode);
		inode->i_mapping->a_ops = &ram_aops;
		mapping_set_gfp_mask(inode->i_mapping, GFP_HIGHUSER);
		mapping_set_unevictable(inode->i_mapping);
		simple_inode_init_ts(inode);
		switch (mode & S_IFMT) {
		default:
			init_special_inode(inode, mode, dev);
			break;
		case S_IFREG:
			inode->i_op = &ramfs_file_inode_operations;
			inode->i_fop = &ramfs_file_operations;
			break;
		case S_IFDIR:
			inode->i_op = &ramfs_dir_inode_operations;
			inode->i_fop = &simple_dir_operations;

			/* directory inodes start off with i_nlink == 2 (for "." entry) */
			inc_nlink(inode);
			break;
		case S_IFLNK:
			inode->i_op = &page_symlink_inode_operations;
			inode_nohighmem(inode);
			break;
		}
	}
	return inode;
}
```

普通文件使用的 `ramfs_file_operations` 几乎全部由 VFS 的通用实现组成：

[`fs/ramfs/file-mmu.c:41-50`](../../linux/fs/ramfs/file-mmu.c#L41-L50)（v6.18.60）

```c
const struct file_operations ramfs_file_operations = {
	.read_iter	= generic_file_read_iter,
	.write_iter	= generic_file_write_iter,
	.mmap_prepare	= generic_file_mmap_prepare,
	.fsync		= noop_fsync,
	.splice_read	= filemap_splice_read,
	.splice_write	= iter_file_splice_write,
	.llseek		= generic_file_llseek,
	.get_unmapped_area	= ramfs_mmu_get_unmapped_area,
};
```

对比 ext4，它需要处理日志、DAX、直接 I/O 等，提供了大量自己的实现，但最终的缓冲读仍回到通用的 `generic_file_read_iter()`：

[`fs/ext4/file.c:970-990`](../../linux/fs/ext4/file.c#L970-L990)（v6.18.60）

```c
const struct file_operations ext4_file_operations = {
	.llseek		= ext4_llseek,
	.read_iter	= ext4_file_read_iter,
	.write_iter	= ext4_file_write_iter,
	.iopoll		= iocb_bio_iopoll,
	.unlocked_ioctl = ext4_ioctl,
#ifdef CONFIG_COMPAT
	.compat_ioctl	= ext4_compat_ioctl,
#endif
	.mmap_prepare	= ext4_file_mmap_prepare,
	.open		= ext4_file_open,
	.release	= ext4_release_file,
	.fsync		= ext4_sync_file,
	.get_unmapped_area = thp_get_unmapped_area,
	.splice_read	= ext4_file_splice_read,
	.splice_write	= iter_file_splice_write,
	.fallocate	= ext4_fallocate,
	.fop_flags	= FOP_MMAP_SYNC | FOP_BUFFER_RASYNC |
			  FOP_DIO_PARALLEL_WRITE |
			  FOP_DONTCACHE,
};
```

[`fs/ext4/file.c:130-148`](../../linux/fs/ext4/file.c#L130-L148)（v6.18.60）

```c
static ssize_t ext4_file_read_iter(struct kiocb *iocb, struct iov_iter *to)
{
	struct inode *inode = file_inode(iocb->ki_filp);

	if (unlikely(ext4_forced_shutdown(inode->i_sb)))
		return -EIO;

	if (!iov_iter_count(to))
		return 0; /* skip atime */

#ifdef CONFIG_FS_DAX
	if (IS_DAX(inode))
		return ext4_dax_read_iter(iocb, to);
#endif
	if (iocb->ki_flags & IOCB_DIRECT)
		return ext4_dio_read_iter(iocb, to);

	return generic_file_read_iter(iocb, to);
}
```

这体现了 VFS 的一条重要设计原则：**通用代码提供默认实现，文件系统可以完全替换、部分包装，也可以直接复用**。另一方面，VFS 在调用方法前通常会检查指针是否为空：方法缺省时要么采用默认行为（如没有 `->alloc_inode` 时直接从 `inode_cachep` 分配），要么返回错误（如没有 `->mkdir` 时返回 `-EPERM`）。

### 嵌入与 container_of

具体文件系统通常需要在 inode 上附加自己的字段。做法是把 `struct inode` 嵌入更大的结构体，由 `->alloc_inode` 分配整个大结构，再用 `container_of()` 在两者之间转换：

[`fs/ext4/ext4.h:1796-1799`](../../linux/fs/ext4/ext4.h#L1796-L1799)（v6.18.60）

```c
static inline struct ext4_inode_info *EXT4_I(struct inode *inode)
{
	return container_of(inode, struct ext4_inode_info, vfs_inode);
}
```

[`fs/ext4/super.c:1393-1399`](../../linux/fs/ext4/super.c#L1393-L1399)（v6.18.60）

```c
static struct inode *ext4_alloc_inode(struct super_block *sb)
{
	struct ext4_inode_info *ei;

	ei = alloc_inode_sb(sb, ext4_inode_cachep, GFP_NOFS);
	if (!ei)
		return NULL;
```

超级块的私有数据走另一条路：通过 `sb->s_fs_info` 指针挂接。文件的私有数据挂在 `file->private_data`，dentry 的挂在 `d_fsdata`。

## 主流程分析：一次 read() 如何穿过 VFS

以从 ext4 上的普通文件读取数据为例，调用链如下：

```
read(fd, buf, count)
└── ksys_read()                         fs/read_write.c:704
    ├── CLASS(fd_pos, f)(fd)            // fdget_pos()：fd → struct file，必要时锁 f_pos
    └── vfs_read()                      fs/read_write.c:552
        ├── rw_verify_area()            // 位置与长度检查、LSM、fsnotify 权限事件
        ├── file->f_op->read()          // 旧式接口（少数驱动）
        └── new_sync_read()             fs/read_write.c:481
            └── file->f_op->read_iter() // ext4_file_read_iter()
                └── generic_file_read_iter()   mm/filemap.c:2911
                    └── filemap_read()          // page cache（9.8 节）
                        ├── page_cache_sync_ra()   // 未命中：同步预读（9.9 节）
                        │   └── a_ops->readahead() // ext4_readahead() → 块设备层
                        └── copy_folio_to_iter()   // 拷贝到用户缓冲区
```

### 系统调用入口

[`fs/read_write.c:704-725`](../../linux/fs/read_write.c#L704-L725)（v6.18.60）

```c
ssize_t ksys_read(unsigned int fd, char __user *buf, size_t count)
{
	CLASS(fd_pos, f)(fd);
	ssize_t ret = -EBADF;

	if (!fd_empty(f)) {
		loff_t pos, *ppos = file_ppos(fd_file(f));
		if (ppos) {
			pos = *ppos;
			ppos = &pos;
		}
		ret = vfs_read(fd_file(f), buf, count, ppos);
		if (ret >= 0 && ppos)
			fd_file(f)->f_pos = pos;
	}
	return ret;
}

SYSCALL_DEFINE3(read, unsigned int, fd, char __user *, buf, size_t, count)
{
	return ksys_read(fd, buf, count);
}
```

`CLASS(fd_pos, f)(fd)` 使用内核的作用域清理机制（`include/linux/cleanup.h`）：它在进入作用域时调用 `fdget_pos()` 把文件描述符转换为 `struct file`，离开作用域时自动调用 `fdput_pos()`。`file_ppos()` 对管道、套接字这类没有位置概念的"流"文件（`FMODE_STREAM`）返回 `NULL`。注意位置的处理方式：先把 `f_pos` 拷贝到局部变量，读完再写回，这样 `vfs_read()` 本身不必关心位置是来自 `f_pos` 还是来自 `pread()` 的参数。

### vfs_read()：检查与分派

[`fs/read_write.c:552-581`](../../linux/fs/read_write.c#L552-L581)（v6.18.60）

```c
ssize_t vfs_read(struct file *file, char __user *buf, size_t count, loff_t *pos)
{
	ssize_t ret;

	if (!(file->f_mode & FMODE_READ))
		return -EBADF;
	if (!(file->f_mode & FMODE_CAN_READ))
		return -EINVAL;
	if (unlikely(!access_ok(buf, count)))
		return -EFAULT;

	ret = rw_verify_area(READ, file, pos, count);
	if (ret)
		return ret;
	if (count > MAX_RW_COUNT)
		count =  MAX_RW_COUNT;

	if (file->f_op->read)
		ret = file->f_op->read(file, buf, count, pos);
	else if (file->f_op->read_iter)
		ret = new_sync_read(file, buf, count, pos);
	else
		ret = -EINVAL;
	if (ret > 0) {
		fsnotify_access(file);
		add_rchar(current, ret);
	}
	inc_syscr(current);
	return ret;
}
```

`vfs_read()` 依次完成：

1. 检查文件是否以读方式打开（`FMODE_READ`），以及是否提供了读方法（`FMODE_CAN_READ`，在打开时根据 `f_op` 计算，9.6 节）。
2. `access_ok()` 检查用户缓冲区地址是否在用户空间范围内。
3. `rw_verify_area()` 检查读写位置是否合法，并调用 LSM 钩子 `security_file_permission()` 与 fsnotify 的权限事件（fanotify 可以在此拦截访问）。
4. 把单次读写长度限制在 `MAX_RW_COUNT`（`INT_MAX & PAGE_MASK`，约 2 GB）以内。
5. 优先调用旧式的 `->read`，否则调用 `->read_iter`。
6. 读取成功后发出 fsnotify 访问事件（inotify 的 `IN_ACCESS`），并累加进程 I/O 统计（`/proc/PID/io` 中的 `rchar`、`syscr`）。

### new_sync_read()：统一到 iov_iter

[`fs/read_write.c:481-496`](../../linux/fs/read_write.c#L481-L496)（v6.18.60）

```c
static ssize_t new_sync_read(struct file *filp, char __user *buf, size_t len, loff_t *ppos)
{
	struct kiocb kiocb;
	struct iov_iter iter;
	ssize_t ret;

	init_sync_kiocb(&kiocb, filp);
	kiocb.ki_pos = (ppos ? *ppos : 0);
	iov_iter_ubuf(&iter, ITER_DEST, buf, len);

	ret = filp->f_op->read_iter(&kiocb, &iter);
	BUG_ON(ret == -EIOCBQUEUED);
	if (ppos)
		*ppos = kiocb.ki_pos;
	return ret;
}
```

`->read_iter` 是现代文件系统实现的读接口。它不接收"用户缓冲区 + 长度"，而是接收一个 `struct kiocb`（描述本次 I/O 的上下文：文件、位置、标志、完成回调）和一个 `struct iov_iter`（描述数据的来源或去向，可以是单个用户缓冲区、iovec 数组、内核 kvec、bio_vec 等）。`new_sync_read()` 把 `read()` 的参数包装成这两个结构，于是 `read()`、`readv()`、`pread()`、`io_uring`、AIO 以及内核内部的 `kernel_read()` 都能复用同一个 `->read_iter` 实现。这部分在 9.7 节详细分析。

## VFS 的初始化

VFS 的各类缓存在启动早期创建。`start_kernel()` 中先调用 `vfs_caches_init_early()`（[`init/main.c:928`](../../linux/init/main.c#L928)），在内存管理初始化之后再调用 `vfs_caches_init()`（[`init/main.c:1053`](../../linux/init/main.c#L1053)）：

[`fs/dcache.c:3255-3278`](../../linux/fs/dcache.c#L3255-L3278)（v6.18.60）

```c
void __init vfs_caches_init_early(void)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(in_lookup_hashtable); i++)
		INIT_HLIST_BL_HEAD(&in_lookup_hashtable[i]);

	dcache_init_early();
	inode_init_early();
}

void __init vfs_caches_init(void)
{
	names_cachep = kmem_cache_create_usercopy("names_cache", PATH_MAX, 0,
			SLAB_HWCACHE_ALIGN|SLAB_PANIC, 0, PATH_MAX, NULL);

	dcache_init();
	inode_init();
	files_init();
	files_maxfiles_init();
	mnt_init();
	bdev_cache_init();
	chrdev_init();
}
```

| 函数 | 创建的对象 |
|---|---|
| `kmem_cache_create_usercopy("names_cache")` | 存放从用户态拷入的路径名（`PATH_MAX` = 4096 字节） |
| `dcache_init()` | `dentry` slab 缓存与 dentry 哈希表（9.4 节） |
| `inode_init()` | `inode_cache` slab 缓存与 inode 哈希表（9.5 节） |
| `files_init()` / `files_maxfiles_init()` | `filp` slab 缓存，并按内存大小计算系统最大打开文件数 `file-max`（9.6 节） |
| `mnt_init()` | `mnt_cache`、挂载哈希表，初始化 sysfs、tmpfs，并创建 rootfs 作为初始挂载树（9.2 节） |
| `bdev_cache_init()` | 块设备伪文件系统 bdevfs |
| `chrdev_init()` | 字符设备映射表 |

dentry 与 inode 哈希表的大小按内存容量计算。若启用 `hashdist`（NUMA 系统默认把大哈希表分散到各节点），哈希表的分配会推迟到 `vfs_caches_init()` 中用 vmalloc 完成，否则在 `*_init_early()` 中用 memblock 分配。启动日志中的 `Dentry cache hash table entries`、`Inode-cache hash table entries`、`Mount-cache hash table entries` 就来自这里。

## 关键细节 / 设计考量

### 缓存与回收

dentry 与 inode 本质上都是磁盘元数据在内存中的缓存。文件关闭、进程退出后，它们并不立即释放，而是留在 LRU 上等待再次使用；只有内存紧张时，超级块的 shrinker（`super_cache_scan()`）才按比例回收（9.4、9.5 节）。由于 dentry 持有对 inode 的引用，必须先回收 dentry，inode 才可能变为未使用。sysctl `vm.vfs_cache_pressure` 调节这两类缓存相对 page cache 的回收倾向。

### 多种引用计数

VFS 对象的生命周期由各自的引用计数管理，它们的实现方式各不相同，反映了不同的访问模式：

| 对象 | 计数 | 实现 | 说明 |
|---|---|---|---|
| dentry | `d_lockref.count` | `struct lockref` | 与 `d_lock` 合并，无竞争时一次 cmpxchg 完成 |
| inode | `i_count` | `atomic_t` | 归零时由 `iput_final()` 决定缓存还是销毁 |
| file | `f_ref` | `file_ref_t` | 配合 `SLAB_TYPESAFE_BY_RCU`，支持在 RCU 下安全地"尝试获取" |
| mount | `mnt_pcp->mnt_count` | per-CPU 计数 | 挂载被极其频繁地 get/put，per-CPU 避免缓存行争用 |
| super_block | `s_active` / `s_count` | `atomic_t` / `int` | 活跃引用与被动引用分离 |

### 锁的层次

VFS 中的锁种类繁多，下表列出本章会反复遇到的几种，更完整的规则见 `Documentation/filesystems/locking.rst` 与 `directory-locking.rst`：

| 锁 | 类型 | 保护对象 |
|---|---|---|
| `inode->i_rwsem` | 读写信号量 | 文件内容的写入；目录的修改（查找时持读锁，创建 / 删除时持写锁） |
| `inode->i_lock` | 自旋锁 | `i_state`、`i_dentry` 等 inode 字段 |
| `dentry->d_lock` | 自旋锁（在 `d_lockref` 中） | dentry 的名字、父指针、标志、引用计数 |
| `d_seq` | 顺序计数 | RCU 查找时检测 dentry 是否被并发修改 |
| `rename_lock` | 顺序锁（全局） | 跨目录重命名，供查找检测并发重命名 |
| `mount_lock` | 顺序锁（全局） | 挂载哈希表与挂载树 |
| `namespace_sem` | 读写信号量（全局） | 挂载命名空间的修改 |
| `sb->s_umount` | 读写信号量 | 超级块的挂载、卸载、重新挂载、冻结 |
| `file->f_pos_lock` | 互斥锁 | 共享 `struct file` 时的读写位置 |
| `mapping->invalidate_lock` | 读写信号量 | page cache 内容与文件块映射的一致性（截断、打洞时） |

### 一切皆文件的实现

管道、套接字、eventfd、epoll 等对象并不位于任何磁盘文件系统中，但同样需要 inode 与 `struct file`。内核为它们准备了一组**伪文件系统**：pipefs、sockfs、anon_inodefs 等，在启动时用 `kern_mount()` 挂载在内核内部、不出现在任何挂载命名空间里。`pipe()`、`socket()` 等系统调用在这些伪文件系统中创建 inode 与 file，于是 `read()`、`write()`、`close()`、`poll()` 都能经由同一套 VFS 接口工作。

## 动手实验

以下实验在 dev 虚拟机中以 root 运行。

### 查看已注册的文件系统与挂载

```bash
cat /proc/filesystems              # 已注册的文件系统类型，nodev 表示不需要块设备
cat /proc/self/mountinfo | head    # 挂载 ID、父挂载 ID、设备号、根、挂载点、选项
findmnt --tree                     # 以树形显示挂载树
```

### 观察 VFS 对象缓存

```bash
cat /proc/sys/fs/dentry-state      # dentry 总数、未使用数、age_limit、want_pages、负 dentry 数
cat /proc/sys/fs/inode-nr          # inode 总数与空闲数
cat /proc/sys/fs/file-nr           # 已分配 file 数、0、file-max
grep -E "^(dentry|inode_cache|filp|mnt_cache|ext4_inode_cache) " /proc/slabinfo
```

### 按文件系统类型统计 vfs_read() 调用

```bash
sudo bpftrace -e '
kprobe:vfs_read {
	$f = (struct file *)arg0;
	@[str($f->f_path.dentry->d_sb->s_type->name)] = count();
}
interval:s:5 { exit(); }'
```

输出中常见的 `proc`、`sysfs`、`pipefs`、`sockfs`、`ext4` 等，直观展示了"一切皆文件"背后各种文件系统共用同一个 `vfs_read()` 入口。

## 小结

- VFS 是系统调用与具体文件系统之间的抽象层，定义了由超级块、inode、dentry、file 组成的通用文件模型，并实现了路径查找、对象缓存、page cache、写回等通用机制。
- inode 描述文件对象本身，dentry 描述目录树中的名字，file 描述一次打开；`struct path`（挂载，dentry）唯一确定挂载树中的位置。
- 五张操作表（`super_operations`、`inode_operations`、`file_operations`、`dentry_operations`、`address_space_operations`）以函数指针实现多态，文件系统可以替换、包装或直接复用通用实现。
- 具体文件系统通过把 `struct inode` 嵌入私有结构、配合 `container_of()` 扩展 inode。
- `read()` 经 `ksys_read()` → `vfs_read()` → `new_sync_read()` 被统一为 `->read_iter(kiocb, iov_iter)` 接口，多数文件系统最终进入通用的 `filemap_read()`。
- dentry、inode、file、mount、super_block 各有适合自身访问模式的引用计数实现。

## 参考

- `linux/Documentation/filesystems/vfs.rst`：VFS 对象与操作表的官方说明
- `linux/Documentation/filesystems/locking.rst`：各操作表方法的加锁规则
- `linux/Documentation/filesystems/porting.rst`：VFS 接口的历史变更记录
- `linux/Documentation/filesystems/ramfs-rootfs-initramfs.rst`
- Neil Brown, "Pathname lookup in Linux", LWN, 2015（`linux/Documentation/filesystems/path-lookup.rst` 为其整理版本）
