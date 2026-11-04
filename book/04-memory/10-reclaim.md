# 4.10 页面回收：LRU、MGLRU 与 kswapd

> **本节源码**：`mm/vmscan.c`、`include/linux/mmzone.h`、`include/linux/mm_inline.h`、`mm/workingset.c`、`mm/shrinker.c`、`include/linux/shrinker.h`
> **关键函数**：`kswapd()`、`balance_pgdat()`、`pgdat_balanced()`、`shrink_node()`、`lru_gen_shrink_node()`、`try_to_shrink_lruvec()`、`get_nr_to_scan()`、`try_to_inc_max_seq()`、`evict_folios()`、`sort_folio()`、`shrink_folio_list()`、`folio_check_references()`、`shrink_slab()`
> **前置知识**：4.4 水位线与慢速路径、4.9 写时复制与反向映射

## 概述

Linux 的设计原则是"空闲内存就是浪费的内存"：文件读写会尽量把数据留在 page cache 中，匿名内存只要进程不释放就一直驻留。于是系统运行一段时间后，空闲内存总会降到水位线附近，此时必须**回收（Reclaim）** 一部分已用内存。

可回收的内存主要有三类：**干净的文件页**（直接丢弃，需要时重新从磁盘读入）、**脏文件页**（先写回再丢弃）、**匿名页**（写入交换空间后丢弃，4.11 节）。此外，内核对象缓存（dentry、inode 等）通过 **shrinker** 机制回收。回收的核心问题是"回收哪些页"：理想的选择是未来最久不会被访问的页，实际中以"最近最少使用（Least Recently Used，LRU）"作为近似。

回收有两个执行者：每个节点一个的后台线程 **kswapd**，在空闲页跌破 low 水位时被唤醒，回收到 high 水位后休眠；以及分配者自身的**直接回收**（4.4 节）。两者最终都调用 `shrink_node()`。

v6.1 引入了**多代 LRU（Multi-Gen LRU，MGLRU）**，本书配置启用了 `CONFIG_LRU_GEN=y` 与 `CONFIG_LRU_GEN_ENABLED=y`，即默认使用 MGLRU。本节先简述传统的双链表 LRU，再详细分析 MGLRU 和共用的回收执行路径。

## LRU 的组织

### lruvec

LRU 链表组织在 `struct lruvec` 中。未启用 memcg 时每个节点一个（`pgdat->__lruvec`）；启用 memcg 时，每个 memcg 在每个节点上各有一个（4.14 节）：

```c
// include/linux/mmzone.h:669-698（v6.18.60）
struct lruvec {
	struct list_head		lists[NR_LRU_LISTS];
	/* per lruvec lru_lock for memcg */
	spinlock_t			lru_lock;
	/*
	 * These track the cost of reclaiming one LRU - file or anon -
	 * over the other. As the observed cost of reclaiming one LRU
	 * increases, the reclaim scan balance tips toward the other.
	 */
	unsigned long			anon_cost;
	unsigned long			file_cost;
	/* Non-resident age, driven by LRU movement */
	atomic_long_t			nonresident_age;
	/* Refaults at the time of last reclaim cycle */
	unsigned long			refaults[ANON_AND_FILE];
	/* Various lruvec state flags (enum lruvec_flags) */
	unsigned long			flags;
#ifdef CONFIG_LRU_GEN
	/* evictable pages divided into generations */
	struct lru_gen_folio		lrugen;
#ifdef CONFIG_LRU_GEN_WALKS_MMU
	/* to concurrently iterate lru_gen_mm_list */
	struct lru_gen_mm_state		mm_state;
#endif
#endif /* CONFIG_LRU_GEN */
#ifdef CONFIG_MEMCG
	struct pglist_data *pgdat;
#endif
	struct zswap_lruvec_state zswap_lruvec_state;
};
```

`lists[]` 是传统 LRU 的链表，`lrugen` 是 MGLRU 的数据结构，两者互斥使用——同一时刻 folio 只会在其中一处。`lru_lock` 保护这些链表。

### 传统 LRU：活跃与非活跃

```c
// include/linux/mmzone.h:316-323（v6.18.60）
enum lru_list {
	LRU_INACTIVE_ANON = LRU_BASE,
	LRU_ACTIVE_ANON = LRU_BASE + LRU_ACTIVE,
	LRU_INACTIVE_FILE = LRU_BASE + LRU_FILE,
	LRU_ACTIVE_FILE = LRU_BASE + LRU_FILE + LRU_ACTIVE,
	LRU_UNEVICTABLE,
	NR_LRU_LISTS
};
```

传统 LRU 把可回收页按"匿名 / 文件"和"活跃 / 非活跃"分为四条链表，另有一条不可回收链表（mlock 的页、ramfs 页等）。新页通常加入非活跃链表头；回收从非活跃链表尾扫描，被访问过两次的页提升到活跃链表；非活跃链表过短时，从活跃链表尾部把页降级到非活跃链表。这一"二次机会"算法的关键判断在 `folio_check_references()`：

