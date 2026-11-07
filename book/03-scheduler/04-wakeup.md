# 3.4 任务创建与唤醒

> **本节源码**：[`kernel/sched/core.c`](../../linux/kernel/sched/core.c)、[`kernel/sched/sched.h`](../../linux/kernel/sched/sched.h)、[`kernel/sched/wait.c`](../../linux/kernel/sched/wait.c)、[`kernel/smp.c`](../../linux/kernel/smp.c)、[`kernel/fork.c`](../../linux/kernel/fork.c)、[`include/linux/sched.h`](../../linux/include/linux/sched.h)、[`include/linux/sched/wake_q.h`](../../linux/include/linux/sched/wake_q.h)
> **关键函数**：`sched_fork()`、`sched_cgroup_fork()`、`wake_up_new_task()`、`try_to_wake_up()`、`ttwu_runnable()`、`select_task_rq()`、`ttwu_queue()`、`sched_ttwu_pending()`、`ttwu_do_activate()`
> **前置知识**：[3.1 调度器框架](01-overview.md)（调度类、rq 锁与 `pi_lock`）、[3.2 主调度函数与上下文切换](02-schedule.md)（阻塞出队与切换收尾）、[3.3 EEVDF](03-eevdf.md)（延迟出队）

## 概述

任务进入可运行集合有两种主要情形：新任务完成创建后的首次入队，以及已有任务等待的事件发生后的唤醒。[3.2 节](02-schedule.md)说明了任务怎样阻塞和让出 CPU，本节沿相反方向分析调度器如何接收任务、确定目标 CPU，并恢复其可运行状态。难点不在于向运行队列插入一个节点，而在于唤醒到来时，等待方可能尚未睡眠、正在出队，或者仍在旧 CPU 上完成切换。

本节以 x86_64、`CONFIG_SMP=y`、`CONFIG_PREEMPT_RT=n`、`CONFIG_SCHED_PROXY_EXEC=n` 的[仓库配置](../../linux/.config)为主线，先分析首次入队，再沿普通睡眠任务的唤醒路径解释状态交接；冻结、延迟出队与跨 CPU 委托在相应位置说明。核心层负责并发协议和 CPU 合法性，调度类负责选核、队列组织与抢占策略，公平类的具体实现留到 [3.12 节](12-fair-class.md)。**唤醒成功表示调度器接收了这次唤醒，不表示目标任务已经开始执行，也不保证它等待的资源仍然可用。**

## 核心数据结构

### 三个维度：状态、队列归属与执行占用

理解唤醒首先要区分 `__state`、`on_rq`、`on_cpu`。它们分别回答「任务以什么状态等待」「任务是否仍归某个运行队列管理」「旧 CPU 是否还在执行或收尾」，并不是同一个状态的三种写法。

