# 2.6 程序执行：exec 框架与 ELF 装载

> **本节源码**：[`fs/exec.c`](../../linux/fs/exec.c)、[`include/linux/binfmts.h`](../../linux/include/linux/binfmts.h)、[`fs/binfmt_elf.c`](../../linux/fs/binfmt_elf.c)、[`fs/binfmt_script.c`](../../linux/fs/binfmt_script.c)
> **关键函数**：`do_execveat_common()`、`bprm_execve()`、`search_binary_handler()`、`begin_new_exec()`、`de_thread()`、`load_elf_binary()`、`create_elf_tables()`
> **前置知识**：2.3 线程组身份；2.4 首次返回路径；2.5 资源继承

## 概述

fork 复制的是现有执行环境，而启动另一个程序需要在保留进程身份的同时更换地址空间和执行入口。exec 框架负责参数、权限与资源交接，可执行格式处理器负责解释文件布局。执行与链接格式（Executable and Linkable Format，ELF）处理器建立映射、装入必要的解释器并准备用户栈，不是简单把文件读到一个缓冲区后跳转。装载跨越不可返回点之后，失败也不能再回到已被破坏的旧程序。

## 核心数据结构

`linux_binprm` 是一次装载事务的状态，而不是长期进程描述符。

| 字段 / 对象 | 含义 | 源码 |
|---|---|---|
| `mm`、`vma`、`p` | 待安装地址空间、参数栈区域与当前栈位置 | [`include/linux/binfmts.h:18-30`](../../linux/include/linux/binfmts.h#L18-L30) |
| `file` / `interpreter` | 当前格式处理对象、脚本等格式重写后要处理的文件 | [`include/linux/binfmts.h:53-55`](../../linux/include/linux/binfmts.h#L53-L55) |
| `filename` / `interp` | 用户原始名称与实际解释路径的记录 | [`include/linux/binfmts.h:59-63`](../../linux/include/linux/binfmts.h#L59-L63) |
| `cred` / `secureexec` | 候选凭据与安全执行状态 | [`include/linux/binfmts.h:33-40`](../../linux/include/linux/binfmts.h#L33-L40)、[`include/linux/binfmts.h:56`](../../linux/include/linux/binfmts.h#L56) |
| `point_of_no_return` | 后续错误不能返回旧用户程序 | [`include/linux/binfmts.h:41-45`](../../linux/include/linux/binfmts.h#L41-L45) |
| `linux_binfmt.load_binary` | 格式处理回调；ELF 与脚本使用不同实现 | [`include/linux/binfmts.h:89-98`](../../linux/include/linux/binfmts.h#L89-L98) |

注意两种“解释器”：脚本的 `#!` 会让通用框架改为处理另一个可执行文件；ELF 的 `PT_INTERP` 则由 ELF 装载器装入动态链接器。二者不可直接等同。

## 主流程分析

```text
execve() / execveat()                   fs/exec.c
└── do_execveat_common()
    ├── alloc_bprm()、统计与复制 argv / envp
    └── bprm_execve()
        ├── prepare_bprm_creds()
        ├── sched_exec()               kernel/sched/core.c
        └── exec_binprm()
            └── search_binary_handler()
                ├── load_script()      fs/binfmt_script.c
                │   └── 设置解释文件，交回通用框架重试
                └── load_elf_binary()   fs/binfmt_elf.c
                    ├── 检查 ELF 与程序头、打开 PT_INTERP
                    ├── begin_new_exec()  fs/exec.c
                    │   ├── de_thread()
                    │   ├── unshare_files() / do_close_on_exec()
                    │   ├── exec_mmap()
                    │   └── 重置信号处理器、提交凭据等
                    ├── 映射 PT_LOAD、装载 ELF 解释器
                    ├── create_elf_tables()
                    └── finalize_exec() / START_THREAD()
```

### 一、在新地址空间中准备参数

`do_execveat_common()` 检查参数，分配 `linux_binprm`，统计并复制参数及环境字符串，再进入 `bprm_execve()`，见 [`fs/exec.c:1807-1889`](../../linux/fs/exec.c#L1807-L1889)。准备阶段就可能因参数过长、地址无效或权限等失败；这还不是“程序执行了一半”。

`bprm_execve()` 准备候选凭据、执行安全检查和调度侧 exec 放置，再调用格式处理流程，见 [`fs/exec.c:1753-1789`](../../linux/fs/exec.c#L1753-L1789)。`sched_exec()` 可能调整 CPU 放置，但不意味着创建新任务。

### 二、格式识别由回调分派

`search_binary_handler()` 遍历注册格式，调用 `load_binary()`。若返回 `-ENOEXEC` 且尚未跨越不可返回点，可以尝试后面的格式；其他结果不再按“不认识格式”继续处理，见 [`fs/exec.c:1674-1705`](../../linux/fs/exec.c#L1674-L1705)。

脚本处理器解析 `#!`，调整参数并打开解释器，见 [`fs/binfmt_script.c:34-148`](../../linux/fs/binfmt_script.c#L34-L148)。通用 `exec_binprm()` 再以解释器文件重复格式识别，见 [`fs/exec.c:1708-1749`](../../linux/fs/exec.c#L1708-L1749)。深度限制应以这里的实际条件为准，不能只引用旁边旧注释推断允许层数。

### 三、不可返回点早于旧地址空间释放

[`fs/exec.c:1124-1140`](../../linux/fs/exec.c#L1124-L1140)（v6.18.60）

```c
	/*
	 * This tracepoint marks the point before flushing the old exec where
	 * the current task is still unchanged, but errors are fatal (point of
	 * no return). The later "sched_process_exec" tracepoint is called after
	 * the current task has successfully switched to the new exec.
	 */
	trace_sched_prepare_exec(current, bprm);

	/*
	 * Ensure all future errors are fatal.
	 */
	bprm->point_of_no_return = true;

	/* Make this the only thread in the thread group */
	retval = de_thread(me);
	if (retval)
		goto out;
```

不可返回点（Point of No Return）设置后，代码开始进行不能完整回滚的线程组与资源交接。它并不是紧贴 `exec_mmap()` 的同义词。若后续失败且没有已有致命信号，`bprm_execve()` 强制致命 `SIGSEGV`，见 [`fs/exec.c:1790-1804`](../../linux/fs/exec.c#L1790-L1804)。

### 四、多线程 exec 要保留进程身份，而不是全部线程

`de_thread()` 终止并等待其他线程；调用者若不是组长，还需要等待旧组长进入退出状态，交换 TID 并迁移 TGID、PGID、SID 关联，见 [`fs/exec.c:912-1036`](../../linux/fs/exec.c#L912-L1036)。因此 exec 不创建新进程，但可以改变执行它的那个线程的 TID。

随后 `begin_new_exec()` 确保文件表私有，关闭 `FD_CLOEXEC` 槽位，安装新地址空间，使信号处理表私有并重置被捕获的处理器，最后提交候选凭据。顺序见 [`fs/exec.c:1158-1213`](../../linux/fs/exec.c#L1158-L1213)、[`fs/exec.c:1272-1290`](../../linux/fs/exec.c#L1272-L1290)。忽略的信号处理方式通常保留，不是“所有信号状态清零”。

### 五、ELF 的入口未必是主程序入口

`PT_LOAD` 程序段决定映射范围和保护属性；ELF 处理器先验证文件、寻找 `PT_INTERP` 并读取解释器，再进行不可回滚的切换，见 [`fs/binfmt_elf.c:832-1025`](../../linux/fs/binfmt_elf.c#L832-L1025)。文件内容通常通过映射和后续缺页取得，而不是 exec 时保证全部驻留内存。

若存在 ELF 解释器，最终 `elf_entry` 指向解释器入口；否则指向主程序入口。`create_elf_tables()` 为用户栈建立参数、环境和辅助向量（Auxiliary Vector，auxv），其中 `AT_ENTRY` 仍记录主程序入口、`AT_BASE` 记录解释器装载基址，见 [`fs/binfmt_elf.c:250-259`](../../linux/fs/binfmt_elf.c#L250-L259)、[`fs/binfmt_elf.c:1246-1306`](../../linux/fs/binfmt_elf.c#L1246-L1306)。

尾部 `START_THREAD()` 设置用户寄存器的指令与栈入口，见 [`fs/binfmt_elf.c:1378-1380`](../../linux/fs/binfmt_elf.c#L1378-L1380)。通常是动态链接器再完成库装载、重定位等用户态工作，随后进入程序启动代码；内核并不直接调用 C 语言的 `main()`。

## 关键细节 / 设计考量

### exec 保留与重置的资源要逐项判断

PID 和普通父子关系保留；地址空间替换；被捕获的信号处理函数重置；普通未标记 `FD_CLOEXEC` 的文件描述符继续存在，但共享文件表先被解除。原信号阻塞掩码不因“换程序”就必然清空。依据分别见本节 `de_thread()`、`begin_new_exec()`，以及 [`kernel/signal.c:543-557`](../../linux/kernel/signal.c#L543-L557)。

### 看得见的程序也可能执行失败

脚本解释器或 ELF 动态链接器缺失，都可能导致原文件存在却无法执行。权限、架构、参数和映射错误需要分开排查。不能把所有 `ENOENT` 都解释为用户指定的可执行文件本身不存在。

## 可观测性

| 入口 | 看什么、怎么解读 | 源码与配置 |
|---|---|---|
| `sched:sched_prepare_exec` | `filename`、`interp`、旧任务身份；已进入失败会终止的交接边界，不是成功事件 | [`include/trace/events/sched.h:448-481`](../../linux/include/trace/events/sched.h#L448-L481)、[`fs/exec.c:1130`](../../linux/fs/exec.c#L1130)，`CONFIG_EVENT_TRACING=y` |
| `sched:sched_process_exec` | 成功装载后的 `pid`、装载前 `old_pid`、原始 `filename`；两者不同可提示非组长 exec | [`include/trace/events/sched.h:424-445`](../../linux/include/trace/events/sched.h#L424-L445)、[`fs/exec.c:1747`](../../linux/fs/exec.c#L1747)，`CONFIG_EVENT_TRACING=y` |
| `/proc/<pid>/exe`、`cmdline`、`maps`、`auxv` | 当前执行文件、参数、映射及辅助向量；不能恢复完整 exec 历史 | 注册见 [`fs/proc/base.c:3307-3338`](../../linux/fs/proc/base.c#L3307-L3338)，映射输出见 [`fs/proc/task_mmu.c`](../../linux/fs/proc/task_mmu.c)，`CONFIG_PROC_FS=y`、`CONFIG_MMU=y`，读取受权限控制 |
| `syscalls:sys_exit_execve` 等事件 | 辅助观察错误返回；只统计成功 exec 应使用成功事件而非仅入口次数 | [`fs/exec.c:2028-2043`](../../linux/fs/exec.c#L2028-L2043)、[`kernel/trace/trace_syscalls.c`](../../linux/kernel/trace/trace_syscalls.c)，`CONFIG_FTRACE_SYSCALLS=y` |

## 动手实验：把 fork 和 exec 分开记录

[lifecycle.c](examples/lifecycle.c) 创建一个子进程，由子进程 exec 同一示例的另一运行模式。它是用户实验程序，不是内核源码。dev 中执行：

```bash
cd /root/linux
work=$(mktemp -d /tmp/process-exec.XXXXXX)
cc -O2 -Wall -Wextra -std=c11 book/02-process/examples/lifecycle.c -o "$work/lifecycle"
strace -f -e trace=clone,clone3,fork,vfork,execve,wait4,waitid,exit_group \
    "$work/lifecycle" --run

# 可选：权限允许时，按进程及其后代采集生命周期事件
perf record -o "$work/lifecycle.data" \
    -e sched:sched_process_fork -e sched:sched_process_exec \
    -e sched:sched_process_exit -- "$work/lifecycle" --run
perf script -i "$work/lifecycle.data"
rm -r -- "$work"
```

观察同一个子进程身份经历 exec，而不是在 exec 时又派生一个新进程。libc 实际采用 clone 还是 fork，以跟踪输出为准。程序包含有意的短时等待以便观察，不把耗时作为性能基准。可选 perf 步骤需要安装对应工具并具备采集权限；这里按进程及后代采集，不重置全局 tracefs，也不保证抓到在其他上下文发出的延迟释放事件。本实验未在目标内核上实测。

## 小结

- exec 是当前进程的装载事务，不是创建新任务的接口。
- 通用框架与格式处理器分工，脚本解释器和 ELF 动态链接器是不同机制。
- 不可返回点早于旧 mm 替换，跨越之后的失败不能返回旧用户代码。
- 多线程 exec 保留进程身份，但非组长调用者的 TID 可能改变。
- ELF 装载准备映射、用户栈和入口，动态程序往往先运行链接器。

## 参考

- [`Documentation/admin-guide/init.rst`](../../linux/Documentation/admin-guide/init.rst)：执行初始用户程序失败的排查线索。
- [`fs/binfmt_script.c`](../../linux/fs/binfmt_script.c)、[`fs/binfmt_elf.c`](../../linux/fs/binfmt_elf.c)。
- [1.11 PID 1：从内核初始化到用户程序](../01-boot/11-userspace.md)。
