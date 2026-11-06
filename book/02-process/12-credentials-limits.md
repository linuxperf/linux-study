# 2.12 进程凭据、权限与创建限制

> **本节源码**：[`include/linux/cred.h`](../../linux/include/linux/cred.h)、[`kernel/cred.c`](../../linux/kernel/cred.c)、[`kernel/sys.c`](../../linux/kernel/sys.c)、[`kernel/fork.c`](../../linux/kernel/fork.c)、[`kernel/cgroup/pids.c`](../../linux/kernel/cgroup/pids.c)、[`fs/proc/array.c`](../../linux/fs/proc/array.c)
> **关键函数**：`prepare_creds()`、`commit_creds()`、`copy_creds()`、`__sys_setuid()`、`copy_process()`、`pids_can_fork()`、`pids_try_charge()`
> **前置知识**：2.4 创建回滚；2.6 exec 凭据交接；2.11 用户命名空间

## 概述

一个任务能否执行操作，不只取决于它的数字 PID，也取决于凭据、能力、命名空间和安全策略。Linux 用进程凭据（Credentials）对象保存身份与权限集合，更新时准备新对象并发布，而不是直接原地修改共享凭据。任务创建还可能同时受到用户数量限制、全局任务限制、编号空间和 cgroup 层级限制。理解这些边界，才能正确解释“权限不足”和“无法再创建线程”两类常见故障。

## 核心数据结构

