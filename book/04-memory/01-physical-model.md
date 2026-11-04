# 4.1 物理内存模型：node、zone、page 与 folio

> **本节源码**：`include/linux/mmzone.h`、`include/linux/mm_types.h`、`include/linux/page-flags.h`、`include/linux/page-flags-layout.h`、`include/asm-generic/memory_model.h`、`include/linux/gfp.h`、`arch/x86/mm/init.c`、`arch/x86/include/asm/page_64_types.h`
> **关键函数**：`zone_sizes_init()`、`gfp_zone()`、`node_zonelist()`、`page_zone()`、`page_folio()`、`pfn_to_page()`
> **前置知识**：操作系统中的分页、物理地址与虚拟地址的概念

## 概述

内核管理物理内存时，要同时面对三类约束：**访问延迟**不均匀（NUMA 机器上访问远端内存更慢），**设备寻址能力**有限（老式 ISA 设备只能对低 16 MB 做 DMA，很多 PCI 设备只能访问 4 GB 以下），以及**元数据规模**巨大（每 4 KB 物理页都需要一个描述符）。

Linux 为此建立了三层抽象：**节点（Node）** 对应一组访问延迟相同的内存，用 `pglist_data` 描述；节点内部按地址范围划分为若干**内存域（Zone）**，用 `struct zone` 描述；最底层每个物理页帧（Page Frame）对应一个 `struct page`。自 v5.16 起，又在 `struct page` 之上引入了 **folio**，用来表示"一段物理连续、作为整体管理的页"。

本节逐层分析这些数据结构，并说明在 x86_64 上如何在物理页帧号（Page Frame Number，PFN）、`struct page` 和内核虚拟地址之间互相转换。后续各节的伙伴系统、页面回收、缺页处理都建立在这套模型之上。

```
             pglist_data (node 0)                  pglist_data (node 1)
 ┌──────────────────────────────────────┐   ┌──────────────────────────┐
 │ node_zones[]                         │   │ node_zones[]             │
 │  ┌────────┐┌────────┐┌────────────┐  │   │  ┌────────────┐          │
 │  │ZONE_DMA││ DMA32  ││ ZONE_NORMAL│..│   │  │ ZONE_NORMAL│ ...      │
 │  └───┬────┘└────────┘└─────┬──────┘  │   │  └────────────┘          │
 │      │ free_area[0..10]    │         │   │                          │
 │ node_zonelists[]  kswapd  __lruvec   │   │                          │
 └──────┼─────────────────────┼─────────┘   └──────────────────────────┘
        ▼                     ▼
   struct page ... struct page ...   （vmemmap 虚拟连续数组，按 PFN 下标访问）
```

*图 4-1：node、zone 与 page 的层次关系*

## NUMA 节点：`pglist_data`

### 为什么需要节点

在多路服务器上，每个 CPU 插槽都有自己的内存控制器。CPU 访问本插槽内存（本地内存）比访问其他插槽内存（远端内存）快。为了让分配器优先使用本地内存，内核把每组"距离相同"的内存抽象为一个节点。本书配置启用了 `CONFIG_NUMA=y`，`CONFIG_NODES_SHIFT=10`，最多支持 1024 个节点；单路机器上则只有节点 0。

节点描述符通过 `NODE_DATA()` 访问：

```c
// include/linux/numa.h:25-26（v6.18.60）
extern struct pglist_data *node_data[];
#define NODE_DATA(nid)	(node_data[nid])
```

### 结构体字段

`pglist_data`（又名 `pg_data_t`）有上百行，这里只保留与本章相关的字段：

```c
// include/linux/mmzone.h:1385-1525（v6.18.60，节选，删去注释）
typedef struct pglist_data {
	struct zone node_zones[MAX_NR_ZONES];		// [注] 本节点的所有 zone
	struct zonelist node_zonelists[MAX_ZONELISTS];	// [注] 分配时的 zone 回退顺序
	int nr_zones; /* number of populated zones in this node */
	...
	unsigned long node_start_pfn;			// [注] 起始 PFN
	unsigned long node_present_pages; /* total number of physical pages */
	unsigned long node_spanned_pages; /* total size of physical page
					     range, including holes */
	int node_id;
	wait_queue_head_t kswapd_wait;			// [注] kswapd 在此睡眠
	wait_queue_head_t pfmemalloc_wait;
	...
	struct task_struct *kswapd;	/* Protected by kswapd_lock */
	int kswapd_order;
	enum zone_type kswapd_highest_zoneidx;
	...
	struct task_struct *kcompactd;			// [注] 后台规整线程（CONFIG_COMPACTION）
	...
	unsigned long		totalreserve_pages;
	...
	struct lruvec		__lruvec;		// [注] 无 memcg 时的 LRU 链表
	...
	atomic_long_t		vm_stat[NR_VM_NODE_STAT_ITEMS];
	...
} pg_data_t;
```

