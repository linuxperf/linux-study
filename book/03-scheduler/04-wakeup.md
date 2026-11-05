# 3.4 任务创建与唤醒

> **本节源码**：[`kernel/sched/core.c`](../../linux/kernel/sched/core.c)、[`kernel/sched/sched.h`](../../linux/kernel/sched/sched.h)、[`kernel/sched/wait.c`](../../linux/kernel/sched/wait.c)、[`kernel/smp.c`](../../linux/kernel/smp.c)、[`kernel/fork.c`](../../linux/kernel/fork.c)、[`include/linux/sched.h`](../../linux/include/linux/sched.h)、[`include/linux/sched/wake_q.h`](../../linux/include/linux/sched/wake_q.h)
> **关键函数**：`sched_fork()`、`sched_cgroup_fork()`、`wake_up_new_task()`、`sched_exec()`、`try_to_wake_up()`、`ttwu_state_match()`、`ttwu_runnable()`、`select_task_rq()`、`set_task_cpu()`、`ttwu_queue_wakelist()`、`sched_ttwu_pending()`、`ttwu_do_activate()`
> **前置知识**：3.1 调度器框架（调度类接口、`pi_lock` 与 rq 锁、`on_rq` / `on_cpu`）、3.2 主调度函数（`try_to_block_task()`、`finish_task()`）

## 概述

3.2 节分析了任务如何离开 CPU：被抢占时仍留在运行队列上，主动睡眠时由 `try_to_block_task()` 移出运行队列。本节分析相反的方向，即任务如何**进入**运行队列：新建任务的第一次入队，以及睡眠任务被唤醒。

这两条路径的主体都在核心层 `kernel/sched/core.c` 中，与具体的调度策略无关。核心层负责维护睡眠 / 唤醒协议、处理唤醒者与被唤醒者之间的并发、规定"何时允许改变任务所在的 CPU"，并把入队工作交给合适的 CPU 执行。调度类只通过一组回调参与：`task_fork()`、`select_task_rq()`、`migrate_task_rq()`、`enqueue_task()`、`wakeup_preempt()`、`task_woken()`。回调内部如何选核、如何放置任务、是否抢占，属于各调度类自己的策略，本节不展开。

唤醒是调度器中并发最激烈的路径：被唤醒者可能还没睡下、正在睡下，或者刚在另一个 CPU 上被切换出去。`try_to_wake_up()` 依靠 `p->pi_lock`、`p->__state`、`p->on_rq`、`p->on_cpu` 四样东西和一组配对的内存屏障，在尽量只获取一把 rq 锁的前提下处理所有这些情形。

## 核心数据结构

### 任务状态

