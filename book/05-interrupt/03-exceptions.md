# 5.3 CPU 异常处理：陷阱、故障与异常修复

> **本节源码**：`arch/x86/kernel/traps.c`、`arch/x86/include/asm/traps.h`、`arch/x86/include/asm/bug.h`、`arch/x86/include/asm/extable.h`、`arch/x86/include/asm/extable_fixup_types.h`、`arch/x86/include/asm/asm.h`、`arch/x86/include/asm/uaccess.h`、`arch/x86/mm/extable.c`、`kernel/extable.c`、`lib/extable.c`、`arch/x86/kernel/dumpstack.c`
> **关键函数**：`do_error_trap()`、`do_trap()`、`exc_invalid_op()`、`handle_bug()`、`exc_general_protection()`、`exc_int3()`、`exc_double_fault()`、`fixup_exception()`、`search_exception_tables()`、`die()`、`notify_die()`
> **前置知识**：5.1 x86_64 中断与异常体系、5.2 中断入口与出口、4.8 页表与缺页异常

## 概述

5.2 节讲的是"如何进入和退出"，本节讲"进来之后做什么"。CPU 异常的处理策略首先取决于**异常发生在哪里**：

- **用户态**引起的异常，大多数意味着用户程序有错误（除零、非法指令、越权访问），内核的做法是向进程发送信号（`SIGFPE`、`SIGILL`、`SIGSEGV`、`SIGTRAP`……），由进程自己决定处理还是终止。
- **内核态**引起的异常，原则上意味着内核有 bug，内核会打印 Oops 并终止当前任务，甚至直接 panic。但有大量例外是**预期之内**的：`copy_from_user()` 访问了无效的用户地址、`rdmsr_safe()` 读了不存在的 MSR、`WARN()` 主动触发的 `ud2`、kprobes 插入的 `int3`……这些都必须被识别出来并"修复"后继续执行。

内核为此提供了几套机制：**异常表**（Exception Table）记录"允许出错的指令及其修复地址"；**bug 表**（`__bug_table`）记录每个 `BUG()` / `WARN()` 的位置；**die 通知链**让 kprobes、kgdb 等子系统有机会接管异常。本节先看通用的处理骨架，再逐个分析几种有代表性的异常，最后讨论异常表和 Oops。缺页异常（#PF）已在 4.8 节详细分析，NMI 与 #MC 留给 5.4 节。

## 通用处理骨架

### do_error_trap 与 do_trap

大多数"简单"异常——#DE、#OF、#BR、#UD、#TS、#NP、#SS、#AC——共用一套逻辑：

```c
// arch/x86/kernel/traps.c:259-270（v6.18.60）
static void do_error_trap(struct pt_regs *regs, long error_code, char *str,
	unsigned long trapnr, int signr, int sicode, void __user *addr)
{
	RCU_LOCKDEP_WARN(!rcu_is_watching(), "entry code didn't wake RCU");

	if (notify_die(DIE_TRAP, str, regs, error_code, trapnr, signr) !=
			NOTIFY_STOP) {
		cond_local_irq_enable(regs);
		do_trap(trapnr, signr, str, regs, error_code, sicode, addr);
		cond_local_irq_disable(regs);
	}
}
```

1. 先调用 `notify_die()` 遍历 die 通知链（`die_chain`），kgdb、kprobes 等可以在此返回 `NOTIFY_STOP` 截获异常。
2. `cond_local_irq_enable()`（`arch/x86/include/asm/traps.h:48-52`）：**如果被打断的上下文原本开着中断**，就重新打开中断。异常属于同步事件，处理它可能需要较长时间（发送信号要获取锁），没必要一直关中断；反过来，如果原上下文关着中断，处理函数也必须保持关闭，否则会破坏原上下文的假设。
3. 调用 `do_trap()` 分发。

```c
// arch/x86/kernel/traps.c:186-256（v6.18.60）
static nokprobe_inline int
do_trap_no_signal(struct task_struct *tsk, int trapnr, const char *str,
		  struct pt_regs *regs,	long error_code)
{
	if (v8086_mode(regs)) {
		...
	} else if (!user_mode(regs)) {
		if (fixup_exception(regs, trapnr, error_code, 0))
			return 0;

		tsk->thread.error_code = error_code;
		tsk->thread.trap_nr = trapnr;
		die(str, regs, error_code);
	} else {
		if (fixup_vdso_exception(regs, trapnr, error_code, 0))
			return 0;
	}
	...
	tsk->thread.error_code = error_code;
	tsk->thread.trap_nr = trapnr;

	return -1;
}
...
static void
do_trap(int trapnr, int signr, char *str, struct pt_regs *regs,
	long error_code, int sicode, void __user *addr)
{
	struct task_struct *tsk = current;

	if (!do_trap_no_signal(tsk, trapnr, str, regs, error_code))
		return;

	show_signal(tsk, signr, "trap ", str, regs, error_code);

	if (!sicode)
		force_sig(signr);
	else
		force_sig_fault(signr, sicode, addr);
}
```

`do_trap_no_signal()` 的三个分支对应三种来源：

- **内核态**：先查异常表，命中则修复返回；否则调用 `die()` 打印 Oops。
- **用户态**：先尝试 `fixup_vdso_exception()`——vDSO 中的 SGX `__vdso_sgx_enter_enclave()` 会在异常表中登记修复点，以便把 enclave 中的异常以返回值形式报告给用户态；否则准备发送信号。
- **vm86 模式**：仅 32 位内核适用。

