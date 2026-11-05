# 7.1 并发来源与执行上下文

> **本节源码**：`include/linux/preempt.h`、`arch/x86/include/asm/preempt.h`、`arch/x86/include/asm/irqflags.h`、`include/linux/bottom_half.h`、`kernel/softirq.c`、`include/linux/kernel.h`、`arch/x86/include/asm/percpu.h`、`include/linux/local_lock_internal.h`
> **关键函数**：`preempt_disable()`、`preempt_enable()`、`local_irq_save()`、`local_bh_disable()`、`__local_bh_enable_ip()`、`might_sleep()`、`this_cpu_add()`、`local_lock()`
> **前置知识**：3.2 主调度函数与上下文切换、3.5 调度时钟与抢占模型

## 概述

同步问题的根源是"同一份数据可能被多条执行流交错访问"。在内核中，这样的执行流有五个来源：**多个 CPU 真正的并行执行**、**内核抢占**（同一 CPU 上的另一个任务插进来）、**硬中断**、**软中断**（softirq）以及 **NMI**。不同来源需要不同的防护手段：关抢占挡得住同 CPU 的其他任务，却挡不住中断；关中断挡得住本 CPU 的中断，却挡不住其他 CPU。

因此，在选择锁之前必须先回答两个问题：数据会在哪些**执行上下文**中被访问？当前代码处于什么上下文、能否睡眠？内核用一个 per-CPU 整数 `preempt_count` 同时记录抢占禁用深度、软中断与硬中断嵌套层数，几乎所有"能不能睡眠""现在在哪"的判断都由它回答。

本节先拆解 `preempt_count` 的编码，然后分析三种"本地关闭"操作——关抢占、关中断、关软中断——在 x86_64 上的实现，最后介绍完全不需要锁的 per-CPU 数据与 `local_lock`。这些是后续所有锁原语的地基：7.4 节的自旋锁本质上就是"关抢占（或关中断）+ 跨 CPU 互斥"。

## 并发来源一览

| 来源 | 发生方式 | 本地防护手段 | 跨 CPU 防护 |
|---|---|---|---|
| 多 CPU 并行 | 其他 CPU 同时执行 | 无（本地手段无效） | 锁、原子操作、RCU |
| 内核抢占 | 中断返回或 `preempt_enable()` 时切换任务 | `preempt_disable()` | — |
| 软中断 | 中断退出、`local_bh_enable()`、`ksoftirqd` | `local_bh_disable()` | — |
| 硬中断 | 外设、IPI、定时器 | `local_irq_save()` | — |
| NMI | 不可屏蔽 | 无法屏蔽，只能用无锁算法 | — |

*表 7-1：内核中的并发来源及其防护手段*

本书配置开启了 `CONFIG_SMP`、`CONFIG_PREEMPT_LAZY` 与 `CONFIG_PREEMPT_DYNAMIC`（3.5 节），内核态代码在任何 `preempt_count` 为 0 的位置都可能被抢占，五种来源都需要考虑。

## 核心数据结构：preempt_count

### 位域布局

`preempt_count` 被划分为几个计数域：