值得注意的是 LRU 链表（`__lruvec`）和 kswapd 都是**按节点**组织的，而不是按 zone。这是 v4.8 的"node-based LRU"改造的结果：之前每个 zone 有自己的 LRU，导致同一节点不同 zone 的老化速度不一致。启用 `CONFIG_MEMCG` 后，实际使用的是每个 memcg 在每个节点上的 `lruvec`（见 4.14 节），`__lruvec` 字段注释中已说明这一点。

## 内存域：`struct zone`

### zone 的类型

```c
// include/linux/mmzone.h:784-877（v6.18.60，删去长注释）
enum zone_type {
#ifdef CONFIG_ZONE_DMA
	ZONE_DMA,
#endif
#ifdef CONFIG_ZONE_DMA32
	ZONE_DMA32,
#endif
	ZONE_NORMAL,
#ifdef CONFIG_HIGHMEM
	ZONE_HIGHMEM,
#endif
	ZONE_MOVABLE,
#ifdef CONFIG_ZONE_DEVICE
	ZONE_DEVICE,
#endif
	__MAX_NR_ZONES

};
```

在本书配置下（`CONFIG_ZONE_DMA=y`、`CONFIG_ZONE_DMA32=y`、`CONFIG_ZONE_DEVICE=y`，x86_64 没有 `CONFIG_HIGHMEM`），共有 5 种 zone：

| zone | x86_64 物理地址范围 | 用途 |
|---|---|---|
| `ZONE_DMA` | 0 ～ 16 MB | 只能寻址 24 位的 ISA 设备 |
| `ZONE_DMA32` | 16 MB ～ 4 GB | 只能寻址 32 位的设备 |
| `ZONE_NORMAL` | 4 GB 以上 | 常规内存 |
| `ZONE_MOVABLE` | 由 `kernelcore=` / `movablecore=` 或内存热插拔划定 | 只放可迁移页，便于内存下线和形成大页 |
| `ZONE_DEVICE` | 设备内存（持久内存、GPU 显存等） | 不进入伙伴系统，仅提供 `struct page` |

x86_64 的边界由 `zone_sizes_init()` 设定：

```c
// arch/x86/mm/init.c:1000-1018（v6.18.60）
void __init zone_sizes_init(void)
{
	unsigned long max_zone_pfns[MAX_NR_ZONES];

	memset(max_zone_pfns, 0, sizeof(max_zone_pfns));

#ifdef CONFIG_ZONE_DMA
	max_zone_pfns[ZONE_DMA]		= min(MAX_DMA_PFN, max_low_pfn);
#endif
#ifdef CONFIG_ZONE_DMA32
	max_zone_pfns[ZONE_DMA32]	= min(MAX_DMA32_PFN, max_low_pfn);
#endif
	max_zone_pfns[ZONE_NORMAL]	= max_low_pfn;
#ifdef CONFIG_HIGHMEM
	max_zone_pfns[ZONE_HIGHMEM]	= max_pfn;
#endif

	free_area_init(max_zone_pfns);
}
```

`MAX_DMA_PFN` 与 `MAX_DMA32_PFN` 分别是 16 MB 和 4 GB 对应的 PFN（`arch/x86/include/asm/dma.h:74`、`:77`）。数组记录的是每个 zone 的**结束** PFN，`free_area_init()` 据此计算各 zone 的起止范围（4.2 节）。可见，内存不足 4 GB 的虚拟机上 `ZONE_NORMAL` 为空，绝大部分内存都在 `ZONE_DMA32` 中。

### 结构体字段

`struct zone` 用 `CACHELINE_PADDING()` 分为三个区域：读多写少的字段、分配器频繁写的字段、规整与统计频繁写的字段，避免伪共享。

