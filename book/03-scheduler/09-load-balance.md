# 3.9 调度域与负载均衡

> **本节源码**：`kernel/sched/fair.c`、`kernel/sched/topology.c`、`kernel/sched/sched.h`、`include/linux/sched/topology.h`、`arch/x86/kernel/smpboot.c`
> **关键函数**：`sd_init()`、`sched_balance_trigger()`、`sched_balance_softirq()`、`sched_balance_domains()`、`sched_balance_rq()`、`sched_balance_find_src_group()`、`update_sd_lb_stats()`、`group_classify()`、`calculate_imbalance()`、`detach_tasks()`、`can_migrate_task()`、`sched_balance_newidle()`、`nohz_balancer_kick()`
> **前置知识**：3.4 任务创建与唤醒（唤醒选核）、3.8 PELT 负载跟踪

## 概述

每个 CPU 有独立的运行队列，这让调度在多核上几乎无锁地扩展，但也带来一个问题：某些 CPU 可能排着一长串任务，另一些却空闲着。3.4 节的唤醒选核在任务进入运行队列时尽量把它放对位置；本节分析的**负载均衡**（Load Balancing）则负责事后纠偏——周期性地或在 CPU 变空闲时，把任务从忙的 CPU 迁到闲的 CPU。

迁移是有代价的：任务会丢失缓存热度，跨 NUMA 节点还会丢失内存局部性。因此 Linux 按硬件拓扑把 CPU 组织成层级化的**调度域**（Scheduling Domain）：越底层的域（如 SMT 兄弟线程）迁移越便宜、均衡越频繁、对不平衡越敏感；越高层的域（如跨 NUMA 节点）迁移越昂贵、均衡越稀疏、门槛越高。

本节介绍调度域的结构与构建，然后分析三种均衡路径：周期性均衡、newidle 均衡和 nohz idle 均衡，最后讨论主动均衡。实时类与 Deadline 类的多核策略（push / pull）已在 3.6、3.7 节介绍，本节只涉及公平类。

## 调度域与调度组

### 层级结构

以一台 2 插槽、每插槽 4 核、每核 2 线程（共 16 个逻辑 CPU）的机器为例：

```
  NUMA 域（跨节点）  ┌──────────────────────── span: CPU 0-15 ────────────────────────┐
                     │  group: node0 (0-7)                group: node1 (8-15)        │
                     └───────────────────────────────────────────────────────────────┘
  MC/PKG 域（插槽内）┌──────── span: 0-7 ──────────┐     ┌──────── span: 8-15 ─────────┐
                     │ group:{0,1} {2,3} {4,5} {6,7}│     │ group:{8,9} ... {14,15}     │
                     └──────────────────────────────┘     └─────────────────────────────┘
  SMT 域（核内）     ┌ span:0-1 ┐┌ span:2-3 ┐ ...
                     │ grp:{0}{1}││grp:{2}{3}│
                     └──────────┘└──────────┘
```

*图 3-14：调度域层级示例（自底向上，每层的 group 是其子域的 span）*

- 每个 CPU 有一条从底到顶的调度域链（`rq->sd` 指向最底层，`sd->parent` 向上），`for_each_domain(cpu, sd)` 沿这条链遍历。
- 每个调度域的 CPU 集合称为 **span**，它被划分成若干**调度组**（Scheduling Group）；通常每个组就是下一层子域的 span。
- 负载均衡在一个域内比较各**组**的负载，而不是逐个比较 CPU：先找出最忙的组，再在组内找最忙的 CPU。

```c
// include/linux/sched/topology.h:73-92（v6.18.60，节选）
struct sched_domain {
	/* These fields must be setup */
	struct sched_domain __rcu *parent;	/* top domain must be null terminated */
	struct sched_domain __rcu *child;	/* bottom domain must be null terminated */
	struct sched_group *groups;	/* the balancing groups of the domain */
	unsigned long min_interval;	/* Minimum balance interval ms */
	unsigned long max_interval;	/* Maximum balance interval ms */
	unsigned int busy_factor;	/* less balancing by factor if busy */
	unsigned int imbalance_pct;	/* No balance until over watermark */
	unsigned int cache_nice_tries;	/* Leave cache hot tasks for # tries */
	unsigned int imb_numa_nr;	/* Nr running tasks that allows a NUMA imbalance */

	int nohz_idle;			/* NOHZ IDLE status */
	int flags;			/* See SD_* */
	int level;

	/* Runtime fields. */
	unsigned long last_balance;	/* init to jiffies. units in jiffies */
	unsigned int balance_interval;	/* initialise to 1. units in ms. */
	unsigned int nr_balance_failed; /* initialise to 0 */
```