```c
// include/linux/preempt.h:14-55（v6.18.60）
/*
 * We put the hardirq and softirq counter into the preemption
 * counter. The bitmask has the following meaning:
 *
 * - bits 0-7 are the preemption count (max preemption depth: 256)
 * - bits 8-15 are the softirq count (max # of softirqs: 256)
 *
 * The hardirq count could in theory be the same as the number of
 * interrupts in the system, but we run all interrupt handlers with
 * interrupts disabled, so we cannot have nesting interrupts. Though
 * there are a few palaeontologic drivers which reenable interrupts in
 * the handler, so we need more than one bit here.
 *
 *         PREEMPT_MASK:	0x000000ff
 *         SOFTIRQ_MASK:	0x0000ff00
 *         HARDIRQ_MASK:	0x000f0000
 *             NMI_MASK:	0x00f00000
 * PREEMPT_NEED_RESCHED:	0x80000000
 */
#define PREEMPT_BITS	8
#define SOFTIRQ_BITS	8
#define HARDIRQ_BITS	4
#define NMI_BITS	4

#define PREEMPT_SHIFT	0
#define SOFTIRQ_SHIFT	(PREEMPT_SHIFT + PREEMPT_BITS)
#define HARDIRQ_SHIFT	(SOFTIRQ_SHIFT + SOFTIRQ_BITS)
#define NMI_SHIFT	(HARDIRQ_SHIFT + HARDIRQ_BITS)

#define __IRQ_MASK(x)	((1UL << (x))-1)

#define PREEMPT_MASK	(__IRQ_MASK(PREEMPT_BITS) << PREEMPT_SHIFT)
#define SOFTIRQ_MASK	(__IRQ_MASK(SOFTIRQ_BITS) << SOFTIRQ_SHIFT)
#define HARDIRQ_MASK	(__IRQ_MASK(HARDIRQ_BITS) << HARDIRQ_SHIFT)
#define NMI_MASK	(__IRQ_MASK(NMI_BITS)     << NMI_SHIFT)

#define PREEMPT_OFFSET	(1UL << PREEMPT_SHIFT)
#define SOFTIRQ_OFFSET	(1UL << SOFTIRQ_SHIFT)
#define HARDIRQ_OFFSET	(1UL << HARDIRQ_SHIFT)
#define NMI_OFFSET	(1UL << NMI_SHIFT)

#define SOFTIRQ_DISABLE_OFFSET	(2 * SOFTIRQ_OFFSET)
```

x86_64 上 `preempt_count` 不在 `thread_info` 里，而是一个 per-CPU 变量 `__preempt_count`（定义在 `arch/x86/kernel/cpu/common.c:2206`），读取时屏蔽掉最高位的 `PREEMPT_NEED_RESCHED`：

```c
// arch/x86/include/asm/preempt.h:10-28（v6.18.60）
DECLARE_PER_CPU_CACHE_HOT(int, __preempt_count);

/* We use the MSB mostly because its available */
#define PREEMPT_NEED_RESCHED	0x80000000

/*
 * We use the PREEMPT_NEED_RESCHED bit as an inverted NEED_RESCHED such
 * that a decrement hitting 0 means we can and should reschedule.
 */
#define PREEMPT_ENABLED	(0 + PREEMPT_NEED_RESCHED)

/*
 * We mask the PREEMPT_NEED_RESCHED bit so as not to confuse all current users
 * that think a non-zero value indicates we cannot preempt.
 */
static __always_inline int preempt_count(void)
{
	return raw_cpu_read_4(__preempt_count) & ~PREEMPT_NEED_RESCHED;
}
```

```
 31  30      24 23   20 19   16 15          8 7           0
┌───┬─────────┬───────┬───────┬─────────────┬─────────────┐
│ R │ (未用)  │  NMI  │HARDIRQ│   SOFTIRQ   │   PREEMPT   │
└───┴─────────┴───────┴───────┴─────────────┴─────────────┘
  │                              │ bit 8：正在执行软中断
  │                              │ bit 9-15：local_bh_disable() 深度（每次 +2）
  └─ PREEMPT_NEED_RESCHED（取反存放：0 表示需要重调度）
```

*图 7-1：x86_64 上 `__preempt_count` 的位域布局（高位在左）*

几个值得注意的设计：

- **软中断域用了两种步长**。`SOFTIRQ_OFFSET`（`1 << 8`）表示"正在执行软中断处理函数"，`SOFTIRQ_DISABLE_OFFSET`（`2 << 8`）表示"禁用了软中断"。这样只需检查第 8 位就能区分"在软中断里"和"只是关了软中断"——这正是 `in_serving_softirq()` 与 `in_softirq()` 的区别。
- **抢占计数为 per-CPU 而非 per-task**。上下文切换时并不保存 / 恢复它，而是依赖一个不变式：任何任务在切换点上的 `preempt_count` 都等于 `2 * PREEMPT_DISABLE_OFFSET`（`schedule()` 自身关一次抢占，持有的 `rq->lock` 再关一次），见 `include/linux/preempt.h:67-76` 的 `FORK_PREEMPT_COUNT` 注释。新任务从 `finish_task_switch()` 开始执行时会把它恢复为 0（3.2 节）。
- **NEED_RESCHED 折叠进最高位**，让 `preempt_enable()` 只需一条 `decl` 指令就能同时判断"计数归零"和"需要调度"（3.2 节"x86 上的 preempt_count 小技巧"）。

### 上下文判断宏

