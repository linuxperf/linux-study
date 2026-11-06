# 9.4 目录项缓存

> **本节源码**：[`fs/dcache.c`](../../linux/fs/dcache.c)、[`include/linux/dcache.h`](../../linux/include/linux/dcache.h)、[`include/linux/lockref.h`](../../linux/include/linux/lockref.h)、[`fs/super.c`](../../linux/fs/super.c)
> **关键函数**：`__d_alloc()`、`d_alloc()`、`d_alloc_parallel()`、`__d_add()`、`d_instantiate()`、`d_splice_alias()`、`__d_lookup_rcu()`、`__d_lookup()`、`d_lookup()`、`dput()`、`fast_dput()`、`retain_dentry()`、`__dentry_kill()`、`d_delete()`、`__d_drop()`、`super_cache_scan()`、`prune_dcache_sb()`、`dentry_lru_isolate()`、`shrink_dentry_list()`
> **前置知识**：9.3 路径名查找、7.2 原子操作、位操作与引用计数（lockref）、7.9 RCU 原理与使用、4.10 页面回收（shrinker）

## 概述

**目录项缓存（Dentry Cache，dcache）** 把路径分量与 inode 的对应关系缓存在内存中，使路径查找在绝大多数情况下无需访问文件系统。它同时承担三种角色：

1. **哈希表**：以（父 dentry，名字哈希）为键快速查找子 dentry，服务于 9.3 节的 `lookup_fast()`。
2. **目录树**：每个 dentry 记录父节点与子节点链表，在内存中维护一棵（可能不完整的）目录树，用于 `..`、`getcwd()`、`d_path()` 以及卸载时的整树回收。
3. **对象缓存**：未被使用的 dentry 不立即释放，而是挂在超级块的 LRU 上，内存紧张时由 shrinker 回收；同时，dentry 持有对 inode 的引用，dcache 也决定了 inode 缓存（9.5 节）的去留。

dcache 还缓存"不存在"：**负目录项（Negative Dentry）** 的 `d_inode` 为 `NULL`，表示该名字在父目录中不存在。

本节分析 dentry 的分配、与 inode 的关联、查找、引用计数与释放，以及 LRU 回收。

## 核心数据结构

`struct dentry` 的字段已在 9.1 节介绍，这里补充与 dcache 实现相关的几个部分。

### 全局哈希表