调度组（`kernel/sched/sched.h:2109-2127`）是一个循环链表，每个组带有 CPU 掩码和指向共享算力信息 `sched_group_capacity` 的指针。

### x86 的拓扑层次

```c
// arch/x86/kernel/smpboot.c:481-491（v6.18.60）
static struct sched_domain_topology_level x86_topology[] = {
	SDTL_INIT(tl_smt_mask, cpu_smt_flags, SMT),
#ifdef CONFIG_SCHED_CLUSTER
	SDTL_INIT(tl_cls_mask, x86_cluster_flags, CLS),
#endif
#ifdef CONFIG_SCHED_MC
	SDTL_INIT(tl_mc_mask, x86_core_flags, MC),
#endif
	SDTL_INIT(tl_pkg_mask, x86_sched_itmt_flags, PKG),
	{ NULL },
};
```

| 层 | 含义 | 典型标志 |
|---|---|---|
| SMT | 同一物理核上的超线程 | `SD_SHARE_CPUCAPACITY`、`SD_SHARE_LLC` |
| CLS | 共享 L2 的核簇（依赖 `CONFIG_SCHED_CLUSTER`） | `SD_SHARE_LLC` |
| MC | 共享 LLC 的多核（依赖 `CONFIG_SCHED_MC`） | `SD_SHARE_LLC` |
| PKG | 整个插槽 | — |
| NUMA | 由 SLIT 距离自动生成（依赖 `CONFIG_NUMA`） | `SD_NUMA`、`SD_SERIALIZE` |

`build_sched_topology()`（`smpboot.c:493-516`）会在插槽内含多个 NUMA 节点时去掉 PKG 层，在每核只有一个线程时去掉 SMT 层。此外，构建时会自动"退化"（degenerate）掉与父域 span 相同或只有一个组的冗余域，所以在 dev 虚拟机这类简单拓扑上通常只剩一两层。

### 域参数

`sd_init()` 为每层设置默认参数，再按拓扑特征调整：

```c
// kernel/sched/topology.c:1646-1668（v6.18.60，节选）
	*sd = (struct sched_domain){
		.min_interval		= sd_weight,
		.max_interval		= 2*sd_weight,
		.busy_factor		= 16,
		.imbalance_pct		= 117,

		.cache_nice_tries	= 0,

		.flags			= 1*SD_BALANCE_NEWIDLE
					| 1*SD_BALANCE_EXEC
					| 1*SD_BALANCE_FORK
					| 0*SD_BALANCE_WAKE
					| 1*SD_WAKE_AFFINE
					| 0*SD_SHARE_CPUCAPACITY
					| 0*SD_SHARE_LLC
					| 0*SD_SERIALIZE
					| 1*SD_PREFER_SIBLING
					| 0*SD_NUMA
					| sd_flags
					,

		.last_balance		= jiffies,
		.balance_interval	= sd_weight,
```

- **均衡间隔**：以毫秒计，初始等于域内 CPU 数（`sd_weight`），在 `[min_interval, max_interval]` 内自适应：均衡成功则重置为最小值，持续平衡则逐步加倍（`sched_balance_rq()` 末尾）。CPU 忙时还要乘以 `busy_factor`（16），所以忙碌 CPU 的高层均衡相当稀疏。
- **`imbalance_pct`**：只有最忙组比本地组负载高出该百分比才迁移。SMT 层为 110，其余默认 117（`topology.c:1692-1697`）。
- **`cache_nice_tries`**：均衡失败这么多次以后，才允许迁移缓存热的任务。共享 LLC 与普通层为 1，NUMA 层为 2。
- **`SD_BALANCE_WAKE` 默认关闭**：这正是 3.4 节"普通唤醒不走调度域慢速路径"的原因。
- **NUMA 层**：设置 `SD_SERIALIZE`（全系统同时只允许一个 CPU 在 NUMA 层均衡），距离过远时还会关闭 fork / exec 均衡与亲和唤醒（`topology.c:1700-1709`）。

