# 7.3 内存屏障与 Linux 内核内存模型

> **本节源码**：[`include/linux/compiler.h`](../../linux/include/linux/compiler.h)、[`include/asm-generic/rwonce.h`](../../linux/include/asm-generic/rwonce.h)、[`arch/x86/include/asm/barrier.h`](../../linux/arch/x86/include/asm/barrier.h)、[`include/asm-generic/barrier.h`](../../linux/include/asm-generic/barrier.h)、[`include/linux/spinlock.h`](../../linux/include/linux/spinlock.h)、[`tools/memory-model/`](../../linux/tools/memory-model/)
> **关键函数**：`barrier()`、`READ_ONCE()`、`WRITE_ONCE()`、`smp_mb()`、`smp_rmb()`、`smp_wmb()`、`smp_store_release()`、`smp_load_acquire()`、`smp_cond_load_acquire()`、`smp_mb__after_spinlock()`
> **前置知识**：7.2 原子操作、位操作与引用计数

## 概述

单线程程序只关心"结果与按程序顺序执行一致"，因此编译器和 CPU 都可以自由地重排、合并、推迟内存访问。多核并发打破了这个前提：另一个 CPU 可能在中途观察到这些访问，并看到与程序顺序不同的先后关系。**内存屏障**（Memory Barrier）用来约束这种重排，**内存模型**（Memory Model）则精确规定在给定屏障下哪些观察结果是允许的。

Linux 不依赖 C11 内存模型，而是定义了自己的 **Linux 内核内存模型**（Linux Kernel Memory Model，LKMM），以一组跨架构可移植的原语（`READ_ONCE()`、`smp_mb()`、acquire/release 等）表达顺序要求，再由各架构映射为最便宜的指令。x86 是强内存序架构，大部分屏障在它上面只是编译器屏障；但内核代码必须按 LKMM 编写，才能在 arm64、POWER 等弱内存序架构上正确运行。

本节先区分编译器重排与 CPU 重排，再分析各类屏障在 x86_64 上的实现，最后用 LKMM 的 litmus 测试说明几种典型模式。

## 两个层次的重排

### 编译器重排与 barrier()

编译器在不改变单线程语义的前提下可以：把循环中的读提出循环、合并两次写、把写推迟到函数末尾、甚至在寄存器压力大时重新读一次内存。最基本的对策是编译器屏障：

