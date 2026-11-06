# 2.4 任务创建：fork、clone 与首次运行

> **本节源码**：[`kernel/fork.c`](../../linux/kernel/fork.c)、[`include/linux/sched/task.h`](../../linux/include/linux/sched/task.h)、[`include/uapi/linux/sched.h`](../../linux/include/uapi/linux/sched.h)、[`arch/x86/kernel/process.c`](../../linux/arch/x86/kernel/process.c)、[`arch/x86/entry/entry_64.S`](../../linux/arch/x86/entry/entry_64.S)
> **关键函数**：`kernel_clone()`、`copy_process()`、`dup_task_struct()`、`copy_thread()`、`wake_up_new_task()`、`ret_from_fork()`
> **前置知识**：2.2 任务状态与内核栈；2.3 PID 与线程组

## 概述

任务创建不仅要复制一个描述符，还要建立可独立调度、可被查找和可正确释放的对象关系。Linux 把多种用户接口收敛到 `kernel_clone()`，再由克隆标志选择资源共享方式。创建路径先在不可见阶段处理大部分可失败操作，然后发布关系，最后使任务可运行。x86_64 还必须为从未执行过的新任务构造首次切换现场。

## 核心数据结构

用户接口 `struct clone_args` 使用固定宽度字段，内部 `struct kernel_clone_args` 则携带已转换的指针、长度及内核入口信息。二者不是同一个 ABI 结构。

| 内部字段 | 作用 |
|---|---|
| `flags` | `CLONE_VM`、`CLONE_FILES` 等克隆选择 |
| `exit_signal` | 子任务退出时的通知信号；与标志字段分开 |
| `pidfd` / `parent_tid` / `child_tid` | 用户态结果或线程协议地址 |
| `stack` / `stack_size` / `tls` | 用户栈参数与线程局部存储（Thread-Local Storage，TLS）参数 |
| `set_tid` / `set_tid_size` | 检查点恢复等场景指定分层 PID |
| `fn` / `fn_arg` / `kthread` | 内核入口函数、参数及内核线程标记，不属于普通用户 clone ABI |

