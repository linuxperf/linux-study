# 2.9 x86_64 信号投递与系统调用重启

> **本节源码**：[`kernel/entry/common.c`](../../linux/kernel/entry/common.c)、[`arch/x86/kernel/signal.c`](../../linux/arch/x86/kernel/signal.c)、[`arch/x86/kernel/signal_64.c`](../../linux/arch/x86/kernel/signal_64.c)、[`arch/x86/include/asm/sigframe.h`](../../linux/arch/x86/include/asm/sigframe.h)、[`kernel/signal.c`](../../linux/kernel/signal.c)
> **关键函数**：`arch_do_signal_or_restart()`、`handle_signal()`、`get_sigframe()`、`x64_setup_rt_frame()`、`signal_setup_done()`、`restore_sigcontext()`
> **前置知识**：2.8 信号待决与动作选择；x86_64 寄存器和用户 / 内核态基础

## 概述

选中用户信号处理器以后，内核必须让目标任务先执行处理函数，之后还能恢复被打断的现场。x86_64 通过在用户栈建立信号帧（Signal Frame），改写返回用户态的寄存器来完成这一点。处理器返回经由用户态恢复桩进入 `rt_sigreturn` 系统调用，再由内核恢复掩码、寄存器和扩展状态。若信号打断一个系统调用，还需要决定向用户报告 `EINTR`，还是按重启协议重新执行。

## 核心数据结构