```c
// mm/vmscan.c:937-962（v6.18.60，节选）
	referenced_folio = folio_test_clear_referenced(folio);

	if (referenced_ptes) {
		/*
		 * All mapped folios start out with page table
		 * references from the instantiating fault, so we need
		 * to look twice if a mapped file/anon folio is used more
		 * than once.
		 *
		 * Mark it and spare it for another trip around the
		 * inactive list.  Another page table reference will
		 * lead to its activation.
		 ...
		 */
		folio_set_referenced(folio);

		if (referenced_folio || referenced_ptes > 1)
			return FOLIOREF_ACTIVATE;

		/*
		 * Activate file-backed executable folios after first usage.
		 */
		if ((vm_flags & VM_EXEC) && folio_is_file_lru(folio))
			return FOLIOREF_ACTIVATE;

		return FOLIOREF_KEEP;
	}
```

`folio_referenced()` 通过反向映射（4.9 节）检查并清除所有映射该页的 PTE 的 Accessed 位。`PG_referenced` 标志则记录通过 `read()`/`write()` 等文件接口的访问（`folio_mark_accessed()`）。

传统 LRU 的主要问题：

- **粒度太粗**：只有活跃/非活跃两级，无法区分"一分钟前访问"和"一小时前访问"；
- **扫描代价高**：判断冷热需要对每个候选页做反向映射遍历，在大内存、多映射的场景下 CPU 开销很大；
- **匿名与文件的平衡依赖启发式**：`get_scan_count()`（`mm/vmscan.c:2562`）根据 `swappiness`、回收代价和 refault 统计决定两类页的扫描比例，调优困难。

## 多代 LRU（MGLRU）

### 设计思想

MGLRU 由 Yu Zhao 开发，v6.1 合入主线。它的核心改进有三点：

1. **多代**：把页按"年龄"分成多个**代（Generation）**，代号越大越年轻。回收从最老的代开始。
2. **老化与驱逐分离**：**老化（Aging）** 负责创建新的一代，并把近期被访问的页提升到最年轻的代；**驱逐（Eviction）** 只负责从最老的代中回收页。
3. **以页表扫描代替反向映射**：老化时直接遍历进程页表收集 Accessed 位（空间局部性好，批量处理），而不是对每个页做反向映射。

### 数据结构

```c
// include/linux/mmzone.h:490-518（v6.18.60）
struct lru_gen_folio {
	/* the aging increments the youngest generation number */
	unsigned long max_seq;
	/* the eviction increments the oldest generation numbers */
	unsigned long min_seq[ANON_AND_FILE];
	/* the birth time of each generation in jiffies */
	unsigned long timestamps[MAX_NR_GENS];
	/* the multi-gen LRU lists, lazily sorted on eviction */
	struct list_head folios[MAX_NR_GENS][ANON_AND_FILE][MAX_NR_ZONES];
	/* the multi-gen LRU sizes, eventually consistent */
	long nr_pages[MAX_NR_GENS][ANON_AND_FILE][MAX_NR_ZONES];
	/* the exponential moving average of refaulted */
	unsigned long avg_refaulted[ANON_AND_FILE][MAX_NR_TIERS];
	/* the exponential moving average of evicted+protected */
	unsigned long avg_total[ANON_AND_FILE][MAX_NR_TIERS];
	/* can only be modified under the LRU lock */
	unsigned long protected[NR_HIST_GENS][ANON_AND_FILE][MAX_NR_TIERS];
	/* can be modified without holding the LRU lock */
	atomic_long_t evicted[NR_HIST_GENS][ANON_AND_FILE][MAX_NR_TIERS];
	atomic_long_t refaulted[NR_HIST_GENS][ANON_AND_FILE][MAX_NR_TIERS];
	/* whether the multi-gen LRU is enabled */
	bool enabled;
	/* the memcg generation this lru_gen_folio belongs to */
	u8 gen;
	/* the list segment this lru_gen_folio belongs to */
	u8 seg;
	/* per-node lru_gen_folio list for global reclaim */
	struct hlist_nulls_node list;
};
```

```c
// include/linux/mmzone.h:398-421（v6.18.60，节选）
#define MIN_NR_GENS		2U
#define MAX_NR_GENS		4U
...
#define MAX_NR_TIERS		4U
```

代用单调递增的**序号（seq）** 标识，`max_seq` 是最年轻的代，`min_seq[type]` 是匿名页（type 0）和文件页（type 1）各自最老的代。同时存在的代数在 `MIN_NR_GENS`（2）与 `MAX_NR_GENS`（4）之间。链表下标为 `seq % MAX_NR_GENS`，所以 `folios[]` 是一个环形数组。每一代又按 type 和 zone 细分，便于只回收特定 zone 的页。folio 所在的代号记录在 `folio->flags` 的 `LRU_GEN` 位段中。