```c
// include/linux/mmzone.h:879-1013（v6.18.60，节选，删去注释）
struct zone {
	/* Read-mostly fields */

	/* zone watermarks, access with *_wmark_pages(zone) macros */
	unsigned long _watermark[NR_WMARK];		// [注] min/low/high/promo 水位线
	unsigned long watermark_boost;			// [注] 碎片化时临时抬高水位
	...
	long lowmem_reserve[MAX_NR_ZONES];		// [注] 为低端 zone 保留的页数
#ifdef CONFIG_NUMA
	int node;
#endif
	struct pglist_data	*zone_pgdat;
	struct per_cpu_pages	__percpu *per_cpu_pageset;	// [注] per-CPU 页缓存（4.3）
	...
	/* zone_start_pfn == zone_start_paddr >> PAGE_SHIFT */
	unsigned long		zone_start_pfn;
	...
	atomic_long_t		managed_pages;		// [注] 由伙伴系统管理的页数
	unsigned long		spanned_pages;		// [注] 跨度，含空洞
	unsigned long		present_pages;		// [注] 实际存在的页数
	...
	const char		*name;
	...
	/* Write-intensive fields used from the page allocator */
	CACHELINE_PADDING(_pad1_);

	/* free areas of different sizes */
	struct free_area	free_area[NR_PAGE_ORDERS];	// [注] 伙伴系统空闲链表
	...
	/* zone flags, see below */
	unsigned long		flags;

	/* Primarily protects free_area */
	spinlock_t		lock;
	...
```

三个"页数"字段的关系在源码注释中写得很清楚：

```
spanned_pages = zone_end_pfn - zone_start_pfn         （包含空洞）
present_pages = spanned_pages - absent_pages          （去掉空洞）
managed_pages = present_pages - reserved_pages        （去掉启动期保留页）
```

水位线、`lowmem_reserve` 等阈值都基于 `managed_pages` 计算（4.4 节）。

### 伙伴系统的空闲链表

`free_area[]` 是伙伴系统的核心，每个阶（order）一项，每项再按迁移类型分成多条链表：

```c
// include/linux/mmzone.h:138-141（v6.18.60）
struct free_area {
	struct list_head	free_list[MIGRATE_TYPES];
	unsigned long		nr_free;
};
```

最大阶由 `MAX_PAGE_ORDER` 决定，x86_64 上未定义 `CONFIG_ARCH_FORCE_MAX_ORDER`，取默认值 10，即一次最多分配 2^10 × 4 KB = 4 MB 连续物理内存：

```c
// include/linux/mmzone.h:29-38（v6.18.60）
#ifndef CONFIG_ARCH_FORCE_MAX_ORDER
#define MAX_PAGE_ORDER 10
#else
#define MAX_PAGE_ORDER CONFIG_ARCH_FORCE_MAX_ORDER
#endif
#define MAX_ORDER_NR_PAGES (1 << MAX_PAGE_ORDER)

#define IS_MAX_ORDER_ALIGNED(pfn) IS_ALIGNED(pfn, MAX_ORDER_NR_PAGES)

#define NR_PAGE_ORDERS (MAX_PAGE_ORDER + 1)
```

注意 v6.4 之前宏名为 `MAX_ORDER`，含义是"最大阶 + 1"；v6.4 将其改为闭区间上界，v6.8 又重命名为 `MAX_PAGE_ORDER` 并引入 `NR_PAGE_ORDERS`。阅读旧资料时需留意这一差异。

迁移类型（Migrate Type）把物理内存按"页能否被移动"分组，是抗碎片化的基础：

```c
// include/linux/mmzone.h:64-87（v6.18.60，删去注释）
enum migratetype {
	MIGRATE_UNMOVABLE,
	MIGRATE_MOVABLE,
	MIGRATE_RECLAIMABLE,
	MIGRATE_PCPTYPES,	/* the number of types on the pcp lists */
	MIGRATE_HIGHATOMIC = MIGRATE_PCPTYPES,
#ifdef CONFIG_CMA
	MIGRATE_CMA,
	__MIGRATE_TYPE_END = MIGRATE_CMA,
#else
	__MIGRATE_TYPE_END = MIGRATE_HIGHATOMIC,
#endif
#ifdef CONFIG_MEMORY_ISOLATION
	MIGRATE_ISOLATE,	/* can't allocate from here */
#endif
	MIGRATE_TYPES
};
```

迁移类型不是记录在单个页上，而是记录在**页块（pageblock）** 上：每 2^`pageblock_order` 个页共享一组标志位（本书配置 `CONFIG_PAGE_BLOCK_MAX_ORDER=10`，启用 `CONFIG_HUGETLB_PAGE` 时 `pageblock_order` 取 `MIN_T(unsigned int, HUGETLB_PAGE_ORDER, PAGE_BLOCK_MAX_ORDER)`（`include/linux/pageblock-flags.h:60`），x86_64 上为 9，对应 2 MB）。4.3 节会分析分配器如何在迁移类型之间"借用"页块。

### zonelist：分配回退顺序

一次分配请求首先确定"最高可用 zone"，然后沿着 zonelist 从高到低、从近到远依次尝试。zonelist 中的每一项是 `zoneref`，缓存了 zone 下标以减少解引用：