基于这些域，内核定义了一组判断当前上下文的宏：

```c
// include/linux/preempt.h:108-133（v6.18.60）
#define nmi_count()	(preempt_count() & NMI_MASK)
#define hardirq_count()	(preempt_count() & HARDIRQ_MASK)
#ifdef CONFIG_PREEMPT_RT
# define softirq_count()	(current->softirq_disable_cnt & SOFTIRQ_MASK)
# define irq_count()		((preempt_count() & (NMI_MASK | HARDIRQ_MASK)) | softirq_count())
#else
# define softirq_count()	(preempt_count() & SOFTIRQ_MASK)
# define irq_count()		(preempt_count() & (NMI_MASK | HARDIRQ_MASK | SOFTIRQ_MASK))
#endif

/*
 * Macros to retrieve the current execution context:
 *
 * in_nmi()		- We're in NMI context
 * in_hardirq()		- We're in hard IRQ context
 * in_serving_softirq()	- We're in softirq context
 * in_task()		- We're in task context
 */
#define in_nmi()		(nmi_count())
#define in_hardirq()		(hardirq_count())
#define in_serving_softirq()	(softirq_count() & SOFTIRQ_OFFSET)
#ifdef CONFIG_PREEMPT_RT
# define in_task()		(!((preempt_count() & (NMI_MASK | HARDIRQ_MASK)) | in_serving_softirq()))
#else
# define in_task()		(!(preempt_count() & (NMI_MASK | HARDIRQ_MASK | SOFTIRQ_OFFSET)))
#endif
```

| 宏 | 为真的条件 |
|---|---|
| `in_nmi()` | NMI 域非 0 |
| `in_hardirq()` | 硬中断域非 0（NMI 中也会设置） |
| `in_serving_softirq()` | 正在执行软中断处理函数 |
| `in_softirq()` | 正在执行软中断**或**关闭了软中断（已不推荐使用） |
| `in_interrupt()` | 以上任一（已不推荐使用） |
| `in_task()` | 不在 NMI、硬中断、软中断处理中——即进程上下文 |
| `in_atomic()` | `preempt_count() != 0` |

`in_atomic()`（`include/linux/preempt.h:186`）的注释特意警告：它**不能**用来判断能否睡眠。在 `CONFIG_PREEMPT_COUNT=n` 的内核上，自旋锁不会修改 `preempt_count`，`in_atomic()` 会给出错误答案；即使在本书配置下，它也看不到"关中断但未关抢占"的状态。驱动代码不应依赖它。

## 主流程分析

### 关抢占：preempt_disable / preempt_enable

在 `CONFIG_PREEMPT_COUNT=y` 且 `CONFIG_PREEMPTION=y`（本书配置）下：

```c
// include/linux/preempt.h:211-235（v6.18.60）
#ifdef CONFIG_PREEMPT_COUNT

#define preempt_disable() \
do { \
	preempt_count_inc(); \
	barrier(); \
} while (0)

#define sched_preempt_enable_no_resched() \
do { \
	barrier(); \
	preempt_count_dec(); \
} while (0)

#define preempt_enable_no_resched() sched_preempt_enable_no_resched()

#define preemptible()	(preempt_count() == 0 && !irqs_disabled())

#ifdef CONFIG_PREEMPTION
#define preempt_enable() \
do { \
	barrier(); \
	if (unlikely(preempt_count_dec_and_test())) \
		__preempt_schedule(); \
} while (0)
```

- `preempt_disable()` 在加一**之后**放一个 `barrier()`；`preempt_enable()` 在减一**之前**放一个 `barrier()`。`barrier()` 是编译器屏障（`include/linux/compiler.h:85`，`asm volatile("" ::: "memory")`），防止编译器把临界区内的内存访问挪到计数修改之外。关抢占只影响本 CPU，因此不需要 CPU 内存屏障。
- `preempt_enable()` 在计数归零且有重调度请求时调用 `__preempt_schedule()`，这是内核抢占的主要触发点之一。`preempt_enable_no_resched()` 则跳过这一检查，只在调度器内部等明确知道马上会调度的场合使用；它对模块不可见（`include/linux/preempt.h:299-307` 用 `#undef` 屏蔽）。

x86 上的加减直接编译为一条带 `%gs` 段前缀的内存指令：