[`fs/dcache.c:112-130`](../../linux/fs/dcache.c#L112-L130)（v6.18.60）

```c
static unsigned int d_hash_shift __ro_after_init __used;

static struct hlist_bl_head *dentry_hashtable __ro_after_init __used;

static inline struct hlist_bl_head *d_hash(unsigned long hashlen)
{
	return runtime_const_ptr(dentry_hashtable) +
		runtime_const_shift_right_32(hashlen, d_hash_shift);
}

#define IN_LOOKUP_SHIFT 10
static struct hlist_bl_head in_lookup_hashtable[1 << IN_LOOKUP_SHIFT];

static inline struct hlist_bl_head *in_lookup_hash(const struct dentry *parent,
					unsigned int hash)
{
	hash += (unsigned long) parent / L1_CACHE_BYTES;
	return in_lookup_hashtable + hash_32(hash, IN_LOOKUP_SHIFT);
}
```

- `dentry_hashtable` 是由 `hlist_bl_head` 组成的数组。`hlist_bl`（bit-locked hlist）把链表头指针的最低位用作自旋锁，每个桶只占 8 字节，又有了独立的锁——哈希表通常有数百万个桶，省下的内存相当可观。
- `d_hash()` 取哈希值的高位作为桶下标。`dentry_hashtable` 与 `d_hash_shift` 在启动后不再改变，v6.11 引入的 runtime constant 机制在启动时把它们的值直接修补进指令的立即数中，省去一次内存加载。
- `in_lookup_hashtable` 是一张固定 1024 个桶的小表，只存放"正在被查找"的 dentry（见后文 `d_alloc_parallel()`）。

### d_flags

`d_flags` 中有几组重要的位（[`include/linux/dcache.h:180-228`](../../linux/include/linux/dcache.h#L180-L228)）：

| 位 | 含义 |
|---|---|
| `DCACHE_OP_HASH` / `_COMPARE` / `_REVALIDATE` / `_DELETE` / `_PRUNE` | 文件系统提供了对应的 `d_op` 方法。在 `d_set_d_op()` 时根据方法表一次性设置，热路径上只需测试标志位，不必解引用 `d_op` |
| `DCACHE_ENTRY_TYPE`（3 位） | 缓存的对象类型：`MISS`（负）、`DIRECTORY`、`AUTODIR`、`REGULAR`、`SPECIAL`、`SYMLINK`、`WHITEOUT`。路径查找据此判断"是否目录""是否符号链接"，不必访问 inode |
| `DCACHE_MOUNTED` / `NEED_AUTOMOUNT` / `MANAGE_TRANSIT` | 挂载点等"受管"dentry（9.3 节） |
| `DCACHE_LRU_LIST` / `SHRINK_LIST` | 在超级块 LRU 上 / 在某个私有回收链表上 |
| `DCACHE_REFERENCED` | 最近被使用过，LRU 扫描时给它"第二次机会" |
| `DCACHE_DONTCACHE` | 最后一个引用释放时直接销毁，不进 LRU |
| `DCACHE_PAR_LOOKUP` | 正在被 `->lookup` 查找中 |
| `DCACHE_DISCONNECTED` | 尚未连接到根的 dentry（NFS 文件句柄解码时产生） |
| `DCACHE_DENTRY_KILLED` | 已被销毁，正等待 RCU 释放 |

类型位由 `d_flags_for_inode()` 在 dentry 与 inode 关联时计算：

[`fs/dcache.c:1888-1921`](../../linux/fs/dcache.c#L1888-L1921)（v6.18.60）

```c
static unsigned d_flags_for_inode(struct inode *inode)
{
	unsigned add_flags = DCACHE_REGULAR_TYPE;

	if (!inode)
		return DCACHE_MISS_TYPE;

	if (S_ISDIR(inode->i_mode)) {
		add_flags = DCACHE_DIRECTORY_TYPE;
		if (unlikely(!(inode->i_opflags & IOP_LOOKUP))) {
			if (unlikely(!inode->i_op->lookup))
				add_flags = DCACHE_AUTODIR_TYPE;
			else
				inode->i_opflags |= IOP_LOOKUP;
		}
		goto type_determined;
	}

	if (unlikely(!(inode->i_opflags & IOP_NOFOLLOW))) {
		if (unlikely(inode->i_op->get_link)) {
			add_flags = DCACHE_SYMLINK_TYPE;
			goto type_determined;
		}
		inode->i_opflags |= IOP_NOFOLLOW;
	}

	if (unlikely(!S_ISREG(inode->i_mode)))
		add_flags = DCACHE_SPECIAL_TYPE;

type_determined:
	if (unlikely(IS_AUTOMOUNT(inode)))
		add_flags |= DCACHE_NEED_AUTOMOUNT;
	return add_flags;
}
```

注意它顺便缓存了两个 inode 级的判断：`IOP_LOOKUP`（目录有 `->lookup` 方法）与 `IOP_NOFOLLOW`（不是符号链接），避免每次都检查 `i_op` 中的函数指针。

### dentry 的状态

| 状态 | `d_inode` | 在哈希表中 | 引用计数 | 说明 |
|---|---|---|---|---|
| 正在使用 | 非空 | 是 | > 0 | 被打开的文件、当前目录、查找途中等持有 |
| 未使用 | 非空 | 是 | 0 | 在 LRU 上，随时可被重新使用或回收 |
| 负 | `NULL` | 是 | ≥ 0 | 缓存"不存在"；未使用时同样在 LRU 上 |
| 查找中 | `NULL` | 在 `in_lookup_hashtable` | > 0 | `->lookup` 尚未完成 |
| 已脱离哈希 | 任意 | 否 | > 0 | `d_drop()` 之后，不能再被查找到，最后一个引用释放时销毁 |
| 已死亡 | — | 否 | `lockref` 标记为死 | `__dentry_kill()` 之后，等待 RCU 宽限期释放 |

### 统计计数

[`fs/dcache.c:141-150`](../../linux/fs/dcache.c#L141-L150)（v6.18.60）

```c
static DEFINE_PER_CPU(long, nr_dentry);
static DEFINE_PER_CPU(long, nr_dentry_unused);
static DEFINE_PER_CPU(long, nr_dentry_negative);
static int dentry_negative_policy;

#if defined(CONFIG_SYSCTL) && defined(CONFIG_PROC_FS)
/* Statistics gathering. */
static struct dentry_stat_t dentry_stat = {
	.age_limit = 45,
};
```

dentry 的数量、未使用数量、负 dentry 数量用 per-CPU 计数维护，只在读取 `/proc/sys/fs/dentry-state` 时求和，避免热路径上的共享计数器争用。

## 主流程分析

### 分配：__d_alloc() 与 d_alloc()

[`fs/dcache.c:1686-1728`](../../linux/fs/dcache.c#L1686-L1728)（v6.18.60）

```c
static struct dentry *__d_alloc(struct super_block *sb, const struct qstr *name)
{
	struct dentry *dentry;
	char *dname;
	int err;

	dentry = kmem_cache_alloc_lru(dentry_cache, &sb->s_dentry_lru,
				      GFP_KERNEL);
	if (!dentry)
		return NULL;
	...
	dentry->d_shortname.string[DNAME_INLINE_LEN-1] = 0;
	if (unlikely(!name)) {
		name = &slash_name;
		dname = dentry->d_shortname.string;
	} else if (name->len > DNAME_INLINE_LEN-1) {
		size_t size = offsetof(struct external_name, name[1]);
		struct external_name *p = kmalloc(size + name->len,
						  GFP_KERNEL_ACCOUNT |
						  __GFP_RECLAIMABLE);
		if (!p) {
			kmem_cache_free(dentry_cache, dentry); 
			return NULL;
		}
		atomic_set(&p->count, 1);
		dname = p->name;
	} else  {
		dname = dentry->d_shortname.string;
	}	

	dentry->__d_name.len = name->len;
	dentry->__d_name.hash = name->hash;
	memcpy(dname, name->name, name->len);
	dname[name->len] = 0;

	/* Make sure we always see the terminating NUL character */
	smp_store_release(&dentry->__d_name.name, dname); /* ^^^ */
```

名字不超过 39 字节（`DNAME_INLINE_LEN - 1`）时直接存放在 dentry 内嵌的 `d_shortname` 中，否则另外分配一个带引用计数的 `struct external_name`。`smp_store_release()` 发布名字指针，保证 RCU 查找者读到新指针时一定也能看到完整的、以 `'\0'` 结尾的名字。分配时传入 `&sb->s_dentry_lru` 是为了让 memcg 感知的 `list_lru` 提前为该 dentry 所属的内存控制组准备好 LRU 结构。

[`fs/dcache.c:1730-1743`](../../linux/fs/dcache.c#L1730-L1743)（v6.18.60）

```c
	dentry->d_flags = 0;
	lockref_init(&dentry->d_lockref);
	seqcount_spinlock_init(&dentry->d_seq, &dentry->d_lock);
	dentry->d_inode = NULL;
	dentry->d_parent = dentry;
	dentry->d_sb = sb;
	dentry->d_op = sb->__s_d_op;
	dentry->d_flags = sb->s_d_flags;
	dentry->d_fsdata = NULL;
	INIT_HLIST_BL_NODE(&dentry->d_hash);
	INIT_LIST_HEAD(&dentry->d_lru);
	INIT_HLIST_HEAD(&dentry->d_children);
	INIT_HLIST_NODE(&dentry->d_u.d_alias);
	INIT_HLIST_NODE(&dentry->d_sib);
```

新 dentry 的父指针指向自己（即 `IS_ROOT`），引用计数为 1，`d_op` 与默认标志来自超级块（`sb->__s_d_op`、`sb->s_d_flags`）。`d_alloc()` 再把它挂到父 dentry 下：

[`fs/dcache.c:1769-1785`](../../linux/fs/dcache.c#L1769-L1785)（v6.18.60）

```c
struct dentry *d_alloc(struct dentry * parent, const struct qstr *name)
{
	struct dentry *dentry = __d_alloc(parent->d_sb, name);
	if (!dentry)
		return NULL;
	spin_lock(&parent->d_lock);
	/*
	 * don't need child lock because it is not subject
	 * to concurrency here
	 */
	dentry->d_parent = dget_dlock(parent);
	hlist_add_head(&dentry->d_sib, &parent->d_children);
	spin_unlock(&parent->d_lock);

	return dentry;
}
EXPORT_SYMBOL(d_alloc);
```

子 dentry 持有对父 dentry 的一个引用（`dget_dlock(parent)`），这保证了**只要子还在，父就不会被释放**，从而 `d_parent` 链一路到根都是有效的。

### 关联 inode：d_add()、d_instantiate() 与 d_splice_alias()

刚分配的 dentry 是负的且不在哈希表中。文件系统在两种场合把它与 inode 关联：

- `->lookup` 找到了名字：调用 `d_splice_alias(inode, dentry)` 或 `d_add(dentry, inode)`，关联 inode **并**加入哈希表；找不到时以 `inode = NULL` 调用，dentry 作为负目录项加入哈希表。
- `->create`、`->mkdir` 等创建了新对象：调用 `d_instantiate(dentry, inode)`。此时 dentry 已经（作为负目录项）在哈希表中，只需填入 inode。

`d_add()` 的核心 `__d_add()`：

[`fs/dcache.c:2694-2722`](../../linux/fs/dcache.c#L2694-L2722)（v6.18.60）

```c
static inline void __d_add(struct dentry *dentry, struct inode *inode,
			   const struct dentry_operations *ops)
{
	wait_queue_head_t *d_wait;
	struct inode *dir = NULL;
	unsigned n;
	spin_lock(&dentry->d_lock);
	if (unlikely(d_in_lookup(dentry))) {
		dir = dentry->d_parent->d_inode;
		n = start_dir_add(dir);
		d_wait = __d_lookup_unhash(dentry);
	}
	if (unlikely(ops))
		d_set_d_op(dentry, ops);
	if (inode) {
		unsigned add_flags = d_flags_for_inode(inode);
		hlist_add_head(&dentry->d_u.d_alias, &inode->i_dentry);
		raw_write_seqcount_begin(&dentry->d_seq);
		__d_set_inode_and_type(dentry, inode, add_flags);
		raw_write_seqcount_end(&dentry->d_seq);
		fsnotify_update_flags(dentry);
	}
	__d_rehash(dentry);
	if (dir)
		end_dir_add(dir, n, d_wait);
	spin_unlock(&dentry->d_lock);
	if (inode)
		spin_unlock(&inode->i_lock);
}
```

关联 inode 的步骤是：把 dentry 加入 inode 的别名链表 `i_dentry`，在 `d_seq` 的写临界区内设置 `d_inode` 与类型标志，然后 `__d_rehash()` 插入哈希表。`d_seq` 写临界区使得并发的 RCU 查找者要么看到修改前的状态，要么在验证序号时发现变化并重试。如果 dentry 处于"查找中"状态，还要把它从 `in_lookup_hashtable` 中移出并唤醒等待者（`end_dir_add()`）。

`d_splice_alias()`（[`fs/dcache.c:2992-3041`](../../linux/fs/dcache.c#L2992-L3041)）在 `d_add()` 的基础上处理一个特殊情况：**目录最多只能有一个 dentry**（否则目录树会出现环或歧义）。如果 inode 是目录并且已经有一个别名（例如 NFS 服务器端通过文件句柄解码得到的"游离"dentry），就把那个已有的 dentry 移动到当前位置并返回它，而不是再创建一个新别名。普通文件没有这个限制，多个硬链接对应多个 dentry。

### 查找：__d_lookup_rcu()

RCU 路径查找调用的 `__d_lookup_rcu()` 是 dcache 中最热的函数：

[`fs/dcache.c:2288-2320`](../../linux/fs/dcache.c#L2288-L2320)（v6.18.60）

```c
	hlist_bl_for_each_entry_rcu(dentry, node, b, d_hash) {
		unsigned seq;

		/*
		 * The dentry sequence count protects us from concurrent
		 * renames, and thus protects parent and name fields.
		 *
		 * The caller must perform a seqcount check in order
		 * to do anything useful with the returned dentry.
		 *
		 * NOTE! We do a "raw" seqcount_begin here. That means that
		 * we don't wait for the sequence count to stabilize if it
		 * is in the middle of a sequence change. If we do the slow
		 * dentry compare, we will do seqretries until it is stable,
		 * and if we end up with a successful lookup, we actually
		 * want to exit RCU lookup anyway.
		 *
		 * Note that raw_seqcount_begin still *does* smp_rmb(), so
		 * we are still guaranteed NUL-termination of ->d_name.name.
		 */
		seq = raw_seqcount_begin(&dentry->d_seq);
		if (dentry->d_parent != parent)
			continue;
		if (d_unhashed(dentry))
			continue;
		if (dentry->d_name.hash_len != hashlen)
			continue;
		if (dentry_cmp(dentry, str, hashlen_len(hashlen)) != 0)
			continue;
		*seqp = seq;
		return dentry;
	}
	return NULL;
```

遍历哈希链时对每个候选 dentry：

1. 用 `raw_seqcount_begin()` 采样 `d_seq`。它不等待正在进行的写者（序号为奇数时也直接返回），因为调用者最终会校验序号，而在不一致时本来就要退出 RCU 模式。
2. 比较父指针，排除不同目录下的同名项。
3. 跳过已从哈希表摘除的 dentry。
4. 一次 64 位比较同时比较哈希与长度。
5. 最后比较名字内容。`dentry_cmp()` 同样以字为单位比较（[`fs/dcache.c:264-283`](../../linux/fs/dcache.c#L264-L283)）。

找到后返回 dentry 与采样的序号，**不获取任何引用**。调用者（`lookup_fast()`）必须在使用前校验序号，这就是 9.3 节所说的链式校验。

父目录设置了 `DCACHE_OP_COMPARE`（如大小写不敏感的文件系统）时，转到 `__d_lookup_rcu_op_compare()`（[`fs/dcache.c:2191-2227`](../../linux/fs/dcache.c#L2191-L2227)）：调用文件系统的 `->d_compare` 前必须拿到一致的（名字，长度）对，因此要在序号稳定后再比较。

### 查找：__d_lookup() 与 d_lookup()

需要获得引用的查找使用 `__d_lookup()`，它对每个哈希匹配的候选者持 `d_lock` 做比较：

[`fs/dcache.c:2392-2418`](../../linux/fs/dcache.c#L2392-L2418)（v6.18.60）

```c
	rcu_read_lock();
	
	hlist_bl_for_each_entry_rcu(dentry, node, b, d_hash) {

		if (dentry->d_name.hash != hash)
			continue;

		spin_lock(&dentry->d_lock);
		if (dentry->d_parent != parent)
			goto next;
		if (d_unhashed(dentry))
			goto next;

		if (!d_same_name(dentry, parent, name))
			goto next;

		dentry->d_lockref.count++;
		found = dentry;
		spin_unlock(&dentry->d_lock);
		break;
next:
		spin_unlock(&dentry->d_lock);
 	}
 	rcu_read_unlock();

 	return found;
}
```

由于并发的跨目录重命名会把 dentry 从一个哈希链移到另一个，遍历中可能"错过"目标，产生假阴性。`d_lookup()` 用全局顺序锁 `rename_lock` 包裹它：查找失败且期间发生过重命名就重试：

[`fs/dcache.c:2334-2346`](../../linux/fs/dcache.c#L2334-L2346)（v6.18.60）

```c
struct dentry *d_lookup(const struct dentry *parent, const struct qstr *name)
{
	struct dentry *dentry;
	unsigned seq;

	do {
		seq = read_seqbegin(&rename_lock);
		dentry = __d_lookup(parent, name);
		if (dentry)
			break;
	} while (read_seqretry(&rename_lock, seq));
	return dentry;
}
```

### 并行查找：d_alloc_parallel()

dcache 未命中时，`lookup_slow()` 只对父目录持共享锁，多个任务可能同时查找同一个不存在于 dcache 中的名字。若每个任务都调用 `->lookup` 并各自创建 dentry，不仅浪费 I/O，还会产生重复的别名。`d_alloc_parallel()` 解决这个问题：

[`fs/dcache.c:2544-2570`](../../linux/fs/dcache.c#L2544-L2570)（v6.18.60）

```c
struct dentry *d_alloc_parallel(struct dentry *parent,
				const struct qstr *name,
				wait_queue_head_t *wq)
{
	unsigned int hash = name->hash;
	struct hlist_bl_head *b = in_lookup_hash(parent, hash);
	struct hlist_bl_node *node;
	struct dentry *new = __d_alloc(parent->d_sb, name);
	struct dentry *dentry;
	unsigned seq, r_seq, d_seq;

	if (unlikely(!new))
		return ERR_PTR(-ENOMEM);

	new->d_flags |= DCACHE_PAR_LOOKUP;
	spin_lock(&parent->d_lock);
	new->d_parent = dget_dlock(parent);
	hlist_add_head(&new->d_sib, &parent->d_children);
	if (parent->d_flags & DCACHE_DISCONNECTED)
		new->d_flags |= DCACHE_DISCONNECTED;
	spin_unlock(&parent->d_lock);

retry:
	rcu_read_lock();
	seq = smp_load_acquire(&parent->d_inode->i_dir_seq);
	r_seq = read_seqbegin(&rename_lock);
	dentry = __d_lookup_rcu(parent, name, &d_seq);
```

它先分配一个带 `DCACHE_PAR_LOOKUP` 标志的新 dentry，然后在 RCU 下再查一次正式的哈希表（也许别人刚刚完成了查找），找到就直接返回已有的 dentry。没找到时，进入 `in_lookup_hashtable` 的检查：

[`fs/dcache.c:2595-2652`](../../linux/fs/dcache.c#L2595-L2652)（v6.18.60）

```c
	hlist_bl_lock(b);
	if (unlikely(READ_ONCE(parent->d_inode->i_dir_seq) != seq)) {
		hlist_bl_unlock(b);
		rcu_read_unlock();
		goto retry;
	}
	/*
	 * No changes for the parent since the beginning of d_lookup().
	 * Since all removals from the chain happen with hlist_bl_lock(),
	 * any potential in-lookup matches are going to stay here until
	 * we unlock the chain.  All fields are stable in everything
	 * we encounter.
	 */
	hlist_bl_for_each_entry(dentry, node, b, d_u.d_in_lookup_hash) {
		if (dentry->d_name.hash != hash)
			continue;
		if (dentry->d_parent != parent)
			continue;
		if (!d_same_name(dentry, parent, name))
			continue;
		hlist_bl_unlock(b);
		/* now we can try to grab a reference */
		if (!lockref_get_not_dead(&dentry->d_lockref)) {
			rcu_read_unlock();
			goto retry;
		}

		rcu_read_unlock();
		/*
		 * somebody is likely to be still doing lookup for it;
		 * wait for them to finish
		 */
		spin_lock(&dentry->d_lock);
		d_wait_lookup(dentry);
		/*
	...
	rcu_read_unlock();
	new->d_wait = wq;
	hlist_bl_add_head(&new->d_u.d_in_lookup_hash, b);
	hlist_bl_unlock(b);
	return new;
```

`i_dir_seq` 是父目录 inode 上的一个顺序计数：每当有 dentry 从"查找中"转为正式（`__d_add()` 中的 `start_dir_add()`/`end_dir_add()`，[`fs/dcache.c:2510-2528`](../../linux/fs/dcache.c#L2510-L2528)），它就递增 2。持有 `in_lookup_hashtable` 桶锁后重新检查它，确认从开始查找到现在，这个目录下没有任何查找完成——否则前面对正式哈希表的查找结果可能已经过时，需要重试。

之后在查找中哈希表里寻找同名项：

- 找到了：说明另一个任务正在执行 `->lookup`。当前任务在 `d_wait_lookup()`（[`fs/dcache.c:2530-2542`](../../linux/fs/dcache.c#L2530-L2542)）中睡眠等待，被唤醒后确认那个 dentry 仍然匹配，就直接使用它的结果。
- 没找到：把新 dentry 加入查找中哈希表并返回，由当前任务负责调用 `->lookup`。完成后 `d_lookup_done()` 把它移出并唤醒所有等待者。

### 引用计数：dget() 与 dput()

dentry 的引用计数存放在 `d_lockref` 中，与 `d_lock` 共享一个 64 位字。x86_64 开启了 `CONFIG_ARCH_USE_CMPXCHG_LOCKREF`，`lockref_get()` 在锁空闲时用一次 cmpxchg 同时完成"确认未加锁 + 计数加一"，不必真正获取自旋锁（7.2 节）：

[`include/linux/dcache.h:347-352`](../../linux/include/linux/dcache.h#L347-L352)（v6.18.60）

```c
static inline struct dentry *dget(struct dentry *dentry)
{
	if (dentry)
		lockref_get(&dentry->d_lockref);
	return dentry;
}
```

`dput()` 释放引用。最后一个引用释放时要决定：把 dentry 留在缓存中，还是立即销毁？

[`fs/dcache.c:899-923`](../../linux/fs/dcache.c#L899-L923)（v6.18.60）

```c
void dput(struct dentry *dentry)
{
	if (!dentry)
		return;
	might_sleep();
	rcu_read_lock();
	if (likely(fast_dput(dentry))) {
		rcu_read_unlock();
		return;
	}
	while (lock_for_kill(dentry)) {
		rcu_read_unlock();
		dentry = __dentry_kill(dentry);
		if (!dentry)
			return;
		if (retain_dentry(dentry, true)) {
			spin_unlock(&dentry->d_lock);
			return;
		}
		rcu_read_lock();
	}
	rcu_read_unlock();
	spin_unlock(&dentry->d_lock);
}
EXPORT_SYMBOL(dput);
```

`fast_dput()`（[`fs/dcache.c:812-870`](../../linux/fs/dcache.c#L812-L870)）先用 `lockref_put_return()` 无锁地减少计数；如果不是最后一个引用，直接返回。若计数降为 0，就在不加锁的情况下调用 `retain_dentry(dentry, false)` 判断是否保留：多数情况下 dentry 已经在 LRU 上并带有 `DCACHE_REFERENCED`，什么都不用做，于是连锁都不必拿。否则获取 `d_lock` 后再判断一次。保留与否的策略集中在 `retain_dentry()`：

[`fs/dcache.c:744-785`](../../linux/fs/dcache.c#L744-L785)（v6.18.60）

```c
static inline bool retain_dentry(struct dentry *dentry, bool locked)
{
	unsigned int d_flags;

	smp_rmb();
	d_flags = READ_ONCE(dentry->d_flags);

	// Unreachable? Nobody would be able to look it up, no point retaining
	if (unlikely(d_unhashed(dentry)))
		return false;

	// Same if it's disconnected
	if (unlikely(d_flags & DCACHE_DISCONNECTED))
		return false;

	// ->d_delete() might tell us not to bother, but that requires
	// ->d_lock; can't decide without it
	if (unlikely(d_flags & DCACHE_OP_DELETE)) {
		if (!locked || dentry->d_op->d_delete(dentry))
			return false;
	}

	// Explicitly told not to bother
	if (unlikely(d_flags & DCACHE_DONTCACHE))
		return false;

	// At this point it looks like we ought to keep it.  We also might
	// need to do something - put it on LRU if it wasn't there already
	// and mark it referenced if it was on LRU, but not marked yet.
	// Unfortunately, both actions require ->d_lock, so in lockless
	// case we'd have to punt rather than doing those.
	if (unlikely(!(d_flags & DCACHE_LRU_LIST))) {
		if (!locked)
			return false;
		d_lru_add(dentry);
	} else if (unlikely(!(d_flags & DCACHE_REFERENCED))) {
		if (!locked)
			return false;
		dentry->d_flags |= DCACHE_REFERENCED;
	}
	return true;
}
```

| 不保留的情况 | 原因 |
|---|---|
| 已从哈希表摘除（`d_unhashed`） | 已无法通过查找到达，保留无意义 |
| `DCACHE_DISCONNECTED` | 游离 dentry 不缓存 |
| `->d_delete` 返回真 | 文件系统要求不缓存（如某些伪文件系统） |
| `DCACHE_DONTCACHE` | 显式要求不缓存（如 `d_mark_dontcache()`、ramfs 的负 dentry） |

保留时，若 dentry 不在 LRU 上就加入（`d_lru_add()`），若已在 LRU 上则设置 `DCACHE_REFERENCED`。注意 dentry 并不是在引用计数降为 0 时才进入 LRU 的——"惰性 LRU"策略让正在使用的 dentry 也可以留在 LRU 上，只在回收扫描时再把仍有引用的项移出（见下文 `dentry_lru_isolate()`），从而避免每次引用计数在 0 与 1 之间变化都去操作 LRU 链表的锁。

### 销毁：__dentry_kill()

不保留的 dentry 由 `__dentry_kill()` 销毁：

[`fs/dcache.c:645-694`](../../linux/fs/dcache.c#L645-L694)（v6.18.60）

```c
static struct dentry *__dentry_kill(struct dentry *dentry)
{
	struct dentry *parent = NULL;
	...
	lockref_mark_dead(&dentry->d_lockref);

	/*
	 * inform the fs via d_prune that this dentry is about to be
	 * unhashed and destroyed.
	 */
	if (dentry->d_flags & DCACHE_OP_PRUNE)
		dentry->d_op->d_prune(dentry);

	if (dentry->d_flags & DCACHE_LRU_LIST) {
		if (!(dentry->d_flags & DCACHE_SHRINK_LIST))
			d_lru_del(dentry);
	}
	/* if it was on the hash then remove it */
	__d_drop(dentry);
	if (dentry->d_inode)
		dentry_unlink_inode(dentry);
	else
		spin_unlock(&dentry->d_lock);
	this_cpu_dec(nr_dentry);
	if (dentry->d_op && dentry->d_op->d_release)
		dentry->d_op->d_release(dentry);

	cond_resched();
	/* now that it's negative, ->d_parent is stable */
	if (!IS_ROOT(dentry)) {
		parent = dentry->d_parent;
		spin_lock(&parent->d_lock);
	}
	spin_lock_nested(&dentry->d_lock, DENTRY_D_LOCK_NESTED);
	dentry_unlist(dentry);
	if (dentry->d_flags & DCACHE_SHRINK_LIST)
		can_free = false;
	spin_unlock(&dentry->d_lock);
	if (likely(can_free))
		dentry_free(dentry);
	if (parent && --parent->d_lockref.count) {
		spin_unlock(&parent->d_lock);
		return NULL;
	}
	return parent;
}
```

1. `lockref_mark_dead()` 把计数设为一个负值，此后任何 `lockref_get_not_dead()` 都会失败——RCU 查找者即使还看得到这个 dentry，也无法再获取它。
2. 通知文件系统（`->d_prune`）、从 LRU 移除、`__d_drop()` 从哈希表摘除。
3. `dentry_unlink_inode()`（[`fs/dcache.c:449-467`](../../linux/fs/dcache.c#L449-L467)）断开与 inode 的关联：从别名链表移除、清空 `d_inode`，然后调用 `->d_iput` 或 `iput()` 释放对 inode 的引用。如果这是 inode 的最后一个引用，inode 可能在此被驱逐（9.5 节）。
4. 从父目录的子链表中摘除，最后由 `dentry_free()` 经 `call_rcu()` 延迟释放内存——RCU 查找者可能仍在访问它。
5. 释放对父 dentry 的引用。若父的计数也因此降为 0，返回父 dentry，由 `dput()` 的循环继续处理。这种手工展开的"尾递归"避免了在很深的目录树上真正递归。

`lock_for_kill()`（[`fs/dcache.c:707-731`](../../linux/fs/dcache.c#L707-L731)）在销毁前获取 inode 的 `i_lock`。锁顺序要求先 `i_lock` 后 `d_lock`，而此时已持有 `d_lock`，因此先尝试 `spin_trylock()`，失败时释放 `d_lock` 再按正确顺序重新加锁，并重新检查 `d_inode` 与引用计数是否有变。

### 删除文件：d_delete()

`unlink()` 删除文件后，VFS 调用 `d_delete()` 处理对应的 dentry：

[`fs/dcache.c:2464-2483`](../../linux/fs/dcache.c#L2464-L2483)（v6.18.60）

```c
void d_delete(struct dentry * dentry)
{
	struct inode *inode = dentry->d_inode;

	spin_lock(&inode->i_lock);
	spin_lock(&dentry->d_lock);
	/*
	 * Are we the only user?
	 */
	if (dentry->d_lockref.count == 1) {
		if (dentry_negative_policy)
			__d_drop(dentry);
		dentry->d_flags &= ~DCACHE_CANT_MOUNT;
		dentry_unlink_inode(dentry);
	} else {
		__d_drop(dentry);
		spin_unlock(&dentry->d_lock);
		spin_unlock(&inode->i_lock);
	}
}
```

如果调用者是唯一的使用者，就把 dentry 就地转为**负目录项**——下次查找同名文件立即得到"不存在"；否则（文件仍被他人打开或作为当前目录）只把它从哈希表摘除，等最后一个使用者释放时再销毁。sysctl `fs.dentry-negative` 设为 1 时，删除文件后也不保留负 dentry，用于防止大量删除操作产生海量负 dentry 占用内存。

### 失效：d_drop() 与 d_invalidate()

[`fs/dcache.c:553-578`](../../linux/fs/dcache.c#L553-L578)（v6.18.60）

```c
static void ___d_drop(struct dentry *dentry)
{
	struct hlist_bl_head *b;
	/*
	 * Hashed dentries are normally on the dentry hashtable,
	 * with the exception of those newly allocated by
	 * d_obtain_root, which are always IS_ROOT:
	 */
	if (unlikely(IS_ROOT(dentry)))
		b = &dentry->d_sb->s_roots;
	else
		b = d_hash(dentry->d_name.hash);

	hlist_bl_lock(b);
	__hlist_bl_del(&dentry->d_hash);
	hlist_bl_unlock(b);
}

void __d_drop(struct dentry *dentry)
{
	if (!d_unhashed(dentry)) {
		___d_drop(dentry);
		dentry->d_hash.pprev = NULL;
		write_seqcount_invalidate(&dentry->d_seq);
	}
}
```

`__d_drop()` 把 dentry 从哈希表中摘除，并用 `write_seqcount_invalidate()` 使 `d_seq` 的序号跳变，让所有基于旧序号的 RCU 查找失败。被 drop 的 dentry 不能再被查找到，但已经持有它的使用者不受影响。`d_invalidate()`（[`fs/dcache.c:1645-1673`](../../linux/fs/dcache.c#L1645-L1673)）在此基础上还要回收它下面的子树并卸载其中的挂载，用于网络文件系统发现目录已被服务器删除的情况。

### LRU 回收

每个超级块有自己的 dentry LRU（`sb->s_dentry_lru`），它是 memcg 与 NUMA 感知的 `list_lru`：按节点、按内存控制组分别维护链表，回收时只扫描受压的那一部分。

超级块的 shrinker 回调 `super_cache_scan()` 按 dentry、inode 与文件系统私有对象的数量比例分配本次扫描量：

[`fs/super.c:200-229`](../../linux/fs/super.c#L200-L229)（v6.18.60）

```c
	if (sb->s_op->nr_cached_objects)
		fs_objects = sb->s_op->nr_cached_objects(sb, sc);

	inodes = list_lru_shrink_count(&sb->s_inode_lru, sc);
	dentries = list_lru_shrink_count(&sb->s_dentry_lru, sc);
	total_objects = dentries + inodes + fs_objects;
	if (!total_objects)
		total_objects = 1;

	/* proportion the scan between the caches */
	dentries = mult_frac(sc->nr_to_scan, dentries, total_objects);
	inodes = mult_frac(sc->nr_to_scan, inodes, total_objects);
	fs_objects = mult_frac(sc->nr_to_scan, fs_objects, total_objects);

	/*
	 * prune the dcache first as the icache is pinned by it, then
	 * prune the icache, followed by the filesystem specific caches
	 *
	 * Ensure that we always scan at least one object - memcg kmem
	 * accounting uses this to fully empty the caches.
	 */
	sc->nr_to_scan = dentries + 1;
	freed = prune_dcache_sb(sb, sc);
	sc->nr_to_scan = inodes + 1;
	freed += prune_icache_sb(sb, sc);

	if (fs_objects) {
		sc->nr_to_scan = fs_objects + 1;
		freed += sb->s_op->free_cached_objects(sb, sc);
	}
```

先回收 dentry 再回收 inode 的顺序很重要：dentry 持有 inode 的引用，只有 dentry 被释放后，对应的 inode 才会变为未使用、进入 inode LRU。函数开头还检查了 `__GFP_FS`：在文件系统内部发起的内存分配（`GFP_NOFS`）不能回过头来回收文件系统对象，否则可能在持有文件系统锁的情况下重入导致死锁。

`prune_dcache_sb()`（[`fs/dcache.c:1215-1224`](../../linux/fs/dcache.c#L1215-L1224)）遍历 LRU，对每个 dentry 调用隔离回调：

[`fs/dcache.c:1145-1201`](../../linux/fs/dcache.c#L1145-L1201)（v6.18.60）

```c
static enum lru_status dentry_lru_isolate(struct list_head *item,
		struct list_lru_one *lru, void *arg)
{
	struct list_head *freeable = arg;
	struct dentry	*dentry = container_of(item, struct dentry, d_lru);


	/*
	 * we are inverting the lru lock/dentry->d_lock here,
	 * so use a trylock. If we fail to get the lock, just skip
	 * it
	 */
	if (!spin_trylock(&dentry->d_lock))
		return LRU_SKIP;

	/*
	 * Referenced dentries are still in use. If they have active
	 * counts, just remove them from the LRU. Otherwise give them
	 * another pass through the LRU.
	 */
	if (dentry->d_lockref.count) {
		d_lru_isolate(lru, dentry);
		spin_unlock(&dentry->d_lock);
		return LRU_REMOVED;
	}

	if (dentry->d_flags & DCACHE_REFERENCED) {
		dentry->d_flags &= ~DCACHE_REFERENCED;
		...
		return LRU_ROTATE;
	}

	d_lru_shrink_move(lru, dentry, freeable);
	spin_unlock(&dentry->d_lock);

	return LRU_REMOVED;
}
```

- 仍有引用的 dentry（惰性 LRU 留下的）直接从 LRU 移除。
- 带 `DCACHE_REFERENCED` 的清除标志后转到 LRU 尾部（`LRU_ROTATE`），获得第二次机会，这是经典的 CLOCK 近似 LRU 算法。
- 其余移入私有的 `dispose` 链表，并设置 `DCACHE_SHRINK_LIST`。

注意这里用的是 `spin_trylock()`：LRU 锁与 `d_lock` 的正常顺序是先 `d_lock`，此处反过来，只能尝试获取，失败就跳过。

收集完毕后，`shrink_dentry_list()` 在不持 LRU 锁的情况下逐个销毁：

[`fs/dcache.c:1122-1143`](../../linux/fs/dcache.c#L1122-L1143)（v6.18.60）

```c
void shrink_dentry_list(struct list_head *list)
{
	while (!list_empty(list)) {
		struct dentry *dentry;

		dentry = list_entry(list->prev, struct dentry, d_lru);
		spin_lock(&dentry->d_lock);
		rcu_read_lock();
		if (!lock_for_kill(dentry)) {
			bool can_free;
			rcu_read_unlock();
			d_shrink_del(dentry);
			can_free = dentry->d_flags & DCACHE_DENTRY_KILLED;
			spin_unlock(&dentry->d_lock);
			if (can_free)
				dentry_free(dentry);
			continue;
		}
		d_shrink_del(dentry);
		shrink_kill(dentry);
	}
}
```

如果在此期间 dentry 又被别人引用了（`lock_for_kill()` 返回 false），就只从私有链表移除，让它继续存活。

```
                   dget / 查找命中
        ┌──────────────────────────────────┐
        ▼                                  │
  ┌───────────┐  dput 至 0，retain   ┌──────────────┐  shrinker 扫描    ┌──────────────┐
  │ 使用中     │ ───────────────────▶ │ 未使用(LRU)  │ ─────────────────▶ │ 私有回收链表  │
  │ count > 0 │                      │ count == 0   │  REFERENCED:轮转  │ SHRINK_LIST  │
  └───────────┘                      └──────────────┘                   └──────┬───────┘
        │ dput 至 0，不 retain（unhashed/DONTCACHE/d_delete）                 │ __dentry_kill
        └──────────────────────────────▶ __dentry_kill ◀─────────────────────┘
                                              │ iput(inode)，dput(parent)
                                              ▼
                                     call_rcu → kmem_cache_free
```

*图 9-4：dentry 的生命周期*

## 关键细节 / 设计考量

### 为什么 dentry 要经 RCU 释放

RCU 路径查找在不持有任何引用的情况下访问 dentry 的名字、父指针、`d_inode`、`d_flags`。如果 dentry 在 `__dentry_kill()` 后立即被 slab 回收并重新分配给别人，查找者就会读到完全无关的数据。`call_rcu()` 保证释放发生在所有可能看到它的 RCU 读者退出之后。inode 同理，`destroy_inode()` 也经 `call_rcu()` 释放（9.5 节）；外部名字 `external_name` 带有引用计数，重命名时旧名字也经 RCU 延迟释放。例外是从未对 RCU 可见的 dentry（`DCACHE_NORCU`），可以立即释放。

### 锁的顺序

dcache 中的锁顺序记录在 [`fs/dcache.c:40-75`](../../linux/fs/dcache.c#L40-L75) 的注释中：

```
inode->i_lock
  dentry->d_lock
    s_dentry_lru 的 list_lru 锁
    哈希桶的位锁
    s_roots 锁
```

有祖先关系的两个 dentry，先锁祖先再锁后代；没有祖先关系的，由 `rename_lock` 串行化，顺序任意。凡是与此顺序相反的地方（LRU 隔离回调、`lock_for_kill()`），都使用 trylock 并在失败时回退。

### 负目录项的代价

负目录项极大地加速了"不存在"的查找，但它们不受任何上限约束：一个程序反复 `stat()` 随机的不存在路径，就能制造出数百万个负 dentry。它们会在内存压力下被正常回收，但在没有压力时会无限增长，曾引起过多次讨论。当前内核提供的手段包括 `fs.dentry-negative` 策略、`vm.vfs_cache_pressure` 调节回收倾向，以及文件系统通过 `DCACHE_DONTCACHE`、`->d_delete` 拒绝缓存。

### 目录树的完整性

dcache 中的目录树总是"向上完整"的：任何 dentry 的祖先链一直到根都在内存中（子持有父的引用）。但它"向下"不完整：一个目录的子链表只包含被查找过的名字，不能用来列出目录内容——`readdir()` 总是调用文件系统的 `->iterate_shared`。

## 动手实验

### 观察 dcache 规模

```bash
cat /proc/sys/fs/dentry-state     # nr_dentry nr_unused age_limit want_pages nr_negative dummy
slabtop -o | grep -E "dentry|inode_cache"
sync; echo 2 > /proc/sys/vm/drop_caches    # 回收 dentry 与 inode（会经过 super_cache_scan）
cat /proc/sys/fs/dentry-state
```

### 负目录项策略

实验目录需位于 ext4 等磁盘文件系统上。tmpfs、ramfs 的 `->lookup` 是 `simple_lookup()`（[`fs/libfs.c:69-83`](../../linux/fs/libfs.c#L69-L83)），它给负 dentry 设置了 `DCACHE_DONTCACHE`，删除文件后不会留下负 dentry。

```bash
mkdir /var/tmp/negtest && cd /var/tmp/negtest
cat /proc/sys/fs/dentry-state
for i in $(seq 20000); do touch f$i; rm f$i; done
cat /proc/sys/fs/dentry-state       # 负 dentry 数显著增加
echo 1 > /proc/sys/fs/dentry-negative
for i in $(seq 20000); do touch g$i; rm g$i; done
cat /proc/sys/fs/dentry-state       # 增长明显变小
echo 0 > /proc/sys/fs/dentry-negative
```

### 谁在回收 dcache

```bash
sudo bpftrace -e '
kprobe:prune_dcache_sb { @[comm, kstack(6)] = count(); }
interval:s:30 { exit(); }'
```

在另一个终端制造内存压力（如 `stress-ng --vm 2 --vm-bytes 90%`），可以看到 kswapd 或直接回收路径经 `shrink_slab()` → `super_cache_scan()` 调用 `prune_dcache_sb()`。

## 小结

- dcache 以（父 dentry，名字哈希）为键的全局哈希表支撑快速查找，以父子指针维护向上完整的内存目录树，以超级块 LRU 缓存未使用的 dentry，并用负 dentry 缓存"不存在"。
- dentry 关联 inode 时在 `d_seq` 写临界区内修改 `d_inode` 与类型位，类型位让路径查找无需访问 inode；目录只能有一个别名，由 `d_splice_alias()` 保证。
- `__d_lookup_rcu()` 不取引用、只返回 `d_seq` 采样值，由调用者链式校验；`d_lookup()` 用 `rename_lock` 防止并发重命名导致的假阴性；`d_alloc_parallel()` 借助查找中哈希表与 `i_dir_seq` 避免对同一名字的重复 `->lookup`。
- 引用计数基于 lockref，`dput()` 在最后一个引用释放时由 `retain_dentry()` 决定保留（加入 LRU 或设置 REFERENCED）还是 `__dentry_kill()` 销毁；销毁释放 inode 引用与父引用，内存经 RCU 延迟释放。
- 超级块 shrinker 先回收 dentry 再回收 inode，LRU 扫描采用惰性移除与 REFERENCED 二次机会，逆锁序的地方一律使用 trylock。

## 参考

- `linux/Documentation/filesystems/vfs.rst`（Directory Entry Cache 一节）
- `linux/Documentation/filesystems/path-lookup.rst`
- `linux/Documentation/admin-guide/sysctl/fs.rst`（`dentry-state`、`dentry-negative`）
- Nick Piggin, "fs: dcache scale hash"、"fs: dcache per-bucket dcache hash locking"（v2.6.38）
- Al Viro, "parallel lookups" 补丁系列（v4.7，引入 `d_alloc_parallel()`）