```c
// include/linux/mmzone.h:1298-1335（v6.18.60，节选）
enum {
	ZONELIST_FALLBACK,	/* zonelist with fallback */
#ifdef CONFIG_NUMA
	ZONELIST_NOFALLBACK,	/* zonelist without fallback (__GFP_THISNODE) */
#endif
	MAX_ZONELISTS
};

struct zoneref {
	struct zone *zone;	/* Pointer to actual zone */
	int zone_idx;		/* zone_idx(zoneref->zone) */
};
...
struct zonelist {
	struct zoneref _zonerefs[MAX_ZONES_PER_ZONELIST + 1];
};
```

每个节点有两份 zonelist：`ZONELIST_FALLBACK` 包含所有节点的 zone，允许跨节点回退；`ZONELIST_NOFALLBACK` 只包含本节点的 zone，供 `__GFP_THISNODE` 使用。

## 从 GFP 标志到 zone

调用者通过 GFP（Get Free Pages）标志表达"能从哪些 zone 分配、能否睡眠、能否回收"等约束。常见组合定义在 `gfp_types.h`：

```c
// include/linux/gfp_types.h:377-390（v6.18.60）
#define GFP_ATOMIC	(__GFP_HIGH|__GFP_KSWAPD_RECLAIM)
#define GFP_KERNEL	(__GFP_RECLAIM | __GFP_IO | __GFP_FS)
#define GFP_KERNEL_ACCOUNT (GFP_KERNEL | __GFP_ACCOUNT)
#define GFP_NOWAIT	(__GFP_KSWAPD_RECLAIM | __GFP_NOWARN)
#define GFP_NOIO	(__GFP_RECLAIM)
#define GFP_NOFS	(__GFP_RECLAIM | __GFP_IO)
#define GFP_USER	(__GFP_RECLAIM | __GFP_IO | __GFP_FS | __GFP_HARDWALL)
#define GFP_DMA		__GFP_DMA
#define GFP_DMA32	__GFP_DMA32
#define GFP_HIGHUSER	(GFP_USER | __GFP_HIGHMEM)
#define GFP_HIGHUSER_MOVABLE	(GFP_HIGHUSER | __GFP_MOVABLE | __GFP_SKIP_KASAN)
#define GFP_TRANSHUGE_LIGHT	((GFP_HIGHUSER_MOVABLE | __GFP_COMP | \
			 __GFP_NOMEMALLOC | __GFP_NOWARN) & ~__GFP_RECLAIM)
#define GFP_TRANSHUGE	(GFP_TRANSHUGE_LIGHT | __GFP_DIRECT_RECLAIM)
```

其中 `__GFP_DMA`、`__GFP_DMA32`、`__GFP_HIGHMEM`、`__GFP_MOVABLE` 四位是 **zone 修饰符**。`gfp_zone()` 把这四位当作下标，查一张编码在常量 `GFP_ZONE_TABLE` 里的表，得到允许使用的最高 zone：

```c
// include/linux/gfp.h:156-165（v6.18.60）
static inline enum zone_type gfp_zone(gfp_t flags)
{
	enum zone_type z;
	int bit = (__force int) (flags & GFP_ZONEMASK);

	z = (GFP_ZONE_TABLE >> (bit * GFP_ZONES_SHIFT)) &
					 ((1 << GFP_ZONES_SHIFT) - 1);
	VM_BUG_ON((GFP_ZONE_BAD >> bit) & 1);
	return z;
}
```

例如 `GFP_KERNEL` 不含 zone 修饰符，结果是 `ZONE_NORMAL`，分配器按 NORMAL → DMA32 → DMA 的顺序回退；`GFP_HIGHUSER_MOVABLE` 含 `__GFP_MOVABLE` 和 `__GFP_HIGHMEM`，结果是 `ZONE_MOVABLE`，可以使用所有 zone。用户态匿名页和 page cache 一般使用后者。

zonelist 的选择则由 `__GFP_THISNODE` 决定：

```c
// include/linux/gfp.h:174-181（v6.18.60）
static inline int gfp_zonelist(gfp_t flags)
{
#ifdef CONFIG_NUMA
	if (unlikely(flags & __GFP_THISNODE))
		return ZONELIST_NOFALLBACK;
#endif
	return ZONELIST_FALLBACK;
}
```

## 页描述符：`struct page`

### 结构体布局

每个物理页帧对应一个 `struct page`。由于数量巨大（16 GB 内存就有 400 万个），内核把它压缩到 64 字节（x86_64），并大量使用联合体让不同用途复用同一块空间：