`error_code` 与 `trap_nr` 被记入 `thread_struct`，信号处理时会通过 `sigcontext` 的 `err`、`trapno` 字段传给用户态。`show_signal()` 在 `show_unhandled_signals` 开启（默认值 1，见 `arch/x86/mm/fault.c:1046`，可通过 sysctl `debug.exception-trace` 修改）且进程没有处理该信号时，在内核日志中打印一行 `traps: xxx[pid] trap divide error ip:... sp:... error:...`。

具体的异常入口只是带上不同参数调用 `do_error_trap()`：

```c
// arch/x86/kernel/traps.c:287-296（v6.18.60）
DEFINE_IDTENTRY(exc_divide_error)
{
	do_error_trap(regs, 0, "divide error", X86_TRAP_DE, SIGFPE,
		      FPE_INTDIV, error_get_trap_addr(regs));
}

DEFINE_IDTENTRY(exc_overflow)
{
	do_error_trap(regs, 0, "overflow", X86_TRAP_OF, SIGSEGV, 0, NULL);
}
```

`error_get_trap_addr()` 返回出错指令的地址，填入 `siginfo.si_addr`。它考虑了 uprobe 的"离线单步"（XOL）：若异常发生在 uprobe 复制出来执行的指令副本上，需要映射回原指令地址（`arch/x86/kernel/traps.c:272-285` 的注释）。

### 异常到信号的映射

| 向量 | 异常 | 内核入口 | 用户态信号 / `si_code` |
|---|---|---|---|
| 0 | #DE 除法错误 | `exc_divide_error()` | `SIGFPE` / `FPE_INTDIV` |
| 1 | #DB 调试 | `exc_debug()` | `SIGTRAP` / `TRAP_TRACE`、`TRAP_HWBKPT` |
| 3 | #BP 断点 | `exc_int3()` | `SIGTRAP` |
| 4 | #OF 溢出 | `exc_overflow()` | `SIGSEGV` |
| 6 | #UD 非法指令 | `exc_invalid_op()` | `SIGILL` / `ILL_ILLOPN` |
| 7 | #NM 设备不可用 | `exc_device_not_available()` | 通常无信号（处理 XFD 等 FPU 状态） |
| 13 | #GP 一般保护 | `exc_general_protection()` | `SIGSEGV` |
| 14 | #PF 缺页 | `exc_page_fault()` | `SIGSEGV` / `SEGV_MAPERR`、`SEGV_ACCERR`；`SIGBUS` |
| 16、19 | #MF、#XF 浮点 | `exc_coprocessor_error()` 等 | `SIGFPE` |
| 17 | #AC 对齐检查 | `exc_alignment_check()` | `SIGBUS` / `BUS_ADRALN` |

*表 5-4：常见异常的入口函数与用户态信号*

## 主流程分析：几种代表性异常

### #UD：BUG() 与 WARN() 的实现

x86 上的 `BUG()` 和 `WARN()` 并不调用函数，而是直接放一条 `ud2`（保证触发 #UD 的两字节指令），再在 `__bug_table` 段中记录这条指令的位置、所在文件与行号：

```c
// arch/x86/include/asm/bug.h:45-106（v6.18.60）
#ifdef CONFIG_DEBUG_BUGVERBOSE
#define __BUG_ENTRY(file, line, flags)					\
	"2:\t" __BUG_REL("1b") "\t# bug_entry::bug_addr\n"		\
	"\t" __BUG_REL(file)   "\t# bug_entry::file\n"			\
	"\t.word " line        "\t# bug_entry::line\n"			\
	"\t.word " flags       "\t# bug_entry::flags\n"
...
#define _BUG_FLAGS_ASM(ins, file, line, flags, size, extra)		\
	"1:\t" ins "\n"							\
	".pushsection __bug_table,\"aw\"\n"				\
	__BUG_ENTRY(file, line, flags)					\
	"\t.org 2b + " size "\n"					\
	".popsection\n"							\
	extra
...
#define HAVE_ARCH_BUG
#define BUG()							\
do {								\
	instrumentation_begin();				\
	_BUG_FLAGS(ASM_UD2, 0, "");				\
	__builtin_unreachable();				\
} while (0)
...
#define __WARN_FLAGS(flags)					\
do {								\
	__auto_type __flags = BUGFLAG_WARNING|(flags);		\
	instrumentation_begin();				\
	_BUG_FLAGS(ASM_UD2, __flags, ARCH_WARN_REACHABLE);	\
	instrumentation_end();					\
} while (0)
```

这样做的好处是热路径上的开销只有 2 字节，且不需要为 `printk` 准备参数——所有信息都在 `__bug_table` 中，只有真正触发时才去查。`BUG()` 与 `WARN()` 的区别仅在于 `flags` 中是否带 `BUGFLAG_WARNING`。

#UD 入口在 `irqentry_enter()` **之前**就尝试识别这种情况：

```c
// arch/x86/kernel/traps.c:383-400（v6.18.60）
DEFINE_IDTENTRY_RAW(exc_invalid_op)
{
	irqentry_state_t state;

	/*
	 * We use UD2 as a short encoding for 'CALL __WARN', as such
	 * handle it before exception entry to avoid recursive WARN
	 * in case exception entry is the one triggering WARNs.
	 */
	if (!user_mode(regs) && handle_bug(regs))
		return;

	state = irqentry_enter(regs);
	instrumentation_begin();
	handle_invalid_op(regs);
	instrumentation_end();
	irqentry_exit(regs, state);
}
```

