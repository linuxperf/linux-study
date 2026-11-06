# 9.2 文件系统注册与挂载

> **本节源码**：[`fs/filesystems.c`](../../linux/fs/filesystems.c)、[`include/linux/fs_context.h`](../../linux/include/linux/fs_context.h)、[`fs/fs_context.c`](../../linux/fs/fs_context.c)、[`fs/fsopen.c`](../../linux/fs/fsopen.c)、[`fs/super.c`](../../linux/fs/super.c)、[`fs/namespace.c`](../../linux/fs/namespace.c)、[`fs/mount.h`](../../linux/fs/mount.h)、[`fs/ramfs/inode.c`](../../linux/fs/ramfs/inode.c)
> **关键函数**：`register_filesystem()`、`get_fs_type()`、`path_mount()`、`do_new_mount()`、`alloc_fs_context()`、`vfs_get_tree()`、`vfs_get_super()`、`sget_fc()`、`fc_mount()`、`vfs_create_mount()`、`do_lock_mount()`、`do_add_mount()`、`attach_recursive_mnt()`、`lookup_mnt()`、`do_umount()`、`deactivate_locked_super()`
> **前置知识**：9.1 VFS 概述

## 概述

一个文件系统要被访问，必须先**挂载（Mount）**：把它的根目录"嫁接"到已有目录树的某个目录（**挂载点**，Mountpoint）上。Linux 把这件事拆成了相互独立的四层：

| 层次 | 对象 | 何时创建 |
|---|---|---|
| 文件系统类型 | `struct file_system_type` | 驱动初始化时注册，如 ext4 模块加载 |
| 文件系统实例 | `struct super_block` | 第一次挂载某个设备（或无设备文件系统的一个实例）时 |
| 挂载 | `struct mount` | 每次挂载（包括绑定挂载）时 |
| 挂载命名空间 | `struct mnt_namespace` | 进程 `unshare(CLONE_NEWNS)` 或 `clone(CLONE_NEWNS)` 时 |

这样的分层带来了很大的灵活性：同一块磁盘可以挂载在多个位置而共享一个超级块；同一个目录子树可以经绑定挂载（Bind Mount）出现在不同位置；不同容器可以看到完全不同的挂载树。

v5.2 起，内核引入了基于**文件系统上下文（Filesystem Context，`struct fs_context`）** 的新挂载 API（David Howells），把"配置参数 → 创建超级块 → 创建挂载 → 接入挂载树"拆成可以分步执行的阶段，并提供了 `fsopen()`、`fsconfig()`、`fsmount()`、`move_mount()` 等新系统调用。传统的 `mount(2)` 在内核中也已改为基于 `fs_context` 实现。本节以 ramfs 为例分析挂载的完整过程，并说明卸载与根文件系统的建立。

## 核心数据结构

### file_system_type

