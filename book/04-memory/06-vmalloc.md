# 4.6 内核虚拟地址空间与 vmalloc

> **本节源码**：[`Documentation/arch/x86/x86_64/mm.rst`](../../linux/Documentation/arch/x86/x86_64/mm.rst)、[`arch/x86/include/asm/pgtable_64_types.h`](../../linux/arch/x86/include/asm/pgtable_64_types.h)、[`arch/x86/include/asm/page_64_types.h`](../../linux/arch/x86/include/asm/page_64_types.h)、[`arch/x86/mm/kaslr.c`](../../linux/arch/x86/mm/kaslr.c)、[`arch/x86/mm/init_64.c`](../../linux/arch/x86/mm/init_64.c)、[`mm/vmalloc.c`](../../linux/mm/vmalloc.c)、[`include/linux/vmalloc.h`](../../linux/include/linux/vmalloc.h)、[`mm/slub.c`](../../linux/mm/slub.c)
> **关键函数**：`kernel_randomize_memory()`、`__vmalloc_node_range_noprof()`、`__get_vm_area_node()`、`alloc_vmap_area()`、`__vmalloc_area_node()`、`vfree()`、`free_vmap_area_noflush()`、`__kvmalloc_node_noprof()`
> **前置知识**：4.1 物理内存模型、4.3 伙伴系统

## 概述

伙伴系统和 slab 分配的内存都位于直接映射区，**物理连续**。但很多场景只需要虚拟连续：加载内核模块、为每个进程分配内核栈（`CONFIG_VMAP_STACK`）、BPF 程序、大型哈希表等。对这些场景坚持物理连续会让大块分配在碎片化后频繁失败。

`vmalloc()` 的做法是：在内核虚拟地址空间中专门划出一段区域，分配时先在这段区域里找一块空闲的**虚拟地址区间**，再逐页从伙伴系统获取**物理上不连续**的页，最后修改内核页表把它们映射到这段连续的虚拟地址上。代价是需要额外的页表项、TLB 覆盖更差，以及释放时需要 TLB 刷新。

本节首先梳理 x86_64 内核虚拟地址空间的整体布局与 KASLR 随机化，然后分析 vmalloc 的分配、映射与延迟释放，最后介绍 `kvmalloc()` 这种"先尝试物理连续、失败再回退"的折中接口。

## x86_64 虚拟地址空间布局

### 规范地址与用户/内核划分

x86_64 在四级页表下使用 48 位虚拟地址，五级页表（LA57）下使用 57 位。地址的高位必须是最高有效位的符号扩展，称为**规范地址（Canonical Address）**。这自然地把 64 位空间分成用户空间（低半部分）和内核空间（高半部分），中间是一个巨大的非规范空洞。