```c
// arch/x86/include/asm/preempt.h:78-98（v6.18.60）
static __always_inline void __preempt_count_add(int val)
{
	raw_cpu_add_4(__preempt_count, val);
}

static __always_inline void __preempt_count_sub(int val)
{
	raw_cpu_add_4(__preempt_count, -val);
}

/*
 * Because we keep PREEMPT_NEED_RESCHED set when we do _not_ need to reschedule
 * a decrement which hits zero means we have no preempt_count and should
 * reschedule.
 */
static __always_inline bool __preempt_count_dec_and_test(void)
{
	return GEN_UNARY_RMWcc("decl", __my_cpu_var(__preempt_count), e,
			       __percpu_arg([var]));
}

```

`raw_cpu_add_4()` 生成形如 `addl $1, %gs:__preempt_count` 的单条指令。单条指令对本 CPU 的中断是原子的——中断只能发生在指令边界上——因此无需 `lock` 前缀或关中断。

关抢占的语义是"当前任务不会在本 CPU 上被换下"，由此还隐含了"不会迁移到其他 CPU"，`smp_processor_id()` 的结果保持有效。另外，关抢占区间对 RCU 而言也是读临界区（7.9 节）。

### 关中断：local_irq_save / local_irq_restore

关中断用于保护与中断处理函数共享的数据。通用层的 `local_irq_save(flags)` 最终调用 `arch_local_irq_save()`。不考虑半虚拟化时，x86 的实现非常直接：

```c
// arch/x86/include/asm/irqflags.h:17-75（v6.18.60）
extern __always_inline unsigned long native_save_fl(void)
{
	unsigned long flags;

	/*
	 * "=rm" is safe here, because "pop" adjusts the stack before
	 * it evaluates its effective address -- this is part of the
	 * documented behavior of the "pop" instruction.
	 */
	asm volatile("# __raw_save_flags\n\t"
		     "pushf ; pop %0"
		     : "=rm" (flags)
		     : /* no input */
		     : "memory");

	return flags;
}

static __always_inline void native_irq_disable(void)
{
	asm volatile("cli": : :"memory");
}

static __always_inline void native_irq_enable(void)
{
	asm volatile("sti": : :"memory");
}

static __always_inline void native_safe_halt(void)
{
	x86_idle_clear_cpu_buffers();
	asm volatile("sti; hlt": : :"memory");
}

static __always_inline void native_halt(void)
{
	x86_idle_clear_cpu_buffers();
	asm volatile("hlt": : :"memory");
}

static __always_inline int native_irqs_disabled_flags(unsigned long flags)
{
	return !(flags & X86_EFLAGS_IF);
}

static __always_inline unsigned long native_local_irq_save(void)
{
	unsigned long flags = native_save_fl();

	native_irq_disable();

	return flags;
}

static __always_inline void native_local_irq_restore(unsigned long flags)
{
	if (!native_irqs_disabled_flags(flags))
		native_irq_enable();
}
```

`save` 用 `pushf; pop` 读出 `RFLAGS`，再执行 `cli`；`restore` 只在原先 `IF` 位为 1 时才执行 `sti`。保存 / 恢复而不是简单的"关 / 开"，使得关中断可以安全嵌套：内层恢复时不会意外打开外层关掉的中断。

本书配置开启了 `CONFIG_PARAVIRT_XXL`，因此实际使用的是 `arch/x86/include/asm/paravirt.h:658-681` 中的版本：

```c
// arch/x86/include/asm/paravirt.h:658-681（v6.18.60）
static __always_inline unsigned long arch_local_save_flags(void)
{
	return PVOP_ALT_CALLEE0(unsigned long, irq.save_fl, "pushf; pop %%rax;",
				ALT_NOT_XEN);
}

static __always_inline void arch_local_irq_disable(void)
{
	PVOP_ALT_VCALLEE0(irq.irq_disable, "cli;", ALT_NOT_XEN);
}

static __always_inline void arch_local_irq_enable(void)
{
	PVOP_ALT_VCALLEE0(irq.irq_enable, "sti;", ALT_NOT_XEN);
}

static __always_inline unsigned long arch_local_irq_save(void)
{
	unsigned long f;

	f = arch_local_save_flags();
	arch_local_irq_disable();
	return f;
}
```

