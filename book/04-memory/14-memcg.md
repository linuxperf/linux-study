# 4.14 内存控制组（memcg）

> **本节源码**：`include/linux/memcontrol.h`、`include/linux/page_counter.h`、`mm/memcontrol.c`、`mm/page_counter.c`、`mm/vmscan.c`、`mm/oom_kill.c`
> **关键函数**：`__mem_cgroup_charge()`、`charge_memcg()`、`try_charge_memcg()`、`consume_stock()`、`page_counter_try_charge()`、`commit_charge()`、`__mem_cgroup_handle_over_high()`、`mem_cgroup_oom()`、`mem_cgroup_calculate_protection()`、`effective_protection()`、`obj_cgroup_charge()`
> **前置知识**：4.10 页面回收、4.12 OOM Killer；了解 cgroup v2 的基本概念

## 概述

容器、systemd 服务等场景需要限制和隔离一组进程的内存使用：某个服务内存泄漏时，应只影响它自己，而不是把整个系统拖入 OOM。**内存控制组（Memory Cgroup，memcg）** 为每个 cgroup 记录其使用的内存量，在超过限制时只回收（或 OOM 杀死）该 cgroup 内部的内存。

memcg 的工作可以分为四部分：**计费（Charge）**——每次分配用户页、page cache 页或带 `__GFP_ACCOUNT` 的内核对象时，把它计入当前任务所属的 memcg；**限制**——超过 `memory.high` 时节流、超过 `memory.max` 时回收直至 OOM；**保护**——`memory.min` / `memory.low` 使 cgroup 在全局回收中免于被过度回收；**按 cgroup 组织 LRU**——每个 memcg 在每个节点上有自己的 lruvec，回收时能够精确选择目标。

本书配置启用 `CONFIG_MEMCG=y`，未启用 cgroup v1 接口（`CONFIG_MEMCG_V1` 未设置），因此本节只讨论 cgroup v2。

## 核心数据结构

### `struct mem_cgroup`

```c
// include/linux/memcontrol.h:189-324（v6.18.60，节选）
struct mem_cgroup {
	struct cgroup_subsys_state css;

	/* Private memcg ID. Used to ID objects that outlive the cgroup */
	struct mem_cgroup_id id;

	/* Accounted resources */
	struct page_counter memory;		/* Both v1 & v2 */

	union {
		struct page_counter swap;	/* v2 only */
		struct page_counter memsw;	/* v1 only */
	};
	...
	/* Range enforcement for interrupt charges */
	struct work_struct high_work;

#ifdef CONFIG_ZSWAP
	unsigned long zswap_max;
	...
	bool zswap_writeback;
#endif
	...
	/*
	 * Should the OOM killer kill all belonging tasks, had it kill one?
	 */
	bool oom_group;

	int swappiness;
	...
	/* memory.stat */
	struct memcg_vmstats	*vmstats;

	/* memory.events */
	atomic_long_t		memory_events[MEMCG_NR_MEMORY_EVENTS];
	atomic_long_t		memory_events_local[MEMCG_NR_MEMORY_EVENTS];
	...
	struct obj_cgroup __rcu	*objcg;
	struct obj_cgroup	*orig_objcg;
	/* list of inherited objcgs, protected by objcg_lock */
	struct list_head objcg_list;

	struct memcg_vmstats_percpu __percpu *vmstats_percpu;
	...
#ifdef CONFIG_TRANSPARENT_HUGEPAGE
	struct deferred_split deferred_split_queue;
#endif

#ifdef CONFIG_LRU_GEN_WALKS_MMU
	/* per-memcg mm_struct list */
	struct lru_gen_mm_list mm_list;
#endif
	...
	struct mem_cgroup_per_node *nodeinfo[];
};
```

