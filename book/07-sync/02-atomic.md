# 7.2 原子操作、位操作与引用计数

> **本节源码**：[`include/linux/types.h`](../../linux/include/linux/types.h)、[`arch/x86/include/asm/atomic.h`](../../linux/arch/x86/include/asm/atomic.h)、[`arch/x86/include/asm/cmpxchg.h`](../../linux/arch/x86/include/asm/cmpxchg.h)、[`arch/x86/include/asm/alternative.h`](../../linux/arch/x86/include/asm/alternative.h)、[`include/linux/atomic/atomic-instrumented.h`](../../linux/include/linux/atomic/atomic-instrumented.h)、[`include/linux/atomic/atomic-arch-fallback.h`](../../linux/include/linux/atomic/atomic-arch-fallback.h)、[`arch/x86/include/asm/bitops.h`](../../linux/arch/x86/include/asm/bitops.h)、[`include/linux/refcount.h`](../../linux/include/linux/refcount.h)、[`lib/refcount.c`](../../linux/lib/refcount.c)
> **关键函数**：`atomic_inc()`、`atomic_try_cmpxchg()`、`raw_atomic_fetch_add_unless()`、`set_bit()`、`test_and_set_bit()`、`refcount_inc()`、`refcount_dec_and_test()`、`refcount_warn_saturate()`
> **前置知识**：7.1 并发来源与执行上下文

## 概述

C 语言的 `x++` 在机器层面是"读—改—写"三步。两个 CPU 同时执行时，可能都读到旧值、各自加一、先后写回，结果只加了一次。**原子操作**（Atomic Operation）保证一次读改写对其他 CPU 而言不可分割，是所有锁和无锁算法的基础构件。

内核为原子操作提供了三层封装：面向使用者的 `atomic_*()` 带有 KASAN / KCSAN 插桩；`raw_atomic_*()` 负责用已有的架构原语"拼出"缺失的操作；最底层的 `arch_atomic_*()` 由各架构用内联汇编实现。在 x86_64 上，几乎所有读改写原子操作都对应一条带 `lock` 前缀的指令。

在原子整数之上，内核还提供了原子位操作和专用于对象生命周期管理的 `refcount_t`。后者在溢出或下溢时"饱和"而不回绕，把引用计数错误从释放后使用（use-after-free）漏洞降级为内存泄漏。

## 核心数据结构

原子类型只是对整数的一层包装，目的是让编译器拒绝对它们做普通算术：