| 字段 | 含义及有效值 | 源码 |
|---|---|---|
| `__state` | 状态位掩码；`TASK_RUNNING` 为 0，其他相关值见下文。等待方设置睡眠状态时，任务可能仍在 CPU 上运行 | [`include/linux/sched.h:823`](../../linux/include/linux/sched.h#L823) |
| `on_rq` | 0 表示已脱离运行队列；`TASK_ON_RQ_QUEUED` 表示在队列上；`TASK_ON_RQ_MIGRATING` 表示运行队列之间的迁移过渡，不能当成普通布尔值使用 | [`include/linux/sched.h:859`](../../linux/include/linux/sched.h#L859)、[`kernel/sched/sched.h:97-98`](../../linux/kernel/sched/sched.h#L97-L98) |
| `on_cpu` | 1 表示 CPU 正在运行该任务或尚未完成其切换收尾；直到 `finish_task()` 才以 release 语义清零 | [`include/linux/sched.h:844`](../../linux/include/linux/sched.h#L844)、[`kernel/sched/core.c:4968-4982`](../../linux/kernel/sched/core.c#L4968-L4982) |
| `saved_state` | 锁等待或冻结期间暂存的原状态；普通唤醒可以只更新它，而不立即恢复任务运行 | [`include/linux/sched.h:826`](../../linux/include/linux/sched.h#L826) |
| `pi_lock` | 串行化同一任务的唤醒，并稳定选核需要的调度类、亲和性与任务组信息 | [`include/linux/sched.h:1229`](../../linux/include/linux/sched.h#L1229)、[`kernel/sched/core.c:4138-4144`](../../linux/kernel/sched/core.c#L4138-L4144) |

`TASK_RUNNING` 表示可运行，不等于正在运行；反过来，睡眠状态也不意味着任务已经离开 CPU。尤其是公平类的延迟出队（Delayed Dequeue），允许任务暂时保留 `on_rq == TASK_ON_RQ_QUEUED`，却因 `se.sched_delayed` 而不作为普通可运行任务执行。这一例外决定了唤醒不能只检查 `__state` 后盲目入队。

本节使用的状态定义如下。`TASK_NORMAL` 是唤醒接口使用的匹配掩码，不是另一种独立的睡眠状态。

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
#define TASK_KILLABLE			(TASK_WAKEKILL | TASK_UNINTERRUPTIBLE)
...
#define TASK_IDLE			(TASK_UNINTERRUPTIBLE | TASK_NOLOAD)
...
#define TASK_NORMAL			(TASK_INTERRUPTIBLE | TASK_UNINTERRUPTIBLE)
```

`TASK_INTERRUPTIBLE` 允许信号打断等待；`TASK_UNINTERRUPTIBLE` 不响应普通信号唤醒；`TASK_KILLABLE` 在不可中断状态上增加 `TASK_WAKEKILL`，允许致命信号打断。信号路径如何构造唤醒掩码，见 [`kernel/signal.c:745-760`](../../linux/kernel/signal.c#L745-L760) 与 [`include/linux/sched/signal.h:466-474`](../../linux/include/linux/sched/signal.h#L466-L474)。`TASK_NEW` 表示新任务尚未完成首次激活，`TASK_WAKING` 表示已有唤醒者接管了任务、入队尚在进行。后两者不匹配 `TASK_NORMAL`，因此常规 `wake_up_process()` 不会把它们重复入队；状态宏与接口实现分别见上面的定义及 [`kernel/sched/core.c:4438-4447`](../../linux/kernel/sched/core.c#L4438-L4447)。

### 放置、投递与记账字段

任务的当前 CPU、唤醒选核的起点以及执行入队工作的 CPU 需要分别看待。`task_cpu(p)` 标识任务当前归属；`wake_cpu` 是下一次唤醒选核的起点，`__set_task_cpu()` 会同步更新它，但 NUMA 交换路径也可能单独修改它，不能始终将二者视为相同值（[`kernel/sched/sched.h:2214-2227`](../../linux/kernel/sched/sched.h#L2214-L2227)、[`kernel/sched/core.c:3373-3383`](../../linux/kernel/sched/core.c#L3373-L3383)，后一行为依赖当前已开启的 `CONFIG_NUMA_BALANCING`）。

| 对象与字段 | 在唤醒中的作用 | 源码 |
|---|---|---|
| `task_struct::nr_cpus_allowed`、`cpus_ptr` | 亲和性约束与可选 CPU 范围 | [`include/linux/sched.h:917-918`](../../linux/include/linux/sched.h#L917-L918) |
| `task_struct::wake_entry` | 向目标 CPU 投递异步唤醒时使用的跨核调用节点 | [`include/linux/sched.h:845`](../../linux/include/linux/sched.h#L845) |
| `task_struct::sched_remote_wakeup` | 向接收方传递本次唤醒是否迁移，即 `WF_MIGRATED`，不是「是否远程唤醒」的通用标志 | [`include/linux/sched.h:981-996`](../../linux/include/linux/sched.h#L981-L996) |
| `task_struct::wake_q` | 调用者暂存待唤醒任务的节点；与 `wake_entry` 用途不同 | [`include/linux/sched.h:1231`](../../linux/include/linux/sched.h#L1231) |
| `rq::ttwu_pending` | 本 CPU 有待处理的异步唤醒，是提示位而非任务计数 | [`kernel/sched/sched.h:1137`](../../linux/kernel/sched/sched.h#L1137) |
| `sched_contributes_to_load`、`rq::nr_uninterruptible` | 记录本次阻塞是否贡献不可中断负载；单个 rq 的增减可以不配对，统计需看全局汇总 | [`include/linux/sched.h:974`](../../linux/include/linux/sched.h#L974)、[`kernel/sched/sched.h:1162-1168`](../../linux/kernel/sched/sched.h#L1162-L1168) |
| `in_iowait`、`rq::nr_iowait` | 标识 I/O 等待并记录其计数；唤醒时必须从原来记账的 rq 撤销 | [`include/linux/sched.h:1006`](../../linux/include/linux/sched.h#L1006)、[`kernel/sched/sched.h:1198`](../../linux/kernel/sched/sched.h#L1198) |

阻塞时是否计入 `nr_uninterruptible`，由 `try_to_block_task()` 根据 `TASK_UNINTERRUPTIBLE`、`TASK_NOLOAD`、`TASK_FROZEN` 决定；只有真正执行 `__block_task()` 才增加计数。因此不能把「设置了不可中断状态」直接等同于「计数已经增加」（[`kernel/sched/core.c:6568-6571`](../../linux/kernel/sched/core.c#L6568-L6571)、[`kernel/sched/sched.h:2769-2810`](../../linux/kernel/sched/sched.h#L2769-L2810)）。

### 唤醒标志与入队标志

唤醒标志描述放置的原因和提示，入队标志描述调度类应如何接收任务。二者不是同一套位定义，不能直接互换。

| 唤醒标志 | 语义 | 在本节路径中的后续作用 |
|---|---|---|
| `WF_FORK`、`WF_EXEC`、`WF_TTWU` | 首次入队、exec 放置、普通唤醒 | 告知调度类本次选核的时机 |
| `WF_SYNC` | 唤醒者预计很快睡眠 | 供选核策略参考，并计入同步唤醒统计；不强制同 CPU 运行 |
| `WF_CURRENT_CPU` | 倾向选择唤醒者当前 CPU | 仅为策略提示，不绕过亲和性约束 |
| `WF_RQ_SELECTED` | 核心包装函数调用过调度类的 `select_task_rq()` | 直接激活路径转换为 `ENQUEUE_RQ_SELECTED` |
| `WF_MIGRATED` | 唤醒过程中改变了任务 CPU | 转换为 `ENQUEUE_MIGRATED`，并避免重复撤销旧 rq 的 I/O 等待计数 |

定义见 [`kernel/sched/sched.h:2327-2339`](../../linux/kernel/sched/sched.h#L2327-L2339)。普通唤醒激活使用 `ENQUEUE_WAKEUP`，新任务使用 `ENQUEUE_INITIAL`，撤销延迟出队使用 `ENQUEUE_DELAYED`；这些标志定义于 [`kernel/sched/sched.h:2389-2400`](../../linux/kernel/sched/sched.h#L2389-L2400)。异步唤醒队列只传递 `WF_MIGRATED`，其余标志不会完整地带到接收 CPU，这一点将在跨核路径中解释。

## 主流程分析

### 调用链总览

首次入队和普通唤醒最终都要进入调度类队列，但二者不共享入口。下面的调用链只保留决定状态和队列归属的节点；虚线箭头表示跨 CPU 的异步交接，而不是普通函数调用。未单独标注文件的核心层函数均位于 [`kernel/sched/core.c`](../../linux/kernel/sched/core.c)。

```text
新任务
kernel_clone()                                  kernel/fork.c
├── copy_process()                              kernel/fork.c
│   ├── sched_fork()                            kernel/sched/core.c
│   ├── sched_cgroup_fork()                      kernel/sched/core.c
│   └── sched_post_fork()                        kernel/sched/core.c
└── wake_up_new_task()                           kernel/sched/core.c
    ├── select_task_rq() → __set_task_cpu()
    └── activate_task() → wakeup_preempt()

已有任务
wake_up_process() / default_wake_function()      kernel/sched/core.c
└── try_to_wake_up()                             kernel/sched/core.c
    ├── ttwu_state_match()                      匹配状态、认领唤醒
    ├── [仍在 rq] ttwu_runnable()               修复状态，不重复激活
    └── [已出队] TASK_WAKING
        ├── [仍在旧 CPU] ttwu_queue_wakelist()  尝试委托原 CPU
        └── 等待 on_cpu == 0
            ├── select_task_rq() → set_task_cpu()
            └── ttwu_queue()
                ├── 直接：ttwu_do_activate()
                └── 委托：__smp_call_single_queue()  kernel/smp.c
                    ⇢ sched_ttwu_pending()     kernel/sched/core.c
                      └── ttwu_do_activate()
                          ├── activate_task() → class->enqueue_task()
                          ├── wakeup_preempt()
                          └── ttwu_do_wakeup()  写入 TASK_RUNNING
```

阅读普通唤醒时，需要始终追踪三个交接点：状态匹配确定这次唤醒是否需要处理；`on_rq == 0` 证明必须重新入队；`on_cpu == 0` 证明旧 CPU 已完成执行交接。异步委托可以改变由谁等待和入队，但不能取消这些约束。

### 新任务：从 TASK_NEW 到首次入队

#### 初始化不能等同于发布可运行状态

`copy_process()` 先复制任务描述符，再由 `sched_fork()` 重置调度状态。`__sched_fork()` 清零 `on_rq`、执行时间、迁移次数等字段，初始化各调度类实体，并把 `wake_entry` 类型设为 `CSD_TYPE_TTWU`；子任务不能继承父任务在运行队列上的位置或正在运行的事实（[`kernel/sched/core.c:4456-4508`](../../linux/kernel/sched/core.c#L4456-L4508)）。

`sched_fork()` 随后设置 `TASK_NEW`，并取父任务的 `normal_prio`，避免继承优先级继承机制带来的临时提升。这里最重要的顺序是：**先处理 fork 时重置策略的请求，再判断子任务是否仍具有 Deadline 优先级。**

[`kernel/sched/core.c:4695-4742`](../../linux/kernel/sched/core.c#L4695-L4742)（v6.18.60，节选）

```c
int sched_fork(u64 clone_flags, struct task_struct *p)
{
	__sched_fork(clone_flags, p);
	...
	p->__state = TASK_NEW;
	...
	p->prio = current->normal_prio;

	uclamp_fork(p);
	...
	if (unlikely(p->sched_reset_on_fork)) {
		...
	}

	if (dl_prio(p->prio))
		return -EAGAIN;
	...
```

因此，「Deadline 任务不允许 fork」是不完整的结论。若未请求重置且子任务仍具有 Deadline 优先级，`sched_fork()` 返回 `-EAGAIN`；若设置了 `sched_reset_on_fork`，代码先把相应子任务的策略重置为 `SCHED_NORMAL`，这项检查便不会因此失败。之后函数选择调度类、初始化负载跟踪，并置 `on_cpu = 0`（[`kernel/sched/core.c:4716-4764`](../../linux/kernel/sched/core.c#L4716-L4764)）。

任务组固定后，`sched_cgroup_fork()` 在 `pi_lock` 下设置 `sched_task_group`，以 `__set_task_cpu()` 把初始 CPU 暂定为当前 CPU，再调用调度类的 `task_fork()`。任务组设置依赖当前已开启的 `CONFIG_CGROUP_SCHED`；调用点位于任务对外发布之前（[`kernel/fork.c:2293-2302`](../../linux/kernel/fork.c#L2293-L2302)、[`kernel/sched/core.c:4766-4795`](../../linux/kernel/sched/core.c#L4766-L4795)）。这个初始 CPU 为类回调提供上下文，不是最终放置结果。

`TASK_NEW` 使普通信号等常规唤醒不能把尚未完成初始化的任务提前激活。若创建在 `sched_fork()` 成功之后失败，回收路径经过 `sched_cancel_fork()`；该函数负责撤销 sched_ext 的 fork 状态，尚未入队的普通调度实体随任务对象一起清理，不需要先从 rq 出队（[`kernel/fork.c:2482-2483`](../../linux/kernel/fork.c#L2482-L2483)、[`kernel/sched/core.c:4797-4806`](../../linux/kernel/sched/core.c#L4797-L4806)）。当前配置未启用 `CONFIG_SCHED_CLASS_EXT`，这部分扩展调度器工作不生效。

#### 首次激活直接使用 TASK_RUNNING

`kernel_clone()` 在 `copy_process()` 成功后调用 `wake_up_new_task()`（[`kernel/fork.c:2645`](../../linux/kernel/fork.c#L2645)）。此时子任务从未运行，`on_rq`、`on_cpu` 均为 0，无须等待旧 CPU 收尾；整个选核和入队过程由 `pi_lock` 串行化，因此可以直接写 `TASK_RUNNING`，不必经过 `TASK_WAKING`。

[`kernel/sched/core.c:4831-4856`](../../linux/kernel/sched/core.c#L4831-L4856)（v6.18.60，节选）

```c
void wake_up_new_task(struct task_struct *p)
{
	struct rq_flags rf;
	struct rq *rq;
	int wake_flags = WF_FORK;

	raw_spin_lock_irqsave(&p->pi_lock, rf.flags);
	WRITE_ONCE(p->__state, TASK_RUNNING);
	...
	p->recent_used_cpu = task_cpu(p);
	rseq_migrate(p);
	__set_task_cpu(p, select_task_rq(p, task_cpu(p), &wake_flags));
	rq = __task_rq_lock(p, &rf);
	update_rq_clock(rq);
	post_init_entity_util_avg(p);

	activate_task(rq, p, ENQUEUE_NOCLOCK | ENQUEUE_INITIAL);
	trace_sched_wakeup_new(p);
	wakeup_preempt(rq, p, wake_flags);
	...
```

以 `WF_FORK` 进行的选核称为 fork 均衡（Fork Balancing）。它推迟到首次激活前执行，因为创建过程中亲和性可能改变，CPU 也可能下线。这里使用 `__set_task_cpu()` 而不是 `set_task_cpu()`，避免对尚未完成建立的任务调用 `migrate_task_rq()`；目标 rq 加锁后才初始化利用率估计，并用 `ENQUEUE_INITIAL` 告诉调度类这是第一次入队。

`activate_task()` 调用 `enqueue_task()` 后写入 `TASK_ON_RQ_QUEUED`，`wakeup_preempt()` 再判断是否需要重调度。若调度类实现了 `task_woken()`，核心层还会调用它，然后释放 rq 锁与 `pi_lock`（[`kernel/sched/core.c:2143-2154`](../../linux/kernel/sched/core.c#L2143-L2154)、[`kernel/sched/core.c:4857-4867`](../../linux/kernel/sched/core.c#L4857-L4867)）。至此任务只是具备被选中的资格；第一次真正执行仍要经过 [3.2 节的首次调度路径](02-schedule.md#新任务的第一次调度)。

### 普通唤醒的前提：先登记等待，再检查条件

普通任务的唤醒入口需要接收目标任务、允许匹配的状态掩码和唤醒提示。`wake_up_process()` 传入 `TASK_NORMAL`；`wake_up_state()` 使用调用者指定的掩码；`wake_up()` 系列则先遍历等待队列（Wait Queue），再通过回调定位任务。它们不能替代等待条件本身的同步：调度器知道任务是否愿意睡眠，却不知道管道是否有数据、锁是否已经可取。

避免丢失唤醒（Lost Wakeup）的关键，是等待方必须在最后一次检查条件之前设置睡眠状态。下面按源码注释改写为协议伪代码，只展示状态与条件的顺序；实际条件还必须使用所属子系统规定的锁或并发访问原语。

依据 [`include/linux/sched.h:201-237`](../../linux/include/linux/sched.h#L201-L237)（v6.18.60，伪代码）

```text
等待方                                  唤醒方
循环：                                  使等待条件成立
    设置睡眠状态                        wake_up_process(p)
    完整内存屏障                            获取 p->pi_lock
    检查条件                                完整内存屏障
    若成立则退出                            检查 p->__state
    schedule()                              按状态完成唤醒
恢复 TASK_RUNNING
```

等待方的完整屏障来自 `set_current_state()` 的 `smp_store_mb()`；唤醒方在获取 `pi_lock` 后执行 `smp_mb__after_spinlock()`。单独的 acquire 加锁不足以约束锁前的条件写入与锁后的状态读取，因此需要后者。两个屏障排除「等待方仍看到条件为假，同时唤醒方仍看到 `TASK_RUNNING`」的结果：要么等待方发现条件成立而不睡，要么唤醒方能接收其睡眠状态。配对位置见 [`include/linux/sched.h:245-250`](../../linux/include/linux/sched.h#L245-L250)、[`kernel/sched/core.c:4191-4200`](../../linux/kernel/sched/core.c#L4191-L4200)，内存模型分析见 [7.3 节](../07-sync/03-memory-model.md)。

使用等待队列时，还必须让唤醒方能找到任务。`___wait_event()` 先执行 `prepare_to_wait_event()`，后检查条件；后者在队列锁下加入等待项并设置任务状态。退出时，`finish_wait()` 恢复 `TASK_RUNNING`，移除仍留在队列中的等待项；默认自动移除回调也会在唤醒成功时摘除节点（[`include/linux/wait.h:307-335`](../../linux/include/linux/wait.h#L307-L335)、[`kernel/sched/wait.c:290-324`](../../linux/kernel/sched/wait.c#L290-L324)、[`kernel/sched/wait.c:384-409`](../../linux/kernel/sched/wait.c#L384-L409)）。这完成了等待项从登记到回收的生命周期，不能仅凭一次状态写入代替。

被唤醒后仍须重新检查条件。资源可能被其他任务先取走，也可能发生伪唤醒（Spurious Wakeup）；状态写入冲突最多让规范的等待循环多执行一轮，而不应被解释为资源所有权已经转交。若条件修改、检查和唤醒由同一把锁串行化，则可使用无完整屏障的 `__set_current_state()`，completion 就是在 `x->wait.lock` 下建立这一约束（[`kernel/sched/completion.c:21-31`](../../linux/kernel/sched/completion.c#L21-L31)、[`kernel/sched/completion.c:85-110`](../../linux/kernel/sched/completion.c#L85-L110)）。

### 第一步：匹配状态并认领唤醒

`try_to_wake_up(p, state, wake_flags)` 用 `guard(preempt)()` 禁止当前执行上下文被抢占，并添加 `WF_TTWU`。对于 `p != current` 的主路径，它以关本地中断的方式获取 `p->pi_lock`，先完成屏障配对，再检查任务状态。

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
		...
```

`pi_lock` 串行化不同 CPU 对同一任务的唤醒，也使 `sched_class`、`cpus_ptr`、`sched_task_group` 在选核期间保持稳定；它不替代 rq 锁，队列成员关系仍须在对应 rq 锁下修改。该函数返回 1 表示唤醒已被接收，返回 0 表示没有匹配到请求的状态，后者不能简单解释为「任务正在运行」：掩码与睡眠类型不匹配时也会返回 0。

`ttwu_state_match()` 的三种结果需要区分（[`kernel/sched/core.c:2236-2246`](../../linux/kernel/sched/core.c#L2236-L2246)、[`kernel/sched/core.c:4007-4037`](../../linux/kernel/sched/core.c#L4007-L4037)）：

| 匹配结果 | 动作 | 对调用者的结果 |
|---|---|---|
| `__state & state` 非零 | 继续处理可运行状态或入队 | `success = 1` |
| 仅 `saved_state & state` 非零 | 将 `saved_state` 改成 `TASK_RUNNING`，不立即激活任务 | `success = 1`，但不触发 `sched_waking` |
| 两者都不匹配 | 不改变任务 | 返回 0 |

第二种情况用于保存不能立即兑现的唤醒。当前启用 `CONFIG_FREEZER=y`：可冻结任务被置为 `TASK_FROZEN` 时，原状态保存在 `saved_state`；普通唤醒只留下 `TASK_RUNNING` 记录。解冻时，若该记录表明期间发生过唤醒，`__thaw_task()` 再以 `TASK_FROZEN` 真正唤醒任务；否则恢复原睡眠状态（[`kernel/freezer.c:109-145`](../../linux/kernel/freezer.c#L109-L145)、[`kernel/freezer.c:190-208`](../../linux/kernel/freezer.c#L190-L208)）。

另一种用途是 PREEMPT_RT 的可睡眠自旋锁替代实现：任务临时等待 `TASK_RTLOCK_WAIT`，普通唤醒修改 `saved_state`，获得锁后再恢复。该分支依赖当前未启用的 `CONFIG_PREEMPT_RT`（[`include/linux/sched.h:269-314`](../../linux/include/linux/sched.h#L269-L314)）。因此，返回成功、产生 `sched_waking`、重新入队并不是三个等价的事件。

`p == current` 则走独立的短路径。例如，任务设置睡眠状态后被本地中断打断，中断处理函数恰好唤醒它。由于当前任务还没有完成阻塞出队，代码匹配状态后直接执行 `ttwu_do_wakeup()`，不获取 `pi_lock` 或 rq 锁；同一 CPU 的执行顺序以及特殊状态设置时的关中断约束提供必要串行化（[`kernel/sched/core.c:4159-4189`](../../linux/kernel/sched/core.c#L4159-L4189)）。这条路径再次说明：一次成功唤醒不一定对应一次出队、入队或上下文切换。

### 第二步：仍在运行队列时修复状态

状态匹配之后，`smp_rmb()` 保证先读 `__state`、再读 `on_rq`。若次序颠倒，唤醒者可能读到上一次入队之前的旧 `on_rq == 0`，却同时读到任务再次运行后设置的新睡眠状态，从而错误地等待 `on_cpu` 清零。源码用与 `__schedule()` 的锁和屏障配对解释了这个约束（[`kernel/sched/core.c:4204-4228`](../../linux/kernel/sched/core.c#L4204-L4228)）。

读到 `on_rq` 非零还不足以直接写状态，`ttwu_runnable()` 必须获取任务所在 rq 的锁并复查，以便与阻塞出队或队列迁移串行化。

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

这个分支覆盖三类情形：任务刚设置睡眠状态但尚未出队；任务设置状态后因抢占而留在队列中；公平类已标记延迟出队但尚未真正移除任务。前两者无需再次激活，第三者则要用 `ENQUEUE_DELAYED` 撤销推迟状态。若任务此刻没有占用 CPU，还要检查它是否应抢占当前任务；最后统一恢复 `TASK_RUNNING`。

rq 锁下的复查是正确性的关键。若获取锁期间任务已经出队，函数返回 0，外层继续完整的重新入队路径；若仍在队列上，则在同一临界区中恢复状态，阻止调度器随后根据过时的睡眠意图把它移除。这里不能用「读到 `on_rq` 非零，所以任务一定不需要入队」概括整个分支。

### 第三步：已出队时接管任务并等待执行交接

走到这里，唤醒方已经确认任务不在运行队列上。接下来的 `smp_acquire__after_ctrl_dep()` 与出队方对 `on_rq` 的 release 写配合，保证阻塞记账及出队决定先完成；此时唤醒方才可以把状态改为 `TASK_WAKING`。但出队并不代表旧 CPU 已停止使用任务：在常规阻塞路径中，`on_rq` 先清零，`on_cpu` 要到切换收尾才清零。

[`kernel/sched/core.c:4253-4295`](../../linux/kernel/sched/core.c#L4253-L4295)（v6.18.60，节选）

```c
		smp_acquire__after_ctrl_dep();
		...
		WRITE_ONCE(p->__state, TASK_WAKING);
		...
		if (smp_load_acquire(&p->on_cpu) &&
		    ttwu_queue_wakelist(p, task_cpu(p), wake_flags))
			break;
		...
		smp_cond_load_acquire(&p->on_cpu, !VAL);
```

`TASK_WAKING` 表示这次唤醒已经被认领。即使随后把入队委托给另一个 CPU、并释放 `pi_lock`，其他常规唤醒也不会再次匹配该任务。它不是「仍在睡眠」的同义词，而是一种跨执行上下文交接的标记。

若 `on_cpu` 仍为 1，代码先尝试把唤醒投递给任务原 CPU，而不是立刻自旋。原 CPU 此时在关中断的调度区段中，唤醒 IPI 只能在切换收尾之后处理，因此它可以在完成旧任务交接后自行重新入队。读取 `on_cpu` 的 acquire 语义还约束后续 `task_cpu(p)` 的读取，避免向更早的旧 CPU 投递。这条捷径**不经过重新选核**；队列使用条件不满足时，才由唤醒者等待 `on_cpu == 0`（[`kernel/sched/core.c:4263-4295`](../../linux/kernel/sched/core.c#L4263-L4295)）。

等待路径的 `smp_cond_load_acquire()` 与 `finish_task()` 的 `smp_store_release(&prev->on_cpu, 0)` 配对。它保证旧 CPU 对任务的执行和收尾先于后续在新 CPU 上的执行，维护任务跨 CPU 的程序序（Program Order）。两个变量承担不同责任：`on_rq` 交接队列和阻塞状态，`on_cpu` 交接执行上下文；只等待其中一个不足以允许重新运行。release 写和完整论证分别见 [`kernel/sched/sched.h:2769-2810`](../../linux/kernel/sched/sched.h#L2769-L2810)、[`kernel/sched/core.c:4968-4982`](../../linux/kernel/sched/core.c#L4968-L4982)、[`kernel/sched/core.c:4039-4120`](../../linux/kernel/sched/core.c#L4039-L4120)。

### 第四步：选定合法 CPU 并迁移记账

确认任务已脱离旧 CPU 后，普通路径以 `p->wake_cpu` 为起点调用核心层的 `select_task_rq()`。这里不是每次都调用调度类：只有任务允许在多个 CPU 上运行且没有禁止迁移时，才询问类回调；否则直接从 `cpus_ptr` 取得 CPU。

[`kernel/sched/core.c:3582-3608`](../../linux/kernel/sched/core.c#L3582-L3608)（v6.18.60，节选）

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
	...
	if (unlikely(!is_cpu_allowed(p, cpu)))
		cpu = select_fallback_rq(task_cpu(p), p);

	return cpu;
}
```

核心层对结果再次检查合法性。普通用户任务要求目标 CPU 处于 active 状态；禁止迁移的任务和 per-CPU 内核线程使用 online 判据；普通内核线程还需排除正在下线的 CPU。检查同时包含任务的 CPU 允许范围（[`kernel/sched/core.c:2435-2459`](../../linux/kernel/sched/core.c#L2435-L2459)）。

如果结果不合法，`select_fallback_rq()` 先在原 NUMA 节点、再在允许掩码中搜索，找不到时尝试 cpuset 回退及 `task_cpu_fallback_mask()`。这是没有合法 CPU 时的恢复机制，不是调度类可以任意违反用户亲和性的许可；提示信息也只有在相应回退、任务存在 `mm` 且通过速率限制时才打印（[`kernel/sched/core.c:3508-3577`](../../linux/kernel/sched/core.c#L3508-L3577)）。

目标 CPU 与 `task_cpu(p)` 不同时，必须先处理旧 rq 上的状态，再公布新归属。

[`kernel/sched/core.c:4297-4310`](../../linux/kernel/sched/core.c#L4297-L4310)（v6.18.60）

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
```

I/O 等待计数必须从原 rq 减去，因此发生在修改 CPU 之前。当前启用了 `CONFIG_PSI=y`；当 PSI 在运行时有效且任务保留了相应状态时，`psi_ttwu_dequeue()` 还会短暂获取旧 rq 锁，撤销睡眠期间保留的 PSI 状态（[`kernel/sched/stats.h:199-216`](../../linux/kernel/sched/stats.h#L199-L216)）。这解释了「唤醒尽量只取一把 rq 锁」为何是优化目标，而不是无条件成立的规则。

`set_task_cpu()` 随后调用调度类的 `migrate_task_rq()`、增加迁移计数并通知相关子系统，最后通过 `__set_task_cpu()` 更新组调度指针和 CPU 编号。该底层函数先执行 `set_task_rq()`，再以写屏障保证任务数据更新先于新 CPU 编号可见（[`kernel/sched/core.c:3302-3354`](../../linux/kernel/sched/core.c#L3302-L3354)、[`kernel/sched/sched.h:2214-2227`](../../linux/kernel/sched/sched.h#L2214-L2227)）。

这里的前提是任务已经处于 `TASK_WAKING`、已出队且完成旧 CPU 交接。不能据此推导出「持有 `pi_lock` 就能迁移任意任务」：普通阻塞任务的放置要留给唤醒路径，已经排队的任务则需遵守 rq 锁及 `TASK_ON_RQ_MIGRATING` 协议；`set_task_cpu()` 对这些状态另有检查，公平类的可运行迁移还有专门约束。

### 第五步：在目标运行队列上激活

`ttwu_queue()` 决定由哪个 CPU 执行入队。先看不使用异步唤醒队列的分支：唤醒者仍持有 `pi_lock`，获取目标 rq 锁、更新时钟，再调用 `ttwu_do_activate()`；目标 rq 可以是本地也可以是远程（[`kernel/sched/core.c:3975-3987`](../../linux/kernel/sched/core.c#L3975-L3987)）。

激活阶段的顺序是先撤销阻塞记账，再入队并判断抢占，最后发布 `TASK_RUNNING`。

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

`nr_uninterruptible` 在目标 rq 上递减，而睡眠时可能在另一个 rq 上递增，所以单个 CPU 的值不能解释为该 CPU 当前拥有的不可中断任务数。`nr_iowait` 的规则不同：若发生迁移，旧 rq 的减计数已在上一步完成，本函数借助 `WF_MIGRATED` 避免重复扣减；若没有迁移，则在这里撤销。

`activate_task()` 调用调度类后将 `on_rq` 置为 `TASK_ON_RQ_QUEUED`。在当前未启用代理执行的配置下，`wakeup_preempt()` 比较目标任务与当前任务的调度类：更高类直接请求重调度，同类则交给类回调判断。它并不直接执行上下文切换（[`kernel/sched/core.c:2143-2154`](../../linux/kernel/sched/core.c#L2143-L2154)、[`kernel/sched/core.c:2219-2234`](../../linux/kernel/sched/core.c#L2219-L2234)）。

`ttwu_do_wakeup()` 最后写入 `TASK_RUNNING` 并触发 `sched_wakeup`。此后若有 `task_woken()` 回调，核心层解除 rq 锁的 pin 标记、调用回调后重新 pin；解除 pin 本身不是解锁，而是允许回调按其约定临时释放 rq 锁。函数还会更新 `avg_idle` 等空闲统计，供后续均衡使用（[`kernel/sched/core.c:3695-3699`](../../linux/kernel/sched/core.c#L3695-L3699)、[`kernel/sched/core.c:3727-3748`](../../linux/kernel/sched/core.c#L3727-L3748)）。

### 跨 CPU 委托：由目标 CPU 批量入队

#### 何时值得委托

直接远程入队需要唤醒者访问目标 rq 的锁和队列数据。`TTWU_QUEUE` 提供另一条路径：唤醒者只投递任务，目标 CPU 自行取得本地 rq 锁完成激活。这里的唤醒队列（Wake List）是基于每 CPU `call_single_queue` 的待激活任务链表，不是等待条件上的等待队列，也不是调用者局部的 `wake_q`。

非 PREEMPT_RT 配置下，`TTWU_QUEUE` 默认开启；PREEMPT_RT 下默认关闭，但不是编译时禁止（[`kernel/sched/features.h:74-83`](../../linux/kernel/sched/features.h#L74-L83)）。`ttwu_queue_cond()` 还要依次排除不允许排队的 sched_ext 任务、stop 类、非 active CPU 和亲和性不允许的目标，然后才检查以下条件（[`kernel/sched/core.c:3915-3973`](../../linux/kernel/sched/core.c#L3915-L3973)）：

| 唤醒者与目标的关系 | 是否使用唤醒队列 |
|---|---|
| 不共享末级缓存（Last Level Cache，LLC） | 使用，减少这次激活对远程 rq 数据的访问 |
| 目标就是当前 CPU | 不使用，本地直接入队 |
| 不同 CPU、共享 LLC，目标 `nr_running == 0` | 使用，把激活工作交给空闲或即将空闲的 CPU |
| 不同 CPU、共享 LLC，目标 `nr_running != 0` | 不使用，由唤醒者直接处理 |

`nr_running == 0` 不等于 CPU 已进入 idle：最后一个任务已经出队但尚在切换收尾时也满足它。前述 `on_cpu == 1` 的捷径复用同一个判据，满足条件就委托原 CPU，否则等待旧执行结束再选核。收益是减少唤醒者的远程访问或自旋；代价是引入投递、目标 CPU 响应以及批处理等待，不能据此无条件声称唤醒延迟更低。

#### 投递不等于立即发送一个 IPI

唤醒者先保存 `WF_MIGRATED`，再置 `ttwu_pending`，最后发布 `wake_entry`。

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

`__smp_call_single_queue()` 用 `llist_add()` 将节点挂到目标 CPU 的 `call_single_queue`，仅在链表原先为空时才尝试发送处理器间中断（Inter-Processor Interrupt，IPI）。因此多个唤醒可以共用一次通知，队列中也可能混有其他跨核回调（[`kernel/smp.c:380-414`](../../linux/kernel/smp.c#L380-L414)）。

发送之前，`call_function_single_prep_ipi()` 还会检查目标 idle 线程是否设置了轮询标志。如果是，`set_nr_if_polling()` 设置 `TIF_NEED_RESCHED` 就足以让它退出轮询，无须真正发送 IPI；idle 路径在调度前主动清理跨核调用队列（[`kernel/sched/core.c:3844-3852`](../../linux/kernel/sched/core.c#L3844-L3852)、[`kernel/sched/idle.c:367-379`](../../linux/kernel/sched/idle.c#L367-L379)）。

在 x86_64 上，实际投递走单播跨核调用 IPI，由 `sysvec_call_function_single()` 进入 `generic_smp_call_function_single_interrupt()`。这与直接远程入队后可能请求的重调度 IPI 不同，不能仅凭看到 `RES` 计数就判断走了唤醒队列（[`arch/x86/kernel/smp.c:248-273`](../../linux/arch/x86/kernel/smp.c#L248-L273)、[`kernel/smp.c:461-464`](../../linux/kernel/smp.c#L461-L464)）。

#### 接收方消费 TASK_WAKING

接收方从跨核调用队列中摘取一批节点，先处理同步调用，再处理非 TTWU 的其他节点，最后把剩余 `CSD_TYPE_TTWU` 节点交给 `sched_ttwu_pending()`（[`kernel/smp.c:480-595`](../../linux/kernel/smp.c#L480-L595)）。除 IPI 处理和 idle 外，迁移线程也可能主动清理队列，因此「异步委托」不等于「必然在 IPI 处理函数中执行」。

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

接收方为这一批任务统一获取 rq 锁并更新时钟，逐个调用的仍是前文 `ttwu_do_activate()`。如果类回调内部临时解锁，不能把它理解为从头到尾绝不释放锁；批处理减少的是外层重复加锁和更新时间的工作。

这条路径只从 `sched_remote_wakeup` 恢复 `WF_MIGRATED`。`WF_SYNC` 等只在发送方有机会影响选核或统计，`WF_RQ_SELECTED` 不会在接收方恢复为 `ENQUEUE_RQ_SELECTED`。因此不能假定直接入队与委托入队传给调度类的标志完全一致。

`ttwu_pending` 在批量激活之后才清零，使 `idle_cpu()` 不会在「已承诺接收任务、尚未入队」的窗口中把目标错误地视为空闲；它不是链表长度，也不能用于推导精确积压数量（[`kernel/sched/syscalls.c:191-205`](../../linux/kernel/sched/syscalls.c#L191-L205)）。任务一旦发布，接收方就可能并发消费，甚至在发送方释放 `pi_lock` 之前完成入队；协议要求的是接收方无须持有发送方的 `pi_lock`，并不规定二者固定的完成先后次序。

## 关键细节 / 设计考量

### 等待队列、wake_q 与唤醒队列的分工

三种队列出现在同一条唤醒链上，却解决不同问题。

| 机制 | 组织对象与保护方式 | 解决的问题 |
|---|---|---|
| 等待队列 | 条件对象的 `wait_queue_head`，队列锁保护等待项和遍历 | 哪些任务在等这个事件 |
| `wake_q` | 调用者局部的 `wake_q_head`，每任务节点原子认领 | 把实际唤醒移出调用者的临界区 |
| TTWU 唤醒队列 | 每 CPU 的 `call_single_queue`，以无锁链表发布 `wake_entry` | 由哪个 CPU 完成运行队列激活 |

`wake_up()` 在等待队列锁下调用各等待项的回调，默认路径经 `default_wake_function()` 进入 `try_to_wake_up()`（[`kernel/sched/core.c:7273-7278`](../../linux/kernel/sched/core.c#L7273-L7278)）。排他等待（Exclusive Wait）用于限制被选中的排他等待者数量，避免一次事件唤醒大量竞争者造成惊群（Thundering Herd）。`wake_up()` 的排他名额为 1，只有回调成功且条目标有 `WQ_FLAG_EXCLUSIVE` 才消耗名额；`wake_up_all()` 传入 0，不按这个名额停止（[`include/linux/wait.h:221-231`](../../linux/include/linux/wait.h#L221-L231)、[`kernel/sched/wait.c:93-147`](../../linux/kernel/sched/wait.c#L93-L147)）。

非排他等待者不消耗名额，但只能保证遍历所经过的条目会被处理，不能保证任何排列下都唤醒全部非排他等待者：优先级排他项可以位于队首，成功后提前结束遍历。源码注释明确指出了这一边界（[`kernel/sched/wait.c:80-92`](../../linux/kernel/sched/wait.c#L80-L92)）。另外，等待队列允许自定义回调，不能把所有回调成功都解释为一次普通任务入队。

`wake_q` 是唤醒批处理队列（Wake Queue），把操作拆成「登记」和「真正唤醒」。`__wake_q_add()` 以原子比较交换认领任务唯一的 `wake_q` 节点；登记成功时 `wake_q_add()` 获取任务引用，`wake_up_q()` 清空节点链接、调用 `wake_up_process()`，最后释放引用。若调用者已经持有引用，可使用 `wake_q_add_safe()` 转交它；重复登记失败时由该接口释放多余引用（[`kernel/sched/core.c:1019-1104`](../../linux/kernel/sched/core.c#L1019-L1104)）。

这种拆分可以缩短锁内工作，并减少被唤醒者立刻争用尚未释放之锁的机会，但它不承诺唤醒一定等到本调用者解锁之后才发生。若任务已被另一个 `wake_q` 认领，另一个执行者可能立即唤醒它，所以调用 `wake_q_add()` 时任务就必须已经具备可唤醒条件；每个已登记队列都必须最终由 `wake_up_q()` 消费（[`include/linux/sched/wake_q.h:5-33`](../../linux/include/linux/sched/wake_q.h#L5-L33)）。

### 屏障不是同一种保证的重复叠加

唤醒路径上的屏障分别保护不同交接，不能简单合并为「保证可见性」。

| 交接关系 | 主要配对 | 防止的错误 |
|---|---|---|
| 条件发布与睡眠状态检查 | `set_current_state()` 与 `pi_lock` 后的完整屏障 | 两侧都读到旧值而丢失唤醒 |
| 状态检查与队列位置读取 | `__state` 后的 `smp_rmb()`，配合调度路径锁与屏障 | 把新睡眠状态与旧的未入队状态拼成错误快照 |
| 阻塞出队与唤醒接管 | `on_rq` 的 release 写与控制依赖后的 acquire | 在出队、记账完成前就迁移或重新激活 |
| 旧 CPU 收尾与新 CPU 执行 | `on_cpu` 的 release 清零与 acquire 等待 | 任务仍被旧 CPU 使用时就到新 CPU 运行 |

这些关系的直接依据是 [`kernel/sched/core.c:4191-4295`](../../linux/kernel/sched/core.c#L4191-L4295) 及 [`kernel/sched/sched.h:2769-2810`](../../linux/kernel/sched/sched.h#L2769-L2810)。它们约束调度状态交接，不替代等待条件所关联数据的锁、引用和发布协议。

普通等待循环可以容忍状态写入被并发唤醒覆盖；特殊状态（Special State）如 `TASK_DEAD`、`TASK_PARKED` 不采用普通条件循环，不能依赖「再循环一次」恢复。`set_special_state()` 因而在关中断并持有 `pi_lock` 时写状态，与唤醒串行化；冻结路径也通过相应锁协议写 `TASK_FROZEN`（[`include/linux/sched.h:156-162`](../../linux/include/linux/sched.h#L156-L162)、[`include/linux/sched.h:252-267`](../../linux/include/linux/sched.h#L252-L267)、[`kernel/freezer.c:109-151`](../../linux/kernel/freezer.c#L109-L151)）。

### TASK_WAKING 不等于始终不在队列上

`TASK_WAKING` 从确认出队后设置，持续到 `ttwu_do_wakeup()` 写 `TASK_RUNNING`。在窗口前半段任务尚未入队；在窗口后半段，`activate_task()` 已把 `on_rq` 设为 `TASK_ON_RQ_QUEUED`，状态仍可能暂时是 `TASK_WAKING`，但这段过程由 rq 锁保护。因此不能把它定义成「既不在队列上又不能被唤醒」的固定组合。

这也解释了 `task_call_func()` 为什么不能只持 `pi_lock` 就认为任务被固定：异步接收方可能正在 rq 锁下入队。`__task_needs_rq_lock()` 遇到 `TASK_RUNNING` 或 `TASK_WAKING` 都要求获取 rq 锁，才能在一致状态下检查任务（[`kernel/sched/core.c:4318-4391`](../../linux/kernel/sched/core.c#L4318-L4391)）。修改亲和性时，`TASK_WAKING` 也会触发 stopper 协助；迁移线程先清理挂起的跨核调用，再判断任务位置，避免迁移与尚未完成的激活互相越过（[`kernel/sched/core.c:3010`](../../linux/kernel/sched/core.c#L3010)、[`kernel/sched/core.c:2557-2562`](../../linux/kernel/sched/core.c#L2557-L2562)）。

### 放置时机与锁成本

对于已经出队且完成切换的睡眠任务，唤醒选核不需要先从旧 rq 拆除一个可运行节点，也不需要 stopper 抢占它。这减少了与运行中任务迁移相比的同步工作，但不代表任何负载下都是迁移成本最低的时刻，缓存局部性和目标 CPU 响应仍会影响结果。

| 路径 | 唤醒者的主要 rq 锁开销 |
|---|---|
| 唤醒 `current` | 无 |
| `ttwu_runnable()` 成功 | 获取任务所在 rq 锁，修复仍在队列上的状态 |
| 已出队，直接激活 | 获取目标 rq 锁 |
| 已出队，异步委托 | 接收方获取目标 rq 锁，发送方不为该次激活获取它 |
| 检查队列期间失去竞争，或迁移需清理 PSI | 还可能获取旧 rq 锁，不能承诺只取一次 |

fork、唤醒和 exec 都是放置机会，但 exec 不属于唤醒。`sched_exec()` 的调用者就是正在运行的任务，它在 `pi_lock` 下直接询问类回调并传入 `WF_EXEC`；若需要换核且目标 active，再由 `stop_one_cpu()` 请求 stopper 执行 `migration_cpu_stop()`，不能沿睡眠任务路径直接修改 CPU（[`fs/exec.c:1770`](../../linux/fs/exec.c#L1770)、[`kernel/sched/core.c:5458-5479`](../../linux/kernel/sched/core.c#L5458-L5479)）。源码将 exec 视为有效工作集较小的均衡机会，但这不能推广成「所有放置主要发生在这三处」；周期均衡、主动迁移、亲和性修改和 CPU 热插拔仍有各自职责，见 [3.9 节](09-load-balance.md)。

## 可观测性

### 用事件区分接收唤醒、完成激活与选择执行

当前配置启用了 `CONFIG_TRACEPOINTS=y`、`CONFIG_EVENT_TRACING=y`，可经 tracefs 或 perf 观察下列事件。唤醒事件共用 `sched_wakeup_template`，其 `pid` 取目标任务的 `p->pid`，是线程标识而非线程组 TGID；`prio` 是内部调度优先级，`target_cpu` 取事件触发时的 `task_cpu(p)`；它们不等于事件记录头部的当前任务与执行 CPU（[`include/trace/events/sched.h:141-186`](../../linux/include/trace/events/sched.h#L141-L186)）。

| 事件 | 对应源码阶段 | 解读边界 |
|---|---|---|
| `sched:sched_waking` | `try_to_wake_up()` 匹配 `__state` 后，见 [`kernel/sched/core.c:4186`](../../linux/kernel/sched/core.c#L4186)、[`kernel/sched/core.c:4202`](../../linux/kernel/sched/core.c#L4202) | 在唤醒者上下文；尚未执行本次选核，`target_cpu` 不是最终放置结果；仅匹配 `saved_state` 时没有该事件 |
| `sched:sched_wakeup` | `ttwu_do_wakeup()` 写入 `TASK_RUNNING` 后，见 [`kernel/sched/core.c:3695-3699`](../../linux/kernel/sched/core.c#L3695-L3699) | 可能在发送方或接收方；也覆盖仍在 rq 或唤醒 `current` 的路径，不能一律叫作「重新入队完成」 |
| `sched:sched_wakeup_new` | 新任务首次激活后，见 [`kernel/sched/core.c:4854-4856`](../../linux/kernel/sched/core.c#L4854-L4856) | 与普通 `sched_waking` 分开；比较记录 CPU 与 `target_cpu` 可观察首次放置 |
| `sched:sched_switch` | 已选出 `next`、准备切换，见 [`kernel/sched/core.c:6941`](../../linux/kernel/sched/core.c#L6941)、[`include/trace/events/sched.h:222-267`](../../linux/include/trace/events/sched.h#L222-L267) | `next_pid` 表示即将运行的任务；事件仍在真正换栈之前，不是用户代码恢复执行的精确时刻 |
| `sched:sched_migrate_task` | `set_task_cpu()`，见 [`kernel/sched/core.c:3342`](../../linux/kernel/sched/core.c#L3342)、[`include/trace/events/sched.h:272-297`](../../linux/include/trace/events/sched.h#L272-L297) | 用 `orig_cpu`、`dest_cpu` 看迁移；也包括非唤醒迁移，fork 的 `__set_task_cpu()` 不产生该事件 |
| `csd:csd_queue_cpu` | 跨核节点发布前，见 [`kernel/smp.c:389-398`](../../linux/kernel/smp.c#L389-L398)、[`include/trace/events/csd.h:10-35`](../../linux/include/trace/events/csd.h#L10-L35) | `func=sched_ttwu_pending` 表示 TTWU 投递，不证明真正发送了 IPI |
| `csd:csd_function_entry`、`csd:csd_function_exit` | 执行跨核回调，TTWU 调用点见 [`kernel/smp.c:588-594`](../../linux/kernel/smp.c#L588-L594)，定义见 [`include/trace/events/csd.h:40-67`](../../linux/include/trace/events/csd.h#L40-L67) | `func=sched_ttwu_pending` 的区间覆盖一批激活，不能把这一批的耗时归给单个任务 |

对已完整阻塞的任务，可以沿同一 `pid` 区分三个时刻：`sched_waking` 为接收请求，`sched_wakeup` 为恢复可运行状态，后续 `sched_switch` 的 `next_pid` 为选中执行。第一段包含状态交接、选核以及可能的跨核投递；第二段主要反映等待调度、抢占点和策略限制。CPU 空闲退出、关中断及虚拟机宿主调度都可能影响观察到的间隔，单个差值不能直接定位某一种原因。

这一关联有明确边界：唤醒 `current` 时任务无需再切入；任务设置睡眠状态后被抢占，也未必真正阻塞；只更新 `saved_state` 时没有普通唤醒事件。盲目把每个 `sched_waking` 配到下一次同 PID 的 `sched_switch`，会把这些情况误算为很长的唤醒延迟。应结合此前 `sched_switch` 的睡眠状态、后续迁移、事件丢失情况以及线程生命周期判读；同一 PID 可复用，跨 CPU 时间差也要求使用可比较的时钟。

IPI 可作为补充证据，但不是任务级因果证明。`irq_vectors:call_function_single_entry` / `irq_vectors:call_function_single_exit` 观察跨核调用 IPI，`irq_vectors:reschedule_entry` / `irq_vectors:reschedule_exit` 观察重调度 IPI，定义见 [`arch/x86/include/asm/trace/irq_vectors.h:84-98`](../../linux/arch/x86/include/asm/trace/irq_vectors.h#L84-L98)，触发见 [`arch/x86/kernel/smp.c:248-273`](../../linux/arch/x86/kernel/smp.c#L248-L273)。`sched:sched_wake_idle_without_ipi` 则表明目标 idle 正在轮询，可以只置标志；该事件也可来自其他重调度或跨核调用，不专属于 TTWU（[`include/trace/events/sched.h:812-827`](../../linux/include/trace/events/sched.h#L812-L827)、[`kernel/sched/core.c:3844-3852`](../../linux/kernel/sched/core.c#L3844-L3852)）。

### 计数器与配置开关

| 入口 | 从哪里产生、看什么 | 限制与配置 |
|---|---|---|
| `/proc/<pid>/sched` 的 `nr_wakeups`、`nr_wakeups_local`、`nr_wakeups_remote`、`nr_wakeups_migrate`、`nr_wakeups_sync` | `ttwu_stat()` 递增，`proc_sched_show_task()` 输出：[`kernel/sched/core.c:3655-3690`](../../linux/kernel/sched/core.c#L3655-L3690)、[`kernel/sched/debug.c:1174-1200`](../../linux/kernel/sched/debug.c#L1174-L1200) | `CONFIG_PROC_FS=y`、`CONFIG_SCHEDSTATS=y`，还需运行时启用；统计成功接收的普通唤醒，包括只改 `saved_state`，不是入队次数或首次激活次数 |
| `/proc/schedstat` 的 `cpuN` 行 | `show_schedstat()` 输出；去掉 `cpuN` 后，第 5、6 个数为 `ttwu_count`、`ttwu_local`：[`kernel/sched/stats.c:124-130`](../../linux/kernel/sched/stats.c#L124-L130) | 以发起唤醒的 CPU 汇总；同样依赖 schedstats；计数是累计值，应比较采样差值 |
| `/proc/<pid>/sched` 的 `se.nr_migrations` | `set_task_cpu()` 真正改变 CPU 时递增，输出见 [`kernel/sched/core.c:3344-3351`](../../linux/kernel/sched/core.c#L3344-L3351)、[`kernel/sched/debug.c:1172`](../../linux/kernel/sched/debug.c#L1172) | 不依赖 schedstats 运行时开关；包含负载均衡等迁移，首次选核不计入 |
| `/proc/interrupts` 的 `CAL`、`RES` | 分别汇总跨核调用与重调度 IPI；递增见 [`arch/x86/kernel/smp.c:251-272`](../../linux/arch/x86/kernel/smp.c#L251-L272)，输出见 [`arch/x86/kernel/irq.c:106-113`](../../linux/arch/x86/kernel/irq.c#L106-L113) | x86、`CONFIG_SMP=y`、`CONFIG_PROC_FS=y`；包含系统其他活动，不能当成某个负载的唤醒计数 |
| `/sys/kernel/debug/sched/features` | 读取 `TTWU_QUEUE` / `NO_TTWU_QUEUE`，文件创建于 [`kernel/sched/debug.c:495-501`](../../linux/kernel/sched/debug.c#L495-L501)，默认值见 [`kernel/sched/features.h:74-83`](../../linux/kernel/sched/features.h#L74-L83) | `CONFIG_DEBUG_FS=y`，需要挂载 debugfs；写入改变全局调度行为，只在受控实验中使用并恢复原值 |

local / remote 是相对**唤醒者**的 CPU，migrate 是相对**被唤醒任务原来的** CPU，两者不能互相替代。任务可以在原 CPU 被远程唤醒而不迁移，也可以迁到唤醒者 CPU，成为一次 local 且 migrated 的唤醒。统计在 `try_to_wake_up()` 末尾取当时的 `task_cpu(p)`，它也不是一个永久绑定的最终 CPU 记录。

schedstats 的静态键默认关闭，可用 `kernel.sched_schedstats` 或启动参数 `schedstats=enable` 打开；其他分析设施也可能调用 `force_schedstat_enabled()`，实验必须读取并恢复实际原值，而不是假定原值为 0（[`kernel/sched/core.c:4574-4634`](../../linux/kernel/sched/core.c#L4574-L4634)）。本版本调度器直接编译调试输出代码，不应从其他版本照搬 `CONFIG_SCHED_DEBUG` 作为这些接口的前置条件（[`kernel/sched/build_utility.c:71`](../../linux/kernel/sched/build_utility.c#L71)）。

### 动态探针与状态调试

需要区分状态不匹配和成功接收时，可以检查 `try_to_wake_up()` 的返回值；需要确认异步激活时，可以探测 `sched_ttwu_pending()`；需要观察首次激活，则选 `wake_up_new_task()`。当前 `CONFIG_KPROBES=y`、`CONFIG_KPROBE_EVENTS=y`、`CONFIG_FUNCTION_TRACER=y`，但源码中的函数名不保证在运行内核中可探测，应检查实际符号、kprobe 限制及 `available_filter_functions`。该列表说明 ftrace 可跟踪性，不等同于所有 kprobe 可用位置。

`ttwu_state_match()` 是 `__always_inline`，`ttwu_do_wakeup()`、核心层 `select_task_rq()`、`__set_task_cpu()` 是内联辅助函数，不应依赖独立探针入口；`ttwu_runnable()`、`ttwu_do_activate()` 等静态函数也可能被内联或改名，优先使用已有事件观察其效果。当前配置未启用 `CONFIG_DEBUG_INFO_BTF`，不能把依赖运行内核 BTF 的 fentry / fexit 作为默认实验条件。

等待方的状态设置还可由裸跟踪点 `sched_set_state_tp` 观察，其条件只记录运行与非运行状态之间的变化，不导出为 tracefs 事件；唤醒方的 `WRITE_ONCE()` 不经过它（[`include/trace/events/sched.h:890-893`](../../linux/include/trace/events/sched.h#L890-L893)、[`include/linux/sched.h:195-199`](../../linux/include/linux/sched.h#L195-L199)）。当前未启用 `CONFIG_DEBUG_ATOMIC_SLEEP`、`CONFIG_DEBUG_PREEMPT`、`CONFIG_PROVE_LOCKING`；如需检查特殊状态误用或锁顺序，应在独立调试内核中开启相关配置，而不是把这些检查视为默认已有（[`include/linux/sched.h:164-193`](../../linux/include/linux/sched.h#L164-L193)、[`kernel/sched/core.c:4012-4015`](../../linux/kernel/sched/core.c#L4012-L4015)）。

## 动手实验

以下步骤面向 `dev` 虚拟机，以 root 执行，需要安装 perf；最后一个实验还使用 Python 3。先确认运行内核与本节分析版本相符，并检查实际配置和事件可用性。**这些步骤尚未在 dev 虚拟机实测，以下只给出预期现象与解读方法，不提供虚构输出。**采集会增加调度开销；全局特性切换会影响整台虚拟机，不应在生产环境直接执行。

### 实验 1：还原一次阻塞、唤醒与选中执行

使用管道往返负载产生睡眠与唤醒，并同时记录请求、激活、迁移及跨核批处理。`-a` 保留系统范围事件，便于同时看到父子任务和两个 CPU 的执行；分析时再限定目标线程，不要用记录头部的当前 PID 代替事件里的目标 `pid`。

```bash
# 虚拟机，root；文件写入 /tmp，不修改内核源码。
perf list 'sched:*'
perf list 'csd:*'
DATA=$(mktemp /tmp/wakeup-perf.XXXXXX)
perf record -a -o "$DATA" \
    -e sched:sched_waking -e sched:sched_wakeup \
    -e sched:sched_wakeup_new -e sched:sched_switch \
    -e sched:sched_migrate_task \
    -e csd:csd_queue_cpu \
    -e csd:csd_function_entry -e csd:csd_function_exit \
    -- perf bench sched pipe -l 20000
perf script -i "$DATA" > "$DATA.txt"
printf '原始记录：%s\n文本记录：%s.txt\n' "$DATA" "$DATA"
```

从一个负载任务的 `sched_switch` 开始，确认它以睡眠状态切出，再查找同一任务的 `sched_waking`、`sched_wakeup` 和后续 `next_pid`。若 `sched_waking` 与 `sched_wakeup` 的记录 CPU 不同，并且附近存在 `func=sched_ttwu_pending` 的投递和处理，便可结合源码判断这次激活发生了跨 CPU 委托；若两个事件在同一 CPU，仍需用 `target_cpu` 区分本地处理和直接操作远程 rq。

`sched_wakeup_new` 可单独用来分析负载子任务的首次放置；不要期待它前面有一次普通 `sched_waking`。对时间差的分析应检查丢事件告警，必要时缩短负载或增加 perf 缓冲区，并排除没有完整阻塞的样本。分析结束后删除本实验文件：`rm -- "$DATA" "$DATA.txt"`。

### 实验 2：比较 TTWU_QUEUE 与 IPI 计数

下面在子 shell 中保存特性原值，并用退出处理恢复。两轮都统计全机事件，比较的是在相同负载下的系统活动变化，不是任务级精确归因。至少需要两个允许负载运行的在线 CPU，才有观察远程路径的条件。

```bash
# 虚拟机，root；debugfs 应已挂载于 /sys/kernel/debug。
(
    set -eu
    FEATURES=/sys/kernel/debug/sched/features
    OLD=$(tr ' ' '\n' < "$FEATURES" | grep -xE '(NO_)?TTWU_QUEUE')
    trap 'printf "%s\n" "$OLD" > "$FEATURES"' EXIT
    trap 'exit 130' INT
    trap 'exit 143' TERM
    EVENTS=sched:sched_waking,sched:sched_wakeup
    EVENTS=$EVENTS,sched:sched_wake_idle_without_ipi
    EVENTS=$EVENTS,irq_vectors:call_function_single_entry
    EVENTS=$EVENTS,irq_vectors:reschedule_entry
    for MODE in TTWU_QUEUE NO_TTWU_QUEUE; do
        printf '%s\n' "$MODE" > "$FEATURES"
        printf '\n当前特性：%s\n' "$MODE"
        perf stat -a -e "$EVENTS" -- perf bench sched pipe -l 200000
    done
)
```

关闭队列后，新发生的普通唤醒不再通过 TTWU 队列委托，但跨核调用 IPI 不会因此全部消失：系统仍有其他跨核调用，直接远程激活也可能请求重调度。开启队列时，合并通知和 idle 轮询又会使唤醒数大于 IPI 数。短期计数差异受 CPU 拓扑、空闲状态和背景任务影响，不能据一次运行推导性能结论。

可将两轮负载都改为 `taskset -c <允许的CPU编号> perf bench sched pipe -l 200000` 做单 CPU 对照。此时限制的是负载进程，不是整台虚拟机；即使负载自身没有跨 CPU 唤醒，`perf stat -a` 中的 IPI 仍可能非零，不能要求其「应接近 0」。退出处理无法覆盖强制断电或 `SIGKILL`，中断实验后仍应检查特性值是否恢复。

### 实验 3：读取有明确生命周期的任务计数

创建一个短周期睡眠的任务，在它存活期间采样。启用 schedstats 不会补记启用前的历史，因此最好先开统计再创建负载，并在多次采样间比较增量。

```bash
# 虚拟机，root。
(
    set -eu
    OLD=$(sysctl -n kernel.sched_schedstats)
    TARGET=
    cleanup() {
        if [ -n "$TARGET" ]; then
            kill "$TARGET" 2>/dev/null || true
            wait "$TARGET" 2>/dev/null || true
        fi
        sysctl -q -w kernel.sched_schedstats="$OLD"
    }
    trap cleanup EXIT
    trap 'exit 130' INT
    trap 'exit 143' TERM
    sysctl -q -w kernel.sched_schedstats=1
    python3 -c 'import time; [time.sleep(0.01) for _ in range(3000)]' &
    TARGET=$!
    for SAMPLE in 1 2; do
        sleep 1
        grep -E '^nr_wakeups(_sync|_migrate|_local|_remote)?[[:space:]]' \
            "/proc/$TARGET/sched"
    done
)
```

预期 `nr_wakeups` 随睡眠结束而增长，但具体 local / remote、迁移次数由实际调度决定。这里不依赖机器上某个服务恰好存在，也不在实验结束后强制把原本开启的 schedstats 关闭。若计数字段缺失，应检查运行内核配置和运行时开关，而不是把缺失字段解释为计数为 0。

## 小结

- 首次入队和普通唤醒是两条不同路径：新任务由 `TASK_NEW` 保护初始化，再以 `ENQUEUE_INITIAL` 激活；普通任务必须先匹配等待状态并处理已有队列归属。
- 睡眠协议依赖「先设置状态、再检查条件」及配对屏障；等待队列还负责等待项的登记和移除。唤醒不转交资源所有权，等待方必须重新检查条件。
- `__state`、`on_rq`、`on_cpu` 分别描述状态、队列关系和执行占用。仍在队列时修复状态，已出队时以 `TASK_WAKING` 接管，并等待或委托旧 CPU 完成交接。
- 选核策略交给调度类，合法性由核心层检查。唤醒迁移要先撤销旧 rq 的相关记账，再修改 CPU 归属；`pi_lock` 不能代替所有迁移场景中的 rq 锁。
- TTWU 唤醒队列改变的是激活的执行者，最终仍调用同一个激活函数。它可以合并通知或省略 IPI，但增加了异步响应环节，并不无条件降低延迟。
- 观测时应区分接收唤醒、恢复可运行状态和被选中执行；成功返回、唤醒事件、入队次数与 IPI 次数不存在一一对应关系。

本节到 `wakeup_preempt()` 为止只说明何时提出重调度需求；请求何时被响应、tick 与抢占模型如何影响它，继续见 [3.5 调度时钟、无 tick 模式与抢占模型](05-tick-preempt.md)。

## 参考

- [`kernel/sched/core.c:4039-4158`](../../linux/kernel/sched/core.c#L4039-L4158)：跨 CPU 程序序与 `try_to_wake_up()` 的设计约束。
- [`include/linux/sched.h:201-267`](../../linux/include/linux/sched.h#L201-L267)：普通等待循环、状态写入竞争与特殊状态。
- [`include/linux/sched/wake_q.h:5-33`](../../linux/include/linux/sched/wake_q.h#L5-L33)：延后唤醒的生命周期和使用约定。
- [`Documentation/memory-barriers.txt`](../../linux/Documentation/memory-barriers.txt)：SLEEP AND WAKE-UP FUNCTIONS，区分状态协议与调用者的数据同步。
- [`Documentation/scheduler/sched-stats.rst`](../../linux/Documentation/scheduler/sched-stats.rst)：schedstats 格式；字段含义仍需与本版本 `ttwu_stat()`、`show_schedstat()` 对照。
- [`Documentation/trace/ftrace.rst`](../../linux/Documentation/trace/ftrace.rst)、[`Documentation/trace/kprobetrace.rst`](../../linux/Documentation/trace/kprobetrace.rst)：事件时钟、可跟踪函数与 kprobe 事件限制。