| 字段 | 对应接口 / 用途 |
|---|---|
| `memory` | 内存用量与 `memory.min`/`low`/`high`/`max`（`page_counter`） |
| `swap` | 交换用量与 `memory.swap.high`/`max` |
| `high_work` | 中断上下文中超过 high 时延迟执行回收的工作项 |
| `zswap_max` / `zswap_writeback` | `memory.zswap.max` / `memory.zswap.writeback`（4.11 节） |
| `oom_group` | `memory.oom.group`：OOM 时整组杀死（4.12 节） |
| `vmstats` | `memory.stat` 的各项统计（per-CPU 累加，按需汇总） |
| `memory_events` | `memory.events` 中 low/high/max/oom/oom_kill 等事件计数 |
| `objcg` | 内核对象计费的间接层（见后文） |
| `deferred_split_queue` | 本 memcg 的 THP 延迟拆分队列（4.13 节） |
| `nodeinfo[]` | 每个 NUMA 节点一个 `mem_cgroup_per_node`，内含该节点的 lruvec |

### `page_counter`：层级计数器

```c
// include/linux/page_counter.h:10-44（v6.18.60）
struct page_counter {
	/*
	 * Make sure 'usage' does not share cacheline with any other field in
	 * v2. The memcg->memory.usage is a hot member of struct mem_cgroup.
	 */
	atomic_long_t usage;
	unsigned long failcnt; /* v1-only field */

	CACHELINE_PADDING(_pad1_);

	/* effective memory.min and memory.min usage tracking */
	unsigned long emin;
	atomic_long_t min_usage;
	atomic_long_t children_min_usage;

	/* effective memory.low and memory.low usage tracking */
	unsigned long elow;
	atomic_long_t low_usage;
	atomic_long_t children_low_usage;

	unsigned long watermark;
	/* Latest cg2 reset watermark */
	unsigned long local_watermark;

	/* Keep all the read most fields in a separete cacheline. */
	CACHELINE_PADDING(_pad2_);

	bool protection_support;
	bool track_failcnt;
	unsigned long min;
	unsigned long low;
	unsigned long high;
	unsigned long max;
	struct page_counter *parent;
} ____cacheline_internodealigned_in_smp;
```

`page_counter` 以页为单位计数，`parent` 指向父 cgroup 的计数器。cgroup v2 的语义是**层级的**：子 cgroup 的用量同时计入所有祖先。`watermark` 即 `memory.peak`。`emin`/`elow` 是经层级换算后的**有效**保护值（见"保护"一节）。

### per-memcg per-node lruvec

```c
// include/linux/memcontrol.h:87-122（v6.18.60，节选）
struct mem_cgroup_per_node {
	/* Keep the read-only fields at the start */
	struct mem_cgroup	*memcg;		/* Back pointer, we cannot */
						/* use container_of	   */

	struct lruvec_stats_percpu __percpu	*lruvec_stats_percpu;
	struct lruvec_stats			*lruvec_stats;
	struct shrinker_info __rcu	*shrinker_info;
	...
	/* Fields which get updated often at the end. */
	struct lruvec		lruvec;
	CACHELINE_PADDING(_pad2_);
	unsigned long		lru_zone_size[MAX_NR_ZONES][NR_LRU_LISTS];
	struct mem_cgroup_reclaim_iter	iter;
	...
};
```

启用 memcg 后，`pgdat->__lruvec` 不再使用（4.1 节）。每个 folio 被计费后，加入 LRU 时进入"其 memcg 在其节点上"的 lruvec（`folio_lruvec()` 通过 `folio->memcg_data` 与 `folio_nid()` 找到）。这样，memcg 回收只需扫描该 memcg（及其子孙）的 lruvec；全局回收则通过 MGLRU 的 memcg LRU（`shrink_many()`，4.10 节）在各 memcg 之间轮转。`shrinker_info` 记录该 memcg 在该节点上哪些 shrinker 有对象可回收。

## 计费

### 入口

用户页（匿名页、page cache、shmem）在分配后由调用者显式计费，例如匿名缺页中的 `alloc_anon_folio()` 和 THP 缺页中的 `vma_alloc_anon_folio_pmd()` 都调用 `mem_cgroup_charge()`（4.8、4.13 节）：