```
 seq:   min_seq ─────────────────────────────────────────▶ max_seq
        ┌──────────────┬──────────────┬──────────────┬──────────────┐
 anon   │ 最老一代      │              │              │ 最年轻一代    │
 file   │ （被驱逐）    │              │              │ （老化时新建）│
        └──────────────┴──────────────┴──────────────┴──────────────┘
           ▲ evict_folios() 从这里取页        try_to_inc_max_seq() 新建 ▲
                                     老化时被访问的页提升到 max_seq
```

*图 4-16：MGLRU 的代（从左到右越来越年轻）*

**层（Tier）** 是代内的进一步细分，用于处理通过文件描述符的访问（`read()` 等不经过页表的访问）。每个 folio 用 `LRU_REFS` 位记录被这类接口访问的次数，访问 N 次的页属于第 log2(N+1) 层。MGLRU 统计每一层页被驱逐后又很快被重新读入（refault）的比例，若某层的 refault 率明显高于第 0 层，就在驱逐时保护该层的页（`sort_folio()` 中的 "protected" 分支）。这就是 `avg_refaulted`、`avg_total`、`protected`、`evicted`、`refaulted` 等数组的用途，其背后是一个 PID 控制器思想的反馈回路。

### 新页放在哪一代

```c
// include/linux/mm_inline.h:220-252（v6.18.60，节选）
static inline unsigned long lru_gen_folio_seq(const struct lruvec *lruvec,
					      const struct folio *folio,
					      bool reclaiming)
{
	int gen;
	int type = folio_is_file_lru(folio);
	const struct lru_gen_folio *lrugen = &lruvec->lrugen;
	...
	if (folio_test_active(folio))
		gen = MIN_NR_GENS - folio_test_workingset(folio);
	else if (reclaiming)
		gen = MAX_NR_GENS;
	else if ((!folio_is_file_lru(folio) && !folio_test_swapcache(folio)) ||
		 (folio_test_reclaim(folio) &&
		  (folio_test_dirty(folio) || folio_test_writeback(folio))))
		gen = MIN_NR_GENS;
	else
		gen = MAX_NR_GENS - folio_test_workingset(folio);

	return max(READ_ONCE(lrugen->max_seq) - gen + 1, READ_ONCE(lrugen->min_seq[type]));
}
```

返回的序号为 `max_seq - gen + 1`：

| folio 类型 | `gen` | 放入的代 |
|---|---|---|
| 刚通过缺页映射的匿名页 | `MIN_NR_GENS` = 2 | `max_seq - 1`（次年轻代） |
| 新读入的文件页 | `MAX_NR_GENS` = 4 | `max_seq - 3`（最老代，受 `min_seq` 下限约束） |
| 新读入但属于工作集的文件页（refault） | 3 | `max_seq - 2` |
| 带 `PG_active` 的页（被提升） | 1 或 2 | `max_seq` 或 `max_seq - 1` |

文件页默认进入最老代，因为大多数文件页只被读一次（如流式读取），没有理由让它们挤占其他内存；只有被证明经常访问的页才会通过老化或分层被提升。

### 回收入口

```c
// mm/vmscan.c:6051-6061（v6.18.60）
static void shrink_node(pg_data_t *pgdat, struct scan_control *sc)
{
	unsigned long nr_reclaimed, nr_scanned, nr_node_reclaimed;
	struct lruvec *target_lruvec;
	bool reclaimable = false;

	if (lru_gen_enabled() && root_reclaim(sc)) {
		memset(&sc->nr, 0, sizeof(sc->nr));
		lru_gen_shrink_node(pgdat, sc);
		return;
	}
```

全局回收（非 memcg 限额触发）在启用 MGLRU 时进入 `lru_gen_shrink_node()`：

```c
// mm/vmscan.c:5043-5083（v6.18.60，节选）
static void lru_gen_shrink_node(struct pglist_data *pgdat, struct scan_control *sc)
{
	...
	lru_add_drain();

	blk_start_plug(&plug);

	set_mm_walk(pgdat, sc->proactive);

	set_initial_priority(pgdat, sc);
	...
	if (mem_cgroup_disabled())
		shrink_one(&pgdat->__lruvec, sc);
	else
		shrink_many(pgdat, sc);
	...
	clear_mm_walk();

	blk_finish_plug(&plug);
done:
	if (sc->nr_reclaimed > reclaimed)
		atomic_set(&pgdat->kswapd_failures, 0);
}
```

`shrink_many()`（`mm/vmscan.c:4952`）在启用 memcg 时按 memcg 的"代"轮流选择 memcg 进行回收（memcg 级别的 LRU，使长期不活跃的 memcg 优先被回收）。每个 memcg 的 lruvec 由 `shrink_one()` 处理：