[`include/linux/types.h:181-190`](../../linux/include/linux/types.h#L181-L190)（v6.18.60）

```c
typedef struct {
	int counter;
} atomic_t;

#define ATOMIC_INIT(i) { (i) }

#ifdef CONFIG_64BIT
typedef struct {
	s64 counter;
} atomic64_t;
```

`atomic_long_t` 在 64 位架构上就是 `atomic64_t`（[`include/linux/atomic/atomic-long.h:13`](../../linux/include/linux/atomic/atomic-long.h#L13)）。`refcount_t` 则再包一层 `atomic_t`：`typedef struct refcount_struct { atomic_t refs; } refcount_t;`（`include/linux/refcount_types.h`）。

| 类型 | 宽度 | 典型用途 |
|---|---|---|
| `atomic_t` | 32 位 | 计数器、状态字（如 qspinlock 的 `val`） |
| `atomic64_t` / `atomic_long_t` | 64 位 | 指针宽度的状态（如 `mutex->owner`、`rwsem->count`） |
| `refcount_t` | 32 位 | 对象引用计数 |
| `unsigned long` 位图 | 64 位一组 | `set_bit()` 等原子位操作 |

## 主流程分析

### x86 的原子指令

x86 对齐的普通读写本身就是原子的（不会被撕裂），因此 `atomic_read()` / `atomic_set()` 只需要阻止编译器优化：

[`arch/x86/include/asm/atomic.h:17-36`](../../linux/arch/x86/include/asm/atomic.h#L17-L36)（v6.18.60）

```c
static __always_inline int arch_atomic_read(const atomic_t *v)
{
	/*
	 * Note for KASAN: we deliberately don't use READ_ONCE_NOCHECK() here,
	 * it's non-inlined function that increases binary size and stack usage.
	 */
	return __READ_ONCE((v)->counter);
}

static __always_inline void arch_atomic_set(atomic_t *v, int i)
{
	__WRITE_ONCE(v->counter, i);
}

static __always_inline void arch_atomic_add(int i, atomic_t *v)
{
	asm_inline volatile(LOCK_PREFIX "addl %1, %0"
		     : "+m" (v->counter)
		     : "ir" (i) : "memory");
}
```

`__READ_ONCE()` / `__WRITE_ONCE()` 通过 `volatile` 访问保证编译器恰好生成一次内存访问（7.3 节）。读改写操作 `arch_atomic_add()` 则使用 `LOCK_PREFIX "addl"`。`lock` 前缀让 CPU 在执行这条指令期间独占对应的缓存行（现代处理器通过缓存一致性协议锁定缓存行，而非锁总线），从而保证读改写对其他 CPU 不可分割，并且它同时是一个**完整的内存屏障**。

`LOCK_PREFIX` 并不是简单的字符串 `"lock "`：

[`arch/x86/include/asm/alternative.h:44-57`](../../linux/arch/x86/include/asm/alternative.h#L44-L57)（v6.18.60）

```c
#ifdef CONFIG_SMP
#define LOCK_PREFIX_HERE \
		".pushsection .smp_locks,\"a\"\n"	\
		".balign 4\n"				\
		".long 671f - .\n" /* offset */		\
		".popsection\n"				\
		"671:"

#define LOCK_PREFIX LOCK_PREFIX_HERE "\n\tlock "

#else /* ! CONFIG_SMP */
#define LOCK_PREFIX_HERE ""
#define LOCK_PREFIX ""
#endif
```

每个 `lock` 前缀的地址都被记录到 `.smp_locks` 段。如果系统只有一个 CPU 在线，`alternatives_smp_unlock()`（[`arch/x86/kernel/alternative.c:2122`](../../linux/arch/x86/kernel/alternative.c#L2122)）会把这些 `lock` 前缀改写为 `ds` 前缀（一个无副作用的字节），省去锁定开销；CPU 热插拔上线第二个 CPU 时再改回来。

### 带返回值的操作与条件码

许多原子操作需要返回"结果是否为零 / 为负"。x86 的算术指令会设置标志位，内核用 `GEN_UNARY_RMWcc()` / `GEN_BINARY_RMWcc()` 宏配合 GCC/Clang 的 `asm goto` 或 `=@cc` 输出约束，直接把标志位转成布尔值，避免再读一次内存：

[`arch/x86/include/asm/atomic.h:65-97`](../../linux/arch/x86/include/asm/atomic.h#L65-L97)（v6.18.60）

```c
static __always_inline bool arch_atomic_dec_and_test(atomic_t *v)
{
	return GEN_UNARY_RMWcc(LOCK_PREFIX "decl", v->counter, e);
}
#define arch_atomic_dec_and_test arch_atomic_dec_and_test

static __always_inline bool arch_atomic_inc_and_test(atomic_t *v)
{
	return GEN_UNARY_RMWcc(LOCK_PREFIX "incl", v->counter, e);
}
#define arch_atomic_inc_and_test arch_atomic_inc_and_test

static __always_inline bool arch_atomic_add_negative(int i, atomic_t *v)
{
	return GEN_BINARY_RMWcc(LOCK_PREFIX "addl", v->counter, s, "er", i);
}
#define arch_atomic_add_negative arch_atomic_add_negative

static __always_inline int arch_atomic_add_return(int i, atomic_t *v)
{
	return i + xadd(&v->counter, i);
}
#define arch_atomic_add_return arch_atomic_add_return

#define arch_atomic_sub_return(i, v) arch_atomic_add_return(-(i), v)

static __always_inline int arch_atomic_fetch_add(int i, atomic_t *v)
{
	return xadd(&v->counter, i);
}
#define arch_atomic_fetch_add arch_atomic_fetch_add

#define arch_atomic_fetch_sub(i, v) arch_atomic_fetch_add(-(i), v)
```

`atomic_dec_and_test()` 是"减一后是否为零"，编译为 `lock decl` 加一个 `sete`/`je`；`atomic_add_return()` 则用 `xadd`（交换并相加）一次拿到旧值。

### 比较并交换：try_cmpxchg

比较并交换（Compare-And-Swap，CAS）是构建任意原子操作的通用原语：若内存中的值等于预期，则写入新值。x86 的 `cmpxchg` 指令隐式使用 `%rax` 存放预期值，失败时把内存中的当前值写回 `%rax`。内核的 `try_cmpxchg()` 把这一特性暴露出来：

[`arch/x86/include/asm/cmpxchg.h:158-215`](../../linux/arch/x86/include/asm/cmpxchg.h#L158-L215)（v6.18.60）

```c
#define __raw_try_cmpxchg(_ptr, _pold, _new, size, lock)		\
({									\
	bool success;							\
	__typeof__(_ptr) _old = (__typeof__(_ptr))(_pold);		\
	__typeof__(*(_ptr)) __old = *_old;				\
	__typeof__(*(_ptr)) __new = (_new);				\
	switch (size) {							\
	case __X86_CASE_B:						\
	{								\
		volatile u8 *__ptr = (volatile u8 *)(_ptr);		\
		asm_inline volatile(lock "cmpxchgb %[new], %[ptr]"	\
			     : "=@ccz" (success),			\
			       [ptr] "+m" (*__ptr),			\
			       [old] "+a" (__old)			\
			     : [new] "q" (__new)			\
			     : "memory");				\
		break;							\
	}								\
	case __X86_CASE_W:						\
	{								\
		volatile u16 *__ptr = (volatile u16 *)(_ptr);		\
		asm_inline volatile(lock "cmpxchgw %[new], %[ptr]"	\
			     : "=@ccz" (success),			\
			       [ptr] "+m" (*__ptr),			\
			       [old] "+a" (__old)			\
			     : [new] "r" (__new)			\
			     : "memory");				\
		break;							\
	}								\
	case __X86_CASE_L:						\
	{								\
		volatile u32 *__ptr = (volatile u32 *)(_ptr);		\
		asm_inline volatile(lock "cmpxchgl %[new], %[ptr]"	\
			     : "=@ccz" (success),			\
			       [ptr] "+m" (*__ptr),			\
			       [old] "+a" (__old)			\
			     : [new] "r" (__new)			\
			     : "memory");				\
		break;							\
	}								\
	case __X86_CASE_Q:						\
	{								\
		volatile u64 *__ptr = (volatile u64 *)(_ptr);		\
		asm_inline volatile(lock "cmpxchgq %[new], %[ptr]"	\
			     : "=@ccz" (success),			\
			       [ptr] "+m" (*__ptr),			\
			       [old] "+a" (__old)			\
			     : [new] "r" (__new)			\
			     : "memory");				\
		break;							\
	}								\
	default:							\
		__cmpxchg_wrong_size();					\
	}								\
	if (unlikely(!success))						\
		*_old = __old;						\
	likely(success);						\
})
```

与传统的 `cmpxchg()`（返回旧值，由调用者比较）相比，`try_cmpxchg()` 有两个优点：返回值直接取自 `ZF` 标志（`=@ccz`）；失败时自动把当前值写回 `*_pold`，调用者的循环无需重新读取。典型写法如下：

[`include/linux/atomic/atomic-arch-fallback.h:2420-2435`](../../linux/include/linux/atomic/atomic-arch-fallback.h#L2420-L2435)（v6.18.60）

```c
static __always_inline int
raw_atomic_fetch_add_unless(atomic_t *v, int a, int u)
{
#if defined(arch_atomic_fetch_add_unless)
	return arch_atomic_fetch_add_unless(v, a, u);
#else
	int c = raw_atomic_read(v);

	do {
		if (unlikely(c == u))
			break;
	} while (!raw_atomic_try_cmpxchg(v, &c, c + a));

	return c;
#endif
}
```

这就是通用的"读—计算—CAS—失败重试"模式。`refcount_t`、`mutex` 快速路径、qspinlock 都大量使用 `try_cmpxchg` 系列。

除 32/64 位版本外，x86_64 还提供 `cmpxchg16b`（`CONFIG_HAVE_CMPXCHG_DOUBLE=y`），可以原子地比较并交换两个相邻的 64 位字。SLUB 用它同时更新 `freelist` 指针和 `tid` 计数（4.5 节），以解决 ABA 问题。

### 三层封装

使用者调用的 `atomic_inc()` 位于自动生成的 `atomic-instrumented.h`：

[`include/linux/atomic/atomic-instrumented.h:432-437`](../../linux/include/linux/atomic/atomic-instrumented.h#L432-L437)（v6.18.60）

```c
static __always_inline void
atomic_inc(atomic_t *v)
{
	instrument_atomic_read_write(v, sizeof(*v));
	raw_atomic_inc(v);
}
```

`instrument_atomic_read_write()` 在开启 KASAN / KCSAN 时检查地址合法性和数据竞争，在本书配置下（两者均未开启）为空。`raw_atomic_inc()` 位于 `atomic-arch-fallback.h`：若架构定义了 `arch_atomic_inc` 就直接调用，否则用 `arch_atomic_add(1, v)` 代替。这三个头文件都由 `scripts/atomic/gen-atomics.sh` 生成，不要手工修改。

`noinstr` 代码（如中断入口的最早阶段）不能被插桩，应直接使用 `raw_atomic_*()`。

### 内存序变体

除了完整版本，大多数原子操作还有 `_relaxed`、`_acquire`、`_release` 三种变体。`Documentation/atomic_t.txt` 规定了它们的语义：

| 操作 | 默认内存序 |
|---|---|
| `atomic_read()` / `atomic_set()` | 无序（relaxed） |
| 不返回值的读改写（`atomic_inc()`、`atomic_add()`） | 无序 |
| 返回值的读改写（`atomic_inc_return()`、`atomic_xchg()`、`atomic_cmpxchg()`） | 完全有序（相当于前后各一个 `smp_mb()`） |
| `_relaxed` 后缀 | 无序 |
| `_acquire` 后缀 | 读部分具有 acquire 语义 |
| `_release` 后缀 | 写部分具有 release 语义 |
| `atomic_try_cmpxchg()` 失败时 | 无序 |

在 x86 上，所有 `lock` 指令都是完全屏障，四种变体生成相同的代码；但在 arm64 等弱内存序架构上它们有实质差别。编写可移植代码时必须使用语义正确的最弱变体，并且不能假设"不返回值的原子操作有序"。需要时可以用 `smp_mb__before_atomic()` / `smp_mb__after_atomic()` 补齐，它们在 x86 上为空（[`arch/x86/include/asm/barrier.h:75-76`](../../linux/arch/x86/include/asm/barrier.h#L75-L76)）。内存序的完整讨论见 7.3 节。

### 原子位操作

`set_bit()`、`clear_bit()`、`test_and_set_bit()` 等对 `unsigned long` 位图中的单个位做原子操作：

[`arch/x86/include/asm/bitops.h:51-69`](../../linux/arch/x86/include/asm/bitops.h#L51-L69)（v6.18.60）

```c
static __always_inline void
arch_set_bit(long nr, volatile unsigned long *addr)
{
	if (__builtin_constant_p(nr)) {
		asm_inline volatile(LOCK_PREFIX "orb %b1,%0"
			: CONST_MASK_ADDR(nr, addr)
			: "iq" (CONST_MASK(nr))
			: "memory");
	} else {
		asm_inline volatile(LOCK_PREFIX __ASM_SIZE(bts) " %1,%0"
			: : RLONG_ADDR(addr), "Ir" (nr) : "memory");
	}
}

static __always_inline void
arch___set_bit(unsigned long nr, volatile unsigned long *addr)
{
	asm volatile(__ASM_SIZE(bts) " %1,%0" : : ADDR, "Ir" (nr) : "memory");
}
```

位号为常量时编译为 `lock orb`，只操作所在字节；否则使用 `lock bts`。带双下划线的 `__set_bit()` 是非原子版本，用于调用者已持锁或对象尚未发布的场合。`test_and_set_bit()` 用 `lock bts` 并通过进位标志返回旧值（[`arch/x86/include/asm/bitops.h:133-137`](../../linux/arch/x86/include/asm/bitops.h#L133-L137)），是实现"位锁"（如 `PG_locked`、`bit_spin_lock()`）的基础。

按 `atomic_t.txt` 的约定，`set_bit()` 等不返回值的位操作是无序的；`test_and_set_bit()` 完全有序；`test_and_set_bit_lock()` / `clear_bit_unlock()` 分别提供 acquire / release 语义，专供位锁使用。

## 引用计数：refcount_t

### 为什么不用 atomic_t

用 `atomic_t` 做引用计数有一个致命问题：如果某条路径多减了一次（或攻击者能反复触发某个"多加一次"的漏洞直到 32 位回绕），计数会提前归零，对象被释放而仍有使用者，形成可被利用的释放后使用漏洞。`refcount_t`（v4.11 引入）的目标是让这类错误**无法越过零点**。

### 饱和语义

`include/linux/refcount.h` 开头的注释给出了设计：

[`include/linux/refcount.h:8-27`](../../linux/include/linux/refcount.h#L8-L27)（v6.18.60）

```c
 * Saturation semantics
 * ====================
 *
 * refcount_t differs from atomic_t in that the counter saturates at
 * REFCOUNT_SATURATED and will not move once there. This avoids wrapping the
 * counter and causing 'spurious' use-after-free issues. In order to avoid the
 * cost associated with introducing cmpxchg() loops into all of the saturating
 * operations, we temporarily allow the counter to take on an unchecked value
 * and then explicitly set it to REFCOUNT_SATURATED on detecting that underflow
 * or overflow has occurred. Although this is racy when multiple threads
 * access the refcount concurrently, by placing REFCOUNT_SATURATED roughly
 * equidistant from 0 and INT_MAX we minimise the scope for error:
 *
 * 	                           INT_MAX     REFCOUNT_SATURATED   UINT_MAX
 *   0                          (0x7fff_ffff)    (0xc000_0000)    (0xffff_ffff)
 *   +--------------------------------+----------------+----------------+
 *                                     <---------- bad value! ---------->
 *
 * (in a signed view of the world, the "bad value" range corresponds to
 * a negative counter value).
```

关键在于 `REFCOUNT_SATURATED = INT_MIN / 2`（即 `0xc000_0000`）：任何检测到的异常（从 0 增加、溢出为负、减到负数）都会把计数直接设为这个"饱和值"，此后它既不会再归零，也不会回到正数，对象只会泄漏而不会被提前释放。之所以选在负数区间的正中，是为了容纳"检测到异常"与"写入饱和值"之间其他 CPU 的并发增减。

### 增加：不用 cmpxchg 循环

`refcount_inc()` 最终调用 `__refcount_add()`：

[`include/linux/refcount.h:280-292`](../../linux/include/linux/refcount.h#L280-L292)（v6.18.60）

```c
static inline __signed_wrap
void __refcount_add(int i, refcount_t *r, int *oldp)
{
	int old = atomic_fetch_add_relaxed(i, &r->refs);

	if (oldp)
		*oldp = old;

	if (unlikely(!old))
		refcount_warn_saturate(r, REFCOUNT_ADD_UAF);
	else if (unlikely(old < 0 || old + i < 0))
		refcount_warn_saturate(r, REFCOUNT_ADD_OVF);
}
```

增加操作只用一条 `atomic_fetch_add_relaxed()`（`lock xadd`），**事后**检查旧值：旧值为 0 说明在给已释放对象加引用；旧值为负或结果为负说明溢出。这比 cmpxchg 循环快得多，代价是存在短暂的"不检查窗口"，注释中已分析其安全边界。

`refcount_inc_not_zero()` 则必须用 cmpxchg 循环，因为它要保证"计数为 0 时绝不增加"——这是 RCU 查找中"拿到指针后尝试获取引用"的标准操作：

[`include/linux/refcount.h:172-189`](../../linux/include/linux/refcount.h#L172-L189)（v6.18.60）

```c

static inline __must_check __signed_wrap
bool __refcount_add_not_zero(int i, refcount_t *r, int *oldp)
{
	int old = refcount_read(r);

	do {
		if (!old)
			break;
	} while (!atomic_try_cmpxchg_relaxed(&r->refs, &old, old + i));

	if (oldp)
		*oldp = old;

	if (unlikely(old < 0 || old + i < 0))
		refcount_warn_saturate(r, REFCOUNT_ADD_NOT_ZERO_OVF);

	return old;
```

### 减少：release + acquire

[`include/linux/refcount.h:386-403`](../../linux/include/linux/refcount.h#L386-L403)（v6.18.60）

```c
static inline __must_check __signed_wrap
bool __refcount_sub_and_test(int i, refcount_t *r, int *oldp)
{
	int old = atomic_fetch_sub_release(i, &r->refs);

	if (oldp)
		*oldp = old;

	if (old > 0 && old == i) {
		smp_acquire__after_ctrl_dep();
		return true;
	}

	if (unlikely(old <= 0 || old - i < 0))
		refcount_warn_saturate(r, REFCOUNT_SUB_UAF);

	return false;
}
```

减引用使用 `atomic_fetch_sub_release()`：release 语义保证本 CPU 在减引用之前对对象的所有读写都已完成，避免"已经放手但还在访问"。当减到零时，再用 `smp_acquire__after_ctrl_dep()` 补一个 acquire，保证随后的释放操作看到其他 CPU 在各自减引用之前的全部写入。这是"最后一个放手者负责释放"模式所需的最小内存序。

一旦检测到异常，`refcount_warn_saturate()` 写入饱和值并打印告警：

[`lib/refcount.c:13-36`](../../linux/lib/refcount.c#L13-L36)（v6.18.60）

```c
void refcount_warn_saturate(refcount_t *r, enum refcount_saturation_type t)
{
	refcount_set(r, REFCOUNT_SATURATED);

	switch (t) {
	case REFCOUNT_ADD_NOT_ZERO_OVF:
		REFCOUNT_WARN("saturated; leaking memory");
		break;
	case REFCOUNT_ADD_OVF:
		REFCOUNT_WARN("saturated; leaking memory");
		break;
	case REFCOUNT_ADD_UAF:
		REFCOUNT_WARN("addition on 0; use-after-free");
		break;
	case REFCOUNT_SUB_UAF:
		REFCOUNT_WARN("underflow; use-after-free");
		break;
	case REFCOUNT_DEC_LEAK:
		REFCOUNT_WARN("decrement hit 0; leaking memory");
		break;
	default:
		REFCOUNT_WARN("unknown saturation event!?");
	}
}
```

### 内存序约定

| 操作 | 内存序 |
|---|---|
| `refcount_inc()` / `refcount_add()` | 无序（调用者已经持有引用，或经由锁 / RCU 获得指针） |
| `refcount_inc_not_zero()` | 无序，但提供控制依赖，保证后续写不会提前 |
| `refcount_dec_and_test()` / `refcount_sub_and_test()` | release；返回真时额外 acquire |
| `refcount_inc_not_zero_acquire()` / `refcount_set_release()` | 用于 `SLAB_TYPESAFE_BY_RCU` 等对象内存可能被复用的场景 |

## 关键细节 / 设计考量

### volatile 不等于原子

`volatile` 只阻止编译器省略或合并访问，不保证读改写不可分割，也不提供 CPU 层面的内存序。`volatile int x; x++;` 在多核上依然会丢失更新。内核规定不得用 `volatile` 修饰共享变量（`Documentation/process/volatile-considered-harmful.rst`），而应使用 `READ_ONCE()` / `WRITE_ONCE()` 或原子类型。

### 原子操作的代价

一条 `lock` 指令在缓存行已位于本 CPU 时约需十几到二十几个周期；若缓存行在其他 CPU 上，需要经过一致性协议获取所有权，代价可达数百周期。多个 CPU 频繁修改同一个原子变量会造成缓存行"乒乓"。因此内核对高频计数器优先使用 per-CPU 计数（`percpu_counter`、`this_cpu_inc()`），对高频引用计数使用 `percpu_ref` 或 `rcuref_t`（v6.4 引入，[`include/linux/types.h:193-195`](../../linux/include/linux/types.h#L193-L195)），只在需要精确值时才汇总。

### 位操作的非原子版本

`__set_bit()` 与 `set_bit()` 的区别不仅是性能：在同一个字中，若一个 CPU 用 `__set_bit()` 修改第 0 位，另一个 CPU 同时用 `set_bit()` 修改第 1 位，非原子的读改写可能把对方的修改覆盖掉。同一个字的所有位只要有并发修改，就必须全部使用原子版本。

## 动手实验

查看 `atomic_inc()` 与 `refcount_inc()` 生成的指令（在 `linux/` 目录下已有构建产物时）：

```bash
cd linux
# 反汇编某个使用 refcount_inc 的函数，例如 get_task_struct 的内联展开点
objdump -d --no-show-raw-insn kernel/fork.o | grep -B2 -A6 "lock xadd" | head -40
```

用 bpftrace 观察引用计数饱和告警（正常系统中不应触发）：

```bash
sudo bpftrace -e 'kprobe:refcount_warn_saturate { printf("%s type=%d\n", comm, arg1); print(kstack); }'
```

## 小结

- 原子操作分三层：带插桩的 `atomic_*()`、负责回退拼装的 `raw_atomic_*()`、架构实现的 `arch_atomic_*()`；后两层头文件由脚本生成。
- x86_64 上读改写原子操作对应一条 `lock` 前缀指令，它同时是完整内存屏障；`.smp_locks` 段允许单 CPU 时去掉 `lock` 前缀。
- `try_cmpxchg()` 失败时自动回写当前值并以 `ZF` 返回结果，是"读—计算—CAS—重试"循环的标准写法。
- 不返回值的原子操作与 `set_bit()` 在内核内存模型中是无序的；返回值的操作完全有序；`_relaxed/_acquire/_release` 提供更细的控制。
- `refcount_t` 以饱和代替回绕，把引用计数错误限制为内存泄漏；增加用 `fetch_add` 事后检查，减少使用 release 语义，减到零时补 acquire。

## 参考

- `linux/Documentation/atomic_t.txt`
- `linux/Documentation/atomic_bitops.txt`
- `linux/Documentation/core-api/refcount-vs-atomic.rst`
- `linux/Documentation/process/volatile-considered-harmful.rst`
- `linux/scripts/atomic/`（原子操作头文件生成脚本）
- Elena Reshetova 等, "refcount_t: introduce a special purpose refcount type", v4.11