### root domain

调度域之上还有 **root domain**（`struct root_domain`）：一组被 cpuset 划分为独立调度分区的 CPU 共享一个 root domain。它保存跨 CPU 的全局状态——3.6 节的 `cpupri` 与 `rto_mask`、3.7 节的 Deadline 带宽账本 `dl_bw`、以及 `overloaded` / `overutilized` 标志。不同 root domain 之间永远不会发生负载均衡。

## 均衡的触发

公平类的负载均衡有三条路径：

| 路径 | 触发时机 | 执行者 | `cpu_idle_type` |
|---|---|---|---|
| 周期性均衡 | tick 中 `sched_balance_trigger()` 发现到期 | 本 CPU 的 `SCHED_SOFTIRQ` | `CPU_IDLE` / `__CPU_NOT_IDLE` |
| newidle 均衡 | 本 CPU 即将进入 idle（`pick_next_task_fair()` 无任务可选） | 本 CPU，在 `__schedule()` 中 | `CPU_NEWLY_IDLE` |
| nohz idle 均衡 | 忙碌 CPU 发现有停了 tick 的空闲 CPU 需要均衡 | 被"踢醒"的一个空闲 CPU，代表所有 nohz 空闲 CPU | `CPU_IDLE` |

### 周期性均衡

```c
// kernel/sched/fair.c:13274-13287（v6.18.60）
void sched_balance_trigger(struct rq *rq)
{
	/*
	 * Don't need to rebalance while attached to NULL domain or
	 * runqueue CPU is not active
	 */
	if (unlikely(on_null_domain(rq) || !cpu_active(cpu_of(rq))))
		return;

	if (time_after_eq(jiffies, rq->next_balance))
		raise_softirq(SCHED_SOFTIRQ);

	nohz_balancer_kick(rq);
}
```

3.5 节的 `sched_tick()` 在末尾调用它。到期时触发 `SCHED_SOFTIRQ`，软中断处理函数为：

```c
// kernel/sched/fair.c:13251-13269（v6.18.60）
static __latent_entropy void sched_balance_softirq(void)
{
	struct rq *this_rq = this_rq();
	enum cpu_idle_type idle = this_rq->idle_balance;
	/*
	 * If this CPU has a pending NOHZ_BALANCE_KICK, then do the
	 * balancing on behalf of the other idle CPUs whose ticks are
	 * stopped. Do nohz_idle_balance *before* sched_balance_domains to
	 * give the idle CPUs a chance to load balance. Else we may
	 * load balance only within the local sched_domain hierarchy
	 * and abort nohz_idle_balance altogether if we pull some load.
	 */
	if (nohz_idle_balance(this_rq, idle))
		return;

	/* normal load balance */
	sched_balance_update_blocked_averages(this_rq->cpu);
	sched_balance_domains(this_rq, idle);
}
```

`sched_balance_update_blocked_averages()` 先衰减本 CPU 上睡眠任务留下的 PELT 负载（3.8 节），使后续比较基于最新数据。`sched_balance_domains()` 自底向上遍历本 CPU 的每一层调度域：

```c
// kernel/sched/fair.c:12529-12566（v6.18.60，节选）
	for_each_domain(cpu, sd) {
		...
		/*
		 * Stop the load balance at this level. There is another
		 * CPU in our sched group which is doing load balancing more
		 * actively.
		 */
		if (!continue_balancing) {
			if (need_decay)
				continue;
			break;
		}

		interval = get_sd_balance_interval(sd, busy);
		if (time_after_eq(jiffies, sd->last_balance + interval)) {
			if (sched_balance_rq(cpu, rq, sd, idle, &continue_balancing)) {
				...
				idle = idle_cpu(cpu);
				busy = !idle && !sched_idle_cpu(cpu);
			}
			sd->last_balance = jiffies;
			interval = get_sd_balance_interval(sd, busy);
		}
		if (time_after(next_balance, sd->last_balance + interval)) {
			next_balance = sd->last_balance + interval;
			update_next_balance = 1;
		}
	}
```