```c
// mm/memcontrol.c:4731-4757（v6.18.60）
static int charge_memcg(struct folio *folio, struct mem_cgroup *memcg,
			gfp_t gfp)
{
	int ret;

	ret = try_charge(memcg, gfp, folio_nr_pages(folio));
	if (ret)
		goto out;

	css_get(&memcg->css);
	commit_charge(folio, memcg);
	memcg1_commit_charge(folio, memcg);
out:
	return ret;
}

int __mem_cgroup_charge(struct folio *folio, struct mm_struct *mm, gfp_t gfp)
{
	struct mem_cgroup *memcg;
	int ret;

	memcg = get_mem_cgroup_from_mm(mm);
	ret = charge_memcg(folio, memcg, gfp);
	css_put(&memcg->css);

	return ret;
}
```

`get_mem_cgroup_from_mm()` 找到 `mm->owner` 任务所在的 memcg（`CONFIG_MEMCG` 下 mm 记录一个 owner）。`commit_charge()` 把 memcg 指针写入 `folio->memcg_data`：

```c
// mm/memcontrol.c:2517-2528（v6.18.60）
static void commit_charge(struct folio *folio, struct mem_cgroup *memcg)
{
	VM_BUG_ON_FOLIO(folio_memcg_charged(folio), folio);
	/*
	 * Any of the following ensures page's memcg stability:
	 *
	 * - the page lock
	 * - LRU isolation
	 * - exclusive reference
	 */
	folio->memcg_data = (unsigned long)memcg;
}
```

**计费归属于首次分配者**：一个 page cache 页被 cgroup A 的进程读入后计入 A，即使之后 cgroup B 的进程也频繁访问它，它仍然属于 A，直到被回收。folio 释放时（`mem_cgroup_uncharge_folios()`）才反向扣减。这一"首次访问者付费"的规则简单高效，但在共享文件的场景下可能不公平。

### `try_charge_memcg()`

```c
// mm/memcontrol.c:2300-2337（v6.18.60，节选）
static int try_charge_memcg(struct mem_cgroup *memcg, gfp_t gfp_mask,
			    unsigned int nr_pages)
{
	unsigned int batch = max(MEMCG_CHARGE_BATCH, nr_pages);
	int nr_retries = MAX_RECLAIM_RETRIES;
	...
retry:
	if (consume_stock(memcg, nr_pages))
		return 0;
	...
	if (!do_memsw_account() ||
	    page_counter_try_charge(&memcg->memsw, batch, &counter)) {
		if (page_counter_try_charge(&memcg->memory, batch, &counter))
			goto done_restock;
		...
		mem_over_limit = mem_cgroup_from_counter(counter, memory);
	} else {
		...
	}

	if (batch > nr_pages) {
		batch = nr_pages;
		goto retry;
	}
```

**per-CPU 预扣（stock）**：`page_counter` 是层级的原子计数器，每次计费都要从当前 cgroup 一路原子加到根，开销不小。因此计费总是一次向 `page_counter` 申请 `MEMCG_CHARGE_BATCH`（64 页，`include/linux/memcontrol.h:331`）的额度，多出的部分存入每 CPU 的 `memcg_stock`，下次同一 memcg 的计费直接从 stock 中扣减：

```c
// mm/memcontrol.c:1797-1825（v6.18.60）
static bool consume_stock(struct mem_cgroup *memcg, unsigned int nr_pages)
{
	struct memcg_stock_pcp *stock;
	uint8_t stock_pages;
	bool ret = false;
	int i;

	if (nr_pages > MEMCG_CHARGE_BATCH ||
	    !local_trylock(&memcg_stock.lock))
		return ret;

	stock = this_cpu_ptr(&memcg_stock);

	for (i = 0; i < NR_MEMCG_STOCK; ++i) {
		if (memcg != READ_ONCE(stock->cached[i]))
			continue;

		stock_pages = READ_ONCE(stock->nr_pages[i]);
		if (stock_pages >= nr_pages) {
			WRITE_ONCE(stock->nr_pages[i], stock_pages - nr_pages);
			ret = true;
		}
		break;
	}

	local_unlock(&memcg_stock.lock);

	return ret;
}
```

