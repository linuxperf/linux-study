# 3.4 任务创建与唤醒

> **本节源码**：[`kernel/sched/core.c`](../../linux/kernel/sched/core.c)、[`kernel/sched/fair.c`](../../linux/kernel/sched/fair.c)、[`kernel/sched/sched.h`](../../linux/kernel/sched/sched.h)、[`kernel/fork.c`](../../linux/kernel/fork.c)、[`kernel/smp.c`](../../linux/kernel/smp.c)、[`include/linux/sched.h`](../../linux/include/linux/sched.h)
> **关键函数**：`sched_fork()`、`sched_cgroup_fork()`、`wake_up_new_task()`、`try_to_wake_up()`、`ttwu_runnable()`、`ttwu_queue()`、`ttwu_queue_wakelist()`、`sched_ttwu_pending()`、`ttwu_do_activate()`、`select_task_rq()`、`select_task_rq_fair()`
> **前置知识**：3.1 调度器框架、3.2 主调度函数、3.3 EEVDF（`place_entity()` 与延迟出队）

## 概述

3.2 节分析了任务如何离开 CPU：要么被抢占（仍在运行队列上），要么主动睡眠（被 `try_to_block_task()` 移出运行队列）。本节分析相反的方向：一个任务如何**进入**运行队列——新建任务的第一次入队，以及睡眠任务的唤醒。

唤醒是调度器中并发最激烈的路径。唤醒者与被唤醒者可能在不同 CPU 上，被唤醒者可能还没有睡下、正在睡下、或者刚从另一个 CPU 上切走。`try_to_wake_up()` 用 `p->pi_lock`、`p->__state`、`p->on_rq`、`p->on_cpu` 四样东西，加上一组精心排列的内存屏障，在**尽量只获取一把 rq 锁**的前提下处理所有这些情况。唤醒还要决定把任务放到哪个 CPU 上，这由调度类的 `select_task_rq()` 负责。

## 睡眠与唤醒的协议

内核中等待某个条件的标准写法是：

```c
// 伪代码（非源码原文）
/* 等待方 */
for (;;) {
	set_current_state(TASK_UNINTERRUPTIBLE);
	if (CONDITION)
		break;
	schedule();
}
__set_current_state(TASK_RUNNING);

/* 唤醒方 */
CONDITION = 1;
wake_up_process(p);
```

这个协议要防止**丢失唤醒**（Lost Wakeup）：等待方检查条件为假之后、调用 `schedule()` 之前，唤醒方恰好设置了条件并执行唤醒。协议的正确性由三处保证：

