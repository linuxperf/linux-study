# 2.11 命名空间：资源视图与身份边界

> **本节源码**：[`include/linux/nsproxy.h`](../../linux/include/linux/nsproxy.h)、[`kernel/nsproxy.c`](../../linux/kernel/nsproxy.c)、[`kernel/pid_namespace.c`](../../linux/kernel/pid_namespace.c)、[`kernel/user_namespace.c`](../../linux/kernel/user_namespace.c)、[`kernel/fork.c`](../../linux/kernel/fork.c)、[`fs/proc/namespaces.c`](../../linux/fs/proc/namespaces.c)
> **关键函数**：`copy_namespaces()`、`create_new_namespaces()`、`ksys_unshare()`、`switch_task_namespaces()`、`pidns_install()`、`zap_pid_ns_processes()`
> **前置知识**：2.3 分层 PID；2.5 共享资源；2.7 父子回收

## 概述

命名空间（Namespace）把系统资源的可见集合或编号解释分成多个视图，让任务可以在不同的挂载、网络、PID 等环境中执行。它并不复制整个内核，也不会自动限制 CPU 或内存用量。不同种类的命名空间还具有不同的加入、继承和权限约束。Linux 用 `nsproxy` 聚合多种视图引用，但用户命名空间和任务当前活动 PID 命名空间并不直接存放在其中。

## 核心数据结构

