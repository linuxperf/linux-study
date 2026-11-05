# 5.11 处理器间中断与 irq_work

> **本节源码**：[`include/linux/smp_types.h`](../../linux/include/linux/smp_types.h)、[`include/linux/smp.h`](../../linux/include/linux/smp.h)、[`kernel/smp.c`](../../linux/kernel/smp.c)、[`kernel/sched/core.c`](../../linux/kernel/sched/core.c)、[`arch/x86/kernel/smp.c`](../../linux/arch/x86/kernel/smp.c)、[`arch/x86/kernel/apic/ipi.c`](../../linux/arch/x86/kernel/apic/ipi.c)、[`arch/x86/mm/tlb.c`](../../linux/arch/x86/mm/tlb.c)、[`include/linux/irq_work_types.h`](../../linux/include/linux/irq_work_types.h)、[`kernel/irq_work.c`](../../linux/kernel/irq_work.c)、[`arch/x86/kernel/irq_work.c`](../../linux/arch/x86/kernel/irq_work.c)
> **关键函数**：`smp_call_function_single()`、`smp_call_function_many_cond()`、`generic_exec_single()`、`__smp_call_single_queue()`、`__flush_smp_call_function_queue()`、`call_function_single_prep_ipi()`、`native_smp_send_reschedule()`、`irq_work_queue()`、`irq_work_queue_on()`、`irq_work_run()`
> **前置知识**：5.5 中断控制器（发送 IPI）、3.4 任务创建与唤醒、7.3 内存屏障与 Linux 内核内存模型

## 概述

多处理器内核经常需要"让另一个 CPU 做点什么"：通知它重新调度、让它刷新 TLB、在它上面读取某个 per-CPU 寄存器、让它停下来以便关机。x86 上实现这些的硬件手段是**处理器间中断**（IPI）：一个 CPU 写本地 APIC 的 ICR，向目标 CPU 投递一个指定向量的中断（5.5 节）。

在 IPI 之上，内核构建了两层通用设施：

- **跨 CPU 函数调用**（`smp_call_function*()`，`kernel/smp.c`）：把一个函数和参数排入目标 CPU 的无锁队列，再发 IPI 让它执行，调用者可以选择等待完成。
- **irq_work**（`kernel/irq_work.c`）：让代码在 NMI 或关中断等受限上下文中"预约"一段稍后在硬中断上下文中执行的工作，通常是给自己发一个 IPI。

二者共享同一个 per-CPU 队列 `call_single_queue`，调度器的远程唤醒（3.4 节的 wake list）也挂在这个队列上。

## x86 上的 IPI 向量

5.1 节列出了 x86 的系统向量，其中与 IPI 相关的有：

| 向量 | 名称 | 入口 | 用途 | `/proc/interrupts` |
|---|---|---|---|---|
| `0xfd` | `RESCHEDULE_VECTOR` | `sysvec_reschedule_ipi()` | 通知目标 CPU 检查重调度 | `RES` |
| `0xfc` | `CALL_FUNCTION_VECTOR` | `sysvec_call_function()` | 多目标函数调用 | `CAL` |
| `0xfb` | `CALL_FUNCTION_SINGLE_VECTOR` | `sysvec_call_function_single()` | 单目标函数调用 | `CAL` |
| `0xf8` | `REBOOT_VECTOR` | `sysvec_reboot()` | 关机 / 重启时停止其他 CPU | — |
| `0xf6` | `IRQ_WORK_VECTOR` | `sysvec_irq_work()` | irq_work 自 IPI | `IWI` |
| `0x02` | `NMI_VECTOR` | `asm_exc_nmi` | 调用栈回溯、kdump 停机（5.4 节） | `NMI` |

*表 5-21：x86 上的 IPI 向量*

处理函数都很短：