每 CPU 的 stock 可以同时缓存 `NR_MEMCG_STOCK`（7）个 memcg 的额度（`mm/memcontrol.c:1748-1758`），适应一个 CPU 上交替运行多个 cgroup 任务的情况。代价是 memcg 的用量统计可能有每 CPU 最多 64 页的误差。

`page_counter_try_charge()` 从当前计数器逐级向上原子增加，任一层超过 `max` 就回滚并返回失败的那一层：

```c
// mm/page_counter.c:118-154（v6.18.60，节选）
bool page_counter_try_charge(struct page_counter *counter,
			     unsigned long nr_pages,
			     struct page_counter **fail)
{
	...
	for (c = counter; c; c = c->parent) {
		long new;
		...
		new = atomic_long_add_return(nr_pages, &c->usage);
		if (new > c->max) {
			atomic_long_sub(nr_pages, &c->usage);
			...
			*fail = c;
			goto failed;
		}
		...
```

注意这里"先加后检查"的乐观做法：没有使用 CAS 循环，代价是并发时可能短暂误判，误差不超过一次计费的大小。

### 超过 `memory.max`：回收、重试、OOM

计费失败时，`mem_over_limit` 指向超限的那一层祖先（可能不是当前 cgroup 本身）：

```c
// mm/memcontrol.c:2346-2446（v6.18.60，节选）
	if (unlikely(current->flags & PF_MEMALLOC))
		goto force;

	if (unlikely(task_in_memcg_oom(current)))
		goto nomem;

	if (!gfpflags_allow_blocking(gfp_mask))
		goto nomem;
	...
	__memcg_memory_event(mem_over_limit, MEMCG_MAX, allow_spinning);
	raised_max_event = true;

	psi_memstall_enter(&pflags);
	nr_reclaimed = try_to_free_mem_cgroup_pages(mem_over_limit, nr_pages,
						    gfp_mask, reclaim_options, NULL);
	psi_memstall_leave(&pflags);

	if (mem_cgroup_margin(mem_over_limit) >= nr_pages)
		goto retry;

	if (!drained) {
		drain_all_stock(mem_over_limit);
		drained = true;
		goto retry;
	}

	if (gfp_mask & __GFP_NORETRY)
		goto nomem;
	...
	if (nr_reclaimed && nr_pages <= (1 << PAGE_ALLOC_COSTLY_ORDER))
		goto retry;

	if (nr_retries--)
		goto retry;

	if (gfp_mask & __GFP_RETRY_MAYFAIL)
		goto nomem;

	/* Avoid endless loop for tasks bypassed by the oom killer */
	if (passed_oom && task_is_dying())
		goto nomem;

	/*
	 * keep retrying as long as the memcg oom killer is able to make
	 * a forward progress or bypass the charge if the oom killer
	 * couldn't make any progress.
	 */
	if (mem_cgroup_oom(mem_over_limit, gfp_mask,
			   get_order(nr_pages * PAGE_SIZE))) {
		passed_oom = true;
		nr_retries = MAX_RECLAIM_RETRIES;
		goto retry;
	}
nomem:
	...
	if (!(gfp_mask & (__GFP_NOFAIL | __GFP_HIGH)))
		return -ENOMEM;
force:
	...
	page_counter_charge(&memcg->memory, nr_pages);
	...
	return 0;
```

流程与全局慢速路径（4.4 节）如出一辙：

1. 回收路径中的分配（`PF_MEMALLOC`）直接强制计费（`force`），允许暂时超限，避免递归；
2. 不能阻塞的分配直接失败（`GFP_ATOMIC` 带 `__GFP_HIGH` 时强制计费）；
3. 记录 `max` 事件，调用 `try_to_free_mem_cgroup_pages()`（`mm/vmscan.c`）**只回收 `mem_over_limit` 子树**中的页；
4. 回收后若有余量则重试；还会清空所有 CPU 上该 memcg 的 stock，归还预扣的额度；
5. 最多重试 `MAX_RECLAIM_RETRIES`（16）次，仍失败则调用 `mem_cgroup_oom()`（`mm/memcontrol.c:1661`）→ `mem_cgroup_out_of_memory()`（`mm/memcontrol.c:1628`）→ `out_of_memory()`，在该 memcg 内选择牺牲者（4.12 节）。

