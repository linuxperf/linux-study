# 3.8 PELT 负载跟踪

> **本节源码**：[`kernel/sched/pelt.c`](../../linux/kernel/sched/pelt.c)、[`kernel/sched/pelt.h`](../../linux/kernel/sched/pelt.h)、[`kernel/sched/sched-pelt.h`](../../linux/kernel/sched/sched-pelt.h)、[`kernel/sched/fair.c`](../../linux/kernel/sched/fair.c)、[`include/linux/sched.h`](../../linux/include/linux/sched.h)
> **关键函数**：`decay_load()`、`accumulate_sum()`、`___update_load_sum()`、`___update_load_avg()`、`__update_load_avg_se()`、`update_load_avg()`、`update_rq_clock_pelt()`、`util_est_update()`、`cpu_util()`
> **前置知识**：3.1 调度器框架（`struct sched_avg`）、3.3 EEVDF、3.5 调度时钟（`clock_pelt`）

## 概述

EEVDF 解决的是"一个 CPU 上谁先运行"，但调度器还要回答另外几个问题：哪个 CPU 更忙、该不该把任务迁走（负载均衡，3.9 节），CPU 该运行在什么频率（schedutil 调频），大小核系统上任务该放到哪种核（能效感知调度，Energy Aware Scheduling，EAS）。这些决策都需要一个反映"近期有多忙"的数值。

**逐实体负载跟踪**（Per-Entity Load Tracking，PELT，v3.8 引入）为每个调度实体、每个 `cfs_rq` 以及 RT / DL / 中断各维护一组几何衰减平均值：最近 1 ms 的活动权重最高，32 ms 前的活动权重减半，更早的贡献逐渐消失。本节分析 PELT 的数学模型、增量更新算法、频率与算力不变性，以及基于 PELT 的 `util_est` 与 CPU 利用率计算。

## 数学模型

把时间切成长度为 1024 μs（约 1 ms）的**周期**（period），令 `uᵢ` 为 i 个周期前实体处于某种状态（可运行 / 正在运行）的时间占比。PELT 维护的累加和是：

```
  sum = u₀ + u₁·y + u₂·y² + u₃·y³ + ...       其中 y³² = 0.5
```

