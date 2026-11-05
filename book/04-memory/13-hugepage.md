# 4.13 大页：hugetlbfs 与透明大页

> **本节源码**：[`include/linux/hugetlb.h`](../../linux/include/linux/hugetlb.h)、[`mm/hugetlb.c`](../../linux/mm/hugetlb.c)、[`mm/hugetlb_vmemmap.c`](../../linux/mm/hugetlb_vmemmap.c)、[`include/linux/huge_mm.h`](../../linux/include/linux/huge_mm.h)、[`mm/huge_memory.c`](../../linux/mm/huge_memory.c)、[`mm/khugepaged.c`](../../linux/mm/khugepaged.c)、[`mm/memory.c`](../../linux/mm/memory.c)
> **关键函数**：`hugetlb_reserve_pages()`、`alloc_hugetlb_folio()`、`hugetlb_fault()`、`hugetlb_vmemmap_optimize_folio()`、`thp_vma_allowable_orders()`、`__thp_vma_allowable_orders()`、`do_huge_pmd_anonymous_page()`、`vma_thp_gfp_mask()`、`deferred_split_folio()`、`khugepaged()`、`collapse_huge_page()`
> **前置知识**：4.8 页表与缺页异常、4.12 内存规整

## 概述

x86_64 的 TLB 条目数有限（L1 dTLB 通常只有几十项，L2 STLB 一两千项）。以 4 KB 页计算，一千多个 TLB 项只能覆盖几 MB 内存；访问数 GB 工作集的程序会频繁发生 TLB 缺失，每次缺失都要遍历四级页表。**大页（Huge Page）** 用 PMD（2 MB）或 PUD（1 GB）直接映射，一个 TLB 项覆盖的范围扩大 512 或 262144 倍，同时减少页表层级和页表内存，缺页次数也相应减少。

Linux 提供两套大页机制：

- **hugetlbfs**（v2.6 起）：管理员预先保留一个大页池，应用通过 `mmap()` hugetlbfs 文件、`MAP_HUGETLB` 或 `SHM_HUGETLB` 显式使用。大页不可回收、不可交换，行为确定。
- **透明大页（Transparent Huge Page，THP）**（v2.6.38 起）：内核在缺页时自动尝试分配 2 MB 页，失败则透明回退到 4 KB 页；后台线程 khugepaged 把已有的小页合并成大页。应用无需修改。v6.8 起扩展为**多尺寸 THP（mTHP）**，支持 16 KB～1 MB 等中间尺寸（4.8 节）。

本书配置启用 `CONFIG_HUGETLBFS=y`、`CONFIG_TRANSPARENT_HUGEPAGE=y`，THP 默认策略为 `madvise`（`CONFIG_TRANSPARENT_HUGEPAGE_MADVISE=y`），即只对用 `madvise(MADV_HUGEPAGE)` 标记过的区域使用 THP。

## hugetlbfs

### hstate：大页池

每种大页尺寸对应一个 `struct hstate`，x86_64 上通常有 2 MB 与 1 GB 两种（后者需要 CPU 支持 `pdpe1gb`）：