memcg OOM 只影响超限 cgroup 内部的进程，系统其余部分不受影响。这是容器内存隔离的核心保证。

### 超过 `memory.high`：节流

`memory.max` 是硬限制，触及时会 OOM；`memory.high` 是**软限制**，超过时不会 OOM，而是让分配者自己承担回收工作并被**节流**（降速），给用户态的管理程序留出反应时间。计费成功后：

```c
// mm/memcontrol.c:2448-2505（v6.18.60，节选）
done_restock:
	if (batch > nr_pages)
		refill_stock(memcg, batch - nr_pages);
	...
	do {
		bool mem_high, swap_high;

		mem_high = page_counter_read(&memcg->memory) >
			READ_ONCE(memcg->memory.high);
		swap_high = page_counter_read(&memcg->swap) >
			READ_ONCE(memcg->swap.high);

		/* Don't bother a random interrupted task */
		if (!in_task()) {
			if (mem_high) {
				schedule_work(&memcg->high_work);
				break;
			}
			continue;
		}

		if (mem_high || swap_high) {
			...
			current->memcg_nr_pages_over_high += batch;
			set_notify_resume(current);
			break;
		}
	} while ((memcg = parent_mem_cgroup(memcg)));
	...
	if (current->memcg_nr_pages_over_high > MEMCG_CHARGE_BATCH &&
	    !(current->flags & PF_MEMALLOC) &&
	    gfpflags_allow_blocking(gfp_mask))
		__mem_cgroup_handle_over_high(gfp_mask);
	return 0;
```

超过 high 时，计费本身仍然成功，但在任务上记录"超出量"并设置 `TIF_NOTIFY_RESUME`。任务**返回用户态前**调用 `mem_cgroup_handle_over_high()`——此时不持有任何内核锁，适合做回收和睡眠：

```c
// mm/memcontrol.c:2210-2297（v6.18.60，节选）
void __mem_cgroup_handle_over_high(gfp_t gfp_mask)
{
	...
	nr_reclaimed = reclaim_high(memcg,
				    in_retry ? SWAP_CLUSTER_MAX : nr_pages,
				    gfp_mask);

	/*
	 * memory.high is breached and reclaim is unable to keep up. Throttle
	 * allocators proactively to slow down excessive growth.
	 */
	penalty_jiffies = calculate_high_delay(memcg, nr_pages,
					       mem_find_max_overage(memcg));

	penalty_jiffies += calculate_high_delay(memcg, nr_pages,
						swap_find_max_overage(memcg));

	/*
	 * Clamp the max delay per usermode return so as to still keep the
	 * application moving forwards and also permit diagnostics, albeit
	 * extremely slowly.
	 */
	penalty_jiffies = min(penalty_jiffies, MEMCG_MAX_HIGH_DELAY_JIFFIES);
	...
	if (nr_reclaimed || nr_retries--) {
		in_retry = true;
		goto retry_reclaim;
	}
	...
	psi_memstall_enter(&pflags);
	schedule_timeout_killable(penalty_jiffies);
	psi_memstall_leave(&pflags);
	...
```

先回收超出的部分；回收跟不上时，按超出比例的平方计算惩罚延迟（`calculate_high_delay()`，`mm/memcontrol.c:2173`），让任务睡眠，最长 2 秒（`MEMCG_MAX_HIGH_DELAY_JIFFIES`，`mm/memcontrol.c:2073`）。睡眠时间计入 PSI，因此 systemd-oomd 等工具可以观察到压力并采取行动。

## 保护：`memory.min` 与 `memory.low`

限制防止一个 cgroup 占用过多，保护则防止一个 cgroup 在**全局**内存压力下被其他 cgroup 挤占：

- `memory.min`：**硬保护**。用量低于有效 min 时，任何回收都不会回收该 cgroup 的内存，即使导致 OOM；
- `memory.low`：**尽力保护**。用量低于有效 low 时，回收先跳过它，只有在其他 cgroup 都无可回收时才回收它，并记录 `low` 事件。