源码注释（[`kernel/sched/pelt.c:152-179`](../../linux/kernel/sched/pelt.c#L152-L179)）解释了这一选择：32 ms 前的贡献权重恰好是当前周期的一半。每跨过一个周期边界，只需把旧和乘以 `y` 再加上新周期的贡献，就完成了"重新编号"。

当实体一直处于该状态时，`sum` 收敛到最大值：

```
  LOAD_AVG_MAX = 1024 × (1 + y + y² + ...) ≈ 1024 / (1 − y) ≈ 47742
```

[`kernel/sched/sched-pelt.h:14-15`](../../linux/kernel/sched/sched-pelt.h#L14-L15)（v6.18.60）

```c
#define LOAD_AVG_PERIOD 32
#define LOAD_AVG_MAX 47742
```

平均值 `avg = sum / LOAD_AVG_MAX` 就落在 `[0, 1]` 之间，再乘以一个刻度得到最终信号。`struct sched_avg` 中的三组信号（3.1 节，[`include/linux/sched.h:460-515`](../../linux/include/linux/sched.h#L460-L515)）分别为：

| 信号 | 定义（源码注释） | 含义 | 范围 |
|---|---|---|---|
| `load_avg` | `runnable% × scale_load_down(load)` | 按权重加权的可运行比例 | 0 ～ 权重 |
| `runnable_avg` | `runnable% × SCHED_CAPACITY_SCALE` | 可运行（含排队等待）比例 | 0 ～ 1024 |
| `util_avg` | `running% × SCHED_CAPACITY_SCALE` | 实际运行比例 | 0 ～ 1024 |

`runnable_avg` 与 `util_avg` 的差值反映了**争用**程度：任务在排队等 CPU 时只增加前者。

## 增量更新

### 衰减：decay_load()

[`kernel/sched/pelt.c:32-56`](../../linux/kernel/sched/pelt.c#L32-L56)（v6.18.60）

```c
static u64 decay_load(u64 val, u64 n)
{
	unsigned int local_n;

	if (unlikely(n > LOAD_AVG_PERIOD * 63))
		return 0;

	/* after bounds checking we can collapse to 32-bit */
	local_n = n;

	/*
	 * As y^PERIOD = 1/2, we can combine
	 *    y^n = 1/2^(n/PERIOD) * y^(n%PERIOD)
	 * With a look-up table which covers y^n (n<PERIOD)
	 *
	 * To achieve constant time decay_load.
	 */
	if (unlikely(local_n >= LOAD_AVG_PERIOD)) {
		val >>= local_n / LOAD_AVG_PERIOD;
		local_n %= LOAD_AVG_PERIOD;
	}

	val = mul_u64_u32_shr(val, runnable_avg_yN_inv[local_n], 32);
	return val;
}
```

计算 `val × yⁿ`：利用 `y³² = 1/2`，先右移 `n / 32` 位，剩余的 `y^(n % 32)` 查表（`runnable_avg_yN_inv[]` 存放 `yⁱ × 2³²`，由 `Documentation/scheduler/sched-pelt.c` 生成）。超过 `32 × 63` 个周期（约 2 秒）的值直接归零。整个计算是常数时间。

### 累加：accumulate_sum()

两次更新之间的时间 `delta` 可能跨越多个周期边界，被分成三段：

```
           d1          d2           d3
           ^           ^            ^
           |           |            |
         |<->|<----------------->|<--->|
 ... |---x---|------| ... |------|-----x (now)
```

*图 3-13：一次 PELT 更新覆盖的三段时间（摘自 [`kernel/sched/pelt.c:86-90`](../../linux/kernel/sched/pelt.c#L86-L90) 注释）*

- `d1`：上次更新所在周期的剩余部分；
- `d2`：中间完整的 `p − 1` 个周期；
- `d3`：当前周期已经过去的部分。

[`kernel/sched/pelt.c:102-150`](../../linux/kernel/sched/pelt.c#L102-L150)（v6.18.60，节选）

```c
static __always_inline u32
accumulate_sum(u64 delta, struct sched_avg *sa,
	       unsigned long load, unsigned long runnable, int running)
{
	u32 contrib = (u32)delta; /* p == 0 -> delta < 1024 */
	u64 periods;

	delta += sa->period_contrib;
	periods = delta / 1024; /* A period is 1024us (~1ms) */

	/*
	 * Step 1: decay old *_sum if we crossed period boundaries.
	 */
	if (periods) {
		sa->load_sum = decay_load(sa->load_sum, periods);
		sa->runnable_sum =
			decay_load(sa->runnable_sum, periods);
		sa->util_sum = decay_load((u64)(sa->util_sum), periods);

		/*
		 * Step 2
		 */
		delta %= 1024;
		if (load) {
			...
			contrib = __accumulate_pelt_segments(periods,
					1024 - sa->period_contrib, delta);
		}
	}
	sa->period_contrib = delta;

	if (load)
		sa->load_sum += load * contrib;
	if (runnable)
		sa->runnable_sum += runnable * contrib << SCHED_CAPACITY_SHIFT;
	if (running)
		sa->util_sum += contrib << SCHED_CAPACITY_SHIFT;

	return periods;
}
```

1. 若跨过了周期边界，先把三个旧和各衰减 `periods` 次；
2. `__accumulate_pelt_segments()`（[`kernel/sched/pelt.c:58-79`](../../linux/kernel/sched/pelt.c#L58-L79)）算出三段的总贡献：`d1·yᵖ + 1024·Σ(n=1..p-1) yⁿ + d3`，其中中间的等比数列和用 `LOAD_AVG_MAX − decay_load(LOAD_AVG_MAX, p) − 1024` 直接求得；
3. `period_contrib` 记住当前周期已累积的长度，供下次使用；
4. 根据实体在这段时间的状态（`load` / `runnable` / `running` 三个参数）把贡献加到对应的和上。注意这里隐含了一个假设：两次更新之间实体的状态没有变化——这正是为什么每次状态变化（入队、出队、切换）前都要先更新 PELT。

### 求平均：___update_load_avg()

[`kernel/sched/pelt.c:257-268`](../../linux/kernel/sched/pelt.c#L257-L268)（v6.18.60）

```c
static __always_inline void
___update_load_avg(struct sched_avg *sa, unsigned long load)
{
	u32 divider = get_pelt_divider(sa);

	/*
	 * Step 2: update *_avg.
	 */
	sa->load_avg = div_u64(load * sa->load_sum, divider);
	sa->runnable_avg = div_u64(sa->runnable_sum, divider);
	WRITE_ONCE(sa->util_avg, sa->util_sum / divider);
}
```

除数不是固定的 `LOAD_AVG_MAX`，而是 `LOAD_AVG_MAX − 1024 + period_contrib`（[`kernel/sched/pelt.h:45-50`](../../linux/kernel/sched/pelt.h#L45-L50)）。注释（[`kernel/sched/pelt.c:233-256`](../../linux/kernel/sched/pelt.c#L233-L256)）解释：当前周期尚未走完的部分不应被当作空闲，否则一个始终运行的任务的 `util_avg` 会在 1002～1024 之间振荡。

`___update_load_sum()`（[`kernel/sched/pelt.c:180-231`](../../linux/kernel/sched/pelt.c#L180-L231)）是对外入口：以 1024 ns 为单位（近似 1 μs，可用移位代替除法）计算 `delta`，只有跨过周期边界时才返回 1，提示调用者需要重新计算平均值。

### 各类实体的更新

`pelt.c` 为不同对象提供了薄包装，区别只在于传入的 `load` / `runnable` / `running`：

[`kernel/sched/pelt.c:307-334`](../../linux/kernel/sched/pelt.c#L307-L334)（v6.18.60）

```c
int __update_load_avg_se(u64 now, struct cfs_rq *cfs_rq, struct sched_entity *se)
{
	if (___update_load_sum(now, &se->avg, !!se->on_rq, se_runnable(se),
				cfs_rq->curr == se)) {

		___update_load_avg(&se->avg, se_weight(se));
		cfs_se_util_change(&se->avg);
		trace_pelt_se_tp(se);
		return 1;
	}

	return 0;
}

int __update_load_avg_cfs_rq(u64 now, struct cfs_rq *cfs_rq)
{
	if (___update_load_sum(now, &cfs_rq->avg,
				scale_load_down(cfs_rq->load.weight),
				cfs_rq->h_nr_runnable,
				cfs_rq->curr != NULL)) {

		___update_load_avg(&cfs_rq->avg, 1);
		trace_pelt_cfs_tp(cfs_rq);
		return 1;
	}

	return 0;
}
```

| 对象 | `load` | `runnable` | `running` | 字段 |
|---|---|---|---|---|
| 公平调度实体 | `on_rq` | 任务：`on_rq`；组：`h_nr_runnable` | 是否为 `cfs_rq->curr` | `se->avg` |
| `cfs_rq` | 队列总权重 | `h_nr_runnable` | 是否有 `curr` | `cfs_rq->avg` |
| RT | 是否在运行 RT | 同左 | 同左 | `rq->avg_rt` |
| Deadline | 是否在运行 DL | 同左 | 同左 | `rq->avg_dl` |
| 中断（`CONFIG_HAVE_SCHED_AVG_IRQ`） | 中断时间 | 同左 | 同左 | `rq->avg_irq` |
| 硬件压力（`CONFIG_SCHED_HW_PRESSURE`） | 被限频的算力 | 同左 | 同左 | `rq->avg_hw` |

RT 与 DL 不跟踪单个任务，只在 rq 级别统计"本 CPU 有多少时间在跑 RT / DL"（`update_rt_rq_load_avg()`、`update_dl_rq_load_avg()`，[`kernel/sched/pelt.c:347-386`](../../linux/kernel/sched/pelt.c#L347-L386)），在 3.6、3.7 节的 `put_prev_task` / `task_tick` 中调用。

注意 `cfs_rq` 的 `load_sum` 已经乘了权重，所以 `___update_load_avg(&cfs_rq->avg, 1)` 传入 1；而 `__update_load_avg_blocked_se()`（[`kernel/sched/pelt.c:296-305`](../../linux/kernel/sched/pelt.c#L296-L305)）以全 0 参数更新睡眠实体，只做衰减。

## 在公平类中的维护

### update_load_avg()

[`kernel/sched/fair.c:4904-4944`](../../linux/kernel/sched/fair.c#L4904-L4944)（v6.18.60）

```c
static inline void update_load_avg(struct cfs_rq *cfs_rq, struct sched_entity *se, int flags)
{
	u64 now = cfs_rq_clock_pelt(cfs_rq);
	int decayed;

	/*
	 * Track task load average for carrying it to new CPU after migrated, and
	 * track group sched_entity load average for task_h_load calculation in migration
	 */
	if (se->avg.last_update_time && !(flags & SKIP_AGE_LOAD))
		__update_load_avg_se(now, cfs_rq, se);

	decayed  = update_cfs_rq_load_avg(now, cfs_rq);
	decayed |= propagate_entity_load_avg(se);

	if (!se->avg.last_update_time && (flags & DO_ATTACH)) {

		/*
		 * DO_ATTACH means we're here from enqueue_entity().
		 * !last_update_time means we've passed through
		 * migrate_task_rq_fair() indicating we migrated.
		 *
		 * IOW we're enqueueing a task on a new CPU.
		 */
		attach_entity_load_avg(cfs_rq, se);
		update_tg_load_avg(cfs_rq);

	} else if (flags & DO_DETACH) {
		/*
		 * DO_DETACH means we're here from dequeue_entity()
		 * and we are migrating task out of the CPU.
		 */
		detach_entity_load_avg(cfs_rq, se);
		update_tg_load_avg(cfs_rq);
	} else if (decayed) {
		cfs_rq_util_change(cfs_rq, 0);

		if (flags & UPDATE_TG)
			update_tg_load_avg(cfs_rq);
	}
}
```

3.3 节中 `enqueue_entity()`、`dequeue_entity()`、`set_next_entity()`、`put_prev_entity()`、`entity_tick()` 开头的 `update_load_avg()` 调用都是为了在状态变化前"结算"PELT：

1. 更新实体自己的信号；
2. 更新所在 `cfs_rq` 的信号（`update_cfs_rq_load_avg()`，[`kernel/sched/fair.c:4746`](../../linux/kernel/sched/fair.c#L4746) 起），并处理 `removed` 中异步移除的负载（迁移走的任务的贡献）；
3. 若为组实体，把子 `cfs_rq` 的变化沿层级向上传播（`propagate_entity_load_avg()`）；
4. 任务迁入新 CPU 时 **attach**（把任务的贡献加到新 `cfs_rq`），迁出时 **detach**；
5. `cfs_rq_util_change()` 通知 cpufreq：利用率变了，可能需要调频；
6. `update_tg_load_avg()` 更新任务组的全局负载（组调度用，3.10 节）。

### 睡眠任务的贡献

`cfs_rq` 的信号是其上**所有实体**的贡献之和，包括已经睡眠但曾在此运行的任务（"blocked load"）。任务睡眠时不会立即从 `cfs_rq->avg` 中移除，而是随时间一起衰减。这让负载均衡看到的是"最近一段时间"的负载，而不是瞬时的可运行任务数——一个频繁短暂睡眠的任务不会让 CPU 看起来忽忙忽闲。

空闲 CPU 不会自己更新 blocked load，所以 nohz idle 负载均衡会顺便替它们衰减（`sched_balance_update_blocked_averages()`，3.9 节），`rq->has_blocked_load` 记录是否还有待衰减的负载（[`kernel/sched/sched.h:1131`](../../linux/kernel/sched/sched.h#L1131)，依赖 `CONFIG_NO_HZ_COMMON`）。

### 新任务的初值

[`kernel/sched/fair.c:1159-1175`](../../linux/kernel/sched/fair.c#L1159-L1175)（v6.18.60）

```c
void init_entity_runnable_average(struct sched_entity *se)
{
	struct sched_avg *sa = &se->avg;

	memset(sa, 0, sizeof(*sa));

	/*
	 * Tasks are initialized with full load to be seen as heavy tasks until
	 * they get a chance to stabilize to their real load level.
	 * Group entities are initialized with zero load to reflect the fact that
	 * nothing has been attached to the task group yet.
	 */
	if (entity_is_task(se))
		sa->load_avg = scale_load_down(se->load.weight);

	/* when this task is enqueued, it will contribute to its cfs_rq's load_avg */
}
```

新任务的 `load_avg` 初始为满值，使 fork 均衡把它当作重任务对待；`util_avg` 则在 `wake_up_new_task()` 中由 `post_init_entity_util_avg()`（[`kernel/sched/fair.c:1204-1240`](../../linux/kernel/sched/fair.c#L1204-L1240)）根据目标 CPU 现有利用率外推，并限制为剩余算力的一半（注释给出了 512、256、128… 的递减序列），防止一次 fork 大量任务时把利用率估爆。

## 频率与算力不变性

如果 CPU 以一半频率运行，完成同样的工作需要两倍时间，未经修正的 `util_avg` 会翻倍，导致 schedutil 误以为需要更高频率——形成正反馈。PELT 通过专用时钟 `clock_pelt` 解决这个问题：

[`kernel/sched/pelt.h:100-127`](../../linux/kernel/sched/pelt.h#L100-L127)（v6.18.60，节选）

```c
static inline void update_rq_clock_pelt(struct rq *rq, s64 delta)
{
	if (unlikely(is_idle_task(rq->curr))) {
		_update_idle_rq_clock_pelt(rq);
		return;
	}
	...
	delta = cap_scale(delta, arch_scale_cpu_capacity(cpu_of(rq)));
	delta = cap_scale(delta, arch_scale_freq_capacity(cpu_of(rq)));

	rq->clock_pelt += delta;
}
```

- 运行时，`clock_pelt` 按"CPU 算力 / 最大算力 × 当前频率 / 最大频率"缩放后前进，因此测得的是"折算到最大算力最高频率下完成的工作量"。x86 上频率比例来自 3.5 节 `sched_tick()` 中 `arch_scale_freq_tick()` 读取的 APERF/MPERF。
- 空闲时，`clock_pelt` 直接同步到 `clock_task`（`_update_idle_rq_clock_pelt()`，[`kernel/sched/pelt.h:78-86`](../../linux/kernel/sched/pelt.h#L78-L86)）。注释中的示意图（[`kernel/sched/pelt.h:88-99`](../../linux/kernel/sched/pelt.h#L88-L99)）展示了半速运行时 `clock_pelt` 先变慢、空闲时再追上的过程。

于是 `util_avg` 的含义变成"需要多少最大算力"，可以直接与 `capacity_of(cpu)` 比较。`update_rq_clock_pelt()` 由 3.5 节 `update_rq_clock_task()` 末尾调用。

## util_est：利用率估计

PELT 的平滑带来一个问题：一个周期性运行 8 ms、睡眠 8 ms 的任务，长时间睡眠后 `util_avg` 会衰减得很低，醒来时调频和选核会低估它的需求，需要几十毫秒才能"爬"回来。

`util_est`（`UTIL_EST` 特性，默认开启，[`kernel/sched/features.h:129`](../../linux/kernel/sched/features.h#L129)）为每个任务记住它**上次睡眠时**的利用率，并用指数加权移动平均（EWMA）平滑：

[`kernel/sched/fair.c:5076-5101`](../../linux/kernel/sched/fair.c#L5076-L5101)（v6.18.60，节选）

```c
	/* Get utilization at dequeue */
	dequeued = task_util(p);

	/*
	 * Reset EWMA on utilization increases, the moving average is used only
	 * to smooth utilization decreases.
	 */
	if (ewma <= dequeued) {
		ewma = dequeued;
		goto done;
	}

	/*
	 * Skip update of task's estimated utilization when its members are
	 * already ~1% close to its last activation value.
	 */
	last_ewma_diff = ewma - dequeued;
	if (last_ewma_diff < UTIL_EST_MARGIN)
		goto done;

	/*
	 * To avoid underestimate of task utilization, skip updates of EWMA if
	 * we cannot grant that thread got all CPU time it wanted.
	 */
	if ((dequeued + UTIL_EST_MARGIN) < task_runnable(p))
		goto done;
```

- **上升立即跟随，下降缓慢平滑**：利用率变大时直接采用新值，变小时按权重 1/4（`UTIL_EST_WEIGHT_SHIFT = 2`，[`include/linux/sched.h:525`](../../linux/include/linux/sched.h#L525)）做 EWMA。
- 如果任务在这次激活中排队时间较多（`runnable` 明显大于 `util`），说明它没拿到想要的 CPU，测得的 `util` 偏低，这次不更新。
- `cfs_rq->avg.util_est` 是当前可运行任务 `util_est` 之和，在 `util_est_enqueue()` / `util_est_dequeue()`（[`kernel/sched/fair.c:5016-5046`](../../linux/kernel/sched/fair.c#L5016-L5046)）中增减。

任务对外的利用率是二者的较大值：

[`kernel/sched/fair.c:5011-5014`](../../linux/kernel/sched/fair.c#L5011-L5014)（v6.18.60）

```c
static inline unsigned long task_util_est(struct task_struct *p)
{
	return max(task_util(p), _task_util_est(p));
}
```

## CPU 利用率：cpu_util()

调度器各处使用的"CPU 有多忙"最终由 `cpu_util()` 给出：

[`kernel/sched/fair.c:8156-8186`](../../linux/kernel/sched/fair.c#L8156-L8186)（v6.18.60，节选）

```c
static unsigned long
cpu_util(int cpu, struct task_struct *p, int dst_cpu, int boost)
{
	bool add_task = p && task_cpu(p) != cpu && dst_cpu == cpu;
	bool sub_task = p && task_cpu(p) == cpu && dst_cpu != cpu;
	struct cfs_rq *cfs_rq = &cpu_rq(cpu)->cfs;
	unsigned long util = READ_ONCE(cfs_rq->avg.util_avg);
	unsigned long runnable;
	...
	if (add_task)
		util += task_util(p);
	else if (sub_task)
		lsub_positive(&util, task_util(p));

	if (boost) {
		runnable = READ_ONCE(cfs_rq->avg.runnable_avg);
		...
		util = max(util, runnable);
	}

	if (sched_feat(UTIL_EST)) {
```

它在 `cfs_rq->avg.util_avg` 的基础上：可选地模拟"任务 p 迁入 / 迁出后"的值（供 EAS 和选核评估）；`boost` 时取 `runnable_avg` 与 `util_avg` 的较大值以反映争用；再与 `util_est` 取较大值；最后不超过 CPU 算力（[`kernel/sched/fair.c:8225`](../../linux/kernel/sched/fair.c#L8225)）。

主要使用者：

| 使用者 | 用到的信号 |
|---|---|
| schedutil 调频（本书配置的默认调频器，`CONFIG_CPU_FREQ_DEFAULT_GOV_SCHEDUTIL=y`） | `cpu_util_cfs_boost()` + `avg_rt` + `avg_dl` + `avg_irq`，经 `effective_cpu_util()`（[`kernel/sched/fair.c:8280`](../../linux/kernel/sched/fair.c#L8280) 起）合成 |
| 负载均衡（3.9 节） | `cpu_load()` = `cfs_rq->avg.load_avg`（[`kernel/sched/fair.c:7374-7377`](../../linux/kernel/sched/fair.c#L7374-L7377)）；`cpu_util_cfs()`；`runnable_avg` |
| 唤醒选核 `wake_affine_weight()`（[`kernel/sched/fair.c:7528-7569`](../../linux/kernel/sched/fair.c#L7528-L7569)） | `cpu_load()`、`task_h_load()` |
| CPU 可用算力 `capacity_of()` | 返回 `rq->cpu_capacity`，由 `scale_rt_capacity()`（[`kernel/sched/fair.c:10261-10286`](../../linux/kernel/sched/fair.c#L10261-L10286)）从实际最大算力（已扣除硬件压力）中扣除 `avg_rt` / `avg_dl` / `avg_irq` |
| EAS、misfit 检测 | `task_util_est()`、`cpu_util()` |

## 关键细节 / 设计考量

- **为什么是 32 ms 半衰期**。半衰期太短，信号抖动大，负载均衡会频繁迁移任务；太长，信号对负载变化反应迟钝，调频跟不上。32 ms 约等于若干个调度周期，是长期实践中的折中。
- **为什么以 1024 为单位**。时间以 1024 ns、周期以 1024 μs 为单位，使大量除法变为移位；`SCHED_CAPACITY_SCALE` 同为 1024，使 `util_avg` 与 CPU 算力在同一刻度上。
- **load 与 util 的区别**。`load_avg` 乘以权重，衡量"该分到多少 CPU"，用于负载均衡中按权重均分；`util_avg` 不含权重，衡量"实际用了多少 CPU"，用于调频和算力匹配。一个 nice 19 的死循环 `util_avg` 接近 1024，`load_avg` 却只有 15。
- **迁移的异步移除**。任务被唤醒迁移时不持有旧 CPU 的 rq 锁（3.4 节），无法直接修改旧 `cfs_rq->avg`，于是 `migrate_task_rq_fair()` 把它的贡献记到旧 `cfs_rq->removed`（[`kernel/sched/sched.h:714`](../../linux/kernel/sched/sched.h#L714) 处的 `struct cfs_rq` 字段），由旧 CPU 下次更新时扣除。

## 动手实验

以下实验在 **dev 虚拟机** 中进行。

1. 观察一个任务 PELT 信号的上升与衰减：

   ```bash
   # 虚拟机，root（读取 debugfs）
   taskset -c 1 sh -c 'while :; do :; done' & pid=$!
   for i in $(seq 10); do grep -E 'se\.avg\.(util_avg|runnable_avg|load_avg)' /proc/$pid/sched; sleep 0.02; done
   kill -STOP $pid
   for i in $(seq 10); do
       grep -A30 '^cfs_rq\[1\]:/$' /sys/kernel/debug/sched/debug | grep -m1 '\.util_avg'
       sleep 0.032
   done
   kill -CONT $pid; kill $pid
   ```

   运行时任务的 `se.avg.util_avg` 应向 1024 逼近。任务被暂停后已出队，它自己的 `se->avg` 要等到下次入队（或迁移时的 `sync_entity_load_avg()`）才会更新，`/proc/<pid>/sched` 中的值因此保持不变；这里改为观察 CPU 1 根 `cfs_rq` 的 `util_avg`，其中的 blocked 负载会在周期性或 nohz 的 blocked load 更新中大致每 32 ms 减半。

2. 对比不同 nice 值下 `load_avg` 与 `util_avg`：

   ```bash
   # 虚拟机，普通用户
   nice -n 19 sh -c 'while :; do :; done' & pid=$!
   sleep 1; grep -E 'se\.avg\.(util_avg|load_avg)|se.load.weight' /proc/$pid/sched
   kill $pid
   ```

3. 用 bpftrace 观察 `cfs_rq` 级别 PELT 更新（裸跟踪点 `pelt_cfs_tp`，参数为 `struct cfs_rq *`，只能看到触发频率）：

   ```bash
   # 虚拟机，root
   bpftrace -e 'rawtracepoint:pelt_cfs_tp { @[cpu] = count(); } interval:s:1 { print(@); clear(@); }'
   ```

## 小结

- PELT 以约 1 ms 为周期、`y³² = 0.5` 为衰减系数，为每个实体维护几何衰减的 `load_avg`、`runnable_avg`、`util_avg`，最大和 `LOAD_AVG_MAX = 47742`。
- 更新分两步：`accumulate_sum()` 衰减旧和并累加跨越的三段时间，`___update_load_avg()` 以考虑了当前周期位置的除数求平均；`decay_load()` 用移位加查表实现常数时间衰减。
- 公平类在每次状态变化前调用 `update_load_avg()`，维护实体、`cfs_rq` 与任务组的信号，并在迁移时 attach / detach；睡眠任务的贡献保留在 `cfs_rq` 中随时间衰减。
- RT、DL、中断、硬件压力只在 rq 级别跟踪，用于从 CPU 算力中扣除非公平任务的占用。
- `clock_pelt` 按 CPU 算力与频率缩放，使 `util_avg` 具有频率与算力不变性，可直接与 CPU 算力比较。
- `util_est` 记住任务睡眠前的利用率（上升立即跟随、下降 EWMA 平滑），弥补 PELT 对周期性任务的低估；`cpu_util()` 综合这些信号，供调频、负载均衡、选核与 EAS 使用。

## 参考

- `linux/Documentation/scheduler/sched-pelt.c`：生成 `runnable_avg_yN_inv[]` 与 `LOAD_AVG_MAX` 的程序。
- `linux/Documentation/scheduler/sched-capacity.rst`：CPU 算力与频率不变性。
- `linux/Documentation/scheduler/schedutil.rst`：schedutil 如何使用 PELT 信号。
- `linux/Documentation/scheduler/sched-util-clamp.rst`：uclamp 对利用率的钳制。