[`include/linux/nsproxy.h:32-43`](../../linux/include/linux/nsproxy.h#L32-L43)（v6.18.60）

```c
struct nsproxy {
	refcount_t count;
	struct uts_namespace *uts_ns;
	struct ipc_namespace *ipc_ns;
	struct mnt_namespace *mnt_ns;
	struct pid_namespace *pid_ns_for_children;
	struct net 	     *net_ns;
	struct time_namespace *time_ns;
	struct time_namespace *time_ns_for_children;
	struct cgroup_namespace *cgroup_ns;
};
```

- `count` 管理聚合对象引用；每种命名空间对象还有自己的生命周期。
- `mnt_ns`、`uts_ns`、`ipc_ns`、`net_ns` 分别关联挂载、主机名、IPC 与网络视图。
- `pid_ns_for_children` 指向后续子任务将使用的 PID 命名空间，不是当前任务的活动 PID 命名空间。
- `time_ns` 与 `time_ns_for_children` 区分当前时间视图和后续交接目标；时间子系统细节见 [8.12](../08-time/12-vdso-timens.md)。
- `cgroup_ns` 改变 cgroup 视图根，不等于自动把任务迁入一个新资源控制组；相应视图处理见 [`kernel/cgroup/namespace.c`](../../linux/kernel/cgroup/namespace.c)。

用户命名空间位于 `cred.user_ns`，见 [`include/linux/cred.h:140-143`](../../linux/include/linux/cred.h#L140-L143)。活动 PID 命名空间由任务 PID 对象取得，见 [`kernel/pid.c:526-529`](../../linux/kernel/pid.c#L526-L529)。

## 主流程分析

```text
创建新任务                            kernel/fork.c
copy_process()
├── copy_creds()                        kernel/cred.c
│   └── CLONE_NEWUSER 时 create_user_ns()  kernel/user_namespace.c
└── copy_namespaces()                   kernel/nsproxy.c
    ├── 无需改变：get_nsproxy()
    └── create_new_namespaces()
        ├── copy_mnt_ns() / copy_utsname() / copy_ipcs()
        ├── copy_pid_ns() / copy_cgroup_ns() / copy_net_ns()
        └── copy_time_ns()

当前任务解除共享
unshare() → ksys_unshare()             kernel/fork.c
├── 准备新的 fs / files / cred
├── unshare_nsproxy_namespaces()        kernel/nsproxy.c
└── 发布新 nsproxy / cred，放下旧引用

加入现有视图
setns()                               kernel/nsproxy.c
├── 准备 nsset、验证 fd 与类型
├── 相应命名空间 install 回调
└── 提交新的资源视图
```

### 一、没有新视图请求时复用引用

`copy_namespaces()` 在没有相关 `CLONE_NEW*` 请求、时间命名空间关系也允许时，直接增加旧 `nsproxy` 引用。需要改变时才创建新聚合对象，见 [`kernel/nsproxy.c:147-192`](../../linux/kernel/nsproxy.c#L147-L192)。

新聚合对象不意味着每一种命名空间都全新创建：各 `copy_*()` 根据标志决定复制视图还是取旧对象引用，见 [`kernel/nsproxy.c:67-125`](../../linux/kernel/nsproxy.c#L67-L125)。这和任务创建时按资源类别选择共享是一致的设计方式。

### 二、clone、unshare 与 setns 的主体不同

| 接口 | 谁发生变化 | 是否必然创建任务 | 典型用途 |
|---|---|---|---|
| clone / clone3 加 `CLONE_NEW*` | 新任务 | 是 | 创建时进入新资源视图 |
| `unshare()` | 当前调用任务的资源关系，部分视图仅影响后续子任务 | 否 | 解除共享并建立新视图 |
| `setns()` | 当前调用任务或其后续子任务使用的指定视图 | 否 | 加入已存在的命名空间 |

`ksys_unshare()` 不能直接照搬构造未运行子任务的 `copy_*()`：它必须安全更换当前活动任务的资源，见 [`kernel/fork.c:3076-3206`](../../linux/kernel/fork.c#L3076-L3206)。其中还会自动扩展相关标志并检查单线程条件，不是把当前多线程进程随意拆成多个隔离进程。

`setns()` 有按类型的安装检查和提交过程，见 [`kernel/nsproxy.c`](../../linux/kernel/nsproxy.c)。它不保证不同类型有统一的迁移语义，也不等于一个“进入容器”的完整操作。

### 三、PID 命名空间变化通常面向后续子任务

调用 `unshare(CLONE_NEWPID)` 后，当前任务并不会突然变成新空间里的 PID 1。新空间引用写入 `pid_ns_for_children`，后续创建的第一个任务才取得该层 PID 1。

`pidns_install()` 同样只替换 `pid_ns_for_children`，并限制只能加入当前或后代 PID 命名空间，见 [`kernel/pid_namespace.c:402-425`](../../linux/kernel/pid_namespace.c#L402-L425)。已存在任务的活动 PID 路径不能被任意迁移到另一个不相交命名空间。

```mermaid
flowchart LR
    A[当前任务：活动 PID 空间 A] -->|unshare CLONE_NEWPID| B[当前任务仍属于 A]
    B --> C[pid_ns_for_children 指向新空间 B]
    C -->|fork| D[子任务：B 中 PID 1]
    D --> E[子任务同时拥有 A 中编号]
```

*图 2-3：PID 命名空间的当前身份与后续子任务视图*

### 四、用户命名空间决定权限解释的范围

`copy_creds()` 先处理 `CLONE_NEWUSER`，随后 `copy_namespaces()` 才用候选任务的 `user_ns` 检查新视图权限，见 [`kernel/cred.c:290-335`](../../linux/kernel/cred.c#L290-L335)、[`kernel/nsproxy.c:147-164`](../../linux/kernel/nsproxy.c#L147-L164)。因此多个命名空间一起创建时，调用顺序具有权限含义。

新用户命名空间中的 UID 0 不等于宿主 UID 0；它的能力（Capability）也受所属命名空间约束。UID / GID 映射、创建数量限制与安全策略可能拒绝创建，`CONFIG_USER_NS=y` 只是必要的编译支持。凭据模型详见 2.12。

### 五、命名空间 PID 1 有额外回收责任

新空间的 PID 1 成为 `child_reaper`，创建路径还会设置相应不可随意终止标记，见 [`kernel/fork.c:2388-2391`](../../linux/kernel/fork.c#L2388-L2391)。这不意味着它能忽略所有来源的所有信号；祖先命名空间与强制信号处理有专门规则，见 [`kernel/signal.c:1226-1263`](../../linux/kernel/signal.c#L1226-L1263)。

当该命名空间收割者线程组结束、需要清空空间时，`zap_pid_ns_processes()` 禁止新 PID 分配，向剩余任务发送终止请求并等待清理，见 [`kernel/pid_namespace.c:192-280`](../../linux/kernel/pid_namespace.c#L192-L280)。容器中的主程序退出与普通子进程退出因此具有不同系统影响。

## 关键细节 / 设计考量

### `/proc` 挂载本身也有视图

新建 PID 命名空间而继续沿用外层 procfs，可能仍看到外层任务列表。要让工具按内层视图解释 `/proc`，通常还需要在独立挂载命名空间中重新挂载 procfs。procfs 上下文与 PID 空间关联见 [`fs/proc/root.c`](../../linux/fs/proc/root.c)。

### 命名空间不是资源配额

PID 空间隔离编号；pids cgroup 限制任务数量；用户命名空间改变权限解释；内存控制器限制内存。这些机制可以组合，但一个不能代替另一个。命名空间对象也可能被打开的 fd 或挂载引用保留，不与最后一个任务的离开时刻严格同步销毁。

## 可观测性

| 入口 | 看什么、怎么解读 | 源码与配置 |
|---|---|---|
| `/proc/<pid>/ns/*` | namespace 链接的类型与对象编号；同类型编号相同可比较对象身份，不表示任务其他资源也相同 | [`fs/proc/namespaces.c:15-41`](../../linux/fs/proc/namespaces.c#L15-L41)，`CONFIG_PROC_FS=y`；类型依赖对应配置 |
| `/proc/<pid>/ns/pid`、`pid_for_children` | 当前 PID 视图与后续子任务视图；刚 unshare 后后者可能尚无可显示的收割者状态 | [`kernel/pid_namespace.c:349-400`](../../linux/kernel/pid_namespace.c#L349-L400)、[`kernel/pid_namespace.c:448-468`](../../linux/kernel/pid_namespace.c#L448-L468)，`CONFIG_PID_NS=y` |
| `/proc/<pid>/status` 的 `NSpid` / `NStgid` | 从 procfs 视图起的层次编号，不是任意观察者都看到全部祖先 | [`fs/proc/array.c:205-222`](../../linux/fs/proc/array.c#L205-L222)，`CONFIG_PROC_FS=y`、`CONFIG_PID_NS=y` |
| `/proc/<pid>/uid_map`、`gid_map` | 用户命名空间内外 ID 区间映射，不要用用户名文本代替映射关系 | [`fs/proc/base.c`](../../linux/fs/proc/base.c)、[`kernel/user_namespace.c`](../../linux/kernel/user_namespace.c)，`CONFIG_PROC_FS=y`、`CONFIG_USER_NS=y` |
| `/proc/<pid>/cgroup` | 任务控制组归属及经视图转换的路径；不是全部资源限额的快照 | [`kernel/cgroup/cgroup.c`](../../linux/kernel/cgroup/cgroup.c)，`CONFIG_PROC_FS=y`、`CONFIG_CGROUPS=y` |

本配置 `CONFIG_UTS_NS`、`CONFIG_IPC_NS`、`CONFIG_NET_NS`、`CONFIG_TIME_NS`、`CONFIG_CGROUPS` 均为 y，对应 `ns` 条目可编译；实际访问仍受权限约束。

## 动手实验：一次性的用户、PID 与挂载视图

以下在 dev 中使用 util-linux 的 `unshare`。不修改宿主挂载，也不改写 `linux/`；若发行版安全策略禁止非特权用户命名空间，命令可能失败，应记录实际错误，不调整宿主全局安全配置来强行通过。

```bash
unshare --user --map-root-user --mount --pid --fork --mount-proc sh -c '
    printf "inside pid=%s\n" "$$"
    grep -E "^(Pid|PPid|NSpid|NStgid):" /proc/self/status
    readlink /proc/self/ns/pid
    readlink /proc/self/ns/user
    ps -eo pid,ppid,comm
'
```

注意 `/proc/self` 指向当时读取它的进程：这里的 grep、readlink 是各自的子进程，不必等于 shell 的 PID。内层 procfs 的 `NSpid` 可以只有一列；要看到外层编号，应从外层 procfs 查看对应任务，不能把单列输出判为实验失败。本实验尚未实测。

## 小结

- 命名空间隔离资源视图，不自动提供数量配额或完整容器隔离。
- `nsproxy` 聚合多种引用，但用户空间与活动 PID 空间有不同存放位置。
- clone 面向新任务，unshare 面向解除共享，setns 面向加入已有视图。
- PID 空间切换通常影响后续子任务，不会即时更改当前活动身份。
- PID 1 承担收割者责任，procfs 挂载也必须与期望的 PID 视图匹配。

## 参考

- [`Documentation/admin-guide/namespaces/index.rst`](../../linux/Documentation/admin-guide/namespaces/index.rst)。
- [`Documentation/admin-guide/namespaces/resource-control.rst`](../../linux/Documentation/admin-guide/namespaces/resource-control.rst)。
- [8.12 vDSO 与时间命名空间](../08-time/12-vdso-timens.md)。