[`include/linux/compiler.h:85`](../../linux/include/linux/compiler.h#L85)（v6.18.60）

```c
# define barrier() __asm__ __volatile__("": : :"memory")
```

空的内联汇编带 `"memory"` 破坏描述，告诉编译器"这里可能读写任意内存"，因此屏障前后的内存访问不能跨越它，寄存器中缓存的内存值也必须作废。它**不产生任何指令**，对 CPU 的重排没有影响。

### READ_ONCE / WRITE_ONCE

`barrier()` 作用于所有内存，粒度太粗。对单个共享变量，内核使用 `READ_ONCE()` / `WRITE_ONCE()`：

[`include/asm-generic/rwonce.h:35-62`](../../linux/include/asm-generic/rwonce.h#L35-L62)（v6.18.60）

```c
#define compiletime_assert_rwonce_type(t)					\
	compiletime_assert(__native_word(t) || sizeof(t) == sizeof(long long),	\
		"Unsupported access size for {READ,WRITE}_ONCE().")

/*
 * Use __READ_ONCE() instead of READ_ONCE() if you do not require any
 * atomicity. Note that this may result in tears!
 */
#ifndef __READ_ONCE
#define __READ_ONCE(x)	(*(const volatile __unqual_scalar_typeof(x) *)&(x))
#endif

#define READ_ONCE(x)							\
({									\
	compiletime_assert_rwonce_type(x);				\
	__READ_ONCE(x);							\
})

#define __WRITE_ONCE(x, val)						\
do {									\
	*(volatile typeof(x) *)&(x) = (val);				\
} while (0)

#define WRITE_ONCE(x, val)						\
do {									\
	compiletime_assert_rwonce_type(x);				\
	__WRITE_ONCE(x, val);						\
} while (0)
```

通过 `volatile` 指针访问，编译器必须**恰好**生成一次访问，不能撕裂（对 `__native_word` 大小的类型）、合并、省略或重复。它解决的是以下问题：

- **读撕裂与重复读**：`if (p) use(p->x)` 中编译器可能读两次 `p`，两次之间 `p` 被其他 CPU 改为 `NULL`。
- **循环提升**：`while (!flag) cpu_relax();` 若 `flag` 不是 `READ_ONCE()`，编译器可能只读一次（`cpu_relax()` 本身含 `"memory"` 破坏可以避免此问题，但不应依赖）。
- **写合并与写撕裂**：对外可见的标志位用普通赋值时，编译器可能用两次 16 位写完成一次 32 位写。

LKMM 中，所有可能与其他 CPU 并发访问的普通变量都应该使用 `READ_ONCE()` / `WRITE_ONCE()`，否则构成**数据竞争**（Data Race），其行为在模型中是未定义的。KCSAN（`CONFIG_KCSAN`，本书配置未开启）就是专门检测此类数据竞争的工具。

### CPU 重排：x86 的 TSO

即使编译器生成的指令顺序正确，CPU 也可能让其他核以不同顺序看到这些访问。x86 采用全序存储（Total Store Order，TSO）模型，它只允许一种重排：

| 先 \ 后 | 读 | 写 |
|---|---|---|
| 读 | 不重排 | 不重排 |
| 写 | **可能重排**（写后读） | 不重排 |

*表 7-3：x86 TSO 允许的重排（不同地址）*

写后读重排的根源是**存储缓冲区**（Store Buffer）：CPU 执行写指令时先把数据放进核内的存储缓冲区，不等它提交到缓存就继续执行后面的读；此时本核能通过转发看到自己的写，其他核却还看不到。arm64、POWER 等弱内存序架构则允许全部四种重排，还可能让不同的核以不同顺序看到其他核的写（非多副本原子性）。

## 主流程分析：x86 上的屏障实现

### 强制屏障与 SMP 屏障

内核有两组 CPU 屏障。`mb()` / `rmb()` / `wmb()` 是**强制屏障**，即使在单处理器内核上也生效，用于与设备之间的同步（例如写完 DMA 描述符后再写门铃寄存器）。`smp_mb()` / `smp_rmb()` / `smp_wmb()` 是 **SMP 屏障**，只用于 CPU 之间的同步，在 `CONFIG_SMP=n` 时退化为 `barrier()`。x86_64 的实现如下：

[`arch/x86/include/asm/barrier.h:21-25`](../../linux/arch/x86/include/asm/barrier.h#L21-L25)（v6.18.60）

```c
#else
#define __mb()	asm volatile("mfence":::"memory")
#define __rmb()	asm volatile("lfence":::"memory")
#define __wmb()	asm volatile("sfence" ::: "memory")
#endif
```

[`arch/x86/include/asm/barrier.h:50-79`](../../linux/arch/x86/include/asm/barrier.h#L50-L79)（v6.18.60）

```c
#define __dma_rmb()	barrier()
#define __dma_wmb()	barrier()

#define __smp_mb()	asm volatile("lock addl $0,-4(%%" _ASM_SP ")" ::: "memory", "cc")

#define __smp_rmb()	dma_rmb()
#define __smp_wmb()	barrier()
#define __smp_store_mb(var, value) do { (void)xchg(&var, value); } while (0)

#define __smp_store_release(p, v)					\
do {									\
	compiletime_assert_atomic_type(*p);				\
	barrier();							\
	WRITE_ONCE(*p, v);						\
} while (0)

#define __smp_load_acquire(p)						\
({									\
	typeof(*p) ___p1 = READ_ONCE(*p);				\
	compiletime_assert_atomic_type(*p);				\
	barrier();							\
	___p1;								\
})

/* Atomic operations are already serializing on x86 */
#define __smp_mb__before_atomic()	do { } while (0)
#define __smp_mb__after_atomic()	do { } while (0)

/* Writing to CR3 provides a full memory barrier in switch_mm(). */
#define smp_mb__after_switch_mm()	do { } while (0)
```

逐个解读：

- **`__smp_mb()`** 用 `lock addl $0,-4(%rsp)`：对栈顶下方一个字做一次无意义的加锁加零。任何 `lock` 指令都会排空存储缓冲区，构成完整屏障；它比 `mfence` 更快，因为 `mfence` 还要等待非临时存储（non-temporal store）和 `clflush` 等弱序操作，而 CPU 之间的同步并不需要这些。栈顶附近的缓存行几乎总在本核的 L1 中，加锁开销很低。
- **`__smp_rmb()` 与 `__smp_wmb()`** 都只是编译器屏障：TSO 本来就不会重排读与读、写与写。`__smp_rmb()` 经由 `dma_rmb()` → `__dma_rmb()` 定义为 `barrier()`。
- **`__smp_store_release()` / `__smp_load_acquire()`** 同样只需要编译器屏障：TSO 下写不会越过之前的读写（满足 release），读也不会被之后的读写越过（满足 acquire）。
- **`__smp_store_mb()`** 用 `xchg`，x86 的 `xchg` 访问内存时隐含 `lock`，"写入 + 完整屏障"一步完成。`set_current_state()` 就用它（3.4 节）。
- **`__smp_mb__before_atomic()` / `__smp_mb__after_atomic()`** 为空：x86 上所有原子读改写都是 `lock` 指令，本身就是完整屏障。

通用层在这些架构原语外面加上 KCSAN 钩子：

[`include/asm-generic/barrier.h:96-122`](../../linux/include/asm-generic/barrier.h#L96-L122)（v6.18.60）

```c
#ifdef CONFIG_SMP

#ifndef smp_mb
#define smp_mb()	do { kcsan_mb(); __smp_mb(); } while (0)
#endif

#ifndef smp_rmb
#define smp_rmb()	do { kcsan_rmb(); __smp_rmb(); } while (0)
#endif

#ifndef smp_wmb
#define smp_wmb()	do { kcsan_wmb(); __smp_wmb(); } while (0)
#endif

#else	/* !CONFIG_SMP */

#ifndef smp_mb
#define smp_mb()	barrier()
#endif

#ifndef smp_rmb
#define smp_rmb()	barrier()
#endif

#ifndef smp_wmb
#define smp_wmb()	barrier()
#endif
```

### 获取与释放

获取（Acquire）与释放（Release）是比完整屏障更弱、也更常用的单向屏障：

- **release 写**：它之前的所有读写，都在它之前对其他 CPU 可见。用于"发布"数据。
- **acquire 读**：它之后的所有读写，都不会提前到它之前执行。用于"获取"数据。

当一个 CPU 的 release 写被另一个 CPU 的 acquire 读读到时，两者"同步"：release 之前的一切对 acquire 之后的一切可见。这正是锁的语义：解锁是 release，加锁是 acquire。7.4 节的 `queued_spin_unlock()` 就是一次 `smp_store_release(&lock->locked, 0)`。

## 典型模式与 LKMM

`tools/memory-model/` 中的 LKMM 用 herd7 工具形式化地判定一个小程序（litmus test）的某个结果是否可能出现。下面用其中两个经典测试说明屏障的作用。

### 消息传递（Message Passing）

生产者先写数据再写标志，消费者先读标志再读数据。若无任何屏障，弱内存序 CPU 上消费者可能看到"标志已置位但数据还是旧的"。正确写法：

[`tools/memory-model/litmus-tests/MP+pooncerelease+poacquireonce.litmus:1-28`](../../linux/tools/memory-model/litmus-tests/MP+pooncerelease+poacquireonce.litmus#L1-L28)（v6.18.60）

```c
C MP+pooncerelease+poacquireonce

(*
 * Result: Never
 *
 * This litmus test demonstrates that smp_store_release() and
 * smp_load_acquire() provide sufficient ordering for the message-passing
 * pattern.
 *)

{}

P0(int *buf, int *flag) // Producer
{
	WRITE_ONCE(*buf, 1);
	smp_store_release(flag, 1);
}

P1(int *buf, int *flag) // Consumer
{
	int r0;
	int r1;

	r0 = smp_load_acquire(flag);
	r1 = READ_ONCE(*buf);
}

exists (1:r0=1 /\ 1:r1=0) (* Bad outcome. *)
```

`exists` 子句描述"坏结果"，`Result: Never` 表示在 LKMM 下不可能出现。在 x86 上，即使去掉 acquire/release、只用 `READ_ONCE()` / `WRITE_ONCE()`，CPU 也不会产生该结果，但编译器仍可能重排——所以至少需要编译器屏障，而写成 acquire/release 则在所有架构上都正确。

旧式写法是写端 `smp_wmb()`、读端 `smp_rmb()`，两者成对使用（"屏障必须配对"是 `memory-barriers.txt` 的核心规则之一）。

### 存储缓冲（Store Buffering）

[`tools/memory-model/litmus-tests/SB+poonceonces.litmus:1-29`](../../linux/tools/memory-model/litmus-tests/SB+poonceonces.litmus#L1-L29)（v6.18.60）

```c
C SB+poonceonces

(*
 * Result: Sometimes
 *
 * This litmus test demonstrates that at least some ordering is required
 * to order the store-buffering pattern, where each process writes to the
 * variable that the preceding process reads.
 *)

{}

P0(int *x, int *y)
{
	int r0;

	WRITE_ONCE(*x, 1);
	r0 = READ_ONCE(*y);
}

P1(int *x, int *y)
{
	int r0;

	WRITE_ONCE(*y, 1);
	r0 = READ_ONCE(*x);
}

exists (0:r0=0 /\ 1:r0=0)
```

两个 CPU 各写一个变量再读对方的变量，"两者都读到 0"在 LKMM 下是允许的（`Result: Sometimes`），**在 x86 上也真实存在**：两次写都还在各自的存储缓冲区里。acquire/release 无法禁止这一结果，必须在写与读之间放 `smp_mb()`。

这一模式在内核中极其常见，形式是"我先声明自己要睡眠 / 等待，再检查条件；对方先设置条件，再检查是否有人等待"。前面已经见过的实例：

- `set_current_state()` 与 `try_to_wake_up()`（3.4 节），双方各有一个完整屏障；
- futex 的 `futex_hb_waiters_inc()` 与 `futex_hb_waiters_pending()`（7.8 节），注释中直接画出了这个 SB 图。

### 依赖与条件读

- **地址依赖**：`p = READ_ONCE(gp); x = p->a;` 第二次读的地址取决于第一次读的值，所有现代 CPU（Alpha 除外，内核在 `READ_ONCE()` 中为其补屏障）都不会重排这两次读。`rcu_dereference()` 正是利用这一点，读端零开销（7.9 节）。
- **控制依赖**：`if (READ_ONCE(x)) WRITE_ONCE(y, 1);` CPU 不会把依赖于条件的**写**提前到条件读之前执行（不能让推测执行的写对外可见），但依赖于条件的**读**可以提前。因此需要"读—读"有序时要补 `smp_acquire__after_ctrl_dep()`：

[`include/asm-generic/barrier.h:222-233`](../../linux/include/asm-generic/barrier.h#L222-L233)（v6.18.60）

```c
/**
 * smp_acquire__after_ctrl_dep() - Provide ACQUIRE ordering after a control dependency
 *
 * A control dependency provides a LOAD->STORE order, the additional RMB
 * provides LOAD->LOAD order, together they provide LOAD->{LOAD,STORE} order,
 * aka. (load)-ACQUIRE.
 *
 * Architectures that do not do load speculation can have this be barrier().
 */
#ifndef smp_acquire__after_ctrl_dep
#define smp_acquire__after_ctrl_dep()		smp_rmb()
#endif
```

自旋等待一个条件成立并获取 acquire 语义的常用写法是 `smp_cond_load_acquire()`：

[`include/asm-generic/barrier.h:246-273`](../../linux/include/asm-generic/barrier.h#L246-L273)（v6.18.60）

```c
#define smp_cond_load_relaxed(ptr, cond_expr) ({		\
	typeof(ptr) __PTR = (ptr);				\
	__unqual_scalar_typeof(*ptr) VAL;			\
	for (;;) {						\
		VAL = READ_ONCE(*__PTR);			\
		if (cond_expr)					\
			break;					\
		cpu_relax();					\
	}							\
	(typeof(*ptr))VAL;					\
})
#endif

/**
 * smp_cond_load_acquire() - (Spin) wait for cond with ACQUIRE ordering
 * @ptr: pointer to the variable to wait on
 * @cond: boolean expression to wait for
 *
 * Equivalent to using smp_load_acquire() on the condition variable but employs
 * the control dependency of the wait to reduce the barrier on many platforms.
 */
#ifndef smp_cond_load_acquire
#define smp_cond_load_acquire(ptr, cond_expr) ({		\
	__unqual_scalar_typeof(*ptr) _val;			\
	_val = smp_cond_load_relaxed(ptr, cond_expr);		\
	smp_acquire__after_ctrl_dep();				\
	(typeof(*ptr))_val;					\
})
```

它先用 `READ_ONCE()` 自旋，条件成立后补一个 `smp_acquire__after_ctrl_dep()`，省去了每次循环都执行 acquire 读的开销。arm64 会用 `WFE` 指令覆盖 `smp_cond_load_relaxed()`，让等待的 CPU 进入低功耗状态直到缓存行被修改；x86 使用通用版本，循环中的 `cpu_relax()` 即 `pause` 指令。qspinlock、qrwlock 的等待循环都建立在它之上（7.4、7.5 节）。

### 锁与 smp_mb__after_spinlock()

LKMM 规定 `spin_lock()` 是 acquire、`spin_unlock()` 是 release。这意味着"解锁之前的写"与"加锁之后的读"之间可以重排——锁并不是完整屏障。在需要完整屏障的地方，内核用 `smp_mb__after_spinlock()`：

[`include/linux/spinlock.h:124-178`](../../linux/include/linux/spinlock.h#L124-L178)（v6.18.60）

```c
/*
 * smp_mb__after_spinlock() provides the equivalent of a full memory barrier
 * between program-order earlier lock acquisitions and program-order later
 * memory accesses.
 *
 * This guarantees that the following two properties hold:
 *
 *   1) Given the snippet:
 *
 *	  { X = 0;  Y = 0; }
 *
 *	  CPU0				CPU1
 *
 *	  WRITE_ONCE(X, 1);		WRITE_ONCE(Y, 1);
 *	  spin_lock(S);			smp_mb();
 *	  smp_mb__after_spinlock();	r1 = READ_ONCE(X);
 *	  r0 = READ_ONCE(Y);
 *	  spin_unlock(S);
 *
 *      it is forbidden that CPU0 does not observe CPU1's store to Y (r0 = 0)
 *      and CPU1 does not observe CPU0's store to X (r1 = 0); see the comments
 *      preceding the call to smp_mb__after_spinlock() in __schedule() and in
 *      try_to_wake_up().
 *
 *   2) Given the snippet:
 *
 *  { X = 0;  Y = 0; }
 *
 *  CPU0		CPU1				CPU2
 *
 *  spin_lock(S);	spin_lock(S);			r1 = READ_ONCE(Y);
 *  WRITE_ONCE(X, 1);	smp_mb__after_spinlock();	smp_rmb();
 *  spin_unlock(S);	r0 = READ_ONCE(X);		r2 = READ_ONCE(X);
 *			WRITE_ONCE(Y, 1);
 *			spin_unlock(S);
 *
 *      it is forbidden that CPU0's critical section executes before CPU1's
 *      critical section (r0 = 1), CPU2 observes CPU1's store to Y (r1 = 1)
 *      and CPU2 does not observe CPU0's store to X (r2 = 0); see the comments
 *      preceding the calls to smp_rmb() in try_to_wake_up() for similar
 *      snippets but "projected" onto two CPUs.
 *
 * Property (2) upgrades the lock to an RCsc lock.
 *
 * Since most load-store architectures implement ACQUIRE with an smp_mb() after
 * the LL/SC loop, they need no further barriers. Similarly all our TSO
 * architectures imply an smp_mb() for each atomic instruction and equally don't
 * need more.
 *
 * Architectures that can implement ACQUIRE better need to take care.
 */
#ifndef smp_mb__after_spinlock
#define smp_mb__after_spinlock()	kcsan_mb()
#endif

```

x86 的加锁本身是 `lock cmpxchg`，已经是完整屏障，所以该宏只剩 KCSAN 钩子。`__schedule()` 和 `try_to_wake_up()` 都在获取锁后调用它（3.2、3.4 节）。

## 关键细节 / 设计考量

### 屏障清单

| 原语 | 约束 | x86_64 实现 |
|---|---|---|
| `barrier()` | 仅编译器 | 空 `asm` + `"memory"` |
| `READ_ONCE()` / `WRITE_ONCE()` | 单次、不撕裂 | `volatile` 访问 |
| `smp_rmb()` / `smp_wmb()` | 读—读 / 写—写 | `barrier()` |
| `smp_mb()` | 全部 | `lock addl $0,-4(%rsp)` |
| `smp_store_release()` / `smp_load_acquire()` | release / acquire | `barrier()` + 普通读写 |
| `smp_store_mb()` | 写 + 完整屏障 | `xchg` |
| `smp_mb__before/after_atomic()` | 增强无序原子操作 | 空 |
| `mb()` / `rmb()` / `wmb()` | 含设备访问 | `mfence` / `lfence` / `sfence` |
| `dma_rmb()` / `dma_wmb()` | 一致性 DMA 内存 | `barrier()` |

*表 7-4：内存屏障原语在 x86_64 上的实现*

### 为什么 x86 的 rmb() 需要 lfence 而 smp_rmb() 不需要

`rmb()` 用于设备访问，需要考虑写合并（WC）内存、非临时读写和 MMIO 等不遵循 TSO 的弱序访问，因此必须使用真正的栅栏指令。`smp_rmb()` 只处理普通回写（WB）内存上的 CPU 间同步，TSO 已经保证读—读有序。

### 不要用屏障"修补"数据竞争

屏障只约束同一 CPU 上访问之间的顺序，本身不会让任何东西"立即可见"，也不提供互斥。用屏障替代锁的代码必须能写成清晰的配对模式（MP、SB 等），并在注释中写明与之配对的另一端。`checkpatch.pl` 会对没有注释的屏障给出警告。

## 动手实验

### 用 herd7 运行 litmus 测试

```bash
# 需要安装 herdtools7（opam install herdtools7）
cd linux/tools/memory-model
herd7 -conf linux-kernel.cfg litmus-tests/SB+poonceonces.litmus
herd7 -conf linux-kernel.cfg litmus-tests/SB+fencembonceonces.litmus
herd7 -conf linux-kernel.cfg litmus-tests/MP+pooncerelease+poacquireonce.litmus
```

第一个测试的输出中 `Observation ... Sometimes`，第二个（加了 `smp_mb()`）为 `Never`。

### 在 x86 上复现写后读重排

```c
// 实验代码（用户态，非内核源码）：两个线程反复执行 SB 模式，统计 r0 == r1 == 0 的次数
#include <pthread.h>
#include <stdio.h>
static volatile int x, y, r0, r1, go0, go1, done0, done1;
static void *t0(void *a) { for (;;) { while (!go0); go0 = 0;
	x = 1; /* asm volatile("mfence" ::: "memory"); */ r0 = y; done0 = 1; } }
static void *t1(void *a) { for (;;) { while (!go1); go1 = 0;
	y = 1; /* asm volatile("mfence" ::: "memory"); */ r1 = x; done1 = 1; } }
int main(void) {
	pthread_t a, b; long hit = 0;
	pthread_create(&a, 0, t0, 0); pthread_create(&b, 0, t1, 0);
	for (long i = 0; i < 1000000; i++) {
		x = y = 0; done0 = done1 = 0; go0 = go1 = 1;
		while (!done0 || !done1);
		if (r0 == 0 && r1 == 0) hit++;
	}
	printf("both zero: %ld\n", hit);
}
```

在多核 x86 上通常能观察到非零计数；取消注释中的 `mfence` 后计数变为 0。

## 小结

- 重排来自编译器与 CPU 两个层次：`barrier()` / `READ_ONCE()` / `WRITE_ONCE()` 约束编译器，`smp_*` 屏障约束 CPU。
- 并发访问的普通变量必须使用 `READ_ONCE()` / `WRITE_ONCE()`，否则在 LKMM 中构成数据竞争。
- x86 TSO 只允许"写后读"重排；因此 `smp_rmb()`、`smp_wmb()`、acquire/release 在 x86 上都只是编译器屏障，`smp_mb()` 则用 `lock addl` 实现。
- 消息传递模式用 release/acquire（或 `smp_wmb()`/`smp_rmb()` 配对）解决；存储缓冲模式必须用完整屏障，内核中的"睡眠—唤醒"协议都属于此类。
- 锁的加锁 / 解锁只是 acquire / release，需要完整屏障时使用 `smp_mb__after_spinlock()`。

## 参考

- `linux/Documentation/memory-barriers.txt`
- `linux/tools/memory-model/Documentation/explanation.txt`
- `linux/tools/memory-model/Documentation/recipes.txt`
- Jade Alglave, Luc Maranget, Paul E. McKenney, Andrea Parri, Alan Stern, "Frightening Small Children and Disconcerting Grown-ups: Concurrency in the Linux Kernel", ASPLOS 2018
- Paul E. McKenney, *Is Parallel Programming Hard, And, If So, What Can You Do About It?*，第 15 章