注释给出了原因：`WARN()` 可能出现在入口代码本身（例如 `irqentry_enter()` 中的断言），如果先调用 `irqentry_enter()`，就可能递归触发同一个 `WARN()`。这也是 #UD 使用 `DEFINE_IDTENTRY_RAW` 的原因。

```c
// arch/x86/kernel/traps.c:308-381（v6.18.60）
static noinstr bool handle_bug(struct pt_regs *regs)
{
	unsigned long addr = regs->ip;
	bool handled = false;
	int ud_type, ud_len;
	s32 ud_imm;

	ud_type = decode_bug(addr, &ud_imm, &ud_len);
	if (ud_type == BUG_NONE)
		return handled;

	/*
	 * All lies, just get the WARN/BUG out.
	 */
	instrumentation_begin();
	...
	/*
	 * Since we're emulating a CALL with exceptions, restore the interrupt
	 * state to what it was at the exception site.
	 */
	if (regs->flags & X86_EFLAGS_IF)
		raw_local_irq_enable();

	switch (ud_type) {
	case BUG_UD2:
		if (report_bug(regs->ip, regs) == BUG_TRAP_TYPE_WARN) {
			handled = true;
			break;
		}
		fallthrough;

	case BUG_UDB:
	case BUG_LOCK:
		if (handle_cfi_failure(regs) == BUG_TRAP_TYPE_WARN) {
			handled = true;
			break;
		}
		break;
	...
	}

	/*
	 * When continuing, and regs->ip hasn't changed, move it to the next
	 * instruction. When not continuing execution, restore the instruction
	 * pointer.
	 */
	if (handled) {
		if (regs->ip == addr)
			regs->ip += ud_len;
	} else {
		regs->ip = addr;
	}

	if (regs->flags & X86_EFLAGS_IF)
		raw_local_irq_disable();
	instrumentation_end();

	return handled;
}
```

- `decode_bug()`（`arch/x86/kernel/traps.c:96-183`）识别 `ud2`、`ud1`（Clang 的 UBSAN 陷阱）、`udb`（FineIBT 使用的 `0xd6`）等几种编码。
- `report_bug()`（`lib/bug.c`）在 `__bug_table` 中按地址查找条目，打印 `WARNING: CPU: ... at file:line` 与调用栈。对 `WARN()` 返回 `BUG_TRAP_TYPE_WARN`，于是把 `regs->ip` 跳过 `ud2` 指令继续执行——效果就像调用了一个打印函数后返回。对 `BUG()` 返回 `BUG_TRAP_TYPE_BUG`，`handled` 保持为假，最终走到 `handle_invalid_op()` → `do_error_trap()` → `die()`。
- 未知的 `ud` 编码同样当作普通 #UD 处理：用户态发送 `SIGILL`，内核态 Oops。

### #GP：一般保护异常

#GP 是"什么都可能"的异常：访问非规范地址（4.6 节）、加载非法段选择子、执行特权指令、`wrmsr` 写不存在的 MSR……

```c
// arch/x86/kernel/traps.c:792-851（v6.18.60）
DEFINE_IDTENTRY_ERRORCODE(exc_general_protection)
{
	char desc[sizeof(GPFSTR) + 50 + 2*sizeof(unsigned long) + 1] = GPFSTR;
	enum kernel_gp_hint hint = GP_NO_HINT;
	unsigned long gp_addr;

	if (user_mode(regs) && try_fixup_enqcmd_gp())
		return;

	cond_local_irq_enable(regs);

	if (static_cpu_has(X86_FEATURE_UMIP)) {
		if (user_mode(regs) && fixup_umip_exception(regs))
			goto exit;
	}
	...
	if (user_mode(regs)) {
		if (fixup_iopl_exception(regs))
			goto exit;

		if (fixup_vdso_exception(regs, X86_TRAP_GP, error_code, 0))
			goto exit;

		gp_user_force_sig_segv(regs, X86_TRAP_GP, error_code, desc);
		goto exit;
	}

	if (gp_try_fixup_and_notify(regs, X86_TRAP_GP, error_code, desc, 0))
		goto exit;

	if (error_code)
		snprintf(desc, sizeof(desc), "segment-related " GPFSTR);
	else
		hint = get_kernel_gp_address(regs, &gp_addr);

	if (hint != GP_NO_HINT)
		snprintf(desc, sizeof(desc), GPFSTR ", %s 0x%lx",
			 (hint == GP_NON_CANONICAL) ? "probably for non-canonical address"
						    : "maybe for address",
			 gp_addr);
	...
	die_addr(desc, regs, error_code, gp_addr);

exit:
	cond_local_irq_disable(regs);
}
```

用户态的 #GP 有几种可以"修复"的情形：

- `try_fixup_enqcmd_gp()`：`ENQCMD` 指令使用的 PASID MSR 是按需加载的，首次执行时 #GP，内核加载后重试。
- `fixup_umip_exception()`：启用 UMIP 后，用户态执行 `sgdt`/`sidt`/`smsw` 等会 #GP，内核为兼容旧程序模拟这些指令并返回伪造值（5.1 节实验）。
- `fixup_iopl_exception()`：模拟 `iopl(3)` 下的 `cli`/`sti`（v5.5 起 `iopl()` 改为基于 I/O 位图模拟，不再真正修改 `RFLAGS.IOPL`）。

