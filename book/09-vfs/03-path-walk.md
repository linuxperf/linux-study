# 9.3 路径名查找

> **本节源码**：[`fs/namei.c`](../../linux/fs/namei.c)、[`include/linux/namei.h`](../../linux/include/linux/namei.h)、[`include/linux/dcache.h`](../../linux/include/linux/dcache.h)、[`fs/dcache.c`](../../linux/fs/dcache.c)、[`fs/namespace.c`](../../linux/fs/namespace.c)
> **关键函数**：`filename_lookup()`、`path_lookupat()`、`path_init()`、`link_path_walk()`、`hash_name()`、`may_lookup()`、`walk_component()`、`lookup_fast()`、`lookup_slow()`、`step_into()`、`handle_mounts()`、`__follow_mount_rcu()`、`pick_link()`、`handle_dots()`、`follow_dotdot_rcu()`、`try_to_unlazy()`、`complete_walk()`
> **前置知识**：9.1 VFS 概述、9.2 文件系统注册与挂载、7.5 读写锁与顺序锁、7.9 RCU 原理与使用

## 概述

**路径名查找（Pathname Lookup）** 把一个字符串路径（如 `/usr/lib/../bin/ls`）解析为挂载树中的一个位置，即 `struct path`（挂载，dentry）。`open()`、`stat()`、`execve()`、`mkdir()`、`unlink()` 等几乎所有以路径为参数的系统调用都从它开始，因此它是 VFS 中被执行最频繁、对性能最敏感的路径之一。

路径查找要正确处理许多情况：绝对路径与相对路径、`.` 与 `..`、符号链接（可以嵌套，也可以指向绝对路径）、挂载点穿越、每一级目录的搜索权限，以及 `openat2()` 引入的各种限制（不准跨越挂载点、不准逃出起始目录等）。同时它还要与并发的重命名、删除、挂载、卸载正确地交错执行。

Linux 的解决方案是**两种模式**的查找：

- **RCU 路径遍历（RCU-walk）**：在 RCU 读临界区内进行，沿途不获取任何引用计数、不加任何锁，只用 dentry 上的顺序计数 `d_seq` 以及全局的 `mount_lock`、`rename_lock` 顺序号验证读到的数据是否一致。绝大多数查找完全命中 dcache，可以在这种模式下完成。
- **引用路径遍历（REF-walk）**：传统方式，沿途对 dentry 与挂载获取引用、必要时睡眠（读磁盘、等锁）。

查找总是先尝试 RCU-walk；一旦遇到无法在不睡眠的前提下完成的情况，就尝试"就地"转为 REF-walk（`try_to_unlazy()`）；如果连转换都失败（数据已被并发修改），就返回 `-ECHILD`，从头以 REF-walk 重来。RCU-walk 由 Nick Piggin 在 v2.6.38 引入，是 dcache 可扩展性改造的核心成果。

## 核心数据结构

### struct nameidata

查找过程的全部状态保存在调用者栈上的 `struct nameidata` 中：