```c
// include/linux/mm_types.h:78-125（v6.18.60，节选，删去部分注释）
struct page {
	memdesc_flags_t flags;		/* Atomic flags, some possibly
					 * updated asynchronously */
	/*
	 * Five words (20/40 bytes) are available in this union.
	 * WARNING: bit 0 of the first word is used for PageTail(). That
	 * means the other users of this union MUST NOT use the bit to
	 * avoid collision and false-positive PageTail().
	 */
	union {
		struct {	/* Page cache and anonymous pages */
			union {
				struct list_head lru;

				/* Or, free page */
				struct list_head buddy_list;
				struct list_head pcp_list;
				struct llist_node pcp_llist;
			};
			struct address_space *mapping;
			union {
				pgoff_t __folio_index;		/* Our offset within mapping. */
				unsigned long share;	/* share count for fsdax */
			};
			unsigned long private;
		};
		...
		struct {	/* Tail pages of compound page */
			unsigned long compound_head;	/* Bit zero is set */
		};
		...
```

```c
// include/linux/mm_types.h:153-221（v6.18.60，节选，删去注释）
	union {		/* This union is 4 bytes in size. */
		unsigned int page_type;
		atomic_t _mapcount;
	};

	/* Usage count. *DO NOT USE DIRECTLY*. See page_ref.h */
	atomic_t _refcount;

#ifdef CONFIG_MEMCG
	unsigned long memcg_data;
#elif defined(CONFIG_SLAB_OBJ_EXT)
	unsigned long _unused_slab_obj_exts;
#endif
	...
} _struct_page_alignment;
```

各字段的含义：

| 字段 | 大小 | 含义 |
|---|---|---|
| `flags` | 8 B | 低位是 `PG_*` 标志，高位编码 node、zone 等信息 |
| `lru` / `buddy_list` / `pcp_list` | 16 B | 在 LRU 链表、伙伴空闲链表或 per-CPU 链表上时的链表节点 |
| `mapping` | 8 B | page cache 页指向 `address_space`；匿名页指向 `anon_vma`，最低位置 1 以示区分 |
| `__folio_index` | 8 B | 页在文件（或匿名映射）中的偏移，以页为单位 |
| `private` | 8 B | 文件系统私有数据；在伙伴系统中记录该空闲块的阶 |
| `compound_head` | 8 B | 尾页指向头页，最低位置 1 |
| `_mapcount` / `page_type` | 4 B | 被页表映射的次数（初值 -1）；或者是 slab、页表等特殊类型的标记 |
| `_refcount` | 4 B | 引用计数，为 0 时页可被释放 |
| `memcg_data` | 8 B | 所属 memcg（`CONFIG_MEMCG`） |

合计 8 + 40 + 4 + 4 + 8 = 64 字节。`struct page` 占物理内存的比例是 64 / 4096 ≈ 1.56%。

`_refcount` 与 `_mapcount` 是两个经常混淆的计数：前者统计**所有**持有者（页表映射、page cache、`get_page()` 等），决定页何时释放；后者只统计**用户页表**映射次数，用于反向映射和回收判断（4.9 节）。

### `flags` 的布局

`flags` 的低位是 `enum pageflags` 定义的标志位：

```c
// include/linux/page-flags.h:93-113（v6.18.60）
enum pageflags {
	PG_locked,		/* Page is locked. Don't touch. */
	PG_writeback,		/* Page is under writeback */
	PG_referenced,
	PG_uptodate,
	PG_dirty,
	PG_lru,
	PG_head,		/* Must be in bit 6 */
	PG_waiters,		/* Page has waiters, check its waitqueue. Must be bit #7 and in the same byte as "PG_locked" */
	PG_active,
	PG_workingset,
	PG_owner_priv_1,	/* Owner use. If pagecache, fs may use */
	PG_owner_2,		/* Owner use. If pagecache, fs may use */
	PG_arch_1,
	PG_reserved,
	PG_private,		/* If pagecache, has fs-private data */
	PG_private_2,		/* If pagecache, has fs aux data */
	PG_reclaim,		/* To be reclaimed asap */
	PG_swapbacked,		/* Page is backed by RAM/swap */
	PG_unevictable,		/* Page is "unevictable"  */
	PG_dropbehind,		/* drop pages on IO completion */
```

高位则存放 node 号、zone 号以及（启用 MGLRU 时）代数信息等。`page-flags-layout.h` 的注释给出了几种布局，在 `CONFIG_SPARSEMEM_VMEMMAP` 下不需要存 section 号：