```c
// mm/vmscan.c:4911-4934（v6.18.60，节选）
static int shrink_one(struct lruvec *lruvec, struct scan_control *sc)
{
	...
	/* lru_gen_age_node() called mem_cgroup_calculate_protection() */
	if (mem_cgroup_below_min(NULL, memcg))
		return MEMCG_LRU_YOUNG;

	if (mem_cgroup_below_low(NULL, memcg)) {
		/* see the comment on MEMCG_NR_GENS */
		if (READ_ONCE(lruvec->lrugen.seg) != MEMCG_LRU_TAIL)
			return MEMCG_LRU_TAIL;

		memcg_memory_event(memcg, MEMCG_LOW);
	}

	success = try_to_shrink_lruvec(lruvec, sc);

	shrink_slab(sc->gfp_mask, pgdat->node_id, memcg, sc->priority);
	...
```

低于 `memory.min` 的 memcg 完全不回收，低于 `memory.low` 的尽量不回收（4.14 节）。回收完 LRU 页后，调用 `shrink_slab()` 回收该 memcg 的 slab 对象。

### 驱逐与老化的协调

```c
// mm/vmscan.c:4873-4909（v6.18.60，节选）
static bool try_to_shrink_lruvec(struct lruvec *lruvec, struct scan_control *sc)
{
	long nr_to_scan;
	unsigned long scanned = 0;
	int swappiness = get_swappiness(lruvec, sc);

	while (true) {
		int delta;

		nr_to_scan = get_nr_to_scan(lruvec, sc, swappiness);
		if (nr_to_scan <= 0)
			break;

		delta = evict_folios(nr_to_scan, lruvec, sc, swappiness);
		if (!delta)
			break;

		scanned += delta;
		if (scanned >= nr_to_scan)
			break;

		if (should_abort_scan(lruvec, sc))
			break;

		cond_resched();
	}
	...
```

`get_nr_to_scan()` 决定本轮扫描多少页，以及是否需要先老化：

```c
// mm/vmscan.c:4782-4841（v6.18.60，节选）
static bool should_run_aging(struct lruvec *lruvec, unsigned long max_seq,
			     int swappiness, unsigned long *nr_to_scan)
{
	...
	*nr_to_scan = 0;
	/* have to run aging, since eviction is not possible anymore */
	if (evictable_min_seq(min_seq, swappiness) + MIN_NR_GENS > max_seq)
		return true;
	...
	*nr_to_scan = size;
	/* better to run aging even though eviction is still possible */
	return evictable_min_seq(min_seq, swappiness) + MIN_NR_GENS == max_seq;
}
...
static long get_nr_to_scan(struct lruvec *lruvec, struct scan_control *sc, int swappiness)
{
	...
	success = should_run_aging(lruvec, max_seq, swappiness, &nr_to_scan);
	...
	/* try to get away with not aging at the default priority */
	if (!success || sc->priority == DEF_PRIORITY)
		return nr_to_scan >> sc->priority;

	/* stop scanning this lruvec as it's low on cold folios */
	return try_to_inc_max_seq(lruvec, max_seq, swappiness, false) ? -1 : 0;
}
```

规则是：必须始终保持至少 `MIN_NR_GENS`（2）代存在，最老的代可被驱逐，最年轻的代用于接收被提升的页。当只剩 2 代时，驱逐无法继续，必须先老化。扫描量为总页数右移 `priority` 位，与传统 LRU 一致：`priority` 从 `DEF_PRIORITY`（12）开始，即先扫描 1/4096，每轮没回收够就降低 priority、加大扫描量。

### 老化：`try_to_inc_max_seq()`

```c
// mm/vmscan.c:4055-4107（v6.18.60，节选）
static bool try_to_inc_max_seq(struct lruvec *lruvec, unsigned long seq,
			       int swappiness, bool force_scan)
{
	...
	/*
	 * If the hardware doesn't automatically set the accessed bit, fallback
	 * to lru_gen_look_around(), which only clears the accessed bit in a
	 * handful of PTEs. Spreading the work out over a period of time usually
	 * is less efficient, but it avoids bursty page faults.
	 */
	if (!should_walk_mmu()) {
		success = iterate_mm_list_nowalk(lruvec, seq);
		goto done;
	}

	walk = set_mm_walk(NULL, true);
	...
	do {
		success = iterate_mm_list(walk, &mm);
		if (mm)
			walk_mm(mm, walk);
	} while (mm);
done:
	if (success) {
		success = inc_max_seq(lruvec, seq, swappiness);
		WARN_ON_ONCE(!success);
	}

	return success;
}
```

老化遍历该 lruvec 所属 memcg 下所有进程的 mm（`lru_gen_mm_list`），对每个 mm 调用 `walk_mm()`（`mm/vmscan.c:3810`）扫描其页表。遇到 Accessed 位为 1 的 PTE，就清除该位，并把对应 folio 的代号更新为 `max_seq`（即将成为新的次年轻代）。为降低开销：