[`include/linux/hugetlb.h:656-677`](../../linux/include/linux/hugetlb.h#L656-L677)（v6.18.60）

```c
struct hstate {
	struct mutex resize_lock;
	struct lock_class_key resize_key;
	int next_nid_to_alloc;
	int next_nid_to_free;
	unsigned int order;
	unsigned int demote_order;
	unsigned long mask;
	unsigned long max_huge_pages;
	unsigned long nr_huge_pages;
	unsigned long free_huge_pages;
	unsigned long resv_huge_pages;
	unsigned long surplus_huge_pages;
	unsigned long nr_overcommit_huge_pages;
	struct list_head hugepage_activelist;
	struct list_head hugepage_freelists[MAX_NUMNODES];
	unsigned int max_huge_pages_node[MAX_NUMNODES];
	unsigned int nr_huge_pages_node[MAX_NUMNODES];
	unsigned int free_huge_pages_node[MAX_NUMNODES];
	unsigned int surplus_huge_pages_node[MAX_NUMNODES];
	char name[HSTATE_NAME_LEN];
};
```

| 字段 | 含义（对应 `/proc/meminfo`） |
|---|---|
| `nr_huge_pages` | 池中大页总数（`HugePages_Total`） |
| `free_huge_pages` | 空闲大页数（`HugePages_Free`） |
| `resv_huge_pages` | 已被预留但尚未缺页使用的大页数（`HugePages_Rsvd`） |
| `surplus_huge_pages` | 超出 `nr_hugepages` 设定、按需临时分配的大页（`HugePages_Surp`） |
| `nr_overcommit_huge_pages` | 允许的超额上限（`/proc/sys/vm/nr_overcommit_hugepages`） |
| `hugepage_freelists[nid]` | 每个节点的空闲大页链表 |

大页池通过 `/proc/sys/vm/nr_hugepages`、`/sys/kernel/mm/hugepages/hugepages-<size>kB/nr_hugepages` 或启动参数 `hugepages=` 调整。运行时增加池大小由 `set_max_huge_pages()`（[`mm/hugetlb.c:3843`](../../linux/mm/hugetlb.c#L3843)）从伙伴系统分配 2 MB（order 9）连续页；1 GB 页超过 `MAX_PAGE_ORDER`，只能在启动时通过 memblock 预留（`hugetlb_bootmem_alloc()`，4.2 节 `mm_core_init()` 的第二步）或经 CMA 获取。

池中的大页一旦分配就不再参与回收、不能被交换，这是 hugetlbfs 行为确定的根源，也是其不灵活之处。

### 预留

hugetlbfs 的一个重要特性是**在 mmap 时预留**：`MAP_SHARED` 映射或 `MAP_PRIVATE` 映射在 `mmap()` 时就通过 `hugetlb_reserve_pages()`（[`mm/hugetlb.c:7280`](../../linux/mm/hugetlb.c#L7280)）从池中预留足够的大页（`resv_huge_pages` 增加），保证以后缺页时一定能拿到大页，而不会像普通内存那样在缺页时才发现内存不足并收到 SIGBUS。预留信息记录在 inode（共享）或 VMA（私有）的 `resv_map` 区间链表中。

### 分配与缺页

[`mm/hugetlb.c:2974-3065`](../../linux/mm/hugetlb.c#L2974-L3065)（v6.18.60，节选）

```c
struct folio *alloc_hugetlb_folio(struct vm_area_struct *vma,
				    unsigned long addr, bool cow_from_owner)
{
	...
	gfp_t gfp = htlb_alloc_mask(h) | __GFP_RETRY_MAYFAIL;
	...
		retval = vma_needs_reservation(h, vma, addr);
		if (retval < 0)
			return ERR_PTR(-ENOMEM);
		map_chg = retval ? MAP_CHG_NEEDED : MAP_CHG_REUSE;
	...
	spin_lock_irq(&hugetlb_lock);
	/*
	 * glb_chg is passed to indicate whether or not a page must be taken
	 * from the global free pool (global change).  gbl_chg == 0 indicates
	 * a reservation exists for the allocation.
	 */
	folio = dequeue_hugetlb_folio_vma(h, vma, addr, gbl_chg);
	if (!folio) {
		spin_unlock_irq(&hugetlb_lock);
		folio = alloc_buddy_hugetlb_folio_with_mpol(h, vma, addr);
		if (!folio)
			goto out_uncharge_cgroup;
		spin_lock_irq(&hugetlb_lock);
		list_add(&folio->lru, &h->hugepage_activelist);
		folio_ref_unfreeze(folio, 1);
		/* Fall through */
	}

	/*
	 * Either dequeued or buddy-allocated folio needs to add special
	 * mark to the folio when it consumes a global reservation.
	 */
	if (!gbl_chg) {
		folio_set_hugetlb_restore_reserve(folio);
		h->resv_huge_pages--;
	}
	...
```

分配先检查是否有预留（`vma_needs_reservation()`），再从池中取一个空闲大页（`dequeue_hugetlb_folio_vma()` 按 NUMA 策略选节点）。池空且允许超额时，`alloc_buddy_hugetlb_folio_with_mpol()` 临时从伙伴系统分配一个 surplus 大页。消耗了预留的分配会减少 `resv_huge_pages`。hugetlb cgroup（`hugetlb.<size>.max`）的计费也在这里完成。

hugetlb VMA 的缺页不走 4.8 节的 `__handle_mm_fault()`，而是由 `handle_mm_fault()` 直接转入 `hugetlb_fault()`（[`mm/hugetlb.c:6692`](../../linux/mm/hugetlb.c#L6692)）。它用一个按 `(mapping, index)` 哈希的互斥锁表（`hugetlb_fault_mutex_table`）串行化同一大页的并发缺页，然后在 hugetlbfs 文件的 page cache 中查找或调用 `alloc_hugetlb_folio()` 分配，最后直接写入 PMD（或 PUD）项。私有映射的写时复制在 `hugetlb_wp()`（[`mm/hugetlb.c:6170`](../../linux/mm/hugetlb.c#L6170)）中处理，需要再分配一个完整的大页。

### HVO：释放大页的 vmemmap

一个 2 MB 大页有 512 个 `struct page`，占 32 KB（8 个 vmemmap 页）；一个 1 GB 大页的 `struct page` 占 16 MB。但大页作为一个整体管理，绝大多数尾页的 `struct page` 内容完全相同。

**HugeTLB Vmemmap Optimization（HVO）**（`CONFIG_HUGETLB_PAGE_OPTIMIZE_VMEMMAP=y`）在大页进入池时，把描述尾页的 vmemmap 物理页释放掉，再把这些 vmemmap 虚拟地址**重新映射为只读**、指向保留下来的第一个 vmemmap 页（`HUGETLB_VMEMMAP_RESERVE_SIZE` = 一页，[`mm/hugetlb_vmemmap.h:19`](../../linux/mm/hugetlb_vmemmap.h#L19)）。2 MB 大页每个节省 7 页（28 KB），1 GB 大页每个节省约 16 MB。这正是 4.1 节 `_compound_head()` 中 `page_fixed_fake_head()` 存在的原因：被重映射的尾页 `struct page` 实际是头页区域的副本，需要特殊识别。

本书配置未设置 `CONFIG_HUGETLB_PAGE_OPTIMIZE_VMEMMAP_DEFAULT_ON`，HVO 默认关闭，可用启动参数 `hugetlb_free_vmemmap=on`（[`mm/hugetlb_vmemmap.c:394`](../../linux/mm/hugetlb_vmemmap.c#L394)）或 `/proc/sys/vm/hugetlb_optimize_vmemmap` 开启。

## 透明大页（THP）

### 启用策略

THP 的全局策略位于 `/sys/kernel/mm/transparent_hugepage/enabled`，默认值由编译选项决定：

[`mm/huge_memory.c:60-69`](../../linux/mm/huge_memory.c#L60-L69)（v6.18.60）

```c
unsigned long transparent_hugepage_flags __read_mostly =
#ifdef CONFIG_TRANSPARENT_HUGEPAGE_ALWAYS
	(1<<TRANSPARENT_HUGEPAGE_FLAG)|
#endif
#ifdef CONFIG_TRANSPARENT_HUGEPAGE_MADVISE
	(1<<TRANSPARENT_HUGEPAGE_REQ_MADV_FLAG)|
#endif
	(1<<TRANSPARENT_HUGEPAGE_DEFRAG_REQ_MADV_FLAG)|
	(1<<TRANSPARENT_HUGEPAGE_DEFRAG_KHUGEPAGED_FLAG)|
	(1<<TRANSPARENT_HUGEPAGE_USE_ZERO_PAGE_FLAG);
```

| `enabled` | 含义 |
|---|---|
| `always` | 所有匿名 VMA 都尝试 THP |
| `madvise`（本书默认） | 只对 `MADV_HUGEPAGE` 标记的 VMA（`VM_HUGEPAGE`）使用 |
| `never` | 禁用 |

`defrag` 控制分配 THP 时愿意付出多大代价（默认 `madvise`）。v6.8 引入 mTHP 后，每个尺寸又有自己的开关 `/sys/kernel/mm/transparent_hugepage/hugepages-<size>kB/enabled`，可取 `always`、`madvise`、`inherit`、`never`；PMD 尺寸默认为 `inherit`（继承全局设置，[`mm/huge_memory.c:848`](../../linux/mm/huge_memory.c#L848)），其他尺寸默认 `never`。

能否在某个 VMA 上使用某些阶，由 `thp_vma_allowable_orders()` 判断：

[`include/linux/huge_mm.h:288-313`](../../linux/include/linux/huge_mm.h#L288-L313)（v6.18.60）

```c
static inline
unsigned long thp_vma_allowable_orders(struct vm_area_struct *vma,
				       vm_flags_t vm_flags,
				       enum tva_type type,
				       unsigned long orders)
{
	/*
	 * Optimization to check if required orders are enabled early. Only
	 * forced collapse ignores sysfs configs.
	 */
	if (type != TVA_FORCED_COLLAPSE && vma_is_anonymous(vma)) {
		unsigned long mask = READ_ONCE(huge_anon_orders_always);

		if (vm_flags & VM_HUGEPAGE)
			mask |= READ_ONCE(huge_anon_orders_madvise);
		if (hugepage_global_always() ||
		    ((vm_flags & VM_HUGEPAGE) && hugepage_global_enabled()))
			mask |= READ_ONCE(huge_anon_orders_inherit);

		orders &= mask;
		if (!orders)
			return 0;
	}

	return __thp_vma_allowable_orders(vma, vm_flags, type, orders);
}
```

三个位图 `huge_anon_orders_always`、`huge_anon_orders_madvise`、`huge_anon_orders_inherit` 分别记录各尺寸的设置。先按 sysfs 配置过滤，再由 `__thp_vma_allowable_orders()`（[`mm/huge_memory.c:166-254`](../../linux/mm/huge_memory.c#L166-L254)）检查 VMA 本身：`prctl(PR_SET_THP_DISABLE)` 或 `MADV_NOHUGEPAGE` 禁用的 VMA、DAX、栈、VMA 太小或地址不对齐等情况被排除。

### THP 缺页

4.8 节的 `__handle_mm_fault()` 在 PMD 为空且允许 PMD 阶时调用 `create_huge_pmd()`，匿名 VMA 进入 `do_huge_pmd_anonymous_page()`：

[`mm/huge_memory.c:1457-1511`](../../linux/mm/huge_memory.c#L1457-L1511)（v6.18.60，节选）

```c
vm_fault_t do_huge_pmd_anonymous_page(struct vm_fault *vmf)
{
	struct vm_area_struct *vma = vmf->vma;
	unsigned long haddr = vmf->address & HPAGE_PMD_MASK;
	vm_fault_t ret;

	if (!thp_vma_suitable_order(vma, haddr, PMD_ORDER))
		return VM_FAULT_FALLBACK;
	ret = vmf_anon_prepare(vmf);
	if (ret)
		return ret;
	khugepaged_enter_vma(vma, vma->vm_flags);

	if (!(vmf->flags & FAULT_FLAG_WRITE) &&
			!mm_forbids_zeropage(vma->vm_mm) &&
			transparent_hugepage_use_zero_page()) {
		...
		zero_folio = mm_get_huge_zero_folio(vma->vm_mm);
		...
				set_huge_zero_folio(pgtable, vma->vm_mm, vma,
						   haddr, vmf->pmd, zero_folio);
		...
		return ret;
	}

	return __do_huge_pmd_anonymous_page(vmf);
}
```

`thp_vma_suitable_order()` 检查以 2 MB 对齐的整个区间 `[haddr, haddr + 2 MB)` 是否都在 VMA 内——这就是 4.7 节中 `__get_unmapped_area()` 对匿名映射做 THP 对齐的意义。读缺页映射全局共享的**大零页**（2 MB 的零页），写缺页分配真正的大页：

[`mm/huge_memory.c:1357-1398`](../../linux/mm/huge_memory.c#L1357-L1398)（v6.18.60，节选）

```c
static vm_fault_t __do_huge_pmd_anonymous_page(struct vm_fault *vmf)
{
	unsigned long haddr = vmf->address & HPAGE_PMD_MASK;
	struct vm_area_struct *vma = vmf->vma;
	struct folio *folio;
	pgtable_t pgtable;
	vm_fault_t ret = 0;

	folio = vma_alloc_anon_folio_pmd(vma, vmf->address);
	if (unlikely(!folio))
		return VM_FAULT_FALLBACK;

	pgtable = pte_alloc_one(vma->vm_mm);
	if (unlikely(!pgtable)) {
		ret = VM_FAULT_OOM;
		goto release;
	}

	vmf->ptl = pmd_lock(vma->vm_mm, vmf->pmd);
	if (unlikely(!pmd_none(*vmf->pmd))) {
		goto unlock_release;
	} else {
		...
		pgtable_trans_huge_deposit(vma->vm_mm, vmf->pmd, pgtable);
		map_anon_folio_pmd(folio, vmf->pmd, vma, haddr);
		mm_inc_nr_ptes(vma->vm_mm);
		spin_unlock(vmf->ptl);
	}

	return 0;
	...
```

两个细节：

- **分配失败即回退**：`vma_alloc_anon_folio_pmd()` 失败返回 `VM_FAULT_FALLBACK`，`__handle_mm_fault()` 随即走普通的 PTE 缺页路径（可能分配 mTHP 或 4 KB 页）。THP 是"尽力而为"的优化。
- **预存 PTE 页表**：`pgtable_trans_huge_deposit()` 预先分配一个 PTE 页表页并挂在 PMD 上。将来这个大页需要被拆分成 512 个 PTE 映射时（如 `mprotect()` 只修改其中一部分），无需在可能失败的上下文中再分配页表。

`map_anon_folio_pmd()`（[`mm/huge_memory.c:1339-1355`](../../linux/mm/huge_memory.c#L1339-L1355)）构造 PMD 项、建立反向映射、加入 LRU，并调用 `deferred_split_folio()`。

### 分配代价：`vma_thp_gfp_mask()`

[`mm/huge_memory.c:1418-1442`](../../linux/mm/huge_memory.c#L1418-L1442)（v6.18.60）

```c
gfp_t vma_thp_gfp_mask(struct vm_area_struct *vma)
{
	const bool vma_madvised = vma && (vma->vm_flags & VM_HUGEPAGE);

	/* Always do synchronous compaction */
	if (test_bit(TRANSPARENT_HUGEPAGE_DEFRAG_DIRECT_FLAG, &transparent_hugepage_flags))
		return GFP_TRANSHUGE | (vma_madvised ? 0 : __GFP_NORETRY);

	/* Kick kcompactd and fail quickly */
	if (test_bit(TRANSPARENT_HUGEPAGE_DEFRAG_KSWAPD_FLAG, &transparent_hugepage_flags))
		return GFP_TRANSHUGE_LIGHT | __GFP_KSWAPD_RECLAIM;

	/* Synchronous compaction if madvised, otherwise kick kcompactd */
	if (test_bit(TRANSPARENT_HUGEPAGE_DEFRAG_KSWAPD_OR_MADV_FLAG, &transparent_hugepage_flags))
		return GFP_TRANSHUGE_LIGHT |
			(vma_madvised ? __GFP_DIRECT_RECLAIM :
					__GFP_KSWAPD_RECLAIM);

	/* Only do synchronous compaction if madvised */
	if (test_bit(TRANSPARENT_HUGEPAGE_DEFRAG_REQ_MADV_FLAG, &transparent_hugepage_flags))
		return GFP_TRANSHUGE_LIGHT |
		       (vma_madvised ? __GFP_DIRECT_RECLAIM : 0);

	return GFP_TRANSHUGE_LIGHT;
}
```

回顾 4.1 节：`GFP_TRANSHUGE_LIGHT` 不含任何回收标志、带 `__GFP_NOMEMALLOC | __GFP_NOWARN`；`GFP_TRANSHUGE` 额外加上 `__GFP_DIRECT_RECLAIM`。`defrag` 设置决定了 THP 缺页时能否进行直接回收与规整：

| `defrag` | 普通 VMA | `MADV_HUGEPAGE` VMA |
|---|---|---|
| `always` | 直接规整（`__GFP_NORETRY`） | 直接回收 + 规整，可重试 |
| `defer` | 只唤醒 kswapd/kcompactd，立即失败 | 同左 |
| `defer+madvise` | 只唤醒 kswapd/kcompactd | 直接回收 + 规整 |
| `madvise`（默认） | 不回收，立即失败 | 直接回收 + 规整 |
| `never` | 不回收 | 不回收 |

这一设计源于实践教训：早期 THP 在 `always` 模式下的直接规整会造成严重的延迟尖峰，许多数据库厂商因此建议关闭 THP。现在的默认值让未明确请求大页的程序永远不会为 THP 付出分配延迟。

### 拆分与延迟拆分

很多操作只针对大页的一部分：`munmap()` 或 `mprotect()` 半个大页、`MADV_DONTNEED` 部分区域、换出时无法分配连续交换槽位、页迁移目标不支持大页等。此时需要**拆分（Split）**：

- **拆分 PMD 映射**（`split_huge_pmd()`）：把一个 PMD 映射转换为 512 个 PTE 映射（使用之前预存的页表页），folio 本身仍是 2 MB 的大 folio；
- **拆分 folio**（`split_huge_page_to_list_to_order()`，[`mm/huge_memory.c:4106`](../../linux/mm/huge_memory.c#L4106)）：把大 folio 拆成更小阶的 folio，需要冻结引用计数、解除所有映射，代价较高。

如果进程 `munmap()` 了大页的一半，另一半仍被映射，那么 folio 有一半的内存实际上已经无用，但又不能立即拆分（可能在持有各种锁的上下文中）。**延迟拆分**机制把这类"部分映射"的大 folio 放入每节点（或每 memcg）的 `deferred_split_queue`：

[`mm/huge_memory.c:4201-4250`](../../linux/mm/huge_memory.c#L4201-L4250)（v6.18.60，节选）

```c
void deferred_split_folio(struct folio *folio, bool partially_mapped)
{
	...
	if (folio_order(folio) <= 1)
		return;

	if (!partially_mapped && !split_underused_thp)
		return;
	...
	ds_queue = folio_split_queue_lock_irqsave(folio, &flags);
	if (partially_mapped) {
		if (!folio_test_partially_mapped(folio)) {
			folio_set_partially_mapped(folio);
			...
		}
	} else {
		/* partially mapped folios cannot become non-partially mapped */
		VM_WARN_ON_FOLIO(folio_test_partially_mapped(folio), folio);
	}
	if (list_empty(&folio->_deferred_list)) {
		...
		list_add_tail(&folio->_deferred_list, &ds_queue->split_queue);
		ds_queue->split_queue_len++;
		...
	}
	...
```

队列由一个 shrinker（`deferred_split_scan()`，[`mm/huge_memory.c:4293`](../../linux/mm/huge_memory.c#L4293)）在内存压力下处理：拆分部分映射的 folio 并释放未映射的部分。`folio->_deferred_list` 字段就是 4.1 节中 `struct folio` 第三部分里的那个链表头。

`deferred_split_folio(folio, false)` 在 THP 缺页时也会被调用（见 `map_anon_folio_pmd()`），用于 v6.12 引入的"拆分利用率低的 THP"功能（`split_underused_thp`）：shrinker 检查队列中大 folio 的子页，如果超过 `max_ptes_none` 个子页全为零（从未被写入），就拆分它并把零子页映射到共享零页，回收被 THP 浪费的内存。

### khugepaged

THP 缺页只在 PMD 为空时才会分配大页。如果一段区域已经有了 4 KB 页（例如 THP 分配失败时回退的结果，或在启用 THP 之前分配的），就需要后台线程 **khugepaged** 把它们**合并（Collapse）** 成大页：

[`mm/khugepaged.c:2629-2648`](../../linux/mm/khugepaged.c#L2629-L2648)（v6.18.60）

```c
static int khugepaged(void *none)
{
	struct mm_slot *slot;

	set_freezable();
	set_user_nice(current, MAX_NICE);

	while (!kthread_should_stop()) {
		khugepaged_do_scan(&khugepaged_collapse_control);
		khugepaged_wait_work();
	}

	spin_lock(&khugepaged_mm_lock);
	slot = khugepaged_scan.mm_slot;
	khugepaged_scan.mm_slot = NULL;
	if (slot)
		collect_mm_slot(slot);
	spin_unlock(&khugepaged_mm_lock);
	return 0;
}
```

khugepaged 以最低优先级（nice 19）运行。进程首次缺页进入可用 THP 的 VMA 时，`khugepaged_enter_vma()` 把其 mm 登记到扫描列表。khugepaged 每隔 `scan_sleep_millisecs`（默认 10 秒，[`mm/khugepaged.c:74`](../../linux/mm/khugepaged.c#L74)）醒来，扫描 `pages_to_scan`（默认 4096 页，即 `HPAGE_PMD_NR * 8`，[`mm/khugepaged.c:377`](../../linux/mm/khugepaged.c#L377)）个 PTE。

对每个 2 MB 对齐区域，`hpage_collapse_scan_pmd()`（[`mm/khugepaged.c:1246`](../../linux/mm/khugepaged.c#L1246)）检查是否值得合并：空 PTE 数不超过 `max_ptes_none`（默认 511，即只要有一个子页存在就合并）、交换项数不超过 `max_ptes_swap`、共享页数不超过 `max_ptes_shared`。满足条件则 `collapse_huge_page()`（[`mm/khugepaged.c:1083`](../../linux/mm/khugepaged.c#L1083)）执行合并：分配一个新的 2 MB folio，持 `mmap_lock` 写锁和 anon_vma 锁，隔离并复制 512 个子页的内容（空 PTE 对应的部分清零），最后用一个 PMD 项替换原来的 PTE 页表。

用户也可以用 `madvise(MADV_COLLAPSE)`（v6.1 引入）同步地触发合并，不依赖 khugepaged 的扫描节奏。

### 文件 THP

THP 不限于匿名内存。shmem/tmpfs 有自己的 `huge=` 挂载选项与 `/sys/kernel/mm/transparent_hugepage/shmem_enabled`；普通文件系统（如 ext4、XFS）支持大 folio 后，page cache 可以直接由大 folio 组成，满足对齐条件时 `do_set_pmd()`（[`mm/memory.c:5339`](../../linux/mm/memory.c#L5339)）在文件缺页中用 PMD 直接映射。这部分与 page cache 实现紧密相关，在第 9 章讨论。

## hugetlbfs 与 THP 的比较

| 方面 | hugetlbfs | THP |
|---|---|---|
| 使用方式 | 显式：hugetlbfs 文件、`MAP_HUGETLB`、`SHM_HUGETLB` | 透明：缺页自动；`madvise` 提示 |
| 尺寸 | 2 MB、1 GB | 2 MB（PMD），mTHP 16 KB～1 MB |
| 内存来源 | 预先保留的池 | 伙伴系统，按需分配 |
| 分配失败 | mmap 时预留即失败，或缺页 SIGBUS | 透明回退到小页 |
| 回收/交换 | 不可 | 可（可能先拆分） |
| 内存开销 | 池中空闲大页对其他用途不可用 | 可能因部分使用而浪费（延迟拆分缓解） |
| 典型用户 | 数据库共享内存、DPDK、虚拟机内存 | 通用应用、JVM、内存数据库 |

## 动手实验

hugetlbfs：

```bash
grep -i huge /proc/meminfo
echo 64 | sudo tee /proc/sys/vm/nr_hugepages          # 保留 64 个 2 MB 大页
ls /sys/kernel/mm/hugepages/
sudo mkdir -p /mnt/huge && sudo mount -t hugetlbfs none /mnt/huge
python3 - <<'EOF'
import mmap
MAP_HUGETLB = 0x40000
m = mmap.mmap(-1, 32 << 20, flags=mmap.MAP_PRIVATE | mmap.MAP_ANONYMOUS | MAP_HUGETLB)
m[0] = 1; input("check HugePages_Free / Rsvd in another shell, then press Enter")
EOF
```

THP：

```bash
cat /sys/kernel/mm/transparent_hugepage/{enabled,defrag}
ls /sys/kernel/mm/transparent_hugepage/          # hugepages-*kB 为各 mTHP 尺寸
python3 - <<'EOF'
import mmap, ctypes, re
libc = ctypes.CDLL(None)
N = 256 << 20
m = mmap.mmap(-1, N)
addr = ctypes.addressof(ctypes.c_char.from_buffer(m))
MADV_HUGEPAGE = 14
print("madvise:", libc.madvise(ctypes.c_void_p(addr), ctypes.c_size_t(N), MADV_HUGEPAGE))
for i in range(0, N, 4096): m[i] = 1
smaps = open("/proc/self/smaps_rollup").read()
print(re.search(r"AnonHugePages:\s+\d+ kB", smaps).group(0))
EOF
grep -E "^thp_(fault_alloc|fault_fallback|collapse_alloc|split_page|deferred_split_page)" /proc/vmstat
```

注意 Python 的 `mmap` 分配的地址不一定 2 MB 对齐，开头和结尾不满 2 MB 的部分只能用小页。

## 小结

- 大页通过 PMD/PUD 直接映射扩大 TLB 覆盖范围，减少 TLB 缺失、页表开销和缺页次数。
- hugetlbfs 使用预先保留的大页池（每种尺寸一个 `hstate`），mmap 时预留、缺页走独立的 `hugetlb_fault()`，不可回收；HVO 可以释放大页尾部 `struct page` 占用的 vmemmap 内存。
- THP 在缺页时自动尝试分配 2 MB 页（或 mTHP 尺寸），失败透明回退；本书配置默认 `madvise` 策略，`defrag` 设置决定分配时是否直接回收与规整。
- 大页需要部分操作时会拆分 PMD 映射或拆分 folio；部分映射或利用率低的大 folio 进入延迟拆分队列，由 shrinker 处理。
- khugepaged 在后台把已有的小页合并成大页，`MADV_COLLAPSE` 提供同步合并。

## 参考

- `linux/Documentation/admin-guide/mm/hugetlbpage.rst`
- `linux/Documentation/admin-guide/mm/transhuge.rst`
- `linux/Documentation/mm/transhuge.rst`
- `linux/Documentation/mm/hugetlbfs_reserv.rst`
- `linux/Documentation/mm/vmemmap_dedup.rst`（HVO）