`PVOP_ALT_*` 在启动时由 alternatives 机制打补丁：裸机和 KVM 客户机上被替换为内联的 `pushf; pop %rax` 与 `cli`，只有 Xen PV 客户机才保留对超级调用的间接调用。所以开启 PARAVIRT 不会给常见场景带来额外开销。

注意 `cli` 只屏蔽**本 CPU** 的可屏蔽中断，NMI 和其他 CPU 不受影响。关中断期间也不会发生抢占（抢占需要中断或显式调度点触发），但 `preempt_count` 并不反映这一状态，判断是否可抢占需看 `preemptible()`（`include/linux/preempt.h:227`），它同时检查 `irqs_disabled()`。

### 关软中断：local_bh_disable / local_bh_enable

"bottom half"（BH）是软中断在 API 中的旧称。关软中断不需要碰硬件，只需增加 `preempt_count` 的软中断域：

```c
// include/linux/bottom_half.h:8-34（v6.18.60）
#if defined(CONFIG_PREEMPT_RT) || defined(CONFIG_TRACE_IRQFLAGS)
extern void __local_bh_disable_ip(unsigned long ip, unsigned int cnt);
#else
static __always_inline void __local_bh_disable_ip(unsigned long ip, unsigned int cnt)
{
	preempt_count_add(cnt);
	barrier();
}
#endif

static inline void local_bh_disable(void)
{
	__local_bh_disable_ip(_THIS_IP_, SOFTIRQ_DISABLE_OFFSET);
}

extern void _local_bh_enable(void);
extern void __local_bh_enable_ip(unsigned long ip, unsigned int cnt);

static inline void local_bh_enable_ip(unsigned long ip)
{
	__local_bh_enable_ip(ip, SOFTIRQ_DISABLE_OFFSET);
}

static inline void local_bh_enable(void)
{
	__local_bh_enable_ip(_THIS_IP_, SOFTIRQ_DISABLE_OFFSET);
}
```

本书配置未开启 `CONFIG_TRACE_IRQFLAGS`（它由 `CONFIG_PROVE_LOCKING` 等调试选项选中），因此 `__local_bh_disable_ip()` 就是第 11-15 行的内联版本：加上 `SOFTIRQ_DISABLE_OFFSET` 再放一个编译器屏障。增加软中断计数的副作用是 `preempt_count` 非 0，所以**关软中断也隐含了关抢占**。

重新打开软中断要复杂一些，因为在关闭期间可能有软中断被挂起，必须在打开时补执行：

```c
// kernel/softirq.c:427-458（v6.18.60）
void __local_bh_enable_ip(unsigned long ip, unsigned int cnt)
{
	WARN_ON_ONCE(in_hardirq());
	lockdep_assert_irqs_enabled();
#ifdef CONFIG_TRACE_IRQFLAGS
	local_irq_disable();
#endif
	/*
	 * Are softirqs going to be turned on now:
	 */
	if (softirq_count() == SOFTIRQ_DISABLE_OFFSET)
		lockdep_softirqs_on(ip);
	/*
	 * Keep preemption disabled until we are done with
	 * softirq processing:
	 */
	__preempt_count_sub(cnt - 1);

	if (unlikely(!in_interrupt() && local_softirq_pending())) {
		/*
		 * Run softirq if any pending. And do it in its own stack
		 * as we may be calling this deep in a task call stack already.
		 */
		do_softirq();
	}

	preempt_count_dec();
#ifdef CONFIG_TRACE_IRQFLAGS
	local_irq_enable();
#endif
	preempt_check_resched();
}
```

这里先减去 `cnt - 1` 而非 `cnt`，让 `preempt_count` 保持至少为 1（抢占仍关闭），再检查是否有挂起的软中断并调用 `do_softirq()` 在软中断栈上执行它们，最后才把剩下的 1 减掉并检查重调度。如果在 `do_softirq()` 之前就允许抢占，任务可能被换走，挂起的软中断就要等到下一次中断退出才能执行。

> **PREEMPT_RT 下的差异**：`kernel/softirq.c:156` 起的另一套实现中，软中断禁用计数改记在 `current->softirq_disable_cnt`，并以一个 per-CPU 的 `local_lock` 串行化，`local_bh_disable()` 不再关抢占。本书配置未开启 `CONFIG_PREEMPT_RT`。

