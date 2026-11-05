# 4.7 进程地址空间：mm_struct、VMA 与 mmap

> **本节源码**：[`include/linux/mm_types.h`](../../linux/include/linux/mm_types.h)、[`mm/mmap.c`](../../linux/mm/mmap.c)、[`mm/vma.c`](../../linux/mm/vma.c)、[`mm/util.c`](../../linux/mm/util.c)、[`mm/mmap_lock.c`](../../linux/mm/mmap_lock.c)、[`arch/x86/mm/mmap.c`](../../linux/arch/x86/mm/mmap.c)、[`arch/x86/kernel/sys_x86_64.c`](../../linux/arch/x86/kernel/sys_x86_64.c)
> **关键函数**：`ksys_mmap_pgoff()`、`vm_mmap_pgoff()`、`do_mmap()`、`__get_unmapped_area()`、`arch_get_unmapped_area_topdown()`、`__mmap_region()`、`vma_merge_new_range()`、`find_vma()`、`lock_vma_under_rcu()`、`sys_brk()`、`__vm_enough_memory()`
> **前置知识**：4.6 内核虚拟地址空间

## 概述

每个用户进程都拥有独立的虚拟地址空间，由 `struct mm_struct` 描述；地址空间中每一段属性相同的连续区间（代码段、堆、某个 `mmap()` 映射的文件……）由一个**虚拟内存区域（Virtual Memory Area，VMA）** 即 `struct vm_area_struct` 描述。

Linux 采用**按需分配（Demand Paging）** 策略：`mmap()`、`brk()` 等系统调用只创建或修改 VMA，即"登记"一段合法的地址区间及其权限，并不分配物理内存，也不建立页表。直到进程真正访问这段地址时，才由缺页异常（4.8 节）分配物理页并填写页表。因此 VMA 是用户内存管理的"合同"，页表是合同的"执行记录"。

本节分析 `mm_struct` 与 VMA 的组织方式（v6.1 起使用 maple tree）、mmap 的完整路径（地址选择、VMA 合并、内存提交检查），以及 v6.4 引入的 per-VMA 锁。

## 核心数据结构

### `struct mm_struct`