[`arch/x86/kernel/smp.c:244-273`](../../linux/arch/x86/kernel/smp.c#L244-L273)（v6.18.60）

```c
/*
 * Reschedule call back. KVM uses this interrupt to force a cpu out of
 * guest mode.
 */
DEFINE_IDTENTRY_SYSVEC_SIMPLE(sysvec_reschedule_ipi)
{
	apic_eoi();
	trace_reschedule_entry(RESCHEDULE_VECTOR);
	inc_irq_stat(irq_resched_count);
	scheduler_ipi();
	trace_reschedule_exit(RESCHEDULE_VECTOR);
}

DEFINE_IDTENTRY_SYSVEC(sysvec_call_function)
{
	apic_eoi();
	trace_call_function_entry(CALL_FUNCTION_VECTOR);
	inc_irq_stat(irq_call_count);
	generic_smp_call_function_interrupt();
	trace_call_function_exit(CALL_FUNCTION_VECTOR);
}

DEFINE_IDTENTRY_SYSVEC(sysvec_call_function_single)
{
	apic_eoi();
	trace_call_function_single_entry(CALL_FUNCTION_SINGLE_VECTOR);
	inc_irq_stat(irq_call_count);
	generic_smp_call_function_single_interrupt();
	trace_call_function_single_exit(CALL_FUNCTION_SINGLE_VECTOR);
}
```

重调度 IPI 使用 `DEFINE_IDTENTRY_SYSVEC_SIMPLE`（5.2 节表 5-3）：不切换到中断栈，只做最小的 `__irq_enter_raw()`，因为它几乎什么都不做——`scheduler_ipi()` 只是确保 `TIF_NEED_RESCHED` 被注意到，真正的调度发生在 `irqentry_exit()` 中（3.2、3.5 节）。两个函数调用向量最终调用同一个函数（`generic_smp_call_function_interrupt` 是 `generic_smp_call_function_single_interrupt` 的别名，[`include/linux/smp.h:177`](../../linux/include/linux/smp.h#L177)）；区分两个向量只是为了让多目标调用可以使用 APIC 的广播简写。

发送端在 `arch/x86/kernel/apic/ipi.c` 中：

[`arch/x86/kernel/apic/ipi.c:68-80`](../../linux/arch/x86/kernel/apic/ipi.c#L68-L80)（v6.18.60）

```c
void native_smp_send_reschedule(int cpu)
{
	if (unlikely(cpu_is_offline(cpu))) {
		WARN(1, "sched: Unexpected reschedule of offline CPU#%d!\n", cpu);
		return;
	}
	__apic_send_IPI(cpu, RESCHEDULE_VECTOR);
}

void native_send_call_func_single_ipi(int cpu)
{
	__apic_send_IPI(cpu, CALL_FUNCTION_SINGLE_VECTOR);
}
```

`__apic_send_IPI()` 经 static call 调用当前 `struct apic` 驱动的 `send_IPI`（5.5 节）。在 KVM 客户机中，`kvm_setup_pv_ipi()`（[`arch/x86/kernel/kvm.c:654-657`](../../linux/arch/x86/kernel/kvm.c#L654-L657)）把多目标发送替换为 `kvm_send_ipi_mask()`：用一次超级调用（hypercall）把位图交给宿主机，而不是对每个目标写一次 ICR、每次都引起 VM exit。

## 核心数据结构

### call_single_data

跨 CPU 函数调用、irq_work、远程唤醒三者的队列节点共用同一个头部：

[`include/linux/smp_types.h:7-67`](../../linux/include/linux/smp_types.h#L7-L67)（v6.18.60）

```c
enum {
	CSD_FLAG_LOCK		= 0x01,

	IRQ_WORK_PENDING	= 0x01,
	IRQ_WORK_BUSY		= 0x02,
	IRQ_WORK_LAZY		= 0x04, /* No IPI, wait for tick */
	IRQ_WORK_HARD_IRQ	= 0x08, /* IRQ context on PREEMPT_RT */

	IRQ_WORK_CLAIMED	= (IRQ_WORK_PENDING | IRQ_WORK_BUSY),

	CSD_TYPE_ASYNC		= 0x00,
	CSD_TYPE_SYNC		= 0x10,
	CSD_TYPE_IRQ_WORK	= 0x20,
	CSD_TYPE_TTWU		= 0x30,

	CSD_FLAG_TYPE_MASK	= 0xF0,
};
...
struct __call_single_node {
	struct llist_node	llist;
	union {
		unsigned int	u_flags;
		atomic_t	a_flags;
	};
#ifdef CONFIG_64BIT
	u16 src, dst;
#endif
};
```

`u_flags` 的高 4 位是类型，低位是各类型自己的标志。四种类型：

| 类型 | 宿主结构 | 来源 | 执行时机 |
|---|---|---|---|
| `CSD_TYPE_SYNC` | `call_single_data_t` | `smp_call_function*(…, wait=1)` | 最先执行，调用者在等 |
| `CSD_TYPE_ASYNC` | `call_single_data_t` | `smp_call_function*(…, wait=0)`、`smp_call_function_single_async()` | 第二批 |
| `CSD_TYPE_IRQ_WORK` | `struct irq_work` | `irq_work_queue_on()` 跨 CPU 提交 | 第二批 |
| `CSD_TYPE_TTWU` | `task_struct.wake_entry` | `ttwu_queue_wakelist()`（3.4 节） | 最后一起处理 |

*表 5-22：`call_single_queue` 上的四种节点*

[`include/linux/smp.h:23-27`](../../linux/include/linux/smp.h#L23-L27)（v6.18.60）

```c
struct __call_single_data {
	struct __call_single_node node;
	smp_call_func_t func;
	void *info;
};
```

每个 CPU 一个无锁单链表（`llist`）：

[`kernel/smp.c:40-48`](../../linux/kernel/smp.c#L40-L48)（v6.18.60）

```c
struct call_function_data {
	call_single_data_t	__percpu *csd;
	cpumask_var_t		cpumask;
	cpumask_var_t		cpumask_ipi;
};

static DEFINE_PER_CPU_ALIGNED(struct call_function_data, cfd_data);

static DEFINE_PER_CPU_SHARED_ALIGNED(struct llist_head, call_single_queue);
```

`cfd_data` 是多目标调用使用的预分配资源：每个发起者 CPU 为每个目标 CPU 准备一个 csd，避免在关抢占的路径上动态分配内存。

## 主流程分析

### 单目标调用

[`kernel/smp.c:636-692`](../../linux/kernel/smp.c#L636-L692)（v6.18.60）

```c
int smp_call_function_single(int cpu, smp_call_func_t func, void *info,
			     int wait)
{
	call_single_data_t *csd;
	call_single_data_t csd_stack = {
		.node = { .u_flags = CSD_FLAG_LOCK | CSD_TYPE_SYNC, },
	};
	int this_cpu;
	int err;

	/*
	 * Prevent preemption and reschedule on another CPU, as well as CPU
	 * removal. This prevents stopper from running on this CPU, thus
	 * providing mutual exclusion of the below cpu_online() check and
	 * IPI sending ensuring IPI are not missed by CPU going offline.
	 */
	this_cpu = get_cpu();

	/*
	 * Can deadlock when called with interrupts disabled.
	 * We allow cpu's that are not yet online though, as no one else can
	 * send smp call function interrupt to this cpu and as such deadlocks
	 * can't happen.
	 */
	WARN_ON_ONCE(cpu_online(this_cpu) && irqs_disabled()
		     && !oops_in_progress);

	/*
	 * When @wait we can deadlock when we interrupt between llist_add() and
	 * arch_send_call_function_ipi*(); when !@wait we can deadlock due to
	 * csd_lock() on because the interrupt context uses the same csd
	 * storage.
	 */
	WARN_ON_ONCE(!in_task());

	csd = &csd_stack;
	if (!wait) {
		csd = this_cpu_ptr(&csd_data);
		csd_lock(csd);
	}

	csd->func = func;
	csd->info = info;
#ifdef CONFIG_CSD_LOCK_WAIT_DEBUG
	csd->node.src = smp_processor_id();
	csd->node.dst = cpu;
#endif

	err = generic_exec_single(cpu, csd);

	if (wait)
		csd_lock_wait(csd);

	put_cpu();

	return err;
}
```

- **同步调用用栈上的 csd**：调用者会一直等待完成，栈上的结构在此期间有效。异步调用则用一个 per-CPU 的 csd，先 `csd_lock()` 等待它的上一次使用完成。
- **不能在关中断时调用**（同步等待的情形）：两个 CPU 同时关着中断互相发起同步调用，双方都在 `csd_lock_wait()` 中自旋，谁也收不到对方的 IPI——经典的死锁。`WARN_ON_ONCE(irqs_disabled())` 就是为此设置的。
- **只能在进程上下文中调用**：`WARN_ON_ONCE(!in_task())`。注释解释了在中断中调用的两种死锁：同步时可能打断了本 CPU 自己"已入队但未发 IPI"的窗口；异步时与被打断代码竞争同一个 per-CPU csd。

**csd 锁**是"函数已执行完、csd 可以复用"的标志：

[`kernel/smp.c:349-376`](../../linux/kernel/smp.c#L349-L376)（v6.18.60）

```c
static __always_inline void csd_lock_wait(call_single_data_t *csd)
{
	smp_cond_load_acquire(&csd->node.u_flags, !(VAL & CSD_FLAG_LOCK));
}
#endif

static __always_inline void csd_lock(call_single_data_t *csd)
{
	csd_lock_wait(csd);
	csd->node.u_flags |= CSD_FLAG_LOCK;

	/*
	 * prevent CPU from reordering the above assignment
	 * to ->flags with any subsequent assignments to other
	 * fields of the specified call_single_data_t structure:
	 */
	smp_wmb();
}

static __always_inline void csd_unlock(call_single_data_t *csd)
{
	WARN_ON(!(csd->node.u_flags & CSD_FLAG_LOCK));

	/*
	 * ensure we're all done before releasing data:
	 */
	smp_store_release(&csd->node.u_flags, 0);
}
```

`csd_unlock()` 用 release 语义清除标志，`csd_lock_wait()` 用 acquire 语义等待（7.3 节）：等待者看到锁被释放时，一定也能看到目标 CPU 在函数中写入的所有结果。`smp_cond_load_acquire()` 在 x86 上是一个带 `pause` 的自旋循环。本书配置未开启 `CONFIG_CSD_LOCK_WAIT_DEBUG`；开启后，等待超时会打印 `csd: Detected non-responsive CSD lock` 并对目标 CPU 发 NMI 回溯（[`kernel/smp.c:231-318`](../../linux/kernel/smp.c#L231-L318)），是诊断"IPI 丢失"问题的利器。

入队与发送：

[`kernel/smp.c:421-453`](../../linux/kernel/smp.c#L421-L453)（v6.18.60）

```c
static int generic_exec_single(int cpu, call_single_data_t *csd)
{
	/*
	 * Preemption already disabled here so stopper cannot run on this CPU,
	 * ensuring mutually exclusive CPU offlining and last IPI flush.
	 */
	if (cpu == smp_processor_id()) {
		smp_call_func_t func = csd->func;
		void *info = csd->info;
		unsigned long flags;

		/*
		 * We can unlock early even for the synchronous on-stack case,
		 * since we're doing this from the same CPU..
		 */
		csd_lock_record(csd);
		csd_unlock(csd);
		local_irq_save(flags);
		csd_do_func(func, info, NULL);
		csd_lock_record(NULL);
		local_irq_restore(flags);
		return 0;
	}

	if ((unsigned)cpu >= nr_cpu_ids || !cpu_online(cpu)) {
		csd_unlock(csd);
		return -ENXIO;
	}

	__smp_call_single_queue(cpu, &csd->node.llist);

	return 0;
}
```

目标是自己时直接在关中断下调用——保持与远程执行一致的"在中断上下文中执行"的语义。否则：

[`kernel/smp.c:400-414`](../../linux/kernel/smp.c#L400-L414)（v6.18.60）

```c
	/*
	 * The list addition should be visible to the target CPU when it pops
	 * the head of the list to pull the entry off it in the IPI handler
	 * because of normal cache coherency rules implied by the underlying
	 * llist ops.
	 *
	 * If IPIs can go out of order to the cache coherency protocol
	 * in an architecture, sufficient synchronisation should be added
	 * to arch code to make it appear to obey cache coherency WRT
	 * locking and barrier primitives. Generic code isn't really
	 * equipped to do the right thing...
	 */
	if (llist_add(node, &per_cpu(call_single_queue, cpu)))
		send_call_function_single_ipi(cpu);
}
```

**只有当队列从空变为非空时才发 IPI**：`llist_add()` 在加入前队列为空时返回真。如果队列中已有节点，说明已经有 IPI 在途（或目标 CPU 正在处理），它处理时会把新节点一起取走。这把"N 个请求 N 次 IPI"合并成了"一批请求一次 IPI"。注释中关于"IPI 不能先于缓存一致性到达"的说明，对应 5.5 节 x2APIC 的 `weak_wrmsr_fence()`。

#### 空闲 CPU 免 IPI 唤醒

[`kernel/smp.c:114-122`](../../linux/kernel/smp.c#L114-L122)（v6.18.60）

```c
static __always_inline void
send_call_function_single_ipi(int cpu)
{
	if (call_function_single_prep_ipi(cpu)) {
		trace_ipi_send_cpu(cpu, _RET_IP_,
				   generic_smp_call_function_single_interrupt);
		arch_send_call_function_single_ipi(cpu);
	}
}
```

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

如果目标 CPU 处于轮询式 idle（`TIF_POLLING_NRFLAG`，例如用 `mwait` 监视自己的 `thread_info.flags`），只需原子地设置它的 `TIF_NEED_RESCHED`，它就会从 `mwait` 中醒来，无需真正的 IPI。醒来的 idle 任务在调度前调用 `flush_smp_call_function_queue()`（[`kernel/smp.c:610-626`](../../linux/kernel/smp.c#L610-L626)）处理队列。IPI 是一个相对昂贵的操作（在虚拟机中尤甚），这一优化对频繁唤醒空闲 CPU 的工作负载很有价值。

### 多目标调用

[`kernel/smp.c:811-879`](../../linux/kernel/smp.c#L811-L879)（v6.18.60）

```c
	/* Check if we need remote execution, i.e., any CPU excluding this one. */
	if (cpumask_any_and_but(mask, cpu_online_mask, this_cpu) < nr_cpu_ids) {
		cfd = this_cpu_ptr(&cfd_data);
		cpumask_and(cfd->cpumask, mask, cpu_online_mask);
		__cpumask_clear_cpu(this_cpu, cfd->cpumask);

		cpumask_clear(cfd->cpumask_ipi);
		for_each_cpu(cpu, cfd->cpumask) {
			call_single_data_t *csd = per_cpu_ptr(cfd->csd, cpu);

			if (cond_func && !cond_func(cpu, info)) {
				__cpumask_clear_cpu(cpu, cfd->cpumask);
				continue;
			}

			/* Work is enqueued on a remote CPU. */
			run_remote = true;

			csd_lock(csd);
			if (wait)
				csd->node.u_flags |= CSD_TYPE_SYNC;
			csd->func = func;
			csd->info = info;
#ifdef CONFIG_CSD_LOCK_WAIT_DEBUG
			csd->node.src = smp_processor_id();
			csd->node.dst = cpu;
#endif
			trace_csd_queue_cpu(cpu, _RET_IP_, func, csd);

			/*
			 * Kick the remote CPU if this is the first work
			 * item enqueued.
			 */
			if (llist_add(&csd->node.llist, &per_cpu(call_single_queue, cpu))) {
				__cpumask_set_cpu(cpu, cfd->cpumask_ipi);
				nr_cpus++;
				last_cpu = cpu;
			}
		}

		/*
		 * Choose the most efficient way to send an IPI. Note that the
		 * number of CPUs might be zero due to concurrent changes to the
		 * provided mask.
		 */
		if (nr_cpus == 1)
			send_call_function_single_ipi(last_cpu);
		else if (likely(nr_cpus > 1))
			send_call_function_ipi_mask(cfd->cpumask_ipi);
	}

	/* Check if we need local execution. */
	if ((scf_flags & SCF_RUN_LOCAL) && cpumask_test_cpu(this_cpu, mask) &&
	    (!cond_func || cond_func(this_cpu, info))) {
		unsigned long flags;

		local_irq_save(flags);
		csd_do_func(func, info, NULL);
		local_irq_restore(flags);
	}

	if (run_remote && wait) {
		for_each_cpu(cpu, cfd->cpumask) {
			call_single_data_t *csd;

			csd = per_cpu_ptr(cfd->csd, cpu);
			csd_lock_wait(csd);
		}
	}
```

`smp_call_function_many_cond()` 是 `smp_call_function_many()`、`on_each_cpu_mask()`、`on_each_cpu_cond_mask()` 的共同实现。三个阶段：

1. **入队**：为每个目标 CPU 填写一个 csd 并加入它的队列；`cond_func` 可以按 CPU 过滤。只有队列从空变非空的 CPU 才记入 `cpumask_ipi`。
2. **发送**：只有一个 CPU 需要 IPI 时用单目标向量，否则用 `CALL_FUNCTION_VECTOR` 批量发送。`native_send_call_func_ipi()`（[`arch/x86/kernel/apic/ipi.c:82-94`](../../linux/arch/x86/kernel/apic/ipi.c#L82-L94)）在目标恰好是"所有在线 CPU"或"除自己外所有在线 CPU"时使用 APIC 广播简写，一次写 ICR 发给所有人。
3. **本地执行与等待**：在发送 IPI **之后**才在本 CPU 上执行（如果需要），让远程 CPU 与本 CPU 并行执行；最后逐个等待每个远程 csd 解锁。

TLB 远程刷新（TLB shootdown）是它最频繁的使用者之一：

[`arch/x86/mm/tlb.c:1361-1376`](../../linux/arch/x86/mm/tlb.c#L1361-L1376)（v6.18.60）

```c
	/*
	 * If no page tables were freed, we can skip sending IPIs to
	 * CPUs in lazy TLB mode. They will flush the CPU themselves
	 * at the next context switch.
	 *
	 * However, if page tables are getting freed, we need to send the
	 * IPI everywhere, to prevent CPUs in lazy TLB mode from tripping
	 * up on the new contents of what used to be page tables, while
	 * doing a speculative memory access.
	 */
	if (info->freed_tables || mm_in_asid_transition(info->mm))
		on_each_cpu_mask(cpumask, flush_tlb_func, (void *)info, true);
	else
		on_each_cpu_cond_mask(should_flush_tlb, flush_tlb_func,
				(void *)info, 1, cpumask);
}
```

`should_flush_tlb()` 作为 `cond_func` 过滤掉处于惰性 TLB 模式（3.2 节）的 CPU——它们下次切换地址空间时会自行刷新。`munmap()`、`mprotect()` 等改动页表的系统调用在多线程进程中的开销，很大一部分就来自这里的同步 IPI 等待。`/proc/interrupts` 中 `TLB` 一行即为此类 IPI 的计数。

### 接收端：处理队列

[`kernel/smp.c:529-595`](../../linux/kernel/smp.c#L529-L595)（v6.18.60）

```c
	/*
	 * First; run all SYNC callbacks, people are waiting for us.
	 */
	prev = NULL;
	llist_for_each_entry_safe(csd, csd_next, entry, node.llist) {
		/* Do we wait until *after* callback? */
		if (CSD_TYPE(csd) == CSD_TYPE_SYNC) {
			smp_call_func_t func = csd->func;
			void *info = csd->info;

			if (prev) {
				prev->next = &csd_next->node.llist;
			} else {
				entry = &csd_next->node.llist;
			}

			csd_lock_record(csd);
			csd_do_func(func, info, csd);
			csd_unlock(csd);
			csd_lock_record(NULL);
		} else {
			prev = &csd->node.llist;
		}
	}

	if (!entry)
		return;

	/*
	 * Second; run all !SYNC callbacks.
	 */
	prev = NULL;
	llist_for_each_entry_safe(csd, csd_next, entry, node.llist) {
		int type = CSD_TYPE(csd);

		if (type != CSD_TYPE_TTWU) {
			if (prev) {
				prev->next = &csd_next->node.llist;
			} else {
				entry = &csd_next->node.llist;
			}

			if (type == CSD_TYPE_ASYNC) {
				smp_call_func_t func = csd->func;
				void *info = csd->info;

				csd_lock_record(csd);
				csd_unlock(csd);
				csd_do_func(func, info, csd);
				csd_lock_record(NULL);
			} else if (type == CSD_TYPE_IRQ_WORK) {
				irq_work_single(csd);
			}

		} else {
			prev = &csd->node.llist;
		}
	}

	/*
	 * Third; only CSD_TYPE_TTWU is left, issue those.
	 */
	if (entry) {
		csd = llist_entry(entry, typeof(*csd), node.llist);
		csd_do_func(sched_ttwu_pending, entry, csd);
	}
}
```

`__flush_smp_call_function_queue()`（[`kernel/smp.c:480-595`](../../linux/kernel/smp.c#L480-L595)）先 `llist_del_all()` 一次取走整条链表，`llist_reverse_order()` 恢复先进先出顺序，然后分三遍处理：

1. **同步调用优先**：有 CPU 在自旋等待它们，越早完成越好。注意顺序：**先执行函数，再解锁**——解锁就是告诉等待者"完成了"。
2. **异步调用与 irq_work**：异步调用**先解锁再执行**——csd 中的 `func` 与 `info` 已复制到局部变量，提前解锁让发起者可以尽早复用 csd。
3. **远程唤醒**：剩下的全是 `CSD_TYPE_TTWU` 节点，一次性交给 `sched_ttwu_pending()` 批量把任务加入本 CPU 的运行队列（3.4 节）。

一个 IPI 因此可以同时完成多个函数调用、irq_work 与任务唤醒。

## irq_work

### 动机

有些代码运行在极受限的上下文中，例如 NMI（5.4 节）、持有调度器 rq 锁时、printk 内部。它们需要触发一些后续动作——唤醒一个任务、执行一次 `printk` 输出、发起 RCU 处理——但在当前上下文中直接做这些事会死锁。irq_work 提供了一个 **NMI 安全**的方法："请在下一次（尽快到来的）硬中断上下文中调用这个函数"。

[`include/linux/irq_work_types.h:8-12`](../../linux/include/linux/irq_work_types.h#L8-L12)（v6.18.60）

```c
struct irq_work {
	struct __call_single_node	node;
	void				(*func)(struct irq_work *);
	struct rcuwait			irqwait;
};
```

### 提交

[`kernel/irq_work.c:54-128`](../../linux/kernel/irq_work.c#L54-L128)（v6.18.60）

```c
/*
 * Claim the entry so that no one else will poke at it.
 */
static bool irq_work_claim(struct irq_work *work)
{
	int oflags;

	oflags = atomic_fetch_or(IRQ_WORK_CLAIMED | CSD_TYPE_IRQ_WORK, &work->node.a_flags);
	/*
	 * If the work is already pending, no need to raise the IPI.
	 * The pairing smp_mb() in irq_work_single() makes sure
	 * everything we did before is visible.
	 */
	if (oflags & IRQ_WORK_PENDING)
		return false;
	return true;
}
...
/* Enqueue on current CPU, work must already be claimed and preempt disabled */
static void __irq_work_queue_local(struct irq_work *work)
{
	struct llist_head *list;
	bool rt_lazy_work = false;
	bool lazy_work = false;
	int work_flags;

	work_flags = atomic_read(&work->node.a_flags);
	if (work_flags & IRQ_WORK_LAZY)
		lazy_work = true;
	else if (IS_ENABLED(CONFIG_PREEMPT_RT) &&
		 !(work_flags & IRQ_WORK_HARD_IRQ))
		rt_lazy_work = true;

	if (lazy_work || rt_lazy_work)
		list = this_cpu_ptr(&lazy_list);
	else
		list = this_cpu_ptr(&raised_list);

	if (!llist_add(&work->node.llist, list))
		return;

	/* If the work is "lazy", handle it from next tick if any */
	if (!lazy_work || tick_nohz_tick_stopped())
		irq_work_raise(work);
}

/* Enqueue the irq work @work on the current CPU */
bool irq_work_queue(struct irq_work *work)
{
	/* Only queue if not already pending */
	if (!irq_work_claim(work))
		return false;

	/* Queue the entry and raise the IPI if needed. */
	preempt_disable();
	__irq_work_queue_local(work);
	preempt_enable();

	return true;
}
```

整个提交路径只使用原子操作和无锁链表，**不获取任何锁、不关中断**，因此可以在 NMI 中安全调用：

- `irq_work_claim()`：用一次 `atomic_fetch_or()` 同时置位 `PENDING | BUSY` 和类型。若原来就是 `PENDING`，说明已在队列中，直接返回假——与 tasklet 的 SCHED 位相同的"调度多次执行一次"语义。
- 普通 irq_work 进入 per-CPU 的 `raised_list`，队列从空变非空时 `irq_work_raise()` 发一个 IPI 给**自己**；`IRQ_WORK_LAZY` 进入 `lazy_list`，不发 IPI，等下一个 tick 处理（tick 已停止时除外）。

x86 的自 IPI 实现：

[`arch/x86/kernel/irq_work.c:16-34`](../../linux/arch/x86/kernel/irq_work.c#L16-L34)（v6.18.60）

```c
#ifdef CONFIG_X86_LOCAL_APIC
DEFINE_IDTENTRY_SYSVEC(sysvec_irq_work)
{
	apic_eoi();
	trace_irq_work_entry(IRQ_WORK_VECTOR);
	inc_irq_stat(apic_irq_work_irqs);
	irq_work_run();
	trace_irq_work_exit(IRQ_WORK_VECTOR);
}

void arch_irq_work_raise(void)
{
	if (!arch_irq_work_has_interrupt())
		return;

	__apic_send_IPI_self(IRQ_WORK_VECTOR);
	apic_wait_icr_idle();
}
#endif
```

给自己发 IPI 的效果是：当前上下文（NMI 或关中断区间）结束、中断重新打开的那一刻，CPU 立即进入 `sysvec_irq_work()`，在一个干净的硬中断上下文中执行工作。这比"等下一个时钟中断"快得多，又比"在当前上下文中直接执行"安全得多。

跨 CPU 提交 `irq_work_queue_on()`（[`kernel/irq_work.c:137-182`](../../linux/kernel/irq_work.c#L137-L182)）则把节点放进目标 CPU 的 `call_single_queue`（类型 `CSD_TYPE_IRQ_WORK`），由函数调用 IPI 处理；由于跨 CPU 发 IPI 的后端不是 NMI 安全的，它在 NMI 中调用会触发警告。5.10 节的 BH 工作队列跨 CPU 触发软中断用的就是这个接口。

### 执行

[`kernel/irq_work.c:201-233`](../../linux/kernel/irq_work.c#L201-L233)（v6.18.60）

```c
void irq_work_single(void *arg)
{
	struct irq_work *work = arg;
	int flags;

	/*
	 * Clear the PENDING bit, after this point the @work can be re-used.
	 * The PENDING bit acts as a lock, and we own it, so we can clear it
	 * without atomic ops.
	 */
	flags = atomic_read(&work->node.a_flags);
	flags &= ~IRQ_WORK_PENDING;
	atomic_set(&work->node.a_flags, flags);

	/*
	 * See irq_work_claim().
	 */
	smp_mb();

	lockdep_irq_work_enter(flags);
	work->func(work);
	lockdep_irq_work_exit(flags);

	/*
	 * Clear the BUSY bit, if set, and return to the free state if no-one
	 * else claimed it meanwhile.
	 */
	(void)atomic_cmpxchg(&work->node.a_flags, flags, flags & ~IRQ_WORK_BUSY);

	if ((IS_ENABLED(CONFIG_PREEMPT_RT) && !irq_work_is_hard(work)) ||
	    !arch_irq_work_has_interrupt())
		rcuwait_wake_up(&work->irqwait);
}
```

两个标志位的分工：`PENDING` 在执行前清除，之后再提交可以重新入队（即"执行期间被再次提交会再执行一次"）；`BUSY` 在执行后清除，`irq_work_sync()`（[`kernel/irq_work.c:286`](../../linux/kernel/irq_work.c#L286) 起）等待它清除，以确定回调不在运行中，可以安全释放 `irq_work` 结构。`smp_mb()` 与 `irq_work_claim()` 中的完全屏障配对（`atomic_fetch_or()` 隐含完全屏障）：提交者在提交前写入的数据，对回调可见。

[`kernel/irq_work.c:259-280`](../../linux/kernel/irq_work.c#L259-L280)（v6.18.60）

```c
void irq_work_run(void)
{
	irq_work_run_list(this_cpu_ptr(&raised_list));
	if (!IS_ENABLED(CONFIG_PREEMPT_RT))
		irq_work_run_list(this_cpu_ptr(&lazy_list));
	else
		wake_irq_workd();
}
EXPORT_SYMBOL_GPL(irq_work_run);

void irq_work_tick(void)
{
	struct llist_head *raised = this_cpu_ptr(&raised_list);

	if (!llist_empty(raised) && !arch_irq_work_has_interrupt())
		irq_work_run_list(raised);

	if (!IS_ENABLED(CONFIG_PREEMPT_RT))
		irq_work_run_list(this_cpu_ptr(&lazy_list));
	else
		wake_irq_workd();
}
```

`irq_work_run()` 在 IPI 中处理两个列表；`irq_work_tick()` 在每个时钟中断中调用，处理惰性工作（以及没有自 IPI 能力的架构上的普通工作）。PREEMPT_RT 下非 `HARD` 的工作交给 per-CPU 的 `irq_work/N` 线程。

### irq_work 的典型使用者

| 使用者 | 为何需要 irq_work |
|---|---|
| perf | 在 NMI 中采样后，需要唤醒读取环形缓冲区的用户进程（`perf_pending_irq()`） |
| printk | 在任意上下文写入环形缓冲区后，延迟唤醒控制台输出（5.4 节） |
| RCU | 在持有 rq 锁或关中断时需要触发后续处理（`rcu_preempt_deferred_qs` 等，7.10 节） |
| BPF | 在跟踪程序中发送信号、更新 ringbuf 通知 |
| 调度器 | `sched_ext`、cpufreq 等在 rq 锁下请求的异步动作 |
| BH 工作队列 | 跨 CPU 触发目标 CPU 的软中断（5.10 节） |

*表 5-23：irq_work 的典型使用者*

## 关键细节 / 设计考量

### IPI 的代价

在物理机上发送一次 IPI 的开销是数百纳秒到几微秒，接收方要打断当前工作、保存现场、处理、返回。在虚拟机中代价更高：发送方写 ICR 引起 VM exit，宿主机再向目标 vCPU 注入中断；目标 vCPU 若未在运行，还需要先被调度。内核在多处尽量减少 IPI：

- 同一目标 CPU 的多个请求合并为一次 IPI（`llist_add()` 的返回值）；
- 轮询式 idle 的 CPU 用设置标志代替 IPI（`set_nr_if_polling()`）；
- TLB 刷新跳过惰性 TLB 的 CPU；
- KVM 客户机用 PV IPI 超级调用批量发送、PV TLB flush 对已被抢占的 vCPU 延迟刷新；
- 远程唤醒通过 wake list 批量处理（3.4 节）。

### 为什么 smp_call_function 不能在中断中同步等待

除了 `smp_call_function_single()` 注释中提到的死锁，还有一个更根本的原因：同步等待期间本 CPU 自旋，如果是在关中断的上下文中，它自己就无法响应别人发来的 IPI。两个 CPU 互相等待就会死锁。需要在中断或原子上下文中跨 CPU 调用时，应使用 `smp_call_function_single_async()` 并自行管理 csd 的生命周期，或者使用 `irq_work_queue_on()`。

## 动手实验

### 统计 IPI 的发送者与类型

```bash
# 在 dev 虚拟机中执行
grep -E '^ *(RES|CAL|TLB|IWI):' /proc/interrupts
sudo bpftrace -e '
tracepoint:ipi:ipi_send_cpu     { @single[ksym(args->callback), comm] = count(); }
tracepoint:ipi:ipi_send_cpumask { @mask[ksym(args->callback), comm] = count(); }
interval:s:5 { exit(); }'
```

`ipi_send_cpu` / `ipi_send_cpumask` 跟踪点（`include/trace/events/ipi.h`）由 `send_call_function_single_ipi()` 等发送路径触发，`callback` 字段是目标 CPU 将执行的处理函数。

### 观察 TLB shootdown

```bash
cat > /tmp/tlb.c <<'EOF'
#include <pthread.h>
#include <sys/mman.h>
#include <unistd.h>
static void *spin(void *a) { for (;;) ; return 0; }
int main(void) {
	pthread_t t[3];
	for (int i = 0; i < 3; i++) pthread_create(&t[i], 0, spin, 0);
	for (int i = 0; i < 100000; i++) {
		char *p = mmap(0, 4096, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
		p[0] = 1; munmap(p, 4096);
	}
	return 0;
}
EOF
gcc -O2 -pthread -o /tmp/tlb /tmp/tlb.c
grep TLB /proc/interrupts; /tmp/tlb; grep TLB /proc/interrupts
```

在至少 4 个 vCPU 的虚拟机中，其他三个线程一直在别的 CPU 上运行同一地址空间，每次 `munmap()` 都要向它们发送 TLB 刷新 IPI，各 CPU 的 `TLB` 计数之和会增加数十万次（具体数值取决于线程分布与 PCID/INVLPGB 等硬件特性）。

## 小结

- x86 用 `RESCHEDULE_VECTOR`、`CALL_FUNCTION_[SINGLE_]VECTOR`、`IRQ_WORK_VECTOR` 等系统向量实现 IPI，处理函数直接调用调度器、跨 CPU 函数调用队列或 irq_work；KVM 客户机用 PV IPI 批量发送。
- 跨 CPU 函数调用把 csd 放入目标 CPU 的无锁 `call_single_queue`，仅当队列从空变非空时才发 IPI；目标处于轮询 idle 时只设置 `TIF_NEED_RESCHED`，免去 IPI。
- csd 锁以 release / acquire 语义标识"函数已执行完"；同步调用不能在关中断或非进程上下文中进行，否则可能互相等待而死锁。
- 接收端一次取走整个队列，先执行同步调用（执行后解锁），再执行异步调用（先解锁后执行）与 irq_work，最后批量处理远程唤醒。
- `smp_call_function_many_cond()` 先入队、再选择单播或广播发送、然后执行本地部分、最后等待；TLB shootdown 是其最频繁的使用者，并跳过惰性 TLB 的 CPU。
- irq_work 的提交只用原子操作和无锁链表，可在 NMI 中调用；本地提交给自己发 `IRQ_WORK_VECTOR` IPI，在上下文恢复开中断后立即执行，`PENDING` 与 `BUSY` 两个标志分别管理重新提交与同步等待。

## 参考

- `kernel/smp.c`、`kernel/irq_work.c`、`include/linux/smp_types.h` 中的源码注释
- `linux/Documentation/arch/x86/tlb.rst`
- Peter Zijlstra 引入 irq_work 的补丁（v2.6.37 合入，由 perf 的 pending 机制泛化而来）
- Frederic Weisbecker 引入 `IRQ_WORK_LAZY` 的补丁（v3.9）
- Peter Zijlstra 将远程唤醒与 irq_work 统一到 `call_single_queue` 的补丁系列（v5.8）