### 不能睡眠的上下文：might_sleep

原子上下文（Atomic Context）指不允许睡眠的执行环境：持有自旋锁、关抢占、关中断、处于中断或软中断处理中。在原子上下文中调用可能睡眠的函数（`mutex_lock()`、`GFP_KERNEL` 分配、`copy_from_user()` 等）是严重错误：要么触发 "scheduling while atomic" 告警，要么在单核上直接死锁。

可能睡眠的函数开头通常会写一句 `might_sleep()`：

```c
// include/linux/kernel.h:75-101（v6.18.60）
#ifdef CONFIG_DEBUG_ATOMIC_SLEEP
extern void __might_resched(const char *file, int line, unsigned int offsets);
extern void __might_sleep(const char *file, int line);
extern void __cant_sleep(const char *file, int line, int preempt_offset);
extern void __cant_migrate(const char *file, int line);

/**
 * might_sleep - annotation for functions that can sleep
 *
 * this macro will print a stack trace if it is executed in an atomic
 * context (spinlock, irq-handler, ...). Additional sections where blocking is
 * not allowed can be annotated with non_block_start() and non_block_end()
 * pairs.
 *
 * This is a useful debugging help to be able to catch problems early and not
 * be bitten later when the calling function happens to sleep when it is not
 * supposed to.
 */
# define might_sleep() \
	do { __might_sleep(__FILE__, __LINE__); might_resched(); } while (0)
/**
 * cant_sleep - annotation for functions that cannot sleep
 *
 * this macro will print a stack trace if it is executed with preemption enabled
 */
# define cant_sleep() \
	do { __cant_sleep(__FILE__, __LINE__, 0); } while (0)
```

只有开启 `CONFIG_DEBUG_ATOMIC_SLEEP` 时 `__might_sleep()` 才会检查 `preempt_count()` 与 `irqs_disabled()` 并打印 "BUG: sleeping function called from invalid context"。本书配置未开启该选项，此时 `might_sleep()` 退化为 `might_resched()`（`include/linux/kernel.h:136`）——在 `PREEMPT_DYNAMIC` 下它是一个 static call，按当前抢占模型决定是否调用 `__cond_resched()`。换言之，在默认配置下错误的睡眠不会被提前报告，开发调试时应打开 `CONFIG_DEBUG_ATOMIC_SLEEP`。

## per-CPU 数据：用"不共享"代替"加锁"

最好的同步是不需要同步。如果每个 CPU 只访问自己的那份数据，跨 CPU 的竞争就不存在，剩下的只有本 CPU 内的抢占和中断。内核中的统计计数器、SLUB 的 `kmem_cache_cpu`、伙伴系统的 PCP（4.3 节）、运行队列都采用这一思路。

### this_cpu 操作

访问 per-CPU 变量需要先拿到"当前 CPU"，再读改写对应的副本。如果这两步之间发生抢占和迁移，就会改到别的 CPU 的副本上。传统写法是 `get_cpu_var()` / `put_cpu_var()`，即先关抢占再访问。x86 提供了更好的办法：利用 `%gs` 段寄存器指向当前 CPU 的 per-CPU 区域，把"定位 + 读改写"合并为一条指令：

```c
// arch/x86/include/asm/percpu.h:218-240（v6.18.60）
/*
 * Generate a per-CPU add to memory instruction and optimize code
 * if one is added or subtracted.
 */
#define percpu_add_op(size, qual, var, val)				\
do {									\
	const int pao_ID__ =						\
		(__builtin_constant_p(val) &&				\
			((val) == 1 ||					\
			 (val) == (typeof(val))-1)) ? (int)(val) : 0;	\
									\
	if (0) {							\
		TYPEOF_UNQUAL(var) pao_tmp__;				\
		pao_tmp__ = (val);					\
		(void)pao_tmp__;					\
	}								\
	if (pao_ID__ == 1)						\
		percpu_unary_op(size, qual, "inc", var);		\
	else if (pao_ID__ == -1)					\
		percpu_unary_op(size, qual, "dec", var);		\
	else								\
		percpu_binary_op(size, qual, "add", var, val);		\
} while (0)
```