[`fs/namei.c:631-659`](../../linux/fs/namei.c#L631-L659)（v6.18.60）

```c
#define EMBEDDED_LEVELS 2
struct nameidata {
	struct path	path;
	struct qstr	last;
	struct path	root;
	struct inode	*inode; /* path.dentry.d_inode */
	unsigned int	flags, state;
	unsigned	seq, next_seq, m_seq, r_seq;
	int		last_type;
	unsigned	depth;
	int		total_link_count;
	struct saved {
		struct path link;
		struct delayed_call done;
		const char *name;
		unsigned seq;
	} *stack, internal[EMBEDDED_LEVELS];
	struct filename	*name;
	const char *pathname;
	struct nameidata *saved;
	unsigned	root_seq;
	int		dfd;
	vfsuid_t	dir_vfsuid;
	umode_t		dir_mode;
} __randomize_layout;

#define ND_ROOT_PRESET 1
#define ND_ROOT_GRABBED 2
#define ND_JUMPED 4
```

| 字段 | 含义 |
|---|---|
| `path` / `inode` | 当前所在位置（已解析到的目录）及其 inode |
| `last` / `last_type` | 刚解析出的路径分量（带哈希值的 `qstr`）及其类型：`LAST_NORM` 普通名字、`LAST_DOT`、`LAST_DOTDOT`、`LAST_ROOT` |
| `root` | 本次查找的根，用于绝对路径与 `..` 的边界判断 |
| `flags` | `LOOKUP_*` 标志，其中 `LOOKUP_RCU` 表示当前处于 RCU-walk |
| `state` | 内部状态：`ND_ROOT_PRESET`（调用者指定了根）、`ND_ROOT_GRABBED`（已持有根的引用）、`ND_JUMPED`（发生过跳转，需要弱重验证） |
| `seq` / `next_seq` | 当前 dentry 与下一个 dentry 的 `d_seq` 采样值 |
| `m_seq` / `r_seq` | 查找开始时 `mount_lock` 与 `rename_lock` 的顺序号 |
| `stack` / `internal` / `depth` | 符号链接栈：每遇到一个需要跟随的符号链接压入一项，记录尚未处理完的剩余路径；前 2 层（`EMBEDDED_LEVELS`）直接放在结构体内 |
| `total_link_count` | 已跟随的符号链接总数，上限 `MAXSYMLINKS`（40） |
| `saved` | 嵌套查找时保存外层的 `nameidata`（例如在 `->get_link` 中又发起查找） |

`nameidata` 的地址保存在 `current->nameidata` 中，`set_nameidata()` 与 `restore_nameidata()`（[`fs/namei.c:676-696`](../../linux/fs/namei.c#L676-L696)）负责设置与恢复。

### 查找标志

[`include/linux/namei.h:20-49`](../../linux/include/linux/namei.h#L20-L49)（v6.18.60）

```c
/* pathwalk mode */
#define LOOKUP_FOLLOW		BIT(0)	/* follow links at the end */
#define LOOKUP_DIRECTORY	BIT(1)	/* require a directory */
#define LOOKUP_AUTOMOUNT	BIT(2)  /* force terminal automount */
#define LOOKUP_EMPTY		BIT(3)	/* accept empty path [user_... only] */
#define LOOKUP_LINKAT_EMPTY	BIT(4) /* Linkat request with empty path. */
#define LOOKUP_DOWN		BIT(5)	/* follow mounts in the starting point */
#define LOOKUP_MOUNTPOINT	BIT(6)	/* follow mounts in the end */
#define LOOKUP_REVAL		BIT(7)	/* tell ->d_revalidate() to trust no cache */
#define LOOKUP_RCU		BIT(8)	/* RCU pathwalk mode; semi-internal */
#define LOOKUP_CACHED		BIT(9) /* Only do cached lookup */
#define LOOKUP_PARENT		BIT(10)	/* Looking up final parent in path */
/* 5 spare bits for pathwalk */

/* These tell filesystem methods that we are dealing with the final component... */
#define LOOKUP_OPEN		BIT(16)	/* ... in open */
#define LOOKUP_CREATE		BIT(17)	/* ... in object creation */
#define LOOKUP_EXCL		BIT(18)	/* ... in target must not exist */
#define LOOKUP_RENAME_TARGET	BIT(19)	/* ... in destination of rename() */

/* 4 spare bits for intent */

/* Scoping flags for lookup. */
#define LOOKUP_NO_SYMLINKS	BIT(24) /* No symlink crossing. */
#define LOOKUP_NO_MAGICLINKS	BIT(25) /* No nd_jump_link() crossing. */
#define LOOKUP_NO_XDEV		BIT(26) /* No mountpoint crossing. */
#define LOOKUP_BENEATH		BIT(27) /* No escaping from starting point. */
#define LOOKUP_IN_ROOT		BIT(28) /* Treat dirfd as fs root. */
/* LOOKUP_* flags which do scope-related checks based on the dirfd. */
#define LOOKUP_IS_SCOPED (LOOKUP_BENEATH | LOOKUP_IN_ROOT)
```

标志分为三组：控制遍历行为的（如 `LOOKUP_FOLLOW` 决定最后一个分量是符号链接时是否跟随，`LOOKUP_DIRECTORY` 要求结果必须是目录）、告诉文件系统意图的（`LOOKUP_OPEN`、`LOOKUP_CREATE` 等，供 `->d_revalidate` 参考）、以及 `openat2()` 的 `RESOLVE_*` 对应的作用域限制。

### qstr：带哈希的名字

[`include/linux/dcache.h:49-56`](../../linux/include/linux/dcache.h#L49-L56)（v6.18.60）

```c
struct qstr {
	union {
		struct {
			HASH_LEN_DECLARE;
		};
		u64 hash_len;
	};
	const unsigned char *name;
```

路径分量用 `struct qstr` 表示。它把 32 位哈希与 32 位长度合并为一个 64 位的 `hash_len`，dcache 比较时一次 64 位比较就能同时排除哈希或长度不同的候选项。

## 主流程分析

### 调用链总览

```
filename_lookup()                       fs/namei.c:2726
├── path_lookupat(LOOKUP_RCU)           // 第一次：RCU-walk
├── path_lookupat(0)                    // 返回 -ECHILD 时：REF-walk
└── path_lookupat(LOOKUP_REVAL)         // 返回 -ESTALE 时：要求文件系统重新验证
    ├── path_init()                     // 确定起点：根、cwd 或 dirfd
    ├── link_path_walk()                // 逐个处理除最后一个以外的分量
    │   └── 循环:
    │       ├── may_lookup()            // 目录的搜索（执行）权限
    │       ├── hash_name()             // 计算分量哈希与长度
    │       └── walk_component()
    │           ├── handle_dots()       // "." 与 ".."
    │           ├── lookup_fast()       // dcache：__d_lookup_rcu() / __d_lookup()
    │           ├── lookup_slow()       // 未命中：d_alloc_parallel() + ->lookup()
    │           └── step_into()
    │               ├── handle_mounts() // 穿越挂载点
    │               └── pick_link()     // 符号链接：压栈并返回链接内容
    ├── lookup_last()                   // 处理最后一个分量
    ├── complete_walk()                 // RCU → REF，弱重验证
    └── terminate_walk()                // 释放引用 / 退出 RCU
```

### filename_lookup()：三次尝试

[`fs/namei.c:2726-2745`](../../linux/fs/namei.c#L2726-L2745)（v6.18.60）

```c
int filename_lookup(int dfd, struct filename *name, unsigned flags,
		    struct path *path, const struct path *root)
{
	int retval;
	struct nameidata nd;
	if (IS_ERR(name))
		return PTR_ERR(name);
	set_nameidata(&nd, dfd, name, root);
	retval = path_lookupat(&nd, flags | LOOKUP_RCU, path);
	if (unlikely(retval == -ECHILD))
		retval = path_lookupat(&nd, flags, path);
	if (unlikely(retval == -ESTALE))
		retval = path_lookupat(&nd, flags | LOOKUP_REVAL, path);

	if (likely(!retval))
		audit_inode(name, path->dentry,
			    flags & LOOKUP_MOUNTPOINT ? AUDIT_INODE_NOEVAL : 0);
	restore_nameidata();
	return retval;
}
```

三次尝试构成了一个逐步降级的策略：

1. **RCU-walk**：最快，但只要遇到需要睡眠或数据不一致就放弃，返回 `-ECHILD`。
2. **REF-walk**：可以睡眠，几乎总能完成。
3. **带 `LOOKUP_REVAL` 的 REF-walk**：网络文件系统（如 NFS）缓存的 dentry 可能已经过时，服务器返回 `ESTALE` 时，以该标志要求 `->d_revalidate` 不信任任何缓存、重新向服务器确认。

`struct filename` 由 `getname()` 从用户空间拷入（[`fs/namei.c:135`](../../linux/fs/namei.c#L135)），路径名存放在 `names_cache` 中，带引用计数以便审计子系统保存。

### path_lookupat()：主循环

[`fs/namei.c:2676-2724`](../../linux/fs/namei.c#L2676-L2724)（v6.18.60）

```c
static inline const char *lookup_last(struct nameidata *nd)
{
	if (nd->last_type == LAST_NORM && nd->last.name[nd->last.len])
		nd->flags |= LOOKUP_FOLLOW | LOOKUP_DIRECTORY;

	return walk_component(nd, WALK_TRAILING);
}
...
static int path_lookupat(struct nameidata *nd, unsigned flags, struct path *path)
{
	const char *s = path_init(nd, flags);
	int err;

	if (unlikely(flags & LOOKUP_DOWN) && !IS_ERR(s)) {
		err = handle_lookup_down(nd);
		if (unlikely(err < 0))
			s = ERR_PTR(err);
	}

	while (!(err = link_path_walk(s, nd)) &&
	       (s = lookup_last(nd)) != NULL)
		;
	if (!err && unlikely(nd->flags & LOOKUP_MOUNTPOINT)) {
		err = handle_lookup_down(nd);
		nd->state &= ~ND_JUMPED; // no d_weak_revalidate(), please...
	}
	if (!err)
		err = complete_walk(nd);

	if (!err && nd->flags & LOOKUP_DIRECTORY)
		if (!d_can_lookup(nd->path.dentry))
			err = -ENOTDIR;
	if (!err) {
		*path = nd->path;
		nd->path.mnt = NULL;
		nd->path.dentry = NULL;
	}
	terminate_walk(nd);
	return err;
}
```

主循环的结构非常紧凑：`link_path_walk()` 解析到最后一个分量之前（`nd->last` 存放最后一个分量，`nd->path` 是它的父目录）；`lookup_last()` 处理最后一个分量。如果最后一个分量是需要跟随的符号链接，`lookup_last()` 返回链接内容，循环把它当作新的路径再交给 `link_path_walk()`；否则返回 `NULL`，循环结束。

`lookup_last()` 的第一行处理路径末尾的斜杠：`foo/` 中 `last.name[last.len]` 是 `'/'`，此时最后一个分量必须是目录，并且即使是符号链接也要跟随。

### path_init()：确定起点

[`fs/namei.c:2569-2616`](../../linux/fs/namei.c#L2569-L2616)（v6.18.60）

```c
static const char *path_init(struct nameidata *nd, unsigned flags)
{
	int error;
	const char *s = nd->pathname;

	/* LOOKUP_CACHED requires RCU, ask caller to retry */
	if ((flags & (LOOKUP_RCU | LOOKUP_CACHED)) == LOOKUP_CACHED)
		return ERR_PTR(-EAGAIN);

	if (!*s)
		flags &= ~LOOKUP_RCU;
	if (flags & LOOKUP_RCU)
		rcu_read_lock();
	else
		nd->seq = nd->next_seq = 0;

	nd->flags = flags;
	nd->state |= ND_JUMPED;

	nd->m_seq = __read_seqcount_begin(&mount_lock.seqcount);
	nd->r_seq = __read_seqcount_begin(&rename_lock.seqcount);
	smp_rmb();

...

	nd->root.mnt = NULL;

	/* Absolute pathname -- fetch the root (LOOKUP_IN_ROOT uses nd->dfd). */
	if (*s == '/' && !(flags & LOOKUP_IN_ROOT)) {
		error = nd_jump_root(nd);
		if (unlikely(error))
			return ERR_PTR(error);
		return s;
	}
```

起点有三种：调用者预设的根（`ND_ROOT_PRESET`，如 `vfs_path_lookup()`）、绝对路径时的进程根目录（`nd_jump_root()` → `set_root()`）、相对路径时的当前工作目录或 `dirfd` 指向的目录：

[`fs/namei.c:2618-2633`](../../linux/fs/namei.c#L2618-L2633)（v6.18.60）

```c
	/* Relative pathname -- get the starting-point it is relative to. */
	if (nd->dfd == AT_FDCWD) {
		if (flags & LOOKUP_RCU) {
			struct fs_struct *fs = current->fs;
			unsigned seq;

			do {
				seq = read_seqbegin(&fs->seq);
				nd->path = fs->pwd;
				nd->inode = nd->path.dentry->d_inode;
				nd->seq = __read_seqcount_begin(&nd->path.dentry->d_seq);
			} while (read_seqretry(&fs->seq, seq));
		} else {
			get_fs_pwd(current->fs, &nd->path);
			nd->inode = nd->path.dentry->d_inode;
		}
```

在 RCU 模式下读取 `fs->pwd` 不能获取引用，因此用 `fs->seq` 顺序锁保证 `pwd` 的 mnt 与 dentry 是同一时刻的一对值，并采样该 dentry 的 `d_seq`。`path_init()` 还在开头记录了 `mount_lock` 与 `rename_lock` 的顺序号（`m_seq`、`r_seq`），之后的穿越挂载点、`..` 处理会用它们检测并发修改。

### link_path_walk()：逐个分量解析

`link_path_walk()` 是路径查找的核心循环。第一部分跳过开头的斜杠，然后对每个分量先检查权限、再计算哈希：

[`fs/namei.c:2470-2501`](../../linux/fs/namei.c#L2470-L2501)（v6.18.60）

```c
static int link_path_walk(const char *name, struct nameidata *nd)
{
	int depth = 0; // depth <= nd->depth
	int err;

	nd->last_type = LAST_ROOT;
	nd->flags |= LOOKUP_PARENT;
	if (IS_ERR(name))
		return PTR_ERR(name);
	if (*name == '/') {
		do {
			name++;
		} while (unlikely(*name == '/'));
	}
	if (unlikely(!*name)) {
		nd->dir_mode = 0; // short-circuit the 'hardening' idiocy
		return 0;
	}

	/* At this point we know we have a real path component. */
	for(;;) {
		struct mnt_idmap *idmap;
		const char *link;
		unsigned long lastword;

		idmap = mnt_idmap(nd->path.mnt);
		err = may_lookup(idmap, nd);
		if (unlikely(err))
			return err;

		nd->last.name = name;
		name = hash_name(nd, name, &lastword);
```

`may_lookup()` 检查当前目录的**搜索权限**（`MAY_EXEC`）——要进入一个目录并解析其中的名字，必须对它有执行权限。在 RCU 模式下带上 `MAY_NOT_BLOCK`，如果权限检查需要睡眠（例如要从磁盘读 ACL），就转为 REF-walk 再检查一次：

[`fs/namei.c:1852-1874`](../../linux/fs/namei.c#L1852-L1874)（v6.18.60）

```c
static inline int may_lookup(struct mnt_idmap *idmap,
			     struct nameidata *restrict nd)
{
	int err, mask;

	mask = nd->flags & LOOKUP_RCU ? MAY_NOT_BLOCK : 0;
	err = inode_permission(idmap, nd->inode, mask | MAY_EXEC);
	if (likely(!err))
		return 0;

	// If we failed, and we weren't in LOOKUP_RCU, it's final
	if (!(nd->flags & LOOKUP_RCU))
		return err;

	// Drop out of RCU mode to make sure it wasn't transient
	if (!try_to_unlazy(nd))
		return -ECHILD;	// redo it all non-lazy

	if (err != -ECHILD)	// hard error
		return err;

	return inode_permission(idmap, nd->inode, MAY_EXEC);
}
```

接着识别 `.` 与 `..`，对普通名字调用文件系统可能提供的 `->d_hash`（如大小写不敏感的文件系统需要对名字做规范化）：

[`fs/namei.c:2503-2526`](../../linux/fs/namei.c#L2503-L2526)（v6.18.60）

```c
		switch(lastword) {
		case LAST_WORD_IS_DOTDOT:
			nd->last_type = LAST_DOTDOT;
			nd->state |= ND_JUMPED;
			break;

		case LAST_WORD_IS_DOT:
			nd->last_type = LAST_DOT;
			break;

		default:
			nd->last_type = LAST_NORM;
			nd->state &= ~ND_JUMPED;

			struct dentry *parent = nd->path.dentry;
			if (unlikely(parent->d_flags & DCACHE_OP_HASH)) {
				err = parent->d_op->d_hash(parent, &nd->last);
				if (err < 0)
					return err;
			}
		}

		if (!*name)
			goto OK;
```

最后判断是否到达路径末尾。不是最后一个分量时调用 `walk_component(nd, WALK_MORE)` 进入下一级；是最后一个分量时直接返回，把它留给调用者（`lookup_last()` 或 `open_last_lookups()`），因为最后一个分量往往需要特殊处理（创建、不跟随符号链接等）：

[`fs/namei.c:2527-2566`](../../linux/fs/namei.c#L2527-L2566)（v6.18.60）

```c
		/*
		 * If it wasn't NUL, we know it was '/'. Skip that
		 * slash, and continue until no more slashes.
		 */
		do {
			name++;
		} while (unlikely(*name == '/'));
		if (unlikely(!*name)) {
OK:
			/* pathname or trailing symlink, done */
			if (!depth) {
				nd->dir_vfsuid = i_uid_into_vfsuid(idmap, nd->inode);
				nd->dir_mode = nd->inode->i_mode;
				nd->flags &= ~LOOKUP_PARENT;
				return 0;
			}
			/* last component of nested symlink */
			name = nd->stack[--depth].name;
			link = walk_component(nd, 0);
		} else {
			/* not the last component */
			link = walk_component(nd, WALK_MORE);
		}
		if (unlikely(link)) {
			if (IS_ERR(link))
				return PTR_ERR(link);
			/* a symlink to follow */
			nd->stack[depth++].name = name;
			name = link;
			continue;
		}
		if (unlikely(!d_can_lookup(nd->path.dentry))) {
			if (nd->flags & LOOKUP_RCU) {
				if (!try_to_unlazy(nd))
					return -ECHILD;
			}
			return -ENOTDIR;
		}
	}
}
```

注意符号链接的处理：`walk_component()` 若返回非空字符串，说明遇到了需要跟随的符号链接，返回值就是链接内容。`link_path_walk()` 把当前尚未解析完的剩余路径 `name` 压入 `nd->stack[depth]`，然后把 `name` 换成链接内容继续循环；链接内容解析到末尾时（`OK` 标签处 `depth > 0`），再从栈中弹出原来的剩余路径接着走。于是符号链接的展开被转化为迭代，而不是递归，避免了内核栈溢出。

### hash_name()：一次处理一个字

x86_64 开启了 `CONFIG_DCACHE_WORD_ACCESS`，`hash_name()` 每次读取 8 个字节，用位运算同时寻找 `'\0'` 与 `'/'` 并计算哈希：

[`fs/namei.c:2344-2368`](../../linux/fs/namei.c#L2344-L2368)（v6.18.60）

```c
static inline const char *hash_name(struct nameidata *nd,
				    const char *name,
				    unsigned long *lastword)
{
	unsigned long a, b, x, y = (unsigned long)nd->path.dentry;
	unsigned long adata, bdata, mask, len;
	const struct word_at_a_time constants = WORD_AT_A_TIME_CONSTANTS;

	/*
	 * The first iteration is special, because it can result in
	 * '.' and '..' and has no mixing other than the final fold.
	 */
	a = load_unaligned_zeropad(name);
	b = a ^ REPEAT_BYTE('/');
	if (has_zero(a, &adata, &constants) | has_zero(b, &bdata, &constants)) {
		adata = prep_zero_mask(a, adata, &constants);
		bdata = prep_zero_mask(b, bdata, &constants);
		mask = create_zero_mask(adata | bdata);
		a &= zero_bytemask(mask);
		*lastword = a;
		len = find_zero(mask);
		nd->last.hash = fold_hash(a, y);
		nd->last.len = len;
		return name + len;
	}
```

`load_unaligned_zeropad()` 允许越过字符串末尾读取（若跨入未映射的页，异常修复代码会补零），`has_zero()` 系列宏用经典的"字中查零字节"技巧，在常数条指令内定位分隔符。第一个字单独处理，顺便提取 `lastword`：若分量是 `.` 或 `..`，它们一定落在第一个字中，于是 `link_path_walk()` 用一次整数比较就能识别（`LAST_WORD_IS_DOT` 为 `0x2e`，`LAST_WORD_IS_DOTDOT` 为 `0x2e2e`）。哈希的种子是父目录 dentry 的地址（`y = nd->path.dentry`），因此同名文件在不同目录下的哈希值不同，自然地分散在哈希表中。更长的分量在后续循环中用 `HASH_MIX` 逐字混合（[`fs/namei.c:2370-2390`](../../linux/fs/namei.c#L2370-L2390)）。

### walk_component() 与 lookup_fast()

[`fs/namei.c:2163-2187`](../../linux/fs/namei.c#L2163-L2187)（v6.18.60）

```c
static const char *walk_component(struct nameidata *nd, int flags)
{
	struct dentry *dentry;
	/*
	 * "." and ".." are special - ".." especially so because it has
	 * to be able to know about the current root directory and
	 * parent relationships.
	 */
	if (unlikely(nd->last_type != LAST_NORM)) {
		if (!(flags & WALK_MORE) && nd->depth)
			put_link(nd);
		return handle_dots(nd, nd->last_type);
	}
	dentry = lookup_fast(nd);
	if (IS_ERR(dentry))
		return ERR_CAST(dentry);
	if (unlikely(!dentry)) {
		dentry = lookup_slow(&nd->last, nd->path.dentry, nd->flags);
		if (IS_ERR(dentry))
			return ERR_CAST(dentry);
	}
	if (!(flags & WALK_MORE) && nd->depth)
		put_link(nd);
	return step_into(nd, flags, dentry);
}
```

普通分量先走 `lookup_fast()` 在 dcache 中查找，未命中再走 `lookup_slow()`：

[`fs/namei.c:1739-1786`](../../linux/fs/namei.c#L1739-L1786)（v6.18.60）

```c
static struct dentry *lookup_fast(struct nameidata *nd)
{
	struct dentry *dentry, *parent = nd->path.dentry;
	int status = 1;

	...
	if (nd->flags & LOOKUP_RCU) {
		dentry = __d_lookup_rcu(parent, &nd->last, &nd->next_seq);
		if (unlikely(!dentry)) {
			if (!try_to_unlazy(nd))
				return ERR_PTR(-ECHILD);
			return NULL;
		}

		/*
		 * This sequence count validates that the parent had no
		 * changes while we did the lookup of the dentry above.
		 */
		if (read_seqcount_retry(&parent->d_seq, nd->seq))
			return ERR_PTR(-ECHILD);

		status = d_revalidate(nd->inode, &nd->last, dentry, nd->flags);
		if (likely(status > 0))
			return dentry;
		if (!try_to_unlazy_next(nd, dentry))
			return ERR_PTR(-ECHILD);
		if (status == -ECHILD)
			/* we'd been told to redo it in non-rcu mode */
			status = d_revalidate(nd->inode, &nd->last,
					      dentry, nd->flags);
	} else {
		dentry = __d_lookup(parent, &nd->last);
		if (unlikely(!dentry))
			return NULL;
		status = d_revalidate(nd->inode, &nd->last, dentry, nd->flags);
	}
	if (unlikely(status <= 0)) {
		if (!status)
			d_invalidate(dentry);
		dput(dentry);
		return ERR_PTR(status);
	}
	return dentry;
}
```

RCU 模式下的关键在于**顺序计数的链式校验**：

1. `__d_lookup_rcu()` 在哈希表中找到子 dentry，并返回它的 `d_seq` 采样值，存入 `nd->next_seq`（9.4 节）。
2. 随后检查**父** dentry 的 `d_seq` 是否仍等于 `nd->seq`。如果父目录在此期间被重命名或删除，顺序号已变，查找结果不可信，返回 `-ECHILD`。
3. 子 dentry 的正确性则由下一步 `step_into()` 检查 `next_seq` 来保证。

这样，从根到叶的每一步都由"先采样子的序号、再验证父的序号"首尾相扣，构成一条完整的一致性链，而整个过程中没有写过任何共享缓存行。

若文件系统提供了 `->d_revalidate`（网络文件系统、procfs 等），还要在 RCU 模式下调用它；文件系统若需要睡眠才能确认，返回 `-ECHILD`，查找通过 `try_to_unlazy_next()` 转为 REF-walk 后再验证一次。验证失败（返回 0）的 dentry 被 `d_invalidate()` 作废。

### lookup_slow()：交给文件系统

dcache 未命中时，必须由文件系统的 `->lookup` 读取目录内容：

[`fs/namei.c:1789-1824`](../../linux/fs/namei.c#L1789-L1824)（v6.18.60）

```c
static struct dentry *__lookup_slow(const struct qstr *name,
				    struct dentry *dir,
				    unsigned int flags)
{
	struct dentry *dentry, *old;
	struct inode *inode = dir->d_inode;
	DECLARE_WAIT_QUEUE_HEAD_ONSTACK(wq);

	/* Don't go there if it's already dead */
	if (unlikely(IS_DEADDIR(inode)))
		return ERR_PTR(-ENOENT);
again:
	dentry = d_alloc_parallel(dir, name, &wq);
	if (IS_ERR(dentry))
		return dentry;
	if (unlikely(!d_in_lookup(dentry))) {
		int error = d_revalidate(inode, name, dentry, flags);
		if (unlikely(error <= 0)) {
			if (!error) {
				d_invalidate(dentry);
				dput(dentry);
				goto again;
			}
			dput(dentry);
			dentry = ERR_PTR(error);
		}
	} else {
		old = inode->i_op->lookup(inode, dentry, flags);
		d_lookup_done(dentry);
		if (unlikely(old)) {
			dput(dentry);
			dentry = old;
		}
	}
	return dentry;
}
```

外层的 `lookup_slow()`（[`fs/namei.c:1826-1836`](../../linux/fs/namei.c#L1826-L1836)）只对父目录持**共享**的 `i_rwsem`，因此同一目录中可以并行查找多个不同的名字。为了防止多个任务同时查找**同一个**名字而重复调用 `->lookup`，`d_alloc_parallel()` 会先分配一个处于"查找中"状态（`DCACHE_PAR_LOOKUP`）的 dentry 并放入一张专门的哈希表；后来者发现同名的查找中 dentry，就在其等待队列上睡眠，直到第一个任务调用 `d_lookup_done()` 完成查找（9.4 节）。

文件系统的 `->lookup` 在目录中查找名字：找到就读入 inode（如 ext4 的 `ext4_lookup()` → `ext4_iget()`），找不到就让 dentry 保持为负；然后通常以 `d_splice_alias(inode, dentry)` 把 dentry 与 inode 关联并加入 dcache 哈希表。以 ext4 为例：

[`fs/ext4/namei.c:1764-1817`](../../linux/fs/ext4/namei.c#L1764-L1817)（v6.18.60）

```c
static struct dentry *ext4_lookup(struct inode *dir, struct dentry *dentry, unsigned int flags)
{
	struct inode *inode;
	struct ext4_dir_entry_2 *de;
	struct buffer_head *bh;

	if (dentry->d_name.len > EXT4_NAME_LEN)
		return ERR_PTR(-ENAMETOOLONG);

	bh = ext4_lookup_entry(dir, dentry, &de);
	if (IS_ERR(bh))
		return ERR_CAST(bh);
	inode = NULL;
	if (bh) {
		__u32 ino = le32_to_cpu(de->inode);
		brelse(bh);
		if (!ext4_valid_inum(dir->i_sb, ino)) {
			EXT4_ERROR_INODE(dir, "bad inode number: %u", ino);
			return ERR_PTR(-EFSCORRUPTED);
		}
		if (unlikely(ino == dir->i_ino)) {
			EXT4_ERROR_INODE(dir, "'%pd' linked to parent dir",
					 dentry);
			return ERR_PTR(-EFSCORRUPTED);
		}
		inode = ext4_iget(dir->i_sb, ino, EXT4_IGET_NORMAL);
	...
	}

	return d_splice_alias(inode, dentry);
}
```

### step_into()：进入下一级

[`fs/namei.c:2011-2051`](../../linux/fs/namei.c#L2011-L2051)（v6.18.60）

```c
static const char *step_into(struct nameidata *nd, int flags,
		     struct dentry *dentry)
{
	struct path path;
	struct inode *inode;
	int err = handle_mounts(nd, dentry, &path);

	if (err < 0)
		return ERR_PTR(err);
	inode = path.dentry->d_inode;
	if (likely(!d_is_symlink(path.dentry)) ||
	   ((flags & WALK_TRAILING) && !(nd->flags & LOOKUP_FOLLOW)) ||
	   (flags & WALK_NOFOLLOW)) {
		/* not a symlink or should not follow */
		if (nd->flags & LOOKUP_RCU) {
			if (read_seqcount_retry(&path.dentry->d_seq, nd->next_seq))
				return ERR_PTR(-ECHILD);
			if (unlikely(!inode))
				return ERR_PTR(-ENOENT);
		} else {
			if (unlikely(unlink_may_reveal(nd, flags, &path, dentry)))
				return ERR_PTR(-ESTALE);
			dput(nd->path.dentry);
			if (nd->path.mnt != path.mnt)
				mntput(nd->path.mnt);
		}
		nd->path = path;
		nd->inode = inode;
		nd->seq = nd->next_seq;
		return NULL;
	}
	if (nd->flags & LOOKUP_RCU) {
		/* make sure that d_is_symlink above matches inode */
		if (read_seqcount_retry(&path.dentry->d_seq, nd->next_seq))
			return ERR_PTR(-ECHILD);
	} else {
		if (path.mnt == nd->path.mnt)
			mntget(path.mnt);
	}
	return pick_link(nd, &path, inode, flags);
}
```

`step_into()` 先通过 `handle_mounts()` 处理挂载点，然后判断结果：

- 不是符号链接，或者是最后一个分量且不要求跟随（未设置 `LOOKUP_FOLLOW`），或者明确要求不跟随（`WALK_NOFOLLOW`，用于 `..`）：直接把 `nd->path` 前移到新位置。RCU 模式下在这里校验 `next_seq`，确认子 dentry 在采样后没有被修改；同时，负 dentry 在此处被识别并返回 `-ENOENT`。
- 是需要跟随的符号链接：交给 `pick_link()`。

### 穿越挂载点

[`fs/namei.c:1625-1653`](../../linux/fs/namei.c#L1625-L1653)（v6.18.60）

```c
static inline int handle_mounts(struct nameidata *nd, struct dentry *dentry,
			  struct path *path)
{
	bool jumped;
	int ret;

	path->mnt = nd->path.mnt;
	path->dentry = dentry;
	if (nd->flags & LOOKUP_RCU) {
		unsigned int seq = nd->next_seq;
		if (likely(__follow_mount_rcu(nd, path)))
			return 0;
		// *path and nd->next_seq might've been clobbered
		path->mnt = nd->path.mnt;
		path->dentry = dentry;
		nd->next_seq = seq;
		if (!try_to_unlazy_next(nd, dentry))
			return -ECHILD;
	}
	ret = traverse_mounts(path, &jumped, &nd->total_link_count, nd->flags);
	if (jumped)
		nd->state |= ND_JUMPED;
	if (unlikely(ret)) {
		dput(path->dentry);
		if (path->mnt != nd->path.mnt)
			mntput(path->mnt);
	}
	return ret;
}
```

RCU 模式下由 `__follow_mount_rcu()` 处理：

[`fs/namei.c:1581-1623`](../../linux/fs/namei.c#L1581-L1623)（v6.18.60）

```c
static bool __follow_mount_rcu(struct nameidata *nd, struct path *path)
{
	struct dentry *dentry = path->dentry;
	unsigned int flags = dentry->d_flags;

	if (likely(!(flags & DCACHE_MANAGED_DENTRY)))
		return true;

	if (unlikely(nd->flags & LOOKUP_NO_XDEV))
		return false;

	for (;;) {
		/*
		 * Don't forget we might have a non-mountpoint managed dentry
		 * that wants to block transit.
		 */
		if (unlikely(flags & DCACHE_MANAGE_TRANSIT)) {
			int res = dentry->d_op->d_manage(path, true);
			if (res)
				return res == -EISDIR;
			flags = dentry->d_flags;
		}

		if (flags & DCACHE_MOUNTED) {
			struct mount *mounted = __lookup_mnt(path->mnt, dentry);
			if (mounted) {
				path->mnt = &mounted->mnt;
				dentry = path->dentry = mounted->mnt.mnt_root;
				nd->state |= ND_JUMPED;
				nd->next_seq = read_seqcount_begin(&dentry->d_seq);
				flags = dentry->d_flags;
				// makes sure that non-RCU pathwalk could reach
				// this state.
				if (read_seqretry(&mount_lock, nd->m_seq))
					return false;
				continue;
			}
			if (read_seqretry(&mount_lock, nd->m_seq))
				return false;
		}
		return !(flags & DCACHE_NEED_AUTOMOUNT);
	}
}
```

绝大多数 dentry 不是挂载点，第一行检查 `DCACHE_MANAGED_DENTRY`（即 `DCACHE_MOUNTED | DCACHE_NEED_AUTOMOUNT | DCACHE_MANAGE_TRANSIT`）就直接返回。如果是挂载点，就用 9.2 节介绍的 `__lookup_mnt()` 在挂载哈希表中查找挂在（当前挂载，当前 dentry）上的子挂载，跳到它的根，并循环处理"挂载点上又被挂载"的情况。每跳一次都检查 `mount_lock` 的顺序号，确认挂载树没有在此期间变化。

自动挂载（`DCACHE_NEED_AUTOMOUNT`，用于 autofs、NFS 的跨服务器引用等）需要睡眠，RCU 模式无法处理，返回 `false` 后转入 REF-walk 的 `traverse_mounts()`（[`fs/namei.c:1468-1542`](../../linux/fs/namei.c#L1468-L1542)），后者通过 `lookup_mnt()` 获取挂载的引用，必要时调用 `follow_automount()` 触发挂载。

### 符号链接：pick_link()

`pick_link()` 先用 `reserve_stack()` 检查是否超过 `MAXSYMLINKS`（40）并在需要时扩展符号链接栈（超过内嵌的 2 层时用 `kmalloc` 分配 40 项），把链接压栈，然后做安全检查：

[`fs/namei.c:1904-1929`](../../linux/fs/namei.c#L1904-L1929)（v6.18.60）

```c
static const char *pick_link(struct nameidata *nd, struct path *link,
		     struct inode *inode, int flags)
{
	struct saved *last;
	const char *res;
	int error = reserve_stack(nd, link);

	if (unlikely(error)) {
		if (!(nd->flags & LOOKUP_RCU))
			path_put(link);
		return ERR_PTR(error);
	}
	last = nd->stack + nd->depth++;
	last->link = *link;
	clear_delayed_call(&last->done);
	last->seq = nd->next_seq;

	if (flags & WALK_TRAILING) {
		error = may_follow_link(nd, inode);
		if (unlikely(error))
			return ERR_PTR(error);
	}

	if (unlikely(nd->flags & LOOKUP_NO_SYMLINKS) ||
			unlikely(link->mnt->mnt_flags & MNT_NOSYMFOLLOW))
		return ERR_PTR(-ELOOP);
```

- `may_follow_link()`（[`fs/namei.c:1168-1196`](../../linux/fs/namei.c#L1168-L1196)）只对最后一个分量（`WALK_TRAILING`）生效，实现 `fs.protected_symlinks` 保护：在全局可写且设置了粘滞位的目录（如 `/tmp`）中，只有符号链接的属主与跟随者一致、或与目录属主一致时才允许跟随，防御经典的 `/tmp` 符号链接攻击。该 sysctl 在内核中默认为 0，主流发行版通过 systemd 将其设为 1。
- `LOOKUP_NO_SYMLINKS`（`openat2()` 的 `RESOLVE_NO_SYMLINKS`）与挂载选项 `nosymfollow` 禁止跟随任何符号链接。

然后取得链接内容：

[`fs/namei.c:1945-1974`](../../linux/fs/namei.c#L1945-L1974)（v6.18.60）

```c
	res = READ_ONCE(inode->i_link);
	if (!res) {
		const char * (*get)(struct dentry *, struct inode *,
				struct delayed_call *);
		get = inode->i_op->get_link;
		if (nd->flags & LOOKUP_RCU) {
			res = get(NULL, inode, &last->done);
			if (res == ERR_PTR(-ECHILD) && try_to_unlazy(nd))
				res = get(link->dentry, inode, &last->done);
		} else {
			res = get(link->dentry, inode, &last->done);
		}
		if (!res)
			goto all_done;
		if (IS_ERR(res))
			return res;
	}
	if (*res == '/') {
		error = nd_jump_root(nd);
		if (unlikely(error))
			return ERR_PTR(error);
		while (unlikely(*++res == '/'))
			;
	}
	if (*res)
		return res;
all_done: // pure jump
	put_link(nd);
	return NULL;
}
```

几个要点：

- 链接内容优先从 `inode->i_link` 直接读取（许多文件系统在读入 inode 时就把短链接缓存在内存里），这可以在 RCU 模式下完成；否则调用 `->get_link`。RCU 模式下以 `dentry = NULL` 调用，表示"不许睡眠"，文件系统若必须读磁盘就返回 `-ECHILD`，再转为 REF-walk 重试。
- 以 `/` 开头的链接内容调用 `nd_jump_root()` 跳回根目录。
- `/proc/PID/fd/N` 这类"魔术链接"在 `->get_link` 中直接调用 `nd_jump_link()` 跳到目标位置，返回 `NULL`（"纯跳转"）。

### 处理 ".."

`..` 看似简单，却要处理三个边界：已经到达本次查找的根（不能再往上）、到达某个挂载的根（应该回到挂载点所在的父挂载中再往上）、以及父目录在此期间被并发移动。RCU 模式下的实现：

[`fs/namei.c:2053-2091`](../../linux/fs/namei.c#L2053-L2091)（v6.18.60）

```c
static struct dentry *follow_dotdot_rcu(struct nameidata *nd)
{
	struct dentry *parent, *old;

	if (path_equal(&nd->path, &nd->root))
		goto in_root;
	if (unlikely(nd->path.dentry == nd->path.mnt->mnt_root)) {
		struct path path;
		unsigned seq;
		if (!choose_mountpoint_rcu(real_mount(nd->path.mnt),
					   &nd->root, &path, &seq))
			goto in_root;
		if (unlikely(nd->flags & LOOKUP_NO_XDEV))
			return ERR_PTR(-ECHILD);
		nd->path = path;
		nd->inode = path.dentry->d_inode;
		nd->seq = seq;
		// makes sure that non-RCU pathwalk could reach this state
		if (read_seqretry(&mount_lock, nd->m_seq))
			return ERR_PTR(-ECHILD);
		/* we know that mountpoint was pinned */
	}
	old = nd->path.dentry;
	parent = old->d_parent;
	nd->next_seq = read_seqcount_begin(&parent->d_seq);
	// makes sure that non-RCU pathwalk could reach this state
	if (read_seqcount_retry(&old->d_seq, nd->seq))
		return ERR_PTR(-ECHILD);
	if (unlikely(!path_connected(nd->path.mnt, parent)))
		return ERR_PTR(-ECHILD);
	return parent;
in_root:
	if (read_seqretry(&mount_lock, nd->m_seq))
		return ERR_PTR(-ECHILD);
	if (unlikely(nd->flags & LOOKUP_BENEATH))
		return ERR_PTR(-ECHILD);
	nd->next_seq = nd->seq;
	return nd->path.dentry;
}
```

`choose_mountpoint_rcu()` 沿 `mnt_parent` 向上找到挂载点，直到不再是某个挂载的根为止。`path_connected()` 处理绑定挂载的一个微妙情况：如果绑定挂载的子目录被重命名到了绑定根之外，从里面执行 `..` 可能"逃出"绑定挂载，因此需要检查结果是否仍在挂载根之下。

对于 `openat2()` 的 `RESOLVE_BENEATH` / `RESOLVE_IN_ROOT` 这类作用域查找，`handle_dots()`（[`fs/namei.c:2125-2161`](../../linux/fs/namei.c#L2125-L2161)）还会在处理 `..` 后检查 `mount_lock` 与 `rename_lock` 的顺序号：只要查找期间发生过任何重命名或挂载变化，就返回 `-EAGAIN`，因为无法保证 `..` 没有借助并发的重命名跳出限制范围。

### 从 RCU-walk 转为 REF-walk

当 RCU-walk 遇到必须睡眠的情况时，并不一定要从头再来：只要此前读到的数据仍然有效，就可以为当前位置补上引用计数，然后以 REF-walk 继续。`try_to_unlazy()` 负责这件事：

[`fs/namei.c:768-788`](../../linux/fs/namei.c#L768-L788)（v6.18.60）

```c
static bool __legitimize_path(struct path *path, unsigned seq, unsigned mseq)
{
	int res = __legitimize_mnt(path->mnt, mseq);
	if (unlikely(res)) {
		if (res > 0)
			path->mnt = NULL;
		path->dentry = NULL;
		return false;
	}
	if (unlikely(!lockref_get_not_dead(&path->dentry->d_lockref))) {
		path->dentry = NULL;
		return false;
	}
	return !read_seqcount_retry(&path->dentry->d_seq, seq);
}

static inline bool legitimize_path(struct nameidata *nd,
			    struct path *path, unsigned seq)
{
	return __legitimize_path(path, seq, nd->m_seq);
}
```

[`fs/namei.c:840-862`](../../linux/fs/namei.c#L840-L862)（v6.18.60）

```c
static bool try_to_unlazy(struct nameidata *nd)
{
	struct dentry *parent = nd->path.dentry;

	BUG_ON(!(nd->flags & LOOKUP_RCU));

	if (unlikely(!legitimize_links(nd)))
		goto out1;
	if (unlikely(!legitimize_path(nd, &nd->path, nd->seq)))
		goto out;
	if (unlikely(!legitimize_root(nd)))
		goto out;
	leave_rcu(nd);
	BUG_ON(nd->inode != parent->d_inode);
	return true;

out1:
	nd->path.mnt = NULL;
	nd->path.dentry = NULL;
out:
	leave_rcu(nd);
	return false;
}
```

"合法化"一个路径包括三步：用 `__legitimize_mnt()` 为挂载获取引用（并检查 `mount_lock` 顺序号）；用 `lockref_get_not_dead()` 为 dentry 获取引用（dentry 若已被标记为死亡则失败）；最后检查 dentry 的 `d_seq` 是否仍是当初采样的值。三者都成功，说明当前位置在拿到引用的那一刻依然有效，于是 `leave_rcu()` 退出 RCU 读临界区。符号链接栈中的每一项、以及根目录也要同样处理。任何一步失败都返回 `false`，调用者随后返回 `-ECHILD`，由 `filename_lookup()` 从头以 REF-walk 重试。

触发转换的常见原因：

| 原因 | 位置 |
|---|---|
| dcache 未命中，需要调用 `->lookup` | `lookup_fast()` |
| `->d_revalidate` 或 `->permission` 需要睡眠 | `lookup_fast()`、`may_lookup()` |
| 符号链接内容不在内存中 | `pick_link()` |
| 需要更新符号链接的 atime | `pick_link()` |
| 自动挂载点或 `->d_manage` 需要阻塞 | `__follow_mount_rcu()` |
| 查找结束，需要返回带引用的结果 | `complete_walk()` |

### 结束查找

[`fs/namei.c:944-997`](../../linux/fs/namei.c#L944-L997)（v6.18.60）

```c
static int complete_walk(struct nameidata *nd)
{
	struct dentry *dentry = nd->path.dentry;
	int status;

	if (nd->flags & LOOKUP_RCU) {
		/*
		 * We don't want to zero nd->root for scoped-lookups or
		 * externally-managed nd->root.
		 */
		if (!(nd->state & ND_ROOT_PRESET))
			if (!(nd->flags & LOOKUP_IS_SCOPED))
				nd->root.mnt = NULL;
		nd->flags &= ~LOOKUP_CACHED;
		if (!try_to_unlazy(nd))
			return -ECHILD;
	}
	...
	if (likely(!(nd->state & ND_JUMPED)))
		return 0;

	if (likely(!(dentry->d_flags & DCACHE_OP_WEAK_REVALIDATE)))
		return 0;

	status = dentry->d_op->d_weak_revalidate(dentry, nd->flags);
	if (status > 0)
		return 0;

	if (!status)
		status = -ESTALE;

	return status;
}
```

无论哪种模式，查找成功后都要返回持有引用的 `struct path`，因此 RCU 模式在 `complete_walk()` 中最后一次调用 `try_to_unlazy()`。之后如果查找过程中发生过跳转（`ND_JUMPED`：经过了 `..`、挂载点、绝对路径的符号链接等），而文件系统提供了 `->d_weak_revalidate`，就对最终结果做一次"弱重验证"——网络文件系统借此确认最终对象仍然存在，而不必验证途经的每一级。

## 关键细节 / 设计考量

### 为什么 RCU-walk 如此重要

路径查找每一级都要访问 dentry。在 REF-walk 中，每进入一级就要对该 dentry 的 `d_lockref` 做一次原子加、离开时再做一次原子减。对于 `/usr/lib/x86_64-linux-gnu/...` 这样的路径，上层目录的 dentry 被系统中所有进程的查找共享，它们的引用计数所在缓存行会在 CPU 之间来回颠簸，成为多核扩展的严重瓶颈。RCU-walk 只**读**这些 dentry、从不写它们，同一缓存行可以同时存在于所有 CPU 的缓存中。代价是复杂的顺序号校验与回退逻辑，以及要求 dentry、inode、挂载都必须经 RCU 延迟释放（`dentry_free()` 中的 `call_rcu()`，9.4 节）。

### 并发重命名与 rename_lock

RCU-walk 依靠 `d_seq` 检测单个 dentry 的变化，但无法检测"目标被移走"这种跨目录的变化：如果 `/a/b` 在查找到达 `/a` 之后被重命名为 `/c/b`，`__d_lookup_rcu()` 在 `/a` 的哈希链上找不到 `b`，得到的是"不存在"。这种假阴性在 RCU-walk 中会落入 `lookup_slow()`，后者持有父目录的锁后再查一次即可得到正确结果。dcache 中需要严格正确的 `d_lookup()`（[`fs/dcache.c:2334-2346`](../../linux/fs/dcache.c#L2334-L2346)）则用全局的 `rename_lock` 顺序锁包裹查找，检测到期间发生过重命名就重试。

### 负目录项

查找一个不存在的名字时，`->lookup` 返回后 dentry 仍会以"负"的状态（`d_inode == NULL`）留在 dcache 中。下次再查同一个名字就能在 dcache 中直接得到"不存在"的结论，不必再访问文件系统。这对 `PATH` 环境变量搜索可执行文件、动态链接器在多个目录中搜索共享库等场景至关重要——它们产生大量注定失败的查找。

### 尾随斜杠与最后一个分量

最后一个分量不在 `link_path_walk()` 中处理，是因为不同的调用者对它的要求不同：

| 调用者 | 最后一个分量的处理 |
|---|---|
| `stat()`、`chdir()` 等 | `lookup_last()`，按 `LOOKUP_FOLLOW` 决定是否跟随符号链接 |
| `open()` | `open_last_lookups()`，可能需要创建文件（9.6 节） |
| `mkdir()`、`unlink()`、`rename()` 等 | `filename_parentat()` 只解析到父目录，由调用者加锁后再查找（9.12 节） |

## 动手实验

### 观察 dcache 命中与未命中

`__d_lookup_rcu()` 与 `d_alloc_parallel()` 都是非静态函数，可以分别作为"RCU 快速查找"和"慢速查找"的观察点：

```bash
sudo bpftrace -e '
kprobe:__d_lookup_rcu { @rcu_lookup = count(); }
kretprobe:__d_lookup_rcu /retval == 0/ { @rcu_miss = count(); }
kprobe:d_alloc_parallel { @slow_lookup[comm] = count(); }
interval:s:5 { exit(); }'
```

在另一个终端反复执行 `ls -R /usr > /dev/null`：第一次运行 `@slow_lookup` 较多（dcache 冷），之后几乎全部命中。可以用 `echo 2 > /proc/sys/vm/drop_caches` 清空 dentry 与 inode 缓存后再观察。

### 负目录项

```bash
cat /proc/sys/fs/dentry-state                  # 第 5 列为未使用的负 dentry 数
for i in $(seq 10000); do stat /tmp/nonexist-$i 2>/dev/null; done
cat /proc/sys/fs/dentry-state
```

### 符号链接上限

```bash
cd /tmp && ln -sf l0 l40
for i in $(seq 0 39); do ln -sf l$((i+1)) l$i; done 2>/dev/null
cat l0            # Too many levels of symbolic links (ELOOP)
```

## 小结

- 路径查找由 `nameidata` 记录状态，`path_init()` 确定起点，`link_path_walk()` 迭代解析中间分量，最后一个分量留给调用者按需处理；符号链接通过显式栈转化为迭代，总数不超过 40 个。
- 查找优先以 RCU-walk 进行：不取引用、不加锁，依靠 `d_seq` 的"先采样子、再验证父"链式校验与 `mount_lock`、`rename_lock` 的顺序号保证一致性。
- 遇到需要睡眠的情况时，`try_to_unlazy()` 尝试就地为当前位置补上引用转为 REF-walk；失败则返回 `-ECHILD`，`filename_lookup()` 以 REF-walk 重试，`-ESTALE` 时再以 `LOOKUP_REVAL` 重试。
- dcache 未命中时，`lookup_slow()` 在父目录共享锁下调用 `->lookup`，`d_alloc_parallel()` 防止同名的重复查找。
- `step_into()` 通过 `__lookup_mnt()` 穿越挂载点，`..` 的处理要考虑查找根、挂载根与绑定挂载的边界。
- 每一级目录都需要搜索权限，`hash_name()` 以字为单位同时完成分隔符查找与哈希计算，哈希以父 dentry 地址为种子。

## 参考

- `linux/Documentation/filesystems/path-lookup.rst`：Neil Brown 对路径查找的系统讲解（含 RCU-walk 细节）
- `linux/Documentation/filesystems/path-lookup.txt`：RCU-walk 的原始设计说明
- Nick Piggin, "fs: rcu-walk for path lookup"（v2.6.38）
- Aleksa Sarai, "openat2(2)" 与 `RESOLVE_*` 标志（v5.6）
- `man 2 openat2`、`man 7 path_resolution`