每层只在自己的间隔到期时才均衡，并据此算出 `rq->next_balance`。`get_sd_balance_interval()`（`fair.c:12336-12358`）在 CPU 忙时把间隔乘以 `busy_factor` 再减 1，注释说明减 1 是为了让不同层的均衡周期不成倍数关系，避免它们总在同一个 tick 里扎堆。

### 谁来均衡：should_we_balance()

如果一个调度组里的每个 CPU 都在同一层做均衡，就会重复劳动并互相争抢。`should_we_balance()`（`fair.c:11942` 起）规定：在一个组内，只有**第一个空闲 CPU**（优先选整个物理核都空闲的）——若都不空闲则是组的第一个 CPU——负责该层的均衡；其他 CPU 返回 0 并设置 `continue_balancing = 0`，停止向更高层均衡。newidle 均衡例外，每个变空闲的 CPU 都可以尝试，但若已有任务或待处理的唤醒就放弃。

## sched_balance_rq()：一次均衡的全过程

```
sched_balance_rq(this_cpu, this_rq, sd, idle)
├── should_we_balance()                      // 本组由我负责吗？
├── sched_balance_find_src_group()           // 找最忙的组，算出 imbalance
│   ├── update_sd_lb_stats()                 //   遍历各组，统计 sg_lb_stats
│   │   └── update_sg_lb_stats() + group_classify()
│   └── calculate_imbalance()                //   决定迁移类型与数量
├── sched_balance_find_src_rq()              // 组内找最忙的 rq
├── rq_lock(busiest)
│   └── detach_tasks()                       // 从 busiest 摘下任务
│       └── can_migrate_task()
├── attach_tasks()                           // 放到本 rq
└── [未迁移成功] need_active_balance() → stop_one_cpu_nowait(active_load_balance_cpu_stop)
```

### 组统计与分类

`update_sd_lb_stats()`（`fair.c:11321` 起）遍历域内每个组，用 `update_sg_lb_stats()` 汇总组内各 CPU 的 PELT 负载、利用率、可运行任务数、空闲 CPU 数等，存入：

```c
// kernel/sched/fair.c:10205-10215（v6.18.60，节选）
struct sg_lb_stats {
	unsigned long avg_load;			/* Avg load            over the CPUs of the group */
	unsigned long group_load;		/* Total load          over the CPUs of the group */
	unsigned long group_capacity;		/* Capacity            over the CPUs of the group */
	unsigned long group_util;		/* Total utilization   over the CPUs of the group */
	unsigned long group_runnable;		/* Total runnable time over the CPUs of the group */
	unsigned int sum_nr_running;		/* Nr of all tasks running in the group */
	unsigned int sum_h_nr_running;		/* Nr of CFS tasks running in the group */
	unsigned int idle_cpus;                 /* Nr of idle CPUs         in the group */
	unsigned int group_weight;
	enum group_type group_type;
```

然后把每个组分成七类，按"严重程度"递增排列：

```c
// kernel/sched/fair.c:10465-10489（v6.18.60）
static inline enum
group_type group_classify(unsigned int imbalance_pct,
			  struct sched_group *group,
			  struct sg_lb_stats *sgs)
{
	if (group_is_overloaded(imbalance_pct, sgs))
		return group_overloaded;

	if (sg_imbalanced(group))
		return group_imbalanced;

	if (sgs->group_asym_packing)
		return group_asym_packing;

	if (sgs->group_smt_balance)
		return group_smt_balance;

	if (sgs->group_misfit_task_load)
		return group_misfit_task;

	if (!group_has_capacity(imbalance_pct, sgs))
		return group_fully_busy;

	return group_has_spare;
}
```

