# 4.5 SLUB 分配器

> **本节源码**：[`mm/slub.c`](../../linux/mm/slub.c)、[`mm/slab.h`](../../linux/mm/slab.h)、[`mm/slab_common.c`](../../linux/mm/slab_common.c)、[`include/linux/slab.h`](../../linux/include/linux/slab.h)
> **关键函数**：`slab_alloc_node()`、`__slab_alloc_node()`、`___slab_alloc()`、`allocate_slab()`、`alloc_from_pcs()`、`slab_free()`、`do_slab_free()`、`__slab_free()`、`__do_kmalloc_node()`、`kfree()`
> **前置知识**：4.3 伙伴系统与 per-CPU 页缓存

## 概述

伙伴系统的最小分配单位是一页（4 KB），而内核中绝大多数对象只有几十到几百字节：`struct dentry` 约 200 字节，`struct file` 约 240 字节。若每个对象都占一页，内存浪费不可接受。**slab 分配器**的做法是从伙伴系统取一个或几个连续页（称为一个 slab），切成大小相同的对象槽位，按对象类型分别管理。

Linux 历史上有过三种实现：SLAB（经典实现）、SLOB（嵌入式精简版）和 SLUB（v2.6.22 引入）。SLOB 在 v6.4、SLAB 在 v6.8 被删除，此后 SLUB 是唯一实现，`CONFIG_SLUB=y` 为固定选项。本书配置未启用 `CONFIG_SLUB_TINY`。

SLUB 的设计要点是：每 CPU 持有一个"当前 slab"，其空闲对象链表可以通过一次 `cmpxchg` 无锁地分配和释放；空闲链表指针就存放在空闲对象内部，不需要额外的元数据；slab 描述符直接复用 `struct page` 的空间（`struct slab`）。v6.18 在此之上又引入了可选的 per-CPU **sheaf**（对象数组）缓存层。

## 核心数据结构

```
 struct kmem_cache (每种对象一个)
 ├── cpu_slab  ──▶ kmem_cache_cpu (per-CPU)
 │                 ├── freelist ──▶ obj ──▶ obj ──▶ NULL   （无锁快速路径）
 │                 ├── slab     ──▶ 当前 slab（frozen）
 │                 └── partial  ──▶ slab ─▶ slab           （per-CPU 部分空 slab）
 ├── cpu_sheaves ─▶ slub_percpu_sheaves (per-CPU，可选)
 │                 └── main / spare ──▶ slab_sheaf { objects[] }
 └── node[nid] ──▶ kmem_cache_node (per-node)
                   ├── partial ──▶ slab ─▶ slab ─▶ ...     （受 list_lock 保护）
                   └── barn    ──▶ 满 / 空 sheaf 仓库
```

*图 4-9：SLUB 的三级结构*

### `struct kmem_cache`

每种对象类型一个缓存描述符：