其余情况发送 `SIGSEGV`。内核态的 #GP 先查异常表与 kprobes、die 通知链（`gp_try_fixup_and_notify()`，`arch/x86/kernel/traps.c:762-781`），仍未处理就 Oops。

#GP 不像 #PF 那样提供出错地址。为了让 Oops 信息更有用，`get_kernel_gp_address()`（`arch/x86/kernel/traps.c:646`）用指令解码器分析出错指令的内存操作数，若算出的地址是非规范地址，就在日志中提示 `probably for non-canonical address 0x...`。很多野指针 bug（例如访问 `LIST_POISON1` 这类故意设置的非规范地址）会表现为这种 #GP 而非 #PF。

### #BP：int3 的多重身份

`int3`（单字节 `0xcc`）在内核中被大量使用：

- **运行时改写代码**：static key、static call、alternatives、ftrace 在多核上修改正在执行的指令时，先把首字节换成 `int3`，同步所有 CPU 后再写入其余字节，最后恢复首字节（`text_poke_bp` 协议）。期间执行到该位置的 CPU 会陷入 `int3`，由 `smp_text_poke_int3_handler()` 模拟新指令；
- **kprobes**：在探测点放置 `int3`；
- **kgdb** 与用户态调试器（`SIGTRAP`）。

```c
// arch/x86/kernel/traps.c:883-915（v6.18.60）
DEFINE_IDTENTRY_RAW(exc_int3)
{
	/*
	 * smp_text_poke_int3_handler() is completely self contained code; it does (and
	 * must) *NOT* call out to anything, lest it hits upon yet another
	 * INT3.
	 */
	if (smp_text_poke_int3_handler(regs))
		return;

	/*
	 * irqentry_enter_from_user_mode() uses static_branch_{,un}likely()
	 * and therefore can trigger INT3, hence smp_text_poke_int3_handler() must
	 * be done before. If the entry came from kernel mode, then use
	 * nmi_enter() because the INT3 could have been hit in any context
	 * including NMI.
	 */
	if (user_mode(regs)) {
		irqentry_enter_from_user_mode(regs);
		instrumentation_begin();
		do_int3_user(regs);
		instrumentation_end();
		irqentry_exit_to_user_mode(regs);
	} else {
		irqentry_state_t irq_state = irqentry_nmi_enter(regs);

		instrumentation_begin();
		if (!do_int3(regs))
			die("int3", regs, 0);
		instrumentation_end();
		irqentry_nmi_exit(regs, irq_state);
	}
}
```

两个值得注意的设计：

- **text_poke 处理必须最先执行**。入口代码自身也用了 static key，如果某个 CPU 正在改写入口路径上的 static key，另一个 CPU 执行到那里就会陷入 `int3`；若此时先调用 `irqentry_enter()`，又会碰到同一个正在改写的位置，形成无限递归。
- **内核态的 `int3` 按 NMI 处理**。kprobes 可以探测几乎任何函数，包括在 NMI 处理过程中调用的函数，所以内核态 `int3` 的"上下文"无法预知。`irqentry_nmi_enter()` 是唯一在任何上下文都安全的进入方式。5.2 节提到的 6 字的栈空隙也是为 `int3` 模拟 `call` 指令准备的（`int3_emulate_call()`）。

`do_int3()`（`arch/x86/kernel/traps.c:853-870`）依次尝试 kgdb、`kprobe_int3_handler()` 和 die 通知链，都不认领时内核态 Oops，用户态发送 `SIGTRAP`。

### #DB：调试异常

#DB 由硬件断点（DR0～DR3）、单步（`RFLAGS.TF`）和 `int1` 触发。它使用 IST 栈，但入口宏 `idtentry_mce_db`（5.2 节表 5-3）会区分来源：来自用户态时切到任务栈调用 `noist_exc_debug()`，按普通异常处理（发 `SIGTRAP`、支持 ptrace 单步）；来自内核态时在 IST 栈上调用 `exc_debug()`，后者调用 `exc_debug_kernel()`：

```c
// arch/x86/kernel/traps.c:1095-1115（v6.18.60）
static noinstr void exc_debug_kernel(struct pt_regs *regs, unsigned long dr6)
{
	/*
	 * Disable breakpoints during exception handling; recursive exceptions
	 * are exceedingly 'fun'.
	 *
	 * Since this function is NOKPROBE, and that also applies to
	 * HW_BREAKPOINT_X, we can't hit a breakpoint before this (XXX except a
	 * HW_BREAKPOINT_W on our stack)
	 *
	 * Entry text is excluded for HW_BP_X and cpu_entry_area, which
	 * includes the entry stack is excluded for everything.
	 *
	 * For FRED, nested #DB should just work fine. But when a watchpoint or
	 * breakpoint is set in the code path which is executed by #DB handler,
	 * it results in an endless recursion and stack overflow. Thus we stay
	 * with the IDT approach, i.e., save DR7 and disable #DB.
	 */
	unsigned long dr7 = local_db_save();
	irqentry_state_t irq_state = irqentry_nmi_enter(regs);
	instrumentation_begin();
```