| 类型 | 含义（`fair.c:9452-9486` 注释） |
|---|---|
| `group_has_spare` | 有空闲算力，还能容纳更多任务 |
| `group_fully_busy` | 算力已用满，但任务之间不争抢 |
| `group_misfit_task` | 有任务对所在 CPU 的算力来说太大（大小核） |
| `group_smt_balance` | SMT 组完全忙碌，可把任务迁到空闲核上受益 |
| `group_asym_packing` | 有更高优先级（如 ITMT 偏好）的本地 CPU 可用 |
| `group_imbalanced` | 之前因任务亲和性限制无法均衡 |
| `group_overloaded` | 任务多于算力，无法满足所有任务的 CPU 需求 |

"有空闲"与"过载"的判断（`group_has_capacity()` / `group_is_overloaded()`，`fair.c:10423-10463`）同时考虑任务数与 PELT：任务数少于 CPU 数一定有空闲；否则用 `imbalance_pct` 做迟滞，比较 `group_util`、`group_runnable` 与 `group_capacity`。注意两者并不互补（源码注释 `fair.c:10440-10446`）：任务数不超过 CPU 数时一定不过载；任务数恰好等于 CPU 数时，是否有空闲还要看 PELT 利用率——利用率已高则既无空闲也不过载，即 `fully_busy`。

### 决策矩阵

`sched_balance_find_src_group()`（`fair.c:11593-11726`）根据本地组与最忙组的类型决定是否均衡，源码给出了完整的决策矩阵：

```c
// kernel/sched/fair.c:11563-11581（v6.18.60）
/*
 * Decision matrix according to the local and busiest group type:
 *
 * busiest \ local has_spare fully_busy misfit asym imbalanced overloaded
 * has_spare        nr_idle   balanced   N/A    N/A  balanced   balanced
 * fully_busy       nr_idle   nr_idle    N/A    N/A  balanced   balanced
 * misfit_task      force     N/A        N/A    N/A  N/A        N/A
 * asym_packing     force     force      N/A    N/A  force      force
 * imbalanced       force     force      N/A    N/A  force      force
 * overloaded       force     force      N/A    N/A  force      avg_load
 *
 * N/A :      Not Applicable because already filtered while updating
 *            statistics.
 * balanced : The system is balanced for these 2 groups.
 * force :    Calculate the imbalance as load migration is probably needed.
 * avg_load : Only if imbalance is significant enough.
 * nr_idle :  dst_cpu is not busy and the number of idle CPUs is quite
 *            different in groups.
 */
```

几个要点：

- 本地组比最忙组"更糟"（类型值更大）时不拉任务；
- 两组都过载时才比较按算力归一化的 `avg_load`，并要求最忙组超出本地组 `imbalance_pct`；
- 最忙组未过载时，只有本 CPU 空闲才尝试均衡，且两组空闲 CPU 数相差超过 1 才认为不平衡——否则只是把不平衡从一个组搬到另一个组；
- 在开启 EAS 的平台上，若 root domain 未过度利用，直接跳过均衡（`fair.c:11616-11618`），把放置交给唤醒时的能效选核。

### 计算迁移量

`calculate_imbalance()`（`fair.c:11391-11559`）不仅算出"迁移多少"，还决定"按什么度量迁移"，存入 `env->migration_type`：

| 迁移类型 | 度量 | 典型场景 |
|---|---|---|
| `migrate_load` | PELT `load_avg`（`task_h_load()`） | 双方都过载，按负载均分 |
| `migrate_util` | `task_util_est()` | 本地组有空闲算力，最忙组利用率过高 |
| `migrate_task` | 任务个数 | 本地有空闲 CPU，按任务数或空闲 CPU 数拉平 |
| `migrate_misfit` | 一个 misfit 任务 | 大小核上把大任务挪到大核 |

过载情形下的最终公式取两者较小值，既不让本地超过平均，也不让最忙组低于平均：

```c
// kernel/sched/fair.c:11546-11559（v6.18.60）
	/*
	 * Both group are or will become overloaded and we're trying to get all
	 * the CPUs to the average_load, so we don't want to push ourselves
	 * above the average load, nor do we wish to reduce the max loaded CPU
	 * below the average load. At the same time, we also don't want to
	 * reduce the group load below the group capacity. Thus we look for
	 * the minimum possible imbalance.
	 */
	env->migration_type = migrate_load;
	env->imbalance = min(
		(busiest->avg_load - sds->avg_load) * busiest->group_capacity,
		(sds->avg_load - local->avg_load) * local->group_capacity
	) / SCHED_CAPACITY_SCALE;
}
```

