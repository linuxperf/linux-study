# 2.2 任务描述符、内核栈与任务状态

> **本节源码**：[`include/linux/sched.h`](../../linux/include/linux/sched.h)、[`kernel/fork.c`](../../linux/kernel/fork.c)、[`kernel/exit.c`](../../linux/kernel/exit.c)、[`arch/x86/include/asm/current.h`](../../linux/arch/x86/include/asm/current.h)、[`fs/proc/array.c`](../../linux/fs/proc/array.c)
> **关键函数**：`dup_task_struct()`、`get_current()`、`task_state_index()`、`put_task_struct_rcu_user()`
> **前置知识**：2.1 统一任务模型；引用计数与链表基础

## 概述

内核必须把执行现场、调度状态和资源引用放到可长期寻址的对象中，同时避免把整个进程环境复制进每一个线程。`task_struct` 是这一组织中心，但它不是用户内存、内核栈和 PID 对象的共同存储区。任务状态用于协调睡眠与唤醒，退出状态则用于协调通知与回收。把这些状态和生命周期分开，是理解 `ps` 输出、退出竞态及内核对象访问规则的前提。

## 核心数据结构

以下为字段索引，不是连续的结构体源码摘录。

| 字段 | 含义 | 源码 |
|---|---|---|
| `thread_info` | 架构线程标志；本配置 `CONFIG_THREAD_INFO_IN_TASK=y`，内嵌在对象开头 | [`include/linux/sched.h:815-823`](../../linux/include/linux/sched.h#L815-L823) |
| `__state` | 可运行、睡眠、停止等状态 | [`include/linux/sched.h:823-826`](../../linux/include/linux/sched.h#L823-L826) |
| `stack` / `usage` / `flags` | 内核栈地址、任务对象引用计数、`PF_*` 属性 | [`include/linux/sched.h:834-837`](../../linux/include/linux/sched.h#L834-L837) |
| `on_cpu` / `on_rq` | 是否仍在 CPU 执行、运行队列关联状态；不能以 `__state` 代替 | [`include/linux/sched.h:844-859`](../../linux/include/linux/sched.h#L844-L859) |
| `mm` / `active_mm` | 用户地址空间与调度时使用的地址空间上下文 | [`include/linux/sched.h:957-958`](../../linux/include/linux/sched.h#L957-L958) |
| `exit_state` / `exit_code` | 退出回收阶段与退出状态值 | [`include/linux/sched.h:961-962`](../../linux/include/linux/sched.h#L961-L962) |
| `real_parent` / `parent` | 自然父关系与通知、ptrace 相关父关系 | [`include/linux/sched.h:1072-1076`](../../linux/include/linux/sched.h#L1072-L1076) |
| `children` / `sibling` / `group_leader` | 父子链表与线程组组长 | [`include/linux/sched.h:1078-1083`](../../linux/include/linux/sched.h#L1078-L1083) |
| `comm` | 定长任务名，不是完整程序路径或全部命令行 | [`include/linux/sched.h:1162-1172`](../../linux/include/linux/sched.h#L1162-L1172) |
| `thread` | 架构相关线程状态，类型为 `thread_struct`，不是另一个调度任务 | [`include/linux/sched.h:1671-1672`](../../linux/include/linux/sched.h#L1671-L1672) |

### `current` 不是固定栈底的强制推导

x86_64 的 `current` 展开为 `get_current()`，从 per-CPU 的当前任务指针读取，见 [`arch/x86/include/asm/current.h:15-28`](../../linux/arch/x86/include/asm/current.h#L15-L28)。本配置不能沿用“把栈指针按线程栈大小对齐就得到 `thread_info`，再得到任务”的旧式模型。

### 内核栈与用户栈是两个对象

任务进入内核后需要独立的调用栈；用户栈只是用户地址空间的一部分。`dup_task_struct()` 分别申请任务对象和内核栈，见 [`kernel/fork.c:867-889`](../../linux/kernel/fork.c#L867-L889)。本配置 `CONFIG_VMAP_STACK=y`；栈分配实现见 [`kernel/fork.c:193-341`](../../linux/kernel/fork.c#L193-L341)，设计说明见 [`Documentation/mm/vmalloced-kernel-stacks.rst`](../../linux/Documentation/mm/vmalloced-kernel-stacks.rst)。

`CONFIG_PAGE_SIZE_4KB=y`、`CONFIG_KASAN=n` 时，`THREAD_SIZE` 为 16 KB，计算见 [`arch/x86/include/asm/page_64_types.h:7-16`](../../linux/arch/x86/include/asm/page_64_types.h#L7-L16)。这不是“一个任务只占 16 KB”，也不包含 CPU 独立的中断栈等额外对象。

### 不要混淆三种“线程现场”

`thread_info` 保存低层标志与入口工作状态；`thread_struct` 保存架构相关的内核栈指针、FS / GS 基址等；进入内核时的用户寄存器现场则由 `pt_regs` 表达。三者既不是同一个结构，也不是各自完整复制一遍所有寄存器，见 [`arch/x86/include/asm/thread_info.h:62-69`](../../linux/arch/x86/include/asm/thread_info.h#L62-L69)、[`arch/x86/include/asm/processor.h:448-474`](../../linux/arch/x86/include/asm/processor.h#L448-L474)。

创建时，`copy_thread()` 把 `thread.sp` 指向新内核栈上的启动帧，同时准备其中的 `pt_regs`，见 [`arch/x86/kernel/process.c:180-186`](../../linux/arch/x86/kernel/process.c#L180-L186)。这样描述执行现场，比笼统说“所有寄存器都保存在 `task_struct` 里”更准确。

## 主流程分析

```text
dup_task_struct()                       kernel/fork.c
├── alloc_task_struct_node()
├── arch_dup_task_struct()              arch/x86/kernel/process.c
├── alloc_thread_stack_node()
├── setup_thread_stack()
└── 初始化 usage、rcu_users 及任务私有状态

退出后的对象释放侧
put_task_struct_rcu_user()              kernel/exit.c
└── call_rcu()
    └── delayed_put_task_struct()
        └── put_task_struct()
            └── 引用归零后释放任务对象
```

第一条路径见 [`kernel/fork.c:867-973`](../../linux/kernel/fork.c#L867-L973)，第二条见 [`kernel/exit.c:228-245`](../../linux/kernel/exit.c#L228-L245)。两者之间还有退出、调度器释放和父进程回收等步骤，不是直接的成对函数调用。

### 一、初始复制不等于最终共享关系

`arch_dup_task_struct()` 提供初始内容，后续代码重新初始化计数器、链表和资源引用。不能根据任务对象最初来自父任务的复制，就认为所有成员都可以直接继承。比如 `copy_process()` 清空私有待决信号，重新建立子任务链表，见 [`kernel/fork.c:2079-2087`](../../linux/kernel/fork.c#L2079-L2087)。

### 二、可运行状态与退出状态分开存储

源码明确把 `__state` 和 `exit_state` 分开，防止一类状态的更新误改另一类，见 [`include/linux/sched.h:95-125`](../../linux/include/linux/sched.h#L95-L125)。

| 状态 | 所属字段 | 含义与限制 |
|---|---|---|
| `TASK_RUNNING` | `__state` | 值为 0，表示可运行或正在运行，不证明当前占用 CPU |
| `TASK_INTERRUPTIBLE` | `__state` | 可被相应信号唤醒的睡眠 |
| `TASK_UNINTERRUPTIBLE` | `__state` | 普通信号不能结束这种等待；不专指磁盘 I/O |
| `TASK_KILLABLE` | `__state` | `TASK_UNINTERRUPTIBLE` 与 `TASK_WAKEKILL` 的组合 |
| `TASK_IDLE` | `__state` | 不计普通负载的不可中断等待，不等于 CPU 的 idle 任务 |
| `TASK_NEW` / `TASK_WAKING` | `__state` | 新任务与唤醒协议中的中间状态 |
| `TASK_DEAD` | `__state` | 任务不再继续执行的调度侧状态 |
| `EXIT_ZOMBIE` / `EXIT_DEAD` | `exit_state` | 保留待回收状态 / 已进入回收阶段 |

宏定义见 [`include/linux/sched.h:104-147`](../../linux/include/linux/sched.h#L104-L147)。`TASK_KILLABLE` 和纯 `TASK_UNINTERRUPTIBLE` 可以都显示为 `D`；只看字符无法判断 `SIGKILL` 能否唤醒该等待。

### 三、状态设置是并发协议，不是普通赋值约定

睡眠者必须把“准备睡眠”与“检查条件”按正确顺序发布，避免唤醒者在检查与调度之间遗漏任务。`set_current_state()` 的屏障语义和示例就在 [`include/linux/sched.h:202-250`](../../linux/include/linux/sched.h#L202-L250)。实际代码应优先使用等待队列等已封装协议，不能只写一次 `__state` 再调用 `schedule()`。

调度器还需要处理任务正在另一 CPU 退出执行、已入队但尚未切换等窗口，所以 `__state`、`on_rq`、`on_cpu` 不是可互换的三个“是否运行”布尔量。完整唤醒顺序见 [3.4](../03-scheduler/04-wakeup.md)。

### 四、引用计数不替代 RCU，任务引用也不替代栈引用

初始化代码设置 `rcu_users=2`，分别覆盖可见状态和调度器侧使用，再以 `usage` 管理任务对象引用，见 [`kernel/fork.c:918-925`](../../linux/kernel/fork.c#L918-L925)。`put_task_struct_rcu_user()` 在相应计数归零后安排 RCU 回调，回调再放下任务引用。

即使持有任务对象，也不能假定退出后的内核栈仍可访问。内核栈有独立保护协议；读取栈的 proc 实现调用 `stack_trace_save_tsk()`，后者先尝试取得栈引用，见 [`fs/proc/base.c:460-493`](../../linux/fs/proc/base.c#L460-L493)、[`kernel/stacktrace.c:136-154`](../../linux/kernel/stacktrace.c#L136-L154)。从无锁查找获得的裸指针，更不能脱离 RCU 或其他保护后继续使用。

## 关键细节 / 设计考量

### `ps` 状态是投影，不是内部位掩码原样输出

`task_state_index()` 综合运行状态与退出状态，再把部分内部状态折叠为用户可见字符。比如冻结状态可显示为不可中断睡眠，见 [`include/linux/sched.h:1697-1735`](../../linux/include/linux/sched.h#L1697-L1735)。观测接口保留了稳定的用户语义，但有意隐藏了部分实现细节。

### 状态采样不是跨字段一致快照

读取 `/proc/<pid>/status` 后再读 `wchan` 时，目标可能已经醒来甚至退出。不要把不同时间采集的状态字符、等待点和线程数量拼成一个假定原子的对象快照。

## 可观测性

| 入口 | 看什么、怎么解读 | 源码与配置 |
|---|---|---|
| `/proc/<pid>/status`、`/proc/<pid>/task/<tid>/status` | `State`、`Pid`、`Tgid`、`Threads`；线程级观测应进入 `task` 目录 | [`fs/proc/array.c:142-224`](../../linux/fs/proc/array.c#L142-L224)、[`fs/proc/array.c:262-302`](../../linux/fs/proc/array.c#L262-L302)，`CONFIG_PROC_FS=y` |
| `/proc/<pid>/wchan` | 任务睡眠时的内核等待位置；0 不证明没有等待，可能受权限、符号或任务状态影响 | [`fs/proc/base.c:412-431`](../../linux/fs/proc/base.c#L412-L431)，`CONFIG_PROC_FS=y`、`CONFIG_KALLSYMS=y` |
| `/proc/<pid>/stack` | 当前可读到的内核栈，不是用户栈 | [`fs/proc/base.c:460-490`](../../linux/fs/proc/base.c#L460-L490)，`CONFIG_STACKTRACE=y`，并受权限检查 |
| `sched:sched_process_free` | RCU 回调走到放下任务引用的位置；不能把它当作精确物理内存释放时刻 | 定义 [`include/trace/events/sched.h:324-326`](../../linux/include/trace/events/sched.h#L324-L326)，发出点 [`kernel/exit.c:228-237`](../../linux/kernel/exit.c#L228-L237)，`CONFIG_EVENT_TRACING=y` |

## 动手实验：比较进程视图与线程视图

在 dev 中用一个已知多线程程序的 PID 替换下面参数；该命令是实验操作，不是内核源码：

```bash
pid=1234
ps -L -p "$pid" -o pid,tid,ppid,stat,wchan:32,comm
for status in /proc/"$pid"/task/*/status; do
    printf '\n%s\n' "$status"
    grep -E '^(Name|State|Tgid|Pid|PPid|Threads):' "$status"
done
```

预期同一线程组的 `Tgid` 相同、各线程的 `Pid` 不同，状态可以各不相同。进程可能在遍历过程中退出，应把文件消失视为正常竞态，不据此推断内核对象损坏。本实验未在目标内核上实测。

## 小结

- `task_struct`、内核栈和用户地址空间分开分配、分别管理生命周期。
- x86_64 的 `current` 来自 per-CPU 指针，不应套用旧的栈底推导模型。
- `TASK_RUNNING` 不等于正在占用 CPU；运行、队列和退出状态各有用途。
- 状态更新必须满足睡眠 / 唤醒协议，不能视为随意字段赋值。
- 任务引用、栈引用与 RCU 保护解决不同的生命周期问题。

## 参考

- [`Documentation/mm/vmalloced-kernel-stacks.rst`](../../linux/Documentation/mm/vmalloced-kernel-stacks.rst)。
- [`Documentation/filesystems/proc.rst`](../../linux/Documentation/filesystems/proc.rst)。
- [3.2 主调度函数与上下文切换](../03-scheduler/02-schedule.md)、[3.4 任务创建与唤醒](../03-scheduler/04-wakeup.md)。