由于 IST 栈不可重入，处理函数一开始就用 `local_db_save()` 清零 DR7，禁止在处理期间再次触发硬件断点，返回前再恢复。内核同时禁止在入口代码和 cpu_entry_area 上设置硬件断点。与 `int3` 一样，内核态 #DB 用 `irqentry_nmi_enter()` 进入。

### #DF：双重故障

双重故障（Double Fault，#DF）发生在 CPU 投递一个异常的过程中又遇到另一个异常（例如投递 #PF 时压栈失败）。在 x86_64 Linux 上，它最常见的原因是**内核栈溢出**：开启 `CONFIG_VMAP_STACK` 后，任务栈两端是未映射的保护页，栈溢出时 CPU 投递 #PF 需要压栈，压栈本身又碰到保护页，于是升级为 #DF。#DF 使用 IST 栈，所以仍能运行：

```c
// arch/x86/kernel/traps.c:564-618（v6.18.60）
	irqentry_nmi_enter(regs);
	instrumentation_begin();
	notify_die(DIE_TRAP, str, regs, error_code, X86_TRAP_DF, SIGSEGV);

	tsk->thread.error_code = error_code;
	tsk->thread.trap_nr = X86_TRAP_DF;

#ifdef CONFIG_VMAP_STACK
	/*
	 * If we overflow the stack into a guard page, the CPU will fail
	 * to deliver #PF and will send #DF instead.  Similarly, if we
	 * take any non-IST exception while too close to the bottom of
	 * the stack, the processor will get a page fault while
	 * delivering the exception and will generate a double fault.
	 *
	 ...
	 */
	if (get_stack_guard_info((void *)address, &info))
		handle_stack_overflow(regs, address, &info);
#endif

	pr_emerg("PANIC: double fault, error_code: 0x%lx\n", error_code);
	die("double fault", regs, error_code);
	if (always_true())
		panic("Machine halted.");
	instrumentation_end();
}
```

`address` 是函数开头读取的 CR2（`arch/x86/kernel/traps.c:505`）。若 CR2 落在某个栈的保护页内，`handle_stack_overflow()` 打印 `BUG: stack guard page was hit at ... (stack is ...)` 并 panic，比一个裸的 "double fault" 更易定位问题。#DF 处理函数总是以 panic 结束。

函数前半部分（`arch/x86/kernel/traps.c:509-562`）还处理了一个特殊情况：返回 16 位栈段的用户程序时会经过 ESPFIX64 栈，如果那里的 `iretq` 失败，#GP 会被升级为 #DF。内核识别出这种情况后，伪造一个 #GP 帧并直接"返回"到 #GP 入口，让它按用户态 #GP 正常处理——这是少数可以从 #DF 恢复的情形。

## 异常表：允许出错的内核指令

### 设计动机

内核代码有时**必须**执行可能出错的指令，最典型的是访问用户内存。用户传给系统调用的指针可能指向任何地方，内核不可能事先完全验证（即使地址有效，页面也可能在验证后被另一个线程 `munmap()`）。与其在每次访问前加锁检查，不如直接访问，出错时再修复。

异常表的思路是：在编译时为每条"可能出错"的指令登记一个修复地址，异常发生时用出错指令的地址查表，查到就把 `regs->ip` 改为修复地址后返回，修复代码负责设置错误码（如 `-EFAULT`）并跳回正常流程。

### 表项格式

```c
// arch/x86/include/asm/extable.h:7-28（v6.18.60）
/*
 * The exception table consists of two addresses relative to the
 * exception table entry itself and a type selector field.
 *
 * The first address is of an instruction that is allowed to fault, the
 * second is the target at which the program should continue.
 *
 * The type entry is used by fixup_exception() to select the handler to
 * deal with the fault caused by the instruction in the first field.
 *
 * All the routines below use bits of fixup code that are out of line
 * with the main instruction path.  This means when everything is well,
 * we don't even have to jump over them.  Further, they do not intrude
 * on our cache or tlb entries.
 */

struct exception_table_entry {
	int insn, fixup, data;
};
struct pt_regs;

#define ARCH_HAS_RELATIVE_EXTABLE
```

- `insn`、`fixup` 都是**相对于字段自身地址的 32 位偏移**（`ARCH_HAS_RELATIVE_EXTABLE`），比存放 64 位绝对地址节省一半空间，也无需在 KASLR 重定位时修改。
- `data` 编码了修复类型以及附加参数：

```c
// arch/x86/include/asm/extable_fixup_types.h:10-21（v6.18.60）
#define EX_DATA_TYPE_MASK		((int)0x000000FF)
#define EX_DATA_REG_MASK		((int)0x00000F00)
#define EX_DATA_FLAG_MASK		((int)0x0000F000)
#define EX_DATA_IMM_MASK		((int)0xFFFF0000)

#define EX_DATA_REG_SHIFT		8
#define EX_DATA_FLAG_SHIFT		12
#define EX_DATA_IMM_SHIFT		16

#define EX_DATA_REG(reg)		((reg) << EX_DATA_REG_SHIFT)
#define EX_DATA_FLAG(flag)		((flag) << EX_DATA_FLAG_SHIFT)
#define EX_DATA_IMM(imm)		((imm) << EX_DATA_IMM_SHIFT)
```