### 摘取任务

找到最忙的 rq 后，`sched_balance_rq()` 锁住它，由 `detach_tasks()` 从 `rq->cfs_tasks` 链表**尾部**（最久未运行、缓存最凉的任务）开始逐个检查：

```c
// kernel/sched/fair.c:9866-9918（v6.18.60，节选）
		p = list_last_entry(tasks, struct task_struct, se.group_node);

		if (!can_migrate_task(p, env))
			goto next;

		switch (env->migration_type) {
		case migrate_load:
			...
			load = max_t(unsigned long, task_h_load(p), 1);

			if (sched_feat(LB_MIN) &&
			    load < 16 && !env->sd->nr_balance_failed)
				goto next;
			...
			if (shr_bound(load, env->sd->nr_balance_failed) > env->imbalance)
				goto next;

			env->imbalance -= load;
			break;

		case migrate_util:
			util = task_util_est(p);

			if (shr_bound(util, env->sd->nr_balance_failed) > env->imbalance)
				goto next;

			env->imbalance -= util;
			break;

		case migrate_task:
			env->imbalance--;
			break;

		case migrate_misfit:
			...
			if (task_fits_cpu(p, env->src_cpu))
				goto next;

			env->imbalance = 0;
			break;
		}
```

- 单个任务的负载超过剩余 `imbalance` 就跳过，避免迁移一个大任务造成反向不平衡；随着 `nr_balance_failed` 增加，比较时把任务负载右移，逐步放宽条件。
- 每次最多检查 `sysctl_sched_nr_migrate`（`loop_max`）个任务，每 `SCHED_NR_MIGRATE_BREAK` 个释放一次锁（`LBF_NEED_BREAK`），限制关中断持锁时间。
- newidle 均衡在可抢占内核中只拉一个任务就停（`fair.c:9925-9933`），尽快让本 CPU 开始工作。

被摘下的任务以 `TASK_ON_RQ_MIGRATING` 状态挂在 `env->tasks` 上，释放 busiest 的锁后再由 `attach_tasks()` 放入本 rq——这样任何时候都只持有一把 rq 锁（`fair.c:12131-12139` 注释）。

### 能否迁移：can_migrate_task()

```c
// kernel/sched/fair.c:9669-9677（v6.18.60）
	/*
	 * We do not migrate tasks that are:
	 * 1) delayed dequeued unless we migrate load, or
	 * 2) target cfs_rq is in throttled hierarchy, or
	 * 3) cannot be migrated to this CPU due to cpus_ptr, or
	 * 4) running (obviously), or
	 * 5) are cache-hot on their current CPU, or
	 * 6) are blocked on mutexes (if SCHED_PROXY_EXEC is enabled)
	 */
```

此外，per-CPU 内核线程不迁移；在均衡尚未失败过时，不迁移在目标 CPU 上会"不合格"（EEVDF 意义，3.3 节）的任务（`fair.c:9684-9692`）。

**缓存热度**由 `task_hot()`（`fair.c:9532-9571`）判断：SMT 兄弟之间共享缓存，永不算热；否则若任务上次开始运行距今不足 `sysctl_sched_migration_cost`（默认 0.5 ms，`fair.c:82`）就算热。热任务只有在 `nr_balance_failed > cache_nice_tries` 后才允许迁移。开启 NUMA 均衡时，还会用 `migrate_degrades_locality()` 判断迁移是否破坏内存局部性。

亲和性受限的任务会设置 `LBF_SOME_PINNED` / `LBF_DST_PINNED`：`sched_balance_rq()` 会尝试把它们迁到本组内另一个允许的 CPU（`fair.c:12172-12188`）；若仍然失败，就在父域的组上标记 `imbalance`，使上一层把该组分类为 `group_imbalanced` 并强制均衡（`fair.c:12193-12198`）。

## 主动均衡