[`arch/x86/include/asm/page_64_types.h:41-54`](../../linux/arch/x86/include/asm/page_64_types.h#L41-L54)（v6.18.60，节选）

```c
#define __PAGE_OFFSET_BASE_L5	_AC(0xff11000000000000, UL)
#define __PAGE_OFFSET_BASE_L4	_AC(0xffff888000000000, UL)
...
#define __PAGE_OFFSET           page_offset_base
...
#define __START_KERNEL_map	_AC(0xffffffff80000000, UL)
...
#define __PHYSICAL_MASK_SHIFT	52
#define __VIRTUAL_MASK_SHIFT	(pgtable_l5_enabled() ? 56 : 47)

#define TASK_SIZE_MAX		task_size_max()
#define DEFAULT_MAP_WINDOW	((1UL << 47) - PAGE_SIZE)
```

本书配置的 `CONFIG_PGTABLE_LEVELS=5`：v6.18 中 x86_64 总是编译五级页表支持，在启动时根据 CPU 是否支持 LA57（以及是否传入 `no5lvl`）决定实际使用四级还是五级，`pgtable_l5_enabled()` 返回运行时结果（[`arch/x86/include/asm/pgtable_64_types.h:31-34`](../../linux/arch/x86/include/asm/pgtable_64_types.h#L31-L34)）。下文以最常见的四级页表为例。

### 布局总览

内核文档 `Documentation/arch/x86/x86_64/mm.rst` 给出了完整的布局。四级页表下的主要区域如下（地址自上而下递增）：

```
 起始地址               大小      区域
 0000000000000000     ~128 TB   用户空间（每个 mm 不同）
 00007ffffffff000        4 KB   保护空洞
 0000800000000000      ~16M TB  非规范地址空洞
 ─────────────────────────────── 以下为内核空间，所有进程共享 ───────────────
 ffff800000000000        8 TB   保护空洞（为 hypervisor 保留）
 ffff880000000000      0.5 TB   PTI 使用的 LDT 重映射
 ffff888000000000       64 TB   直接映射区（page_offset_base）
 ffffc88000000000      0.5 TB   未使用
 ffffc90000000000       32 TB   vmalloc/ioremap 区（vmalloc_base）
 ffffe90000000000        1 TB   未使用
 ffffea0000000000        1 TB   vmemmap（vmemmap_base）
 ffffeb0000000000        1 TB   未使用
 ffffec0000000000       16 TB   KASAN 影子内存
 fffffc0000000000        2 TB   未使用（KASLR 的 vaddr_end）
 fffffe0000000000      0.5 TB   cpu_entry_area
 ffffff0000000000      0.5 TB   %esp fixup 栈（espfix64）
 ffffffef00000000       64 GB   EFI 运行时服务映射
 ffffffff80000000      512 MB   内核代码段映射（__START_KERNEL_map）
 ffffffffa0000000     1520 MB   模块映射区（MODULES_VADDR）
 ffffffffff600000        4 KB   传统 vsyscall
```

*图 4-10：x86_64 四级页表下的虚拟地址空间布局（摘自 `Documentation/arch/x86/x86_64/mm.rst`）*

对应的宏定义：

[`arch/x86/include/asm/pgtable_64_types.h:107-118`](../../linux/arch/x86/include/asm/pgtable_64_types.h#L107-L118)（v6.18.60）

```c
#define __VMALLOC_BASE_L4	0xffffc90000000000UL
#define __VMALLOC_BASE_L5 	0xffa0000000000000UL

#define VMALLOC_SIZE_TB_L4	32UL
#define VMALLOC_SIZE_TB_L5	12800UL

#define __VMEMMAP_BASE_L4	0xffffea0000000000UL
#define __VMEMMAP_BASE_L5	0xffd4000000000000UL

# define VMALLOC_START		vmalloc_base
# define VMALLOC_SIZE_TB	(pgtable_l5_enabled() ? VMALLOC_SIZE_TB_L5 : VMALLOC_SIZE_TB_L4)
# define VMEMMAP_START		vmemmap_base
```

几个值得注意的区域：

- **直接映射区**：线性映射所有物理内存，`__va()` / `__pa()` 在此区域内是简单的加减法（4.1 节）。
- **内核代码段映射**：内核镜像被映射在 `-2 GB` 处，这样内核代码可以使用 `-mcmodel=kernel`，所有符号地址都能用 32 位有符号立即数表示。同一份物理内存在直接映射区中也有一份映射。
- **模块映射区**：紧接内核代码段，保证模块与内核之间的相对跳转在 ±2 GB 之内。v6.10 起模块内存由 `execmem` 子系统统一管理（`CONFIG_EXECMEM=y`）。
- **cpu_entry_area**：每 CPU 的 GDT、TSS、入口栈等，在启用 PTI（页表隔离）时也被映射到用户页表中，以便从用户态陷入内核时能找到入口栈（第 5 章）。

### KASLR 随机化

启用 `CONFIG_RANDOMIZE_MEMORY`（本书配置已启用）后，直接映射区、vmalloc 区、vmemmap 区这三个区域的基址在启动时随机化，它们的顺序保持不变：

[`arch/x86/mm/kaslr.c:48-63`](../../linux/arch/x86/mm/kaslr.c#L48-L63)（v6.18.60）

```c
static __initdata struct kaslr_memory_region {
	unsigned long *base;
	unsigned long *end;
	unsigned long size_tb;
} kaslr_regions[] = {
	{
		.base	= &page_offset_base,
		.end	= &direct_map_physmem_end,
	},
	{
		.base	= &vmalloc_base,
	},
	{
		.base	= &vmemmap_base,
	},
};
```

`kernel_randomize_memory()`（在 4.2 节所述 `setup_arch()` 中、`max_pfn` 确定之后调用）先计算三个区域各自需要的大小，把剩余空间作为熵在三个区域之间分配：

[`arch/x86/mm/kaslr.c:105-168`](../../linux/arch/x86/mm/kaslr.c#L105-L168)（v6.18.60，节选）

```c
	kaslr_regions[0].size_tb = 1 << (MAX_PHYSMEM_BITS - TB_SHIFT);
	kaslr_regions[1].size_tb = VMALLOC_SIZE_TB;
	...
	memory_tb = DIV_ROUND_UP(max_pfn << PAGE_SHIFT, 1UL << TB_SHIFT) +
		CONFIG_RANDOMIZE_MEMORY_PHYSICAL_PADDING;
	...
	if (!IS_ENABLED(CONFIG_ZONE_DEVICE) && (memory_tb < kaslr_regions[0].size_tb))
		kaslr_regions[0].size_tb = memory_tb;
	...
	vmemmap_size = (kaslr_regions[0].size_tb << (TB_SHIFT - PAGE_SHIFT)) *
			sizeof(struct page);
	kaslr_regions[2].size_tb = DIV_ROUND_UP(vmemmap_size, 1UL << TB_SHIFT);

	/* Calculate entropy available between regions */
	remain_entropy = vaddr_end - vaddr_start;
	for (i = 0; i < ARRAY_SIZE(kaslr_regions); i++)
		remain_entropy -= get_padding(&kaslr_regions[i]);

	prandom_seed_state(&rand_state, kaslr_get_random_long("Memory"));

	for (i = 0; i < ARRAY_SIZE(kaslr_regions); i++) {
		unsigned long entropy;

		/*
		 * Select a random virtual address using the extra entropy
		 * available.
		 */
		entropy = remain_entropy / (ARRAY_SIZE(kaslr_regions) - i);
		prandom_bytes_state(&rand_state, &rand, sizeof(rand));
		entropy = (rand % (entropy + 1)) & PUD_MASK;
		vaddr += entropy;
		*kaslr_regions[i].base = vaddr;
		...
		/* Add a minimum padding based on randomization alignment. */
		vaddr = round_up(vaddr + 1, PUD_SIZE);
		remain_entropy -= entropy;
	}
```

随机化粒度是 PUD（1 GB）。值得注意的是，本书配置启用了 `CONFIG_ZONE_DEVICE`，此时直接映射区必须保留完整的 `1 << MAX_PHYSMEM_BITS` 大小（因为设备内存可能出现在任意物理地址），可用于随机化的熵相应减少。因此在实际系统中看到的 `page_offset_base` 不一定是 `0xffff888000000000`。

## vmalloc 的数据结构

[`include/linux/vmalloc.h:52-85`](../../linux/include/linux/vmalloc.h#L52-L85)（v6.18.60）

```c
struct vm_struct {
	struct vm_struct	*next;
	void			*addr;
	unsigned long		size;
	unsigned long		flags;
	struct page		**pages;
#ifdef CONFIG_HAVE_ARCH_HUGE_VMALLOC
	unsigned int		page_order;
#endif
	unsigned int		nr_pages;
	phys_addr_t		phys_addr;
	const void		*caller;
	unsigned long		requested_size;
};

struct vmap_area {
	unsigned long va_start;
	unsigned long va_end;

	struct rb_node rb_node;         /* address sorted rbtree */
	struct list_head list;          /* address sorted list */

	/*
	 * The following two variables can be packed, because
	 * a vmap_area object can be either:
	 *    1) in "free" tree (root is free_vmap_area_root)
	 *    2) or "busy" tree (root is vmap_area_root)
	 */
	union {
		unsigned long subtree_max_size; /* in "free" tree */
		struct vm_struct *vm;           /* in "busy" tree */
	};
	unsigned long flags; /* mark type of vm_map_ram area */
};
```

两者的分工是：`vmap_area` 描述一段**虚拟地址区间**，是地址分配器的管理单元；`vm_struct` 描述"这段区间里映射了什么"，包括物理页数组 `pages[]`、标志（`VM_ALLOC`、`VM_IOREMAP`、`VM_MAP` 等）和调用者地址（`/proc/vmallocinfo` 显示的来源）。

`vmap_area` 同时位于以下几类树中的一种：

| 树 | 位置 | 内容 |
|---|---|---|
| 空闲树 | 全局 `free_vmap_area_root` | 空闲区间，按地址排序的**增强红黑树**，节点额外维护子树中的最大空闲长度 `subtree_max_size` |
| busy 树 | 每个 `vmap_node` 的 `busy` | 已分配区间，用于 `find_vmap_area()` 由地址查找 |
| lazy 树 | 每个 `vmap_node` 的 `lazy` | 已解除映射、等待批量刷新 TLB 后回收的区间 |

### vmap_node：分散锁竞争

早期 vmalloc 只有一把全局锁 `vmap_area_lock`，在高并发场景下成为瓶颈。v6.9 引入了 **vmap_node**：

[`mm/vmalloc.c:938-954`](../../linux/mm/vmalloc.c#L938-L954)（v6.18.60）

```c
static struct vmap_node {
	/* Simple size segregated storage. */
	struct vmap_pool pool[MAX_VA_SIZE_PAGES];
	spinlock_t pool_lock;
	bool skip_populate;

	/* Bookkeeping data of this node. */
	struct rb_list busy;
	struct rb_list lazy;

	/*
	 * Ready-to-free areas.
	 */
	struct list_head purge_list;
	struct work_struct purge_work;
	unsigned long nr_purged;
} single;
```

这里的"node"与 NUMA 节点无关，而是把 vmalloc 地址空间按固定粒度（`vmap_zone_size`，16 页）交错地划分给若干个逻辑分区，分区数为可能的 CPU 数（限制在 1～128 之间），由 `vmalloc_init()` 调用的 `vmap_init_nodes()` 确定（[`mm/vmalloc.c:5227-5234`](../../linux/mm/vmalloc.c#L5227-L5234)）。地址到分区的映射是取模运算：

[`mm/vmalloc.c:970-979`](../../linux/mm/vmalloc.c#L970-L979)（v6.18.60）

```c
static inline unsigned int
addr_to_node_id(unsigned long addr)
{
	return (addr / vmap_zone_size) % nr_vmap_nodes;
}

static inline struct vmap_node *
addr_to_node(unsigned long addr)
{
	return &vmap_nodes[addr_to_node_id(addr)];
}
```

每个分区有独立的 busy/lazy 树和锁。此外 `pool[]` 按大小（以页为单位，最多 `MAX_VA_SIZE_PAGES`）缓存了最近释放的区间，小尺寸分配可以直接从池中取用，完全绕过全局空闲树。

## 分配路径

### 调用链总览

```
vmalloc(size)                                    mm/vmalloc.c:4021
└── __vmalloc_node_noprof()
    └── __vmalloc_node_range_noprof()            mm/vmalloc.c:3849
        ├── __get_vm_area_node()                 mm/vmalloc.c:3194  // 分配虚拟区间
        │   ├── kzalloc_node(vm_struct)
        │   └── alloc_vmap_area()                mm/vmalloc.c:2031
        │       ├── node_alloc()                 //   从 vmap_node 池中取
        │       └── __alloc_vmap_area()          //   从全局空闲树中找
        └── __vmalloc_area_node()                mm/vmalloc.c:3708  // 分配物理页并映射
            ├── vm_area_alloc_pages()            //   批量从伙伴系统取页
            └── vmap_pages_range()               //   填写内核页表
```

### 入口

[`mm/vmalloc.c:3849-3929`](../../linux/mm/vmalloc.c#L3849-L3929)（v6.18.60，节选）

```c
void *__vmalloc_node_range_noprof(unsigned long size, unsigned long align,
			unsigned long start, unsigned long end, gfp_t gfp_mask,
			pgprot_t prot, unsigned long vm_flags, int node,
			const void *caller)
{
	...
	if ((size >> PAGE_SHIFT) > totalram_pages()) {
		warn_alloc(gfp_mask, NULL,
			"vmalloc error: size %lu, exceeds total pages",
			size);
		return NULL;
	}

	if (vmap_allow_huge && (vm_flags & VM_ALLOW_HUGE_VMAP)) {
		...
		if (arch_vmap_pmd_supported(prot) && size >= PMD_SIZE)
			shift = PMD_SHIFT;
		else
			shift = arch_vmap_pte_supported_shift(size);

		align = max(original_align, 1UL << shift);
	}

again:
	area = __get_vm_area_node(size, align, shift, VM_ALLOC |
				  VM_UNINITIALIZED | vm_flags, start, end, node,
				  gfp_mask, caller);
	...
	/* Allocate physical pages and map them into vmalloc space. */
	ret = __vmalloc_area_node(area, gfp_mask, prot, shift, node);
	if (!ret)
		goto fail;
```

`VM_ALLOW_HUGE_VMAP`（`vmalloc_huge()`、`kvmalloc()` 会设置）允许在 x86_64 上用 2 MB 的 PMD 大页映射（`CONFIG_HAVE_ARCH_HUGE_VMALLOC=y`），减少页表与 TLB 开销；大页分配失败时 `fail` 标签处会退回 4 KB 页重试（[`mm/vmalloc.c:3958-3965`](../../linux/mm/vmalloc.c#L3958-L3965)）。普通 `vmalloc()` 不带此标志。

### 分配虚拟区间

[`mm/vmalloc.c:3194-3227`](../../linux/mm/vmalloc.c#L3194-L3227)（v6.18.60，节选）

```c
struct vm_struct *__get_vm_area_node(unsigned long size,
		unsigned long align, unsigned long shift, unsigned long flags,
		unsigned long start, unsigned long end, int node,
		gfp_t gfp_mask, const void *caller)
{
	struct vmap_area *va;
	struct vm_struct *area;
	unsigned long requested_size = size;

	BUG_ON(in_interrupt());
	size = ALIGN(size, 1ul << shift);
	...
	area = kzalloc_node(sizeof(*area), gfp_mask & GFP_RECLAIM_MASK, node);
	if (unlikely(!area))
		return NULL;

	if (!(flags & VM_NO_GUARD))
		size += PAGE_SIZE;

	area->flags = flags;
	area->caller = caller;
	area->requested_size = requested_size;

	va = alloc_vmap_area(size, align, start, end, node, gfp_mask, 0, area);
```

注意 `size += PAGE_SIZE`：除非指定 `VM_NO_GUARD`，每个 vmalloc 区域后面都多留一页**不映射的保护页（Guard Page）**，越界访问会立即触发缺页异常而不是悄悄破坏相邻区域。`CONFIG_VMAP_STACK` 正是利用这一点检测内核栈溢出。

`alloc_vmap_area()` 先尝试 `node_alloc()` 从分区池中直接取，失败再到全局空闲树中查找：

[`mm/vmalloc.c:2063-2115`](../../linux/mm/vmalloc.c#L2063-L2115)（v6.18.60，节选）

```c
	va = node_alloc(size, align, vstart, vend, &addr, &vn_id);
	if (!va) {
		va = kmem_cache_alloc_node(vmap_area_cachep, gfp_mask, node);
		if (unlikely(!va))
			return ERR_PTR(-ENOMEM);
		...
	}

retry:
	if (IS_ERR_VALUE(addr)) {
		preload_this_cpu_lock(&free_vmap_area_lock, gfp_mask, node);
		addr = __alloc_vmap_area(&free_vmap_area_root, &free_vmap_area_list,
			size, align, vstart, vend);
		spin_unlock(&free_vmap_area_lock);
		...
		cond_resched();
	}
	...
	if (IS_ERR_VALUE(addr))
		goto overflow;

	va->va_start = addr;
	va->va_end = addr + size;
	va->vm = NULL;
	va->flags = (va_flags | vn_id);

	if (vm) {
		vm->addr = (void *)va->va_start;
		vm->size = va_size(va);
		va->vm = vm;
	}

	vn = addr_to_node(va->va_start);

	spin_lock(&vn->busy.lock);
	insert_vmap_area(va, &vn->busy.root, &vn->busy.head);
	spin_unlock(&vn->busy.lock);
```

`__alloc_vmap_area()` 在增强红黑树中做**首次适配（First Fit）**查找：由于每个节点记录了子树中最大的空闲区间长度，查找时可以直接跳过整棵"装不下"的子树，复杂度为 O(log n)，找到的总是满足条件的最低地址区间。分配后剩余部分被拆分放回树中。

地址空间耗尽时（`overflow`），先调用 `reclaim_and_purge_vmap_areas()` 强制回收所有 lazy 区间再重试；仍失败则通知注册了 `vmap_notify_list` 的子系统释放缓存的映射（[`mm/vmalloc.c:2128-2148`](../../linux/mm/vmalloc.c#L2128-L2148)）。

### 分配物理页并建立映射

[`mm/vmalloc.c:3708-3818`](../../linux/mm/vmalloc.c#L3708-L3818)（v6.18.60，节选）

```c
static void *__vmalloc_area_node(struct vm_struct *area, gfp_t gfp_mask,
				 pgprot_t prot, unsigned int page_shift,
				 int node)
{
	const gfp_t nested_gfp = (gfp_mask & GFP_RECLAIM_MASK) | __GFP_ZERO;
	...
	array_size = (unsigned long)nr_small_pages * sizeof(struct page *);

	if (!(gfp_mask & (GFP_DMA | GFP_DMA32)))
		gfp_mask |= __GFP_HIGHMEM;

	/* Please note that the recursion is strictly bounded. */
	if (array_size > PAGE_SIZE) {
		area->pages = __vmalloc_node_noprof(array_size, 1, nested_gfp, node,
					area->caller);
	} else {
		area->pages = kmalloc_node_noprof(array_size, nested_gfp, node);
	}
	...
	area->nr_pages = vm_area_alloc_pages((page_order ?
		gfp_mask & ~__GFP_NOFAIL : gfp_mask) | __GFP_NOWARN,
		node, page_order, nr_small_pages, area->pages);
	...
	/*
	 * page tables allocations ignore external gfp mask, enforce it
	 * by the scope API
	 */
	if ((gfp_mask & (__GFP_FS | __GFP_IO)) == __GFP_IO)
		flags = memalloc_nofs_save();
	else if ((gfp_mask & (__GFP_FS | __GFP_IO)) == 0)
		flags = memalloc_noio_save();

	do {
		ret = vmap_pages_range(addr, addr + size, prot, area->pages,
			page_shift);
		if (nofail && (ret < 0))
			schedule_timeout_uninterruptible(1);
	} while (nofail && (ret < 0));
	...
	return area->addr;
```

几个设计细节：

- 页指针数组本身可能很大（1 GB 的 vmalloc 需要 2 MB 的数组），超过一页时**递归**使用 vmalloc 分配。由于数组大小只有原始大小的 1/512，递归深度严格有界。
- `vm_area_alloc_pages()`（[`mm/vmalloc.c:3621`](../../linux/mm/vmalloc.c#L3621)）对 order 0 使用批量接口 `alloc_pages_bulk_node_noprof()` / `alloc_pages_bulk_mempolicy_noprof()`（[`mm/vmalloc.c:3652-3656`](../../linux/mm/vmalloc.c#L3652-L3656)），一次从 PCP 获取多个页，减少逐页分配的开销。
- 页表页的分配固定使用 `GFP_KERNEL`，不接受调用者的 GFP 参数。为了尊重调用者的 `GFP_NOFS` / `GFP_NOIO` 语义，这里用 `memalloc_nofs_save()` 等**作用域 API** 临时限制当前任务，4.3 节的 `current_gfp_context()` 会据此收紧所有嵌套分配。

`vmap_pages_range()` 逐级遍历内核页表（PGD → P4D → PUD → PMD → PTE），缺失的中间层页表按需分配，最后填写 PTE（或 PMD 大页项）。

### 为什么 x86_64 不需要同步页表

内核空间的映射被所有进程共享，但每个进程有自己的 PGD。如果 vmalloc 在某一级新建了页表项，其他进程的 PGD 中就没有这一项。32 位 x86 通过"vmalloc 缺页"在访问时惰性同步。x86_64 则在启动时**预先分配** vmalloc 区域所需的所有 P4D/PUD 层页表，使所有进程的 PGD 一开始就指向相同的下级页表，之后的修改只发生在共享的下级页表中：

[`arch/x86/mm/init_64.c:1332-1348`](../../linux/arch/x86/mm/init_64.c#L1332-L1348)（v6.18.60，节选）

```c
/*
 * Pre-allocates page-table pages for the vmalloc area in the kernel page-table.
 * Only the level which needs to be synchronized between all page-tables is
 * allocated because the synchronization can be expensive.
 */
static void __init preallocate_vmalloc_pages(void)
{
	unsigned long addr;
	const char *lvl;

	for (addr = VMALLOC_START; addr <= VMEMORY_END; addr = ALIGN(addr + 1, PGDIR_SIZE)) {
		pgd_t *pgd = pgd_offset_k(addr);
		p4d_t *p4d;
		pud_t *pud;

		lvl = "p4d";
		p4d = p4d_alloc(&init_mm, pgd, addr);
```

该函数由 `mem_init()` 调用（[`arch/x86/mm/init_64.c:1406`](../../linux/arch/x86/mm/init_64.c#L1406)）。

## 释放路径与延迟 TLB 刷新

### 为什么要延迟

解除内核映射后必须刷新**所有** CPU 的 TLB，这需要发送 IPI（处理器间中断），代价很高。vmalloc 的策略是：`vfree()` 只清除页表项、释放物理页，把虚拟区间放入 lazy 树；当 lazy 区间累计超过阈值时，再**一次性**对整个范围做一次全局 TLB 刷新，并把区间归还空闲树。在刷新之前，这些虚拟地址不会被重新分配，所以残留的 TLB 项不会被误用。

### `vfree()`

[`mm/vmalloc.c:3433-3477`](../../linux/mm/vmalloc.c#L3433-L3477)（v6.18.60，节选）

```c
void vfree(const void *addr)
{
	struct vm_struct *vm;
	int i;

	if (unlikely(in_interrupt())) {
		vfree_atomic(addr);
		return;
	}

	BUG_ON(in_nmi());
	kmemleak_free(addr);
	might_sleep();

	if (!addr)
		return;

	vm = remove_vm_area(addr);
	...
	if (unlikely(vm->flags & VM_FLUSH_RESET_PERMS))
		vm_reset_perms(vm);
	...
	for (i = 0; i < vm->nr_pages; i++) {
		struct page *page = vm->pages[i];

		BUG_ON(!page);
		/*
		 * High-order allocs for huge vmallocs are split, so
		 * can be freed as an array of order-0 allocations
		 */
		__free_page(page);
		cond_resched();
	}
	...
	kvfree(vm->pages);
	kfree(vm);
}
```

`vfree()` 可能睡眠，中断上下文中会转交给 `vfree_atomic()`，后者通过工作队列延迟处理。`VM_FLUSH_RESET_PERMS` 用于模块、BPF 等曾被设为只读/可执行的区域：释放前必须恢复直接映射中对应页的权限并立即刷新 TLB，否则这些页重新分配后可能仍然带着可执行权限。

`remove_vm_area()`（[`mm/vmalloc.c:3310`](../../linux/mm/vmalloc.c#L3310)）从 busy 树摘下区间并调用 `free_unmap_vmap_area()` 清除页表项，最终进入：

[`mm/vmalloc.c:2435-2465`](../../linux/mm/vmalloc.c#L2435-L2465)（v6.18.60，节选）

```c
static void free_vmap_area_noflush(struct vmap_area *va)
{
	unsigned long nr_lazy_max = lazy_max_pages();
	...
	nr_lazy = atomic_long_add_return_relaxed(va_size(va) >> PAGE_SHIFT,
					 &vmap_lazy_nr);
	...
	vn = is_vn_id_valid(vn_id) ?
		id_to_node(vn_id):addr_to_node(va->va_start);

	spin_lock(&vn->lazy.lock);
	insert_vmap_area(va, &vn->lazy.root, &vn->lazy.head);
	spin_unlock(&vn->lazy.lock);

	trace_free_vmap_area_noflush(va_start, nr_lazy, nr_lazy_max);

	/* After this point, we may free va at any time */
	if (unlikely(nr_lazy > nr_lazy_max))
		schedule_work(&drain_vmap_work);
}
```

阈值由 `lazy_max_pages()` 给出：

[`mm/vmalloc.c:2179-2186`](../../linux/mm/vmalloc.c#L2179-L2186)（v6.18.60）

```c
static unsigned long lazy_max_pages(void)
{
	unsigned int log;

	log = fls(num_online_cpus());

	return log * (32UL * 1024 * 1024 / PAGE_SIZE);
}
```

即 32 MB × log2(CPU 数)。超过后由 `drain_vmap_work` 工作项调用 `__purge_vmap_area_lazy()`（[`mm/vmalloc.c:2331`](../../linux/mm/vmalloc.c#L2331)）：合并所有分区的 lazy 区间，计算覆盖范围，执行一次 `flush_tlb_kernel_range()`，然后由各分区的 `purge_work` 并行地把区间放回分区池或全局空闲树。

## `kvmalloc()`：两全其美的折中

很多调用者需要的内存可能较大，但又希望在内存充足时得到物理连续的内存以获得更好的性能。`kvmalloc()` 先尝试 kmalloc，失败再回退到 vmalloc：

[`mm/slub.c:7155-7189`](../../linux/mm/slub.c#L7155-L7189)（v6.18.60，节选）

```c
void *__kvmalloc_node_noprof(DECL_BUCKET_PARAMS(size, b), unsigned long align,
			     gfp_t flags, int node)
{
	void *ret;

	/*
	 * It doesn't really make sense to fallback to vmalloc for sub page
	 * requests
	 */
	ret = __do_kmalloc_node(size, PASS_BUCKET_PARAM(b),
				kmalloc_gfp_adjust(flags, size),
				node, _RET_IP_);
	if (ret || size <= PAGE_SIZE)
		return ret;

	/* non-sleeping allocations are not supported by vmalloc */
	if (!gfpflags_allow_blocking(flags))
		return NULL;
	...
	return __vmalloc_node_range_noprof(size, align, VMALLOC_START, VMALLOC_END,
			flags, PAGE_KERNEL, VM_ALLOW_HUGE_VMAP,
			node, __builtin_return_address(0));
}
```

关键在 `kmalloc_gfp_adjust()`（[`mm/slub.c:7110-7131`](../../linux/mm/slub.c#L7110-L7131)）：对大于一页的请求，kmalloc 尝试时加上 `__GFP_NOWARN`，并且**去掉 `__GFP_DIRECT_RECLAIM`**（除非调用者指定了 `__GFP_RETRY_MAYFAIL`）。也就是说，kmalloc 只在不需要回收就能满足时才成功，否则立即转向 vmalloc，避免为了物理连续而触发昂贵的回收和规整。对应的释放函数 `kvfree()` 通过 `is_vmalloc_addr()` 判断该走哪条路径。

| 接口 | 物理连续 | 最大尺寸 | 可在原子上下文使用 | 典型用途 |
|---|---|---|---|---|
| `kmalloc()` | 是 | 4 MB | 是（`GFP_ATOMIC`） | 小对象 |
| `alloc_pages()` | 是 | 4 MB（order 10） | 是 | 页粒度缓冲区、DMA |
| `vmalloc()` | 否 | 受 vmalloc 区大小限制 | 否 | 模块、大表、内核栈 |
| `kvmalloc()` | 视情况 | 同 vmalloc | 否 | 大小不确定的缓冲区 |

## 动手实验

查看当前所有 vmalloc 区域及其调用者（root）：

```bash
sudo head -20 /proc/vmallocinfo
# 0xffffc90000000000-0xffffc90000005000   20480 irq_init_percpu_irqstack+0x... vmap
# 0xffffc90000005000-0xffffc90000007000    8192 acpi_os_map_iomem+0x... phys=... ioremap
sudo awk '{s+=$2} END {print s/1024/1024 " MB"}' /proc/vmallocinfo
grep -E "Vmalloc(Total|Used|Chunk)" /proc/meminfo
```

`/proc/vmallocinfo` 中每个区域的大小都包含末尾的保护页。

确认 KASLR 随机化后的区域基址（需要 root 且 `kptr_restrict` 允许）：

```bash
sudo grep -E " (page_offset_base|vmalloc_base|vmemmap_base)$" /proc/kallsyms
# 这些是变量地址；变量的值可通过 crash/drgn 读取，例如：
sudo drgn -c /proc/kcore -e 'print(hex(prog["page_offset_base"].value_()))'
```

观察 vmalloc 延迟回收触发的全局 TLB 刷新（root）：

```bash
sudo bpftrace -e 'kprobe:__purge_vmap_area_lazy { @purge = count(); }
                  tracepoint:vmalloc:purge_vmap_area_lazy { @pages = hist(args->npurged); }'
```

## 小结

- x86_64 内核空间依次包含直接映射区、vmalloc 区、vmemmap、KASAN 影子、cpu_entry_area、内核代码段与模块区；前三者的基址在启用 `CONFIG_RANDOMIZE_MEMORY` 时以 1 GB 粒度随机化。
- vmalloc 用 `vmap_area` 管理虚拟区间（空闲区间放在增强红黑树中，首次适配），用 `vm_struct` 记录物理页数组，每个区域默认带一页保护页。
- 分配分两步：先分配虚拟区间，再逐页（或按 PMD 大页）从伙伴系统取页并写入内核页表；x86_64 预先分配了 vmalloc 区的上层页表，无需在进程间同步。
- v6.9 引入的 vmap_node 把 busy/lazy 树按地址交错分区，并为小尺寸区间提供缓存池，缓解了全局锁竞争。
- 释放采用延迟 TLB 刷新：lazy 区间累计超过 32 MB × log2(CPU 数) 后再批量刷新。
- `kvmalloc()` 先以"不回收"的方式尝试 kmalloc，失败再回退 vmalloc。

## 参考

- `linux/Documentation/arch/x86/x86_64/mm.rst`
- `linux/Documentation/mm/vmalloc.rst`
- `linux/Documentation/mm/vmalloced-kernel-stacks.rst`
- `linux/Documentation/core-api/memory-allocation.rst`