[`include/linux/sched.h:104-142`](../../linux/include/linux/sched.h#L104-L142)（v6.18.60，节选）

```c
/* Used in tsk->__state: */
#define TASK_RUNNING			0x00000000
#define TASK_INTERRUPTIBLE		0x00000001
#define TASK_UNINTERRUPTIBLE		0x00000002
...
#define TASK_WAKEKILL			0x00000100
#define TASK_WAKING			0x00000200
#define TASK_NOLOAD			0x00000400
#define TASK_NEW			0x00000800
...
/* Convenience macros for the sake of set_current_state: */
#define TASK_KILLABLE			(TASK_WAKEKILL | TASK_UNINTERRUPTIBLE)
...
#define TASK_IDLE			(TASK_UNINTERRUPTIBLE | TASK_NOLOAD)

/* Convenience macros for the sake of wake_up(): */
#define TASK_NORMAL			(TASK_INTERRUPTIBLE | TASK_UNINTERRUPTIBLE)
```

`__state` 是一个位掩码。`TASK_RUNNING`（0）表示"可运行"，并不意味着正在运行；其他值表示任务想要睡眠或处于某种特殊状态。与本节相关的状态如下：

| 状态 | 设置者 | 含义 |
|---|---|---|
| `TASK_INTERRUPTIBLE` | 等待方 | 可被信号唤醒的睡眠 |
| `TASK_UNINTERRUPTIBLE` | 等待方 | 不响应信号的睡眠，计入 `nr_uninterruptible`（带 `TASK_NOLOAD` 时除外） |
| `TASK_KILLABLE` | 等待方 | 不可中断，但能被致命信号唤醒 |
| `TASK_WAKING` | `try_to_wake_up()` | 唤醒已被认领、任务正在入队途中的过渡状态 |
| `TASK_NEW` | `sched_fork()` | 新任务第一次入队之前的状态，常规唤醒无法匹配 |

唤醒本质上是一次**状态匹配**：`try_to_wake_up(p, state, wake_flags)` 的 `state` 参数是"允许唤醒的状态集合"，只有 `p->__state & state` 非零才真正唤醒。`wake_up_process()` 传入 `TASK_NORMAL`，`wake_up_interruptible()` 只传入 `TASK_INTERRUPTIBLE`；信号投递传入 `TASK_INTERRUPTIBLE`，致命信号还会加上 `TASK_WAKEKILL` 等位（[`kernel/signal.c:745-760`](../../linux/kernel/signal.c#L745-L760)、[`include/linux/sched/signal.h:466-474`](../../linux/include/linux/sched/signal.h#L466-L474)）。所以 `TASK_UNINTERRUPTIBLE` 睡眠不会被普通信号打断，而 `TASK_KILLABLE` 睡眠能被 `SIGKILL` 唤醒——它的状态中含有 `TASK_WAKEKILL` 位。

### task_struct 中与唤醒相关的字段

| 字段 | 位置 | 作用 |
|---|---|---|
| `__state` | [`include/linux/sched.h:823`](../../linux/include/linux/sched.h#L823) | 任务状态，唤醒时与 `state` 掩码匹配 |
| `saved_state` | [`include/linux/sched.h:826`](../../linux/include/linux/sched.h#L826) | 阻塞在 PREEMPT_RT 睡眠锁上或被冻结期间暂存的原状态 |
| `on_cpu` | [`include/linux/sched.h:844`](../../linux/include/linux/sched.h#L844) | 任务正占用某个 CPU，包括正在被切换出去的过程 |
| `wake_entry` | [`include/linux/sched.h:845`](../../linux/include/linux/sched.h#L845) | 经唤醒队列投递时挂入目标 CPU 链表的节点 |
| `recent_used_cpu` | [`include/linux/sched.h:857`](../../linux/include/linux/sched.h#L857) | 最近使用过的 CPU，供调度类选核时参考 |
| `wake_cpu` | [`include/linux/sched.h:858`](../../linux/include/linux/sched.h#L858) | 唤醒选核的起点。`__set_task_cpu()` 总是把它与 `task_cpu(p)` 同步；NUMA 均衡交换两个任务时会单独修改它（[`kernel/sched/core.c:3376-3383`](../../linux/kernel/sched/core.c#L3376-L3383)，依赖 `CONFIG_NUMA_BALANCING`） |
| `on_rq` | [`include/linux/sched.h:859`](../../linux/include/linux/sched.h#L859) | 是否在运行队列上（3.1 节） |
| `nr_cpus_allowed` / `cpus_ptr` | [`include/linux/sched.h:917-918`](../../linux/include/linux/sched.h#L917-L918) | 允许运行的 CPU 数与 CPU 掩码 |
| `sched_contributes_to_load` | [`include/linux/sched.h:974`](../../linux/include/linux/sched.h#L974) | 本次睡眠是否计入 `rq->nr_uninterruptible`（3.2 节） |
| `sched_remote_wakeup` | [`include/linux/sched.h:996`](../../linux/include/linux/sched.h#L996) | 经唤醒队列投递时，记录唤醒过程中是否换了 CPU |
| `in_iowait` | [`include/linux/sched.h:1006`](../../linux/include/linux/sched.h#L1006) | 因等待 I/O 而睡眠（`io_schedule()`），计入 `rq->nr_iowait` |
| `pi_lock` | [`include/linux/sched.h:1229`](../../linux/include/linux/sched.h#L1229) | 串行化对该任务的并发唤醒，并稳定选核所需的字段 |
| `wake_q` | [`include/linux/sched.h:1231`](../../linux/include/linux/sched.h#L1231) | `wake_q` 链表节点（见下文"唤醒的入口"） |

### 运行队列中与唤醒相关的字段

| 字段 | 位置 | 作用 |
|---|---|---|
| `ttwu_pending` | [`kernel/sched/sched.h:1137`](../../linux/kernel/sched/sched.h#L1137) | 已有任务投递到本 CPU 的唤醒队列、但尚未入队 |
| `nr_uninterruptible` | [`kernel/sched/sched.h:1168`](../../linux/kernel/sched/sched.h#L1168) | 不可中断睡眠的任务数。字段注释（[`kernel/sched/sched.h:1162-1167`](../../linux/kernel/sched/sched.h#L1162-L1167)）说明只有各 CPU 之和有意义：任务可以在一个 CPU 上加、迁移后在另一个 CPU 上减 |
| `nr_iowait` | [`kernel/sched/sched.h:1198`](../../linux/kernel/sched/sched.h#L1198) | 在本 CPU 上因 I/O 睡眠的任务数 |
| `idle_stamp` / `avg_idle` | [`kernel/sched/sched.h:1238-1239`](../../linux/kernel/sched/sched.h#L1238-L1239) | CPU 进入空闲的时刻 / 平均空闲时长，供 newidle 负载均衡参考（3.9 节） |
| `ttwu_count` / `ttwu_local` | [`kernel/sched/sched.h:1282-1283`](../../linux/kernel/sched/sched.h#L1282-L1283) | 本 CPU 发起的唤醒次数 / 其中被唤醒者落在本 CPU 上的次数（依赖 `CONFIG_SCHEDSTATS`） |

### 唤醒标志

[`kernel/sched/sched.h:2327-2339`](../../linux/kernel/sched/sched.h#L2327-L2339)（v6.18.60）

```c
/* Wake flags. The first three directly map to some SD flag value */
#define WF_EXEC			0x02 /* Wakeup after exec; maps to SD_BALANCE_EXEC */
#define WF_FORK			0x04 /* Wakeup after fork; maps to SD_BALANCE_FORK */
#define WF_TTWU			0x08 /* Wakeup;            maps to SD_BALANCE_WAKE */

#define WF_SYNC			0x10 /* Waker goes to sleep after wakeup */
#define WF_MIGRATED		0x20 /* Internal use, task got migrated */
#define WF_CURRENT_CPU		0x40 /* Prefer to move the wakee to the current CPU. */
#define WF_RQ_SELECTED		0x80 /* ->select_task_rq() was called */

static_assert(WF_EXEC == SD_BALANCE_EXEC);
static_assert(WF_FORK == SD_BALANCE_FORK);
static_assert(WF_TTWU == SD_BALANCE_WAKE);
```

- `WF_EXEC`、`WF_FORK`、`WF_TTWU` 标明放置发生的时机：`execve()`、fork 后首次入队、唤醒。它们与调度域标志 `SD_BALANCE_*` 取值相同，并由 `static_assert` 保证，调度类可以直接拿它们与调度域标志比较（调度域见 3.9 节）。`try_to_wake_up()` 一进入就加上 `WF_TTWU`。
- `WF_SYNC`：唤醒者提示"我马上就要睡眠"。`__wake_up_sync_key()`（[`kernel/sched/wait.c:187-194`](../../linux/kernel/sched/wait.c#L187-L194)）传入它，管道读写、子进程退出时唤醒父进程（[`kernel/exit.c:1625-1629`](../../linux/kernel/exit.c#L1625-L1629)）等都走这条路径。
- `WF_CURRENT_CPU`：唤醒者希望被唤醒者到自己所在的 CPU 上运行，由 `__wake_up_on_current_cpu()`（[`kernel/sched/wait.c:151-154`](../../linux/kernel/sched/wait.c#L151-L154)）和 `complete_on_current_cpu()` 传入。
- `WF_MIGRATED`、`WF_RQ_SELECTED` 由核心层内部设置，分别表示"唤醒过程中任务换了 CPU"和"调用过调度类的 `select_task_rq()`"。

`WF_SYNC` 与 `WF_CURRENT_CPU` 都只是**提示**：核心层只把它们原样传给调度类（`WF_SYNC` 另外计入 schedstats），是否采纳由调度类决定。

入队时，核心层根据唤醒标志和所处的路径设置入队标志（[`kernel/sched/sched.h:2389-2400`](../../linux/kernel/sched/sched.h#L2389-L2400)），传给调度类的 `enqueue_task()`：

| 入队标志 | 何时设置 | 告诉调度类 |
|---|---|---|
| `ENQUEUE_WAKEUP` | `ttwu_do_activate()` 总是设置 | 这是一次唤醒入队，与睡眠出队时的 `DEQUEUE_SLEEP` 配对 |
| `ENQUEUE_MIGRATED` | 带有 `WF_MIGRATED`，或任务处于 `TASK_ON_RQ_MIGRATING` | 任务刚换了 CPU |
| `ENQUEUE_RQ_SELECTED` | 带有 `WF_RQ_SELECTED` | 本次入队前调用过 `select_task_rq()` |
| `ENQUEUE_INITIAL` | `wake_up_new_task()` | 新任务的第一次入队 |
| `ENQUEUE_DELAYED` | `ttwu_runnable()` | 撤销调度类推迟的出队（见下文） |

## 睡眠与唤醒的协议

### 标准写法

[`include/linux/sched.h:201-225`](../../linux/include/linux/sched.h#L201-L225)（v6.18.60）

```c
/*
 * set_current_state() includes a barrier so that the write of current->__state
 * is correctly serialised wrt the caller's subsequent test of whether to
 * actually sleep:
 *
 *   for (;;) {
 *	set_current_state(TASK_UNINTERRUPTIBLE);
 *	if (CONDITION)
 *	   break;
 *
 *	schedule();
 *   }
 *   __set_current_state(TASK_RUNNING);
 *
 * If the caller does not need such serialisation (because, for instance, the
 * CONDITION test and condition change and wakeup are under the same lock) then
 * use __set_current_state().
 *
 * The above is typically ordered against the wakeup, which does:
 *
 *   CONDITION = 1;
 *   wake_up_state(p, TASK_UNINTERRUPTIBLE);
 *
 * where wake_up_state()/try_to_wake_up() executes a full memory barrier before
 * accessing p->__state.
```

这个协议要防止**丢失唤醒**（3.2 节）：等待方检查条件为假之后、真正睡下之前，唤醒方恰好设置了条件并执行唤醒；如果唤醒方此时看到的还是 `TASK_RUNNING`，它会认为没有人需要唤醒，等待方则永远睡下去。

### 屏障配对

把两侧的内存访问按程序顺序排开：

```text
// 伪代码：两侧的访问顺序
等待方                                   唤醒方
STORE current->__state = 睡眠状态        STORE CONDITION = 1
smp_mb()        // set_current_state()   LOCK p->pi_lock
                                         smp_mb__after_spinlock()
LOAD CONDITION                           LOAD p->__state
```

这正是 7.3 节的存储缓冲（Store Buffering，SB）模式：两个 CPU 各写一个变量再读对方的变量。两侧都有完整屏障时，"两个读都读到旧值"不可能发生，所以要么等待方看到 `CONDITION` 为真而不睡，要么唤醒方看到睡眠状态而执行唤醒。

等待方的屏障来自 `set_current_state()` 中的 `smp_store_mb()`：

[`include/linux/sched.h:245-250`](../../linux/include/linux/sched.h#L245-L250)（v6.18.60）

```c
#define set_current_state(state_value)					\
	do {								\
		debug_normal_state_change((state_value));		\
		trace_set_current_state(state_value);			\
		smp_store_mb(current->__state, (state_value));		\
	} while (0)
```

唤醒方的屏障是 `try_to_wake_up()` 获取 `pi_lock` 之后的 `smp_mb__after_spinlock()`。单独的加锁只有 acquire 语义，不能阻止"锁之前的写"与"锁之后的读"重排，所以必须补上这个屏障（7.3 节）。如果条件检查与唤醒已经被同一把锁串行化（例如 7.6 节 mutex 的 `wait_lock`），等待方可以改用不带屏障的 `__set_current_state()`。

### 状态写入的冲突

[`include/linux/sched.h:227-234`](../../linux/include/linux/sched.h#L227-L234) 的注释指出了另一个细节：唤醒方看到睡眠状态后会写入 `TASK_RUNNING`，这次写入可能与等待方自己的写入"撞车"。

- 与等待方循环结束时的 `__set_current_state(TASK_RUNNING)` 撞车：两边写的值相同，无害。
- 与等待方下一轮循环的 `set_current_state(TASK_UNINTERRUPTIBLE)` 撞车，睡眠状态被覆盖成 `TASK_RUNNING`：等待方只是多绕一圈循环，由条件检查兜底。

因此睡眠必须写成"循环 + 重新检查条件"的形式，并且要容忍**伪唤醒**（Spurious Wakeup）——被唤醒时条件不一定成立。少数状态（`__TASK_STOPPED`、`__TASK_TRACED`、`TASK_PARKED`、`TASK_DEAD`、`TASK_FROZEN`）不使用这种循环，不能容忍状态被覆盖，它们用 `set_special_state()` 在 `pi_lock` 保护下写状态，与 `try_to_wake_up()` 串行化：

[`include/linux/sched.h:258-267`](../../linux/include/linux/sched.h#L258-L267)（v6.18.60）

```c
#define set_special_state(state_value)					\
	do {								\
		unsigned long flags; /* may shadow */			\
									\
		raw_spin_lock_irqsave(&current->pi_lock, flags);	\
		debug_special_state_change((state_value));		\
		trace_set_current_state(state_value);			\
		WRITE_ONCE(current->__state, (state_value));		\
		raw_spin_unlock_irqrestore(&current->pi_lock, flags);	\
	} while (0)
```

### 唤醒可能落在哪个阶段

从唤醒方的角度看，等待方可能处于下列任一阶段，`try_to_wake_up()` 要分别处理：

| 唤醒发生时等待方所处阶段 | `on_rq` | `on_cpu` | `try_to_wake_up()` 的处理 |
|---|---|---|---|
| 尚未执行 `set_current_state()` | 1 | 1 | `__state` 仍为 `TASK_RUNNING`，匹配失败，返回 0；等待方随后会看到条件为真 |
| 已设置睡眠状态，尚未在 `__schedule()` 中出队 | 1 | 1 | `ttwu_runnable()`：把状态改回 `TASK_RUNNING`，`__schedule()` 不再让它出队 |
| 设置睡眠状态后被抢占，正排队等待 CPU | 1 | 0 | 同上，并检查它能否抢占当前任务 |
| 调度类推迟了出队，任务名义上仍在队列中 | 1 | 0 或 1 | 同上，并以 `ENQUEUE_DELAYED` 撤销推迟 |
| 已出队，但切换尚未完成 | 0 | 1 | 置 `TASK_WAKING`，交给原 CPU 的唤醒队列，或自旋等待 `on_cpu` 清零 |
| 已完全切换出去 | 0 | 0 | 置 `TASK_WAKING`，选 CPU、入队 |

另有一种特例：被唤醒者就是当前任务本身（典型情形是中断处理函数唤醒了被它打断的任务），此时无需任何锁。

## 新任务的第一次入队

### fork 路径中的调度器钩子

```
kernel_clone()                      kernel/fork.c:2573
├── copy_process()                  kernel/fork.c:1935
│   ├── sched_fork()                kernel/fork.c:2158  // 初始化调度字段，状态置为 TASK_NEW
│   ├── sched_cgroup_fork()         kernel/fork.c:2302  // 确定任务组与初始 CPU，调用 task_fork()
│   └── sched_post_fork()           kernel/fork.c:2429  // uclamp 等收尾
└── wake_up_new_task()              kernel/fork.c:2645  // 选 CPU、第一次入队
```

`copy_process()` 在 `sched_fork()` 之后的任何一步失败，都会经 `sched_cancel_fork()`（[`kernel/fork.c:2483`](../../linux/kernel/fork.c#L2483)）撤销。

### sched_fork()：初始化与 TASK_NEW

[`kernel/sched/core.c:4695-4764`](../../linux/kernel/sched/core.c#L4695-L4764)（v6.18.60，节选）

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
	...
	// [注] 按 p->prio 与策略选定 p->sched_class，规则同 __setscheduler_class()（3.1 节）
	...
	init_entity_runnable_average(&p->se);
	...
	p->on_cpu = 0;
	...
	return 0;
}
```

- `__sched_fork()`（[`kernel/sched/core.c:4456-4508`](../../linux/kernel/sched/core.c#L4456-L4508)）清零 `on_rq`，重置各调度类的调度实体和调度统计。子进程经 `dup_task_struct()` 从父进程复制来的调度状态在这里被丢弃。它还把 `wake_entry` 的类型设为 `CSD_TYPE_TTWU`（[`kernel/sched/core.c:4505`](../../linux/kernel/sched/core.c#L4505)），这是唤醒队列识别"任务唤醒"节点的依据。
- `TASK_NEW` 不在任何常规唤醒使用的状态掩码中，`try_to_wake_up()` 的状态匹配必然失败。从任务加入 PID 哈希表、对外可见，到 `wake_up_new_task()` 之间，信号等外部事件都无法把它提前放进运行队列。
- `p->prio` 取父进程的 `normal_prio` 而非 `prio`，防止父进程因优先级继承获得的临时提升"遗传"给子进程。
- `SCHED_RESET_ON_FORK` 的处理、调度类的选择规则与 3.1 节一致；Deadline 策略的任务不允许 fork，直接返回 `-EAGAIN`（原因见 3.7 节）。
- `init_entity_runnable_average()` 初始化任务的 PELT 负载（3.8 节）。

### sched_cgroup_fork()：任务组与初始 CPU

[`kernel/sched/core.c:4766-4795`](../../linux/kernel/sched/core.c#L4766-L4795)（v6.18.60）

```c
int sched_cgroup_fork(struct task_struct *p, struct kernel_clone_args *kargs)
{
	unsigned long flags;

	/*
	 * Because we're not yet on the pid-hash, p->pi_lock isn't strictly
	 * required yet, but lockdep gets upset if rules are violated.
	 */
	raw_spin_lock_irqsave(&p->pi_lock, flags);
#ifdef CONFIG_CGROUP_SCHED
	if (1) {
		struct task_group *tg;
		tg = container_of(kargs->cset->subsys[cpu_cgrp_id],
				  struct task_group, css);
		tg = autogroup_task_group(p, tg);
		p->sched_task_group = tg;
	}
#endif
	rseq_migrate(p);
	/*
	 * We're setting the CPU for the first time, we don't migrate,
	 * so use __set_task_cpu().
	 */
	__set_task_cpu(p, smp_processor_id());
	if (p->sched_class->task_fork)
		p->sched_class->task_fork(p);
	raw_spin_unlock_irqrestore(&p->pi_lock, flags);

	return scx_fork(p);
}
```

`copy_process()` 在 cgroup 被固定之后、任务对外可见之前调用它（[`kernel/fork.c:2293-2302`](../../linux/kernel/fork.c#L2293-L2302)）：

- 确定任务所属的 `task_group`（含 autogroup，3.10 节）；
- 用 `__set_task_cpu()` 把初始 CPU 暂定为当前 CPU。这只是一个临时位置，真正的选核推迟到 `wake_up_new_task()`；
- 调用调度类的 `task_fork()` 回调，让调度类初始化与父进程相关的状态。

### wake_up_new_task()：选核与第一次入队

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

1. **持 `pi_lock` 直接置 `TASK_RUNNING`**。新任务从未运行过（`on_cpu == 0`），也不在任何运行队列上，不存在 `try_to_wake_up()` 要应对的那些中间状态；整个过程又都在 `pi_lock` 之下，并发的唤醒者只能等它完成，之后看到的已经是 `TASK_RUNNING`。因此这里不需要 `TASK_WAKING`。
2. **fork 均衡**（Fork Balancing）：以 `WF_FORK` 调用 `select_task_rq()`。注释解释了选核为什么推迟到这里：fork 过程中 `cpus_ptr` 可能改变，早先选好的 CPU 也可能被热拔。这里用 `__set_task_cpu()` 而不是 `set_task_cpu()`，以免在任务尚未完全初始化时调用调度类的 `migrate_task_rq()`。
3. 锁住目标 rq，初始化任务的利用率估计（`post_init_entity_util_avg()`，3.8 节）。
4. `activate_task()` 以 `ENQUEUE_INITIAL` 入队，让调度类知道这是第一次入队，可以为新任务做特殊放置。
5. `wakeup_preempt()` 判断是否抢占目标 CPU 上的当前任务（3.1 节），再调用 `task_woken()` 回调。

新任务被选中后的第一次运行从 `ret_from_fork_asm` 开始（3.2 节）。

## 唤醒的入口

内核中有多种唤醒接口，最终都汇聚到 `try_to_wake_up()`：

```
wake_up_process(p)                   kernel/sched/core.c:4438  → try_to_wake_up(p, TASK_NORMAL, 0)
wake_up_state(p, state)              kernel/sched/core.c:4444  → try_to_wake_up(p, state, 0)
wake_up() / wake_up_interruptible()  include/linux/wait.h:221-231
└── __wake_up()                      kernel/sched/wait.c:144
    └── __wake_up_common_lock()      kernel/sched/wait.c:119
        └── __wake_up_common()       kernel/sched/wait.c:93
            └── curr->func()         // 常见为 autoremove_wake_function()
                └── default_wake_function()          kernel/sched/core.c:7273
                    └── try_to_wake_up(curr->private, mode, wake_flags)
wake_up_q(head)                      kernel/sched/core.c:1084  → wake_up_process()
```

`try_to_wake_up()` 返回 1 表示这次调用确实唤醒了任务，返回 0 表示任务状态不匹配（通常是它已经在运行）。调用者会利用这个返回值，例如信号投递在唤醒失败时调用 `kick_process()`（[`kernel/sched/core.c:3476-3483`](../../linux/kernel/sched/core.c#L3476-L3483)），向正在其他 CPU 上运行的目标任务发送重调度 IPI，让它尽快进入内核处理信号。

### 等待队列

**等待队列**（Wait Queue）是最常用的唤醒接口：等待方把一个 `wait_queue_entry` 挂到 `wait_queue_head` 上再睡眠，唤醒方遍历链表，逐个调用其中的唤醒函数。

[`kernel/sched/wait.c:93-117`](../../linux/kernel/sched/wait.c#L93-L117)（v6.18.60）

```c
static int __wake_up_common(struct wait_queue_head *wq_head, unsigned int mode,
			int nr_exclusive, int wake_flags, void *key)
{
	wait_queue_entry_t *curr, *next;

	lockdep_assert_held(&wq_head->lock);

	curr = list_first_entry(&wq_head->head, wait_queue_entry_t, entry);

	if (&curr->entry == &wq_head->head)
		return nr_exclusive;

	list_for_each_entry_safe_from(curr, next, &wq_head->head, entry) {
		unsigned flags = curr->flags;
		int ret;

		ret = curr->func(curr, mode, wake_flags, key);
		if (ret < 0)
			break;
		if (ret && (flags & WQ_FLAG_EXCLUSIVE) && !--nr_exclusive)
			break;
	}

	return nr_exclusive;
}
```

- `mode` 就是传给 `try_to_wake_up()` 的状态掩码，`wake_up()` 为 `TASK_NORMAL`，`wake_up_interruptible()` 为 `TASK_INTERRUPTIBLE`（[`include/linux/wait.h:221-231`](../../linux/include/linux/wait.h#L221-L231)）。
- **排他等待**（`WQ_FLAG_EXCLUSIVE`）用于避免"惊群"：`wake_up()` 只唤醒 1 个排他等待者，`wake_up_all()` 传入 `nr_exclusive == 0`，唤醒全部。注意只有唤醒函数返回非零（真正唤醒了任务）才消耗名额；函数头注释（[`kernel/sched/wait.c:80-92`](../../linux/kernel/sched/wait.c#L80-L92)）说明，任务可能已经开始运行但状态还不是 `TASK_RUNNING`，此时 `try_to_wake_up()` 返回 0，遍历继续向后寻找。
- `default_wake_function()`（[`kernel/sched/core.c:7273-7278`](../../linux/kernel/sched/core.c#L7273-L7278)）只允许 `WF_SYNC` 与 `WF_CURRENT_CPU` 两种唤醒标志从等待队列传入。

### wake_q：先登记、后唤醒

持有自旋锁时直接唤醒有两个缺点：唤醒本身有一定开销，会拉长临界区；被唤醒的任务可能立即在别的 CPU 上运行，转头就来争抢唤醒者还没释放的锁。`wake_q`（Wake Queue）机制把唤醒拆成两步：在临界区内只把任务登记到一个局部链表，释放锁之后再由 `wake_up_q()` 逐个调用 `wake_up_process()`。mutex、rwsem、futex 的唤醒都使用它（7.6～7.8 节）。

[`kernel/sched/core.c:1019-1041`](../../linux/kernel/sched/core.c#L1019-L1041)（v6.18.60）

```c
static bool __wake_q_add(struct wake_q_head *head, struct task_struct *task)
{
	struct wake_q_node *node = &task->wake_q;

	/*
	 * Atomically grab the task, if ->wake_q is !nil already it means
	 * it's already queued (either by us or someone else) and will get the
	 * wakeup due to that.
	 *
	 * In order to ensure that a pending wakeup will observe our pending
	 * state, even in the failed case, an explicit smp_mb() must be used.
	 */
	smp_mb__before_atomic();
	if (unlikely(cmpxchg_relaxed(&node->next, NULL, WAKE_Q_TAIL)))
		return false;

	/*
	 * The head is context local, there can be no concurrency.
	 */
	*head->lastp = node;
	head->lastp = &node->next;
	return true;
}
```

- 每个任务只有一个 `wake_q` 节点，`cmpxchg` 保证它同一时刻最多在一个链表中。如果任务已被别人登记，这次登记直接放弃——别人的 `wake_up_q()` 很快就会唤醒它。
- 登记成功时 `wake_q_add()` 增加任务引用计数，`wake_up_q()`（[`kernel/sched/core.c:1084-1104`](../../linux/kernel/sched/core.c#L1084-L1104)）唤醒后再释放，保证唤醒时 `task_struct` 仍然有效。
- 头文件注释（[`include/linux/sched/wake_q.h:5-33`](../../linux/include/linux/sched/wake_q.h#L5-L33)）强调了两点：唤醒可能在 `wake_q_add()` 处就立即发生（如果任务已在别人的链表中），所以调用 `wake_q_add()` 时任务必须已经可以被唤醒；这一机制也可能造成伪唤醒，等待方必须在循环中确认条件。

## try_to_wake_up() 主流程

### 调用链总览

```
try_to_wake_up(p, state, wake_flags)                  kernel/sched/core.c:4159
├── [p == current] ttwu_state_match() → ttwu_do_wakeup()       // 只改状态，不取锁
└── scoped_guard(p->pi_lock)
    ├── ttwu_state_match()                            // 不匹配：返回 0
    ├── [on_rq] ttwu_runnable()                       // 仍在队列上：改回 TASK_RUNNING
    ├── p->__state = TASK_WAKING
    ├── [on_cpu] ttwu_queue_wakelist(p, task_cpu(p))  // 仍在切换：交给原 CPU
    ├── smp_cond_load_acquire(&p->on_cpu, !VAL)       // 否则等待切换完成
    ├── select_task_rq() → class->select_task_rq()
    ├── [CPU 改变] set_task_cpu() → class->migrate_task_rq()
    └── ttwu_queue()
        ├── ttwu_queue_wakelist() ⇢ IPI ⇢ sched_ttwu_pending() → ttwu_do_activate()
        └── ttwu_do_activate()
            ├── activate_task() → class->enqueue_task()
            ├── wakeup_preempt() → class->wakeup_preempt() 或 resched_curr()
            ├── ttwu_do_wakeup()                      // __state = TASK_RUNNING
            └── class->task_woken()
```

函数头注释（[`kernel/sched/core.c:4122-4158`](../../linux/kernel/sched/core.c#L4122-L4158)）概括了它的设计：概念上只做"若 `state & p->__state` 则置 `TASK_RUNNING`，必要时放回运行队列"；用 `p->pi_lock` 串行化并发唤醒；**尽量只获取一把 rq 锁**。

### 唤醒当前任务

[`kernel/sched/core.c:4159-4189`](../../linux/kernel/sched/core.c#L4159-L4189)（v6.18.60）

```c
int try_to_wake_up(struct task_struct *p, unsigned int state, int wake_flags)
{
	guard(preempt)();
	int cpu, success = 0;

	wake_flags |= WF_TTWU;

	if (p == current) {
		/*
		 * We're waking current, this means 'p->on_rq' and 'task_cpu(p)
		 * == smp_processor_id()'. Together this means we can special
		 * case the whole 'p->on_rq && ttwu_runnable()' case below
		 * without taking any locks.
		 *
		 * Specifically, given current runs ttwu() we must be before
		 * schedule()'s block_task(), as such this must not observe
		 * sched_delayed.
		 *
		 * In particular:
		 *  - we rely on Program-Order guarantees for all the ordering,
		 *  - we're serialized against set_special_state() by virtue of
		 *    it disabling IRQs (this allows not taking ->pi_lock).
		 */
		WARN_ON_ONCE(p->se.sched_delayed);
		if (!ttwu_state_match(p, state, &success))
			goto out;

		trace_sched_waking(p);
		ttwu_do_wakeup(p);
		goto out;
	}
```

整个函数在关抢占下运行（`guard(preempt)()`）。被唤醒者就是 `current` 时，唤醒代码由任务自己或打断它的中断执行，任务必然还没走到 `schedule()` 中的出队，一定在本 CPU 的运行队列上。同一 CPU 上的程序顺序足以保证正确性；唯一可能同时修改状态的 `set_special_state()` 在关中断下执行，与中断中的唤醒天然互斥。所以这条路径不取任何锁，匹配成功后只把 `__state` 改回 `TASK_RUNNING`。

### 持 pi_lock 匹配状态

[`kernel/sched/core.c:4191-4228`](../../linux/kernel/sched/core.c#L4191-L4228)（v6.18.60，节选）

```c
	/*
	 * If we are going to wake up a thread waiting for CONDITION we
	 * need to ensure that CONDITION=1 done by the caller can not be
	 * reordered with p->state check below. This pairs with smp_store_mb()
	 * in set_current_state() that the waiting thread does.
	 */
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

`p->pi_lock` 在这里有两个作用：

- 串行化对同一任务的并发唤醒。两个唤醒者不会同时认领同一个任务。
- 稳定 `p->sched_class`、`p->cpus_ptr`、`p->sched_task_group`。修改这些字段的路径（如 `sched_setscheduler()`、`sched_setaffinity()`）同时持有 `pi_lock` 与 rq 锁（3.1 节），所以只持 `pi_lock` 的唤醒者可以安全地据此选核和迁移。

`smp_mb__after_spinlock()` 就是前文屏障配对中唤醒方的那个完整屏障。

状态匹配由 `ttwu_state_match()` 完成：

[`kernel/sched/core.c:4007-4037`](../../linux/kernel/sched/core.c#L4007-L4037)（v6.18.60，节选）

```c
static __always_inline
bool ttwu_state_match(struct task_struct *p, unsigned int state, int *success)
{
	int match;

	/* 省略 CONFIG_DEBUG_PREEMPT 相关代码 */
	*success = !!(match = __task_state_match(p, state));

	/*
	 * Saved state preserves the task state across blocking on
	 * an RT lock or TASK_FREEZABLE tasks.  If the state matches,
	 * set p::saved_state to TASK_RUNNING, but do not wake the task
	 * because it waits for a lock wakeup or __thaw_task(). Also
	 * indicate success because from the regular waker's point of
	 * view this has succeeded.
	 *
	 * After acquiring the lock the task will restore p::__state
	 * from p::saved_state which ensures that the regular
	 * wakeup is not lost. The restore will also set
	 * p::saved_state to TASK_RUNNING so any further tests will
	 * not result in false positives vs. @success
	 */
	if (match < 0)
		p->saved_state = TASK_RUNNING;

	return match > 0;
}
```

`__task_state_match()`（[`kernel/sched/core.c:2236-2246`](../../linux/kernel/sched/core.c#L2236-L2246)）先比较 `__state`，命中返回 1；再比较 `saved_state`，命中返回 -1。后一种情况出现在两类场景：

- **PREEMPT_RT 睡眠锁**（依赖 `CONFIG_PREEMPT_RT`）：`spinlock_t` 在 PREEMPT_RT 下是基于 rt_mutex 的可睡眠锁。任务设置睡眠状态之后、调用 `schedule()` 之前若阻塞在这种锁上，原状态被暂存到 `saved_state`，`__state` 改为 `TASK_RTLOCK_WAIT`（[`include/linux/sched.h:294-303`](../../linux/include/linux/sched.h#L294-L303)）。
- **冻结**：可冻结的任务被冻结时，原状态同样暂存在 `saved_state` 中。

此时任务真正等待的是锁释放或解冻，普通唤醒不能把它从那里拉出来，但也不能丢失。于是只把 `saved_state` 改为 `TASK_RUNNING` 并报告成功：任务拿到锁或解冻后从 `saved_state` 恢复状态，会发现自己"已经被唤醒过"。

匹配成功后，`smp_rmb()` 保证先读 `__state`、再读 `on_rq`。被省略的注释（[`kernel/sched/core.c:4204-4225`](../../linux/kernel/sched/core.c#L4204-L4225)）给出了反例：任务被前一次唤醒放回队列、切入运行后又设置了睡眠状态；如果读 `on_rq` 被提前到读 `__state` 之前，唤醒者可能看到前一次唤醒之前那个过时的 `on_rq == 0`，误以为任务已出队，随后卡在等待 `on_cpu` 清零的自旋中。

### 任务仍在队列上：ttwu_runnable()

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

`on_rq` 非零说明任务还在运行队列上，对应前文表格中的三种情形：

1. 任务刚设置了睡眠状态，正在运行或正在执行 `__schedule()`，还没出队；
2. 任务设置睡眠状态后被抢占。抢占不会让任务出队（3.2 节），它正排队等待 CPU；
3. 任务已经"睡下"，但调度类推迟了真正的出队。3.2 节提到，`block_task()` 只有在调度类的 `dequeue_task()` 返回真时才清零 `on_rq`；调度类可以借此把睡眠任务暂时留在队列上（延迟出队，`p->se.sched_delayed`，3.3 节）。

无论哪种情形，任务都还没有离开运行队列，只需把状态改回 `TASK_RUNNING`。但必须获取任务所在 rq 的锁，与 `__schedule()` 中的出队操作串行化：持锁后用 `task_on_rq_queued()` 再检查一次，如果任务恰好在这期间出队了，就返回 0，走后面的完整唤醒路径。这是 `try_to_wake_up()` 中不可避免的一次旧 rq 加锁（源码注释："old rq, unavoidable"）。

对第 3 种情形，以 `ENQUEUE_DELAYED` 调用 `enqueue_task()`，让调度类撤销推迟状态。若任务此刻不在 CPU 上（第 2 种情形，以及切换已完成的第 3 种情形），它只是在队列中排队，还要用 `wakeup_preempt()` 检查它是否应抢占当前任务。

### 已出队：TASK_WAKING 与 on_cpu

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

走到这里说明 `on_rq == 0`，任务确实已经出队。但"已出队"不等于"已切换完"：3.2 节中 `prev` 在 `__schedule()` 早期就出队了，`on_cpu` 却要到 `finish_task()` 才清零。这段窗口里任务仍在旧 CPU 上执行调度代码、使用着自己的内核栈，绝不能让它同时在别的 CPU 上运行。

- `smp_acquire__after_ctrl_dep()` 把前面"读到 `on_rq == 0` 才继续"的控制依赖升级为 acquire 语义，与 `__block_task()` 中的 `smp_store_release(&p->on_rq, 0)` 配对（3.2 节）。此后可以确信 `__schedule()` 已经处理完 `prev->__state`，不会再读它。
- **`TASK_WAKING`** 表示"唤醒已被认领，任务正在入队途中"。有了这个标记，后续的入队可以在不持 `pi_lock` 的情况下完成（例如交给远程 CPU 的唤醒队列），而其他唤醒者看到 `TASK_WAKING` 时状态匹配会失败，不会重复处理。
- **`on_cpu` 仍为 1**：任务正在某个 CPU 上被切换出去。与其原地自旋，不如把唤醒请求挂到**那个 CPU** 的唤醒队列上（`ttwu_queue_wakelist(p, task_cpu(p), ...)`），让唤醒者立即返回。这是安全的：那个 CPU 在关中断状态下执行 `__schedule()`，IPI 只能在切换完成、`on_cpu` 清零并开中断之后才被处理（[`kernel/sched/core.c:4263-4281`](../../linux/kernel/sched/core.c#L4263-L4281) 的注释）。注意这条路径直接在原 CPU 上唤醒，**不经过选核**。
- 不满足唤醒队列的使用条件时，用 `smp_cond_load_acquire()` 自旋等待 `on_cpu` 变为 0。它与 `finish_task()` 中的 `smp_store_release(&prev->on_cpu, 0)`（[`kernel/sched/core.c:4968-4982`](../../linux/kernel/sched/core.c#L4968-L4982)）配对，保证任务在旧 CPU 上的所有操作都先于它在新 CPU 上的执行。[`kernel/sched/core.c:4039-4120`](../../linux/kernel/sched/core.c#L4039-L4120) 的长注释称之为**程序序**（Program Order）保证：迁移靠 rq 锁的释放 / 获取链提供这一保证，睡眠后的唤醒则没有锁链可用，只能靠 `on_cpu` 的 release / acquire。

### 选核、迁移与入队

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

到这里任务已完全离开旧 CPU，又不在任何运行队列上，`pi_lock` 足以保护它。唤醒者以 `p->wake_cpu` 为起点调用 `select_task_rq()` 选出目标 CPU（见下一节），若与原 CPU 不同：

- 任务的 iowait 计数记在旧 CPU 的 `nr_iowait` 上，必须趁 `task_rq(p)` 还指向旧 rq 时撤销；
- 加上 `WF_MIGRATED`，入队时转换为 `ENQUEUE_MIGRATED`；
- `psi_ttwu_dequeue()`（[`kernel/sched/stats.h:199-216`](../../linux/kernel/sched/stats.h#L199-L216)）把任务的 PSI 睡眠状态从旧 CPU 上注销，这需要短暂获取旧 rq 的锁；
- `set_task_cpu()` 修改 `task_cpu(p)`。

最后由 `ttwu_queue()` 把任务放入目标 CPU 的运行队列（见"远程唤醒队列"一节）。

返回前，若唤醒成功，`ttwu_stat()`（[`kernel/sched/core.c:3655-3690`](../../linux/kernel/sched/core.c#L3655-L3690)）更新 schedstats 计数（依赖 `CONFIG_SCHEDSTATS`，且需运行时开启）。注意"local / remote"是相对**唤醒者**所在的 CPU 而言的：被唤醒者落在唤醒者 CPU 上计入 `nr_wakeups_local` 与 `rq->ttwu_local`，否则计入 `nr_wakeups_remote`；`WF_MIGRATED` 与 `WF_SYNC` 还分别计入 `nr_wakeups_migrate` 与 `nr_wakeups_sync`。

## 选择目标 CPU

### 三个放置时机

| 时机 | 入口 | 唤醒标志 | 如何询问调度类 | 如何改变任务的 CPU |
|---|---|---|---|---|
| fork | `wake_up_new_task()` | `WF_FORK` | 核心层 `select_task_rq()` | `__set_task_cpu()`：任务尚未入队、尚未完全初始化 |
| exec | `sched_exec()` | `WF_EXEC` | 直接调用 `class->select_task_rq()` | 任务正在运行，借 stopper 线程执行 `migration_cpu_stop()` |
| 唤醒 | `try_to_wake_up()` | `WF_TTWU` | 核心层 `select_task_rq()` | `set_task_cpu()`：任务不在任何运行队列上 |

三者的共同点是：都持有 `p->pi_lock`，都只在任务"轻装"的时刻决定放置。已在队列上的任务也会被迁移（负载均衡、修改亲和性、CPU 下线等），但那需要持有 rq 锁并经过 `TASK_ON_RQ_MIGRATING` 状态（3.1 节、3.9 节），不属于本节范围。

### select_task_rq()：核心层的包装

[`kernel/sched/core.c:3579-3608`](../../linux/kernel/sched/core.c#L3579-L3608)（v6.18.60）

```c
/*
 * The caller (fork, wakeup) owns p->pi_lock, ->cpus_ptr is stable.
 */
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

- **只有能选择时才询问调度类**。任务只能在一个 CPU 上运行（绑核任务、per-CPU 内核线程），或处于禁止迁移状态（此时 `cpus_ptr` 临时指向只含当前 CPU 的掩码，3.1 节）时，直接取掩码中的那个 CPU，完全绕过调度类的选核逻辑。
- 调度类回调收到三个参数：任务、起点 CPU（唤醒时为 `p->wake_cpu`，fork 时为 `sched_cgroup_fork()` 暂定的 CPU）、唤醒标志。它可以按自己的策略返回任意 CPU；各调度类的选核策略不在本节讨论。
- **合法性由核心层统一兜底**。`is_cpu_allowed()`（[`kernel/sched/core.c:2435-2459`](../../linux/kernel/sched/core.c#L2435-L2459)）要求 CPU 在任务的掩码中；普通用户任务还要求 CPU 处于 active 状态，per-CPU 内核线程与禁止迁移的任务只要求 online。不合法时由 `select_fallback_rq()`（[`kernel/sched/core.c:3508-3577`](../../linux/kernel/sched/core.c#L3508-L3577)）重选：先在同一 NUMA 节点中找允许且可用的 CPU，再在整个 `cpus_ptr` 中找；仍找不到就逐级放宽亲和性（先放宽到 cpuset 允许的范围，再放宽到所有可能的 CPU），并打印 "no longer affine to cpu" 提示。

源码注释点明了这样分工的好处：调度类的回调可以简单地返回 `task_cpu(p)`，不必关心 CPU 热插拔和亲和性这类通用约束。

### set_task_cpu()：改变任务所在的 CPU

[`kernel/sched/core.c:3302-3354`](../../linux/kernel/sched/core.c#L3302-L3354)（v6.18.60，节选）

```c
void set_task_cpu(struct task_struct *p, unsigned int new_cpu)
{
	unsigned int state = READ_ONCE(p->__state);

	/*
	 * We should never call set_task_cpu() on a blocked task,
	 * ttwu() will sort out the placement.
	 */
	WARN_ON_ONCE(state != TASK_RUNNING && state != TASK_WAKING && !p->on_rq);
	...
	/* 省略 CONFIG_LOCKDEP 相关代码：要求调用者持有 p->pi_lock 或 task_rq(p) 的锁 */
	/*
	 * Clearly, migrating tasks to offline CPUs is a fairly daft thing.
	 */
	WARN_ON_ONCE(!cpu_online(new_cpu));

	WARN_ON_ONCE(is_migration_disabled(p));

	trace_sched_migrate_task(p, new_cpu);

	if (task_cpu(p) != new_cpu) {
		if (p->sched_class->migrate_task_rq)
			p->sched_class->migrate_task_rq(p, new_cpu);
		p->se.nr_migrations++;
		rseq_migrate(p);
		sched_mm_cid_migrate_from(p);
		perf_event_task_migrate(p);
	}

	__set_task_cpu(p, new_cpu);
}
```

- 第一条检查表达了本节的核心约束：**不能对已阻塞的任务调用 `set_task_cpu()`**，除非它处于 `TASK_WAKING`，即已被某个唤醒者认领。睡眠任务的放置只能由 `try_to_wake_up()` 决定。
- 被省略的 lockdep 检查要求调用者持有 `p->pi_lock`（唤醒中的任务）或 `task_rq(p)` 的锁（可运行任务），二者之一即可"钉住"任务所在的 CPU。
- CPU 真正改变时调用调度类的 `migrate_task_rq()`，让调度类把任务在旧 CPU 上的记账转走。由于唤醒者并不持有旧 rq 的锁，调度类在这个回调中只能做不依赖旧 rq 锁的操作（3.8 节"迁移的异步移除"就是一例）。随后累加迁移次数，并通知 rseq、mm_cid 与 perf。

真正写入 CPU 编号的是 `__set_task_cpu()`：

[`kernel/sched/sched.h:2214-2227`](../../linux/kernel/sched/sched.h#L2214-L2227)（v6.18.60）

```c
static inline void __set_task_cpu(struct task_struct *p, unsigned int cpu)
{
	set_task_rq(p, cpu);
#ifdef CONFIG_SMP
	/*
	 * After ->cpu is set up to a new value, task_rq_lock(p, ...) can be
	 * successfully executed on another CPU. We must ensure that updates of
	 * per-task data have been completed by this moment.
	 */
	smp_wmb();
	WRITE_ONCE(task_thread_info(p)->cpu, cpu);
	p->wake_cpu = cpu;
#endif /* CONFIG_SMP */
}
```

`set_task_rq()` 更新组调度相关的每 CPU 指针（3.10 节）。`smp_wmb()` 保证这些修改先于新的 CPU 编号可见：一旦写入新编号，其他 CPU 上的 `task_rq_lock()` 就可能找到并锁住新的 rq，此时它们看到的任务数据必须已经是完整的。

### sched_exec()：exec 时的放置

[`kernel/sched/core.c:5458-5479`](../../linux/kernel/sched/core.c#L5458-L5479)（v6.18.60）

```c
/*
 * sched_exec - execve() is a valuable balancing opportunity, because at
 * this point the task has the smallest effective memory and cache footprint.
 */
void sched_exec(void)
{
	struct task_struct *p = current;
	struct migration_arg arg;
	int dest_cpu;

	scoped_guard (raw_spinlock_irqsave, &p->pi_lock) {
		dest_cpu = p->sched_class->select_task_rq(p, task_cpu(p), WF_EXEC);
		if (dest_cpu == smp_processor_id())
			return;

		if (unlikely(!cpu_active(dest_cpu)))
			return;

		arg = (struct migration_arg){ p, dest_cpu };
	}
	stop_one_cpu(task_cpu(p), migration_cpu_stop, &arg);
}
```

`bprm_execve()` 在装载新程序之前调用它（[`fs/exec.c:1770`](../../linux/fs/exec.c#L1770)）。注释解释了动机：此时旧地址空间即将被丢弃、新程序尚未建立工作集，任务的内存与缓存足迹最小，迁移代价最低。

与唤醒不同，调用者就是正在运行的任务本身，不能直接改写自己的 `task_cpu`。如果调度类选中了别的 CPU，就请本 CPU 的 stopper 线程执行 `migration_cpu_stop()`：stopper 抢占当前任务，再按 3.1 节的方式把它从本 CPU 的队列迁到目标 CPU。

## 远程唤醒队列

### 为什么需要唤醒队列

最直接的远程唤醒方式是：唤醒者获取目标 CPU 的 rq 锁，把任务插入目标运行队列，必要时再发 IPI 让目标 CPU 重新调度。在大型多核机器上，rq 锁与运行队列数据所在的缓存行因此要在唤醒者与目标 CPU 之间来回传递；两个 CPU 不共享**末级缓存**（Last Level Cache，LLC）时，代价尤其高。

`TTWU_QUEUE` 特性提供了另一种方式：唤醒者只把任务挂到目标 CPU 的一个无锁链表——**唤醒队列**（Wake List）——上，再发送**处理器间中断**（Inter-Processor Interrupt，IPI），由目标 CPU 自己获取本地 rq 锁完成入队。这样运行队列只被本地 CPU 访问。

[`kernel/sched/features.h:74-83`](../../linux/kernel/sched/features.h#L74-L83)（v6.18.60）

```c
#ifdef CONFIG_PREEMPT_RT
SCHED_FEAT(TTWU_QUEUE, false)
#else

/*
 * Queue remote wakeups on the target CPU and process them
 * using the scheduler IPI. Reduces rq->lock contention/bounces.
 */
SCHED_FEAT(TTWU_QUEUE, true)
#endif
```

该特性在未开启 `CONFIG_PREEMPT_RT` 时默认开启；开启 `CONFIG_PREEMPT_RT` 时默认关闭，此时所有唤醒都由唤醒者直接完成入队。运行时可以通过 `/sys/kernel/debug/sched/features` 切换。

整条路径横跨两个 CPU：

```
[唤醒者 CPU]
ttwu_queue_wakelist()                          kernel/sched/core.c:3964
└── __ttwu_queue_wakelist()                    kernel/sched/core.c:3860
    └── __smp_call_single_queue()              kernel/smp.c:380      // 链表原本为空时才继续
        └── send_call_function_single_ipi()    kernel/smp.c:115
            ├── call_function_single_prep_ipi()  kernel/sched/core.c:3844  // 目标在轮询：只置 NEED_RESCHED
            └── arch_send_call_function_single_ipi()                       // 否则发送 IPI

[目标 CPU，IPI 处理]
sysvec_call_function_single()                  arch/x86/kernel/smp.c:266
└── generic_smp_call_function_single_interrupt()  kernel/smp.c:461
    └── __flush_smp_call_function_queue()      kernel/smp.c:480
        └── sched_ttwu_pending()               kernel/sched/core.c:3801
            └── ttwu_do_activate()

[目标 CPU，从轮询 idle 中退出]
do_idle()                                      kernel/sched/idle.c:276
└── flush_smp_call_function_queue()            kernel/smp.c:610
    └── __flush_smp_call_function_queue() → sched_ttwu_pending()
```

### 何时使用：ttwu_queue_cond()

[`kernel/sched/core.c:3915-3962`](../../linux/kernel/sched/core.c#L3915-L3962)（v6.18.60，节选）

```c
static inline bool ttwu_queue_cond(struct task_struct *p, int cpu)
{
	// [注] 省略两项针对特定调度类的排除条件
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

排除 CPU 热插拔过程中和亲和性不允许的情形后，满足以下任一条件就使用唤醒队列：

- **唤醒者与目标 CPU 不共享 LLC**：避免跨缓存域访问远程运行队列。`cpus_share_cache()`（[`kernel/sched/core.c:3895-3901`](../../linux/kernel/sched/core.c#L3895-L3901)）比较两个 CPU 的 `sd_llc_id`，即各自所在 LLC 调度域的标识（3.9 节）。
- **目标 CPU 空闲**（`nr_running == 0`）：把入队工作卸载给反正无事可做的目标 CPU，忙碌的唤醒者可以更快返回。被省略的注释（[`kernel/sched/core.c:3947-3957`](../../linux/kernel/sched/core.c#L3947-L3957)）指出，"任务正在切换出去、且是目标 CPU 上唯一的任务"也属于这种情况——被唤醒者此时已出队，不再计入 `nr_running`。前文 `on_cpu` 仍为 1 时的委托能否成功，就取决于这一条。

本地唤醒，以及目标 CPU 与唤醒者共享 LLC 且正忙时，不使用唤醒队列。外层的 `ttwu_queue_wakelist()`（[`kernel/sched/core.c:3964-3973`](../../linux/kernel/sched/core.c#L3964-L3973)）还要求 `TTWU_QUEUE` 特性开启，并在投递前调用一次 `sched_clock_cpu(cpu)` 同步目标 CPU 的调度时钟。

### 投递：__ttwu_queue_wakelist()

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

- `sched_remote_wakeup` 把"是否换了 CPU"带到目标 CPU，那里会据此恢复 `WF_MIGRATED`。
- `rq->ttwu_pending` 告诉其他 CPU "这个 CPU 马上就有任务了"：`idle_cpu()`（[`kernel/sched/syscalls.c:191-205`](../../linux/kernel/sched/syscalls.c#L191-L205)）在它非零时不把该 CPU 视为空闲，避免选核逻辑在任务真正入队之前继续往这里放任务。
- `p->wake_entry` 是嵌入 `task_struct` 的跨核调用节点，类型为 `CSD_TYPE_TTWU`。`__smp_call_single_queue()` 用 `llist_add()` 把它挂到目标 CPU 的 `call_single_queue` 上，**只有链表原本为空时**才发送 IPI（[`kernel/smp.c:412-413`](../../linux/kernel/smp.c#L412-L413)）。一个 IPI 尚未被处理期间到达的后续唤醒都搭这一班车。

发送 IPI 之前还有一个优化：

[`kernel/sched/core.c:3844-3852`](../../linux/kernel/sched/core.c#L3844-L3852)（v6.18.60）

```c
bool call_function_single_prep_ipi(int cpu)
{
	if (set_nr_if_polling(cpu_rq(cpu)->idle)) {
		trace_sched_wake_idle_without_ipi(cpu);
		return false;
	}

	return true;
}
```

如果目标 CPU 的 idle 线程正处于轮询状态（设置了 `TIF_POLLING_NRFLAG`，在循环中不断检查重调度标志），`set_nr_if_polling()` 只给它设置 `TIF_NEED_RESCHED`，省去 IPI。idle 循环看到标志后退出轮询，在调用 `schedule_idle()` 之前先执行 `flush_smp_call_function_queue()`，处理挂起的唤醒（[`kernel/sched/idle.c:367-379`](../../linux/kernel/sched/idle.c#L367-L379)）。

### 处理：sched_ttwu_pending()

目标 CPU 处理跨核调用队列时，先执行同步回调，再执行其余异步回调，最后把剩下的所有 `CSD_TYPE_TTWU` 节点一次性交给 `sched_ttwu_pending()`（[`kernel/smp.c:588-594`](../../linux/kernel/smp.c#L588-L594)）：

[`kernel/sched/core.c:3801-3836`](../../linux/kernel/sched/core.c#L3801-L3836)（v6.18.60）

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

	/*
	 * Must be after enqueueing at least once task such that
	 * idle_cpu() does not observe a false-negative -- if it does,
	 * it is possible for select_idle_siblings() to stack a number
	 * of tasks on this CPU during that window.
	 *
	 * It is OK to clear ttwu_pending when another task pending.
	 * We will receive IPI after local IRQ enabled and then enqueue it.
	 * Since now nr_running > 0, idle_cpu() will always get correct result.
	 */
	WRITE_ONCE(rq->ttwu_pending, 0);
	rq_unlock_irqrestore(rq, &rf);
}
```

- 一批唤醒只需获取一次本地 rq 锁。
- 两个 `WARN_ON_ONCE` 检查的条件本应由发送方保证：IPI 在 `on_cpu` 清零后才会被处理，`task_cpu(p)` 在投递前已设为本 CPU。
- 唤醒标志只恢复了 `WF_MIGRATED`。`WF_SYNC`、`WF_RQ_SELECTED` 等不会随任务传到目标 CPU，在这条路径上只影响唤醒者一侧的选核，不再参与入队标志与 `wakeup_preempt()` 的判断。
- `ttwu_pending` 必须在至少一个任务入队之后才清零，这样 `idle_cpu()` 在任何时刻都不会把本 CPU 误判为空闲。

此时被唤醒任务的 `__state` 一直是 `TASK_WAKING`，`pi_lock` 早已被唤醒者释放，`ttwu_do_activate()` 在目标 CPU 上完成剩下的工作。

### 直接入队：ttwu_queue()

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

不使用唤醒队列时，唤醒者在仍持有 `pi_lock` 的情况下获取目标 rq 锁，自己完成入队。

## ttwu_do_activate()：入队与收尾

[`kernel/sched/core.c:3701-3725`](../../linux/kernel/sched/core.c#L3701-L3725)（v6.18.60）

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
```

1. **撤销睡眠时的统计**。`__block_task()` 在睡眠时增加了 `nr_uninterruptible` 与 `nr_iowait`（3.2 节），这里对应地减少。`nr_uninterruptible` 在目标 rq 上减，这正是它"只有全局之和有意义"的原因。`nr_iowait` 则必须在原 CPU 上减：没有迁移时 `task_rq(p)` 就是原 CPU，在这里处理；发生了迁移时已在 `try_to_wake_up()` 中提前处理，这里通过 `WF_MIGRATED` 跳过。
2. **翻译标志并入队**。`activate_task()` 调用调度类的 `enqueue_task()`，并把 `on_rq` 置为 `TASK_ON_RQ_QUEUED`（3.1 节）。
3. **唤醒抢占**。`wakeup_preempt()` 判断被唤醒者是否应抢占目标 CPU 上的当前任务：所属调度类更高时直接 `resched_curr()`，同类时交给调度类判断（3.1 节）。
4. **最后才置 `TASK_RUNNING`**。`ttwu_do_wakeup()`（[`kernel/sched/core.c:3695-3699`](../../linux/kernel/sched/core.c#L3695-L3699)）写入 `TASK_RUNNING` 并触发 `sched_wakeup` 跟踪点。在此之前，`TASK_WAKING` 一直标示"唤醒进行中"。

[`kernel/sched/core.c:3727-3748`](../../linux/kernel/sched/core.c#L3727-L3748)（v6.18.60）

```c
	if (p->sched_class->task_woken) {
		/*
		 * Our task @p is fully woken up and running; so it's safe to
		 * drop the rq->lock, hereafter rq is only used for statistics.
		 */
		rq_unpin_lock(rq, rf);
		p->sched_class->task_woken(rq, p);
		rq_repin_lock(rq, rf);
	}

	if (rq->idle_stamp) {
		u64 delta = rq_clock(rq) - rq->idle_stamp;
		u64 max = 2*rq->max_idle_balance_cost;

		update_avg(&rq->avg_idle, delta);

		if (rq->avg_idle > max)
			rq->avg_idle = max;

		rq->idle_stamp = 0;
	}
}
```

5. **`task_woken()` 回调**。任务已完全入队，调度类可以在这里做唤醒之后的工作，例如把任务推到其他 CPU。由于回调可能临时释放 rq 锁，调用前后要解除并恢复锁的 pin 标记。
6. **更新平均空闲时长**。CPU 即将空闲、尝试 newidle 负载均衡时会记录 `idle_stamp`。若它非零，说明这次唤醒结束了一段空闲：用 `update_avg()`（[`kernel/sched/sched.h:239-244`](../../linux/kernel/sched/sched.h#L239-L244)，每次向新样本靠近 1/8）把这段时长计入 `avg_idle`，并以 `2 * max_idle_balance_cost` 封顶。newidle 均衡据此判断"预计空闲的时间是否值得付出一次均衡的开销"（3.9 节）。

## 关键细节 / 设计考量

### 尽量只取一把 rq 锁

把各条路径获取的 rq 锁汇总如下：

| 情形 | 唤醒者获取的 rq 锁 |
|---|---|
| 唤醒当前任务 | 无 |
| 任务仍在队列上（`ttwu_runnable()`） | 任务所在的 rq，不可避免：必须与 `__schedule()` 的出队串行化 |
| 走唤醒队列 | 无，由目标 CPU 获取本地 rq 锁 |
| 直接入队（`ttwu_queue()`） | 目标 rq |
| 迁移且任务带有 PSI 状态 | 额外短暂获取旧 rq（`psi_ttwu_dequeue()`，源码注释称之为 "much sadness"） |

睡眠任务不属于任何运行队列，这是唤醒路径能"只取一把锁"的前提：选核和修改 `task_cpu(p)` 只需要 `pi_lock`。

### 为什么在唤醒时决定放置

唤醒是任务迁移代价最低的时刻：任务不在任何运行队列上，`set_task_cpu()` 无需同时持有新旧两个 rq 锁；任务也没有在运行，不需要 stopper 线程把它从 CPU 上赶下来。相比之下，负载均衡迁移一个排队中的任务，需要锁住源 rq、标记 `TASK_ON_RQ_MIGRATING`、出队，再锁住目标 rq 入队（3.1 节、3.9 节）。因此 Linux 把大部分放置决策放在 fork、exec、唤醒这三个时刻，周期性的负载均衡只负责纠正长期失衡。

唤醒选核也有例外：前文 `on_cpu` 仍为 1 时委托给原 CPU 的唤醒不经过选核。这是让唤醒者尽快返回与放置质量之间的取舍，放置不佳的任务由后续的负载均衡纠正。

### TASK_WAKING 窗口

从唤醒者写入 `TASK_WAKING` 到 `ttwu_do_wakeup()` 写入 `TASK_RUNNING` 之间，任务处于一个特殊窗口：既不在任何运行队列上，又不能再被唤醒；走唤醒队列时，连 `pi_lock` 也已经释放。其他需要"钉住"任务的路径必须识别这个窗口：

- `set_task_cpu()` 允许对 `TASK_WAKING` 任务调用，这是唤醒迁移的合法通道。
- `__task_needs_rq_lock()`（[`kernel/sched/core.c:4318-4348`](../../linux/kernel/sched/core.c#L4318-L4348)）在状态为 `TASK_RUNNING` 或 `TASK_WAKING` 时要求额外获取 rq 锁，因为唤醒者可能已经释放 `pi_lock` 而入队尚未完成。`task_call_func()` 依赖它在固定的状态下检查任务。
- 修改 CPU 亲和性时，若任务处于 `TASK_WAKING`，`affine_move_task()` 把迁移交给 stopper 线程处理（[`kernel/sched/core.c:3010`](../../linux/kernel/sched/core.c#L3010)）；`migration_cpu_stop()` 在检查任务位置之前先调用 `flush_smp_call_function_queue()`，把挂起的唤醒处理完（[`kernel/sched/core.c:2557-2562`](../../linux/kernel/sched/core.c#L2557-L2562)）。

### 伪唤醒不可避免

前面多处提到伪唤醒的来源：唤醒方的 `TASK_RUNNING` 写入可能覆盖等待方下一轮的睡眠状态；`wake_q` 登记与真正唤醒之间存在时间差，被登记的任务可能早已因别的原因醒来并进入下一次睡眠；排他等待中，被唤醒者可能发现资源已被其他任务抢走。调度器不保证"被唤醒即条件成立"，这一责任留给等待方的条件循环。

### 唤醒延迟的构成

从 `sched_waking` 跟踪点到被唤醒任务真正在 CPU 上运行，延迟由几部分组成：

1. 唤醒路径本身，可能包括在 `on_cpu` 上的自旋；
2. 走唤醒队列时 IPI 的投递，以及目标 CPU 关中断区段造成的推迟；
3. 目标 CPU 从深度空闲状态退出的时间；
4. 在运行队列中等待被选中的时间，取决于调度类的策略和 `wakeup_preempt()` 的判断；
5. 已请求重调度时，等到下一个抢占点的时间，取决于抢占模型（3.5 节）。

下面的实验可以分别观察其中几项。

## 动手实验

以下实验在 **dev 虚拟机**中进行，需要 **root 权限**。`perf bench sched pipe` 让两个进程通过管道来回传递数据，可以稳定地产生大量唤醒，用作负载。

1. 区分唤醒在哪里完成。`sched_waking` 在唤醒者一侧触发，`sched_wakeup` 在 `ttwu_do_wakeup()` 中触发，二者的 CPU 不同，说明入队是在目标 CPU 上经唤醒队列完成的：

   ```bash
   # 虚拟机，root；另开一个终端运行 perf bench sched pipe -l 1000000
   bpftrace -e '
   tracepoint:sched:sched_waking { @wcpu[args.pid] = cpu + 1; }
   tracepoint:sched:sched_wakeup /@wcpu[args.pid]/ {
       $w = @wcpu[args.pid] - 1;
       if ($w != cpu) {
           @path["wake list: activated on target CPU"] = count();
       } else if (args.target_cpu == cpu) {
           @path["local: wakee stays on waker CPU"] = count();
       } else {
           @path["direct: waker locked remote rq"] = count();
       }
       delete(@wcpu[args.pid]);
   }
   interval:s:10 { exit(); }'
   ```

   然后关闭 `TTWU_QUEUE` 重复测量，"wake list" 一项应基本消失；测完恢复原值。若内核开启了 `CONFIG_PREEMPT_RT`，该特性默认就是关闭的，可反过来打开它对比：

   ```bash
   # 虚拟机，root
   tr ' ' '\n' < /sys/kernel/debug/sched/features | grep TTWU_QUEUE   # 查看当前值
   echo NO_TTWU_QUEUE > /sys/kernel/debug/sched/features              # 关闭
   echo TTWU_QUEUE > /sys/kernel/debug/sched/features                 # 开启
   ```

2. 统计 IPI 与免 IPI 唤醒。`sched_wake_idle_without_ipi` 在目标 CPU 正在轮询、省去 IPI 时触发；`call_function_single_entry` 是 x86 上跨核单播调用 IPI 的入口（也包括唤醒以外的跨核调用）。对比两个进程分散运行与限定在同一个 CPU 上的差别：

   ```bash
   # 虚拟机，root
   EVENTS=sched:sched_waking,sched:sched_wakeup,sched:sched_wake_idle_without_ipi,irq_vectors:call_function_single_entry
   perf stat -a -e $EVENTS -- perf bench sched pipe -l 200000
   perf stat -a -e $EVENTS -- taskset -c 0 perf bench sched pipe -l 200000
   grep CAL /proc/interrupts   # 各 CPU 累计的 Function call interrupts
   ```

3. 唤醒延迟分布（从 `sched_waking` 到被唤醒者切入 CPU，单位 µs）。被唤醒时仍在运行的任务不会立即发生切换，会带来少量偏大的样本：

   ```bash
   # 虚拟机，root
   bpftrace -e '
   tracepoint:sched:sched_waking { @ts[args.pid] = nsecs; }
   tracepoint:sched:sched_switch /@ts[args.next_pid]/ {
       @lat_us = hist((nsecs - @ts[args.next_pid]) / 1000);
       delete(@ts[args.next_pid]);
   }
   interval:s:10 { exit(); }'
   ```

4. 查看单个任务的唤醒统计（[`kernel/sched/debug.c:1196-1200`](../../linux/kernel/sched/debug.c#L1196-L1200)）。`nr_wakeups_local` / `nr_wakeups_remote` 是相对唤醒者 CPU 而言的，`nr_wakeups_migrate` 对应 `WF_MIGRATED`，`nr_wakeups_sync` 对应 `WF_SYNC`。同一位置输出的其他 `nr_wakeups_*` 字段由调度类维护，这里不讨论：

   ```bash
   # 虚拟机，root
   sysctl kernel.sched_schedstats=1
   sleep 10
   grep -E '^nr_wakeups(_sync|_migrate|_local|_remote)?\s' /proc/$(pgrep -o systemd-journal)/sched
   sysctl kernel.sched_schedstats=0
   ```

   `/proc/schedstat` 中每个 `cpuN` 行的第 5、6 个字段分别是该 CPU 的 `ttwu_count` 与 `ttwu_local`（`linux/Documentation/scheduler/sched-stats.rst`）。

5. 观察 fork 均衡。`sched_wakeup_new` 在父进程上下文中、`wake_up_new_task()` 选核之后触发，比较当前 CPU 与 `target_cpu` 即可知道子进程是否被放到了别的 CPU：

   ```bash
   # 虚拟机，root
   bpftrace -e '
   tracepoint:sched:sched_wakeup_new {
       if (args.target_cpu == cpu) { @fork["child placed on parent CPU"] = count(); }
       else { @fork["child placed on another CPU"] = count(); }
   }
   interval:s:5 { exit(); }' &
   sleep 1; for i in $(seq 200); do /bin/true; done; wait
   ```

## 小结

- 唤醒是一次状态匹配：`try_to_wake_up()` 只唤醒 `__state` 与给定掩码相交的任务；`TASK_NEW` 与 `TASK_WAKING` 不匹配任何常规掩码，从而挡住了不合时宜的唤醒。
- 睡眠 / 唤醒协议是一个 SB 模式：`set_current_state()` 的 `smp_store_mb()` 与 `try_to_wake_up()` 的 `smp_mb__after_spinlock()` 配对，避免丢失唤醒；等待方必须用条件循环容忍伪唤醒，`set_special_state()` 用 `pi_lock` 保护不能容忍覆盖的状态。
- 新任务经 `sched_fork()` 以 `TASK_NEW` 初始化，由 `sched_cgroup_fork()` 确定任务组和临时 CPU，最后由 `wake_up_new_task()` 以 `WF_FORK` 选核、以 `ENQUEUE_INITIAL` 第一次入队。
- `try_to_wake_up()` 以 `pi_lock` 串行化唤醒，按 `on_rq` / `on_cpu` 分情形处理：仍在队列上只改状态；已出队则置 `TASK_WAKING`，等待或委托切换完成，再选核、入队，最后才置 `TASK_RUNNING`。
- 核心层的 `select_task_rq()` 只在任务有选择余地时询问调度类，并统一用 `is_cpu_allowed()` / `select_fallback_rq()` 保证结果合法；`set_task_cpu()` 只允许对可运行或 `TASK_WAKING` 的任务调用。
- fork、exec、唤醒是三个放置时机；唤醒时任务不在任何队列上，只需 `pi_lock` 即可迁移，代价最低。
- `TTWU_QUEUE` 唤醒队列把跨 LLC 或面向空闲 CPU 的入队委托给目标 CPU，经一次 IPI（或对轮询 idle 免 IPI）在 `sched_ttwu_pending()` 中批量完成，减少 rq 锁与运行队列数据的跨核传递。

## 参考

- [`kernel/sched/core.c:4039-4120`](../../linux/kernel/sched/core.c#L4039-L4120)：迁移与唤醒中程序序保证的源码注释。
- [`include/linux/sched.h:201-237`](../../linux/include/linux/sched.h#L201-L237)：`set_current_state()` 与睡眠协议的源码注释。
- [`include/linux/sched/wake_q.h:5-33`](../../linux/include/linux/sched/wake_q.h#L5-L33)：`wake_q` 的使用约定。
- `linux/Documentation/memory-barriers.txt`："SLEEP AND WAKE-UP FUNCTIONS" 一节。
- `linux/Documentation/scheduler/sched-stats.rst`：schedstats 各字段含义。
- 7.3 节"存储缓冲"与"锁与 `smp_mb__after_spinlock()`"。