定义见 [`include/linux/sched/task.h:23-55`](../../linux/include/linux/sched/task.h#L23-L55)、[`include/uapi/linux/sched.h:92-110`](../../linux/include/uapi/linux/sched.h#L92-L110)。

## 主流程分析

```text
fork() / vfork() / clone() / clone3()   kernel/fork.c
└── kernel_clone()
    ├── copy_process()
    │   ├── 验证标志组合
    │   ├── dup_task_struct()
    │   ├── copy_creds()、创建限制检查
    │   ├── sched_fork()               kernel/sched/core.c
    │   ├── copy_files() / copy_fs() / copy_sighand() / copy_signal()
    │   ├── copy_mm() / copy_namespaces()
    │   ├── copy_thread()              arch/x86/kernel/process.c
    │   ├── alloc_pid()                kernel/pid.c
    │   ├── cgroup_can_fork() / sched_cgroup_fork()
    │   └── 在 tasklist_lock 下发布关联
    ├── trace_sched_process_fork()
    ├── wake_up_new_task()             kernel/sched/core.c
    └── 若为 vfork，等待子任务释放旧 mm

新任务被首次选中
ret_from_fork_asm()                    arch/x86/entry/entry_64.S
└── ret_from_fork()                    arch/x86/kernel/process.c
    ├── schedule_tail()
    ├── 可选 fn(fn_arg)
    └── syscall_exit_to_user_mode()
```

### 一、四个接口，共同的创建核心

`fork()` 设置退出通知为 `SIGCHLD`，不请求共享用户地址空间；`vfork()` 设置 `CLONE_VFORK | CLONE_VM`；旧 `clone()` 从标志低位拆出退出信号，见 [`kernel/fork.c:2694-2759`](../../linux/kernel/fork.c#L2694-L2759)。这些是内核系统调用入口，不能据此假定 libc 的 `fork()` 包装必然使用同名系统调用。

`clone3()` 使用结构体和显式长度，支持独立的 pidfd 输出位置、栈长度及扩展字段。参数复制与验证见 [`kernel/fork.c:2764-2918`](../../linux/kernel/fork.c#L2764-L2918)。它仍然调用 `kernel_clone()`，不是另一套任务构造器。

x86_64 的用户系统调用映射可从 [`arch/x86/entry/syscalls/syscall_64.tbl`](../../linux/arch/x86/entry/syscalls/syscall_64.tbl) 核实；本节从公共 C 层开始，不展开第 6 章的系统调用入口汇编。

### 二、标志不是任意可组合的开关

[`kernel/fork.c:1958-1971`](../../linux/kernel/fork.c#L1958-L1971)（v6.18.60）

```c
	/*
	 * Thread groups must share signals as well, and detached threads
	 * can only be started up within the thread group.
	 */
	if ((clone_flags & CLONE_THREAD) && !(clone_flags & CLONE_SIGHAND))
		return ERR_PTR(-EINVAL);

	/*
	 * Shared signal handlers imply shared VM. By way of the above,
	 * thread groups also imply shared VM. Blocking this case allows
	 * for various simplifications in other code.
	 */
	if ((clone_flags & CLONE_SIGHAND) && !(clone_flags & CLONE_VM))
		return ERR_PTR(-EINVAL);
```

其他约束包括 `CLONE_NEWNS` 不能与 `CLONE_FS` 同时使用，新 PID / 用户命名空间不能与同线程组创建组合，见 [`kernel/fork.c:1948-1999`](../../linux/kernel/fork.c#L1948-L1999)。共享信号处理器时若不共享地址空间，处理函数地址和用户栈等就会失去统一解释；约束本身也是实现能够简化的前提。

### 三、构造过程按资源所有权回滚

任务对象分配后，路径依次处理凭据、调度初始化、文件系统上下文、信号、地址空间、命名空间和架构现场，见 [`kernel/fork.c:2020-2212`](../../linux/kernel/fork.c#L2020-L2212)。中途失败跳到相应标签，只释放已经取得的资源。

错误路径不是“统一 `kfree(p)`”。共享资源需要放引用，复制资源需要析构，PID 预留和 pidfd 槽位也需要撤销，见 [`kernel/fork.c:2441-2506`](../../linux/kernel/fork.c#L2441-L2506)。理解标签时应先列资源取得顺序，再沿反方向核对。

### 四、发布父子与线程组关系

在 `tasklist_lock` 下，普通新进程设 `group_leader=p`、`tgid=pid`，新线程指向现有组长并继承 TGID。普通进程加入自然父任务的 `children` 和全局任务链表；线程加入 `signal->thread_head`。相关构造见 [`kernel/fork.c:2259-2266`](../../linux/kernel/fork.c#L2259-L2266)、[`kernel/fork.c:2322-2448`](../../linux/kernel/fork.c#L2322-L2448)。

`CLONE_PARENT`、`CLONE_THREAD` 对自然父关系还有特殊规则。不能认为每次调用 clone 的当前线程都必然成为新任务的等待父进程。

### 五、可被查到之后，才允许首次运行

`copy_process()` 返回成功后，`kernel_clone()` 先发出 fork 事件、取得 PID 引用并处理必要的父侧结果，再调用 `wake_up_new_task()`，见 [`kernel/fork.c:2613-2645`](../../linux/kernel/fork.c#L2613-L2645)。此后子任务可能在其他 CPU 上迅速执行并退出，所以源码特意把需要访问子任务的操作安排在唤醒之前。

普通 fork 没有“父先子后”的用户态执行保证。子任务是否已经入队、第一次获得 CPU 及父侧返回时刻，需要分别观测。

### 六、子任务为何从 fork 返回 0

x86_64 的 `copy_thread()` 在新内核栈上设置 `fork_frame`，把返回地址指向 `ret_from_fork_asm()`；普通用户任务复制父寄存器后，把 `childregs->ax` 设为 0，必要时替换用户栈指针，见 [`arch/x86/kernel/process.c:170-186`](../../linux/arch/x86/kernel/process.c#L170-L186)、[`arch/x86/kernel/process.c:239-269`](../../linux/arch/x86/kernel/process.c#L239-L269)。

这不是在用户态重新调用了一遍 fork。父侧的返回值来自 `kernel_clone()` 返回的子任务编号；子侧来自预先构造的寄存器现场。汇编包装与 C 层收尾见 [`arch/x86/entry/entry_64.S:228-260`](../../linux/arch/x86/entry/entry_64.S#L228-L260)、[`arch/x86/kernel/process.c:151-168`](../../linux/arch/x86/kernel/process.c#L151-L168)。

## 关键细节 / 设计考量

### 创建成功不等于线程入口已执行

`copy_process()` 成功、`sched_process_fork` 事件、首次 `sched_switch` 和用户函数入口是不同边界。线程库还需在用户态启动包装中设置运行时状态。把 fork 事件时间直接当成用户线程开始执行时间，会把调度延迟与用户运行时开销混在一起。

### `CLONE_PIDFD` 在本版本可配合线程创建

当前代码在 `CLONE_THREAD` 情况下为 pidfd 准备 `PIDFD_THREAD`，见 [`kernel/fork.c:2214-2240`](../../linux/kernel/fork.c#L2214-L2240)。不要从旧版本限制推导“`CLONE_PIDFD` 与 `CLONE_THREAD` 永远互斥”。旧 clone 的 pidfd 与 `parent_tid` 共用参数地址，另有冲突检查，见 [`kernel/fork.c:2582-2594`](../../linux/kernel/fork.c#L2582-L2594)。

## 可观测性

| 入口 | 看什么、怎么解读 | 源码与配置 |
|---|---|---|
| `sched:sched_process_fork` | `parent_pid`、`child_pid` 是任务编号，可包含线程创建；发出时子任务尚未由这里唤醒 | [`include/trace/events/sched.h:396-419`](../../linux/include/trace/events/sched.h#L396-L419)、[`kernel/fork.c:2623-2625`](../../linux/kernel/fork.c#L2623-L2625)，`CONFIG_EVENT_TRACING=y` |
| `task:task_newtask` | `clone_flags` 可区分共享方式；在 `copy_process()` 的成功尾部发出 | [`include/trace/events/task.h:9-32`](../../linux/include/trace/events/task.h#L9-L32)、[`kernel/fork.c:2428-2439`](../../linux/kernel/fork.c#L2428-L2439)，`CONFIG_EVENT_TRACING=y` |
| `syscalls:sys_enter_clone`、`syscalls:sys_exit_clone`、clone3 对应事件 | 请求参数、错误码和父侧返回值；失败没有成功 fork 事件 | syscall 定义 [`kernel/fork.c:2721-2759`](../../linux/kernel/fork.c#L2721-L2759)、[`kernel/fork.c:2895-2918`](../../linux/kernel/fork.c#L2895-L2918)，元数据生成见 [`include/linux/syscalls.h`](../../linux/include/linux/syscalls.h)、[`kernel/trace/trace_syscalls.c`](../../linux/kernel/trace/trace_syscalls.c)，`CONFIG_FTRACE_SYSCALLS=y` |
| `kernel_clone()` / `copy_process()` 动态探针 | 创建总耗时、错误返回；`copy_process()` 返回的是指针或错误指针，不是 PID | 定义同本节主流程，`CONFIG_KPROBES=y`；先确认目标构建中可探测符号 |

## 小结

- fork、vfork、clone 与 clone3 汇合到共同创建核心。
- 克隆标志决定资源关系，同时受一组不可违反的组合约束。
- 创建过程有构造、发布和唤醒三个边界，失败回滚遵守资源所有权。
- 子侧返回 0 来自预先构造的 x86_64 寄存器现场。
- 观测 fork 事件时必须区分任务创建、线程组创建和首次执行。

## 参考

- [`include/uapi/linux/sched.h`](../../linux/include/uapi/linux/sched.h)：克隆参数与标志。
- [`Documentation/trace/events.rst`](../../linux/Documentation/trace/events.rst)。
- [3.4 任务创建与唤醒](../03-scheduler/04-wakeup.md)。