- **布隆过滤器（Bloom Filter）**：记录上一轮中"包含足够多年轻 PTE 的 PMD"，本轮只扫描这些 PMD 下的 PTE 页表，跳过大片冷区域；
- **PMD 层 Accessed 位**：x86 在 PMD 项中也会设置 Accessed 位（非叶 PMD 项，`CONFIG_ARCH_HAS_NONLEAF_PMD_YOUNG`），可以先检查 PMD 层再决定是否进入；
- **批量更新**：代号变化先记录在 walk 结构中，再批量更新 `nr_pages` 统计。

`inc_max_seq()` 最终把 `max_seq` 加 1，记录时间戳，使所有代"变老"一级。

**look-around**：驱逐过程中，`folio_referenced()` 经反向映射找到一个年轻 PTE 时，`lru_gen_look_around()`（`mm/vmscan.c:4235`）会顺便检查同一 PTE 页表中相邻的若干 PTE，把其中年轻的也一并提升。这利用了空间局部性，用极小的代价获得额外的访问信息。

### 驱逐：`evict_folios()`

```c
// mm/vmscan.c:4693-4778（v6.18.60，节选）
static int evict_folios(unsigned long nr_to_scan, struct lruvec *lruvec,
			struct scan_control *sc, int swappiness)
{
	...
	spin_lock_irq(&lruvec->lru_lock);

	scanned = isolate_folios(nr_to_scan, lruvec, sc, swappiness, &type, &list);

	scanned += try_to_inc_min_seq(lruvec, swappiness);

	if (evictable_min_seq(lrugen->min_seq, swappiness) + MIN_NR_GENS > lrugen->max_seq)
		scanned = 0;

	spin_unlock_irq(&lruvec->lru_lock);

	if (list_empty(&list))
		return scanned;
retry:
	reclaimed = shrink_folio_list(&list, pgdat, sc, &stat, false, memcg);
	sc->nr.unqueued_dirty += stat.nr_unqueued_dirty;
	sc->nr_reclaimed += reclaimed;
	...
	spin_lock_irq(&lruvec->lru_lock);

	move_folios_to_lru(lruvec, &list);
	...
	spin_unlock_irq(&lruvec->lru_lock);
	...
	return scanned;
}
```

`isolate_folios()`（`mm/vmscan.c:4670`）根据 `swappiness` 与两类页各层的 refault 统计选择从匿名还是文件的最老代取页，再由 `scan_folios()` 遍历最老代链表，对每个 folio 调用 `sort_folio()` 做"延迟排序"：

```c
// mm/vmscan.c:4469-4516（v6.18.60，节选）
static bool sort_folio(struct lruvec *lruvec, struct folio *folio, struct scan_control *sc,
		       int tier_idx)
{
	int gen = folio_lru_gen(folio);
	int type = folio_is_file_lru(folio);
	int zone = folio_zonenum(folio);
	int delta = folio_nr_pages(folio);
	int refs = folio_lru_refs(folio);
	bool workingset = folio_test_workingset(folio);
	int tier = lru_tier_from_refs(refs, workingset);
	struct lru_gen_folio *lrugen = &lruvec->lrugen;
	...
	/* promoted */
	if (gen != lru_gen_from_seq(lrugen->min_seq[type])) {
		list_move(&folio->lru, &lrugen->folios[gen][type][zone]);
		return true;
	}

	/* protected */
	if (tier > tier_idx || refs + workingset == BIT(LRU_REFS_WIDTH) + 1) {
		gen = folio_inc_gen(lruvec, folio, false);
		list_move(&folio->lru, &lrugen->folios[gen][type][zone]);
		...
		return true;
	}

	/* ineligible */
	if (zone > sc->reclaim_idx) {
		gen = folio_inc_gen(lruvec, folio, false);
		list_move_tail(&folio->lru, &lrugen->folios[gen][type][zone]);
		return true;
	}

	return false;
}
```

老化时只修改了 folio 的代号字段，并没有在链表间移动 folio（那需要获取 `lru_lock`）。驱逐扫描到这类"代号已不是最老"的 folio 时，才把它移到正确的链表上——这就是"lazily sorted on eviction"的含义。受分层保护的、或属于不符合本次回收条件 zone 的 folio 被移入下一代。其余 folio 经 `isolate_folio()` 从 LRU 摘下，交给 `shrink_folio_list()`。

当最老代的某个 type 被扫空时，`try_to_inc_min_seq()` 把 `min_seq[type]` 加 1。

## 共用的回收执行：`shrink_folio_list()`

无论传统 LRU 还是 MGLRU，被选中的 folio 最终都交给 `shrink_folio_list()`（`mm/vmscan.c:1104`）逐个处理。这是一个长达数百行的函数，主要步骤如下：