`this_cpu_add(pcp, val)` 最终展开为 `percpu_add_op(..., volatile, ...)`（`arch/x86/include/asm/percpu.h:540`），生成一条 `add`/`inc`/`dec` 加 `%gs:` 前缀的指令。由于是单条指令，它对本 CPU 的抢占和中断天然原子，**无需关抢占、无需 `lock` 前缀**。这类操作族包括 `this_cpu_read/write/add/inc/xchg/cmpxchg`，SLUB 快速路径用的 `this_cpu_try_cmpxchg128()` 也属于此类（4.5 节）。

与之相对，`raw_cpu_*()` / `__this_cpu_*()` 不保证对抢占安全，调用者须自行关抢占；在 x86 上它们与 `this_cpu_*()` 生成的指令几乎相同，区别只在 `volatile` 限定和调试检查。

### local_lock

当 per-CPU 数据的更新涉及多个字段、无法用单条指令完成时，就需要关抢占或关中断。但直接写 `preempt_disable()` 有两个问题：lockdep 无法知道它保护的是哪份数据；在 PREEMPT_RT 下关抢占区间不能包含会睡眠的锁。`local_lock_t`（v5.8 引入）为这类"per-CPU 临界区"提供了带名字的锁对象：

```c
// include/linux/local_lock_internal.h:9-16（v6.18.60）
#ifndef CONFIG_PREEMPT_RT

typedef struct {
#ifdef CONFIG_DEBUG_LOCK_ALLOC
	struct lockdep_map	dep_map;
	struct task_struct	*owner;
#endif
} local_lock_t;
```

非 RT 内核且未开启 `CONFIG_DEBUG_LOCK_ALLOC` 时，`local_lock_t` 是一个空结构体，加锁操作就是关抢占或关中断：

```c
// include/linux/local_lock_internal.h:116-132（v6.18.60）
#define __local_lock(lock)					\
	do {							\
		preempt_disable();				\
		__local_lock_acquire(lock);			\
	} while (0)

#define __local_lock_irq(lock)					\
	do {							\
		local_irq_disable();				\
		__local_lock_acquire(lock);			\
	} while (0)

#define __local_lock_irqsave(lock, flags)			\
	do {							\
		local_irq_save(flags);				\
		__local_lock_acquire(lock);			\
	} while (0)
```

所以在本书配置下 `local_lock()` 与 `preempt_disable()` 的生成代码完全相同，它的价值在于语义：一方面开启 lockdep 后能检查 per-CPU 锁的使用顺序；另一方面 PREEMPT_RT 内核会把它替换为真正的 per-CPU `spinlock_t`（基于 rt_mutex，可睡眠），只禁止迁移而不关抢占。`mm/swap.c:55` 的 folio 批处理（`cpu_fbatches`）、`mm/memcontrol.c` 的 per-CPU 计费缓存、`mm/slub.c` 的 per-CPU 结构（`mm/slub.c:433`）都使用了 `local_lock` 或 `local_trylock`。

## 关键细节 / 设计考量

### 选择防护手段的决策表

| 数据被访问的上下文 | 进程上下文中的写法 | 软中断中的写法 | 硬中断中的写法 |
|---|---|---|---|
| 仅进程上下文（可睡眠） | `mutex` / `rwsem` | — | — |
| 仅进程上下文（不可睡眠） | `spin_lock()` | — | — |
| 进程 + 软中断 | `spin_lock_bh()` | `spin_lock()` | — |
| 进程 + 硬中断 | `spin_lock_irqsave()` | `spin_lock_irqsave()` | `spin_lock()` |
| 仅本 CPU（per-CPU） | `this_cpu_*()` / `local_lock()` | `this_cpu_*()` | `this_cpu_*()` |
| 读多写少，读者不睡眠 | 读：`rcu_read_lock()`；写：锁 + RCU 发布 | 同左 | 同左 |

*表 7-2：按访问上下文选择同步原语*

规则的核心是：**在低优先级上下文持锁时，必须屏蔽所有可能来争抢同一把锁的高优先级上下文**。否则高优先级上下文在本 CPU 上打断持锁者后自旋等待，持锁者永远得不到运行，形成死锁。反过来，在硬中断处理函数中，本 CPU 的中断已经关闭（x86 的中断门会清 `IF`），直接 `spin_lock()` 即可。

### 为什么要区分 hardirq 与 softirq 计数

