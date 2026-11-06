# 3.8 PELT 负载跟踪与利用率估计

> **本节源码**：[`kernel/sched/pelt.c`](../../linux/kernel/sched/pelt.c)、[`kernel/sched/pelt.h`](../../linux/kernel/sched/pelt.h)、[`kernel/sched/sched-pelt.h`](../../linux/kernel/sched/sched-pelt.h)、[`kernel/sched/fair.c`](../../linux/kernel/sched/fair.c)、[`include/linux/sched.h`](../../linux/include/linux/sched.h)
> **关键函数**：`decay_load()`、`accumulate_sum()`、`___update_load_sum()`、`___update_load_avg()`、`update_load_avg()`、`attach_entity_load_avg()`、`update_rq_clock_pelt()`、`util_est_update()`、`cpu_util()`
> **前置知识**：[3.1 调度器框架](01-overview.md)、[3.3 EEVDF](03-eevdf.md)、[3.5 调度时钟](05-tick-preempt.md)

## 概述

EEVDF 解决的是“一个 CPU 上谁先运行”，但调度器还需要判断哪个 CPU 更忙、是否迁移任务、应当请求什么频率。能效感知调度（Energy Aware Scheduling，EAS）也需要比较任务需求与 CPU 算力，不能只看瞬时任务数。

逐实体负载跟踪（Per-Entity Load Tracking，PELT）用几何衰减平均值描述近期的负载、可运行程度和实际运行程度。它既跟踪公平调度实体，也维护运行队列级别的统计；睡眠任务留下的历史贡献不会立刻消失。公平类 PELT 的时间来自经过算力与频率缩放的专用时钟，利用率估计（Utilization Estimation，`util_est`）则保存激活结束时的利用率样本，弥补睡眠期间衰减造成的低估。本节按“时钟 → 累加和 → 平均值 → 队列聚合 → 决策使用者”的顺序分析。

## 核心数据结构

### `struct sched_avg`：累加和与平均值分开保存