```
shrink_folio_list() 对每个 folio:
├── folio_trylock()                     失败 → 保留
├── 正在写回？                           → 根据情况等待、标记 PG_reclaim 或保留
├── folio_check_references()            近期被访问 → 激活/保留
├── 匿名页且不在交换缓存？
│   └── folio_alloc_swap()              分配交换槽位，加入交换缓存（4.11）
├── folio_mapped()？
│   └── try_to_unmap()                  经 rmap 解除所有 PTE 映射
├── 脏页？
│   ├── 文件页且非 kswapd 或未满足条件   → 标记 PG_reclaim，交给 flusher 写回
│   └── pageout()                       调用 ->writepage / swap_writeout 发起写回
├── filemap_release_folio()             释放 buffer_head 等私有数据
├── __remove_mapping()                  从 page cache / 交换缓存中删除，留下 shadow 项
└── free_unref_folios()                 批量释放回伙伴系统
```

解除映射与写回的核心代码：

```c
// mm/vmscan.c:1396-1403（v6.18.60）
			try_to_unmap(folio, flags);
			if (folio_mapped(folio)) {
				stat->nr_unmap_fail += nr_pages;
				if (!was_swapbacked &&
				    folio_test_swapbacked(folio))
					stat->nr_lazyfree_fail += nr_pages;
				goto activate_locked;
			}
```

```c
// mm/vmscan.c:1417-1442（v6.18.60，节选）
		if (folio_test_dirty(folio)) {
			/*
			 * Only kswapd can writeback filesystem folios
			 * to avoid risk of stack overflow. But avoid
			 * injecting inefficient single-folio I/O into
			 * flusher writeback as much as possible: only
			 * write folios when we've encountered many
			 * dirty folios, and when we've already scanned
			 * the rest of the LRU for clean folios and see
			 * the same dirty folios again (with the reclaim
			 * flag set).
			 */
			if (folio_is_file_lru(folio) &&
			    (!current_is_kswapd() ||
			     !folio_test_reclaim(folio) ||
			     !test_bit(PGDAT_DIRTY, &pgdat->flags))) {
				...
				node_stat_mod_folio(folio, NR_VMSCAN_IMMEDIATE,
						nr_pages);
				folio_set_reclaim(folio);

				goto activate_locked;
			}
```

**回收路径尽量不直接写回文件页**：直接回收可能发生在任意深的调用栈中，再进入文件系统写回有栈溢出风险；而且逐页的随机写 I/O 效率远低于 flusher 线程按文件顺序批量写回。因此只有 kswapd、且同一脏页第二次被扫描到、且节点被标记为"脏页过多"（`PGDAT_DIRTY`）时才会就地写回。其他情况只设置 `PG_reclaim`，让 flusher 写回完成时（`folio_end_writeback()` → `folio_rotate_reclaimable()`）把它移到 LRU 尾部，下次扫描即可直接回收。匿名页（写入交换区）不受此限制。

`__remove_mapping()` 从 page cache（或交换缓存）中删除 folio 时，会调用 `workingset_eviction()`（`mm/workingset.c:381`）在原位置留下一个 **shadow 项**，记录驱逐时刻的"LRU 时钟"。以后若该页被重新读入（refault），`workingset_refault()`（`mm/workingset.c:534`）根据 shadow 计算"重新访问距离"：如果距离小于当前内存可容纳的工作集大小，说明该页属于工作集、不该被驱逐，于是新读入的 folio 被标记 `PG_workingset`，并直接放入较年轻的代。MGLRU 使用自己的 `lru_gen_eviction()` / `lru_gen_refault()`（`mm/workingset.c:232`、`:283`），把 refault 计入对应层的统计。

## kswapd

### 主循环

```c
// mm/vmscan.c:7304-7374（v6.18.60，节选）
static int kswapd(void *p)
{
	unsigned int alloc_order, reclaim_order;
	unsigned int highest_zoneidx = MAX_NR_ZONES - 1;
	pg_data_t *pgdat = (pg_data_t *)p;
	struct task_struct *tsk = current;
	...
	tsk->flags |= PF_MEMALLOC | PF_KSWAPD;
	set_freezable();

	WRITE_ONCE(pgdat->kswapd_order, 0);
	WRITE_ONCE(pgdat->kswapd_highest_zoneidx, MAX_NR_ZONES);
	atomic_set(&pgdat->nr_writeback_throttled, 0);
	for ( ; ; ) {
		bool was_frozen;

		alloc_order = reclaim_order = READ_ONCE(pgdat->kswapd_order);
		highest_zoneidx = kswapd_highest_zoneidx(pgdat,
							highest_zoneidx);

kswapd_try_sleep:
		kswapd_try_to_sleep(pgdat, alloc_order, reclaim_order,
					highest_zoneidx);

		/* Read the new order and highest_zoneidx */
		alloc_order = READ_ONCE(pgdat->kswapd_order);
		highest_zoneidx = kswapd_highest_zoneidx(pgdat,
							highest_zoneidx);
		WRITE_ONCE(pgdat->kswapd_order, 0);
		WRITE_ONCE(pgdat->kswapd_highest_zoneidx, MAX_NR_ZONES);
		...
		reclaim_order = balance_pgdat(pgdat, alloc_order,
						highest_zoneidx);
		if (reclaim_order < alloc_order)
			goto kswapd_try_sleep;
	}
	...
```