```c
// include/linux/page-flags-layout.h:44-50（v6.18.60）
 * No sparsemem or sparsemem vmemmap: |       NODE     | ZONE |             ... | FLAGS |
 *      " plus space for last_cpupid: |       NODE     | ZONE | LAST_CPUPID ... | FLAGS |
 * classic sparse with space for node:| SECTION | NODE | ZONE |             ... | FLAGS |
 *      " plus space for last_cpupid: | SECTION | NODE | ZONE | LAST_CPUPID ... | FLAGS |
 * classic sparse no space for node:  | SECTION |     ZONE    | ... | FLAGS |
```

于是从 `struct page` 找到所属 zone 只需要两次移位与掩码：

```c
// include/linux/mmzone.h:1187-1196（v6.18.60）
static inline enum zone_type memdesc_zonenum(memdesc_flags_t flags)
{
	ASSERT_EXCLUSIVE_BITS(flags.f, ZONES_MASK << ZONES_PGSHIFT);
	return (flags.f >> ZONES_PGSHIFT) & ZONES_MASK;
}

static inline enum zone_type page_zonenum(const struct page *page)
{
	return memdesc_zonenum(page->flags);
}
```

```c
// include/linux/mm.h:1909-1912（v6.18.60）
static inline struct zone *page_zone(const struct page *page)
{
	return &NODE_DATA(page_to_nid(page))->node_zones[page_zonenum(page)];
}
```

## folio：以"页组"为单位管理内存

### 为什么需要 folio

为了一次分配多个连续页（如 2 MB 透明大页），内核早就有**复合页（Compound Page）**：第一个页称为头页（Head Page），设置 `PG_head`；其余为尾页（Tail Page），其 `compound_head` 字段指向头页且最低位为 1。

问题在于：很多函数接收一个 `struct page *`，却无法知道它是头页、尾页还是普通页，只好在内部反复调用 `compound_head()`。这既浪费 CPU，又容易出错（对尾页设置标志位是常见 bug）。folio 的思路是**用类型系统区分"一组页的整体"和"其中某一页"**：`struct folio *` 保证指向的一定是头页（或者普通的单页），对它的操作天然作用于整体。

### 结构体布局

`struct folio` 与连续 4 个 `struct page` 在内存上重叠。第一部分与头页字段一一对应，之后三部分复用第 1～3 个尾页的空间存放大 folio 的元数据：

```c
// include/linux/mm_types.h:377-421（v6.18.60，节选）
struct folio {
	/* private: don't document the anon union */
	union {
		struct {
	/* public: */
			memdesc_flags_t flags;
			union {
				struct list_head lru;
				...
			};
			struct address_space *mapping;
			union {
				pgoff_t index;
				unsigned long share;
			};
			union {
				void *private;
				swp_entry_t swap;
			};
			atomic_t _mapcount;
			atomic_t _refcount;
#ifdef CONFIG_MEMCG
			unsigned long memcg_data;
			...
		};
		struct page page;
	};
```

```c
// include/linux/mm_types.h:423-453（v6.18.60，节选）
	union {
		struct {
			unsigned long _flags_1;
			unsigned long _head_1;
			union {
				struct {
	/* public: */
					atomic_t _large_mapcount;
					atomic_t _nr_pages_mapped;
#ifdef CONFIG_64BIT
					atomic_t _entire_mapcount;
					atomic_t _pincount;
#endif /* CONFIG_64BIT */
					...
				};
				unsigned long _usable_1[4];
			};
			atomic_t _mapcount_1;
			atomic_t _refcount_1;
	/* public: */
#ifdef NR_PAGES_IN_LARGE_FOLIO
			unsigned int _nr_pages;
#endif /* NR_PAGES_IN_LARGE_FOLIO */
	...
		};
		struct page __page_1;
	};
```

为了保证"重叠"严格成立，源码用一组 `static_assert` 检查偏移量：

```c
// include/linux/mm_types.h:484-492（v6.18.60）
#define FOLIO_MATCH(pg, fl)						\
	static_assert(offsetof(struct page, pg) == offsetof(struct folio, fl))
FOLIO_MATCH(flags, flags);
FOLIO_MATCH(lru, lru);
FOLIO_MATCH(mapping, mapping);
FOLIO_MATCH(compound_head, lru);
FOLIO_MATCH(__folio_index, index);
FOLIO_MATCH(private, private);
FOLIO_MATCH(_mapcount, _mapcount);
```

```
  struct page[0] (head)   │ flags │ lru │ mapping │ index │ private │ _mapcount │ _refcount │ memcg │  ← folio 第 1 部分
  struct page[1] (tail)   │ _flags_1 │ _head_1 │ _large_mapcount ... _pincount ... │ _nr_pages  │  ← folio 第 2 部分
  struct page[2] (tail)   │ _flags_2 │ _head_2 │ _deferred_list ...                            │  ← folio 第 3 部分
  struct page[3] (tail)   │ _flags_3 │ _head_3 │ _hugetlb_subpool ...                          │  ← folio 第 4 部分
  struct page[4..N-1]     │ ...      │ compound_head = head | 1 ...                            │
```

