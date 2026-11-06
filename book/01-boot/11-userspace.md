# 1.11 PID 1：从内核初始化到用户程序

> **本节源码**：[`init/main.c`](../../linux/init/main.c)、[`kernel/fork.c`](../../linux/kernel/fork.c)、[`fs/exec.c`](../../linux/fs/exec.c)、[`fs/binfmt_elf.c`](../../linux/fs/binfmt_elf.c)、[`arch/x86/kernel/process.c`](../../linux/arch/x86/kernel/process.c)
> **关键函数**：`kernel_init()`、`kernel_init_freeable()`、`run_init_process()`、`kernel_execve()`、`ret_from_fork()`
> **前置知识**：1.7 PID 1 的创建、1.9 初始化代码生命周期、1.10 根文件系统

## 概述

PID 1 并不是在所有初始化结束后才突然创建的用户进程。它早在 `rest_init()` 中就被创建，先执行内核函数 `kernel_init()`，再用 exec 机制替换为用户程序。这个过程不需要再 fork 一个“真正的 init”，PID 保持不变。理解这一边界，还需要区分程序路径选择、文件可访问、可执行文件装载成功和真正返回用户态四个步骤。

## 核心数据与生命周期

| 对象或标记 | 作用 | 依据 |
|---|---|---|
| `ramdisk_execute_command` | 初始根程序路径，默认 `/init`，由 `rdinit=` 改写 | [`init/main.c:164-165`](../../linux/init/main.c#L164-L165)、[`init/main.c:582-592`](../../linux/init/main.c#L582-L592) |
| `execute_command` | `init=` 指定的普通 init 路径 | [`init/main.c:565-580`](../../linux/init/main.c#L565-L580) |
| `argv_init` / `envp_init` | 首个程序的参数与环境 | 定义与参数归类见 [`init/main.c:194-196`](../../linux/init/main.c#L194-L196)、[`init/main.c:480-563`](../../linux/init/main.c#L480-L563) |
| `__init` | 初始化结束后可释放的代码 | [`include/linux/init.h:43-48`](../../linux/include/linux/init.h#L43-L48) |
| `__ref` | 标记跨初始化段引用的特殊代码，不代表可任意忽略生命周期 | [`include/linux/init.h:62-71`](../../linux/include/linux/init.h#L62-L71) |

`kernel_init()` 带 `__ref`，而 `kernel_init_freeable()` 带 `__init`。后者返回后才能释放初始化段，仍需执行的 init 路径选择代码不能一起被回收。

## 主流程分析

```text
rest_init()                            init/main.c
└── user_mode_thread(kernel_init)      kernel/fork.c
    └── 新任务首次得到调度
        └── ret_from_fork()            arch/x86/kernel/process.c
            └── kernel_init()         init/main.c
                ├── wait_for_completion(kthreadd_done)
                ├── kernel_init_freeable()
                ├── async_synchronize_full()
                ├── free_initmem()
                ├── mark_readonly() / pti_finalize()
                └── run_init_process()
                    └── kernel_execve()   fs/exec.c
                        └── bprm_execve()
                            └── 可执行格式处理器〔例如 load_elf_binary〕
            → ret_from_fork() 完成返回用户态准备
```

最后几层包含 exec 框架的格式选择与间接调用，不表示 `bprm_execve()` 只支持 ELF。

### 为什么 PID 1 使用 `user_mode_thread()`

当前源码里 `kernel_thread()` 会在克隆参数中设置 `.kthread = 1`，而 `user_mode_thread()` 不设置这个标志，见 [`kernel/fork.c:2663-2691`](../../linux/kernel/fork.c#L2663-L2691)。两者都可以先以一个 C 函数作为新任务的执行入口，但前者被标为内核线程。

这一区别直接影响 exec：`kernel_execve()` 对 `PF_KTHREAD` 有显式拒绝检查，见 [`fs/exec.c:1892-1904`](../../linux/fs/exec.c#L1892-L1904)。因此不能根据“当前在内核态执行初始化函数”，就把 PID 1 在该版本中当作普通 `PF_KTHREAD` 内核线程。

### 回收初始化内存之前必须等待异步工作

`kernel_init_freeable()` 返回后，`kernel_init()` 先执行 `async_synchronize_full()`，再清理 kprobe、ftrace 等对初始化内存的使用，随后释放初始化段并收紧映射权限，见 [`init/main.c:1434-1467`](../../linux/init/main.c#L1434-L1467)。

当前 `CONFIG_STRICT_KERNEL_RWX=y`，`mark_readonly()` 会根据策略执行只读保护等收尾；`pti_finalize()` 再完成相应页表隔离（Page Table Isolation，PTI）工作。`system_state` 切到 `SYSTEM_RUNNING` 时还没有保证用户空间 init 已执行成功，更不能据此判断服务已经就绪。

### 程序选择有明确优先级

程序选择在 [`init/main.c:1471-1507`](../../linux/init/main.c#L1471-L1507)：

1. 若 `ramdisk_execute_command` 仍非空，先尝试 `/init` 或 `rdinit=` 指定的程序。
2. 若 `init=` 设置了 `execute_command`，再尝试该路径；失败会 panic，不继续默默回退。
3. 若 `CONFIG_DEFAULT_INIT` 为非空字符串，尝试该默认路径；当前为空字符串，跳过。
4. 按顺序尝试 `/sbin/init`、`/etc/init`、`/bin/init`、`/bin/sh`。
5. 全部失败，报告 `No working init found` 并 panic。

**`init=` 不会自动覆盖一个可成功执行的 initramfs `/init`**。某些发行版的 `/init` 会自行读取并处理 `init=`，那是用户空间策略，不是这里改变了内核优先级。

还需留意失败时点：若 `/init` 在访问检查阶段不可用，内核已在 1.10 的路径中调用 `prepare_namespace()`；若它访问检查通过但真正 exec 失败，此处不会再自动回到 `prepare_namespace()` 挂载另一个根，只会在当前根环境中继续后续候选。

### exec 成功后如何离开内核初始化函数

`run_init_process()` 设置 `argv_init[0]`，打印尝试路径，再调用 `kernel_execve()`，见 [`init/main.c:1352-1365`](../../linux/init/main.c#L1352-L1365)。后者建立 `linux_binprm`、复制内核提供的参数和环境，交给通用可执行文件装载框架，见 [`fs/exec.c:1892-1949`](../../linux/fs/exec.c#L1892-L1949)。

ELF 处理器装入程序及必要解释器并准备用户寄存器，尾部通过 `START_THREAD()` 设置执行入口和用户栈，见 [`fs/binfmt_elf.c:1379`](../../linux/fs/binfmt_elf.c#L1379)。成功后 `kernel_init()` 可以返回到新任务启动包装函数，由它完成返回用户态：

[`arch/x86/kernel/process.c:151-168`](../../linux/arch/x86/kernel/process.c#L151-L168)（v6.18.60）

```c
__visible void ret_from_fork(struct task_struct *prev, struct pt_regs *regs,
				     int (*fn)(void *), void *fn_arg)
{
	schedule_tail(prev);

	/* Is this a kernel thread? */
	if (unlikely(fn)) {
		fn(fn_arg);
		/*
		 * A kernel thread is allowed to return here after successfully
		 * calling kernel_execve().  Exit to userspace to complete the
		 * execve() syscall.
		 */
		regs->ax = 0;
	}

	syscall_exit_to_user_mode(regs);
}
```

这里保留了源码中的历史措辞；就本版本 PID 1 的实际创建标志而言，仍应以前面的 `user_mode_thread()` 和 `PF_KTHREAD` 检查为准。不要把“exec 成功不会返回到旧用户程序”的常识误套成“内核装载辅助函数绝不能返回”。

## 关键细节 / 设计考量

### “路径存在”不足以保证 exec 成功

ELF 解释器、动态链接器、依赖库或脚本解释器缺失，都可能导致看得见的程序无法执行。架构不匹配、权限与格式错误也会失败。排查时应使用日志中的返回码、检查解释器及依赖，而不是只执行一次 `ls /sbin/init`。内核故障排查文档见 [`Documentation/admin-guide/init.rst`](../../linux/Documentation/admin-guide/init.rst)。

### 三个标准文件描述符来自初始控制台

`console_on_rootfs()` 在初始根中打开 `/dev/console`，并通过三次 `init_dup()` 为初始标准输入、输出和错误准备文件引用，见 [`init/main.c:1510-1522`](../../linux/init/main.c#L1510-L1522)。若失败会告警；程序没有输出不一定是根文件系统没有挂上，也可能是控制台链路有问题。

## 可观测性

| 入口 | 看什么、如何解读 | 定义与配置 |
|---|---|---|
| `Run ... as init process` 日志 | 表示开始尝试该路径，不是成功证明；要结合后续错误 | [`init/main.c:1352-1378`](../../linux/init/main.c#L1352-L1378)，`CONFIG_PRINTK=y` |
| `sched:sched_process_exec` | 成功装载后的 `filename`、`pid`、`old_pid`；若 PID 为 1，可定位其 exec，但不等于用户入口首条指令已执行 | [`include/trace/events/sched.h:424-448`](../../linux/include/trace/events/sched.h#L424-L448)，发出点 [`fs/exec.c:1747`](../../linux/fs/exec.c#L1747)，`CONFIG_EVENT_TRACING=y` |
| `/proc/1/exe`、`/proc/1/cmdline` | PID 1 当前映像与参数；可能已被早期 `/init` 再次 exec，不保留完整历史 | [`fs/proc/base.c:3322-3334`](../../linux/fs/proc/base.c#L3322-L3334)，`CONFIG_PROC_FS=y`，读取受权限控制 |

要抓 PID 1 的首轮 exec，可在临时启动项中使用 `trace_event=sched:sched_process_exec`。登录后挂 `kernel_execve()` 或打开事件，只能抓到之后的行为，不能补回本次首次交接。

## 动手实验：不接磁盘的一次性 initramfs

以下实验在 dev 虚拟机内运行一个嵌套的 QEMU x86_64 客体，不替换 Vagrant 系统内核，也不接入宿主磁盘。前提是已有目标构建的 `bzImage`，并安装 `gcc`、静态 libc 开发文件、`cpio` 与 `qemu-system-x86_64`。所有临时文件写在 `/tmp`，**不在只读的 `linux/` 中构建或修改文件**。本实验未实测，若工具或静态链接依赖缺失，应先补齐环境。

在 dev 中执行；以下 C 程序是实验用户程序，不是内核源码摘录：

```bash
set -eu
image=/root/linux/linux/arch/x86/boot/bzImage
test -r "$image"
work=$(mktemp -d /tmp/linux-boot.XXXXXX)
mkdir -p "$work/root/proc" "$work/root/dev"
export work
python3 - <<'PY'
import os
from pathlib import Path
Path(os.environ['work'], 'init.c').write_text(r'''
#include <stdio.h>
#include <unistd.h>
#include <sys/mount.h>
#include <sys/utsname.h>
int main(void)
{
    struct utsname u;
    setvbuf(stdout, NULL, _IONBF, 0);
    if (mount("proc", "/proc", "proc", 0, NULL) < 0)
        perror("mount proc");
    if (uname(&u) == 0)
        printf("release=%s pid=%ld\n", u.release, (long)getpid());
    puts("PID 1 is running in userspace; stop QEMU with Ctrl-a x.");
    for (;;)
        pause();
}
''')
PY
gcc -static -O2 -o "$work/root/init" "$work/init.c"
```

内核默认内置归档包含 `/dev/console`，本例外部归档不替换它，出处见 [`usr/default_cpio_list`](../../linux/usr/default_cpio_list)。确认当前目标配置没有改掉这项前提后，打包并启动：

```bash
(cd "$work/root" && find . -print0 | cpio --null -o --format=newc --owner=0:0) \
    > "$work/initramfs.cpio"
qemu-system-x86_64 -m 1024 -smp 2 -nographic -no-reboot -nic none \
    -kernel "$image" -initrd "$work/initramfs.cpio" \
    -append 'console=ttyS0 rdinit=/init'
```

未指定 KVM，允许以软件模拟运行，但可能较慢。客体没有磁盘；程序保持 PID 1 存活，避免它退出导致 panic。退出 QEMU 后可记录实际 `release`、PID 和日志，再删除本次临时目录：

```bash
rm -r -- "$work"
```

观察目标是实际出现 `pid=1` 的用户程序输出，并将其与 `Run /init as init process` 的内核日志区分。若只看到后者，应继续查执行错误和控制台，而不能宣称已经完成用户态交接。

## 小结

- PID 1 在较早阶段创建，先完成内核初始化，再 exec 用户程序，PID 不变。
- 本版本使用 `user_mode_thread()`，不能把 PID 1 等同于带 `PF_KTHREAD` 的普通内核线程。
- 异步初始化结束后才能释放初始化段；权限收紧发生在尝试 init 之前。
- `/init`、`init=`、编译期默认值和常规候选路径有明确优先级与不同失败处理。
- exec 成功装载与真正返回用户态由通用装载框架和架构返回路径共同完成。

## 参考

- [`Documentation/admin-guide/init.rst`](../../linux/Documentation/admin-guide/init.rst)。
- [`Documentation/filesystems/ramfs-rootfs-initramfs.rst`](../../linux/Documentation/filesystems/ramfs-rootfs-initramfs.rst)。
- [`arch/x86/entry/entry_64.S:228-260`](../../linux/arch/x86/entry/entry_64.S#L228-L260)：新任务的汇编返回包装。