4.10 节 `shrink_one()` 中的 `mem_cgroup_below_min()` 与 `mem_cgroup_below_low()` 正是这两种检查。

### 有效保护值

保护值是层级的，子 cgroup 不能比父 cgroup 获得更多保护；而且兄弟 cgroup 声明的保护总和可能超过父 cgroup 的保护（超额声明）。`mem_cgroup_calculate_protection()`（`mm/memcontrol.c:4716`）在每次回收开始时，自顶向下为每个 cgroup 计算有效值 `emin`/`elow`：

```c
// mm/page_counter.c:337-359（v6.18.60，节选）
static unsigned long effective_protection(unsigned long usage,
					  unsigned long parent_usage,
					  unsigned long setting,
					  unsigned long parent_effective,
					  unsigned long siblings_protected,
					  bool recursive_protection)
{
	unsigned long protected;
	unsigned long ep;

	protected = min(usage, setting);
	/*
	 * If all cgroups at this level combined claim and use more
	 * protection than what the parent affords them, distribute
	 * shares in proportion to utilization.
	 *
	 * We are using actual utilization rather than the statically
	 * claimed protection in order to be work-conserving: claimed
	 * but unused protection is available to siblings that would
	 * otherwise get a smaller chunk than what they claimed.
	 */
	if (siblings_protected > parent_effective)
		return protected * parent_effective / siblings_protected;
	...
```

规则（详见 `mm/page_counter.c:300-336` 的注释）：

1. 第一层的有效保护等于其设置值；
2. 下层的有效保护不超过父层的有效保护；
3. 兄弟间的保护声明总和超过父层有效保护时，按各自**实际受保护的用量**（`min(usage, setting)`）比例分配父层的保护额度——没用到的保护额度让给兄弟，这是"工作守恒"的；
4. 挂载选项 `memory_recursive_prot` 启用时，父层未分配出去的"浮动"保护按子 cgroup 的用量比例分配，使整个子树作为整体受保护。

`min_usage` / `low_usage` 等字段由 `propagate_protected_usage()` 在计费路径上实时维护，避免回收时重新遍历整棵树。

## 内核对象计费：obj_cgroup

带 `__GFP_ACCOUNT`（`GFP_KERNEL_ACCOUNT`）的内核分配也会计入 memcg，例如进程的页表、内核栈、管道缓冲区、socket 缓冲区、dentry/inode 等。这类内存与用户页有两点不同：

1. **slab 对象远小于一页**，不能以页为单位计费；
2. **生命周期可能长于 cgroup**：cgroup 被删除后，它分配的 dentry 可能仍在 dcache 中长期存在。如果每个对象都直接引用 `mem_cgroup`，被删除的 cgroup 就无法释放（"僵尸 memcg"问题，在 v5.9 之前曾导致大量内存泄漏）。

v5.9 引入的解决方案是 **obj_cgroup**：一个轻量的间接层对象，slab 对象（通过 `slab->obj_exts`，`CONFIG_SLAB_OBJ_EXT=y`）和内核页（`page->memcg_data` 低位标记为 objcg）引用的是 `obj_cgroup` 而不是 `mem_cgroup`。cgroup 被删除时，只需把它的 `objcg` **重新指向父 memcg**（reparenting），所有存量对象的计费就自动转移到父 cgroup，原 memcg 可以立即释放。

`obj_cgroup_charge()`（`mm/memcontrol.c:3126`）以**字节**为单位计费，内部维护每 CPU 的字节级 stock（`consume_obj_stock()`，`mm/memcontrol.c:2936`）：不足一页的部分在 stock 中累计，凑满一页才向 `page_counter` 计费一次。整页的内核分配（`__GFP_ACCOUNT` 的 `alloc_pages()`）则在 4.3 节 `__alloc_frozen_pages_noprof()` 的末尾经 `__memcg_kmem_charge_page()`（`mm/memcontrol.c:2852`）计费。4.5 节 kmalloc 的 `KMALLOC_CGROUP` 缓存（`kmalloc-cg-*`）专门存放这类计费对象，避免与非计费对象混在同一 slab 中。