`struct cred` 的定义见 [`include/linux/cred.h:113-149`](../../linux/include/linux/cred.h#L113-L149)。

| 字段 | 作用 |
|---|---|
| `uid` / `gid` | 实际用户 / 组身份 |
| `euid` / `egid` | 有效身份，参与通常的权限决策 |
| `suid` / `sgid` | 保存身份，参与后续身份切换规则 |
| `fsuid` / `fsgid` | 文件系统权限检查使用的身份 |
| `cap_permitted` / `cap_effective` | 可用能力范围与当前有效能力 |
| `cap_inheritable` / `cap_bset` / `cap_ambient` | exec 能力继承相关集合，不能当成三个同义副本 |
| `user_ns` | 身份和能力的命名空间解释范围 |
| `ucounts` | 用户与命名空间相关资源计数 |
| `group_info` | 附加组集合 |
| `security`、密钥相关字段 | LSM 和密钥子系统关联，依赖 `CONFIG_SECURITY=y`、`CONFIG_KEYS=y` |

`task_struct.real_cred` 是客观身份与原本的主观凭据，`cred` 是执行访问检查时的有效主观凭据，可被受控地临时覆盖，见 [`include/linux/sched.h:1151-1155`](../../linux/include/linux/sched.h#L1151-L1155)。二者通常相同，但设计上不是可以删掉一个的冗余字段。

内核使用 `kuid_t` / `kgid_t`，不能把命名空间内用户输入整数直接当作内核全局身份。

## 主流程分析

```text
setuid()                               kernel/sys.c
└── __sys_setuid()
    ├── make_kuid(current_user_ns(), uid)
    ├── prepare_creds()                 kernel/cred.c
    ├── 能力、身份与 LSM 检查
    ├── 更新候选凭据与 ucounts
    └── commit_creds() 或 abort_creds() kernel/cred.c

创建侧
copy_process()                         kernel/fork.c
├── copy_creds()                        kernel/cred.c
├── RLIMIT_NPROC、max_threads 等检查
├── alloc_pid()                         kernel/pid.c
└── cgroup_can_fork()                   kernel/cgroup/cgroup.c
    └── pids_can_fork()                 kernel/cgroup/pids.c
        └── pids_try_charge()：沿祖先计费、失败回滚
```

### 一、先准备新凭据，再发布

`prepare_creds()` 以当前凭据为基础准备可修改的新对象，并取得必要的附属对象引用，见 [`kernel/cred.c:206-250`](../../linux/kernel/cred.c#L206-L250)。调用者只修改尚未发布的候选对象；成功后交给 `commit_creds()`，失败则 `abort_creds()`。

`commit_creds()` 检查当前没有处于主观凭据覆盖状态，处理 dumpability、父死亡信号等变化，再以 RCU 发布两个凭据指针并放下旧引用，见 [`kernel/cred.c:392-472`](../../linux/kernel/cred.c#L392-L472)。安全属性与身份发布顺序具有并发意义，不是两次普通指针赋值的语法替换。

```mermaid
flowchart LR
    A[当前任务引用旧 cred] --> B[prepare_creds 准备候选]
    B --> C[身份与能力检查 / LSM 检查]
    C -->|失败| D[abort_creds 放下候选]
    C -->|成功| E[commit_creds 发布新引用]
    E --> F[旧凭据按引用与 RCU 条件释放]
```

*图 2-4：凭据更新采用新对象发布，而不是原地改写共享身份*

### 二、`setuid()` 的参数需要命名空间转换和权限检查

`__sys_setuid()` 先调用 `make_kuid()` 转换输入并拒绝无效映射；是否可以改变实际与保存 UID，取决于命名空间内的 `CAP_SETUID` 及既有身份。之后还有 `security_task_fix_setuid()`、用户计数更新与提交，见 [`kernel/sys.c:651-700`](../../linux/kernel/sys.c#L651-L700)。

“有效 UID 为 0 就自动通过全部检查”不符合该模型。能力集合、目标命名空间、LSM 和具体操作规则都会参与判断。本配置 `CONFIG_MULTIUSER=y`、`CONFIG_USER_NS=y`、`CONFIG_SECURITY=y`。

内核凭据按任务持有；POSIX 线程库为维持进程级身份一致性，会在用户态包装中协调其他线程。因此绕过 libc 直接做身份系统调用时，不能把单任务内核行为等同于线程库向应用保证的整体语义。

### 三、创建时不一定分配全新凭据

`copy_creds()` 对满足条件的同线程组创建可以共享凭据并增加引用；有线程密钥等情况时可能需要准备新对象。`CLONE_NEWUSER` 则在候选凭据中创建用户命名空间，见 [`kernel/cred.c:290-352`](../../linux/kernel/cred.c#L290-L352)。所以既不能说“所有线程永远共享同一指针”，也不能说“每次 fork 都把所有身份附属对象深复制”。

exec 同样使用候选凭据，完成格式与安全交接后提交；setuid 位、文件能力和安全策略的完整变换还要结合 [`security/commoncap.c`](../../linux/security/commoncap.c)，不能只看 `euid` 一个字段判断执行后的权限。

### 四、创建限制是多层独立检查

| 层次 | 主要对象 / 入口 | 失败含义 |
|---|---|---|
| 用户数量限制 | `RLIMIT_NPROC`、`ucounts` | 创建使相关真实用户计数超过限制，存在特权等例外 |
| 全局任务数量 | `nr_threads`、`max_threads` | 系统任务数到达上限；并发检查不是严格串行配额 |
| PID 编号分配 | 各层 `pid_namespace.idr`、`pid_max` | 某层无法取得编号，或目标命名空间已停止分配 |
| pids 控制器 | 目标及祖先 cgroup 的计数和限制 | 任一受约束层级计费超限，创建被拒绝 |
| 内存等其他资源 | 任务、内核栈、mm、页表等分配 | 资源不足，不能统一归因为 PID 耗尽 |

用户与全局任务检查见 [`kernel/fork.c:2060-2083`](../../linux/kernel/fork.c#L2060-L2083)，PID 失败处理见 [`kernel/pid.c:163-310`](../../linux/kernel/pid.c#L163-L310)，pids 计费见 [`kernel/cgroup/pids.c:157-198`](../../linux/kernel/cgroup/pids.c#L157-L198)。

`RLIMIT_NPROC` 在 Linux 中约束的是包含线程在内的任务创建，不只是有独立 TGID 的进程数。代码通过 `UCOUNT_RLIMIT_NPROC` 计费，回收时减计数，见 [`kernel/cred.c:307-352`](../../linux/kernel/cred.c#L307-L352)、[`kernel/exit.c:258-264`](../../linux/kernel/exit.c#L258-L264)。

### 五、pids 控制器沿层级计费并回滚

`pids_can_fork()` 选取目标 cgroup 状态，`pids_try_charge()` 沿父级增加计数；任一层超过限制则撤销本次已经增加的计数并返回 `-EAGAIN`，见 [`kernel/cgroup/pids.c:166-198`](../../linux/kernel/cgroup/pids.c#L166-L198)、[`kernel/cgroup/pids.c:278-299`](../../linux/kernel/cgroup/pids.c#L278-L299)。依赖 `CONFIG_CGROUPS=y`、`CONFIG_CGROUP_PIDS=y`。

创建后续失败还需通过 `cancel_fork` 回滚。正常任务的计数在控制器 release 回调中扣除，见 [`kernel/cgroup/pids.c:291-304`](../../linux/kernel/cgroup/pids.c#L291-L304)；不能认为看到 `sched_process_exit` 就保证 `pids.current` 已立刻下降。

## 关键细节 / 设计考量

### `pids.current` 可以大于 `pids.max`

降低现有上限或组织性迁入任务可以形成这种状态；控制器限制新创建，不通过任意强杀来修复既有超限。层级限制与组织操作说明见 [`Documentation/admin-guide/cgroup-v2.rst:2403-2422`](../../linux/Documentation/admin-guide/cgroup-v2.rst#L2403-L2422)。

### 失败 errno 不能唯一定位限制来源

多个创建限制都可能返回 `EAGAIN`。应结合 syscall 错误、`/proc/<pid>/limits`、pids 事件计数、祖先限额和全局任务情况诊断，不应看到一次 `EAGAIN` 就修改 `pid_max`。用户命名空间、seccomp 或 LSM 拒绝也不等于任务数量耗尽。

### `pids.events` 的层级语义依赖挂载选项

本版本默认 cgroup v2 与 `pids_localevents`、旧层级在事件归属与传播上有区别。计数增加和选择输出项的代码分别见 [`kernel/cgroup/pids.c:243-271`](../../linux/kernel/cgroup/pids.c#L243-L271)、[`kernel/cgroup/pids.c:366-393`](../../linux/kernel/cgroup/pids.c#L366-L393)。不能把所有环境中的 `max` 都解释为“这个叶子里发起 fork 失败的次数”。

## 可观测性

| 入口 | 看什么、怎么解读 | 源码与配置 |
|---|---|---|
| `/proc/<pid>/status` 的 `Uid` / `Gid` | 依次为实际、有效、保存、文件系统 ID，按 proc 输出用户视图转换 | [`fs/proc/array.c:186-196`](../../linux/fs/proc/array.c#L186-L196)，`CONFIG_PROC_FS=y`、`CONFIG_MULTIUSER=y` |
| 同一 status 的 `CapInh` / `CapPrm` / `CapEff` / `CapBnd` / `CapAmb` | 凭据能力集合；不是一个“是否 root”布尔值 | [`fs/proc/array.c:308-335`](../../linux/fs/proc/array.c#L308-L335)，`CONFIG_PROC_FS=y` |
| `/proc/<pid>/limits` | `signal->rlim[]` 中软 / 硬限额；不含 cgroup 祖先施加的全部限制 | [`fs/proc/base.c:628-669`](../../linux/fs/proc/base.c#L628-L669)，`CONFIG_PROC_FS=y` |
| `/proc/sys/kernel/threads-max` | 全局 `max_threads` 上限，不是当前线程数 | [`kernel/fork.c:3258-3272`](../../linux/kernel/fork.c#L3258-L3272)，`CONFIG_SYSCTL=y` |
| cgroup v2 的 `pids.current`、`pids.max`、`pids.peak`、`pids.events`、`pids.events.local` | 当前计数、上限、水位与拒绝相关事件；先确认目标路径和祖先，不读取不存在的根组接口 | [`kernel/cgroup/pids.c:356-424`](../../linux/kernel/cgroup/pids.c#L356-L424)，`CONFIG_CGROUP_PIDS=y`，还须挂载并在相应子树启用控制器 |
| `syscalls:sys_exit_clone` / `sys_exit_clone3` | 创建错误返回，与成功 fork 事件互补 | [`kernel/fork.c`](../../linux/kernel/fork.c)、[`kernel/trace/trace_syscalls.c`](../../linux/kernel/trace/trace_syscalls.c)，`CONFIG_FTRACE_SYSCALLS=y` |

## 动手实验：只读检查创建限制

先在 dev 查看当前 shell 的身份与用户限额，不调整系统配置：

```bash
grep -E '^(Uid|Gid|CapInh|CapPrm|CapEff|CapBnd|CapAmb):' /proc/$$/status
grep -E 'Max processes|Max open files' /proc/$$/limits
printf 'threads-max='; sysctl -n kernel.threads-max
printf 'pid_max='; sysctl -n kernel.pid_max
printf '\ncgroup membership:\n'
grep '^0::' /proc/$$/cgroup
```

若为 cgroup v2，用 `findmnt -t cgroup2` 确认挂载及命名空间视图，再定位相应目录，读取 pids 文件并向祖先逐级检查。不要假定 `/proc/$$/cgroup` 的路径总可直接拼接到 `/sys/fs/cgroup`，也不要为了实验在宿主组设置极低限制或运行 fork bomb。以上操作未在目标内核上实测。

## 小结

- 凭据保存多组身份与能力，不能简化为一个有效 UID。
- 凭据更新先构造候选，再以 RCU 发布；共享对象不能任意原地修改。
- 用户命名空间改变身份和能力的解释范围，不赋予宿主无限权限。
- 用户限额、全局任务数、PID 编号与 cgroup 限制互相独立。
- 创建错误需要多入口关联诊断，pids 计数和事件也有生命周期及层级语义。

## 参考

- [`Documentation/security/credentials.rst`](../../linux/Documentation/security/credentials.rst)。
- [`Documentation/admin-guide/cgroup-v2.rst`](../../linux/Documentation/admin-guide/cgroup-v2.rst)。
- [`Documentation/admin-guide/cgroup-v1/pids.rst`](../../linux/Documentation/admin-guide/cgroup-v1/pids.rst)。