[`mm/slab.h:238-296`](../../linux/mm/slab.h#L238-L296)（v6.18.60，节选）

```c
struct kmem_cache {
	struct kmem_cache_cpu __percpu *cpu_slab;
	struct lock_class_key lock_key;
	struct slub_percpu_sheaves __percpu *cpu_sheaves;
	/* Used for retrieving partial slabs, etc. */
	slab_flags_t flags;
	unsigned long min_partial;
	unsigned int size;		/* Object size including metadata */
	unsigned int object_size;	/* Object size without metadata */
	struct reciprocal_value reciprocal_size;
	unsigned int offset;		/* Free pointer offset */
#ifdef CONFIG_SLUB_CPU_PARTIAL
	/* Number of per cpu partial objects to keep around */
	unsigned int cpu_partial;
	/* Number of per cpu partial slabs to keep around */
	unsigned int cpu_partial_slabs;
#endif
	unsigned int sheaf_capacity;
	struct kmem_cache_order_objects oo;

	/* Allocation and freeing of slabs */
	struct kmem_cache_order_objects min;
	gfp_t allocflags;		/* gfp flags to use on each alloc */
	int refcount;			/* Refcount for slab cache destroy */
	void (*ctor)(void *object);	/* Object constructor */
	unsigned int inuse;		/* Offset to metadata */
	unsigned int align;		/* Alignment */
	unsigned int red_left_pad;	/* Left redzone padding size */
	const char *name;		/* Name (only for display!) */
	struct list_head list;		/* List of slab caches */
	...
#ifdef CONFIG_SLAB_FREELIST_HARDENED
	unsigned long random;
#endif
	...
	struct kmem_cache_node *node[MAX_NUMNODES];
};
```

| 字段 | 含义 |
|---|---|
| `object_size` / `size` | 对象原始大小 / 加上对齐、调试信息后的实际槽位大小 |
| `offset` | 空闲指针在对象中的偏移，通常为 0（或对象中部，见后文） |
| `oo` / `min` | 打包的"slab 阶数 + 每 slab 对象数"，`oo` 为首选，`min` 为内存紧张时的最低配置 |
| `min_partial` | 每节点 partial 链表至少保留的 slab 数，低于它时空 slab 不归还伙伴系统 |
| `cpu_partial_slabs` | per-CPU partial 链表最多保留的 slab 数 |
| `random` | `CONFIG_SLAB_FREELIST_HARDENED` 下用于混淆空闲指针的随机数 |

### `struct slab`：复用 `struct page`

slab 描述符不单独分配，而是与 slab 首页的 `struct page` 重叠，就像 folio 一样：

[`mm/slab.h:52-102`](../../linux/mm/slab.h#L52-L102)（v6.18.60，节选）

```c
struct slab {
	memdesc_flags_t flags;

	struct kmem_cache *slab_cache;
	union {
		struct {
			union {
				struct list_head slab_list;
				...
#ifdef CONFIG_SLUB_CPU_PARTIAL
				struct {
					struct slab *next;
					int slabs;	/* Nr of slabs left */
				};
#endif
			};
			/* Double-word boundary */
			union {
				struct {
					void *freelist;		/* first free object */
					union {
						unsigned long counters;
						struct {
							unsigned inuse:16;
							unsigned objects:15;
							...
							unsigned frozen:1;
						};
					};
				};
#ifdef system_has_freelist_aba
				freelist_aba_t freelist_counter;
#endif
			};
		};
		struct rcu_head rcu_head;
	};

	unsigned int __page_type;
	atomic_t __page_refcount;
#ifdef CONFIG_SLAB_OBJ_EXT
	unsigned long obj_exts;
#endif
};
```

`freelist` 与 `counters`（含 `inuse`、`objects`、`frozen`）被放在相邻的两个字中，并按双字对齐，这样就能用一条 `cmpxchg16b` 原子地同时更新"空闲链表头"和"使用计数"。`frozen = 1` 表示该 slab 正被某个 CPU 作为当前 slab 使用，其他 CPU 释放对象到这个 slab 时不需要处理链表归属。`slab_cache` 与 `page->compound_head` 重叠，`SLAB_MATCH()` 断言保证其最低位为 0，不会被误判为尾页。

### `kmem_cache_cpu` 与 `kmem_cache_node`

[`mm/slub.c:421-437`](../../linux/mm/slub.c#L421-L437)（v6.18.60）

```c
struct kmem_cache_cpu {
	union {
		struct {
			void **freelist;	/* Pointer to next available object */
			unsigned long tid;	/* Globally unique transaction id */
		};
		freelist_aba_t freelist_tid;
	};
	struct slab *slab;	/* The slab from which we are allocating */
#ifdef CONFIG_SLUB_CPU_PARTIAL
	struct slab *partial;	/* Partially allocated slabs */
#endif
	local_trylock_t lock;	/* Protects the fields above */
#ifdef CONFIG_SLUB_STATS
	unsigned int stat[NR_SLUB_STAT_ITEMS];
#endif
};
```

[`mm/slub.c:492-502`](../../linux/mm/slub.c#L492-L502)（v6.18.60）

```c
struct kmem_cache_node {
	spinlock_t list_lock;
	unsigned long nr_partial;
	struct list_head partial;
#ifdef CONFIG_SLUB_DEBUG
	atomic_long_t nr_slabs;
	atomic_long_t total_objects;
	struct list_head full;
#endif
	struct node_barn *barn;
};
```

注意 per-CPU 的 `freelist` 与 slab 的 `freelist` 是**两条不同的链**：当某 slab 成为 CPU 的当前 slab 时，它的全部空闲对象被整体"转移"到 `c->freelist`（`slab->freelist` 置 NULL），此后本 CPU 从 `c->freelist` 分配；而其他 CPU 释放到这个 slab 的对象则挂回 `slab->freelist`。两条链在 `c->freelist` 用尽时通过 `get_freelist()` 合并。

`tid`（transaction id）是每次操作递增的序号，与 `freelist` 一起做双字 `cmpxchg`，用来检测"读取 freelist 之后、更新之前，是否有其他操作（包括被抢占后在本 CPU 上运行的其他任务）修改过它"，从而解决 ABA 问题。

### 空闲指针

空闲对象的前 8 字节（`offset` 处）存放下一个空闲对象的地址。启用 `CONFIG_SLAB_FREELIST_HARDENED`（本书配置已启用）后，指针被编码存储：

[`mm/slub.c:562-573`](../../linux/mm/slub.c#L562-L573)（v6.18.60）

```c
static inline freeptr_t freelist_ptr_encode(const struct kmem_cache *s,
					    void *ptr, unsigned long ptr_addr)
{
	unsigned long encoded;

#ifdef CONFIG_SLAB_FREELIST_HARDENED
	encoded = (unsigned long)ptr ^ s->random ^ swab(ptr_addr);
#else
	encoded = (unsigned long)ptr;
#endif
	return (freeptr_t){.v = encoded};
}
```

编码值 = 真实指针 ⊕ 缓存随机数 ⊕ 存放地址的字节反转。攻击者即使通过 UAF 漏洞覆写了空闲对象，也很难构造出能解码为目标地址的值。`set_freepointer()`（[`mm/slub.c:629-638`](../../linux/mm/slub.c#L629-L638)）中还有一个简单的 `BUG_ON(object == fp)` 用于发现连续两次释放同一对象。

对于有构造函数（`ctor`）或 `SLAB_TYPESAFE_BY_RCU` 的缓存，对象内容在空闲时也必须保持有效，此时空闲指针被放到对象之后（`offset ≥ object_size`），由 `calculate_sizes()`（[`mm/slub.c:7864`](../../linux/mm/slub.c#L7864)）决定。

## 分配路径

### 调用链总览

```
kmalloc(size, gfp) / kmem_cache_alloc(s, gfp)
└── slab_alloc_node()                           mm/slub.c:5294
    ├── slab_pre_alloc_hook()                   // memcg、fault injection 等
    ├── kfence_alloc()                          // KFENCE 采样（CONFIG_KFENCE）
    ├── alloc_from_pcs()                        // ① sheaf 缓存（若启用）
    ├── __slab_alloc_node()                     // ② 无锁快速路径：c->freelist
    │   └── __slab_alloc() → ___slab_alloc()    // ③ 慢速路径
    │       ├── get_freelist()                  //    取回当前 slab 上远程释放的对象
    │       ├── slub_percpu_partial()           //    per-CPU partial slab
    │       ├── get_partial()                   //    per-node partial slab
    │       └── new_slab() → allocate_slab()    //    向伙伴系统要新 slab
    └── slab_post_alloc_hook()                  // 清零、KASAN、memcg 计费
```

### 入口：`slab_alloc_node()`

[`mm/slub.c:5294-5327`](../../linux/mm/slub.c#L5294-L5327)（v6.18.60）

```c
static __fastpath_inline void *slab_alloc_node(struct kmem_cache *s, struct list_lru *lru,
		gfp_t gfpflags, int node, unsigned long addr, size_t orig_size)
{
	void *object;
	bool init = false;

	s = slab_pre_alloc_hook(s, gfpflags);
	if (unlikely(!s))
		return NULL;

	object = kfence_alloc(s, orig_size, gfpflags);
	if (unlikely(object))
		goto out;

	if (s->cpu_sheaves)
		object = alloc_from_pcs(s, gfpflags, node);

	if (!object)
		object = __slab_alloc_node(s, gfpflags, node, addr, orig_size);

	maybe_wipe_obj_freeptr(s, object);
	init = slab_want_init_on_alloc(gfpflags, s);

out:
	/*
	 * When init equals 'true', like for kzalloc() family, only
	 * @orig_size bytes might be zeroed instead of s->object_size
	 * In case this fails due to memcg_slab_post_alloc_hook(),
	 * object is set to NULL
	 */
	slab_post_alloc_hook(s, lru, gfpflags, 1, &object, init, orig_size);

	return object;
}
```

KFENCE（`CONFIG_KFENCE=y`）是一种低开销的内存错误检测器，按采样间隔把少量分配重定向到带保护页的专用池。本书配置中 `CONFIG_KFENCE_SAMPLE_INTERVAL=0`，默认不采样，可通过启动参数 `kfence.sample_interval=100` 开启。

### ① sheaf：per-CPU 对象数组（v6.18 新增）

sheaf 是 v6.18 引入的**可选**缓存层：每 CPU 持有一个"主 sheaf"（`main`），它是一个定长指针数组，分配就是弹出栈顶元素：

[`mm/slub.c:5138-5207`](../../linux/mm/slub.c#L5138-L5207)（v6.18.60，节选）

```c
static __fastpath_inline
void *alloc_from_pcs(struct kmem_cache *s, gfp_t gfp, int node)
{
	struct slub_percpu_sheaves *pcs;
	bool node_requested;
	void *object;
	...
	if (!local_trylock(&s->cpu_sheaves->lock))
		return NULL;

	pcs = this_cpu_ptr(s->cpu_sheaves);

	if (unlikely(pcs->main->size == 0)) {
		pcs = __pcs_replace_empty_main(s, pcs, gfp);
		if (unlikely(!pcs))
			return NULL;
	}

	object = pcs->main->objects[pcs->main->size - 1];
	...
	pcs->main->size--;

	local_unlock(&s->cpu_sheaves->lock);

	stat(s, ALLOC_PCS);

	return object;
}
```

主 sheaf 空了就与备用 sheaf（`spare`）交换，或从节点级的 `barn` 仓库换一个满的回来。与传统的 freelist 相比，sheaf 的优势在于：对象可以来自任意 slab，批量填充/回收时一次操作多个对象；`kfree_rcu()` 也可以把对象攒进 `rcu_free` sheaf，等一个 RCU 宽限期后整体处理。目前只有显式在 `kmem_cache_args` 中设置 `sheaf_capacity` 的缓存才启用 sheaf，如 maple tree 节点缓存（[`lib/maple_tree.c:5872`](../../linux/lib/maple_tree.c#L5872)）和 VMA 缓存（[`mm/vma_init.c:19`](../../linux/mm/vma_init.c#L19)），其余缓存的 `cpu_sheaves` 为 NULL。

### ② 无锁快速路径：`__slab_alloc_node()`

[`mm/slub.c:4846-4941`](../../linux/mm/slub.c#L4846-L4941)（v6.18.60，节选）

```c
static __always_inline void *__slab_alloc_node(struct kmem_cache *s,
		gfp_t gfpflags, int node, unsigned long addr, size_t orig_size)
{
	struct kmem_cache_cpu *c;
	struct slab *slab;
	unsigned long tid;
	void *object;

redo:
	...
	c = raw_cpu_ptr(s->cpu_slab);
	tid = READ_ONCE(c->tid);
	...
	barrier();
	...
	object = c->freelist;
	slab = c->slab;
	...
	if (!USE_LOCKLESS_FAST_PATH() ||
	    unlikely(!object || !slab || !node_match(slab, node))) {
		object = __slab_alloc(s, gfpflags, node, addr, c, orig_size);
	} else {
		void *next_object = get_freepointer_safe(s, object);
		...
		if (unlikely(!__update_cpu_freelist_fast(s, object, next_object, tid))) {
			note_cmpxchg_failure("slab_alloc", s, tid);
			goto redo;
		}
		prefetch_freepointer(s, next_object);
		stat(s, ALLOC_FASTPATH);
	}

	return object;
}
```

这条路径**不关抢占、不关中断、不加锁**。它先读 `tid`，再读 `freelist`，然后用 `this_cpu_try_cmpxchg` 把 `(freelist, tid)` 原子地替换为 `(next_object, tid + 1)`：

[`mm/slub.c:4429-4439`](../../linux/mm/slub.c#L4429-L4439)（v6.18.60）

```c
static inline bool
__update_cpu_freelist_fast(struct kmem_cache *s,
			   void *freelist_old, void *freelist_new,
			   unsigned long tid)
{
	freelist_aba_t old = { .freelist = freelist_old, .counter = tid };
	freelist_aba_t new = { .freelist = freelist_new, .counter = next_tid(tid) };

	return this_cpu_try_cmpxchg_freelist(s->cpu_slab->freelist_tid.full,
					     &old.full, new.full);
}
```

`this_cpu_*` 操作在 x86_64 上编译为带 `%gs` 前缀的 `cmpxchg16b` 指令，天然作用于"执行时所在 CPU"的变量。如果读取期间任务被迁移到另一个 CPU，那么 cmpxchg 会作用于新 CPU 的 `freelist_tid`，其 `tid` 与之前读到的不同（`tid` 编码了 CPU 号），比较失败后重试即可。这就是 SLUB 快速路径不需要关抢占的原因。`USE_LOCKLESS_FAST_PATH()` 仅在 `CONFIG_PREEMPT_RT` 下为 false（[`mm/slub.c:209-225`](../../linux/mm/slub.c#L209-L225)）。

### ③ 慢速路径：`___slab_alloc()`

快速路径在 `c->freelist` 为空、当前没有 slab 或节点不匹配时进入 `___slab_alloc()`。它按代价从低到高寻找对象来源：

**(a) 当前 slab 上的远程释放对象**：

[`mm/slub.c:4579-4614`](../../linux/mm/slub.c#L4579-L4614)（v6.18.60，节选）

```c
	/* must check again c->slab in case we got preempted and it changed */
	local_lock_cpu_slab(s, flags);

	if (unlikely(slab != c->slab)) {
		local_unlock_cpu_slab(s, flags);
		goto reread_slab;
	}
	freelist = c->freelist;
	if (freelist)
		goto load_freelist;

	freelist = get_freelist(s, slab);

	if (!freelist) {
		c->slab = NULL;
		c->tid = next_tid(c->tid);
		local_unlock_cpu_slab(s, flags);
		stat(s, DEACTIVATE_BYPASS);
		goto new_slab;
	}

	stat(s, ALLOC_REFILL);

load_freelist:

	lockdep_assert_held(this_cpu_ptr(&s->cpu_slab->lock));
	...
	VM_BUG_ON(!c->slab->frozen);
	c->freelist = get_freepointer(s, freelist);
	c->tid = next_tid(c->tid);
	local_unlock_cpu_slab(s, flags);
	return freelist;
```

`get_freelist()`（[`mm/slub.c:4449`](../../linux/mm/slub.c#L4449)）用 `cmpxchg` 把 `slab->freelist` 整条取走并置空；它总是把 `inuse` 设为 `objects`（对象全部归 CPU 所有），并且只有取到非空链表时才保持 `frozen = 1`；若为空，说明当前 slab 已满，`frozen` 被清 0，slab 就此"解冻"，变成一个不在任何链表上的满 slab。

**(b) per-CPU partial 链表**（`CONFIG_SLUB_CPU_PARTIAL`，本书配置已启用）：

[`mm/slub.c:4633-4664`](../../linux/mm/slub.c#L4633-L4664)（v6.18.60，节选）

```c
#ifdef CONFIG_SLUB_CPU_PARTIAL
	while (slub_percpu_partial(c)) {
		...
		slab = slub_percpu_partial(c);
		slub_set_percpu_partial(c, slab);

		if (likely(node_match(slab, node) &&
			   pfmemalloc_match(slab, gfpflags)) ||
		    !allow_spin) {
			c->slab = slab;
			freelist = get_freelist(s, slab);
			VM_BUG_ON(!freelist);
			stat(s, CPU_PARTIAL_ALLOC);
			goto load_freelist;
		}
		...
	}
#endif
```

per-CPU partial 链表上的 slab 都已处于 frozen 状态，取用时不需要节点锁。

**(c) 节点 partial 链表**：`get_partial()` 先在目标节点的 `kmem_cache_node->partial` 上查找（持 `list_lock`），若失败再按距离尝试其他节点（受 `remote_node_defrag_ratio` 约束）。找到的 slab 经 `freeze_slab()` 冻结后成为当前 slab。`get_partial_node()`（[`mm/slub.c:3534`](../../linux/mm/slub.c#L3534)）在取走第一个 slab 后，还会继续取若干 slab 放入 per-CPU partial 链表，直到达到 `cpu_partial_slabs` 的一半。

**(d) 新建 slab**：以上都失败时调用 `new_slab()`，从伙伴系统分配新页：

[`mm/slub.c:3260-3331`](../../linux/mm/slub.c#L3260-L3331)（v6.18.60，节选）

```c
static struct slab *allocate_slab(struct kmem_cache *s, gfp_t flags, int node)
{
	...
	flags |= s->allocflags;

	/*
	 * Let the initial higher-order allocation fail under memory pressure
	 * so we fall-back to the minimum order allocation.
	 */
	alloc_gfp = (flags | __GFP_NOWARN | __GFP_NORETRY) & ~__GFP_NOFAIL;
	if ((alloc_gfp & __GFP_DIRECT_RECLAIM) && oo_order(oo) > oo_order(s->min))
		alloc_gfp = (alloc_gfp | __GFP_NOMEMALLOC) & ~__GFP_RECLAIM;

	/*
	 * __GFP_RECLAIM could be cleared on the first allocation attempt,
	 * so pass allow_spin flag directly.
	 */
	slab = alloc_slab_page(alloc_gfp, node, oo, allow_spin);
	if (unlikely(!slab)) {
		oo = s->min;
		alloc_gfp = flags;
		/*
		 * Allocation may have failed due to fragmentation.
		 * Try a lower order alloc if possible
		 */
		slab = alloc_slab_page(alloc_gfp, node, oo, allow_spin);
		if (unlikely(!slab))
			return NULL;
		stat(s, ORDER_FALLBACK);
	}

	slab->objects = oo_objects(oo);
	slab->inuse = 0;
	slab->frozen = 0;
	...
	slab->slab_cache = s;
	...
	shuffle = shuffle_freelist(s, slab, allow_spin);

	if (!shuffle) {
		start = fixup_red_left(s, start);
		start = setup_object(s, start);
		slab->freelist = start;
		for (idx = 0, p = start; idx < slab->objects - 1; idx++) {
			next = p + s->size;
			next = setup_object(s, next);
			set_freepointer(s, p, next);
			p = next;
		}
		set_freepointer(s, p, NULL);
	}

	return slab;
}
```

这里体现了 SLUB 的一个重要策略：**先以首选阶 `oo` 尝试，且不做回收（清除 `__GFP_RECLAIM`），失败再退回最低阶 `min` 正常分配**。较高的阶能让一个 slab 容纳更多对象、减少慢速路径次数，但不值得为此触发回收。`shuffle_freelist()`（`CONFIG_SLAB_FREELIST_RANDOM=y`）把初始空闲链表顺序随机打乱，增加堆利用的难度；未启用时按地址顺序串联所有对象。

新 slab 的 `inuse` 随后被设为 `objects`、`frozen` 设为 1，空闲链表整体交给 `c->freelist`（[`mm/slub.c:4751-4754`](../../linux/mm/slub.c#L4751-L4754)）。

### slab 的阶数选择

[`mm/slub.c:7597-7650`](../../linux/mm/slub.c#L7597-L7650)（v6.18.60，节选）

```c
static inline int calculate_order(unsigned int size)
{
	...
	min_objects = slub_min_objects;
	if (!min_objects) {
		...
		unsigned int nr_cpus = num_present_cpus();
		if (nr_cpus <= 1)
			nr_cpus = nr_cpu_ids;
		min_objects = 4 * (fls(nr_cpus) + 1);
	}
	...
	for (unsigned int fraction = 16; fraction > 1; fraction /= 2) {
		order = calc_slab_order(size, min_order, slub_max_order,
					fraction);
		if (order <= slub_max_order)
			return order;
	}
```

CPU 越多，每个 slab 期望容纳的对象数越多（8 CPU 时为 4 × (4 + 1) = 20 个），以降低 partial 链表的竞争。在不超过 `slub_max_order`（默认 `PAGE_ALLOC_COSTLY_ORDER` = 3，[`mm/slub.c:7548-7549`](../../linux/mm/slub.c#L7548-L7549)）的前提下，从浪费不超过 1/16 开始，逐步放宽到 1/2，选最小的合适阶。

## 释放路径

### 调用链总览

```
kfree(obj) / kmem_cache_free(s, obj)
└── slab_free()                                 mm/slub.c:6688
    ├── memcg_slab_free_hook() / slab_free_hook()
    ├── free_to_pcs()                           // ① sheaf（若启用且对象属于本节点）
    └── do_slab_free()                          // ② 快速路径：对象属于当前 CPU slab
        └── __slab_free()                       // ③ 慢速路径：对象属于其他 slab
            ├── put_cpu_partial()               //    满 → 部分空：放入 per-CPU partial
            ├── add_partial()                   //    （无 cpu partial 时）放入节点 partial
            └── discard_slab()                  //    完全空且节点 partial 足够：归还伙伴系统
```

`kfree()` 需要先找到对象所属的缓存：

[`mm/slub.c:6885-6906`](../../linux/mm/slub.c#L6885-L6906)（v6.18.60）

```c
void kfree(const void *object)
{
	struct folio *folio;
	struct slab *slab;
	struct kmem_cache *s;
	void *x = (void *)object;

	trace_kfree(_RET_IP_, object);

	if (unlikely(ZERO_OR_NULL_PTR(object)))
		return;

	folio = virt_to_folio(object);
	if (unlikely(!folio_test_slab(folio))) {
		free_large_kmalloc(folio, (void *)object);
		return;
	}

	slab = folio_slab(folio);
	s = slab->slab_cache;
	slab_free(s, slab, x, _RET_IP_);
}
```

由虚拟地址找到 folio，再由 folio 的 `slab_cache` 字段找到缓存，所以 `kfree()` 无需调用者提供大小信息。不属于 slab 的 folio 是大于 `KMALLOC_MAX_CACHE_SIZE` 的 kmalloc，直接还给伙伴系统。

### 快速路径：`do_slab_free()`

[`mm/slub.c:6619-6685`](../../linux/mm/slub.c#L6619-L6685)（v6.18.60，节选）

```c
redo:
	...
	c = raw_cpu_ptr(s->cpu_slab);
	tid = READ_ONCE(c->tid);

	/* Same with comment on barrier() in __slab_alloc_node() */
	barrier();

	if (unlikely(slab != c->slab)) {
		...
			__slab_free(s, slab, head, tail, cnt, addr);
		...
		return;
	}
	...
	if (USE_LOCKLESS_FAST_PATH()) {
		freelist = READ_ONCE(c->freelist);

		set_freepointer(s, tail, freelist);

		if (unlikely(!__update_cpu_freelist_fast(s, freelist, head, tid))) {
			note_cmpxchg_failure("slab_free", s, tid);
			goto redo;
		}
	} else {
		...
	}
	stat_add(s, FREE_FASTPATH, cnt);
```

只有当对象恰好属于本 CPU 的当前 slab 时才能走快速路径：把对象压入 `c->freelist` 栈顶，与分配对称。

### 慢速路径：`__slab_free()`

对象属于其他 slab 时，要把它挂回该 slab 自己的 `freelist`，并根据 slab 状态的变化调整其链表归属：

[`mm/slub.c:5924-5957`](../../linux/mm/slub.c#L5924-L5957)（v6.18.60）

```c
	do {
		if (unlikely(n)) {
			spin_unlock_irqrestore(&n->list_lock, flags);
			n = NULL;
		}
		prior = slab->freelist;
		counters = slab->counters;
		set_freepointer(s, tail, prior);
		new.counters = counters;
		was_frozen = new.frozen;
		new.inuse -= cnt;
		if ((!new.inuse || !prior) && !was_frozen) {
			/* Needs to be taken off a list */
			if (!kmem_cache_has_cpu_partial(s) || prior) {

				n = get_node(s, slab_nid(slab));
				/*
				 * Speculatively acquire the list_lock.
				 * If the cmpxchg does not succeed then we may
				 * drop the list_lock without any processing.
				 *
				 * Otherwise the list_lock will synchronize with
				 * other processors updating the list of slabs.
				 */
				spin_lock_irqsave(&n->list_lock, flags);

				on_node_partial = slab_test_node_partial(slab);
			}
		}

	} while (!slab_update_freelist(s, slab,
		prior, counters,
		head, new.counters,
		"__slab_free"));
```

`slab_update_freelist()` 用 `cmpxchg16b` 同时更新 `slab->freelist` 与 `counters`。之后根据三个状态决定后续动作：

| 释放前状态 | 条件 | 动作 |
|---|---|---|
| frozen（某 CPU 的当前 slab 或在 per-CPU partial 上） | `was_frozen` | 无需处理链表，直接返回 |
| 满 slab（`prior == NULL`） | 有 cpu partial | `put_cpu_partial()` 放入本 CPU 的 partial 链表 |
| 部分空，释放后变为全空 | `n->nr_partial >= min_partial` | 从节点 partial 摘下，`discard_slab()` 归还伙伴系统 |
| 部分空，释放后仍部分空 | — | 保持在节点 partial 上 |

[`mm/slub.c:5959-6013`](../../linux/mm/slub.c#L5959-L6013)（v6.18.60，节选）

```c
	if (likely(!n)) {

		if (likely(was_frozen)) {
			/*
			 * The list lock was not taken therefore no list
			 * activity can be necessary.
			 */
			stat(s, FREE_FROZEN);
		} else if (kmem_cache_has_cpu_partial(s) && !prior) {
			/*
			 * If we started with a full slab then put it onto the
			 * per cpu partial list.
			 */
			put_cpu_partial(s, slab, 1);
			stat(s, CPU_PARTIAL_FREE);
		}

		return;
	}
	...
	if (unlikely(!new.inuse && n->nr_partial >= s->min_partial))
		goto slab_empty;
	...
slab_empty:
	if (prior) {
		/*
		 * Slab on the partial list.
		 */
		remove_partial(n, slab);
		stat(s, FREE_REMOVE_PARTIAL);
	}

	spin_unlock_irqrestore(&n->list_lock, flags);
	stat(s, FREE_SLAB);
	discard_slab(s, slab);
```

满 slab 不在任何链表上（SLUB 默认不跟踪满 slab，只有调试模式下才挂在 `n->full` 上），因此释放到满 slab 的常见情形完全不需要节点锁。per-CPU partial 链表超过 `cpu_partial_slabs` 时，`put_cpu_partial()` 会把整条链表转移到节点 partial 上（`__put_partials()`）。

## kmalloc

### 大小分级

`kmalloc()` 建立在一组预先创建的通用缓存之上，大小按 2 的幂次分级，另有 96 和 192 字节两个中间档：

[`include/linux/slab.h:727-747`](../../linux/include/linux/slab.h#L727-L747)（v6.18.60，节选）

```c
	if (!size)
		return 0;

	if (size <= KMALLOC_MIN_SIZE)
		return KMALLOC_SHIFT_LOW;

	if (KMALLOC_MIN_SIZE <= 32 && size > 64 && size <= 96)
		return 1;
	if (KMALLOC_MIN_SIZE <= 64 && size > 128 && size <= 192)
		return 2;
	if (size <=          8) return 3;
	if (size <=         16) return 4;
	if (size <=         32) return 5;
	if (size <=         64) return 6;
	if (size <=        128) return 7;
	if (size <=        256) return 8;
	if (size <=        512) return 9;
	if (size <=       1024) return 10;
	if (size <=   2 * 1024) return 11;
	if (size <=   4 * 1024) return 12;
	if (size <=   8 * 1024) return 13;
	...
```

[`include/linux/slab.h:592-601`](../../linux/include/linux/slab.h#L592-L601)（v6.18.60，节选）

```c
#define KMALLOC_SHIFT_HIGH	(PAGE_SHIFT + 1)
#define KMALLOC_SHIFT_MAX	(MAX_PAGE_ORDER + PAGE_SHIFT)
#ifndef KMALLOC_SHIFT_LOW
#define KMALLOC_SHIFT_LOW	3
#endif

/* Maximum allocatable size */
#define KMALLOC_MAX_SIZE	(1UL << KMALLOC_SHIFT_MAX)
/* Maximum size for which we actually use a slab cache */
#define KMALLOC_MAX_CACHE_SIZE	(1UL << KMALLOC_SHIFT_HIGH)
```

x86_64 上 slab 缓存覆盖 8 B～8 KB（`kmalloc-8` ～ `kmalloc-8k`）；超过 8 KB 的请求由 `__kmalloc_large_node_noprof()` 直接向伙伴系统分配复合页，上限为 `KMALLOC_MAX_SIZE` = 4 MB。当 `size` 是编译期常量时，整个分级判断会在编译期折叠为一个常数。

### 缓存类型

同一大小又按用途分成多组缓存：

[`include/linux/slab.h:637-664`](../../linux/include/linux/slab.h#L637-L664)（v6.18.60）

```c
enum kmalloc_cache_type {
	KMALLOC_NORMAL = 0,
#ifndef CONFIG_ZONE_DMA
	KMALLOC_DMA = KMALLOC_NORMAL,
#endif
#ifndef CONFIG_MEMCG
	KMALLOC_CGROUP = KMALLOC_NORMAL,
#endif
#ifndef CONFIG_SLAB_OBJ_EXT
	KMALLOC_NO_OBJ_EXT = KMALLOC_NORMAL,
#endif
	KMALLOC_RANDOM_START = KMALLOC_NORMAL,
	KMALLOC_RANDOM_END = KMALLOC_RANDOM_START + RANDOM_KMALLOC_CACHES_NR,
#ifdef CONFIG_SLUB_TINY
	KMALLOC_RECLAIM = KMALLOC_NORMAL,
#else
	KMALLOC_RECLAIM,
#endif
#ifdef CONFIG_ZONE_DMA
	KMALLOC_DMA,
#endif
#ifdef CONFIG_MEMCG
	KMALLOC_CGROUP,
#endif
#ifdef CONFIG_SLAB_OBJ_EXT
	KMALLOC_NO_OBJ_EXT,
#endif
	NR_KMALLOC_TYPES
};
```

| 类型 | 触发条件 | `/proc/slabinfo` 中的名称 |
|---|---|---|
| `KMALLOC_NORMAL` + 15 份随机副本 | 普通请求 | `kmalloc-64`、`kmalloc-rnd-01-64` …… |
| `KMALLOC_RECLAIM` | `__GFP_RECLAIMABLE` | `kmalloc-rcl-64` |
| `KMALLOC_DMA` | `__GFP_DMA` | `dma-kmalloc-64` |
| `KMALLOC_CGROUP` | `__GFP_ACCOUNT`（计入 memcg） | `kmalloc-cg-64` |

`CONFIG_RANDOM_KMALLOC_CACHES=y`（v6.6 引入）时，普通请求按"调用点地址 ⊕ 启动随机种子"的哈希分散到 16 组缓存中（`kmalloc_type()`，[`include/linux/slab.h:681-709`](../../linux/include/linux/slab.h#L681-L709)），使攻击者难以让漏洞对象与目标对象落在同一个 slab 里，从而增加堆喷射（heap spraying）利用的难度。

### `__do_kmalloc_node()`

[`mm/slub.c:5666-5689`](../../linux/mm/slub.c#L5666-L5689)（v6.18.60）

```c
static __always_inline
void *__do_kmalloc_node(size_t size, kmem_buckets *b, gfp_t flags, int node,
			unsigned long caller)
{
	struct kmem_cache *s;
	void *ret;

	if (unlikely(size > KMALLOC_MAX_CACHE_SIZE)) {
		ret = __kmalloc_large_node_noprof(size, flags, node);
		trace_kmalloc(caller, ret, size,
			      PAGE_SIZE << get_order(size), flags, node);
		return ret;
	}

	if (unlikely(!size))
		return ZERO_SIZE_PTR;

	s = kmalloc_slab(size, b, flags, caller);

	ret = slab_alloc_node(s, NULL, flags, node, caller, size);
	ret = kasan_kmalloc(s, ret, size, flags);
	trace_kmalloc(caller, ret, size, s->size, flags, node);
	return ret;
}
```

参数 `b`（`kmem_buckets`）对应 `CONFIG_SLAB_BUCKETS`（v6.11 引入）：子系统可以用 `kmem_buckets_create()` 创建一组私有的 kmalloc 缓存，把容易被用户控制大小和内容的分配（如 `msg_msg`、xattr）隔离出去。

## 关键细节

### 缓存合并

`kmem_cache_create()` 创建新缓存时，`__kmem_cache_alias()`（[`mm/slab_common.c:264`](../../linux/mm/slab_common.c#L264)）会尝试找一个对象大小、对齐、标志兼容的现有缓存与之**合并**（`CONFIG_SLAB_MERGE_DEFAULT=y`），只增加引用计数并在 sysfs 中创建别名。这减少了 slab 数量和内存碎片，但也意味着不同子系统的对象可能共享 slab。带 `SLAB_NO_MERGE`、`SLAB_TYPESAFE_BY_RCU`、有构造函数或启用调试的缓存不会被合并；启动参数 `slab_nomerge` 可全局关闭合并。

### 调试

`CONFIG_SLUB_DEBUG=y` 编译了调试代码，但默认关闭（`CONFIG_SLUB_DEBUG_ON` 未设置）。启动参数 `slab_debug=FZPU,<cache>` 可以为指定缓存打开一致性检查（F）、红区（Z）、毒化（P）、调用者跟踪（U）。启用调试的缓存不走 per-CPU 快速路径，所有分配释放都经过 `alloc_single_from_partial()` / `free_to_partial_list()`，并持 `list_lock`。

## 动手实验

查看系统中的 slab 缓存：

```bash
sudo cat /proc/slabinfo | head -20
sudo slabtop -o -s c | head -20        # 按占用内存排序
ls /sys/kernel/slab/ | head            # 每个缓存一个目录；合并的缓存为符号链接
cat /sys/kernel/slab/kmalloc-64/{object_size,slab_size,objs_per_slab,order,cpu_partial,min_partial}
ls -l /sys/kernel/slab/ | grep -- '->' | head   # 查看被合并的缓存别名
```

用 bpftrace 统计 kmalloc 请求的大小分布与调用者（root）：

```bash
sudo bpftrace -e 'tracepoint:kmem:kmalloc {
  @bytes = hist(args->bytes_req);
  @waste = sum(args->bytes_alloc - args->bytes_req);
}'
```

`bytes_alloc - bytes_req` 就是分级带来的内部碎片。

## 小结

- SLUB 是 v6.8 起唯一的 slab 实现，以"每缓存、每 CPU 一个当前 slab + 嵌入式空闲链表"为核心，slab 描述符复用 `struct page`。
- 快速路径用 `(freelist, tid)` 双字 `cmpxchg` 实现无锁、无需关抢占的分配与释放；`tid` 同时编码 CPU 和操作序号，解决迁移与 ABA 问题。
- 慢速路径依次尝试：当前 slab 的远程释放对象 → per-CPU partial → 节点 partial → 新 slab；新 slab 先以首选阶无回收地尝试，失败再退回最低阶。
- 释放时按 slab 状态（frozen / 满 / 部分空 / 全空）决定链表归属，满 slab 不在任何链表上。
- v6.18 新增可选的 sheaf 层，目前用于 maple tree 与 VMA 缓存。
- kmalloc 覆盖 8 B～8 KB，按 NORMAL（含 15 份随机副本）、RECLAIM、DMA、CGROUP 分组；更大的请求直接走伙伴系统。空闲指针混淆、链表随机化、随机 kmalloc 缓存共同加固堆安全。

## 参考

- `linux/Documentation/mm/slab.rst`
- `linux/Documentation/admin-guide/mm/slab.rst`（`slab_debug` 等启动参数）
- Christoph Lameter, "SLUB: The unqueued slab allocator V6", LKML, 2007
- Vlastimil Babka, "SLUB percpu sheaves" 补丁系列（v6.18 合入）