## 常用接口

| 文件 | 含义 |
|---|---|
| `memory.current` / `memory.peak` | 当前 / 峰值用量（含子孙） |
| `memory.min` / `memory.low` | 硬 / 尽力保护 |
| `memory.high` / `memory.max` | 节流阈值 / 硬上限 |
| `memory.swap.current` / `memory.swap.max` | 交换用量 / 上限 |
| `memory.zswap.current` / `memory.zswap.max` | zswap 用量 / 上限 |
| `memory.oom.group` | OOM 时整组杀死 |
| `memory.events` | `low`、`high`、`max`、`oom`、`oom_kill` 等事件计数 |
| `memory.stat` | `anon`、`file`、`kernel`、`slab`、`sock`、`pgfault`、`workingset_refault_*` 等细分统计 |
| `memory.pressure` | 该 cgroup 的 PSI |
| `memory.reclaim` | 主动回收：写入字节数，内核尝试从该 cgroup 回收这么多内存（v5.19 引入） |

## 动手实验

在 dev 虚拟机中创建 cgroup 并观察计费、节流与 OOM（root，cgroup v2）：

```bash
cat /sys/fs/cgroup/cgroup.controllers          # 应包含 memory
sudo mkdir /sys/fs/cgroup/demo
echo "+memory" | sudo tee /sys/fs/cgroup/cgroup.subtree_control

# 1. 计费：读入一个大文件，page cache 计入该 cgroup
sudo sh -c 'echo $$ > /sys/fs/cgroup/demo/cgroup.procs; cat /usr/lib/x86_64-linux-gnu/*.so* > /dev/null'
grep -E "^(anon|file|kernel|slab) " /sys/fs/cgroup/demo/memory.stat

# 2. memory.high 节流：分配超过 high 的内存，观察耗时与 PSI
echo 100M | sudo tee /sys/fs/cgroup/demo/memory.high
sudo sh -c 'echo $$ > /sys/fs/cgroup/demo/cgroup.procs; time python3 -c "b=bytearray(300<<20)"'
cat /sys/fs/cgroup/demo/memory.events /sys/fs/cgroup/demo/memory.pressure

# 3. 主动回收
echo 50M | sudo tee /sys/fs/cgroup/demo/memory.reclaim
cat /sys/fs/cgroup/demo/memory.current

sudo rmdir /sys/fs/cgroup/demo
```

使用 systemd 可以更方便地完成同样的实验：

```bash
systemd-run --user --scope -p MemoryHigh=100M -p MemoryMax=200M python3 -c "b=bytearray(300<<20)"
```

## 小结

- memcg 以层级 `page_counter` 记录每个 cgroup 的内存与交换用量，子 cgroup 的用量计入所有祖先；本书配置只使用 cgroup v2。
- 用户页在分配后由 `mem_cgroup_charge()` 计费并写入 `folio->memcg_data`，归属首次分配者；每 CPU stock 以 64 页为批次预扣额度，降低原子操作开销。
- 超过 `memory.max` 时只回收超限子树，重试失败后在该 memcg 内触发 OOM；超过 `memory.high` 时在返回用户态前回收并按超出程度节流（最长 2 秒）。
- `memory.min`/`low` 提供硬/尽力保护，有效保护值按层级和实际用量比例分配，回收时据此跳过受保护的 cgroup。
- 每个 memcg 在每个节点上有自己的 lruvec 与 shrinker 状态，使回收能精确定位到 cgroup。
- 内核对象经 obj_cgroup 间接计费，cgroup 删除时通过重定向 objcg 把存量计费转移给父 cgroup，解决了僵尸 memcg 问题。

## 参考

- `linux/Documentation/admin-guide/cgroup-v2.rst`（Memory 一节）
- `linux/mm/page_counter.c` 中关于有效保护的注释
- Roman Gushchin, "The new cgroup slab memory controller" 补丁系列（v5.9 引入 obj_cgroup）
- Johannes Weiner, "mm: memcontrol: memory.high throttling" 相关提交