kswapd 设置 `PF_MEMALLOC`，可以无视水位分配少量内存（4.4 节）。它在 `kswapd_try_to_sleep()`（`mm/vmscan.c:7207`）中睡眠，被 `wakeup_kswapd()` 唤醒后读取请求的阶与 zone 上限，调用 `balance_pgdat()`。睡眠前它会先"小睡" `HZ/10`（约 100 ms，`mm/vmscan.c:7240`），若期间没有再被唤醒且节点平衡，才进入长睡眠并唤醒 kcompactd 做后台规整。

### `balance_pgdat()`

```c
// mm/vmscan.c:6975-7151（v6.18.60，节选）
static int balance_pgdat(pg_data_t *pgdat, int order, int highest_zoneidx)
{
	...
	struct scan_control sc = {
		.gfp_mask = GFP_KERNEL,
		.order = order,
		.may_unmap = 1,
	};
	...
	nr_boost_reclaim = 0;
	for_each_managed_zone_pgdat(zone, pgdat, i, highest_zoneidx) {
		nr_boost_reclaim += zone->watermark_boost;
		zone_boosts[i] = zone->watermark_boost;
	}
	boosted = nr_boost_reclaim;

restart:
	set_reclaim_active(pgdat, highest_zoneidx);
	sc.priority = DEF_PRIORITY;
	do {
		...
		balanced = pgdat_balanced(pgdat, sc.order, highest_zoneidx);
		if (!balanced && nr_boost_reclaim) {
			nr_boost_reclaim = 0;
			goto restart;
		}
		...
		if (!nr_boost_reclaim && balanced)
			goto out;
		...
		sc.may_writepage = !laptop_mode && !nr_boost_reclaim;
		sc.may_swap = !nr_boost_reclaim;

		/*
		 * Do some background aging, to give pages a chance to be
		 * referenced before reclaiming. All pages are rotated
		 * regardless of classzone as this is about consistent aging.
		 */
		kswapd_age_node(pgdat, &sc);
		...
		if (kswapd_shrink_node(pgdat, &sc))
			raise_priority = false;
		...
		if (raise_priority || !nr_reclaimed)
			sc.priority--;
	} while (sc.priority >= 1);
	...
	if (!sc.nr_reclaimed)
		atomic_inc(&pgdat->kswapd_failures);
```

`balance_pgdat()` 以 priority 从 12 递减到 1 的循环逐步加大回收力度，每轮调用 `kswapd_shrink_node()`（`mm/vmscan.c:6902`），后者计算回收目标（每个 zone 达到 high 水位所需的页数）并调用 `shrink_node()`。循环在节点"平衡"时结束：

```c
// mm/vmscan.c:6780-6798（v6.18.60，节选）
static bool pgdat_balanced(pg_data_t *pgdat, int order, int highest_zoneidx)
{
	...
	for_each_managed_zone_pgdat(zone, pgdat, i, highest_zoneidx) {
		...
		if (sysctl_numa_balancing_mode & NUMA_BALANCING_MEMORY_TIERING)
			mark = promo_wmark_pages(zone);
		else
			mark = high_wmark_pages(zone);
```

即 `highest_zoneidx` 及以下**任意一个** zone 的空闲页达到 high 水位（且满足阶要求）。

水位提升（4.4 节）引起的回收是"轻量模式"：`may_writepage` 和 `may_swap` 均为 0，只回收干净的文件页，不发起任何 I/O；回收量达到 boost 总量即停止，然后清零 boost 并唤醒 kcompactd 整理碎片。

连续 `MAX_RECLAIM_RETRIES`（16）次完全没有回收到页时，`kswapd_failures` 达到上限，`wakeup_kswapd()` 不再唤醒该节点的 kswapd（"无望节点"），直到有页被释放时 `free_frozen_page_commit()` 等路径重置它（4.3 节）。

## shrinker：回收内核对象缓存

dentry、inode、文件系统元数据、GPU 驱动缓存等不在 LRU 上，它们通过注册 **shrinker** 参与回收：

```c
// include/linux/shrinker.h:82-91（v6.18.60）
struct shrinker {
	unsigned long (*count_objects)(struct shrinker *,
				       struct shrink_control *sc);
	unsigned long (*scan_objects)(struct shrinker *,
				      struct shrink_control *sc);

	long batch;	/* reclaim batch size, 0 = default */
	int seeks;	/* seeks to recreate an obj */
	unsigned flags;
```