1. `set_current_state()` 使用 `smp_store_mb()`，写状态后带一个完整内存屏障，保证"写状态"先于"读条件"：

   [`include/linux/sched.h:245-250`](../../linux/include/linux/sched.h#L245-L250)（v6.18.60）

   ```c
   #define set_current_state(state_value)					\
   	do {								\
   		debug_normal_state_change((state_value));		\
   		trace_set_current_state(state_value);			\
   		smp_store_mb(current->__state, (state_value));		\
   	} while (0)
   ```

2. `try_to_wake_up()` 在获取 `pi_lock` 后执行 `smp_mb__after_spinlock()`，保证"写条件"先于"读状态"。两侧屏障配对后，至少有一方能看到对方的写：要么等待方看到条件为真而不睡，要么唤醒方看到 `__state` 非 `TASK_RUNNING` 而执行唤醒。
3. 如果唤醒发生在等待方 `set_current_state()` 之后、`schedule()` 完成出队之前，任务仍在运行队列上（`on_rq` 为 1），唤醒方只需把 `__state` 改回 `TASK_RUNNING`；随后 `__schedule()` 看到 `prev_state == 0`，不会让它出队（3.2 节）。

## 新任务的第一次入队

### fork 路径中的调度器钩子

```
kernel_clone()                        kernel/fork.c:2573
├── copy_process()                    kernel/fork.c:1935
│   ├── sched_fork()                  kernel/fork.c:2158  // 初始化调度字段，状态 TASK_NEW
│   ├── sched_cgroup_fork()           kernel/fork.c:2302  // 确定 task_group 与初始 CPU
│   └── sched_post_fork()             kernel/fork.c:2429  // uclamp、sched_ext 收尾
└── wake_up_new_task()                kernel/fork.c:2645  // 选 CPU、首次入队
```

### sched_fork()

[`kernel/sched/core.c:4695-4751`](../../linux/kernel/sched/core.c#L4695-L4751)（v6.18.60，节选）

```c
int sched_fork(u64 clone_flags, struct task_struct *p)
{
	__sched_fork(clone_flags, p);
	/*
	 * We mark the process as NEW here. This guarantees that
	 * nobody will actually run it, and a signal or other external
	 * event cannot wake it up and insert it on the runqueue either.
	 */
	p->__state = TASK_NEW;

	/*
	 * Make sure we do not leak PI boosting priority to the child.
	 */
	p->prio = current->normal_prio;

	uclamp_fork(p);

	/*
	 * Revert to default priority/policy on fork if requested.
	 */
	if (unlikely(p->sched_reset_on_fork)) {
		...
	}

	if (dl_prio(p->prio))
		return -EAGAIN;

	scx_pre_fork(p);

	if (rt_prio(p->prio)) {
		p->sched_class = &rt_sched_class;
#ifdef CONFIG_SCHED_CLASS_EXT
	} else if (task_should_scx(p->policy)) {
		p->sched_class = &ext_sched_class;
#endif
	} else {
		p->sched_class = &fair_sched_class;
	}

	init_entity_runnable_average(&p->se);
```

- `__sched_fork()`（[`kernel/sched/core.c:4456-4508`](../../linux/kernel/sched/core.c#L4456-L4508)）把 `on_rq`、`se.vruntime`、`se.vlag`、`se.sum_exec_runtime` 等清零，初始化 RT / DL 实体。子进程从父进程 `dup_task_struct()` 复制来的调度状态在这里被重置。
- `TASK_NEW` 状态保证新任务在 `wake_up_new_task()` 之前不会被常规唤醒放入运行队列：信号、等待队列等使用的状态掩码（如 `TASK_NORMAL`）不包含 `TASK_NEW`，`try_to_wake_up()` 的状态匹配会失败。
- `p->prio` 取父进程的 `normal_prio` 而不是 `prio`，防止父进程因优先级继承获得的临时提升"遗传"给子进程。
- `SCHED_RESET_ON_FORK` 标志会把实时 / Deadline 子进程降为 `SCHED_NORMAL`，并把负 nice 恢复为 0。
- `SCHED_DEADLINE` 任务不允许 fork（返回 `-EAGAIN`）。源码未在此处注释原因；从设计上看，Deadline 带宽需要经过准入控制（3.7 节），子进程不能凭空复制一份父进程的带宽。
- `init_entity_runnable_average()` 初始化 PELT 负载（3.8 节）。

`sched_cgroup_fork()`（[`kernel/sched/core.c:4766-4795`](../../linux/kernel/sched/core.c#L4766-L4795)）在持有 `pi_lock` 的情况下确定任务所属的 `task_group`（含 autogroup），用 `__set_task_cpu()` 把初始 CPU 设为当前 CPU，并调用调度类的 `task_fork()` 回调。

### wake_up_new_task()

[`kernel/sched/core.c:4831-4867`](../../linux/kernel/sched/core.c#L4831-L4867)（v6.18.60）

```c
void wake_up_new_task(struct task_struct *p)
{
	struct rq_flags rf;
	struct rq *rq;
	int wake_flags = WF_FORK;

	raw_spin_lock_irqsave(&p->pi_lock, rf.flags);
	WRITE_ONCE(p->__state, TASK_RUNNING);
	/*
	 * Fork balancing, do it here and not earlier because:
	 *  - cpus_ptr can change in the fork path
	 *  - any previously selected CPU might disappear through hotplug
	 *
	 * Use __set_task_cpu() to avoid calling sched_class::migrate_task_rq,
	 * as we're not fully set-up yet.
	 */
	p->recent_used_cpu = task_cpu(p);
	rseq_migrate(p);
	__set_task_cpu(p, select_task_rq(p, task_cpu(p), &wake_flags));
	rq = __task_rq_lock(p, &rf);
	update_rq_clock(rq);
	post_init_entity_util_avg(p);

	activate_task(rq, p, ENQUEUE_NOCLOCK | ENQUEUE_INITIAL);
	trace_sched_wakeup_new(p);
	wakeup_preempt(rq, p, wake_flags);
	if (p->sched_class->task_woken) {
		/*
		 * Nothing relies on rq->lock after this, so it's fine to
		 * drop it.
		 */
		rq_unpin_lock(rq, &rf);
		p->sched_class->task_woken(rq, p);
		rq_repin_lock(rq, &rf);
	}
	task_rq_unlock(rq, p, &rf);
}
```

1. **fork 均衡**：以 `WF_FORK` 调用 `select_task_rq()`。`WF_FORK` 对应调度域标志 `SD_BALANCE_FORK`，公平类会走慢速路径，在调度域中寻找最空闲的 CPU（见下文），以便把新任务分散开。
2. `post_init_entity_util_avg()` 根据所在 CPU 的利用率为新任务估算一个初始 `util_avg`。
3. `activate_task(..., ENQUEUE_INITIAL)`：公平类的 `place_entity()` 会以 lag 0 放置新任务，并只给它半个时间片的截止时间（3.3 节）。
4. `wakeup_preempt()` 判断是否抢占当前任务。公平类对 `WF_FORK` 通常不做唤醒抢占（除非新任务的时间片比当前任务短，触发 `PREEMPT_SHORT`，见 3.3 节），但如果新任务属于更高的调度类（如实时任务 fork 出的实时子进程），仍会通过核心层的跨类比较抢占。

新任务被选中后的第一次运行，从 `ret_from_fork_asm` 开始（3.2 节）。

## try_to_wake_up() 主流程

### 调用链总览

```
wake_up_process(p)                     kernel/sched/core.c:4438
└── try_to_wake_up(p, TASK_NORMAL, 0)  kernel/sched/core.c:4159
    ├── [p == current] ttwu_do_wakeup()                  // 只改状态
    ├── raw_spin_lock(&p->pi_lock)
    ├── ttwu_state_match()                               // 状态不匹配则返回
    ├── [p->on_rq] ttwu_runnable()                       // 尚未出队：改状态即可
    ├── p->__state = TASK_WAKING
    ├── [p->on_cpu] ttwu_queue_wakelist()                // 仍在切换中：交给目标 CPU
    ├── smp_cond_load_acquire(&p->on_cpu, !VAL)          // 等待切换完成
    ├── select_task_rq()                                 // 选 CPU
    │   └── p->sched_class->select_task_rq()
    ├── set_task_cpu()                                   // 需要迁移时
    └── ttwu_queue()
        ├── ttwu_queue_wakelist() → IPI → sched_ttwu_pending()
        └── ttwu_do_activate()
            ├── activate_task(ENQUEUE_WAKEUP)
            ├── wakeup_preempt()
            └── ttwu_do_wakeup()                         // __state = TASK_RUNNING
```

等待队列的唤醒（`wake_up()` 系列）最终调用 `default_wake_function()`（[`kernel/sched/core.c:7273-7278`](../../linux/kernel/sched/core.c#L7273-L7278)），它同样进入 `try_to_wake_up()`。`TASK_NORMAL` 为 `TASK_INTERRUPTIBLE | TASK_UNINTERRUPTIBLE`（[`include/linux/sched.h:142`](../../linux/include/linux/sched.h#L142)）。

### 第一步：唤醒自己

[`kernel/sched/core.c:4159-4189`](../../linux/kernel/sched/core.c#L4159-L4189)（v6.18.60，节选）

```c
int try_to_wake_up(struct task_struct *p, unsigned int state, int wake_flags)
{
	guard(preempt)();
	int cpu, success = 0;

	wake_flags |= WF_TTWU;

	if (p == current) {
		...
		WARN_ON_ONCE(p->se.sched_delayed);
		if (!ttwu_state_match(p, state, &success))
			goto out;

		trace_sched_waking(p);
		ttwu_do_wakeup(p);
		goto out;
	}
```

一个任务在 `set_current_state()` 之后、`schedule()` 之前，可能在中断处理函数中被"唤醒"（例如中断唤醒了当前任务正在等待的事件）。此时任务必然还在运行队列上、就在本 CPU 上运行，只需把 `__state` 改回 `TASK_RUNNING`，不需要任何锁。

### 第二步：持 pi_lock 检查状态

[`kernel/sched/core.c:4197-4228`](../../linux/kernel/sched/core.c#L4197-L4228)（v6.18.60，节选）

```c
	scoped_guard (raw_spinlock_irqsave, &p->pi_lock) {
		smp_mb__after_spinlock();
		if (!ttwu_state_match(p, state, &success))
			break;

		trace_sched_waking(p);
		...
		smp_rmb();
		if (READ_ONCE(p->on_rq) && ttwu_runnable(p, wake_flags))
			break;
```

`p->pi_lock` 序列化对同一任务的并发唤醒，并稳定 `p->sched_class`、`p->cpus_ptr`、`p->sched_task_group`——这正是选择 CPU 和迁移所需要的（函数头注释 [`kernel/sched/core.c:4141-4144`](../../linux/kernel/sched/core.c#L4141-L4144)）。

`ttwu_state_match()`（[`kernel/sched/core.c:4008-4037`](../../linux/kernel/sched/core.c#L4008-L4037)）检查 `p->__state & state`。若状态不匹配（例如任务已是 `TASK_RUNNING`，或以 `TASK_UNINTERRUPTIBLE` 睡眠而调用者只唤醒 `TASK_INTERRUPTIBLE`），唤醒直接失败。它还处理 `saved_state`：在 PREEMPT_RT 的睡眠锁或冻结场景下，任务的"真实"睡眠状态被暂存在 `saved_state` 中，此时只把 `saved_state` 改为 `TASK_RUNNING`，不真正唤醒。

`smp_rmb()` 保证先读 `__state` 再读 `on_rq`，源码注释（[`kernel/sched/core.c:4205-4224`](../../linux/kernel/sched/core.c#L4205-L4224)）画出了不加屏障时可能出现的错误：误以为 `on_rq == 0`，随后在 `on_cpu` 上永远自旋。

### ttwu_runnable()：任务还没出队

[`kernel/sched/core.c:3775-3799`](../../linux/kernel/sched/core.c#L3775-L3799)（v6.18.60）

```c
static int ttwu_runnable(struct task_struct *p, int wake_flags)
{
	struct rq_flags rf;
	struct rq *rq;
	int ret = 0;

	rq = __task_rq_lock(p, &rf);
	if (task_on_rq_queued(p)) {
		update_rq_clock(rq);
		if (p->se.sched_delayed)
			enqueue_task(rq, p, ENQUEUE_NOCLOCK | ENQUEUE_DELAYED);
		if (!task_on_cpu(rq, p)) {
			/*
			 * When on_rq && !on_cpu the task is preempted, see if
			 * it should preempt the task that is current now.
			 */
			wakeup_preempt(rq, p, wake_flags);
		}
		ttwu_do_wakeup(p);
		ret = 1;
	}
	__task_rq_unlock(rq, &rf);

	return ret;
}
```

`on_rq` 非 0 说明任务还在运行队列上，有三种可能：

1. 任务正在运行，刚执行完 `set_current_state()`，还没进入 `schedule()`；
2. 任务在设置状态后、调用 `schedule()` 前被抢占（3.2 节："抢占时不出队"），现在正排队等待 CPU；
3. 任务已经"睡下"，但处于**延迟出队**状态（`sched_delayed`，3.3 节），名义上仍在队列中。

必须获取任务所在 rq 的锁，与 `__schedule()` 中的出队操作串行化。持锁后再次检查 `task_on_rq_queued()`：如果此时已被出队，返回 0，走完整唤醒路径；否则只需把状态改回 `TASK_RUNNING`。对于延迟出队的任务，以 `ENQUEUE_DELAYED` 调用 `enqueue_task()`，由公平类的 `requeue_delayed_entity()` 清除延迟标记。对于情形 2，还要检查它是否应抢占当前任务。

### 第三步：TASK_WAKING 与 on_cpu

[`kernel/sched/core.c:4253-4295`](../../linux/kernel/sched/core.c#L4253-L4295)（v6.18.60，节选）

```c
		smp_acquire__after_ctrl_dep();

		/*
		 * We're doing the wakeup (@success == 1), they did a dequeue (p->on_rq
		 * == 0), which means we need to do an enqueue, change p->state to
		 * TASK_WAKING such that we can unlock p->pi_lock before doing the
		 * enqueue, such as ttwu_queue_wakelist().
		 */
		WRITE_ONCE(p->__state, TASK_WAKING);
		...
		if (smp_load_acquire(&p->on_cpu) &&
		    ttwu_queue_wakelist(p, task_cpu(p), wake_flags))
			break;

		/*
		 * If the owning (remote) CPU is still in the middle of schedule() with
		 * this task as prev, wait until it's done referencing the task.
		 *
		 * Pairs with the smp_store_release() in finish_task().
		 *
		 * This ensures that tasks getting woken will be fully ordered against
		 * their previous state and preserve Program Order.
		 */
		smp_cond_load_acquire(&p->on_cpu, !VAL);
```

走到这里说明 `on_rq == 0`：任务确实已经出队。但"已出队"不等于"已切换完"——回顾 3.2 节，`prev` 在 `__schedule()` 早期就出队了，而 `on_cpu` 要到 `finish_task_switch()` 中才清零。这段窗口里任务的内核栈仍在使用，不能让它在别的 CPU 上运行。

- `TASK_WAKING` 是一个过渡状态，表示"唤醒已被认领，正在入队途中"。它让 `ttwu_queue_wakelist()` 可以在不持 `pi_lock` 的目标 CPU 上完成入队，也防止其他唤醒者重复处理。
- **若 `on_cpu` 仍为 1**：任务正在某个 CPU 上切换出去。与其原地自旋等待，不如把唤醒请求挂到**该 CPU** 的唤醒链表上（`ttwu_queue_wakelist()`），由它在切换完成、开中断后处理。这让唤醒者可以立即返回。
- 若无法使用唤醒链表，就用 `smp_cond_load_acquire()` 自旋等待 `on_cpu` 变为 0。它与 `finish_task()` 中的 `smp_store_release(&prev->on_cpu, 0)` 配对，保证任务在旧 CPU 上的所有操作先于它在新 CPU 上的执行——源码 [`kernel/sched/core.c:4040-4119`](../../linux/kernel/sched/core.c#L4040-L4119) 的长注释称之为程序序（Program-Order）保证。

### 第四步：选 CPU、迁移、入队

[`kernel/sched/core.c:4297-4316`](../../linux/kernel/sched/core.c#L4297-L4316)（v6.18.60）

```c
		cpu = select_task_rq(p, p->wake_cpu, &wake_flags);
		if (task_cpu(p) != cpu) {
			if (p->in_iowait) {
				delayacct_blkio_end(p);
				atomic_dec(&task_rq(p)->nr_iowait);
			}

			wake_flags |= WF_MIGRATED;
			psi_ttwu_dequeue(p);
			set_task_cpu(p, cpu);
		}

		ttwu_queue(p, cpu, wake_flags);
	}
out:
	if (success)
		ttwu_stat(p, task_cpu(p), wake_flags);

	return success;
}
```

睡眠任务不属于任何运行队列，所以可以在**不持任何 rq 锁**的情况下为它挑选新 CPU 并修改 `task_cpu(p)`——这是唤醒时迁移任务代价最低的时机。`set_task_cpu()`（[`kernel/sched/core.c:3302-3354`](../../linux/kernel/sched/core.c#L3302-L3354)）在 CPU 改变时调用调度类的 `migrate_task_rq()`（公平类借此把 PELT 负载从旧 CPU 上摘除），并累加 `se.nr_migrations`。

`select_task_rq()` 是核心层的包装：

[`kernel/sched/core.c:3582-3608`](../../linux/kernel/sched/core.c#L3582-L3608)（v6.18.60）

```c
static inline
int select_task_rq(struct task_struct *p, int cpu, int *wake_flags)
{
	lockdep_assert_held(&p->pi_lock);

	if (p->nr_cpus_allowed > 1 && !is_migration_disabled(p)) {
		cpu = p->sched_class->select_task_rq(p, cpu, *wake_flags);
		*wake_flags |= WF_RQ_SELECTED;
	} else {
		cpu = cpumask_any(p->cpus_ptr);
	}

	/*
	 * In order not to call set_task_cpu() on a blocking task we need
	 * to rely on ttwu() to place the task on a valid ->cpus_ptr
	 * CPU.
	 *
	 * Since this is common to all placement strategies, this lives here.
	 *
	 * [ this allows ->select_task() to simply return task_cpu(p) and
	 *   not worry about this generic constraint ]
	 */
	if (unlikely(!is_cpu_allowed(p, cpu)))
		cpu = select_fallback_rq(task_cpu(p), p);

	return cpu;
}
```

只能运行在一个 CPU 上的任务（绑核任务、per-CPU 内核线程）不调用调度类的选核逻辑。若选出的 CPU 已不可用（例如被热拔），`select_fallback_rq()` 会在允许的 CPU 中另选一个，必要时放宽亲和性。

## 远程唤醒队列

### 为什么需要唤醒队列

最直接的远程唤醒方式是：唤醒者获取**目标 CPU** 的 rq 锁，把任务插入目标运行队列。在大型多核机器上，这意味着 rq 锁所在的缓存行要在唤醒者与目标 CPU 之间来回传递；如果两个 CPU 不共享末级缓存（Last Level Cache，LLC），代价尤其高。

`TTWU_QUEUE` 特性（[`kernel/sched/features.h:82`](../../linux/kernel/sched/features.h#L82)，非 PREEMPT_RT 时默认开启）提供了另一种方式：唤醒者只把任务挂到目标 CPU 的无锁链表——唤醒队列（Wake List）——上，再发送一个 IPI，由目标 CPU 自己完成入队。这样 rq 锁和运行队列数据始终只被本地 CPU 访问。

### 何时使用

[`kernel/sched/core.c:3915-3962`](../../linux/kernel/sched/core.c#L3915-L3962)（v6.18.60，节选）

```c
static inline bool ttwu_queue_cond(struct task_struct *p, int cpu)
{
	...
	/*
	 * Do not complicate things with the async wake_list while the CPU is
	 * in hotplug state.
	 */
	if (!cpu_active(cpu))
		return false;

	/* Ensure the task will still be allowed to run on the CPU. */
	if (!cpumask_test_cpu(cpu, p->cpus_ptr))
		return false;

	/*
	 * If the CPU does not share cache, then queue the task on the
	 * remote rqs wakelist to avoid accessing remote data.
	 */
	if (!cpus_share_cache(smp_processor_id(), cpu))
		return true;

	if (cpu == smp_processor_id())
		return false;
	...
	if (!cpu_rq(cpu)->nr_running)
		return true;

	return false;
}
```

满足以下任一条件时使用唤醒队列：

- 唤醒者与目标 CPU **不共享 LLC**：避免跨缓存域访问远程 rq；
- 目标 CPU **空闲**（`nr_running == 0`）：把入队工作卸载给反正无事可做的目标 CPU，唤醒者可以更快返回。

前文"`on_cpu` 仍为 1"的情况也调用 `ttwu_queue_wakelist(p, task_cpu(p), ...)`，此时目标是任务**当前所在**的 CPU：那个 CPU 关着中断执行 `__schedule()`，IPI 一定会在 `on_cpu` 清零后才被处理，所以无需等待。

### 入队与处理

[`kernel/sched/core.c:3860-3870`](../../linux/kernel/sched/core.c#L3860-L3870)（v6.18.60）

```c
static void __ttwu_queue_wakelist(struct task_struct *p, int cpu, int wake_flags)
{
	struct rq *rq = cpu_rq(cpu);

	p->sched_remote_wakeup = !!(wake_flags & WF_MIGRATED);

	WRITE_ONCE(rq->ttwu_pending, 1);
#ifdef CONFIG_SMP
	__smp_call_single_queue(cpu, &p->wake_entry.llist);
#endif
}
```

`p->wake_entry` 是嵌入 `task_struct` 的 CSD 节点（3.1 节），类型在 `__sched_fork()` 中被设为 `CSD_TYPE_TTWU`（[`kernel/sched/core.c:4505`](../../linux/kernel/sched/core.c#L4505)）。`__smp_call_single_queue()` 把它挂到目标 CPU 的 `call_single_queue` 上，必要时发送 IPI；若目标 CPU 正在 idle 中轮询，`call_function_single_prep_ipi()`（[`kernel/sched/core.c:3844-3852`](../../linux/kernel/sched/core.c#L3844-L3852)）只设置 `TIF_NEED_RESCHED` 而省去 IPI。`rq->ttwu_pending` 让 `idle_cpu()` 等判断知道"这个 CPU 马上就有任务了"。

目标 CPU 在处理 call-function IPI 时，识别出 `CSD_TYPE_TTWU` 类型的节点，调用 `sched_ttwu_pending()`（[`kernel/smp.c:394-395`](../../linux/kernel/smp.c#L394-L395)）：

[`kernel/sched/core.c:3801-3836`](../../linux/kernel/sched/core.c#L3801-L3836)（v6.18.60，节选）

```c
void sched_ttwu_pending(void *arg)
{
	struct llist_node *llist = arg;
	struct rq *rq = this_rq();
	struct task_struct *p, *t;
	struct rq_flags rf;

	if (!llist)
		return;

	rq_lock_irqsave(rq, &rf);
	update_rq_clock(rq);

	llist_for_each_entry_safe(p, t, llist, wake_entry.llist) {
		if (WARN_ON_ONCE(p->on_cpu))
			smp_cond_load_acquire(&p->on_cpu, !VAL);

		if (WARN_ON_ONCE(task_cpu(p) != cpu_of(rq)))
			set_task_cpu(p, cpu_of(rq));

		ttwu_do_activate(rq, p, p->sched_remote_wakeup ? WF_MIGRATED : 0, &rf);
	}
	...
	WRITE_ONCE(rq->ttwu_pending, 0);
	rq_unlock_irqrestore(rq, &rf);
}
```

一批唤醒只需获取一次本地 rq 锁。

### 不走唤醒队列：ttwu_queue()

[`kernel/sched/core.c:3975-3987`](../../linux/kernel/sched/core.c#L3975-L3987)（v6.18.60）

```c
static void ttwu_queue(struct task_struct *p, int cpu, int wake_flags)
{
	struct rq *rq = cpu_rq(cpu);
	struct rq_flags rf;

	if (ttwu_queue_wakelist(p, cpu, wake_flags))
		return;

	rq_lock(rq, &rf);
	update_rq_clock(rq);
	ttwu_do_activate(rq, p, wake_flags, &rf);
	rq_unlock(rq, &rf);
}
```

本地唤醒、或目标 CPU 共享 LLC 且正忙时，唤醒者直接获取目标 rq 锁完成入队。

## ttwu_do_activate()：真正的入队

[`kernel/sched/core.c:3701-3748`](../../linux/kernel/sched/core.c#L3701-L3748)（v6.18.60，节选）

```c
static void
ttwu_do_activate(struct rq *rq, struct task_struct *p, int wake_flags,
		 struct rq_flags *rf)
{
	int en_flags = ENQUEUE_WAKEUP | ENQUEUE_NOCLOCK;

	lockdep_assert_rq_held(rq);

	if (p->sched_contributes_to_load)
		rq->nr_uninterruptible--;

	if (wake_flags & WF_RQ_SELECTED)
		en_flags |= ENQUEUE_RQ_SELECTED;
	if (wake_flags & WF_MIGRATED)
		en_flags |= ENQUEUE_MIGRATED;
	else
	if (p->in_iowait) {
		delayacct_blkio_end(p);
		atomic_dec(&task_rq(p)->nr_iowait);
	}

	activate_task(rq, p, en_flags);
	wakeup_preempt(rq, p, wake_flags);

	ttwu_do_wakeup(p);

	if (p->sched_class->task_woken) {
		...
		p->sched_class->task_woken(rq, p);
		...
	}

	if (rq->idle_stamp) {
		u64 delta = rq_clock(rq) - rq->idle_stamp;
		u64 max = 2*rq->max_idle_balance_cost;

		update_avg(&rq->avg_idle, delta);
		...
		rq->idle_stamp = 0;
	}
}
```

1. 撤销睡眠时 `__block_task()` 做的统计：`nr_uninterruptible` 与 `nr_iowait`。注意 `nr_uninterruptible` 是"全局之和才有意义"的计数（[`kernel/sched/sched.h:1162-1167`](../../linux/kernel/sched/sched.h#L1162-L1167) 的字段注释），在 A CPU 上加、B CPU 上减是正常的。
2. `activate_task(ENQUEUE_WAKEUP)`：调用调度类的 `enqueue_task()` 并置 `on_rq = TASK_ON_RQ_QUEUED`。公平类会在 `place_entity()` 中恢复任务睡眠前的 lag。
3. `wakeup_preempt()`：判断被唤醒者是否应抢占当前任务，必要时 `resched_curr()`（3.1、3.3 节）。
4. `ttwu_do_wakeup()`：把 `__state` 置为 `TASK_RUNNING`，并触发 `sched_wakeup` 跟踪点。**状态最后才改**，在此之前 `TASK_WAKING` 一直标示"唤醒进行中"。
5. `task_woken()`：RT / DL 类借此检查是否需要把任务推（push）到其他 CPU。
6. 如果该 CPU 刚从 idle 中醒来，用这段空闲时长更新 `avg_idle`，供 newidle 负载均衡判断"值不值得去拉任务"（3.9 节）。

## 公平任务的选核：select_task_rq_fair()

公平类的选核要在两个目标之间权衡：**缓存亲和**（放在上次运行的 CPU 或唤醒者的 CPU 上，数据可能还在缓存中）与**负载分散**（找一个空闲 CPU 立即运行）。

[`kernel/sched/fair.c:8740-8808`](../../linux/kernel/sched/fair.c#L8740-L8808)（v6.18.60，节选）

```c
static int
select_task_rq_fair(struct task_struct *p, int prev_cpu, int wake_flags)
{
	int sync = (wake_flags & WF_SYNC) && !(current->flags & PF_EXITING);
	struct sched_domain *tmp, *sd = NULL;
	int cpu = smp_processor_id();
	int new_cpu = prev_cpu;
	int want_affine = 0;
	/* SD_flags and WF_flags share the first nibble */
	int sd_flag = wake_flags & 0xF;
	...
	if (wake_flags & WF_TTWU) {
		record_wakee(p);
		...
		if (!is_rd_overutilized(this_rq()->rd)) {
			new_cpu = find_energy_efficient_cpu(p, prev_cpu);
			if (new_cpu >= 0)
				return new_cpu;
			new_cpu = prev_cpu;
		}

		want_affine = !wake_wide(p) && cpumask_test_cpu(cpu, p->cpus_ptr);
	}

	rcu_read_lock();
	for_each_domain(cpu, tmp) {
		/*
		 * If both 'cpu' and 'prev_cpu' are part of this domain,
		 * cpu is a valid SD_WAKE_AFFINE target.
		 */
		if (want_affine && (tmp->flags & SD_WAKE_AFFINE) &&
		    cpumask_test_cpu(prev_cpu, sched_domain_span(tmp))) {
			if (cpu != prev_cpu)
				new_cpu = wake_affine(tmp, p, cpu, prev_cpu, sync);

			sd = NULL; /* Prefer wake_affine over balance flags */
			break;
		}
		...
		if (tmp->flags & sd_flag)
			sd = tmp;
		else if (!want_affine)
			break;
	}

	if (unlikely(sd)) {
		/* Slow path */
		new_cpu = sched_balance_find_dst_cpu(sd, p, cpu, prev_cpu, sd_flag);
	} else if (wake_flags & WF_TTWU) { /* XXX always ? */
		/* Fast path */
		new_cpu = select_idle_sibling(p, prev_cpu, new_cpu);
	}
	rcu_read_unlock();

	return new_cpu;
}
```

`WF_EXEC` / `WF_FORK` / `WF_TTWU` 的取值与调度域标志 `SD_BALANCE_EXEC` / `SD_BALANCE_FORK` / `SD_BALANCE_WAKE` 一一对应（[`kernel/sched/sched.h:2328-2330`](../../linux/kernel/sched/sched.h#L2328-L2330)），所以 `wake_flags & 0xF` 可以直接当作调度域标志使用。整个流程分三步：

1. **能效感知调度**（Energy Aware Scheduling，EAS）：在非对称算力平台（如大小核）上且系统未过载时，`find_energy_efficient_cpu()` 选能耗最低的 CPU。对称的 x86_64 服务器上 EAS 通常未启用，此时 `is_rd_overutilized()` 恒返回真（它等价于 `!sched_energy_enabled() || rd->overutilized`），这一步被直接跳过。
2. **是否亲和唤醒**：`want_affine` 为真时，在同时包含唤醒者 CPU 与 `prev_cpu` 的最小调度域内，由 `wake_affine()` 在两者之间二选一。
3. **快速路径与慢速路径**：
   - **慢速路径**：普通唤醒时调度域一般不设 `SD_BALANCE_WAKE`，所以只有 fork / exec 会进入慢速路径。`sched_balance_find_dst_cpu()` 自上而下在调度域中找最空闲的组、再找组内最空闲的 CPU，开销较大，但能把新任务均匀铺开。
   - **快速路径**：`select_idle_sibling()` 在目标 CPU 所在的 LLC 域内寻找空闲 CPU。

### wake_wide()：一对多的唤醒关系

[`kernel/sched/fair.c:7472-7483`](../../linux/kernel/sched/fair.c#L7472-L7483)（v6.18.60）

```c
static int wake_wide(struct task_struct *p)
{
	unsigned int master = current->wakee_flips;
	unsigned int slave = p->wakee_flips;
	int factor = __this_cpu_read(sd_llc_size);

	if (master < slave)
		swap(master, slave);
	if (slave < factor || master < slave * factor)
		return 0;
	return 1;
}
```

`record_wakee()`（[`kernel/sched/fair.c:7438-7453`](../../linux/kernel/sched/fair.c#L7438-L7453)）在唤醒者每次换一个被唤醒对象时递增 `wakee_flips`，并每秒衰减一半。如果唤醒者频繁地唤醒许多不同任务（例如一个分发线程 + 多个工作线程），且这种"扇出"超过 LLC 的 CPU 数，就判定为 wake wide：不再把被唤醒者拉到唤醒者身边，以免所有工作线程挤在一个缓存域里。

### wake_affine()：唤醒者 CPU 还是原 CPU

`wake_affine()`（[`kernel/sched/fair.c:7571-7589`](../../linux/kernel/sched/fair.c#L7571-L7589)）依次尝试两个启发式：

- `wake_affine_idle()`（[`kernel/sched/fair.c:7497-7526`](../../linux/kernel/sched/fair.c#L7497-L7526)）只看"当下"：唤醒者 CPU 空闲且与 `prev_cpu` 共享缓存时，若 `prev_cpu` 也空闲则选 `prev_cpu`（避免迁移），否则选唤醒者 CPU；同步唤醒（`WF_SYNC`，唤醒者马上要睡）且唤醒者 CPU 上只有唤醒者一个任务时，选唤醒者 CPU；`prev_cpu` 空闲则选 `prev_cpu`。
- `wake_affine_weight()`（[`kernel/sched/fair.c:7528-7569`](../../linux/kernel/sched/fair.c#L7528-L7569)）比较两个 CPU 按算力折算后的负载（把 `p` 的负载计入唤醒者 CPU、从 `prev_cpu` 中扣除），只有唤醒者 CPU 更轻时才选它。`WA_BIAS` 把 `prev_cpu` 的负载乘以 `100 + (imbalance_pct - 100) / 2`、唤醒者 CPU 只乘以 100，使 `prev_cpu` 显得略重，从而略偏向亲和唤醒。

返回值只是"起点"，随后 `select_idle_sibling()` 还会在这个 CPU 附近寻找真正空闲的 CPU。

### select_idle_sibling()：在 LLC 内找空闲 CPU

`select_idle_sibling()`（[`kernel/sched/fair.c:7988`](../../linux/kernel/sched/fair.c#L7988) 起）按代价由低到高依次尝试：

1. `target` 本身空闲（或只运行 `SCHED_IDLE` 任务）→ 选它；
2. `prev_cpu` 与 `target` 共享缓存且空闲 → 选 `prev_cpu`；
3. 唤醒者是 per-CPU 内核线程且就在 `prev_cpu` 上（典型如 I/O 完成）→ 叠放在 `prev_cpu`；
4. `p->recent_used_cpu` 共享缓存且空闲 → 选它；
5. 开启 SMT 时，先在 `prev_cpu` 的 SMT 兄弟中找（`select_idle_smt()`），再在整个 LLC 内扫描空闲核或空闲 CPU（`select_idle_cpu()`），扫描深度受 `SIS_UTIL` 按 LLC 利用率限制；
6. 都失败则返回 `target`。

调度域与 LLC 的构建、负载的精确定义留到 3.8、3.9 节。

## 关键细节 / 设计考量

### 四个状态变量

唤醒路径的复杂性来自四个变量的组合，下表总结了 `try_to_wake_up()` 看到的各种情形：

| `__state` | `on_rq` | `on_cpu` | 含义 | ttwu 的处理 |
|---|---|---|---|---|
| 不匹配 `state` | — | — | 已在运行，或睡眠类型不符 | 直接返回失败 |
| 睡眠态 | 1 | 1 | 设好状态，尚未调用 / 正在执行 `schedule()` | `ttwu_runnable()`：改回 `RUNNING` |
| 睡眠态 | 1 | 0 | 设好状态后被抢占；或延迟出队中 | `ttwu_runnable()`：改回 `RUNNING`，可能抢占当前任务 |
| 睡眠态 | 0 | 1 | 已出队，切换尚未完成 | `TASK_WAKING`，交给原 CPU 唤醒队列或等待 `on_cpu` 清零 |
| 睡眠态 | 0 | 0 | 完全睡下 | `TASK_WAKING`，选 CPU、入队 |

### 为什么唤醒时选核

唤醒是任务迁移代价最低的时刻：任务不在任何运行队列上，`set_task_cpu()` 无需持有新旧两个 rq 锁。相比之下，负载均衡迁移一个正在排队的任务需要同时锁住两个 rq（3.9 节）。因此 Linux 把大量"放置"决策放在唤醒路径上，周期性负载均衡只负责纠正长期失衡。

### WF_SYNC

`wake_up_sync()` 系列（如管道、Unix 域套接字在写入后唤醒读者）会传入 `WF_SYNC`，提示"唤醒者马上要睡"。公平类借此倾向于把被唤醒者放在唤醒者 CPU 上（`wake_affine_idle()`、`wake_affine_weight()`），以利用刚写入的缓存数据。在唤醒抢占方面，只有开启 `NEXT_BUDDY` 特性（默认关闭）时才会经 `preempt_sync()` 参考 `WF_SYNC`，而且要求被唤醒者的截止时间更早、当前任务已运行超过 `sysctl_sched_migration_cost`（[`kernel/sched/fair.c:8940-8973`](../../linux/kernel/sched/fair.c#L8940-L8973)）。

### 唤醒延迟的构成

从 `sched_waking` 到 `sched_switch` 切入被唤醒者，延迟由三部分组成：唤醒路径本身（含可能的 IPI）、在运行队列中等待被选中的时间（取决于 EEVDF 的截止时间与抢占决策），以及目标 CPU 从 idle 深睡眠状态退出的时间。下面的实验分别观察它们。

## 动手实验

以下实验在 **dev 虚拟机** 中进行，需要 **root 权限**。

1. 用 bpftrace 统计唤醒延迟（从 `sched_wakeup` 到被唤醒任务真正上 CPU）的分布：

   ```bash
   # 虚拟机，root
   bpftrace -e '
   tracepoint:sched:sched_wakeup,
   tracepoint:sched:sched_wakeup_new { @ts[args.pid] = nsecs; }
   tracepoint:sched:sched_switch /@ts[args.next_pid]/ {
       @lat_us = hist((nsecs - @ts[args.next_pid]) / 1000);
       delete(@ts[args.next_pid]);
   }'
   ```

2. 观察远程唤醒是否使用了唤醒队列：`sched_wake_idle_without_ipi` 跟踪点在目标 CPU 正在 idle 轮询、省去 IPI 时触发：

   ```bash
   # 虚拟机，root
   perf stat -e 'sched:sched_waking,sched:sched_wakeup,sched:sched_wake_idle_without_ipi,irq_vectors:call_function_single_entry' -a sleep 5
   ```

3. 打开 schedstats，查看单个任务的唤醒统计（`nr_wakeups_local` / `_remote` / `_affine` / `_migrate` 等，[`kernel/sched/debug.c:1196-1204`](../../linux/kernel/sched/debug.c#L1196-L1204)）：

   ```bash
   # 虚拟机，root
   sysctl kernel.sched_schedstats=1
   grep nr_wakeups /proc/$(pgrep -o sshd)/sched
   sysctl kernel.sched_schedstats=0
   ```

4. 对比 fork 均衡：连续启动多个短命 CPU 密集任务，观察它们被放到了哪些 CPU：

   ```bash
   # 虚拟机，普通用户
   for i in $(seq 4); do (timeout 2 sh -c 'while :; do :; done' &); done
   sleep 1; ps -eLo pid,psr,comm | grep -w sh
   ```

## 小结

- 新任务经 `sched_fork()` 以 `TASK_NEW` 状态初始化调度字段（清零 `vruntime` / `vlag`，按 `prio` 选调度类，不继承 PI 提升），由 `wake_up_new_task()` 以 `WF_FORK` 选核、以 `ENQUEUE_INITIAL` 首次入队。
- 睡眠/唤醒协议依靠 `set_current_state()` 中的 `smp_store_mb()` 与 `try_to_wake_up()` 中的 `smp_mb__after_spinlock()` 配对，避免丢失唤醒。
- `try_to_wake_up()` 以 `p->pi_lock` 串行化唤醒，按 `on_rq` / `on_cpu` 区分情形：仍在队列上则由 `ttwu_runnable()` 只改状态；已出队则置 `TASK_WAKING`，等待（或委托）切换完成后选核、入队。
- 唤醒是迁移代价最低的时机：任务不在任何运行队列上，`select_task_rq()` + `set_task_cpu()` 无需持有 rq 锁。
- `TTWU_QUEUE` 唤醒队列把跨 LLC 或面向空闲 CPU 的唤醒委托给目标 CPU，通过 IPI 在 `sched_ttwu_pending()` 中批量入队，减少 rq 锁与运行队列数据的跨核传递。
- `ttwu_do_activate()` 依次完成入队、唤醒抢占判断，最后才把 `__state` 置为 `TASK_RUNNING`。
- 公平类的 `select_task_rq_fair()` 在缓存亲和与负载分散之间权衡：fork / exec 走调度域慢速路径找最空闲 CPU；普通唤醒经 `wake_wide()` / `wake_affine()` 确定起点，再由 `select_idle_sibling()` 在 LLC 内找空闲 CPU。

## 参考

- [`kernel/sched/core.c:4040-4119`](../../linux/kernel/sched/core.c#L4040-L4119)：唤醒路径上程序序保证的源码注释。
- `linux/Documentation/memory-barriers.txt`："SLEEP AND WAKE-UP FUNCTIONS" 一节。
- `linux/Documentation/scheduler/sched-domains.rst`：调度域与 `SD_BALANCE_*` / `SD_WAKE_AFFINE` 标志。
- `linux/Documentation/scheduler/sched-stats.rst`：schedstats 各字段含义。