低 8 位是类型，接着 4 位是寄存器编号，再 4 位是标志（如"出错时清零 `ax`"），高 16 位是有符号立即数。例如 `EX_TYPE_EFAULT_REG` 就是 `EX_TYPE_IMM_REG | EX_DATA_IMM(-EFAULT)`：出错时把指定寄存器设为 `-EFAULT`（`arch/x86/include/asm/extable_fixup_types.h:55-56`）。

### 登记表项

汇编宏 `_ASM_EXTABLE_TYPE()` 把一个表项放进 `__ex_table` 段：

```c
// arch/x86/include/asm/asm.h:179-185（v6.18.60）
# define _ASM_EXTABLE_TYPE(from, to, type)			\
	" .pushsection \"__ex_table\",\"a\"\n"			\
	" .balign 4\n"						\
	" .long (" #from ") - .\n"				\
	" .long (" #to ") - .\n"				\
	" .long " __stringify(type) " \n"			\
	" .popsection\n"
```

以 `get_user()` 系列为例，本书配置（Clang，`CONFIG_CC_HAS_ASM_GOTO_OUTPUT=y`）下的实现使用 `asm goto`，修复地址直接是一个 C 标签：

```c
// arch/x86/include/asm/uaccess.h:297-303（v6.18.60）
#define __get_user_asm(x, addr, itype, ltype, label)			\
	asm_goto_output("\n"						\
		     "1:	mov"itype" %[umem],%[output]\n"		\
		     _ASM_EXTABLE_UA(1b, %l2)				\
		     : [output] ltype(x)				\
		     : [umem] "m" (__m(addr))				\
		     : : label)
```

`_ASM_EXTABLE_UA(1b, %l2)` 表示"标号 1 处的 `mov` 允许出错，修复后跳到 C 标签 `label`"，`EX_TYPE_UACCESS` 类型。调用者在 `label` 处返回 `-EFAULT`。正常执行时这条 `mov` 与普通内存访问完全相同，没有任何额外开销。

### 查表与修复

链接时所有 `__ex_table` 段被合并，构建脚本 `scripts/sorttable` 在构建期按 `insn` 排序，运行时用二分查找：

```c
// kernel/extable.c:53-64（v6.18.60）
/* Given an address, look for it in the exception tables. */
const struct exception_table_entry *search_exception_tables(unsigned long addr)
{
	const struct exception_table_entry *e;

	e = search_kernel_exception_table(addr);
	if (!e)
		e = search_module_extables(addr);
	if (!e)
		e = search_bpf_extables(addr);
	return e;
}
```

依次查内核本体、已加载模块、BPF JIT 程序（BPF 程序访问内核内存的 `PROBE_MEM` 指令也登记了异常表）。

`fixup_exception()` 按类型分派处理：

```c
// arch/x86/mm/extable.c:299-373（v6.18.60）
int fixup_exception(struct pt_regs *regs, int trapnr, unsigned long error_code,
		    unsigned long fault_addr)
{
	const struct exception_table_entry *e;
	int type, reg, imm;
	...
	e = search_exception_tables(regs->ip);
	if (!e)
		return 0;

	type = FIELD_GET(EX_DATA_TYPE_MASK, e->data);
	reg  = FIELD_GET(EX_DATA_REG_MASK,  e->data);
	imm  = FIELD_GET(EX_DATA_IMM_MASK,  e->data);

	switch (type) {
	case EX_TYPE_DEFAULT:
	case EX_TYPE_DEFAULT_MCE_SAFE:
		return ex_handler_default(e, regs);
	case EX_TYPE_FAULT:
	case EX_TYPE_FAULT_MCE_SAFE:
		return ex_handler_fault(e, regs, trapnr);
	case EX_TYPE_UACCESS:
		return ex_handler_uaccess(e, regs, trapnr, fault_addr);
	...
	case EX_TYPE_WRMSR:
		return ex_handler_msr(e, regs, true, false, reg);
	case EX_TYPE_RDMSR:
		return ex_handler_msr(e, regs, false, false, reg);
	...
	case EX_TYPE_POP_REG:
		regs->sp += sizeof(long);
		fallthrough;
	case EX_TYPE_IMM_REG:
		return ex_handler_imm_reg(e, regs, reg, imm);
	...
	case EX_TYPE_ZEROPAD:
		return ex_handler_zeropad(e, regs, fault_addr);
	...
	}
	BUG();
}
```

最基础的 `ex_handler_default()` 只做一件事——把 `regs->ip` 改成修复地址：

```c
// arch/x86/mm/extable.c:27-43（v6.18.60）
static inline unsigned long
ex_fixup_addr(const struct exception_table_entry *x)
{
	return (unsigned long)&x->fixup + x->fixup;
}

static bool ex_handler_default(const struct exception_table_entry *e,
			       struct pt_regs *regs)
{
	if (e->data & EX_FLAG_CLEAR_AX)
		regs->ax = 0;
	if (e->data & EX_FLAG_CLEAR_DX)
		regs->dx = 0;

	regs->ip = ex_fixup_addr(e);
	return true;
}
```

异常处理函数返回后，`iretq` 就会跳到修复代码执行。几种常见类型的用途：