*图 4-2：大 folio 与 `struct page` 数组的重叠关系（地址自上而下递增）*

### page 与 folio 的转换

从任意页找到所属 folio 就是找到头页：

```c
// include/linux/page-flags.h:282-308（v6.18.60，节选）
static __always_inline unsigned long _compound_head(const struct page *page)
{
	unsigned long head = READ_ONCE(page->compound_head);

	if (unlikely(head & 1))
		return head - 1;
	return (unsigned long)page_fixed_fake_head(page);
}
...
#define page_folio(p)		(_Generic((p),				\
	const struct page *:	(const struct folio *)_compound_head(p), \
	struct page *:		(struct folio *)_compound_head(p)))
```

`page_fixed_fake_head()` 处理 HugeTLB vmemmap 优化（`CONFIG_HUGETLB_PAGE_OPTIMIZE_VMEMMAP`）后出现的"伪头页"，4.13 节会提到。folio 的大小通过 `folio_order()` / `folio_nr_pages()` 获得，单页 folio 的阶为 0。

在 v6.18 中，page cache、匿名内存、LRU、回收、迁移的主体代码都已改为以 folio 为单位操作；`struct page` 则逐步退化为"物理页帧的描述符"。本章后续统一使用"folio"指代作为整体管理的页组。

## 稀疏内存模型与 vmemmap

### 为什么不用一个大数组

最简单的做法是为所有物理页分配一个连续的 `struct page` 数组 `mem_map[]`，下标就是 PFN（`CONFIG_FLATMEM`）。但物理地址空间中存在大量空洞（如 3～4 GB 的 PCI 洞、NUMA 节点之间的间隙），而且支持热插拔的机器无法在启动时确定最大地址。为空洞分配 `struct page` 会浪费大量内存。

**稀疏内存模型（SPARSEMEM）** 把物理地址空间按固定大小切成段（section），x86_64 上每段 128 MB：

```c
// arch/x86/include/asm/sparsemem.h:27-29（v6.18.60）
#else /* CONFIG_X86_32 */
# define SECTION_SIZE_BITS	27 /* matt - 128 is convenient right now */
# define MAX_PHYSMEM_BITS	(pgtable_l5_enabled() ? 52 : 46)
```

每个段用一个 `mem_section` 描述，只有存在内存的段才分配 `struct page`。本书配置启用 `CONFIG_SPARSEMEM_EXTREME`，`mem_section` 本身也是二级动态分配的：

```c
// include/linux/mmzone.h:1937-1952（v6.18.60）
#ifdef CONFIG_SPARSEMEM_EXTREME
#define SECTIONS_PER_ROOT       (PAGE_SIZE / sizeof (struct mem_section))
#else
#define SECTIONS_PER_ROOT	1
#endif

#define SECTION_NR_TO_ROOT(sec)	((sec) / SECTIONS_PER_ROOT)
#define NR_SECTION_ROOTS	DIV_ROUND_UP(NR_MEM_SECTIONS, SECTIONS_PER_ROOT)
#define SECTION_ROOT_MASK	(SECTIONS_PER_ROOT - 1)

#ifdef CONFIG_SPARSEMEM_EXTREME
extern struct mem_section **mem_section;
#else
extern struct mem_section mem_section[NR_SECTION_ROOTS][SECTIONS_PER_ROOT];
#endif
```

### vmemmap：虚拟连续、物理稀疏

经典 SPARSEMEM 中，`pfn_to_page()` 需要先找到 section 再计算偏移，开销较大。`CONFIG_SPARSEMEM_VMEMMAP`（本书配置已启用）的做法是：在内核虚拟地址空间中预留一段区域 `vmemmap`，把**所有可能的** `struct page` 按 PFN 顺序排列在这段虚拟地址上，但只为真正存在内存的部分建立页表映射。这样转换就退化为指针运算：

```c
// include/asm-generic/memory_model.h:43-47（v6.18.60）
#elif defined(CONFIG_SPARSEMEM_VMEMMAP)

/* memmap is virtually contiguous.  */
#define __pfn_to_page(pfn)	(vmemmap + (pfn))
#define __page_to_pfn(page)	(unsigned long)((page) - vmemmap)
```

x86_64 上 `vmemmap` 的基址是 `vmemmap_base`，在四级页表下默认为 `0xffffea0000000000`（`arch/x86/include/asm/pgtable_64_types.h:113`），开启 `CONFIG_RANDOMIZE_MEMORY` 后会在启动时随机化（4.6 节）。

