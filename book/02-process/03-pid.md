# 2.3 PID、线程组与进程标识

> **本节源码**：[`include/linux/pid.h`](../../linux/include/linux/pid.h)、[`include/linux/pid_namespace.h`](../../linux/include/linux/pid_namespace.h)、[`kernel/pid.c`](../../linux/kernel/pid.c)、[`kernel/sys.c`](../../linux/kernel/sys.c)、[`fs/pidfs.c`](../../linux/fs/pidfs.c)
> **关键函数**：`alloc_pid()`、`pid_nr_ns()`、`task_active_pid_ns()`、`pidfd_prepare()`、`pidfd_poll()`
> **前置知识**：2.1 任务与线程组；2.2 任务对象生命周期

## 概述

进程标识符（Process ID，PID）既用于用户接口，也用于内核查找任务，但一个整数不足以稳定表示长期存在的目标。任务退出后，数字可以被重用；命名空间又允许同一个任务在不同层看到不同编号。Linux 通过 `struct pid` 把稳定对象身份、分层数字编号和任务关联分开。线程组、进程组、会话也复用这套标识基础，但不是同一类关系。

## 核心数据结构

[`include/linux/pid.h:52-55`](../../linux/include/linux/pid.h#L52-L55)（v6.18.60）

```c
struct upid {
	int nr;
	struct pid_namespace *ns;
};
```

`struct pid` 的关键字段见 [`include/linux/pid.h:57-77`](../../linux/include/linux/pid.h#L57-L77)：

| 字段 | 含义 |
|---|---|
| `count` | 标识对象引用计数，不是任务数 |
| `level` | 最深 PID 命名空间层级 |
| `numbers[]` | 各层 `upid`；一个身份对象可在祖先命名空间中有不同数字 |
| `tasks[PIDTYPE_MAX]` | 按 PID 类型组织任务关联 |
| `wait_pidfd` | pidfd 退出通知所用等待队列 |
| `ino`、`pidfs_node` 等 | pidfs 身份与文件系统关联，不应将本版本描述为只有旧式匿名文件包装 |

与之配合的 `pid_namespace` 保存 `idr` 编号索引、`level` 层级、`parent` 父空间、`child_reaper` 收割者和 `pid_max` 编号上限，见 [`include/linux/pid_namespace.h:26-38`](../../linux/include/linux/pid_namespace.h#L26-L38)。数字查找属于某一命名空间的索引操作，稳定引用则属于 `struct pid` 的对象寿命管理。

### 四种关系标识

| 类型 | 用户空间语义 | 内核关系 |
|---|---|---|
| `PIDTYPE_PID` | 线程标识符（Thread ID，TID） | 标识一个任务 |
| `PIDTYPE_TGID` | 线程组标识符（Thread Group ID，TGID），通常是用户所说的 PID | 标识线程组 |
| `PIDTYPE_PGID` | 进程组标识符（Process Group ID，PGID） | 多个进程的作业控制分组 |
| `PIDTYPE_SID` | 会话标识符（Session ID，SID） | 会话及控制终端关系 |

类型定义见 [`include/linux/pid_types.h`](../../linux/include/linux/pid_types.h)。`getpid()` 返回命名空间内 TGID，`gettid()` 返回调用任务的 TID，见 [`kernel/sys.c:991-1008`](../../linux/kernel/sys.c#L991-L1008)。组长的 TID 与 TGID 相等；其他线程的 TID 不等于 TGID。

`task_struct.pid`、`tgid` 是初始 PID 命名空间编号，不应直接当成任意容器内的返回值。赋值见 [`kernel/fork.c:2259-2266`](../../linux/kernel/fork.c#L2259-L2266)，按命名空间转换的辅助函数见 [`kernel/pid.c:490-529`](../../linux/kernel/pid.c#L490-L529)。

## 主流程分析

```text
copy_process()                          kernel/fork.c
├── alloc_pid(pid_ns_for_children, ...) kernel/pid.c
│   ├── 从最深层向祖先命名空间预留数字
│   ├── 填写 numbers[]、初始化引用和等待队列
│   └── idr_replace() 发布已初始化的 struct pid
├── init_task_pid()                     kernel/fork.c
└── attach_pid()                        kernel/pid.c

查找与编号转换                         kernel/pid.c
find_pid_ns() / find_vpid() → struct pid
pid_task() / get_pid_task() → task_struct
pid_nr_ns() / pid_vnr() → 调用视图中的整数
```

### 一、分配的是一组层次编号

`alloc_pid()` 从 `ns->level` 向 0 逐层分配。普通分配使用 `idr_alloc_cyclic()`，检查点恢复可通过 `set_tid` 请求指定编号，但须通过层级、范围、PID 1 存在性和权限检查，见 [`kernel/pid.c:163-250`](../../linux/kernel/pid.c#L163-L250)。

分配时先在 IDR 中保存空指针，避免查找者拿到未初始化对象；全部初始化完成后，再用 `idr_replace()` 发布实际指针，见 [`kernel/pid.c:232-289`](../../linux/kernel/pid.c#L232-L289)。这里发布的是 PID 对象，任务关联还由 `copy_process()` 随后建立。

### 二、查找必须指定视图并考虑对象寿命

`find_vpid()` 使用调用任务的活动 PID 命名空间；`pid_nr_ns()` 只有在目标对象的层次路径包含指定命名空间时才返回对应编号，否则返回 0，见 [`kernel/pid.c:319-327`](../../linux/kernel/pid.c#L319-L327)、[`kernel/pid.c:490-508`](../../linux/kernel/pid.c#L490-L508)。

因此“返回 0”不表示目标是 idle 任务，也可能只是该身份在这个命名空间不可见。`getppid()` 在父任务位于视图外时也可呈现 0，不能据此宣称目标没有父关系。

查找函数得到的指针仍要遵守 RCU、锁和引用协议。需要跨越查找临界区保存对象时，应使用相应取引用接口；`get_task_pid()`、`get_pid_task()` 的实现见 [`kernel/pid.c:456-476`](../../linux/kernel/pid.c#L456-L476)，不能把数字转指针后的裸值长期缓存。

### 三、数字回收不等于身份对象立即销毁

`free_pid()` 从各层 IDR 中移除编号，并通过 RCU 延迟放下对象引用，见 [`kernel/pid.c:117-148`](../../linux/kernel/pid.c#L117-L148)。外部持有的 `struct pid` 引用可以继续存在，但不能阻止同一个整数后来分配给另一个新对象。

这正是使用对象引用的意义：旧对象不会突然“变成”同号的新进程。持有对象也不保证它仍关联一个活任务，调用方仍须处理目标已退出的情况。

### 四、pidfd 把稳定身份带到用户空间

PID 文件描述符（PID File Descriptor，pidfd）以 fd 引用身份对象，适合发送信号、等待通知等需要避免 PID 重用竞态的操作。`pidfd_open()` 的入口见 [`kernel/pid.c:620-662`](../../linux/kernel/pid.c#L620-L662)，底层文件分配见 [`fs/pidfs.c:1075-1102`](../../linux/fs/pidfs.c#L1075-L1102)。

本版本支持 `PIDFD_THREAD`：不带该标志的 `pidfd_open()` 要求目标为线程组组长，带标志可以指定单个线程，见 [`kernel/pid.c:633-661`](../../linux/kernel/pid.c#L633-L661)。所以“pidfd 永远不能表示非组长线程”不是本版本的规则。

pidfd 不是权限绕过机制，也不是任意进程的回收权凭证。`waitid(P_PIDFD, ...)` 仍受等待关系检查；`poll()` 报告退出并不自动回收，后续应由有资格的父进程执行等待。等待入口见 [`kernel/exit.c:1743-1829`](../../linux/kernel/exit.c#L1743-L1829)。

## 关键细节 / 设计考量

### `pid_max` 不是最大存活任务数量

本版本把 `pid_max` 放在 `pid_namespace` 中，分配每一层编号时使用该层上限，见 [`kernel/pid.c:191-239`](../../linux/kernel/pid.c#L191-L239)。sysctl 表也按 PID 命名空间注册与替换数据指针，见 [`kernel/pid.c:718-765`](../../linux/kernel/pid.c#L718-L765)。不能机械沿用“唯一的全局 `pid_max`”模型。

即使某层还有空编号，创建仍可能被任务总数、用户限制或 cgroup 限制拒绝，详见 2.12。

### 进程树和作业控制分组不能混淆

`CLONE_PARENT` 改变的是父关系，`CLONE_THREAD` 改变线程组关系；`setpgid()`、`setsid()` 操作进程组 / 会话，见 [`kernel/sys.c:1114-1183`](../../linux/kernel/sys.c#L1114-L1183)、[`kernel/sys.c:1268-1306`](../../linux/kernel/sys.c#L1268-L1306)。同一进程组不意味着共享地址空间或有相同父进程。

## 可观测性

| 入口 | 看什么、怎么解读 | 源码与配置 |
|---|---|---|
| `/proc/<pid>/status` | `Pid`、`Tgid`、`NSpid`、`NStgid`、`NSpgid`、`NSsid`；多层编号从该 procfs 的视图起列出，不保证始终从宿主层起列 | [`fs/proc/array.c:180-222`](../../linux/fs/proc/array.c#L180-L222)，`CONFIG_PROC_FS=y`、`CONFIG_PID_NS=y` |
| `/proc/<pid>/fdinfo/<fd>` | 对 pidfd 输出 `Pid`、`NSpid`；身份已无任务关联时可显示 -1，跨不相交命名空间视图可显示 0 | [`fs/pidfs.c:179-242`](../../linux/fs/pidfs.c#L179-L242)，`CONFIG_PROC_FS=y`、多层编号依赖 `CONFIG_PID_NS=y` |
| pidfd 的 `poll()` / `epoll()` | 退出时可读；组长提前退出、其他线程仍在时不应直接视为整个进程退出 | [`fs/pidfs.c:247-269`](../../linux/fs/pidfs.c#L247-L269)；等待机制与 fd 状态共同决定行为 |
| `/proc/sys/kernel/pid_max` | 当前 PID 命名空间的编号上限，不是已占用数量 | [`kernel/pid.c:718-765`](../../linux/kernel/pid.c#L718-L765)，`CONFIG_SYSCTL=y` |

## 动手实验：同时查看标识关系

在 dev 中选择一个普通进程执行，以下命令未在目标内核上实测：

```bash
pid=$$
ps -L -p "$pid" -o pid,tid,ppid,pgid,sid,comm
grep -E '^(Pid|Tgid|PPid|NSpid|NStgid|NSpgid|NSsid):' /proc/"$pid"/status
```

若只显示一层命名空间编号，说明采样视图到目标之间没有更深的层次，不能据此判断内核没编译 PID 命名空间。嵌套命名空间的对照实验见 2.11。

## 小结

- TID 标识任务，TGID 标识线程组，PGID 和 SID 服务于作业控制。
- 一个 `struct pid` 可承载多层编号；整数相同不代表身份对象相同。
- PID 查找需要视图和寿命保护，转换结果为 0 也可能代表不可见。
- pidfd 提供稳定引用，但不保证目标存活，也不改变权限与父子等待关系。
- 本版本的 `pid_max` 按命名空间管理，与任务数量限制不同。

## 参考

- [`include/linux/pid.h`](../../linux/include/linux/pid.h)：对象引用与数字重用问题的源码注释。
- [`Documentation/filesystems/proc.rst`](../../linux/Documentation/filesystems/proc.rst)。
- [`Documentation/admin-guide/namespaces/resource-control.rst`](../../linux/Documentation/admin-guide/namespaces/resource-control.rst)。