[`arch/x86/include/asm/sigframe.h:59-64`](../../linux/arch/x86/include/asm/sigframe.h#L59-L64)（v6.18.60）

```c
struct rt_sigframe {
	char __user *pretcode;
	struct ucontext uc;
	struct siginfo info;
	/* fp state follows here */
};
```

- `pretcode` 是用户处理器返回时使用的恢复桩地址，不是内核栈上的返回地址。
- `uc` 保存用户上下文，包括信号掩码、备用栈状态和机器寄存器现场。
- `info` 对应 `SA_SIGINFO` 形式的信号信息。
- 浮点与扩展处理器状态也需要保存；不能把整个信号帧视为固定几个通用寄存器。

这里分析原生 x86_64 ABI；兼容 32 位与 x32 有各自帧格式，不能套用同一字段偏移。结构定义同见 [`arch/x86/include/asm/sigframe.h`](../../linux/arch/x86/include/asm/sigframe.h)。

## 主流程分析

```text
返回用户态检查                        kernel/entry/common.c
└── arch_do_signal_or_restart()        arch/x86/kernel/signal.c
    ├── get_signal()                   kernel/signal.c
    ├── handle_signal()
    │   ├── 调整被打断系统调用的重启现场
    │   ├── setup_rt_frame()
    │   │   └── x64_setup_rt_frame()   arch/x86/kernel/signal_64.c
    │   │       ├── get_sigframe()     arch/x86/kernel/signal.c
    │   │       ├── 保存 ucontext / siginfo
    │   │       └── 改写用户 ip、sp 和参数寄存器
    │   └── signal_setup_done()        kernel/signal.c
    └── 无处理器待执行时：处理重启与恢复暂存掩码

用户态处理器 → 用户恢复桩 → rt_sigreturn 系统调用
                                      arch/x86/kernel/signal_64.c
├── 恢复 blocked 与备用栈状态
└── restore_sigcontext() / 恢复扩展状态
```

### 一、信号检查位于安全的用户态返回边界

通用入口代码在发现 `_TIF_SIGPENDING` 或 `_TIF_NOTIFY_SIGNAL` 等工作时调用架构处理函数，见 [`kernel/entry/common.c:23-49`](../../linux/kernel/entry/common.c#L23-L49)。发送方不会直接把当前 CPU 任意切入另一个任务的用户处理器。

`arch_do_signal_or_restart()` 先用 `get_signal()` 取得动作；只有需要用户处理器时才调用 `handle_signal()`，见 [`arch/x86/kernel/signal.c:333-374`](../../linux/arch/x86/kernel/signal.c#L333-L374)。默认致命信号可能已在通用路径中退出，不经过用户信号帧。

### 二、选择普通栈还是备用信号栈

`get_sigframe()` 默认从当前用户栈取空间；原生 64 位路径为 red zone 留出 128 字节，并按 ABI 对齐。设置 `SA_ONSTACK` 且满足备用栈条件时，改用 `sigaltstack()` 登记的区域，见 [`arch/x86/kernel/signal.c:94-177`](../../linux/arch/x86/kernel/signal.c#L94-L177)。

备用信号栈（Alternate Signal Stack）适合原用户栈不足等场景，但不是内核代为提供的一块无限空间。帧还包含浮点扩展状态，对齐、栈边界和可访问性检查都可能失败。

### 三、保存旧现场，改写新入口

`x64_setup_rt_frame()` 要求 `SA_RESTORER`，把用户恢复桩写入 `pretcode`，保存上下文和掩码，见 [`arch/x86/kernel/signal_64.c:164-196`](../../linux/arch/x86/kernel/signal_64.c#L164-L196)。该底层标志通常由 libc 包装负责，普通应用应使用 `sigaction()`，不要未经 ABI 处理直接拼装原始系统调用参数。

随后设置寄存器：

[`arch/x86/kernel/signal_64.c:201-212`](../../linux/arch/x86/kernel/signal_64.c#L201-L212)（v6.18.60）

```c
	/* Set up registers for signal handler */
	regs->di = ksig->sig;
	/* In case the signal handler was declared without prototypes */
	regs->ax = 0;

	/* This also works for non SA_SIGINFO handlers because they expect the
	   next argument after the signal number on the stack. */
	regs->si = (unsigned long)&frame->info;
	regs->dx = (unsigned long)&frame->uc;
	regs->ip = (unsigned long) ksig->ka.sa.sa_handler;

	regs->sp = (unsigned long)frame;
```

用户态恢复执行时先进入处理器，原现场则留在信号帧内。用户处理器仍属于原任务，不会新增线程，也不是在内核态执行的 C 回调。

投递成功后，`signal_delivered()` 把 `sa_mask` 并入当前掩码；若没有 `SA_NODEFER`，还会阻塞当前信号，见 [`kernel/signal.c:3133-3161`](../../linux/kernel/signal.c#L3133-L3161)。这降低同一信号立即递归投递的风险，但不代表处理器不会被任何其他信号打断。

### 四、`rt_sigreturn` 不是普通函数返回

用户处理器的 `ret` 先到恢复桩，再发起 `rt_sigreturn`。内核从用户帧恢复原掩码、备用栈和寄存器，并恢复浮点等状态，见 [`arch/x86/kernel/signal_64.c:246-277`](../../linux/arch/x86/kernel/signal_64.c#L246-L277)。

用户帧不可信：恢复路径检查访问，限制可恢复的标志位并强制用户特权级，见 [`arch/x86/kernel/signal_64.c:50-95`](../../linux/arch/x86/kernel/signal_64.c#L50-L95)。坏帧会进入错误信号路径，而不是允许任意恢复内核权限。

本配置 `CONFIG_X86_USER_SHADOW_STACK=y`，代码还包含影子栈（Shadow Stack）处理。是否对某个任务实际启用仍取决于硬件与用户程序状态；不能仅由编译配置推断所有处理器返回都使用影子栈。

## 关键细节 / 设计考量

### `SA_RESTART` 不是所有系统调用自动重试

`handle_signal()` 依据内核内部重启码区分处理，见 [`arch/x86/kernel/signal.c:263-283`](../../linux/arch/x86/kernel/signal.c#L263-L283)：

| 内部返回状态 | 有用户处理器时的主要决策 |
|---|---|
| `-ERESTARTNOHAND`、`-ERESTART_RESTARTBLOCK` | 此处改为 `-EINTR` |
| `-ERESTARTSYS` | 无 `SA_RESTART` 时改为 `-EINTR`，否则准备重启 |
| `-ERESTARTNOINTR` | 准备重启，不依赖 `SA_RESTART` |

这些是内核内部协议值，不是应用应直接处理的 errno。允许重启时，原生 x86 路径恢复 `orig_ax` 并回退指令指针；无处理器的重启路径还可能改用 `restart_syscall`，见 [`arch/x86/kernel/signal.c:343-365`](../../linux/arch/x86/kernel/signal.c#L343-L365)。具体调用是否重启，还取决于调用实现是否返回这类状态、是否已有部分结果等。

### 用户处理器的异步安全是另一层约束

处理器可能打断 libc 正在持锁的代码。不能因为它运行在用户态，就在其中随意调用 `printf()`、分配器或复杂日志函数。示例采用 `volatile sig_atomic_t` 记录结果，处理器只作标记；实际应用应遵守异步信号安全约定。

### 信号帧失败与原始异常要分开

栈不可写、备用栈空间不足等会导致建帧失败，最终可能出现 `SIGSEGV`，与最初请求投递的信号不同。相关收尾见 [`kernel/signal.c:3163-3172`](../../linux/kernel/signal.c#L3163-L3172)、[`arch/x86/kernel/signal.c:376-399`](../../linux/arch/x86/kernel/signal.c#L376-L399)。

## 可观测性

| 入口 | 看什么、怎么解读 | 源码与配置 |
|---|---|---|
| `signal:signal_deliver` | 内核已经选定动作，仍早于用户处理器真正开始执行 | [`include/trace/events/signal.h:83-120`](../../linux/include/trace/events/signal.h#L83-L120)，`CONFIG_EVENT_TRACING=y` |
| `syscalls:sys_enter_rt_sigreturn` | 用户恢复桩进入恢复协议；仅消费 signalfd / 同步等待不必出现 | syscall 定义 [`arch/x86/kernel/signal_64.c:246-277`](../../linux/arch/x86/kernel/signal_64.c#L246-L277)，事件生成见 [`kernel/trace/trace_syscalls.c`](../../linux/kernel/trace/trace_syscalls.c)，`CONFIG_FTRACE_SYSCALLS=y` |
| `/proc/<pid>/task/<tid>/status` 的 `SigBlk` | 处理器期间可能包含临时加入的信号；瞬时采样可能看不到短处理器 | [`fs/proc/array.c:262-302`](../../linux/fs/proc/array.c#L262-L302)，`CONFIG_PROC_FS=y` |
| `arch_do_signal_or_restart()`、`x64_setup_rt_frame()` 探针 | 区分动作选择、建帧和失败；不能据函数返回就证明用户处理器执行完成 | 定义见本节主流程，`CONFIG_KPROBES=y`；核对实际构建符号与探针限制 |

## 动手实验：区分同步消费与用户处理器

2.8 的示例末尾另安装一个 `SIGUSR2` 处理器，调用 `raise()` 后检查处理器标记。用 strace 观察两种路径：

```bash
cd /root/linux
work=$(mktemp -d /tmp/process-frame.XXXXXX)
cc -O2 -Wall -Wextra -std=c11 book/02-process/examples/pending-signals.c \
    -o "$work/pending-signals"
strace -e trace=rt_sigaction,rt_sigprocmask,rt_sigtimedwait,rt_sigreturn \
    "$work/pending-signals"
rm -r -- "$work"
```

预期前面的阻塞信号由同步等待取走，不对应普通处理器帧；后面的 `SIGUSR2` 处理器返回会经过 `rt_sigreturn`。该实验不用于验证所有重启码，也不制造坏帧或栈溢出；尚未在目标内核上实测。

## 小结

- 用户信号处理器通过返回用户态寄存器改写进入，不是内核直接调用。
- 信号帧保存上下文、掩码和扩展状态，可以位于备用用户栈。
- 处理器返回必须经过用户恢复桩与 `rt_sigreturn`，并接受恢复合法性检查。
- `SA_RESTART` 只参与特定内部重启码的决策，不适用于所有调用。
- 动作选择事件、建帧成功、用户入口和恢复完成不能混作一个时刻。

## 参考

- [`arch/x86/include/uapi/asm/ucontext.h`](../../linux/arch/x86/include/uapi/asm/ucontext.h)。
- [`Documentation/arch/x86/shstk.rst`](../../linux/Documentation/arch/x86/shstk.rst)。
- [`include/trace/events/signal.h`](../../linux/include/trace/events/signal.h)。