如果只有一个"中断嵌套"计数，`local_bh_disable()` 之后 `in_interrupt()` 会为真，许多"在中断中则走原子路径"的代码会做出错误判断。`preempt_count` 把禁用软中断与执行软中断分开（步长 2 与步长 1），再把硬中断、NMI 各放一个域，使得 `in_serving_softirq()`、`in_hardirq()`、`in_nmi()` 都能精确回答。软中断处理入口 `softirq_handle_begin()`（`kernel/softirq.c:461-464`）只加 `SOFTIRQ_OFFSET`，与禁用操作互不干扰。

### NMI 中能做什么

NMI 可以打断关中断的代码，因此 NMI 处理函数中**不能获取任何可能被普通上下文持有的锁**——包括 `raw_spinlock_t`。NMI 中可用的同步手段只有原子操作、per-CPU 变量和专为 NMI 设计的无锁结构（如 `printk` 的环形缓冲区、`irq_work`、`llist`）。7.4 节会看到 qspinlock 为 NMI 预留了第 4 个 MCS 节点，但这只是为了"NMI 中获取仅在 NMI 中使用的锁"的极少数情况。

## 动手实验

### 观察软中断上下文

用 bpftrace 统计软中断入口次数，并观察网络收包函数在哪个任务上下文中执行（需要 root）：

```bash
# 在 dev 虚拟机中执行
sudo bpftrace -e '
tracepoint:irq:softirq_entry { @softirq[args->vec] = count(); }
kprobe:tcp_v4_rcv   { @rcv_ctx[comm] = count(); }
interval:s:5 { exit(); }'
```

`tcp_v4_rcv()` 通常在 `NET_RX_SOFTIRQ` 中执行，`comm` 会显示为被打断的任务或 `ksoftirqd/N`。

### 统计关中断最长时间

```bash
# 需要 CONFIG_IRQSOFF_TRACER（本书配置未开启，需自行编译）
echo irqsoff | sudo tee /sys/kernel/tracing/current_tracer
echo 0 | sudo tee /sys/kernel/tracing/tracing_max_latency
sleep 10
sudo cat /sys/kernel/tracing/trace | head -40
```

### 验证 might_sleep 检查

在开启 `CONFIG_DEBUG_ATOMIC_SLEEP` 的内核上加载如下模块，`dmesg` 会打印 "BUG: sleeping function called from invalid context" 及调用栈：

```c
// 实验代码（非内核源码）
static DEFINE_SPINLOCK(demo_lock);
static int __init demo_init(void)
{
	spin_lock(&demo_lock);
	msleep(1);		/* 持自旋锁睡眠：错误示范 */
	spin_unlock(&demo_lock);
	return 0;
}
```

## 小结

- 内核并发来自多 CPU、抢占、软中断、硬中断和 NMI 五个来源，本地关闭手段只能挡住同 CPU 的来源，跨 CPU 必须依靠锁、原子操作或 RCU。
- x86_64 的 `preempt_count` 是 per-CPU 变量，按位划分为抢占、软中断、硬中断、NMI 计数，并把取反的 NEED_RESCHED 折叠在最高位；上下文切换不保存它，依赖切换点上的不变式。
- `preempt_disable()` 只是一条 `%gs` 前缀的 `add` 加编译器屏障；`local_irq_save()` 是 `pushf; pop; cli`；`local_bh_disable()` 给软中断域加 2，`local_bh_enable()` 负责补执行挂起的软中断。
- `in_atomic()` 不能可靠判断能否睡眠；`might_sleep()` 的检查依赖 `CONFIG_DEBUG_ATOMIC_SLEEP`，本书配置下未启用。
- per-CPU 数据配合 `this_cpu_*()` 单指令操作可以完全避免锁；多字段更新使用 `local_lock`，它在非 RT 内核中等价于关抢占，但为 lockdep 和 PREEMPT_RT 提供了语义。

## 参考

- `linux/Documentation/locking/preempt-locking.rst`
- `linux/Documentation/locking/locktypes.rst`（各类锁在 RT 与非 RT 下的语义）
- `linux/Documentation/locking/local_lock.rst`
- `linux/Documentation/core-api/this_cpu_ops.rst`
- Thomas Gleixner, "Introduce local_lock()", LKML, 2020（v5.8 合入）