如果最忙 CPU 上只有一个正在运行的任务，`detach_tasks()` 无能为力——它不能迁移正在运行的任务。`need_active_balance()`（`fair.c:11911-11938`）判断此时是否值得"强行"迁移：例如大小核的 misfit 任务、源 CPU 因 RT / 中断导致算力显著下降而本 CPU 空闲、或多次均衡失败。

满足条件时，`sched_balance_rq()` 在最忙 CPU 上唤起 stopper 线程执行 `active_load_balance_cpu_stop()`（触发代码见 `fair.c:12235-12272`，函数本身位于 `fair.c:12379` 起）。stopper 属于最高的 stop 调度类（3.1 节），它抢占正在运行的任务，使之变为"在队列上但不在运行"，再把它推到目标 CPU。

## newidle 均衡

```c
// kernel/sched/fair.c:13145-13166（v6.18.60，节选）
	if (!get_rd_overloaded(this_rq->rd) ||
	    this_rq->avg_idle < sd->max_newidle_lb_cost) {

		update_next_balance(sd, &next_balance);
		rcu_read_unlock();
		goto out;
	}
	rcu_read_unlock();

	raw_spin_rq_unlock(this_rq);

	t0 = sched_clock_cpu(this_cpu);
	sched_balance_update_blocked_averages(this_cpu);

	rcu_read_lock();
	for_each_domain(this_cpu, sd) {
		u64 domain_cost;

		update_next_balance(sd, &next_balance);

		if (this_rq->avg_idle < curr_cost + sd->max_newidle_lb_cost)
			break;
```

当公平类在本 CPU 上选不出任务时，`pick_next_task_fair()` 会调用 `sched_balance_newidle()`（3.3 节），在进入 idle 之前尝试拉一个任务。它的核心是**成本控制**：

- 只有 root domain 存在过载 CPU（`rd->overloaded`）时才尝试；
- 比较本 CPU 的平均空闲时长 `avg_idle`（3.4 节 `ttwu_do_activate()` 中维护）与各层的历史最大均衡耗时 `max_newidle_lb_cost`：如果预计空闲时间还不够付一次均衡的开销，就不做——否则会因均衡本身延迟下一个唤醒；
- `NI_RANDOM` 特性（默认开启，`features.h:136`）按各层的历史成功率随机决定是否在该层尝试，`sd_init()` 将初始成功率设为 50%（`topology.c:1670-1673`）；
- 拉到任务或遇到更高调度类的任务即停止。函数释放过 rq 锁，若期间有 RT / DL 任务到来，返回 -1，对应 3.2 节的 `RETRY_TASK`。

## nohz idle 均衡

开启 `CONFIG_NO_HZ_COMMON` 后，空闲 CPU 会停掉 tick（3.5 节），也就不再执行周期性均衡。如果所有空闲 CPU 都在沉睡，忙碌 CPU 上积压的任务将无人来拉。

内核的做法是让**忙碌 CPU 在 tick 中检查**：`nohz_balancer_kick()`（`fair.c:12668` 起）在满足条件时——例如本 CPU 有两个以上可运行任务、存在 misfit 任务、或 LLC 内忙碌 CPU 较多——选出一个空闲 CPU（`find_new_ilb()`），通过 IPI 把它"踢醒"（`kick_ilb()`）。被踢醒的 CPU 在 `SCHED_SOFTIRQ` 中执行 `nohz_idle_balance()`，**代表所有 nohz 空闲 CPU** 逐个运行 `sched_balance_domains()`，并顺带衰减它们的 blocked 负载（`NOHZ_STATS_KICK`）。这就是 `sched_balance_softirq()` 先调用 `nohz_idle_balance()` 的原因。

## 关键细节 / 设计考量