| 类型 | 典型使用者 | 修复动作 |
|---|---|---|
| `EX_TYPE_UACCESS` | `get_user()`、`copy_from_user()` | 跳到修复地址；#GP 时检查是否为非规范地址并告警 |
| `EX_TYPE_FAULT` | `copy_mc_*()` 等需要知道向量号的场合 | 把 `trapnr` 写入 `ax` 后跳转 |
| `EX_TYPE_RDMSR_SAFE` / `WRMSR_SAFE` | `rdmsr_safe()`、`wrmsr_safe()` | 目标寄存器置 `-EIO` |
| `EX_TYPE_RDMSR` / `WRMSR` | 普通 `rdmsr()` | 打印一次告警，读返回 0，继续执行 |
| `EX_TYPE_IMM_REG` / `EFAULT_REG` | 不支持 `asm goto` 输出的 `get_user()` | 目标寄存器置为立即数 |
| `EX_TYPE_ZEROPAD` | `load_unaligned_zeropad()` | 跨页读字时下一页不存在，补零 |
| `EX_TYPE_BPF` | BPF JIT 的 `PROBE_MEM` | 目标寄存器清零，跳过该指令 |
| `EX_TYPE_ERETU` | FRED 返回用户态的 `ERETU` | 伪造用户态异常（`CONFIG_X86_FRED`） |

*表 5-5：常用异常表修复类型*

### 谁会查异常表

`fixup_exception()` 在以下内核态异常处理路径中被调用：`do_trap_no_signal()`（#DE、#UD 等）、`gp_try_fixup_and_notify()`（#GP）、`kernelmode_fixup_or_oops()`（#PF，4.8 节）、`math_error()`（#MF/#XF）以及 MCE 处理中的"可恢复"路径。**缺页处理是异常表最主要的使用者**：`copy_from_user()` 访问用户地址缺页时，`do_user_addr_fault()` 先尝试正常处理缺页（换入页面）；只有当地址确实无效时，才回到 `kernelmode_fixup_or_oops()` 查异常表。

一个需要注意的边界：异常表只说明"这条指令允许出错"，不说明"允许在什么上下文出错"。在关闭了缺页处理（`pagefault_disable()`）的原子上下文中访问用户内存，缺页会直接走异常表修复返回 `-EFAULT`，调用者需要处理这种"并非地址无效"的失败（例如 futex 在原子上下文中失败后，先释放锁、用 `fault_in_user_writeable()` 换入页面再重试，7.8 节）。

## Oops 与 die

当内核态异常无法修复时，调用 `die()`：

```c
// arch/x86/kernel/dumpstack.c:449-461（v6.18.60）
/*
 * This is gone through when something in the kernel has done something bad
 * and is about to be terminated:
 */
void die(const char *str, struct pt_regs *regs, long err)
{
	unsigned long flags = oops_begin();
	int sig = SIGSEGV;

	if (__die(str, regs, err))
		sig = 0;
	oops_end(flags, regs, sig);
}
```

`oops_begin()`（`arch/x86/kernel/dumpstack.c:349-370`）关中断并获取全局的 `die_lock`，保证多个 CPU 同时 Oops 时输出不会交错；`console_verbose()` 提高控制台日志级别，`bust_spinlocks(1)` 让 `printk` 尽量绕过可能被持有的控制台锁。`__die()` 打印 `Oops: general protection fault: 0000 [#1] SMP PTI` 形式的头部、寄存器、调用栈与已加载模块，并给 `DIE_OOPS` 通知链一个机会。

```c
// arch/x86/kernel/dumpstack.c:375-409（v6.18.60）
void oops_end(unsigned long flags, struct pt_regs *regs, int signr)
{
	if (regs && kexec_should_crash(current))
		crash_kexec(regs);

	bust_spinlocks(0);
	die_owner = -1;
	add_taint(TAINT_DIE, LOCKDEP_NOW_UNRELIABLE);
	die_nest_count--;
	if (!die_nest_count)
		/* Nest count reaches zero, release the lock. */
		arch_spin_unlock(&die_lock);
	raw_local_irq_restore(flags);
	oops_exit();

	/* Executive summary in case the oops scrolled away */
	__show_regs(&exec_summary_regs, SHOW_REGS_ALL, KERN_DEFAULT);

	if (!signr)
		return;
	if (in_interrupt())
		panic("Fatal exception in interrupt");
	if (panic_on_oops)
		panic("Fatal exception");

	/*
	 * We're not going to return, but we might be on an IST stack or
	 * have very little stack space left.  Rewind the stack and kill
	 * the task.
	 * Before we rewind the stack, we have to tell KASAN that we're going to
	 * reuse the task stack and that existing poisons are invalid.
	 */
	kasan_unpoison_task_stack(current);
	rewind_stack_and_make_dead(signr);
}
```

Oops 之后的去向：

1. 若配置了 kdump 且满足条件，`crash_kexec()` 直接切换到捕获内核保存现场。
2. 内核被标记为 `TAINT_DIE`（`/proc/sys/kernel/tainted` 的 `D` 位）。
3. **在中断上下文中 Oops 一律 panic**：中断"借用"了当前任务，杀死该任务无济于事，且中断处理函数可能持有关键锁。
4. `panic_on_oops` 为真时 panic（本书配置 `CONFIG_PANIC_ON_OOPS` 未开启，可通过 `kernel.panic_on_oops` sysctl 设置）。
5. 否则调用 `rewind_stack_and_make_dead()`（`arch/x86/entry/entry_64.S:1490-1500`）：把 RSP 重置到任务栈顶（当前可能在 IST 栈上或栈已接近耗尽），然后调用 `make_task_dead()` 终止当前任务。系统继续运行，但该任务持有的锁可能永远不会释放，后续可能出现死锁。