## 地址转换小结

内核中常用的几种地址/描述符之间的转换关系如下：

```
          __pa() / virt_to_phys()               PHYS_PFN()
  内核直接映射虚拟地址 ─────────────────▶ 物理地址 ──────────────▶ PFN
         ▲            ◀─────────────────        ◀──────────────   │ ▲
         │              __va() / phys_to_virt()      PFN_PHYS()   │ │
         │                                                         ▼ │
         └──────────── page_address() ◀────────────── struct page ◀──┘
                                                 pfn_to_page() / page_to_pfn()
```

*图 4-3：虚拟地址、物理地址、PFN 与 `struct page` 的转换*

x86_64 把全部物理内存线性映射在 `PAGE_OFFSET`（即 `page_offset_base`，四级页表下默认 `0xffff888000000000`）开始的区域，称为**直接映射区（Direct Map）**：

```c
// arch/x86/include/asm/page.h:58（v6.18.60）
#define __va(x)			((void *)((unsigned long)(x)+PAGE_OFFSET))
```

反方向的 `__pa()` 稍复杂，因为内核镜像本身还被映射在 `__START_KERNEL_map`（`0xffffffff80000000`）开始的区域，需要区分两种来源：

```c
// arch/x86/include/asm/page_64.h:22-30（v6.18.60）
static __always_inline unsigned long __phys_addr_nodebug(unsigned long x)
{
	unsigned long y = x - __START_KERNEL_map;

	/* use the carry flag to determine if x was < __START_KERNEL_map */
	x = y + ((x > y) ? phys_base : (__START_KERNEL_map - PAGE_OFFSET));

	return x;
}
```

若 `x ≥ __START_KERNEL_map`，减法不借位，`x > y` 成立，物理地址为 `x - __START_KERNEL_map + phys_base`；否则说明 `x` 在直接映射区，物理地址为 `x - PAGE_OFFSET`。一次减法加一次比较即可完成判断。

## 动手实验

以下命令在 dev 虚拟机中以普通用户运行即可。

查看节点与 zone 概况：

```bash
# 每个 zone 的水位线、managed/present/spanned 页数、per-CPU 缓存参数
cat /proc/zoneinfo | grep -E "^Node|pages free|min|low|high|spanned|present|managed"

# 每个 zone 各阶空闲块数量（从左到右为 order 0～10）
cat /proc/buddyinfo

# 按迁移类型细分的空闲块，以及每个 zone 各迁移类型的页块数
sudo cat /proc/pagetypeinfo
```

在内存小于 4 GB 的虚拟机上，可以观察到 `Normal` zone 很小甚至不存在，大部分内存属于 `DMA32`。

查看 `struct page` 的大小和 vmemmap 所占内存（需要内核调试信息，例如安装 `linux-image-*-dbgsym` 后使用 `pahole`）：

```bash
pahole -C page /usr/lib/debug/boot/vmlinux-$(uname -r) | tail -3
pahole -C folio /usr/lib/debug/boot/vmlinux-$(uname -r) | tail -3
```

## 小结

- 物理内存按"节点 → zone → 页"三层组织：节点反映 NUMA 距离，zone 反映设备寻址限制，`struct page` 描述单个页帧。
- x86_64 上有 `ZONE_DMA`（< 16 MB）、`ZONE_DMA32`（< 4 GB）、`ZONE_NORMAL`、`ZONE_MOVABLE`、`ZONE_DEVICE`，边界由 `zone_sizes_init()` 设定。
- 每个 zone 的 `free_area[0..MAX_PAGE_ORDER]` 按阶和迁移类型组织空闲页，是伙伴系统的基础；LRU 与 kswapd 则按节点组织。
- GFP 标志中的 zone 修饰符经 `gfp_zone()` 查表得到最高可用 zone，`__GFP_THISNODE` 决定是否允许跨节点回退。
- `struct page` 在 x86_64 上为 64 字节，靠联合体复用字段；`flags` 高位编码 node 与 zone。
- folio 用类型区分"页组整体"和"单个页"，v6.18 中内存管理的主体代码都已以 folio 为操作单位。
- `CONFIG_SPARSEMEM_VMEMMAP` 让 `pfn_to_page()` 变为一次指针加法，直接映射区让 `__va()` 变为一次加法。

## 参考

- `linux/Documentation/mm/physical_memory.rst`
- `linux/Documentation/mm/memory-model.rst`
- `linux/Documentation/arch/x86/x86_64/mm.rst`
- Jonathan Corbet, "Clarifying memory management with page folios", LWN: https://lwn.net/Articles/849538/