- **唤醒放置与周期均衡的分工**。大部分任务放置在唤醒时完成（3.4 节），代价最低；周期均衡以 ms 到数百 ms 的间隔纠正长期失衡；newidle 均衡在 CPU 即将空转时"捡漏"。三者共同在吞吐与延迟之间取得平衡。
- **为什么用组而不是 CPU 比较**。在高层域逐 CPU 比较代价过高，而且忽视了拓扑：把任务从一个忙核的 SMT 线程迁到同一核的另一个线程，并不能获得更多算力。按组比较让每层只关心"这一层的子单元之间"是否平衡。
- **迟滞与自适应**。`imbalance_pct` 防止负载在两个组之间来回振荡；均衡间隔在成功时回到最小值、平衡时逐步变长，使稳定系统的均衡开销自动下降。
- **SMT 与核空闲优先**。`should_we_balance()`、`select_idle_sibling()` 都优先选择"整个物理核空闲"的 CPU，因为 SMT 兄弟共享执行单元，两个任务挤在一个核上的总吞吐低于分别放在两个核上。

## 动手实验

以下实验在 **dev 虚拟机** 中进行，需要 **root 权限**。

1. 查看本机调度域层级与参数（依赖 `CONFIG_DEBUG_FS`；`verbose` 打开后才会创建 `domains` 目录）：

   ```bash
   # 虚拟机，root
   echo Y > /sys/kernel/debug/sched/verbose
   ls /sys/kernel/debug/sched/domains/cpu0/
   for d in /sys/kernel/debug/sched/domains/cpu0/domain*; do
       echo "$d: $(cat $d/name) flags=$(cat $d/flags)"
   done
   lscpu -e
   ```

2. 观察负载均衡迁移：先把多个死循环绑在 CPU 0，再解除绑定，统计 `sched_migrate_task` 事件：

   ```bash
   # 虚拟机，root
   for i in 1 2 3 4; do taskset -c 0 sh -c 'while :; do :; done' & done
   sleep 1
   bpftrace -e 'tracepoint:sched:sched_migrate_task /str(args.comm) == "sh"/ { @[args.orig_cpu, args.dest_cpu] = count(); }' &
   bt=$!
   for p in $(pgrep -f "while :"); do taskset -p -c 0-$(($(nproc)-1)) $p; done
   sleep 3; kill $bt; pkill -f "while :"
   ```

   注意：修改亲和性本身会通过 stopper 立即迁移正在运行的任务，此后的再分布才来自负载均衡。

3. 查看 schedstats 中的均衡统计（`/proc/schedstat` 的 domain 行，各字段含义见 `Documentation/scheduler/sched-stats.rst`）：

   ```bash
   # 虚拟机，root
   sysctl kernel.sched_schedstats=1
   sleep 5; grep -A1 '^cpu0' /proc/schedstat; grep '^domain' /proc/schedstat | head
   sysctl kernel.sched_schedstats=0
   ```

## 小结

- 调度域按硬件拓扑（SMT → CLS → MC → PKG → NUMA）层层组织 CPU，每层的 span 划分为调度组；越高的层均衡越稀疏、门槛越高。root domain 之间不做均衡。
- 公平类有三条均衡路径：tick 触发 `SCHED_SOFTIRQ` 的周期性均衡、CPU 即将空闲时的 newidle 均衡、由忙碌 CPU 踢醒一个空闲 CPU 代为执行的 nohz idle 均衡。
- `sched_balance_rq()` 先找最忙的组、再找最忙的 rq，按组类型的决策矩阵判断是否均衡，由 `calculate_imbalance()` 决定按负载、利用率、任务数或 misfit 迁移多少。
- `detach_tasks()` 从最忙 rq 的 `cfs_tasks` 尾部摘取任务，`can_migrate_task()` 排除正在运行、亲和性不允许、缓存热（未超过失败次数阈值）的任务；摘取与放入分两步完成，始终只持有一把 rq 锁。
- 无法迁移正在运行的单个任务时，通过 stopper 线程做主动均衡。
- newidle 均衡以 `avg_idle` 与各层历史耗时 `max_newidle_lb_cost` 控制成本，并按成功率随机化，避免均衡本身拖慢唤醒。

## 参考

- `linux/Documentation/scheduler/sched-domains.rst`：调度域的设计说明。
- `linux/Documentation/scheduler/sched-stats.rst`：`/proc/schedstat` 中负载均衡相关字段。
- `linux/Documentation/scheduler/sched-capacity.rst`：CPU 算力与 misfit 任务。
- `linux/kernel/sched/fair.c:11563-11581`：负载均衡决策矩阵注释。