[`include/linux/sched.h:505-515`](../../linux/include/linux/sched.h#L505-L515)（v6.18.60，添加字段注释）

```c
struct sched_avg {
	u64				last_update_time; /* [注] 上次结算的 PELT 时间，单位为 ns */
	u64				load_sum;        /* [注] 负载累加和，实体与队列的加权方式不同 */
	u64				runnable_sum;    /* [注] 可运行贡献，含 1024 刻度 */
	u32				util_sum;        /* [注] 实际运行贡献，含 1024 刻度 */
	u32				period_contrib;  /* [注] 当前周期已过去的计量单位数，0～1023 */
	unsigned long			load_avg;        /* [注] 加权负载平均值 */
	unsigned long			runnable_avg;    /* [注] 可运行平均值 */
	unsigned long			util_avg;        /* [注] 实际运行平均值 */
	unsigned int			util_est;        /* [注] 任务估计值或根队列的估计值之和 */
} ____cacheline_aligned;
```

`last_update_time` 仍以 ns 保存；只有计算时间差时才右移 10 位。任务的 `util_est` 最高位是 `UTIL_AVG_UNCHANGED` 标志，不属于利用率数值；根 `cfs_rq` 的同名字段保存去除标志后的任务估计值之和，见 [`include/linux/sched.h:517-526`](../../linux/include/linux/sched.h#L517-L526) 与 [`kernel/sched/fair.c:5006-5046`](../../linux/kernel/sched/fair.c#L5006-L5046)。本版本没有单独的 `struct util_est`。

### 信号含义与聚合范围

三类平均值的定义见 [`include/linux/sched.h:460-488`](../../linux/include/linux/sched.h#L460-L488)。下表先考虑普通、非延迟出队的单个任务：

| 信号 | 定义 | 含义 | 单任务的名义范围 |
|---|---|---|---|
| `load_avg` | 可运行比例 × `se_weight(se)` | 带 nice 权重的负载 | 0 ～ `scale_load_down(se->load.weight)` |
| `runnable_avg` | 可运行比例 × `SCHED_CAPACITY_SCALE` | 含排队等待的 CPU 需求 | 0 ～ 1024 |
| `util_avg` | 实际运行比例 × `SCHED_CAPACITY_SCALE` | 已获得的 CPU 时间对应的利用率 | 0 ～ 1024 |

`runnable_avg` 明显高于 `util_avg` 通常表示争用：排队时前者仍累加，后者只衰减。这不是瞬时等待时长，两者都保留了历史。

上述范围不能直接套用到队列或任务组。`cfs_rq->avg.runnable_avg` 聚合其下任务的可运行贡献，两个持续可运行的任务可使它接近 2048；组实体也可承载多个任务的贡献。队列的 `util_avg` 聚合同一个 CPU 的实际运行贡献，名义满刻度仍是 1024；迁移、初值和舍入可能造成超调，使用者还要按 CPU 算力钳制。聚合关系与超调说明分别见 [`kernel/sched/pelt.c:270-294`](../../linux/kernel/sched/pelt.c#L270-L294)、[`kernel/sched/fair.c:8144-8152`](../../linux/kernel/sched/fair.c#L8144-L8152)。

## 主流程分析

### 调用链总览

下面以公平类 tick 路径为主线；入队、出队和切换路径也会调用 `update_load_avg()`：

```text
sched_tick()                               kernel/sched/core.c
├── update_rq_clock()                      kernel/sched/core.c
│   └── update_rq_clock_task()             kernel/sched/core.c
│       └── update_rq_clock_pelt()         kernel/sched/pelt.h
└── task_tick_fair()                       kernel/sched/fair.c
    └── entity_tick()                     kernel/sched/fair.c
        └── update_load_avg()             kernel/sched/fair.c
            ├── __update_load_avg_se()    kernel/sched/pelt.c
            │   ├── ___update_load_sum()  kernel/sched/pelt.c
            │   │   └── accumulate_sum()  kernel/sched/pelt.c
            │   │       ├── decay_load()  kernel/sched/pelt.c
            │   │       └── __accumulate_pelt_segments()  kernel/sched/pelt.c
            │   └── ___update_load_avg()  kernel/sched/pelt.c
            ├── update_cfs_rq_load_avg()   kernel/sched/fair.c
            │   └── __update_load_avg_cfs_rq()  kernel/sched/pelt.c
            └── propagate_entity_load_avg()   kernel/sched/fair.c
```

`task_tick_fair()` 沿实体层级逐层调用 `entity_tick()`；实体和队列更新分别维护自己的平均值，不是每次遍历所有任务求和。入口见 [`kernel/sched/core.c:5597-5636`](../../linux/kernel/sched/core.c#L5597-L5636)、[`kernel/sched/fair.c:13605-13621`](../../linux/kernel/sched/fair.c#L13605-L13621)。

### 数学模型：理论几何级数与整数实现

令 `uᵢ` 为第 `i` 个历史周期中某种状态的时间占比，则理论模型为：

```text
S = u₀ + u₁·y + u₂·y² + …，y = 0.5^(1/32)
```

每跨过一个周期，旧贡献乘以 `y`，新的贡献加入最高权重的位置，见 [`kernel/sched/pelt.c:152-179`](../../linux/kernel/sched/pelt.c#L152-L179)。内核先把时间差右移 10 位，以 1024 ns 近似 1 μs，再把 1024 个计量单位视为一个周期。因此周期严格为 `2²⁰ ns = 1.048576 ms`，半衰期是 32 个周期，即约 33.55 ms 的 **PELT 时间**；源码与文档常近似称为“1 ms 周期、32 ms 半衰期”，不是严格的墙上时间保证。

实现保存的是按周期内时长累积的整数和，而不是上述无量纲的 `S`。若满周期贡献为 1024，理想实数级数的上限为 `1024 / (1 − y) ≈ 47788.08`，不能把它直接等同于整数常量 47742。

[`kernel/sched/sched-pelt.h:14-15`](../../linux/kernel/sched/sched-pelt.h#L14-L15)（v6.18.60）

```c
#define LOAD_AVG_PERIOD 32
#define LOAD_AVG_MAX 47742
```

`LOAD_AVG_MAX` 来自定点乘法逐次截断后的收敛计算；生成程序按“旧和乘定点 `y`，右移 32 位，再加 1024”迭代，见 [`Documentation/scheduler/sched-pelt.c:58-85`](../../linux/Documentation/scheduler/sched-pelt.c#L58-L85)。真正求平均还要考虑当前周期的位置，不能始终用 `LOAD_AVG_MAX` 作除数。

### 增量更新

#### 衰减：`decay_load()`

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

计算 `val × yⁿ`：利用 `y³² = 1/2`，先右移 `n / 32` 位，再用查表系数计算余下的衰减。表项是 `floor((2³² − 1) × yⁱ)`，不是精确的 `yⁱ × 2³²`，见 [`Documentation/scheduler/sched-pelt.c:17-30`](../../linux/Documentation/scheduler/sched-pelt.c#L17-L30)。超过 `32 × 63` 个周期直接归零，也避免了超过 63 位的右移；整个计算不随睡眠时长线性增长。

#### 累加：`accumulate_sum()`

两次更新之间的时间 `delta` 可能跨越多个周期边界。下面横轴时间从左向右递增：

[`kernel/sched/pelt.c:86-90`](../../linux/kernel/sched/pelt.c#L86-L90)（v6.18.60，注释中的示意图）

```text
           d1          d2           d3
           ^           ^            ^
           |           |            |
         |<->|<----------------->|<--->|
 ... |---x---|------| ... |------|-----x (now)
```

*图 3-13：一次跨周期的 PELT 更新覆盖的三段时间*

- `d1`：上次更新所在周期的剩余部分；
- `d2`：中间完整的 `p − 1` 个周期；
- `d3`：当前周期已经过去的部分；`p` 是跨过的周期边界数。

函数先计算 `p = (delta + period_contrib) / 1024`。若 `p == 0`，不衰减，直接把本次 `delta` 加入累加和；否则先将旧和衰减 `p` 次，再计算三段新贡献。为突出分段计算，下面保留 `accumulate_sum()` 的跨周期分支与尾部，前面的变量初始化见 [`kernel/sched/pelt.c:102-110`](../../linux/kernel/sched/pelt.c#L102-L110)。

[`kernel/sched/pelt.c:115-150`](../../linux/kernel/sched/pelt.c#L115-L150)（v6.18.60，省略一段注释）

```c
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

新贡献为 `d1·yᵖ + 1024·Σ(n=1..p−1) yⁿ + d3`。中间的等比数列由 `LOAD_AVG_MAX − decay_load(LOAD_AVG_MAX, p) − 1024` 近似计算，整个区间只需常数次运算，见 [`kernel/sched/pelt.c:58-79`](../../linux/kernel/sched/pelt.c#L58-L79)。

`period_contrib` 保存新的周期内位置。三个输入不是都当作布尔值：`load` 和 `runnable` 分别作为乘数，`running` 则只判断是否非零。此算法假设两次更新之间输入不变，因此状态切换前必须先按旧状态结算。

#### 时间推进与求平均

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

除数不是固定的 `LOAD_AVG_MAX`，而是 `LOAD_AVG_MAX − 1024 + period_contrib`（[`kernel/sched/pelt.h:45-50`](../../linux/kernel/sched/pelt.h#L45-L50)）。当前周期尚未走完的部分不应被当作空闲，否则一个始终运行的任务的 `util_avg` 会在约 1002～1024 之间振荡，推导见 [`kernel/sched/pelt.c:233-256`](../../linux/kernel/sched/pelt.c#L233-L256)。普通实体的 `load_sum` 不含权重，求 `load_avg` 时才乘 `se_weight(se)`；队列的 `load_sum` 已包含历史权重，求平均时乘数传 1。

调用顺序上，包装函数先执行 `___update_load_sum()`，只有其返回 1 才执行上面的求平均。它不是外部 API，而是标记为 `static __always_inline` 的内部函数。时间处理有三个边界：

1. 若 `(s64)(now − last_update_time) < 0`，只重置时间戳，不累加，处理调度时钟初始化等回退情况。
2. `delta >>= 10` 后为 0 时直接返回；否则只推进 `delta << 10`，保留不足 1024 ns 的余数，而不是直接令时间戳等于 `now`。
3. 有计量单位推进时，`*_sum` 与 `period_contrib` 就会更新；只有跨过周期边界才返回 1，因此 `*_avg` 不一定与刚更新的 `*_sum` 同步。

这些逻辑见 [`kernel/sched/pelt.c:180-206`](../../linux/kernel/sched/pelt.c#L180-L206)。此外，函数在 [`kernel/sched/pelt.c:208-230`](../../linux/kernel/sched/pelt.c#L208-L230) 强制 `load == 0` 时令 `runnable = running = 0`，避免实体已经出队、`cfs_rq->curr` 却暂时仍指向它的过渡状态误记运行贡献。这也是跨周期分支在 `load == 0` 时可以跳过三段贡献计算的前提。

### 各类实体的更新

[`kernel/sched/pelt.c`](../../linux/kernel/sched/pelt.c) 为不同对象提供了包装函数。先看公平类实体和队列，它们的区别主要在于输入信号和求平均时的权重：

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
| 公平调度实体 | `!!se->on_rq` | `se_runnable(se)` | `cfs_rq->curr == se` | `se->avg` |
| `cfs_rq` | `scale_load_down(cfs_rq->load.weight)` | `h_nr_runnable` | `cfs_rq->curr != NULL` | `cfs_rq->avg` |
| RT | 是否在运行 RT | 同左 | 同左 | `rq->avg_rt`，主要使用 `util_avg` |
| Deadline | 是否在运行 DL | 同左 | 同左 | `rq->avg_dl`，主要使用 `util_avg` |
| IRQ，启用时 | 空闲段为 0，占用段为 1 | 同左 | 同左 | `rq->avg_irq.util_avg` |
| 硬件压力，启用时 | 损失的算力值 | 同左 | 同左 | `rq->avg_hw.load_avg` |

`se_runnable()` 对延迟出队实体返回 0；普通任务返回 `!!se->on_rq`，组实体返回缓存的 `runnable_weight`，由 `se_update_runnable()` 从子队列的 `h_nr_runnable` 更新，见 [`kernel/sched/sched.h:924-963`](../../linux/kernel/sched/sched.h#L924-L963)。不能把实体的 `on_rq` 一概解释为“任务现在可运行”。

RT 与 DL 在 rq 级别统计运行时间，不为每个 RT / DL 任务维护这组 PELT 信号，见 [`kernel/sched/pelt.c:336-386`](../../linux/kernel/sched/pelt.c#L336-L386)。切入该类时先按 `running = 0` 结算此前区间，切出和 tick 时按 `running = 1` 结算运行区间；例如 [`kernel/sched/rt.c:1657-1666`](../../linux/kernel/sched/rt.c#L1657-L1666)、[`kernel/sched/rt.c:1716-1726`](../../linux/kernel/sched/rt.c#L1716-L1726) 与 [`kernel/sched/deadline.c:2544-2548`](../../linux/kernel/sched/deadline.c#L2544-L2548)、[`kernel/sched/deadline.c:2602-2612`](../../linux/kernel/sched/deadline.c#L2602-L2612)。

IRQ 跟踪不能使用不含 IRQ 时间的任务时钟：`update_irq_load_avg()` 先缩放已测得的占用时长，再在 `rq->clock` 上先结算非占用段、后结算占用段，见 [`kernel/sched/pelt.c:420-470`](../../linux/kernel/sched/pelt.c#L420-L470)。硬件压力则是“损失多少算力”的时间加权信号，使用未做不变性缩放的 `rq_clock_task()`，有效字段是 `load_avg`，不是 `util_avg`，见 [`kernel/sched/pelt.c:388-418`](../../linux/kernel/sched/pelt.c#L388-L418)、[`kernel/sched/pelt.c:477-490`](../../linux/kernel/sched/pelt.c#L477-L490)。

**本书配置的边界**：[`linux/.config`](../../linux/.config) 中 `CONFIG_SMP=y`，但 `CONFIG_IRQ_TIME_ACCOUNTING` 与 `CONFIG_PARAVIRT_TIME_ACCOUNTING` 都未开启，因而 `CONFIG_HAVE_SCHED_AVG_IRQ` 未启用；`CONFIG_SCHED_HW_PRESSURE` 也未启用。后两组信号在这里仅用于解释通用代码，不是本书运行时一定存在的字段，声明条件见 [`kernel/sched/sched.h:1230-1237`](../../linux/kernel/sched/sched.h#L1230-L1237)，配置依赖见 [`init/Kconfig:603-624`](../../linux/init/Kconfig#L603-L624)。当前配置下 `update_rq_clock_task()` 不扣除 IRQ / steal 时间，不能把通用注释中的理想分账方式无条件套用，见 [`kernel/sched/core.c:787-843`](../../linux/kernel/sched/core.c#L787-L843)。

### 在公平类中的维护

#### `update_load_avg()`

[`kernel/sched/fair.c:4904-4944`](../../linux/kernel/sched/fair.c#L4904-L4944)（v6.18.60）

```c
static inline void update_load_avg(struct cfs_rq *cfs_rq, struct sched_entity *se, int flags)
{
	u64 now = cfs_rq_clock_pelt(cfs_rq);
	int decayed;

...
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

调用点不是都在函数开头，但都必须在相关 PELT 输入变化前结算：`enqueue_entity()` 在设置 `on_rq` 前更新，`dequeue_entity()` 在清除它之前更新；`set_next_entity()` 在设置 `curr` 前更新，`put_prev_entity()` 在清空 `curr` 前更新，见 [`kernel/sched/fair.c:5427-5480`](../../linux/kernel/sched/fair.c#L5427-L5480)、[`kernel/sched/fair.c:5557-5663`](../../linux/kernel/sched/fair.c#L5557-L5663)、[`kernel/sched/fair.c:5707-5743`](../../linux/kernel/sched/fair.c#L5707-L5743)。

`update_load_avg()` 的工作分为三层：

1. 先结算实体，再结算 `cfs_rq`。`update_cfs_rq_load_avg()` 先扣除 `removed` 中的异步移除量，再推进队列的时间，见 [`kernel/sched/fair.c:4745-4808`](../../linux/kernel/sched/fair.c#L4745-L4808)。
2. 依赖 `CONFIG_FAIR_GROUP_SCHED=y`，`propagate_entity_load_avg()` 把子队列的变化传播到组实体和父队列。利用率与可运行信号可以传递，负载还要考虑组权重，不能把所有 nice 权重直接跨层相加，见 [`kernel/sched/fair.c:4572-4601`](../../linux/kernel/sched/fair.c#L4572-L4601)。
3. 根据标志决定是否改变 PELT 归属，并按相应分支通知 cpufreq 或更新任务组的全局负载。`DO_ATTACH` 的实际判断是“尚无有效时间戳且允许挂接”，不只服务迁移，也包括新实体首次挂接；`DO_DETACH` 用于有锁的迁出路径。

`UPDATE_TG`、`SKIP_AGE_LOAD`、`DO_ATTACH`、`DO_DETACH` 的定义见 [`kernel/sched/fair.c:4895-4901`](../../linux/kernel/sched/fair.c#L4895-L4901)。其中 `SKIP_AGE_LOAD` 只跳过实体的时间推进，不跳过队列更新；`cfs_rq_util_change()` 只在根 `cfs_rq` 上直接触发 `cpufreq_update_util()`，子队列的变化需向上到达根队列，见 [`kernel/sched/fair.c:4155-4178`](../../linux/kernel/sched/fair.c#L4155-L4178)。

#### PELT 挂接不是可运行队列的入队

普通睡眠只改变当前输入，不会自动调用 `detach_entity_load_avg()`。同一 CPU 上再次唤醒时，实体通常仍属于原来的 PELT 聚合，不应再次把整份历史加一遍。

真正挂接时必须对齐时间窗口。`attach_entity_load_avg()` 把实体的 `last_update_time`、`period_contrib` 对齐到目标队列，并用目标除数从 `*_avg` 重建 `*_sum`，再加入队列；不能把旧 CPU 上的原始累加和直接相加，见 [`kernel/sched/fair.c:4818-4863`](../../linux/kernel/sched/fair.c#L4818-L4863)。反向移除也同时更新和与平均值，并用 `avg × PELT_MIN_DIVIDER` 维护舍入后的下界，避免频繁迁移后出现“平均值非零、累加和却已减成零”，见 [`kernel/sched/fair.c:4873-4893`](../../linux/kernel/sched/fair.c#L4873-L4893)。

迁移有两条路径，见 [`kernel/sched/fair.c:8815-8839`](../../linux/kernel/sched/fair.c#L8815-L8839)：

| 场景 | 旧 CPU 上如何移除 | 到达新 CPU 前的处理 |
|---|---|---|
| 可运行任务以 `TASK_ON_RQ_MIGRATING` 迁移 | `dequeue_entity()` 设置 `DO_DETACH`，持旧 rq 锁同步移除 | 将 `last_update_time` 置 0，入队时重新挂接 |
| 非 `TASK_ON_RQ_MIGRATING` 路径，例如睡眠任务唤醒迁移 | `remove_entity_load_avg()` 先同步实体，再在 `removed.lock` 下累计待移除量 | 必要时修正空闲源 CPU 的时钟滞后，然后置 0 |

第二条路径不必持旧 rq 锁。旧队列下次更新时才扣除 `removed`，而不是“任意迁移都异步移除”，见 [`kernel/sched/fair.c:4949-4983`](../../linux/kernel/sched/fair.c#L4949-L4983)。依赖 `CONFIG_NO_HZ_COMMON=y`，`migrate_se_pelt_lag()` 还会在源 CPU 空闲、时钟可能陈旧时估计漏掉的空闲时间，继续衰减任务，避免到新 CPU 后利用率虚高；这里的 PELT lag 是时间滞后，不是 EEVDF 的公平性 lag，见 [`kernel/sched/fair.c:4648-4726`](../../linux/kernel/sched/fair.c#L4648-L4726)。

#### 睡眠贡献与延迟出队

队列聚合保留已睡眠实体的历史贡献，称为阻塞负载（Blocked Load），见 [`kernel/sched/fair.c:4731-4743`](../../linux/kernel/sched/fair.c#L4731-L4743)。实体真正出队后，队列按新的瞬时权重和运行状态继续更新，旧贡献逐步衰减。睡眠任务自己的 `se->avg` 通常到唤醒或显式同步时才追赶队列时间；队列衰减不要求逐个修改睡眠任务。

本版本默认启用延迟出队（`DELAY_DEQUEUE`，[`kernel/sched/features.h:58`](../../linux/kernel/sched/features.h#L58)），因此“睡眠”不一定立即使 `se->on_rq` 归零。负 lag 的普通睡眠实体可暂留队列，`set_delayed()` 会减少层级上的 `h_nr_runnable`，而 `se_runnable()` 返回 0；但 `load` 输入仍由 `on_rq` 决定，可能保持为 1，直至真正出队。必须区分 `load` 与 `runnable`，不能用一个布尔状态描述三组输入，见 [`kernel/sched/fair.c:5511-5528`](../../linux/kernel/sched/fair.c#L5511-L5528)、[`kernel/sched/fair.c:5566-5585`](../../linux/kernel/sched/fair.c#L5566-L5585)。

停 tick 的空闲 CPU 缺少常规更新机会，nohz 负载均衡会补做 blocked load 更新。`sched_balance_update_blocked_averages()` 持目标 rq 锁、更新时钟，先结算 RT / DL 等信号，再自底向上更新公平队列，必要时通知 cpufreq；`rq->has_blocked_load` 表示是否还需继续维护，见 [`kernel/sched/fair.c:10045-10116`](../../linux/kernel/sched/fair.c#L10045-L10116)、[`kernel/sched/fair.c:10181-10199`](../../linux/kernel/sched/fair.c#L10181-L10199)。它不是每 32 ms 精确触发的定时器。

#### 新任务的初值

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

新任务的 `load_avg` 初始为满权重，避免尚未运行就被当作轻任务；组实体从零负载开始。`util_avg` 则由 `post_init_entity_util_avg()` 根据目标队列外推，并限制为剩余算力预算的一半；目标队列利用率为零时直接采用这个上限，预算非正时保留零值。它还令 `runnable_avg = util_avg`，随后挂接时再从这些平均值建立累加和，见 [`kernel/sched/fair.c:1204-1240`](../../linux/kernel/sched/fair.c#L1204-L1240)。注释中的 512、256、128… 是算力为 1024、连续采用上限时的示意序列，不是所有 fork 的固定初值。

### 频率与算力不变性

相同工作在低频或小核上需要更长运行时间，直接使用运行时间比例会把同一需求测成不同大小。频率不变性（Frequency Invariance）与算力不变性（CPU Invariance）通过专用时钟修正这一问题，而不是每次更新平均值时再乘一个系数：

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

运行时，`delta` 先乘当前 CPU 的相对最大算力，再乘频率比例，两次都按 1024 刻度归一化；空闲时底层 `clock_pelt` 追赶 `clock_task`。例如同一周期工作在半频下需要更长时间，但缩放后的有效工作时长与高频情况接近，追赶部分则表现为空闲时间，见 [`kernel/sched/pelt.h:78-127`](../../linux/kernel/sched/pelt.h#L78-L127)。

这里还有两个不能省略的修正：

- **饱和时不能补入虚假的空闲**。若 CFS、RT、DL 的 `util_sum` 之和接近满载，`update_idle_rq_clock_pelt()` 把低算力运行期间积累的时钟差记入 `lost_idle_time`；真正使用的 `rq_clock_pelt()` 返回 `clock_pelt − lost_idle_time`，见 [`kernel/sched/pelt.h:69-75`](../../linux/kernel/sched/pelt.h#L69-L75)、[`kernel/sched/pelt.h:129-162`](../../linux/kernel/sched/pelt.h#L129-L162)。因此不能只用“空闲时无条件把全部差值作为衰减”解释满载后的行为。
- **带宽节流可冻结相应队列的 PELT 时间**。本书 `CONFIG_CFS_BANDWIDTH=y`；当节流路径设置 `pelt_clock_throttled` 时，`cfs_rq_clock_pelt()` 使用冻结值，恢复后还会扣除累计节流时间，避免把 quota 限制误算成任务自身需求降低。时钟实现见 [`kernel/sched/pelt.h:164-181`](../../linux/kernel/sched/pelt.h#L164-L181)，冻结与恢复条件见 [`kernel/sched/fair.c:5482-5495`](../../linux/kernel/sched/fair.c#L5482-L5495)、[`kernel/sched/fair.c:5624-5635`](../../linux/kernel/sched/fair.c#L5624-L5635)。这与任务主动睡眠不同，详见 3.10 节。

在本书 x86_64 SMP 配置下，`arch_scale_freq_tick()` 用 APERF / MPERF 的差值更新频率比例，但仍依赖硬件提供计数器且运行时启用了频率不变性；虚拟机不一定满足。异构算力未启用时 `arch_scale_cpu_capacity()` 返回 1024，相关实现见 [`arch/x86/kernel/cpu/aperfmperf.c:428-497`](../../linux/arch/x86/kernel/cpu/aperfmperf.c#L428-L497)。不能仅凭编译配置就断言虚拟机能观察到动态频率缩放。

### `util_est`：利用率估计

PELT 在任务睡眠期间衰减。周期性任务再次唤醒时，即使每次激活的工作量相近，当前 `util_avg` 也可能低于上次激活结束时的值，导致选核与调频重新爬升。

`UTIL_EST` 默认开启，见 [`kernel/sched/features.h:129`](../../linux/kernel/sched/features.h#L129)。任务的 `util_est` 保存激活结束时利用率的估计，下降时使用指数加权移动平均（Exponentially Weighted Moving Average，EWMA），并非简单复制上次值。`util_est_update()` 仅在 `DEQUEUE_SLEEP` 时取样，迁移不算激活结束；若自上次取样以来 PELT 未更新，也跳过取样，入口检查见 [`kernel/sched/fair.c:5050-5074`](../../linux/kernel/sched/fair.c#L5050-L5074)。以下是取样后的分支：

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

**上升立即跟随，下降缓慢平滑**：若样本不小于旧估计，直接采用样本；下降幅度小于 `UTIL_EST_MARGIN = 1024 / 100 = 10` 时维持旧值。若 `dequeued + 10 < task_runnable(p)`，说明运行机会可能不足，这次也不下调，避免把争用误判为需求降低。

只有通过这些检查的下降样本才执行 `ewma_new = floor((3 × ewma_old + dequeued) / 4)`。实际代码的移位次序如下，而不是先算 `(ewma_old − dequeued) / 4` 再相减，二者可能差 1：

[`kernel/sched/fair.c:5119-5128`](../../linux/kernel/sched/fair.c#L5119-L5128)（v6.18.60）

```c
	ewma <<= UTIL_EST_WEIGHT_SHIFT;
	ewma  -= last_ewma_diff;
	ewma >>= UTIL_EST_WEIGHT_SHIFT;
done:
	ewma |= UTIL_AVG_UNCHANGED;
	WRITE_ONCE(p->se.avg.util_est, ewma);

	trace_sched_util_est_se_tp(&p->se);
}
```

`UTIL_AVG_UNCHANGED` 的生命周期是：完成取样或某些跳过分支后置位；后续实体 PELT 跨周期更新时，`cfs_se_util_change()` 清除此位，允许下次睡眠重新取样，见 [`kernel/sched/pelt.h:52-67`](../../linux/kernel/sched/pelt.h#L52-L67)。`_task_util_est()` 读取时先屏蔽该位，不能直接把 `se->avg.util_est` 的原始整数当作 CPU 需求。

根 `rq->cfs.avg.util_est` 通过 `util_est_enqueue()` / `util_est_dequeue()` 增减 `_task_util_est(p)`，不是逐任务的 `max(util_avg, util_est)` 之和，见 [`kernel/sched/fair.c:5016-5046`](../../linux/kernel/sched/fair.c#L5016-L5046)。`enqueue_task_fair()` 在可能触发调频通知前先加估计值，`dequeue_task_fair()` 先扣旧估计再更新任务估计；延迟出队实体避免重复扣除，见 [`kernel/sched/fair.c:7101-7113`](../../linux/kernel/sched/fair.c#L7101-L7113)、[`kernel/sched/fair.c:7329-7340`](../../linux/kernel/sched/fair.c#L7329-L7340)。

`task_util_est()` 则是另一个辅助函数：它返回单任务 PELT 利用率与去标志估计值的较大值。它本身不检查 `UTIL_EST` 开关，不能把它与队列聚合路径混为一谈：

[`kernel/sched/fair.c:5011-5014`](../../linux/kernel/sched/fair.c#L5011-L5014)（v6.18.60）

```c
static inline unsigned long task_util_est(struct task_struct *p)
{
	return max(task_util(p), _task_util_est(p));
}
```

### CPU 利用率：`cpu_util()`

`cpu_util()` 估计的是 **CFS 部分**的 CPU 需求，不是包括所有调度类与中断在内的总利用率。先从根队列的 `util_avg` 开始，按需要模拟任务迁入 / 迁出；`boost` 打开时，还把 `runnable_avg` 纳入比较：

[`kernel/sched/fair.c:8156-8185`](../../linux/kernel/sched/fair.c#L8156-L8185)（v6.18.60，省略一段注释）

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
		if (add_task)
			runnable += READ_ONCE(p->se.avg.runnable_avg);
		else if (sub_task)
			lsub_positive(&runnable,
				      READ_ONCE(p->se.avg.runnable_avg));
		util = max(util, runnable);
	}
```

然后处理 `util_est` 并返回。下面接续同一函数，补齐估计值修正与最终钳制：

[`kernel/sched/fair.c:8186-8226`](../../linux/kernel/sched/fair.c#L8186-L8226)（v6.18.60，省略一段注释）

```c
	if (sched_feat(UTIL_EST)) {
		unsigned long util_est;

		util_est = READ_ONCE(cfs_rq->avg.util_est);
...
		if (dst_cpu == cpu)
			util_est += _task_util_est(p);
		else if (p && unlikely(task_on_rq_queued(p) || current == p))
			lsub_positive(&util_est, _task_util_est(p));

		util = max(util, util_est);
	}

	return min(util, arch_scale_cpu_capacity(cpu));
}
```

两类修正的条件不同：睡眠任务的历史 `util_avg` 仍在源队列中，但它已不计入可运行任务的 `util_est` 总和；因此不能给二者机械地套同一组加减条件。唤醒预测可向目标队列加估计值，移除估计值时还要检查任务确实在队列或为 `current`，以缩小与负载均衡出队竞态的窗口，解释见 [`kernel/sched/fair.c:8192-8219`](../../linux/kernel/sched/fair.c#L8192-L8219)。

最终上限是 `arch_scale_cpu_capacity(cpu)`，即该 CPU 的原始最大算力，不是已经扣除 RT / DL / IRQ 压力的 `capacity_of(cpu)`，更不是当前频率下的瞬时算力。`cpu_util_cfs()` 关闭 `boost`，`cpu_util_cfs_boost()` 打开它，见 [`kernel/sched/fair.c:8228-8236`](../../linux/kernel/sched/fair.c#L8228-L8236)。

主要使用者如下：

| 使用者 | 用到的信号与后续处理 |
|---|---|
| schedutil 调频 | `sugov_get_util()` 取 `cpu_util_cfs_boost()`，再经 `effective_cpu_util()` 合成 RT / DL / IRQ 与钳制提示，见 [`kernel/sched/cpufreq_schedutil.c:237-247`](../../linux/kernel/sched/cpufreq_schedutil.c#L237-L247) |
| 负载均衡，3.9 节 | `cpu_load()` 读取 `load_avg`，同时结合 `cpu_util_cfs()` 与 `runnable_avg`，见 [`kernel/sched/fair.c:7374-7377`](../../linux/kernel/sched/fair.c#L7374-L7377) |
| 唤醒亲和选核，3.12 节 | `wake_affine_weight()` 使用 `cpu_load()`、`task_h_load()`，见 [`kernel/sched/fair.c:7528-7569`](../../linux/kernel/sched/fair.c#L7528-L7569) |
| 可用算力计算 | `get_actual_cpu_capacity()` 从原始算力中扣除硬件压力与 cpufreq 压力的较大值；`scale_rt_capacity()` 再扣除 RT / DL 并按 IRQ 比例缩放，见 [`kernel/sched/fair.c:5130-5137`](../../linux/kernel/sched/fair.c#L5130-L5137)、[`kernel/sched/fair.c:10261-10297`](../../linux/kernel/sched/fair.c#L10261-L10297) |
| EAS、misfit 检测 | 使用任务估计值、CPU 利用率与算力匹配判据，不直接用 nice 权重代替算力需求，见 [`kernel/sched/fair.c:5139-5148`](../../linux/kernel/sched/fair.c#L5139-L5148)、[`kernel/sched/fair.c:8443-8460`](../../linux/kernel/sched/fair.c#L8443-L8460) |

`effective_cpu_util()` 不是简单把 IRQ 再加一次。令 `C` 为原始算力、`I` 为 IRQ 利用率、`U` 为 CFS + RT + DL 利用率，在未饱和分支中合成为 `I + (C − I) × U / C`，因为任务时钟与 IRQ 时间的基准不同；它还通过 `min` / `max` 输出 Deadline 带宽和 `uclamp` 提示，见 [`kernel/sched/fair.c:8280-8351`](../../linux/kernel/sched/fair.c#L8280-L8351)。当前配置未启用 IRQ PELT，`cpu_util_irq()` 返回 0，见 [`kernel/sched/sched.h:3462-3467`](../../linux/kernel/sched/sched.h#L3462-L3467)。

本书 `CONFIG_CPU_FREQ_DEFAULT_GOV_SCHEDUTIL=y` 只表示 cpufreq 默认调频器的编译选择；实际驱动、运行模式和虚拟机是否提供调频接口仍需运行时确认。

## 关键细节 / 设计考量

- **可组合性使迁移成本可控**。在相同时钟与窗口位置下，几何衰减是线性的，可以转移实体贡献而不必重放运行历史。实际代码仍需处理窗口对齐、任务组权重和整数舍入，不能把数学上的线性当作无条件精确相等，见 [`Documentation/scheduler/schedutil.rst:25-35`](../../linux/Documentation/scheduler/schedutil.rst#L25-L35)。
- **时间刻度与算力刻度不同**。时间计量单位是 1024 ns，一个周期含 1024 个单位；`SCHED_CAPACITY_SCALE = 1024` 是另一种定点数刻度，定义见 [`include/linux/sched.h:448-453`](../../linux/include/linux/sched.h#L448-L453)。二者数值相同，不代表平均值直接以 μs 表示。
- **权重不是利用率上限**。nice 19 的持续运行任务，在独占一个原始算力为 1024 的 CPU、充分收敛时，`util_avg` 可接近 1024，但 `load_avg` 接近 15。15 来自权重表，见 [`kernel/sched/core.c:10331-10340`](../../linux/kernel/sched/core.c#L10331-L10340)。x86_64 的 `se.load.weight` 还有额外精度，不能直接拿 `/proc` 中该字段与 `load_avg` 比较，见 [`kernel/sched/sched.h:133-164`](../../linux/kernel/sched/sched.h#L133-L164)。
- **PELT 不等于系统 load average**。这里讨论的 `load_avg` 属于实体或公平队列，不是 `/proc/loadavg` 的 1、5、15 分钟系统负载均值，后者见 3.5 节。

## 可观测性

### 任务与队列快照

以下配置值均以 [`linux/.config`](../../linux/.config) 为准。接口展示的是最近一次结算的结果，读取本身不保证触发 PELT 追赶当前时间。

| 入口 | 看什么、如何解读 | 输出位置与配置 |
|---|---|---|
| `/proc/<pid>/sched` | `se.avg.load_sum` / `runnable_sum` / `util_sum`、三组平均值、`last_update_time`、`util_est`；估计值输出已屏蔽 `UTIL_AVG_UNCHANGED` | `proc_sched_show_task()`，[`kernel/sched/debug.c:1232-1240`](../../linux/kernel/sched/debug.c#L1232-L1240)；`CONFIG_PROC_FS=y`，入口见 [`fs/proc/base.c:3311`](../../linux/fs/proc/base.c#L3311) |
| `/sys/kernel/debug/sched/debug` | `cfs_rq[CPU]:路径` 下的 `h_nr_runnable`、`load_avg`、`runnable_avg`、`util_avg`、`util_est` 与 `removed.*`；`runnable_avg` 可超过 1024，睡眠后的队列利用率可继续衰减 | `print_cfs_rq()`，[`kernel/sched/debug.c:801-862`](../../linux/kernel/sched/debug.c#L801-L862)；`CONFIG_DEBUG_FS=y`，创建见 [`kernel/sched/debug.c:530`](../../linux/kernel/sched/debug.c#L530) |
| `/sys/kernel/debug/sched/features` | 确认 `UTIL_EST` / `NO_UTIL_EST`、`DELAY_DEQUEUE` / `NO_DELAY_DEQUEUE`；这些是运行时调度特性，不是同名 `CONFIG_*` | `sched_feat_show()`，[`kernel/sched/debug.c:59-72`](../../linux/kernel/sched/debug.c#L59-L72)；`CONFIG_DEBUG_FS=y`，创建见 [`kernel/sched/debug.c:501`](../../linux/kernel/sched/debug.c#L501) |

本树没有 `CONFIG_SCHED_DEBUG` 配置项；不要照搬其他版本的接口开启条件。调度调试实现由 [`kernel/sched/build_utility.c:71`](../../linux/kernel/sched/build_utility.c#L71) 引入。`CONFIG_SCHEDSTATS=y` 提供额外的调度统计，但 PELT 这些字段的打印不位于 `schedstat_enabled()` 分支内，不要求先开启 `kernel.sched_schedstats`。

### 裸跟踪点与动态探针

PELT 提供的是裸跟踪点（Bare Tracepoint），不是可在 tracefs 中直接启用的 `sched:pelt_*` 事件。声明见 [`include/trace/events/sched.h:829-871`](../../linux/include/trace/events/sched.h#L829-L871)，`DECLARE_TRACE()` 会自动给名字追加 `_tp`，见 [`include/linux/tracepoint.h:474-477`](../../linux/include/linux/tracepoint.h#L474-L477)。

| 裸跟踪点 | 参数及含义 | 触发位置 |
|---|---|---|
| `pelt_se_tp` | `struct sched_entity *`，观察实体的 `avg`，组实体不一定对应单个 PID | [`kernel/sched/pelt.c:296-318`](../../linux/kernel/sched/pelt.c#L296-L318) |
| `pelt_cfs_tp` | `struct cfs_rq *`，观察队列的三组平均值 | 周期更新见 [`kernel/sched/pelt.c:321-334`](../../linux/kernel/sched/pelt.c#L321-L334)；挂接、移除和组传播也会触发 |
| `pelt_rt_tp` / `pelt_dl_tp` | `struct rq *`，读取 `avg_rt.util_avg` / `avg_dl.util_avg` | [`kernel/sched/pelt.c:347-386`](../../linux/kernel/sched/pelt.c#L347-L386) |
| `pelt_irq_tp` / `pelt_hw_tp` | `struct rq *`，分别观察 IRQ 利用率和硬件压力 | 依赖 `CONFIG_HAVE_SCHED_AVG_IRQ` / `CONFIG_SCHED_HW_PRESSURE`，本书均未启用，见 [`kernel/sched/pelt.c:388-470`](../../linux/kernel/sched/pelt.c#L388-L470) |
| `sched_util_est_se_tp` / `sched_util_est_cfs_tp` | 分别为 `struct sched_entity *` / `struct cfs_rq *`，观察任务估计值写回与队列估计值增减 | [`kernel/sched/fair.c:5016-5046`](../../linux/kernel/sched/fair.c#L5016-L5046)、[`kernel/sched/fair.c:5123-5127`](../../linux/kernel/sched/fair.c#L5123-L5127) |

`pelt_*` 通常在平均值更新后触发，但 `pelt_cfs_tp` 还覆盖聚合调整，不能把其触发次数解释成精确的 PELT 周期数。这些参数是内核对象指针，不是已序列化的字段；读取任务原始 `util_est` 时仍要去掉最高标志位。

本书 `CONFIG_BPF_SYSCALL=y`、`CONFIG_BPF_EVENTS=y`、`CONFIG_TRACEPOINTS=y`，可以用 BPF raw tracepoint 挂接；未启用 `CONFIG_DEBUG_INFO_BTF`，不能假定 bpftrace 能自动识别 `struct cfs_rq` 等内部类型。下方实验只计数，不解引用对象。若要获取字段，应使用与运行内核匹配的类型信息，或编写匹配本版本头文件的内核探测模块。

动态探针可优先检查 `__update_load_avg_se()`、`__update_load_avg_cfs_rq()`、`update_rt_rq_load_avg()`、`update_dl_rq_load_avg()` 是否保留为可探测符号。`___update_load_sum()`、`___update_load_avg()`、`accumulate_sum()` 为 `__always_inline`，`update_load_avg()` 也是内联候选，`decay_load()` 为静态函数，不能保证可以直接挂接。`CONFIG_KPROBE_EVENTS=y` 支持 kprobe / kretprobe；fentry / fexit 还需要 BTF 和实际可用的函数入口，不能仅凭源码函数名判断。

## 动手实验

以下步骤供 **dev 虚拟机** 验证，尚未实测，不附虚构输出。先确认运行的是本书内核，并确保至少有两个在线 vCPU、CPU 1 在当前亲和性允许范围内。示例将任务固定在 CPU 1，最好让观测程序运行在另一 CPU。读取队列快照需要挂载 debugfs；不要在生产环境随意切换 `UTIL_EST` 等全局特性。

### 1. 对比任务快照与队列的阻塞负载衰减

```bash
# 虚拟机，root；若 CPU 1 不可用，请修改 cpu
uname -r
cpu=1
mountpoint -q /sys/kernel/debug || mount -t debugfs none /sys/kernel/debug
(
    taskset -c "$cpu" sh -c 'while :; do :; done' &
    pid=$!
    trap 'kill -CONT "$pid" 2>/dev/null; kill "$pid" 2>/dev/null; wait "$pid" 2>/dev/null' EXIT
    for i in $(seq 10); do
        grep -E 'se\.avg\.(util_avg|runnable_avg|load_avg|util_est)' "/proc/$pid/sched"
        sleep 0.02
    done
    kill -STOP "$pid"
    sleep 0.02
    for i in $(seq 10); do
        grep -E 'se\.avg\.(util_avg|last_update_time)' "/proc/$pid/sched"
        awk -v header="cfs_rq[$cpu]:/" '
            $0 == header { found = 1; next }
            found && /^[^[:space:]]/ { exit }
            found && /\.(util_avg|util_est)[[:space:]]*:/ { print }
        ' /sys/kernel/debug/sched/debug
        sleep 0.034
    done
)
```

充分运行后，若任务独占原始算力为 1024 的 CPU，`se.avg.util_avg` 应接近 1024。暂停后，任务快照的 `last_update_time` 通常不再前进，而队列历史贡献仍会衰减；队列 `util_est` 在出队时扣除，不像 PELT 那样保留阻塞贡献。根队列还包括同 CPU 的其他公平任务，不能把整个 `util_avg` 都归到实验任务。

若看到阶梯状而非每次采样减半，不应直接判为错误：采样间隔不是更新周期，nohz 更新有延迟，PELT 时钟也不总等于墙上时间。若任务位于 CPU 带宽受限的 cgroup，还要考虑节流期间冻结队列时钟的影响。

### 2. 对比 nice 权重与利用率

```bash
# 虚拟机，普通用户；与实验 1 分开运行
cpu=1
(
    taskset -c "$cpu" nice -n 19 sh -c 'while :; do :; done' &
    pid=$!
    trap 'kill "$pid" 2>/dev/null; wait "$pid" 2>/dev/null' EXIT
    sleep 1
    grep -E 'se\.avg\.(util_avg|load_avg)|se\.load\.weight' "/proc/$pid/sched"
)
```

无竞争且充分收敛时，nice 19 不妨碍 `util_avg` 接近该 CPU 的满载刻度，但 `load_avg` 接近 15；x86_64 的 `se.load.weight` 通常显示为 `15 × 1024`，额外的 1024 来自权重精度，而不是任务有 15 个 CPU 的利用率。

### 3. 统计裸跟踪点触发次数

```bash
# 虚拟机，root；先检查探针是否可用，按 Ctrl-C 结束计数
bpftrace -l 'rawtracepoint:pelt_cfs_tp'
bpftrace -e '
rawtracepoint:pelt_cfs_tp { @[cpu] = count(); }
interval:s:1 { print(@); clear(@); }
'
```

该脚本不依赖解引用 `struct cfs_rq`，也不要求 BTF。它统计的是执行挂钩的 CPU，远程 blocked load 更新时不一定是参数队列所属的 CPU；同一 CPU 也可能维护多个任务组队列。因此结果只能反映挂钩活动量，不能当作“每 CPU 的唯一队列每毫秒更新一次”的直接证明。

## 小结

- PELT 以 1024 ns 为计量单位、1024 个单位为周期，32 个周期使历史贡献减半；“32 ms”是近似说法，而且属于 PELT 时间。
- `LOAD_AVG_MAX = 47742` 是整数定点迭代的收敛常量；求平均使用 `LOAD_AVG_MAX − 1024 + period_contrib`，不能直接等同于实数几何级数的和。
- `*_sum` 可以在周期内推进，`*_avg` 通常只在跨周期时重算。`load`、`runnable`、`running` 含义不同，尤其要考虑延迟出队。
- PELT 挂接与可运行入队不是同一件事：睡眠保留历史，迁移需要移除、对齐窗口并重新挂接，且同步 detach 与异步 `removed` 是两条不同路径。
- `rq_clock_pelt()` 同时处理频率 / 算力缩放和 `lost_idle_time`，带宽节流还会冻结相应队列的时间；IRQ 与硬件压力有独立的时钟与配置边界。
- `util_est` 在激活结束时更新，上升立即跟随、下降按条件平滑；队列估计值是任务估计值之和，`cpu_util()` 再与 CFS PELT 信号比较并钳制。
- 观测时应同时区分任务快照、队列聚合与裸跟踪点；停止任务自己的快照不持续衰减，不代表队列的阻塞负载没有更新。

## 参考

- [`Documentation/scheduler/sched-pelt.c`](../../linux/Documentation/scheduler/sched-pelt.c)：生成查表系数与 `LOAD_AVG_MAX` 的程序。
- [`Documentation/scheduler/sched-capacity.rst`](../../linux/Documentation/scheduler/sched-capacity.rst)：CPU 算力、任务利用率与不变性。
- [`Documentation/scheduler/schedutil.rst`](../../linux/Documentation/scheduler/schedutil.rst)：PELT、`util_est` 与 schedutil 的设计说明；具体聚合和判据以本节引用的 C 源码为准。
- [`Documentation/scheduler/sched-util-clamp.rst`](../../linux/Documentation/scheduler/sched-util-clamp.rst)：`uclamp` 对性能请求的限制。
- [`kernel/sched/pelt.h`](../../linux/kernel/sched/pelt.h)：有效 PELT 时钟、满载后的空闲时间修正与节流时钟处理。