## 关键细节 / 设计考量

### 为什么异常按"进入方式"区分

本节出现了三种进入方式：普通的 `irqentry_enter()`（#DE、#GP 等）、`irqentry_nmi_enter()`（内核态 #BP、#DB、#DF）和先处理再进入（#UD 的 `handle_bug()`、#PF 的读 CR2）。选择依据是"这个异常可能在什么上下文中发生"：

- #GP、#DE 等只会由"出错的代码"触发。如果内核在 NMI 中执行了除零，那是 bug，按普通方式处理并 Oops 即可。
- `int3` 与硬件断点是**故意放置**的，kprobes 和 ftrace 可以把它们放到 NMI 处理函数、调度器内部、甚至 RCU 内部。只有把它们当作 NMI 进入，才能保证在任何上下文中都不破坏 RCU 和 lockdep 的状态。

### noinstr 与可探测性

`exc_int3()`、`exc_debug()` 等自身都在 `.noinstr.text` 中，被 kprobes 黑名单排除（`_ASM_NOKPROBE` / `NOKPROBE_SYMBOL`）。如果允许在 `int3` 处理函数中放 kprobe，处理第一个 `int3` 时会触发第二个，形成无限递归。`.noinstr.text`、`.entry.text` 和 cpu_entry_area 也都禁止设置硬件断点。

### 异常表与 SMAP

开启 SMAP 后，内核态默认不能访问用户页，即使地址有效也会 #PF。`copy_from_user()` 等函数在访问前后用 `stac`/`clac` 临时打开访问权限（`__uaccess_begin()` / `__uaccess_end()`）。入口代码中的 `ASM_CLAC`（5.2 节）保证异常或中断打断 `stac` 区间时，处理函数本身不会继承用户访问权限；返回时 `iretq` 恢复被打断现场的 `RFLAGS.AC`。

## 动手实验

### 观察用户态异常转化为信号

```bash
# 在 dev 虚拟机中执行
cat > /tmp/de.c <<'EOF'
#include <stdio.h>
int main(void) { volatile int z = 0; printf("%d\n", 1 / z); return 0; }
EOF
gcc -O0 -o /tmp/de /tmp/de.c && /tmp/de; echo "exit=$?"
sudo dmesg | tail -2
```

shell 报告 `Floating point exception`，退出码 136（128 + `SIGFPE` 的 8）；`dmesg` 中可以看到 `show_signal()` 打印的 `traps: de[pid] trap divide error ip:... sp:... error:0 in de[...]`。

### 统计异常表命中

`copy_from_user()` 失败的路径会经过 `fixup_exception()`，可以用 kprobe 统计：

```bash
sudo bpftrace -e 'kprobe:fixup_exception { @[kstack(4)] = count(); } interval:s:10 { exit(); }'
```

在正常运行的系统上，命中通常来自 `strncpy_from_user()`（路径名跨页）、`rdmsr_safe()` 或 `load_unaligned_zeropad()` 等。

### 制造一个 WARN

```c
// 实验代码（非内核源码）：加载后 dmesg 出现 WARNING 与调用栈，模块正常返回
static int __init warn_demo_init(void)
{
	WARN(1, "demo warning from %s\n", __func__);
	return 0;
}
```

用 `objdump -d` 查看编译出的模块，可以在 `warn_demo_init` 中找到 `ud2` 指令，以及对应的 `__bug_table` 段条目。

## 小结

- 异常处理的第一步是区分来源：用户态异常转为信号，内核态异常先查异常表、kprobes 与 die 通知链，都不认领才 Oops。
- 处理函数在被打断上下文原本开中断时才重新开中断（`cond_local_irq_enable()`），保持与原上下文一致。
- `BUG()` / `WARN()` 编译为 `ud2` 加 `__bug_table` 条目；#UD 入口在 `irqentry_enter()` 之前调用 `handle_bug()`，`WARN()` 处理后跳过 `ud2` 继续执行。
- 内核态的 `int3` 与 #DB 一律按 NMI 方式进入，因为 kprobes、text_poke 可以把它们放在任何上下文中；text_poke 的 `int3` 必须在一切其他处理之前识别。
- #DF 在 x86_64 上最常见的原因是栈溢出撞到保护页，处理函数借助 CR2 判断并打印溢出信息后 panic。
- 异常表以"相对偏移 + 类型编码"记录允许出错的指令，构建期排序、运行期二分查找；`copy_from_user()`、`rdmsr_safe()` 等都依赖它把硬件异常转为错误返回值，正常路径零开销。
- 无法修复的内核态异常经 `die()` 打印 Oops；在中断上下文或设置了 `panic_on_oops` 时 panic，否则终止当前任务并标记内核 `TAINT_DIE`。

## 参考

- `linux/Documentation/arch/x86/exception-tables.rst`
- `linux/Documentation/admin-guide/bug-hunting.rst`（解读 Oops 信息）
- `linux/Documentation/admin-guide/tainted-kernels.rst`
- `linux/Documentation/trace/kprobes.rst`
- Intel® 64 and IA-32 Architectures Software Developer's Manual, Vol. 3A, §6.15 "Exception and Interrupt Reference"