`shrink_slab()`（`mm/shrinker.c:623`）在每次 LRU 回收后被调用，按照"本轮 LRU 扫描的比例"（与 priority 相关）乘以 `count_objects()` 返回的对象数，计算该 shrinker 应扫描的对象数，再分批调用 `scan_objects()`（`do_shrink_slab()`，`mm/shrinker.c:380`）。`seeks` 表示重建一个对象的代价（默认 `DEFAULT_SEEKS` = 2），代价越高，扫描比例越小。支持 memcg 的 shrinker（`SHRINKER_MEMCG_AWARE`）只回收目标 memcg 的对象，例如 dentry/inode 的 `list_lru`。

## 关键参数

| 参数 | 默认值 | 作用 |
|---|---|---|
| `vm.swappiness` | 60（`mm/vmscan.c:202`） | 匿名页相对文件页的回收倾向，范围 0～200；0 表示尽量不换出匿名页，200 表示两者同等对待 |
| `vm.vfs_cache_pressure` | 100 | dentry/inode 缓存相对 page cache 的回收倾向 |
| `vm.watermark_scale_factor` | 10 | 影响 low/high 水位间距，即 kswapd 的启动时机和每次回收量（4.4 节） |
| `/sys/kernel/mm/lru_gen/enabled` | 0x0007（`CONFIG_LRU_GEN_ENABLED=y` 时） | MGLRU 开关及特性位（基础、页表遍历、非叶 PMD Accessed 位） |
| `/sys/kernel/mm/lru_gen/min_ttl_ms` | 0 | 工作集保护：最年轻代存活不足该时间时不回收，转而触发 OOM，防止抖动（thrashing） |

## 动手实验

查看 LRU 统计与回收计数：

```bash
grep -E "^(Active|Inactive)\((anon|file)\)|^Unevictable|^Mlocked" /proc/meminfo
grep -E "^(pgscan|pgsteal|pgrefill|workingset_)" /proc/vmstat
```

查看 MGLRU 状态（每个 memcg、每个节点的代及各代页数）：

```bash
cat /sys/kernel/mm/lru_gen/enabled
sudo cat /sys/kernel/debug/lru_gen | head -30
#  memcg     1 /
#   node     0
#           12      48631      12041    129402     # 代序号 年龄(ms) 匿名页数 文件页数
#           13      21088       1532      3011
```

触发一次主动老化和驱逐（root，命令格式见 `Documentation/admin-guide/mm/multigen_lru.rst`）：

```bash
# + memcg_id node_id max_gen_nr [can_swap [force_scan]]  —— 老化
# - memcg_id node_id min_gen_nr [swappiness [nr_to_reclaim]] —— 驱逐
echo "+ 1 0 13 1 1" | sudo tee /sys/kernel/debug/lru_gen
```

观察 kswapd 的唤醒与回收（root）：

```bash
sudo bpftrace -e '
tracepoint:vmscan:mm_vmscan_kswapd_wake { printf("kswapd%d wake order=%d\n", args->nid, args->order); }
tracepoint:vmscan:mm_vmscan_lru_shrink_inactive { @reclaimed = sum(args->nr_reclaimed); @scanned = sum(args->nr_scanned); }'
# 另一终端制造内存压力：stress-ng --vm 2 --vm-bytes 90% -t 30s
```

## 小结

- 回收对象包括干净/脏文件页、匿名页（经交换）以及 shrinker 管理的内核缓存；执行者是 kswapd（low 唤醒、high 休眠）和直接回收。
- 传统 LRU 使用活跃/非活跃双链表与"二次机会"判断，依赖逐页反向映射检查 Accessed 位。
- v6.1 引入、本书配置默认启用的 MGLRU 把页分为 2～4 代：老化遍历页表批量提升年轻页并新建一代，驱逐从最老代取页并延迟排序；分层与 refault 反馈保护经文件描述符频繁访问的页。
- `shrink_folio_list()` 是共用的执行路径：检查引用 → 分配交换槽位 → 解除映射 → 写回 → 从映射中删除并留下 shadow 项 → 释放；文件脏页尽量交给 flusher 写回。
- shadow 项让 refault 能够被识别为工作集，从而调整后续的回收决策。
- kswapd 的 `balance_pgdat()` 以递减的 priority 逐步加大扫描量，直到任一合格 zone 达到 high 水位；水位提升触发的回收只回收干净页。

## 参考

- `linux/Documentation/mm/multigen_lru.rst`（MGLRU 设计）
- `linux/Documentation/admin-guide/mm/multigen_lru.rst`（MGLRU 用户接口）
- `linux/Documentation/mm/page_reclaim.rst`
- `linux/Documentation/admin-guide/sysctl/vm.rst`（`swappiness`、`vfs_cache_pressure`）
- `linux/mm/workingset.c` 文件开头关于 refault distance 的长注释
- LWN, "The multi-generational LRU": https://lwn.net/Articles/851184/