[`include/linux/fs.h:2684-2705`](../../linux/include/linux/fs.h#L2684-L2705)（v6.18.60）

```c
struct file_system_type {
	const char *name;
	int fs_flags;
#define FS_REQUIRES_DEV		1 
#define FS_BINARY_MOUNTDATA	2
#define FS_HAS_SUBTYPE		4
#define FS_USERNS_MOUNT		8	/* Can be mounted by userns root */
#define FS_DISALLOW_NOTIFY_PERM	16	/* Disable fanotify permission events */
#define FS_ALLOW_IDMAP         32      /* FS has been updated to handle vfs idmappings. */
#define FS_MGTIME		64	/* FS uses multigrain timestamps */
#define FS_LBS			128	/* FS supports LBS */
#define FS_POWER_FREEZE		256	/* Always freeze on suspend/hibernate */
#define FS_USERNS_DELEGATABLE	1024	/* Can be mounted inside userns from outside */
#define FS_RENAME_DOES_D_MOVE	32768	/* FS will handle d_move() during rename() internally. */
	int (*init_fs_context)(struct fs_context *);
	const struct fs_parameter_spec *parameters;
	struct dentry *(*mount) (struct file_system_type *, int,
		       const char *, void *);
	void (*kill_sb) (struct super_block *);
	struct module *owner;
	struct file_system_type * next;
	struct hlist_head fs_supers;
```

- `name`：类型名，即 `mount -t` 后面的字符串。
- `fs_flags`：`FS_REQUIRES_DEV` 表示需要块设备（`/proc/filesystems` 中没有 `nodev` 标记的类型）；`FS_USERNS_MOUNT` 允许在非初始用户命名空间中挂载。
- `init_fs_context`：新 API 的入口，初始化 `fs_context` 并设置它的操作表；没有实现它的旧式文件系统使用 `->mount` 方法，由 VFS 的 legacy 适配层包装（[`fs/fs_context.c:703-710`](../../linux/fs/fs_context.c#L703-L710)）。
- `kill_sb`：最后一个活跃引用消失时关闭超级块。
- `next`：所有已注册类型组成一个单链表。
- `fs_supers`：本类型的所有超级块实例。
- 结尾的若干 `lock_class_key` 让 lockdep 能区分不同文件系统的同名锁。

### fs_context

[`include/linux/fs_context.h:90-122`](../../linux/include/linux/fs_context.h#L90-L122)（v6.18.60）

```c
struct fs_context {
	const struct fs_context_operations *ops;
	struct mutex		uapi_mutex;	/* Userspace access mutex */
	struct file_system_type	*fs_type;
	void			*fs_private;	/* The filesystem's context */
	void			*sget_key;
	struct dentry		*root;		/* The root and superblock */
	struct user_namespace	*user_ns;	/* The user namespace for this mount */
	struct net		*net_ns;	/* The network namespace for this mount */
	const struct cred	*cred;		/* The mounter's credentials */
	struct p_log		log;		/* Logging buffer */
	const char		*source;	/* The source name (eg. dev path) */
	void			*security;	/* LSM options */
	void			*s_fs_info;	/* Proposed s_fs_info */
	unsigned int		sb_flags;	/* Proposed superblock flags (SB_*) */
	unsigned int		sb_flags_mask;	/* Superblock flags that were changed */
	unsigned int		s_iflags;	/* OR'd with sb->s_iflags */
	enum fs_context_purpose	purpose:8;
	enum fs_context_phase	phase:8;	/* The phase the context is in */
	bool			need_free:1;	/* Need to call ops->free() */
	bool			global:1;	/* Goes into &init_user_ns */
	bool			oldapi:1;	/* Coming from mount(2) */
	bool			exclusive:1;    /* create new superblock, reject existing one */
};

struct fs_context_operations {
	void (*free)(struct fs_context *fc);
	int (*dup)(struct fs_context *fc, struct fs_context *src_fc);
	int (*parse_param)(struct fs_context *fc, struct fs_parameter *param);
	int (*parse_monolithic)(struct fs_context *fc, void *data);
	int (*get_tree)(struct fs_context *fc);
	int (*reconfigure)(struct fs_context *fc);
};
```

`fs_context` 是一次挂载（或重新挂载）请求的"配置单"：

- `fs_type`、`ops`：目标文件系统类型及其上下文操作表。`ops->parse_param` 逐个解析参数，`ops->get_tree` 根据参数获取或创建超级块，并把根 dentry 放入 `root`。
- `source`：挂载源，如 `/dev/sda1`；无设备文件系统通常忽略它。
- `fs_private`、`s_fs_info`：文件系统自己的解析结果，后者在创建超级块时转交给 `sb->s_fs_info`。
- `sb_flags`：请求的超级块标志（`SB_RDONLY` 等）。
- `purpose`、`phase`：用途（新挂载、子挂载、重新配置）与新 API 下的状态机阶段。
- `log`：错误信息缓冲，新 API 下用户态可以 `read()` 到详细的错误原因。

### mount 与 mountpoint

[`fs/mount.h:38-101`](../../linux/fs/mount.h#L38-L101)（v6.18.60）

```c
struct mountpoint {
	struct hlist_node m_hash;
	struct dentry *m_dentry;
	struct hlist_head m_list;
};

struct mount {
	struct hlist_node mnt_hash;
	struct mount *mnt_parent;
	struct dentry *mnt_mountpoint;
	struct vfsmount mnt;
	union {
		struct rb_node mnt_node; /* node in the ns->mounts rbtree */
		struct rcu_head mnt_rcu;
		struct llist_node mnt_llist;
	};
#ifdef CONFIG_SMP
	struct mnt_pcp __percpu *mnt_pcp;
#else
	int mnt_count;
	int mnt_writers;
#endif
	struct list_head mnt_mounts;	/* list of children, anchored here */
	struct list_head mnt_child;	/* and going through their mnt_child */
	struct mount *mnt_next_for_sb;	/* the next two fields are hlist_node, */
	struct mount * __aligned(1) *mnt_pprev_for_sb;
					/* except that LSB of pprev is stolen */
#define WRITE_HOLD 1			/* ... for use by mnt_hold_writers() */
	const char *mnt_devname;	/* Name of device e.g. /dev/dsk/hda1 */
	struct list_head mnt_list;
	struct list_head mnt_expire;	/* link in fs-specific expiry list */
	struct list_head mnt_share;	/* circular list of shared mounts */
	struct hlist_head mnt_slave_list;/* list of slave mounts */
	struct hlist_node mnt_slave;	/* slave list entry */
	struct mount *mnt_master;	/* slave is on master->mnt_slave_list */
	...
	struct mnt_namespace *mnt_ns;
	struct mountpoint *mnt_mp;	/* where is it mounted */
	...
	struct mount *overmount;	/* mounted on ->mnt_root */
} __randomize_layout;
```

- `mnt`：内嵌的 `struct vfsmount`（`mnt_root`、`mnt_sb`、`mnt_flags`），对外传递的都是它的指针，VFS 内部用 `real_mount()` 换回 `struct mount`。
- `mnt_parent`、`mnt_mountpoint`：父挂载与挂载点 dentry。二者合在一起就是"挂在哪里"——挂载点是**父挂载中**的一个 dentry。挂载树的根以自身为父。
- `mnt_hash`：挂入全局挂载哈希表 `mount_hashtable`，键是（父挂载，挂载点 dentry），路径查找经由它从挂载点"穿越"到被挂载的文件系统（9.3 节）。
- `mnt_mounts`、`mnt_child`：子挂载链表，组成挂载树。
- `mnt_pcp`：per-CPU 的引用计数与写者计数（依赖 `CONFIG_SMP`）。
- `mnt_share`、`mnt_slave_list`、`mnt_master`：挂载传播关系（见后文）。
- `mnt_ns`：所属挂载命名空间。

`struct mountpoint` 代表"某个 dentry 被用作挂载点"这件事，通过 `mountpoint_hashtable` 按 dentry 查找，`m_list` 串起挂在它上面的所有 `struct mount`。dentry 一旦成为挂载点，就会被打上 `DCACHE_MOUNTED` 标志，路径查找只需检查这一位就能知道是否要查挂载哈希表。

```
  挂载树（struct mount）                          dentry 树
  ┌────────────────────────┐                ┌─────────────────────────┐
  │ M1: ext4 /dev/sda2     │  mnt_root ───▶ │ "/" ─┬─ "home"           │  (sda2 的 dentry)
  │ mnt_parent = M1        │                │      ├─ "proc"  [MOUNTED] │
  └─────┬──────────────┬───┘                │      └─ "mnt"   [MOUNTED] │
   mnt_mounts       mnt_mounts              └─────────────────────────┘
        ▼              ▼
  ┌──────────────┐ ┌──────────────┐          ┌──────────────┐
  │ M2: proc     │ │ M3: ramfs    │ mnt_root │ ramfs 的 "/" │
  │ parent = M1  │ │ parent = M1  │ ───────▶ │  └─ "a.txt"  │
  │ mountpoint = │ │ mountpoint = │          └──────────────┘
  │  sda2:"proc" │ │  sda2:"mnt"  │
  └──────────────┘ └──────────────┘

  mount_hashtable:  hash(&M1->mnt, dentry"proc") ─▶ M2
                    hash(&M1->mnt, dentry"mnt")  ─▶ M3
```

*图 9-3：挂载树与挂载哈希表（`mount -t ramfs none /mnt` 之后）*

## 主流程分析

### 文件系统类型的注册

所有已注册的 `file_system_type` 组成一个由读写锁保护的单链表：

[`fs/filesystems.c:34-35`](../../linux/fs/filesystems.c#L34-L35)（v6.18.60）

```c
static struct file_system_type *file_systems;
static DEFINE_RWLOCK(file_systems_lock);
```

[`fs/filesystems.c:72-92`](../../linux/fs/filesystems.c#L72-L92)（v6.18.60）

```c
int register_filesystem(struct file_system_type * fs)
{
	int res = 0;
	struct file_system_type ** p;

	if (fs->parameters &&
	    !fs_validate_description(fs->name, fs->parameters))
		return -EINVAL;

	BUG_ON(strchr(fs->name, '.'));
	if (fs->next)
		return -EBUSY;
	write_lock(&file_systems_lock);
	p = find_filesystem(fs->name, strlen(fs->name));
	if (*p)
		res = -EBUSY;
	else
		*p = fs;
	write_unlock(&file_systems_lock);
	return res;
}
```

注册只是把结构体挂到链表末尾（同名则返回 `-EBUSY`），并不分配任何超级块。ramfs 在 `fs_initcall` 阶段注册：

[`fs/ramfs/inode.c:319-331`](../../linux/fs/ramfs/inode.c#L319-L331)（v6.18.60）

```c
static struct file_system_type ramfs_fs_type = {
	.name		= "ramfs",
	.init_fs_context = ramfs_init_fs_context,
	.parameters	= ramfs_fs_parameters,
	.kill_sb	= ramfs_kill_sb,
	.fs_flags	= FS_USERNS_MOUNT,
};

static int __init init_ramfs_fs(void)
{
	return register_filesystem(&ramfs_fs_type);
}
fs_initcall(init_ramfs_fs);
```

挂载时按名字查找类型。若找不到，`get_fs_type()` 会调用 `request_module("fs-%s")` 请求用户态的 `modprobe` 加载对应模块——这正是各文件系统模块用 `MODULE_ALIAS_FS("ext4")` 声明 `fs-ext4` 别名的原因：

[`fs/filesystems.c:265-296`](../../linux/fs/filesystems.c#L265-L296)（v6.18.60）

```c
static struct file_system_type *__get_fs_type(const char *name, int len)
{
	struct file_system_type *fs;

	read_lock(&file_systems_lock);
	fs = *(find_filesystem(name, len));
	if (fs && !try_module_get(fs->owner))
		fs = NULL;
	read_unlock(&file_systems_lock);
	return fs;
}

struct file_system_type *get_fs_type(const char *name)
{
	struct file_system_type *fs;
	const char *dot = strchr(name, '.');
	int len = dot ? dot - name : strlen(name);

	fs = __get_fs_type(name, len);
	if (!fs && (request_module("fs-%.*s", len, name) == 0)) {
		fs = __get_fs_type(name, len);
		if (!fs)
			pr_warn_once("request_module fs-%.*s succeeded, but still no fs?\n",
				     len, name);
	}

	if (dot && fs && !(fs->fs_flags & FS_HAS_SUBTYPE)) {
		put_filesystem(fs);
		fs = NULL;
	}
	return fs;
}
```

`try_module_get()` 为模块加一个引用，防止挂载过程中模块被卸载。类型名中的 `.` 用于"子类型"，例如 FUSE 的 `fuse.sshfs`。

### mount(2) 的整体调用链

```
mount(dev, dir, type, flags, data)                     fs/namespace.c:4309
└── do_mount()                                          fs/namespace.c:4133
    ├── user_path_at(dir)                               // 查找挂载点（9.3 节）
    └── path_mount()                                    fs/namespace.c:4054
        └── do_new_mount()                              fs/namespace.c:3741
            ├── get_fs_type()                           fs/filesystems.c:277
            ├── fs_context_for_mount()                  fs/fs_context.c:326
            │   └── alloc_fs_context()
            │       └── type->init_fs_context()         // ramfs_init_fs_context()
            ├── vfs_parse_fs_string(fc, "source", dev)
            ├── parse_monolithic_mount_data(fc, data)   // 拆分 "a=1,b" 逐个 parse_param
            └── do_new_mount_fc()                       fs/namespace.c:3708
                ├── fc_mount()                          fs/namespace.c:1213
                │   ├── vfs_get_tree()                  fs/super.c:1757
                │   │   └── fc->ops->get_tree()         // ramfs_get_tree()
                │   │       └── get_tree_nodev() → vfs_get_super()
                │   │           ├── sget_fc()           // 查找或新建 super_block
                │   │           │   └── alloc_super()
                │   │           └── fill_super()        // ramfs_fill_super()
                │   └── vfs_create_mount()              // 分配 struct mount
                ├── LOCK_MOUNT() → do_lock_mount()      // 锁住挂载点
                └── do_add_mount()                      fs/namespace.c:3671
                    └── graft_tree() → attach_recursive_mnt()
                        ├── mnt_set_mountpoint()
                        └── commit_tree() → make_visible()   // 加入挂载哈希表
```

### path_mount()：分派不同的挂载操作

`mount(2)` 用一个系统调用承载了新挂载、重新挂载、绑定挂载、移动挂载、修改传播类型等多种操作，`path_mount()` 负责区分它们：

[`fs/namespace.c:4083-4131`](../../linux/fs/namespace.c#L4083-L4131)（v6.18.60）

```c
	/* Separate the per-mountpoint flags */
	if (flags & MS_NOSUID)
		mnt_flags |= MNT_NOSUID;
	if (flags & MS_NODEV)
		mnt_flags |= MNT_NODEV;
	if (flags & MS_NOEXEC)
		mnt_flags |= MNT_NOEXEC;
	if (flags & MS_NOATIME)
		mnt_flags |= MNT_NOATIME;
	if (flags & MS_NODIRATIME)
		mnt_flags |= MNT_NODIRATIME;
	if (flags & MS_STRICTATIME)
		mnt_flags &= ~(MNT_RELATIME | MNT_NOATIME);
	if (flags & MS_RDONLY)
		mnt_flags |= MNT_READONLY;
	if (flags & MS_NOSYMFOLLOW)
		mnt_flags |= MNT_NOSYMFOLLOW;
	...
	sb_flags = flags & (SB_RDONLY |
			    SB_SYNCHRONOUS |
			    SB_MANDLOCK |
			    SB_DIRSYNC |
			    SB_SILENT |
			    SB_POSIXACL |
			    SB_LAZYTIME |
			    SB_I_VERSION);

	if ((flags & (MS_REMOUNT | MS_BIND)) == (MS_REMOUNT | MS_BIND))
		return do_reconfigure_mnt(path, mnt_flags);
	if (flags & MS_REMOUNT)
		return do_remount(path, sb_flags, mnt_flags, data_page);
	if (flags & MS_BIND)
		return do_loopback(path, dev_name, flags & MS_REC);
	if (flags & (MS_SHARED | MS_PRIVATE | MS_SLAVE | MS_UNBINDABLE))
		return do_change_type(path, flags);
	if (flags & MS_MOVE)
		return do_move_mount_old(path, dev_name);

	return do_new_mount(path, type_page, sb_flags, mnt_flags, dev_name,
			    data_page);
}
```

这段代码完成两件事。一是把 `MS_*` 标志拆成两类：`MNT_NOSUID`、`MNT_NODEV`、`MNT_NOEXEC`、`MNT_NOATIME` 等属于**挂载**（同一个超级块的不同挂载可以各不相同），而 `SB_RDONLY`、`SB_SYNCHRONOUS` 等属于**超级块**（影响整个文件系统实例）。只读比较特殊，两层都有：`MNT_READONLY` 让某个挂载只读，`SB_RDONLY` 让整个实例只读。二是按标志分派：没有特殊标志时才是创建新挂载，进入 `do_new_mount()`。另外，未指定 atime 相关选项时默认使用 `MNT_RELATIME`（[`fs/namespace.c:4079-4081`](../../linux/fs/namespace.c#L4079-L4081)）。

### do_new_mount()：建立 fs_context

[`fs/namespace.c:3741-3792`](../../linux/fs/namespace.c#L3741-L3792)（v6.18.60）

```c
static int do_new_mount(const struct path *path, const char *fstype,
			int sb_flags, int mnt_flags,
			const char *name, void *data)
{
	...
	if (!fstype)
		return -EINVAL;

	type = get_fs_type(fstype);
	if (!type)
		return -ENODEV;
	...
	fc = fs_context_for_mount(type, sb_flags);
	put_filesystem(type);
	if (IS_ERR(fc))
		return PTR_ERR(fc);

	/*
	 * Indicate to the filesystem that the mount request is coming
	 * from the legacy mount system call.
	 */
	fc->oldapi = true;

	if (subtype)
		err = vfs_parse_fs_string(fc, "subtype", subtype);
	if (!err && name)
		err = vfs_parse_fs_string(fc, "source", name);
	if (!err)
		err = parse_monolithic_mount_data(fc, data);
	if (!err && !mount_capable(fc))
		err = -EPERM;
	if (!err)
		err = do_new_mount_fc(fc, path, mnt_flags);

	put_fs_context(fc);
	return err;
}
```

`fs_context_for_mount()` 直接转调 `alloc_fs_context()`：

[`fs/fs_context.c:272-319`](../../linux/fs/fs_context.c#L272-L319)（v6.18.60）

```c
static struct fs_context *alloc_fs_context(struct file_system_type *fs_type,
				      struct dentry *reference,
				      unsigned int sb_flags,
				      unsigned int sb_flags_mask,
				      enum fs_context_purpose purpose)
{
	...
	fc = kzalloc(sizeof(struct fs_context), GFP_KERNEL_ACCOUNT);
	if (!fc)
		return ERR_PTR(-ENOMEM);

	fc->purpose	= purpose;
	fc->sb_flags	= sb_flags;
	fc->sb_flags_mask = sb_flags_mask;
	fc->fs_type	= get_filesystem(fs_type);
	fc->cred	= get_current_cred();
	fc->net_ns	= get_net(current->nsproxy->net_ns);
	fc->log.prefix	= fs_type->name;
	...
	switch (purpose) {
	case FS_CONTEXT_FOR_MOUNT:
		fc->user_ns = get_user_ns(fc->cred->user_ns);
		break;
	case FS_CONTEXT_FOR_SUBMOUNT:
		fc->user_ns = get_user_ns(reference->d_sb->s_user_ns);
		break;
	case FS_CONTEXT_FOR_RECONFIGURE:
		atomic_inc(&reference->d_sb->s_active);
		fc->user_ns = get_user_ns(reference->d_sb->s_user_ns);
		fc->root = dget(reference);
		break;
	}

	/* TODO: Make all filesystems support this unconditionally */
	init_fs_context = fc->fs_type->init_fs_context;
	if (!init_fs_context)
		init_fs_context = legacy_init_fs_context;

	ret = init_fs_context(fc);
	if (ret < 0)
		goto err_fc;
	fc->need_free = true;
	return fc;
```

它记录挂载者的凭据与网络命名空间，然后调用文件系统的 `init_fs_context`。ramfs 的实现分配私有的 `ramfs_fs_info`（目前只有一个 `mode` 参数）并挂上自己的上下文操作表：

[`fs/ramfs/inode.c:283-311`](../../linux/fs/ramfs/inode.c#L283-L311)（v6.18.60）

```c
static int ramfs_get_tree(struct fs_context *fc)
{
	return get_tree_nodev(fc, ramfs_fill_super);
}

static void ramfs_free_fc(struct fs_context *fc)
{
	kfree(fc->s_fs_info);
}

static const struct fs_context_operations ramfs_context_ops = {
	.free		= ramfs_free_fc,
	.parse_param	= ramfs_parse_param,
	.get_tree	= ramfs_get_tree,
};

int ramfs_init_fs_context(struct fs_context *fc)
{
	struct ramfs_fs_info *fsi;

	fsi = kzalloc(sizeof(*fsi), GFP_KERNEL);
	if (!fsi)
		return -ENOMEM;

	fsi->mount_opts.mode = RAMFS_DEFAULT_MODE;
	fc->s_fs_info = fsi;
	fc->ops = &ramfs_context_ops;
	return 0;
}
```

`mount(2)` 传入的选项字符串（如 `"mode=0700"`）由 `parse_monolithic_mount_data()` 按逗号拆开，逐个交给 `vfs_parse_fs_param()`：

[`fs/fs_context.c:127-160`](../../linux/fs/fs_context.c#L127-L160)（v6.18.60）

```c
int vfs_parse_fs_param(struct fs_context *fc, struct fs_parameter *param)
{
	int ret;

	if (!param->key)
		return invalf(fc, "Unnamed parameter\n");

	ret = vfs_parse_sb_flag(fc, param->key);
	if (ret != -ENOPARAM)
		return ret;

	ret = security_fs_context_parse_param(fc, param);
	if (ret != -ENOPARAM)
		/* Param belongs to the LSM or is disallowed by the LSM; so
		 * don't pass to the FS.
		 */
		return ret;

	if (fc->ops->parse_param) {
		ret = fc->ops->parse_param(fc, param);
		if (ret != -ENOPARAM)
			return ret;
	}

	/* If the filesystem doesn't take any arguments, give it the
	 * default handling of source.
	 */
	ret = vfs_parse_fs_param_source(fc, param);
	if (ret != -ENOPARAM)
		return ret;

	return invalf(fc, "%s: Unknown parameter '%s'",
		      fc->fs_type->name, param->key);
}
```

参数依次经过三道关口：通用的超级块标志（`ro`、`sync` 等）、LSM（如 SELinux 的 `context=`）、文件系统自己的 `parse_param`。ramfs 用 `fs_parse()` 按 `ramfs_fs_parameters` 描述表解析 `mode`（[`fs/ramfs/inode.c:226-260`](../../linux/fs/ramfs/inode.c#L226-L260)）。

### vfs_get_tree()：获取超级块与根

[`fs/super.c:1757-1792`](../../linux/fs/super.c#L1757-L1792)（v6.18.60）

```c
int vfs_get_tree(struct fs_context *fc)
{
	struct super_block *sb;
	int error;

	if (fc->root)
		return -EBUSY;

	/* Get the mountable root in fc->root, with a ref on the root and a ref
	 * on the superblock.
	 */
	error = fc->ops->get_tree(fc);
	if (error < 0)
		return error;

	if (!fc->root) {
		pr_err("Filesystem %s get_tree() didn't set fc->root, returned %i\n",
		       fc->fs_type->name, error);
		/* We don't know what the locking state of the superblock is -
		 * if there is a superblock.
		 */
		BUG();
	}

	sb = fc->root->d_sb;
	WARN_ON(!sb->s_bdi);

	/*
	 * super_wake() contains a memory barrier which also care of
	 * ordering for super_cache_count(). We place it before setting
	 * SB_BORN as the data dependency between the two functions is
	 * the superblock structure contents that we just set up, not
	 * the SB_BORN flag.
	 */
	super_wake(sb, SB_BORN);

```

`vfs_get_tree()` 调用文件系统的 `get_tree`，要求它返回时：`fc->root` 指向待挂载的根 dentry（持有引用），对应的超级块已持有一个活跃引用并且 `s_umount` 处于写锁定状态。随后用 `super_wake(sb, SB_BORN)` 宣告超级块已初始化完毕——并发挂载同一设备的其他任务正等待这个标志。

对于无设备文件系统，VFS 提供了几个现成的 `get_tree` 辅助函数，它们的差别仅在于"什么情况下复用已有超级块"：

| 辅助函数 | 复用条件 | 典型用户 |
|---|---|---|
| `get_tree_nodev()` | 从不复用，每次挂载都是新实例 | ramfs、tmpfs |
| `get_tree_single()` | 全系统只有一个实例 | configfs、debugfs |
| `get_tree_keyed()` | `s_fs_info` 相同则复用 | mqueue（每个 IPC 命名空间一个） |
| `get_tree_bdev()` | 块设备号相同则复用 | ext4、XFS、Btrfs |

前三者都基于 `vfs_get_super()`：

[`fs/super.c:1328-1362`](../../linux/fs/super.c#L1328-L1362)（v6.18.60）

```c
static int vfs_get_super(struct fs_context *fc,
		int (*test)(struct super_block *, struct fs_context *),
		int (*fill_super)(struct super_block *sb,
				  struct fs_context *fc))
{
	struct super_block *sb;
	int err;

	sb = sget_fc(fc, test, set_anon_super_fc);
	if (IS_ERR(sb))
		return PTR_ERR(sb);

	if (!sb->s_root) {
		err = fill_super(sb, fc);
		if (err)
			goto error;

		sb->s_flags |= SB_ACTIVE;
	}

	fc->root = dget(sb->s_root);
	return 0;

error:
	deactivate_locked_super(sb);
	return err;
}

int get_tree_nodev(struct fs_context *fc,
		  int (*fill_super)(struct super_block *sb,
				    struct fs_context *fc))
{
	return vfs_get_super(fc, NULL, fill_super);
}
EXPORT_SYMBOL(get_tree_nodev);
```

`sget_fc()` 是查找或创建超级块的核心：

[`fs/super.c:748-789`](../../linux/fs/super.c#L748-L789)（v6.18.60）

```c
retry:
	spin_lock(&sb_lock);
	if (test) {
		hlist_for_each_entry(old, &fc->fs_type->fs_supers, s_instances) {
			/* Only unlinked at the last passive reference. */
			if (super_flags(old, SB_DEAD))
				continue;
			if (test(old, fc))
				goto share_extant_sb;
		}
	}
	if (!s) {
		spin_unlock(&sb_lock);
		s = alloc_super(fc->fs_type, fc->sb_flags, user_ns);
		if (!s)
			return ERR_PTR(-ENOMEM);
		goto retry;
	}

	s->s_fs_info = fc->s_fs_info;
	err = set(s, fc);
	...
	fc->s_fs_info = NULL;
	s->s_type = fc->fs_type;
	s->s_iflags |= fc->s_iflags;
	strscpy(s->s_id, s->s_type->name, sizeof(s->s_id));
	/*
	 * Make the superblock visible on @super_blocks and @fs_supers.
	 * It's in a nascent state and users should wait on SB_BORN or
	 * SB_DYING to be set.
	 */
	list_add_tail(&s->s_list, &super_blocks);
	hlist_add_head(&s->s_instances, &s->s_type->fs_supers);
	spin_unlock(&sb_lock);
	get_filesystem(s->s_type);
	shrinker_register(s->s_shrink);
	return s;
```

这里使用了内核中常见的"先解锁分配、再加锁重查"模式：持 `sb_lock` 时遍历 `fs_supers` 调用 `test` 比较；若没找到，就释放锁去分配新超级块（分配可能睡眠），再回到 `retry` 重新查找——期间别人可能已经创建了匹配的超级块。最终要么复用旧的（`share_extant_sb` 分支，[`fs/super.c:791-804`](../../linux/fs/super.c#L791-L804)：`grab_super()` 增加活跃引用并获取 `s_umount`；若旧超级块属于另一个用户命名空间，或调用者要求独占，则返回 `-EBUSY`），要么调用 `set`（无设备文件系统用 `set_anon_super_fc()` 分配一个匿名设备号）后把新超级块挂入全局链表 `super_blocks` 与类型的 `fs_supers`，并注册它的 shrinker。

`alloc_super()`（[`fs/super.c:316-397`](../../linux/fs/super.c#L316-L397)）初始化超级块的各个字段：`s_umount` 以写锁状态返回、`s_active` 与 `s_count` 置 1、`s_bdi` 指向 `noop_backing_dev_info`（无回写能力的默认 BDI），并创建 shrinker 与两个 `list_lru`（dentry LRU 与 inode LRU）。

若 `sb->s_root` 为空，说明这是新超级块，`vfs_get_super()` 调用文件系统的 `fill_super` 填充它。ramfs 的实现：

[`fs/ramfs/inode.c:262-281`](../../linux/fs/ramfs/inode.c#L262-L281)（v6.18.60）

```c
static int ramfs_fill_super(struct super_block *sb, struct fs_context *fc)
{
	struct ramfs_fs_info *fsi = sb->s_fs_info;
	struct inode *inode;

	sb->s_maxbytes		= MAX_LFS_FILESIZE;
	sb->s_blocksize		= PAGE_SIZE;
	sb->s_blocksize_bits	= PAGE_SHIFT;
	sb->s_magic		= RAMFS_MAGIC;
	sb->s_op		= &ramfs_ops;
	sb->s_d_flags		= DCACHE_DONTCACHE;
	sb->s_time_gran		= 1;

	inode = ramfs_get_inode(sb, NULL, S_IFDIR | fsi->mount_opts.mode, 0);
	sb->s_root = d_make_root(inode);
	if (!sb->s_root)
		return -ENOMEM;

	return 0;
}
```

`fill_super` 的职责是设置超级块参数与操作表，然后创建根 inode，再用 `d_make_root()` 为它分配根 dentry。对于磁盘文件系统，这一步还要从设备读取并校验磁盘上的超级块。

块设备文件系统使用 `get_tree_bdev()`。它先把 `source` 解析为设备号，再以设备号为键查找超级块，这保证了**同一块设备无论挂载多少次都只有一个超级块**：

[`fs/super.c:1676-1719`](../../linux/fs/super.c#L1676-L1719)（v6.18.60）

```c
int get_tree_bdev_flags(struct fs_context *fc,
		int (*fill_super)(struct super_block *sb,
				  struct fs_context *fc), unsigned int flags)
{
	struct super_block *s;
	int error = 0;
	dev_t dev;
	...
	if (!fc->source)
		return invalf(fc, "No source specified");

	error = lookup_bdev(fc->source, &dev);
	if (error) {
	...
	}
	fc->sb_flags |= SB_NOSEC;
	s = sget_dev(fc, dev);
	if (IS_ERR(s))
		return PTR_ERR(s);

	if (s->s_root) {
		/* Don't summarily change the RO/RW state. */
		if ((fc->sb_flags ^ s->s_flags) & SB_RDONLY) {
			warnf(fc, "%pg: Can't mount, would change RO state", s->s_bdev);
			deactivate_locked_super(s);
			return -EBUSY;
		}
	} else {
		error = setup_bdev_super(s, fc->sb_flags, fc);
		if (!error)
			error = fill_super(s, fc);
		if (error) {
			deactivate_locked_super(s);
			return error;
		}
		s->s_flags |= SB_ACTIVE;
	}

	BUG_ON(fc->root);
	fc->root = dget(s->s_root);
	return 0;
}
```

### 创建 struct mount

`fc_mount()` 在 `vfs_get_tree()` 成功后释放 `s_umount`，然后调用 `vfs_create_mount()`（[`fs/namespace.c:1193-1210`](../../linux/fs/namespace.c#L1193-L1210)）为超级块创建一个挂载实例：`alloc_vfsmnt()` 从 `mnt_cache` 分配 `struct mount`、分配挂载 ID 与 per-CPU 计数，再由 `setup_mnt()` 填充：

[`fs/namespace.c:1170-1222`](../../linux/fs/namespace.c#L1170-L1222)（v6.18.60）

```c
static void setup_mnt(struct mount *m, struct dentry *root)
{
	struct super_block *s = root->d_sb;

	atomic_inc(&s->s_active);
	m->mnt.mnt_sb = s;
	m->mnt.mnt_root = dget(root);
	m->mnt_mountpoint = m->mnt.mnt_root;
	m->mnt_parent = m;

	guard(mount_locked_reader)();
	mnt_add_instance(m, s);
}
...
struct vfsmount *fc_mount(struct fs_context *fc)
{
	int err = vfs_get_tree(fc);
	if (!err) {
		up_write(&fc->root->d_sb->s_umount);
		return vfs_create_mount(fc);
	}
	return ERR_PTR(err);
}
EXPORT_SYMBOL(fc_mount);
```

`setup_mnt()` 让新挂载持有超级块的一个活跃引用（`s_active`）和根 dentry 的一个引用；此时 `mnt_parent` 指向自身，它还没有接入任何挂载树。

### 锁定挂载点：do_lock_mount()

把新挂载接入挂载树之前，必须同时锁住挂载点所在目录的 inode 与全局的 `namespace_sem`，并确认挂载点在加锁过程中没有被并发改变：

[`fs/namespace.c:2724-2743`](../../linux/fs/namespace.c#L2724-L2743)（v6.18.60）

```c
static inline struct mount *where_to_mount(const struct path *path,
					   struct dentry **dentry,
					   bool beneath)
{
	struct mount *m;

	if (unlikely(beneath)) {
		m = topmost_overmount(real_mount(path->mnt));
		*dentry = m->mnt_mountpoint;
		return m->mnt_parent;
	}
	m = __lookup_mnt(path->mnt, path->dentry);
	if (unlikely(m)) {
		m = topmost_overmount(m);
		*dentry = m->mnt.mnt_root;
		return m;
	}
	*dentry = path->dentry;
	return real_mount(path->mnt);
}
```

`where_to_mount()` 处理"挂载点上已经挂了东西"的情况：如果目标位置已被覆盖（Overmount），新挂载应该挂在**最上层**那个挂载的根上，而不是被覆盖的原始目录上。`do_lock_mount()`（[`fs/namespace.c:2776-2835`](../../linux/fs/namespace.c#L2776-L2835)）先在 `mount_lock` 读锁下确定位置，然后依次获取 inode 锁和 `namespace_sem`，再重新计算一次位置；如果两次结果不同（期间有人在这里挂载或卸载），就释放锁重试。最终调用 `get_mountpoint()` 为挂载点 dentry 取得（或新建）`struct mountpoint`，并为 dentry 设置 `DCACHE_MOUNTED`。

调用者通过 `LOCK_MOUNT()` 宏使用它，借助 `__cleanup` 属性在作用域结束时自动解锁：

[`fs/namespace.c:2852-2858`](../../linux/fs/namespace.c#L2852-L2858)（v6.18.60）

```c
#define LOCK_MOUNT_MAYBE_BENEATH(mp, path, beneath) \
	struct pinned_mountpoint mp __cleanup(unlock_mount) = {}; \
	do_lock_mount((path), &mp, (beneath))
#define LOCK_MOUNT(mp, path) LOCK_MOUNT_MAYBE_BENEATH(mp, (path), false)
#define LOCK_MOUNT_EXACT(mp, path) \
	struct pinned_mountpoint mp __cleanup(unlock_mount) = {}; \
	lock_mount_exact((path), &mp)
```

### 接入挂载树

[`fs/namespace.c:3671-3700`](../../linux/fs/namespace.c#L3671-L3700)（v6.18.60）

```c
static int do_add_mount(struct mount *newmnt, const struct pinned_mountpoint *mp,
			int mnt_flags)
{
	struct mount *parent = mp->parent;

	if (IS_ERR(parent))
		return PTR_ERR(parent);

	mnt_flags &= ~MNT_INTERNAL_FLAGS;

	if (unlikely(!check_mnt(parent))) {
		/* that's acceptable only for automounts done in private ns */
		if (!(mnt_flags & MNT_SHRINKABLE))
			return -EINVAL;
		/* ... and for those we'd better have mountpoint still alive */
		if (!is_mounted(&parent->mnt))
			return -EINVAL;
	}

	/* Refuse the same filesystem on the same mount point */
	if (parent->mnt.mnt_sb == newmnt->mnt.mnt_sb &&
	    parent->mnt.mnt_root == mp->mp->m_dentry)
		return -EBUSY;

	if (d_is_symlink(newmnt->mnt.mnt_root))
		return -EINVAL;

	newmnt->mnt.mnt_flags = mnt_flags;
	return graft_tree(newmnt, mp);
}
```

`do_add_mount()` 拒绝把同一文件系统重复挂载在它自己的根上（否则会出现无意义的自我覆盖），也拒绝挂载以符号链接为根的树。`graft_tree()`（[`fs/namespace.c:2860-2870`](../../linux/fs/namespace.c#L2860-L2870)）再检查类型匹配：目录只能挂到目录上，文件只能挂到文件上，然后进入 `attach_recursive_mnt()`。

`attach_recursive_mnt()`（[`fs/namespace.c:2590-2722`](../../linux/fs/namespace.c#L2590-L2722)）要处理挂载传播、移动挂载、覆盖挂载等复杂情况。对于最常见的"在私有挂载点上新建挂载"，它的核心工作可以概括为：

[`fs/namespace.c:2673-2702`](../../linux/fs/namespace.c#L2673-L2702)（v6.18.60）

```c
	mnt_set_mountpoint(dest_mnt, dest_mp, source_mnt);
	/*
	 * Now the original copy is in the same state as the secondaries -
	 * its root attached to mountpoint, but not hashed and all mounts
	 * in it are either in our namespace or in no namespace at all.
	 * Add the original to the list of copies and deal with the
	 * rest of work for all of them uniformly.
	 */
	hlist_add_head(&source_mnt->mnt_hash, &tree_list);

	hlist_for_each_entry_safe(child, n, &tree_list, mnt_hash) {
		struct mount *q;
		hlist_del_init(&child->mnt_hash);
		/* Notice when we are propagating across user namespaces */
		if (child->mnt_parent->mnt_ns->user_ns != user_ns)
			lock_mnt_tree(child);
		q = __lookup_mnt(&child->mnt_parent->mnt,
				 child->mnt_mountpoint);
		commit_tree(child);
		if (q) {
			struct mount *r = topmost_overmount(child);
			struct mountpoint *mp = root.mp;

			if (unlikely(shorter) && child != source_mnt)
				mp = shorter;
			mnt_change_mountpoint(r, mp, q);
		}
	}
	unpin_mountpoint(&root);
	unlock_mount_hash();
```

`tree_list` 中除了 `source_mnt` 本身，还有 `propagate_mnt()` 为对等挂载生成的副本，它们被统一处理：逐个 `commit_tree()`；若某个副本的目标位置上原本已有挂载（`q`），就把原有挂载移到新挂载之上，相当于把新挂载"插入"到它下面。其中 `mnt_set_mountpoint()` 设置父子关系，`commit_tree()` 把挂载加入命名空间并调用 `make_visible()`：

[`fs/namespace.c:1033-1051`](../../linux/fs/namespace.c#L1033-L1051)（v6.18.60）

```c
void mnt_set_mountpoint(struct mount *mnt,
			struct mountpoint *mp,
			struct mount *child_mnt)
{
	child_mnt->mnt_mountpoint = mp->m_dentry;
	child_mnt->mnt_parent = mnt;
	child_mnt->mnt_mp = mp;
	hlist_add_head(&child_mnt->mnt_mp_list, &mp->m_list);
}

static void make_visible(struct mount *mnt)
{
	struct mount *parent = mnt->mnt_parent;
	if (unlikely(mnt->mnt_mountpoint == parent->mnt.mnt_root))
		parent->overmount = mnt;
	hlist_add_head_rcu(&mnt->mnt_hash,
			   m_hash(&parent->mnt, mnt->mnt_mountpoint));
	list_add_tail(&mnt->mnt_child, &parent->mnt_mounts);
}
```

`make_visible()` 把新挂载插入挂载哈希表并链入父挂载的 `mnt_mounts`。**插入哈希表的那一刻，挂载对路径查找就可见了**：此后查找穿过挂载点 dentry 时，`__lookup_mnt()` 就能找到它。

[`fs/namespace.c:203-209`](../../linux/fs/namespace.c#L203-L209)（v6.18.60）

```c
static inline struct hlist_head *m_hash(struct vfsmount *mnt, struct dentry *dentry)
{
	unsigned long tmp = ((unsigned long)mnt / L1_CACHE_BYTES);
	tmp += ((unsigned long)dentry / L1_CACHE_BYTES);
	tmp = tmp + (tmp >> m_hash_shift);
	return &mount_hashtable[tmp & m_hash_mask];
}
```

[`fs/namespace.c:808-840`](../../linux/fs/namespace.c#L808-L840)（v6.18.60）

```c
struct mount *__lookup_mnt(struct vfsmount *mnt, struct dentry *dentry)
{
	struct hlist_head *head = m_hash(mnt, dentry);
	struct mount *p;

	hlist_for_each_entry_rcu(p, head, mnt_hash)
		if (&p->mnt_parent->mnt == mnt && p->mnt_mountpoint == dentry)
			return p;
	return NULL;
}

/**
 * lookup_mnt - Return the child mount mounted at given location
 * @path:	location in the namespace
 *
 * Acquires and returns a new reference to mount at given location
 * or %NULL if nothing is mounted there.
 */
struct vfsmount *lookup_mnt(const struct path *path)
{
	struct mount *child_mnt;
	struct vfsmount *m;
	unsigned seq;

	rcu_read_lock();
	do {
		seq = read_seqbegin(&mount_lock);
		child_mnt = __lookup_mnt(path->mnt, path->dentry);
		m = child_mnt ? &child_mnt->mnt : NULL;
	} while (!legitimize_mnt(m, seq));
	rcu_read_unlock();
	return m;
}
```

`__lookup_mnt()` 在 RCU 保护下遍历哈希链，按（父挂载，挂载点）精确匹配。`lookup_mnt()` 在 `mount_lock` 顺序锁的保护下调用它，并用 `legitimize_mnt()` 为找到的挂载获取引用；若期间挂载树被修改，顺序号变化导致重试。修改挂载树的一方（如 `attach_recursive_mnt()` 中的 `lock_mount_hash()`）以写方式持有 `mount_lock`。这种"读者无锁、写者顺序锁"的设计让每天上亿次的路径查找几乎不受挂载操作的影响。

### 新挂载 API

v5.2 引入的新 API 把上述流程拆成用户态可见的几步：

```
fd = fsopen("ramfs", FSOPEN_CLOEXEC);            // 创建 fs_context，返回一个 fd
fsconfig(fd, FSCONFIG_SET_STRING, "mode", "0700", 0);   // 逐个设置参数
fsconfig(fd, FSCONFIG_CMD_CREATE, NULL, NULL, 0);       // vfs_get_tree()，创建超级块
mfd = fsmount(fd, FSMOUNT_CLOEXEC, MOUNT_ATTR_NODEV);   // 创建游离的 struct mount
move_mount(mfd, "", AT_FDCWD, "/mnt", MOVE_MOUNT_F_EMPTY_PATH);  // 接入挂载树
```

`fsopen()` 只做前半段：

[`fs/fsopen.c:120-158`](../../linux/fs/fsopen.c#L120-L158)（v6.18.60）

```c
SYSCALL_DEFINE2(fsopen, const char __user *, _fs_name, unsigned int, flags)
{
	struct file_system_type *fs_type;
	struct fs_context *fc;
	const char *fs_name;
	int ret;

	if (!may_mount())
		return -EPERM;

	if (flags & ~FSOPEN_CLOEXEC)
		return -EINVAL;

	fs_name = strndup_user(_fs_name, PAGE_SIZE);
	if (IS_ERR(fs_name))
		return PTR_ERR(fs_name);

	fs_type = get_fs_type(fs_name);
	kfree(fs_name);
	if (!fs_type)
		return -ENODEV;

	fc = fs_context_for_mount(fs_type, 0);
	put_filesystem(fs_type);
	if (IS_ERR(fc))
		return PTR_ERR(fc);

	fc->phase = FS_CONTEXT_CREATE_PARAMS;

	ret = fscontext_alloc_log(fc);
	if (ret < 0)
		goto err_fc;

	return fscontext_create_fd(fc, flags & FSOPEN_CLOEXEC ? O_CLOEXEC : 0);

err_fc:
	put_fs_context(fc);
	return ret;
}
```

`FSCONFIG_CMD_CREATE` 命令对应 `vfs_cmd_create()`，它调用的正是同一个 `vfs_get_tree()`：

[`fs/fsopen.c:220-252`](../../linux/fs/fsopen.c#L220-L252)（v6.18.60）

```c
static int vfs_cmd_create(struct fs_context *fc, bool exclusive)
{
	struct super_block *sb;
	int ret;

	if (fc->phase != FS_CONTEXT_CREATE_PARAMS)
		return -EBUSY;

	if (!mount_capable(fc))
		return -EPERM;

	fc->phase = FS_CONTEXT_CREATING;
	fc->exclusive = exclusive;

	ret = vfs_get_tree(fc);
	if (ret) {
		fc->phase = FS_CONTEXT_FAILED;
		return ret;
	}

	sb = fc->root->d_sb;
	ret = security_sb_kern_mount(sb);
	if (unlikely(ret)) {
		fc_drop_locked(fc);
		fc->phase = FS_CONTEXT_FAILED;
		return ret;
	}

	/* vfs_get_tree() callchains will have grabbed @s_umount */
	up_write(&sb->s_umount);
	fc->phase = FS_CONTEXT_AWAITING_MOUNT;
	return 0;
}
```

`fsmount()`（[`fs/namespace.c:4377-4496`](../../linux/fs/namespace.c#L4377-L4496)）用 `vfs_create_mount()` 创建挂载，把它放进一个新建的**匿名挂载命名空间**，再返回一个指向挂载根的 `O_PATH` 文件描述符。此时挂载已经存在，但不在任何进程可见的挂载树中；用户态可以继续用 `mount_setattr()` 调整属性，最后用 `move_mount()` 把它移到目标位置，后者最终走的仍是 `do_move_mount()` → `attach_recursive_mnt()`。

新 API 的好处在于：参数可以逐个设置并得到精确的错误信息（通过读取 fs_context 的 fd）；创建超级块与接入挂载树分离，便于容器运行时先在宿主中准备好挂载，再整体移入容器；以文件描述符代表挂载，避免了路径名带来的竞态。

### 卸载

```
umount2(dir, flags)
└── ksys_umount() → path_umount()                 fs/namespace.c:2066
    ├── do_umount()                               fs/namespace.c:1891
    │   ├── MNT_EXPIRE / MNT_FORCE 处理
    │   ├── propagate_mount_busy()                // 是否还有引用
    │   └── umount_tree()                         // 从挂载哈希表与命名空间摘除
    └── mntput_no_expire()                        fs/namespace.c:1412
        └── mntput_no_expire_slowpath()           // 最后一个引用
            └── task_work_add(cleanup_mnt)        // 返回用户态前执行
                └── cleanup_mnt()                 fs/namespace.c:1315
                    ├── dput(mnt_root)
                    └── deactivate_super()        fs/super.c:499
                        └── deactivate_locked_super()
                            └── fs->kill_sb()     // ramfs_kill_sb() → kill_litter_super()
                                └── generic_shutdown_super()
```

`do_umount()` 的核心判断：

[`fs/namespace.c:1959-1990`](../../linux/fs/namespace.c#L1959-L1990)（v6.18.60）

```c
	namespace_lock();
	lock_mount_hash();

	/* Repeat the earlier racy checks, now that we are holding the locks */
	retval = -EINVAL;
	if (!check_mnt(mnt))
		goto out;

	if (mnt->mnt.mnt_flags & MNT_LOCKED)
		goto out;

	if (!mnt_has_parent(mnt)) /* not the absolute root */
		goto out;

	event++;
	if (flags & MNT_DETACH) {
		umount_tree(mnt, UMOUNT_PROPAGATE);
		retval = 0;
	} else {
		smp_mb(); // paired with __legitimize_mnt()
		shrink_submounts(mnt);
		retval = -EBUSY;
		if (!propagate_mount_busy(mnt, 2)) {
			umount_tree(mnt, UMOUNT_PROPAGATE|UMOUNT_SYNC);
			retval = 0;
		}
	}
out:
	unlock_mount_hash();
	namespace_unlock();
	return retval;
}
```

普通卸载要求挂载不再被使用：`propagate_mount_busy(mnt, 2)` 检查引用计数是否只剩"父挂载持有的 1 个 + 本次 `umount` 查找路径持有的 1 个"。只要还有进程以它为当前目录、打开着其中的文件，就返回 `-EBUSY`。`MNT_DETACH`（`umount -l`，惰性卸载）则跳过这项检查，立即把挂载从树中摘除，使之对新的查找不可见；已打开的文件可以继续使用，直到最后一个引用释放时才真正清理。

挂载的最后一个引用释放时，清理工作通过 `task_work` 推迟到当前任务返回用户态之前执行（内核线程则交给工作队列），这样调用 `mntput()` 的上下文不必持有可能导致死锁的锁：

[`fs/namespace.c:1398-1410`](../../linux/fs/namespace.c#L1398-L1410)（v6.18.60）

```c
	if (likely(!(mnt->mnt.mnt_flags & MNT_INTERNAL))) {
		struct task_struct *task = current;
		if (likely(!(task->flags & PF_KTHREAD))) {
			init_task_work(&mnt->mnt_rcu, __cleanup_mnt);
			if (!task_work_add(task, &mnt->mnt_rcu, TWA_RESUME))
				return;
		}
		if (llist_add(&mnt->mnt_llist, &delayed_mntput_list))
			schedule_delayed_work(&delayed_mntput_work, 1);
		return;
	}
	cleanup_mnt(mnt);
}
```

`cleanup_mnt()` 释放挂载持有的根 dentry 和超级块活跃引用。当 `s_active` 降为 0，`deactivate_locked_super()` 调用文件系统的 `kill_sb`：

[`fs/super.c:466-487`](../../linux/fs/super.c#L466-L487)（v6.18.60）

```c
void deactivate_locked_super(struct super_block *s)
{
	struct file_system_type *fs = s->s_type;
	if (atomic_dec_and_test(&s->s_active)) {
		shrinker_free(s->s_shrink);
		fs->kill_sb(s);

		kill_super_notify(s);

		/*
		 * Since list_lru_destroy() may sleep, we cannot call it from
		 * put_super(), where we hold the sb_lock. Therefore we destroy
		 * the lru lists right now.
		 */
		list_lru_destroy(&s->s_dentry_lru);
		list_lru_destroy(&s->s_inode_lru);

		put_super(s);
	} else {
		super_unlock_excl(s);
	}
}
```

`kill_sb` 通常会调用 `generic_shutdown_super()`（[`fs/super.c:612-680`](../../linux/fs/super.c#L612-L680)），按顺序回收该文件系统的 dentry（`shrink_dcache_for_umount()`）、同步写回脏数据（`sync_filesystem()`）、驱逐所有 inode（`evict_inodes()`）、调用 `->put_super`。ramfs 使用的 `kill_litter_super()` 会先用 `d_genocide()` 释放那些被"钉"在内存中的 dentry（9.12 节）。

### 初始挂载树：rootfs

启动时还没有任何块设备驱动，内核需要一棵初始挂载树作为一切的起点。`mnt_init()` 在初始化完各种哈希表后调用 `init_mount_tree()`：

[`fs/namespace.c:6104-6128`](../../linux/fs/namespace.c#L6104-L6128)（v6.18.60）

```c
static void __init init_mount_tree(void)
{
	struct vfsmount *mnt;
	struct mount *m;
	struct path root;

	mnt = vfs_kern_mount(&rootfs_fs_type, 0, "rootfs", initramfs_options);
	if (IS_ERR(mnt))
		panic("Can't create rootfs");

	m = real_mount(mnt);
	init_mnt_ns.root = m;
	init_mnt_ns.nr_mounts = 1;
	mnt_add_to_ns(&init_mnt_ns, m);
	init_task.nsproxy->mnt_ns = &init_mnt_ns;
	get_mnt_ns(&init_mnt_ns);

	root.mnt = mnt;
	root.dentry = mnt->mnt_root;

	set_fs_pwd(current->fs, &root);
	set_fs_root(current->fs, &root);

	ns_tree_add(&init_mnt_ns);
}
```

`rootfs_fs_type` 实际是 tmpfs 或 ramfs 的一个实例：在 `CONFIG_TMPFS=y`（本书配置）的前提下，若启动参数没有指定 `root=`，或 `rootfstype=` 中包含 tmpfs，就使用 tmpfs，否则使用 ramfs（[`init/do_mounts.c:499-521`](../../linux/init/do_mounts.c#L499-L521)）。它成为初始挂载命名空间 `init_mnt_ns` 的根，0 号进程的根目录与当前目录也指向它。随后 initramfs 的内容被解压到 rootfs 中；真正的根文件系统要么由 initramfs 中的用户态程序挂载后 `switch_root` 过去，要么由内核的 `prepare_namespace()` 挂载到 `/root` 后用 `MS_MOVE` 移到 `/`。这也是 `/proc/self/mountinfo` 中 rootfs 不可见、却始终存在于挂载树底层的原因。

## 关键细节 / 设计考量

### 超级块、挂载与绑定挂载

`mount --bind /a /b` 不创建新的超级块，而是用 `clone_mnt()` 复制出一个新的 `struct mount`，它与原挂载共享超级块，但 `mnt_root` 指向 `/a` 对应的 dentry。于是同一组 dentry 和 inode 出现在挂载树的两个位置——这正是 `struct path` 必须同时包含挂载与 dentry 的原因。容器中常见的卷挂载就是绑定挂载。

| 操作 | 新 super_block | 新 mount | 说明 |
|---|---|---|---|
| 首次挂载 `/dev/sda1` | 是 | 是 | `sget_dev()` 新建 |
| 再次挂载 `/dev/sda1` 到另一位置 | 否 | 是 | 复用同一超级块，两处内容实时一致 |
| 绑定挂载 | 否 | 是 | `mnt_root` 可以是子目录 |
| 每次挂载 tmpfs / ramfs | 是 | 是 | `get_tree_nodev()` 从不复用 |

### 挂载命名空间与传播

每个进程通过 `task->nsproxy->mnt_ns` 属于一个挂载命名空间。`unshare(CLONE_NEWNS)` 会复制整棵挂载树（`copy_tree()`），此后两个命名空间中的挂载、卸载互不影响。

完全隔离有时并不方便：宿主新插入一个 U 盘，希望所有容器都能看到。为此内核引入了**挂载传播（Mount Propagation）**：挂载可以是 shared（同一对等组内的挂载事件相互传播）、slave（只接收主挂载的事件）、private（不传播，默认）或 unbindable。`attach_recursive_mnt()` 中的 `propagate_mnt()` 正是在目标是 shared 挂载时，把新挂载复制到对等组中的每一个成员上。详细语义见 `Documentation/filesystems/sharedsubtree.rst`。

### 并发控制

| 锁 | 定义 | 用途 |
|---|---|---|
| `namespace_sem` | [`fs/namespace.c:89`](../../linux/fs/namespace.c#L89) | 全局读写信号量，串行化所有挂载树修改 |
| `mount_lock` | [`fs/namespace.c:133`](../../linux/fs/namespace.c#L133) | 全局顺序锁：写者修改挂载哈希表与树结构，读者（路径查找）无锁读、靠顺序号检测并发修改 |
| 挂载点 inode 的 `i_rwsem` | — | 防止挂载点目录在挂载过程中被删除 |
| `sb->s_umount` | — | 串行化同一超级块的挂载、重新挂载、卸载、冻结 |
| `sb_lock` | [`fs/super.c:46`](../../linux/fs/super.c#L46) | 保护 `super_blocks` 与各类型的 `fs_supers` 链表 |

### 超级块的两种引用

`s_active` 与 `s_count` 的区别常令人困惑。`s_active` 表示"文件系统处于活跃状态"，每个挂载持有一个，降到 0 时 `kill_sb` 被调用，文件系统被关闭。`s_count` 只保证 `struct super_block` 内存不被释放，遍历超级块的代码（如 `sync` 调用的 `iterate_supers()`）会临时持有它；此时即使文件系统已经被关闭，结构体仍然可以安全访问，遍历者通过检查 `SB_DYING`、`SB_DEAD` 等标志得知它已无效。

## 动手实验

### 挂载 ramfs 并观察挂载树

```bash
mkdir -p /mnt/r1 /mnt/r2
mount -t ramfs -o mode=0700 none /mnt/r1
echo hello > /mnt/r1/a.txt
mount --bind /mnt/r1 /mnt/r2            # 绑定挂载：共享超级块
cat /mnt/r2/a.txt
grep /mnt/r /proc/self/mountinfo        # 两行的设备号（第 3 列）相同，挂载 ID 不同
umount /mnt/r2 /mnt/r1
```

### 观察 mount 命令使用的系统调用

util-linux 2.39 起，`mount` 命令优先使用新挂载 API：

```bash
strace -f -e trace=mount,fsopen,fsconfig,fsmount,move_mount,mount_setattr \
	mount -t tmpfs -o size=16m none /mnt/r1
```

### 跟踪超级块的创建

```bash
sudo bpftrace -e '
kprobe:vfs_get_tree {
	$fc = (struct fs_context *)arg0;
	printf("%s: get_tree %s source=%s\n", comm,
	       str($fc->fs_type->name), str($fc->source));
}
kretprobe:sget_fc { printf("  -> sb %p\n", retval); }'
```

### 挂载命名空间隔离

```bash
unshare -m --propagation private bash
mount -t ramfs none /mnt/r1             # 只在新命名空间中可见
findmnt /mnt/r1
exit                                    # 回到原命名空间后 /mnt/r1 未挂载
```

## 小结

- Linux 把挂载拆成四层：文件系统类型（`file_system_type`）、实例（`super_block`）、挂载（`struct mount`）、挂载命名空间；同一超级块可以有多个挂载，绑定挂载复用超级块与 dentry。
- `mount(2)` 和新 API（`fsopen`/`fsconfig`/`fsmount`/`move_mount`）在内核中统一基于 `fs_context`：先 `init_fs_context` 与 `parse_param` 收集参数，再由 `get_tree` 获取超级块和根 dentry。
- `sget_fc()` 以"解锁分配、加锁重查"的方式查找或创建超级块；`get_tree_nodev/single/keyed/bdev` 的区别只是复用策略。
- `do_lock_mount()` 在 inode 锁与 `namespace_sem` 下确定最终挂载位置，`attach_recursive_mnt()` 处理传播并通过 `make_visible()` 把挂载插入以（父挂载，挂载点）为键的哈希表，从此对路径查找可见。
- 路径查找经 `__lookup_mnt()` 在 RCU 与 `mount_lock` 顺序锁下无锁地穿越挂载点。
- 卸载时挂载先从树中摘除，最后一个引用释放后经 `task_work` 执行 `cleanup_mnt()`，`s_active` 归零时调用 `kill_sb` 关闭文件系统。

## 参考

- `linux/Documentation/filesystems/mount_api.rst`：fs_context 与新挂载 API
- `linux/Documentation/filesystems/sharedsubtree.rst`：挂载传播
- `linux/Documentation/filesystems/ramfs-rootfs-initramfs.rst`：rootfs 与 initramfs
- David Howells, "VFS: Introduce filesystem context" 补丁系列（v5.2 合入）
- `man 2 fsopen`、`man 2 fsmount`、`man 2 move_mount`、`man 7 mount_namespaces`