[`include/linux/mm_types.h:946-1134`](../../linux/include/linux/mm_types.h#L946-L1134)（v6.18.60，节选，删去部分注释）

```c
struct mm_struct {
	struct {
		struct {
			atomic_t mm_count;
		} ____cacheline_aligned_in_smp;

		struct maple_tree mm_mt;		// [注] 所有 VMA 组成的 maple tree

		unsigned long mmap_base;	/* base of mmap area */
		unsigned long mmap_legacy_base;	/* base of mmap area in bottom-up allocations */
		...
		unsigned long task_size;	/* size of task vm space */
		pgd_t * pgd;				// [注] 顶级页表
		...
		atomic_t mm_users;
		...
#ifdef CONFIG_MMU
		atomic_long_t pgtables_bytes;	/* size of all page tables */
#endif
		int map_count;			/* number of VMAs */

		spinlock_t page_table_lock; /* Protects page tables and some
					     * counters
					     */
		...
		struct rw_semaphore mmap_lock;
		...
#ifdef CONFIG_PER_VMA_LOCK
		struct rcuwait vma_writer_wait;
		...
		seqcount_t mm_lock_seq;
#endif
		...
		unsigned long hiwater_rss; /* High-watermark of RSS usage */
		unsigned long hiwater_vm;  /* High-water virtual memory usage */

		unsigned long total_vm;	   /* Total pages mapped */
		unsigned long locked_vm;   /* Pages that have PG_mlocked set */
		atomic64_t    pinned_vm;   /* Refcount permanently increased */
		unsigned long data_vm;	   /* VM_WRITE & ~VM_SHARED & ~VM_STACK */
		unsigned long exec_vm;	   /* VM_EXEC & ~VM_WRITE & ~VM_STACK */
		unsigned long stack_vm;	   /* VM_STACK */
		vm_flags_t def_flags;
		...
		unsigned long start_code, end_code, start_data, end_data;
		unsigned long start_brk, brk, start_stack;
		unsigned long arg_start, arg_end, env_start, env_end;
		...
		struct percpu_counter rss_stat[NR_MM_COUNTERS];
		...
		/* Architecture-specific MM context */
		mm_context_t context;			// [注] x86 上含 ASID/PCID、LDT 等

		mm_flags_t flags; /* Must use mm_flags_* hlpers to access */
		...
```

两个引用计数的区别是理解 `mm_struct` 生命周期的关键：

| 计数 | 含义 | 归零时 |
|---|---|---|
| `mm_users` | 使用该地址空间的**用户**数（共享 mm 的线程、`get_task_mm()` 的临时持有者） | 调用 `exit_mmap()` 拆除所有 VMA 与页表，并释放一个 `mm_count` |
| `mm_count` | 对 `mm_struct` **结构体本身**的引用（`mm_users` 整体算 1 个，惰性 TLB 下借用该 mm 的内核线程各算 1 个） | 释放 `mm_struct` |

内核线程没有自己的用户地址空间（`task->mm == NULL`），被调度时借用上一个任务的 `active_mm`（第 3 章"惰性 TLB"），此时只增加 `mm_count`，保证页表在借用期间不被释放，但不阻止地址空间被拆除。

`mmap_lock` 是一把读写信号量，保护 VMA 树的结构与 VMA 字段。读者包括缺页处理、`/proc/PID/maps` 等；写者包括 `mmap()`、`munmap()`、`mprotect()`、`brk()` 等。它长期是多线程程序的扩展性瓶颈，这正是 per-VMA 锁要解决的问题。

`rss_stat[]` 记录驻留集大小（Resident Set Size，RSS），分为文件页、匿名页、交换项、共享内存四类（`MM_FILEPAGES`、`MM_ANONPAGES`、`MM_SWAPENTS`、`MM_SHMEMPAGES`），使用 per-CPU 计数器以避免缺页路径上的原子操作竞争。

### `struct vm_area_struct`

[`include/linux/mm_types.h:815-919`](../../linux/include/linux/mm_types.h#L815-L919)（v6.18.60，节选，删去部分注释）

```c
struct vm_area_struct {
	/* The first cache line has the info for VMA tree walking. */

	union {
		struct {
			/* VMA covers [vm_start; vm_end) addresses within mm */
			unsigned long vm_start;
			unsigned long vm_end;
		};
		freeptr_t vm_freeptr; /* Pointer used by SLAB_TYPESAFE_BY_RCU */
	};

	struct mm_struct *vm_mm;
	pgprot_t vm_page_prot;          /* Access permissions of this VMA. */

	union {
		const vm_flags_t vm_flags;
		vm_flags_t __private __vm_flags;
	};

#ifdef CONFIG_PER_VMA_LOCK
	unsigned int vm_lock_seq;
#endif
	struct list_head anon_vma_chain; /* Serialized by mmap_lock &
					  * page_table_lock */
	struct anon_vma *anon_vma;	/* Serialized by page_table_lock */

	/* Function pointers to deal with this struct. */
	const struct vm_operations_struct *vm_ops;

	/* Information about our backing store: */
	unsigned long vm_pgoff;		/* Offset (within vm_file) in PAGE_SIZE
					   units */
	struct file * vm_file;		/* File we map to (can be NULL). */
	void * vm_private_data;		/* was vm_pte (shared mem) */
	...
#ifdef CONFIG_PER_VMA_LOCK
	/* Unstable RCU readers are allowed to read this. */
	refcount_t vm_refcnt ____cacheline_aligned_in_smp;
	...
#endif
	struct {
		struct rb_node rb;
		unsigned long rb_subtree_last;
	} shared;
#ifdef CONFIG_ANON_VMA_NAME
	struct anon_vma_name *anon_name;
#endif
	struct vm_userfaultfd_ctx vm_userfaultfd_ctx;
	...
} __randomize_layout;
```

| 字段 | 含义 |
|---|---|
| `vm_start` / `vm_end` | 区间 `[vm_start, vm_end)`，均按页对齐 |
| `vm_flags` | `VM_READ`/`VM_WRITE`/`VM_EXEC`/`VM_SHARED`，以及 `VM_MAY*`（允许 `mprotect()` 升级到的权限）、`VM_GROWSDOWN`（栈）、`VM_LOCKED`、`VM_HUGEPAGE` 等 |
| `vm_page_prot` | 由 `vm_flags` 推导出的页表项保护位，缺页时直接写入 PTE |
| `vm_ops` | 回调表：`fault`（缺页时获取页）、`open`/`close`、`map_pages`（预映射）等 |
| `vm_file` / `vm_pgoff` | 文件映射的文件及起始页偏移；匿名映射 `vm_file` 为 NULL |
| `anon_vma` / `anon_vma_chain` | 匿名页的反向映射结构（4.9 节） |
| `shared` | 文件映射挂在 `address_space->i_mmap` 区间树上的节点（4.9 节） |

`vm_flags` 被声明为 `const` 联合体成员，修改必须通过 `vm_flags_set()` 等辅助函数，它们会断言调用者持有 VMA 写锁。

VMA 按映射类型可分为三类：

| 类型 | `vm_file` | `VM_SHARED` | 例子 |
|---|---|---|---|
| 私有匿名 | NULL | 否 | 堆、栈、`MAP_PRIVATE \| MAP_ANONYMOUS` |
| 共享匿名 | shmem 内部文件 | 是 | `MAP_SHARED \| MAP_ANONYMOUS`，`shmem_zero_setup()` 为其创建一个 shmem 文件 |
| 私有文件 | 文件 | 否 | 程序代码段、动态库；写入时写时复制 |
| 共享文件 | 文件 | 是 | `MAP_SHARED` 映射文件，修改写回文件 |

### maple tree：VMA 的组织

v6.1 之前，VMA 同时挂在一棵红黑树（按地址查找）和一个有序双链表（顺序遍历）上，并有一个 per-thread 的 VMA 缓存。v6.1 用 **maple tree**（`mm->mm_mt`）统一取代了三者。

maple tree 是一种为区间设计的 B 树变体：每个节点可容纳多个"区间 → 指针"的条目（64 位上一个节点 256 字节，最多 16 个槽位），树高很低，对 CPU 缓存友好；它原生支持 RCU 安全的无锁读取，这是 per-VMA 锁能够实现的前提。VMA 之间的空隙在树中被表示为 NULL 条目，因此"查找某地址之后的第一个 VMA"和"查找足够大的空隙"都是 O(log n) 操作。

`find_vma()` 返回第一个 `vm_end > addr` 的 VMA（注意它可能不包含 `addr`）：

[`mm/mmap.c:904-910`](../../linux/mm/mmap.c#L904-L910)（v6.18.60）

```c
struct vm_area_struct *find_vma(struct mm_struct *mm, unsigned long addr)
{
	unsigned long index = addr;

	mmap_assert_locked(mm);
	return mt_find(&mm->mm_mt, &index, ULONG_MAX);
}
```

需要"恰好包含 `addr` 的 VMA"时应使用 `vma_lookup()`（[`include/linux/mm.h:2663`](../../linux/include/linux/mm.h#L2663)）。内核代码中遍历 VMA 使用 `VMA_ITERATOR` / `for_each_vma()` 迭代器。

## 用户地址空间布局

x86_64 默认使用**自顶向下（top-down）** 的 mmap 布局：栈位于用户空间顶部向下增长，mmap 区从栈下方开始向下分配，堆（brk）从程序数据段之后向上增长：

```
 0x00007fffffffffff ┌────────────────────────────┐  TASK_SIZE（47 位，地址自下而上递增）
                    │ 栈（向下增长）  [stack]     │  ← start_stack（随机化）
                    ├────────────────────────────┤
                    │ 栈大小上限 + 随机化间隙     │  gap ≥ 128 MB
                    ├────────────────────────────┤  ← mmap_base（随机化）
                    │ mmap 区（向下分配）         │  共享库、匿名 mmap、文件映射
                    │            ↓               │
                    │                            │
                    │            ↑               │
                    │ 堆（向上增长）  [heap]      │  ← start_brk ~ brk（随机化）
                    ├────────────────────────────┤
                    │ .bss / .data / .text        │  ← start_code（PIE 时随机化）
 0x0000000000000000 └────────────────────────────┘
```

*图 4-11：x86_64 用户地址空间典型布局（top-down，地址自下而上递增）*

`mmap_base` 由 `arch_pick_mmap_layout()` 在 `execve()` 时确定：

[`arch/x86/mm/mmap.c:82-99`](../../linux/arch/x86/mm/mmap.c#L82-L99)（v6.18.60）

```c
static unsigned long mmap_base(unsigned long rnd, unsigned long task_size,
			       const struct rlimit *rlim_stack)
{
	unsigned long gap = rlim_stack->rlim_cur;
	unsigned long pad = stack_maxrandom_size(task_size) + stack_guard_gap;

	/* Values close to RLIM_INFINITY can overflow. */
	if (gap + pad > gap)
		gap += pad;

	/*
	 * Top of mmap area (just below the process stack).
	 * Leave an at least ~128 MB hole with possible stack randomization.
	 */
	gap = clamp(gap, SIZE_128M, (task_size / 6) * 5);

	return PAGE_ALIGN(task_size - gap - rnd);
}
```

即在栈的 rlimit（默认 8 MB）基础上加上栈随机化范围和栈保护间隙（`stack_guard_gap`，默认 256 页 = 1 MB），夹在 128 MB 与 5/6 用户空间之间，再减去随机偏移 `rnd`。`rnd` 来自 `arch_rnd(mmap64_rnd_bits)`，随机熵位数（以页为单位）由 `CONFIG_ARCH_MMAP_RND_BITS` 决定，本书配置为 32，运行时可通过 `/proc/sys/vm/mmap_rnd_bits` 调整。设置 `vm.legacy_va_layout=1` 或 `ADDR_COMPAT_LAYOUT` personality 会切换回传统的自底向上布局（[`arch/x86/mm/mmap.c:62-68`](../../linux/arch/x86/mm/mmap.c#L62-L68)）。

用户空间默认只使用低 47 位（128 TB，`DEFAULT_MAP_WINDOW`），即使 CPU 支持五级页表。程序必须显式传入高于 47 位的 `mmap()` 提示地址才会获得更高的地址，以兼容那些把指针高位挪作他用的旧程序（[`arch/x86/kernel/sys_x86_64.c:227-228`](../../linux/arch/x86/kernel/sys_x86_64.c#L227-L228)）。

## mmap 系统调用

### 调用链总览

```
sys_mmap()                                       arch/x86/kernel/sys_x86_64.c:89
└── ksys_mmap_pgoff()                            mm/mmap.c:566        // fget(fd)，MAP_HUGETLB 处理
    └── vm_mmap_pgoff()                          mm/util.c:565        // 持 mmap_lock 写锁
        ├── do_mmap()                            mm/mmap.c:334        // 参数检查、计算 vm_flags
        │   ├── __get_unmapped_area()            mm/mmap.c:812        // 选择地址
        │   │   └── arch_get_unmapped_area_topdown()  arch/x86/kernel/sys_x86_64.c:174
        │   │       └── vm_unmapped_area()       mm/mmap.c:663        // 在 maple tree 中找空隙
        │   └── mmap_region()                    mm/vma.c:2784
        │       └── __mmap_region()              mm/vma.c:2705
        │           ├── __mmap_setup()           // 解除重叠映射、内存提交检查
        │           ├── vma_merge_new_range()    mm/vma.c:1043        // 尝试与相邻 VMA 合并
        │           ├── __mmap_new_vma()         mm/vma.c:2518        // 分配新 VMA
        │           │   └── call_mmap() → file->f_op->mmap()
        │           └── __mmap_complete()        // 统计、VM_LOCKED 处理
        └── mm_populate()                        // MAP_POPULATE / VM_LOCKED：预先缺页
```

### `vm_mmap_pgoff()`：加锁

[`mm/util.c:565-589`](../../linux/mm/util.c#L565-L589)（v6.18.60）

```c
unsigned long vm_mmap_pgoff(struct file *file, unsigned long addr,
	unsigned long len, unsigned long prot,
	unsigned long flag, unsigned long pgoff)
{
	loff_t off = (loff_t)pgoff << PAGE_SHIFT;
	unsigned long ret;
	struct mm_struct *mm = current->mm;
	unsigned long populate;
	LIST_HEAD(uf);

	ret = security_mmap_file(file, prot, flag);
	if (!ret)
		ret = fsnotify_mmap_perm(file, prot, off, len);
	if (!ret) {
		if (mmap_write_lock_killable(mm))
			return -EINTR;
		ret = do_mmap(file, addr, len, prot, flag, 0, pgoff, &populate,
			      &uf);
		mmap_write_unlock(mm);
		userfaultfd_unmap_complete(mm, &uf);
		if (populate)
			mm_populate(ret, populate);
	}
	return ret;
}
```

`mm_populate()` 在释放写锁**之后**执行，它会以读锁遍历区间并对每页模拟缺页（`__mm_populate()` → `populate_vma_page_range()` → GUP），避免在长时间的预取过程中阻塞其他 mmap 操作。

### `do_mmap()`：参数检查与标志计算

`do_mmap()` 的主要工作是把用户传入的 `prot`、`flags` 翻译成 `vm_flags`，并做各种合法性检查：

[`mm/mmap.c:367-408`](../../linux/mm/mmap.c#L367-L408)（v6.18.60，节选）

```c
	/* Careful about overflows.. */
	len = PAGE_ALIGN(len);
	if (!len)
		return -ENOMEM;

	/* offset overflow? */
	if ((pgoff + (len >> PAGE_SHIFT)) < pgoff)
		return -EOVERFLOW;

	/* Too many mappings? */
	if (mm->map_count > sysctl_max_map_count)
		return -ENOMEM;
	...
	vm_flags |= calc_vm_prot_bits(prot, pkey) | calc_vm_flag_bits(file, flags) |
			mm->def_flags | VM_MAYREAD | VM_MAYWRITE | VM_MAYEXEC;

	/* Obtain the address to map to. we verify (or select) it and ensure
	 * that it represents a valid section of the address space.
	 */
	addr = __get_unmapped_area(file, addr, len, pgoff, flags, vm_flags);
	if (IS_ERR_VALUE(addr))
		return addr;
```

`sysctl_max_map_count`（`vm.max_map_count`，默认 65530）限制每个进程的 VMA 数量。对文件映射，还要检查文件的打开模式与映射权限是否相容：

[`mm/mmap.c:445-465`](../../linux/mm/mmap.c#L445-L465)（v6.18.60，节选）

```c
		case MAP_SHARED_VALIDATE:
			if (flags & ~flags_mask)
				return -EOPNOTSUPP;
			if (prot & PROT_WRITE) {
				if (!(file->f_mode & FMODE_WRITE))
					return -EACCES;
				if (IS_SWAPFILE(file->f_mapping->host))
					return -ETXTBSY;
			}
			...
			vm_flags |= VM_SHARED | VM_MAYSHARE;
			if (!(file->f_mode & FMODE_WRITE))
				vm_flags &= ~(VM_MAYWRITE | VM_SHARED);
			fallthrough;
```

以只读方式打开的文件做 `MAP_SHARED` 映射时，`VM_MAYWRITE` 被清除，之后 `mprotect(PROT_WRITE)` 会失败——`VM_MAY*` 标志就是"权限上限"。

对私有匿名映射，`pgoff` 被设为 `addr >> PAGE_SHIFT`（[`mm/mmap.c:537`](../../linux/mm/mmap.c#L537)），这样同一 anon_vma 中的匿名页可以用"虚拟页号"作为 `folio->index`，便于反向映射时计算地址（4.9 节）。

最后，`MAP_POPULATE` 或 `VM_LOCKED` 时设置 `*populate`，通知上层预先填充。

### 选择地址：`arch_get_unmapped_area_topdown()`

[`arch/x86/kernel/sys_x86_64.c:174-232`](../../linux/arch/x86/kernel/sys_x86_64.c#L174-L232)（v6.18.60，节选）

```c
arch_get_unmapped_area_topdown(struct file *filp, unsigned long addr0,
			  unsigned long len, unsigned long pgoff,
			  unsigned long flags, vm_flags_t vm_flags)
{
	...
	/* No address checking. See comment at mmap_address_hint_valid() */
	if (flags & MAP_FIXED)
		return addr;
	...
	/* requesting a specific address */
	if (addr) {
		addr &= PAGE_MASK;
		if (!mmap_address_hint_valid(addr, len))
			goto get_unmapped_area;

		vma = find_vma(mm, addr);
		if (!vma || addr + len <= vm_start_gap(vma))
			return addr;
	}
get_unmapped_area:

	info.flags = VM_UNMAPPED_AREA_TOPDOWN;
	info.length = len;
	if (!in_32bit_syscall() && (flags & MAP_ABOVE4G))
		info.low_limit = SZ_4G;
	else
		info.low_limit = PAGE_SIZE;

	info.high_limit = get_mmap_base(0);
	if (!(filp && is_file_hugepages(filp))) {
		info.start_gap = stack_guard_placement(vm_flags);
		info.align_offset = pgoff << PAGE_SHIFT;
	}
	...
	if (addr > DEFAULT_MAP_WINDOW && !in_32bit_syscall())
		info.high_limit += TASK_SIZE_MAX - DEFAULT_MAP_WINDOW;
	...
```

`MAP_FIXED` 直接使用调用者给定的地址（已有映射将被替换）；带提示地址时，如果该处空闲（包括与下一个 VMA 之间的栈保护间隙 `vm_start_gap()`）就采用；否则调用 `vm_unmapped_area()`，在 `[low_limit, mmap_base)` 之间自顶向下找第一个足够大的空隙。上层的 `__get_unmapped_area()`（[`mm/mmap.c:812`](../../linux/mm/mmap.c#L812)）还会对匿名映射尝试 THP 对齐：没有提示地址且长度为 2 MB 整数倍时，调用 `thp_get_unmapped_area_vmflags()` 选择 2 MB 对齐的地址（[`mm/mmap.c:844-849`](../../linux/mm/mmap.c#L844-L849)），使之后的缺页能够直接使用 PMD 大页（4.13 节）。

### 创建 VMA：`__mmap_region()`

[`mm/vma.c:2705-2750`](../../linux/mm/vma.c#L2705-L2750)（v6.18.60，节选）

```c
static unsigned long __mmap_region(struct file *file, unsigned long addr,
		unsigned long len, vm_flags_t vm_flags, unsigned long pgoff,
		struct list_head *uf)
{
	struct mm_struct *mm = current->mm;
	struct vm_area_struct *vma = NULL;
	const bool have_mmap_prepare = file && file->f_op->mmap_prepare;
	VMA_ITERATOR(vmi, mm, addr);
	MMAP_STATE(map, mm, &vmi, addr, len, pgoff, vm_flags, file);
	bool allocated_new;
	int error;
	...
	error = __mmap_setup(&map, uf);
	if (error)
		goto abort_munmap;
	if (have_mmap_prepare)
		error = call_mmap_prepare(&map);
	if (error)
		goto unacct_error;
	...
	/* Attempt to merge with adjacent VMAs... */
	if (map.prev || map.next) {
		VMG_MMAP_STATE(vmg, &map, /* vma = */ NULL);

		vma = vma_merge_new_range(&vmg);
	}

	/* ...but if we can't, allocate a new VMA. */
	allocated_new = !vma;
	if (allocated_new) {
		error = __mmap_new_vma(&map, &vma);
		if (error)
			goto unacct_error;
	}
	...
	__mmap_complete(&map, vma);

	return addr;
```

v6.11 起，VMA 操作的核心代码从 `mm/mmap.c` 拆分到 `mm/vma.c`，并以 `mmap_state`、`vma_merge_struct` 等状态对象组织，使逻辑可以在用户态单元测试框架（`tools/testing/vma/`）中独立测试。

`__mmap_setup()` 做两件事：一是若新区间与现有 VMA 重叠（`MAP_FIXED`），先收集要解除的 VMA（真正的解除推迟到新 VMA 建立之后，以便失败时回滚）；二是做**内存提交检查**（见下文）。

**VMA 合并**：如果新区间与前一个或后一个 VMA 相邻，且标志、文件、偏移、anon_vma、mempolicy、userfaultfd 上下文等都兼容，`vma_merge_new_range()` 会直接扩展相邻的 VMA，而不分配新的 VMA。这对 `brk()` 逐步扩展堆、连续的匿名 mmap 尤为重要，否则 VMA 数量会迅速膨胀。

**新建 VMA**：

[`mm/vma.c:2518-2567`](../../linux/mm/vma.c#L2518-L2567)（v6.18.60，节选）

```c
static int __mmap_new_vma(struct mmap_state *map, struct vm_area_struct **vmap)
{
	...
	vma = vm_area_alloc(map->mm);
	if (!vma)
		return -ENOMEM;

	vma_iter_config(vmi, map->addr, map->end);
	vma_set_range(vma, map->addr, map->end, map->pgoff);
	vm_flags_init(vma, map->vm_flags);
	vma->vm_page_prot = map->page_prot;

	if (vma_iter_prealloc(vmi, vma)) {
		error = -ENOMEM;
		goto free_vma;
	}

	if (map->file)
		error = __mmap_new_file_vma(map, vma);
	else if (map->vm_flags & VM_SHARED)
		error = shmem_zero_setup(vma);
	else
		vma_set_anonymous(vma);

	if (error)
		goto free_iter_vma;
	...
	/* Lock the VMA since it is modified after insertion into VMA tree */
	vma_start_write(vma);
	vma_iter_store_new(vmi, vma);
	map->mm->map_count++;
	vma_link_file(vma);
	...
```

- `vma_iter_prealloc()` 预先分配 maple tree 插入所需的全部节点，之后的 `vma_iter_store_new()` 保证不会失败；
- 文件映射调用 `__mmap_new_file_vma()`，其中执行文件系统的 `->mmap()` 回调，由文件系统设置 `vm_ops`（如通用的 `generic_file_vm_ops`，其 `fault` 为 `filemap_fault()`）；
- 私有匿名映射只需 `vma_set_anonymous()` 把 `vm_ops` 置 NULL——注意此时 `anon_vma` 仍为 NULL，要等第一次写缺页时才通过 `anon_vma_prepare()` 分配（4.9 节）；
- `vma_link_file()` 把文件映射挂入 `address_space->i_mmap` 区间树。

### 内存提交（Overcommit）

`mmap()` 并不立即分配物理内存，那么内核如何防止进程"承诺"使用远超实际内存的空间？答案是**提交记账（Commit Accounting）**：只有可能需要新物理页或交换空间的映射才计入，即私有可写映射：

[`mm/vma.c:2349-2359`](../../linux/mm/vma.c#L2349-L2359)（v6.18.60）

```c
static bool accountable_mapping(struct file *file, vm_flags_t vm_flags)
{
	/*
	 * hugetlb has its own accounting separate from the core VM
	 * VM_HUGETLB may not be set yet so we cannot check for that flag.
	 */
	if (file && is_file_hugepages(file))
		return false;

	return (vm_flags & (VM_NORESERVE | VM_SHARED | VM_WRITE)) == VM_WRITE;
}
```

共享文件映射的页可以写回文件，只读映射不会产生新页，因此不计入。计入的页数经 `security_vm_enough_memory_mm()` → `__vm_enough_memory()` 检查：

[`mm/util.c:930-966`](../../linux/mm/util.c#L930-L966)（v6.18.60，节选）

```c
int __vm_enough_memory(const struct mm_struct *mm, long pages, int cap_sys_admin)
{
	long allowed;
	unsigned long bytes_failed;

	vm_acct_memory(pages);

	/*
	 * Sometimes we want to use more memory than we have
	 */
	if (sysctl_overcommit_memory == OVERCOMMIT_ALWAYS)
		return 0;

	if (sysctl_overcommit_memory == OVERCOMMIT_GUESS) {
		if (pages > totalram_pages() + total_swap_pages)
			goto error;
		return 0;
	}

	allowed = vm_commit_limit();
	/*
	 * Reserve some for root
	 */
	if (!cap_sys_admin)
		allowed -= sysctl_admin_reserve_kbytes >> (PAGE_SHIFT - 10);
	...
	if (percpu_counter_read_positive(&vm_committed_as) < allowed)
		return 0;
```

| `vm.overcommit_memory` | 策略 |
|---|---|
| 0（`OVERCOMMIT_GUESS`，默认） | 只拒绝单次请求超过"全部内存 + 全部交换空间"的明显不合理请求 |
| 1（`OVERCOMMIT_ALWAYS`） | 从不拒绝 |
| 2（`OVERCOMMIT_NEVER`） | 严格记账：总提交量不得超过 `swap + RAM × overcommit_ratio%`（或 `overcommit_kbytes`） |

`/proc/meminfo` 中的 `Committed_AS` 即 `vm_committed_as` 的当前值，`CommitLimit` 是严格模式下的上限。在默认策略下，真正的内存不足要等到缺页时才会暴露，并最终由 OOM Killer 处理（4.12 节）。`MAP_NORESERVE` 在非严格模式下设置 `VM_NORESERVE`，跳过记账。

## brk：堆的扩展

`brk()` 是 `malloc()` 在小分配时使用的传统接口。它本质上是一种特殊的匿名 mmap：

[`mm/mmap.c:154-195`](../../linux/mm/mmap.c#L154-L195)（v6.18.60，节选）

```c
	newbrk = PAGE_ALIGN(brk);
	oldbrk = PAGE_ALIGN(mm->brk);
	if (oldbrk == newbrk) {
		mm->brk = brk;
		goto success;
	}

	/* Always allow shrinking brk. */
	if (brk <= mm->brk) {
		/* Search one past newbrk */
		vma_iter_init(&vmi, mm, newbrk);
		brkvma = vma_find(&vmi, oldbrk);
		if (!brkvma || brkvma->vm_start >= oldbrk)
			goto out; /* mapping intersects with an existing non-brk vma. */
		...
		mm->brk = brk;
		if (do_vmi_align_munmap(&vmi, brkvma, mm, newbrk, oldbrk, &uf,
					/* unlock = */ true))
			goto out;

		goto success_unlocked;
	}

	if (check_brk_limits(oldbrk, newbrk - oldbrk))
		goto out;

	/*
	 * Only check if the next VMA is within the stack_guard_gap of the
	 * expansion area
	 */
	vma_iter_init(&vmi, mm, oldbrk);
	next = vma_find(&vmi, newbrk + PAGE_SIZE + stack_guard_gap);
	if (next && newbrk + PAGE_SIZE > vm_start_gap(next))
		goto out;

	brkvma = vma_prev_limit(&vmi, mm->start_brk);
	/* Ok, looks good - let it rip. */
	if (do_brk_flags(&vmi, brkvma, oldbrk, newbrk - oldbrk, 0) < 0)
		goto out;
```

收缩时解除映射（释放对应物理页），扩展时由 `do_brk_flags()` 尽量直接扩大已有的堆 VMA。`mm->brk` 可以不按页对齐，但 VMA 边界总是页对齐的。失败时 `brk()` 返回原来的 `brk` 值而不是错误码，这是该系统调用历史遗留的接口约定。

## per-VMA 锁

### 动机

在多线程程序中，一个线程调用 `mmap()` 持有 `mmap_lock` 写锁时，所有其他线程的缺页都要等待（缺页需要读锁）。即使它们访问的是毫不相干的 VMA。对于大型数据库、JVM 等应用，这种串行化严重影响扩展性。

v6.4 引入的 per-VMA 锁（`CONFIG_PER_VMA_LOCK=y`）让缺页处理可以只锁住**单个 VMA**，完全不碰 `mmap_lock`。

### 实现思路

- 写者：修改 VMA 的操作仍需持有 `mmap_lock` 写锁，并在修改前对涉及的 VMA 调用 `vma_start_write()`，把 `vma->vm_lock_seq` 设为当前的 `mm->mm_lock_seq`，同时等待已有的 VMA 读者退出。写锁释放时 `mm_lock_seq` 递增，所有被"写锁定"的 VMA 自动解锁——不需要逐个解锁。
- 读者：缺页时在 RCU 保护下从 maple tree 查找 VMA，然后尝试 `vma_start_read()`：若 `vma->vm_lock_seq == mm->mm_lock_seq`（该 VMA 正被写），则失败；否则增加 `vm_refcnt` 获得读锁。

[`mm/mmap_lock.c:224-269`](../../linux/mm/mmap_lock.c#L224-L269)（v6.18.60）

```c
struct vm_area_struct *lock_vma_under_rcu(struct mm_struct *mm,
					  unsigned long address)
{
	MA_STATE(mas, &mm->mm_mt, address, address);
	struct vm_area_struct *vma;

retry:
	rcu_read_lock();
	vma = mas_walk(&mas);
	if (!vma) {
		rcu_read_unlock();
		goto inval;
	}

	vma = vma_start_read(mm, vma);
	if (IS_ERR_OR_NULL(vma)) {
		/* Check if the VMA got isolated after we found it */
		if (PTR_ERR(vma) == -EAGAIN) {
			count_vm_vma_lock_event(VMA_LOCK_MISS);
			/* The area was replaced with another one */
			mas_set(&mas, address);
			goto retry;
		}

		/* Failed to lock the VMA */
		goto inval;
	}
	/*
	 * At this point, we have a stable reference to a VMA: The VMA is
	 * locked and we know it hasn't already been isolated.
	 * From here on, we can access the VMA without worrying about which
	 * fields are accessible for RCU readers.
	 */
	rcu_read_unlock();

	/* Check if the vma we locked is the right one. */
	if (unlikely(address < vma->vm_start || address >= vma->vm_end)) {
		vma_end_read(vma);
		goto inval;
	}

	return vma;

inval:
	count_vm_vma_lock_event(VMA_LOCK_ABORT);
	return NULL;
}
```

VMA 由 `SLAB_TYPESAFE_BY_RCU` 的缓存分配（这就是 `vm_area_struct` 第一个字段与 `vm_freeptr` 共用联合体的原因），即使 VMA 在 RCU 读期间被释放并重新分配给别处，其内存仍是一个合法的 VMA。因此获得锁之后必须重新检查地址范围。`lock_vma_under_rcu()` 返回 NULL 时，缺页处理回退到传统的 `mmap_lock` 读锁路径（4.8 节）。

## 动手实验

查看进程的 VMA 列表：

```bash
cat /proc/self/maps
# 地址范围              权限 偏移     设备  inode  路径
# 5581c3a00000-5581c3a02000 r--p 00000000 fd:01 1835  /usr/bin/cat
# 7ffd4e3a1000-7ffd4e3c2000 rw-p 00000000 00:00 0     [stack]
cat /proc/self/smaps | head -25     # 每个 VMA 的 RSS、PSS、匿名页、交换量等
cat /proc/self/status | grep -E "^Vm|^Rss"
```

`p` 表示私有映射，`s` 表示共享映射。观察 mmap 不立即占用物理内存：

```bash
python3 - <<'EOF'
import mmap, os, re
def rss():
    return re.search(r"VmRSS:\s+(\d+)", open("/proc/self/status").read()).group(1)
print("before :", rss(), "kB")
m = mmap.mmap(-1, 1 << 30)            # 1 GB 匿名映射
print("mmap   :", rss(), "kB")          # RSS 几乎不变
for i in range(0, 256 << 20, 4096):     # 只触碰前 256 MB
    m[i] = 1
print("touched:", rss(), "kB")          # RSS 增加约 256 MB
EOF
```

统计 per-VMA 锁的命中情况（需 `CONFIG_PER_VMA_LOCK_STATS`，本书配置未开启，此时下列计数不存在）：

```bash
grep -E "vma_lock" /proc/vmstat
```

查看提交记账：

```bash
grep -E "Commit" /proc/meminfo
cat /proc/sys/vm/overcommit_memory /proc/sys/vm/max_map_count
```

## 小结

- `mm_struct` 描述进程地址空间，`mm_users` 管理地址空间内容的生命周期，`mm_count` 管理结构体本身的生命周期。
- VMA 记录一段连续地址的权限与后备存储；`mmap()`/`brk()` 只创建 VMA，物理内存在缺页时按需分配。
- v6.1 起 VMA 组织在 maple tree 中，取代了红黑树 + 链表 + VMA 缓存，并支持 RCU 无锁查找。
- x86_64 默认 top-down 布局，`mmap_base` 位于栈下方至少 128 MB 处并随机化；用户空间默认限制在 47 位。
- mmap 的核心路径是：检查参数 → 计算 `vm_flags` → 选地址 → 解除重叠映射 → 提交记账 → 合并或新建 VMA → 调用文件的 `->mmap()`。
- 只有私有可写映射计入提交量，默认的 `OVERCOMMIT_GUESS` 策略几乎不拒绝请求。
- per-VMA 锁让缺页处理可以绕过 `mmap_lock`，依赖 maple tree 的 RCU 安全性和 `SLAB_TYPESAFE_BY_RCU` 的 VMA 缓存。

## 参考

- `linux/Documentation/mm/process_addrs.rst`（VMA 锁规则的权威说明）
- `linux/Documentation/mm/overcommit-accounting.rst`
- `linux/Documentation/core-api/maple_tree.rst`
- `linux/Documentation/filesystems/proc.rst`（`/proc/PID/maps`、`smaps` 字段说明）
- Liam R. Howlett, "Introducing maple trees", LWN: https://lwn.net/Articles/845507/
