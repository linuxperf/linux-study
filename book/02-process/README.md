# 第 2 章 进程管理

本章以 Linux v6.18.60、x86_64 和仓库 [`linux/.config`](../../linux/.config) 为基线，分析任务从创建、执行到退出回收的完整生命周期。重点不是调度算法，而是解释内核如何表示执行主体、如何组织共享资源，以及身份、信号和权限如何随生命周期变化。

Linux 用 `task_struct` 表示每个可调度任务；用户空间的一个多线程进程对应一个线程组，而不是一个装着全部线程的单独任务对象。阅读本章时，应始终区分 **任务对象、共享资源对象、数字标识和对象引用**。

## 阅读路线

- **建立模型**：2.1～2.3，先分清任务、线程组、状态和命名空间中的 PID。
- **创建与执行**：2.4～2.6，沿 `kernel_clone()`、资源继承、exec 装载主线阅读。
- **结束与通知**：2.7～2.9，把退出、父进程等待、信号排队和用户态处理串起来。
- **扩展执行环境**：2.10～2.12，讨论内核线程、命名空间、凭据与创建限制。

与其他章节的边界：首次入队与唤醒细节见 [3.4](../03-scheduler/04-wakeup.md)，上下文切换见 [3.2](../03-scheduler/02-schedule.md)，地址空间与 COW 深入分析见 [4.7](../04-memory/07-address-space.md)、[4.9](../04-memory/09-cow-rmap.md)，文件描述符模型见 [9.6](../09-vfs/06-open.md)。

## 本章配置

下表均为当前 [`linux/.config`](../../linux/.config) 的值。实验运行前仍须核对 dev 虚拟机实际启动的内核；源码基线与虚拟机正在运行的发行版内核不一定相同。

| 配置项 | 取值 | 对本章的影响 |
|---|---|---|
| `CONFIG_X86_64` / `CONFIG_MMU` / `CONFIG_SMP` | y / y / y | 64 位、多任务地址空间与多核并发 |
| `CONFIG_THREAD_INFO_IN_TASK` / `CONFIG_VMAP_STACK` | y / y | `thread_info` 内嵌任务对象，任务内核栈采用虚拟映射 |
| `CONFIG_PAGE_SIZE_4KB` / `CONFIG_KASAN` | y / n | 本配置普通任务内核栈为 16 KB，不含独立的中断等栈 |
| `CONFIG_BINFMT_ELF` / `CONFIG_BINFMT_SCRIPT` / `CONFIG_COREDUMP` | y / y / y | ELF、脚本与 core dump 支持 |
| `CONFIG_FUTEX` / `CONFIG_SIGNALFD` | y / y | 线程退出通知与基于 fd 的信号消费接口 |
| `CONFIG_NAMESPACES` / `CONFIG_USER_NS` / `CONFIG_PID_NS` | y / y / y | 命名空间与 UID、PID 分层视图 |
| `CONFIG_UTS_NS` / `CONFIG_IPC_NS` / `CONFIG_NET_NS` / `CONFIG_TIME_NS` | y / y / y / y | 相应命名空间可用，创建仍受权限等检查 |
| `CONFIG_CGROUPS` / `CONFIG_CGROUP_PIDS` | y / y | cgroup 管理与任务数量限制 |
| `CONFIG_MULTIUSER` / `CONFIG_SECURITY` / `CONFIG_KEYS` | y / y / y | 多用户身份、LSM 钩子与密钥引用 |
| `CONFIG_PROC_FS` / `CONFIG_PROC_CHILDREN` | y / y | 任务状态、线程目录及子任务列表 |
| `CONFIG_EVENT_TRACING` / `CONFIG_FTRACE_SYSCALLS` | y / y | 生命周期、信号及系统调用事件 |
| `CONFIG_KPROBES` / `CONFIG_FUNCTION_TRACER` / `CONFIG_STACKTRACE` | y / y / y | 动态探针、函数跟踪与内核栈观测；仍受目标函数和权限限制 |

内核栈大小的计算依据是 [`arch/x86/include/asm/page_64_types.h:7-16`](../../linux/arch/x86/include/asm/page_64_types.h#L7-L16)，不是对所有架构与配置的固定承诺。

## 小节索引

| 小节 | 重点 |
|---|---|
| [2.1 进程管理全景：任务、资源与生命周期](01-overview.md) | 统一任务模型与阅读地图 |
| [2.2 任务描述符、内核栈与任务状态](02-task.md) | `task_struct`、`current`、状态与引用生命周期 |
| [2.3 PID、线程组与进程标识](03-pid.md) | TID、TGID、PGID、SID、`struct pid` 与 pidfd |
| [2.4 任务创建：fork、clone 与首次运行](04-fork.md) | 参数约束、发布边界、首次返回路径 |
| [2.5 资源继承：共享、复制与写时复制](05-resources.md) | `mm`、文件表、信号表、vfork 与退出通知 |
| [2.6 程序执行：exec 框架与 ELF 装载](06-exec.md) | 格式处理器、不可返回点、解释器和用户入口 |
| [2.7 任务退出、僵尸与等待回收](07-exit-wait.md) | 单线程 / 线程组退出、父进程等待、重托管 |
| [2.8 信号生成、排队与线程选择](08-signal.md) | 私有 / 共享待决集合、合并与接收线程 |
| [2.9 x86_64 信号投递与系统调用重启](09-signal-frame.md) | 信号帧、`rt_sigreturn()` 与重启协议 |
| [2.10 内核线程：创建、运行与停止](10-kthread.md) | `kthreadd()`、协作停止与对象所有权 |
| [2.11 命名空间：资源视图与身份边界](11-namespaces.md) | `nsproxy`、clone / unshare / setns、命名空间 PID 1 |
| [2.12 进程凭据、权限与创建限制](12-credentials-limits.md) | `cred` 发布、能力与多层数量限制 |

各非概述小节均包含可观测性入口、源码依据和配置说明。正文为已撰写并静态核验的初稿；运行结果不作为已实测结论。

## 实验与验证边界

- [进程生命周期程序](examples/lifecycle.c)：供 2.6、2.7 使用，观察 fork、exec、僵尸保留与回收。
- [待决信号程序](examples/pending-signals.c)：供 2.8、2.9 使用，比较标准信号合并与实时信号排队，并观察处理器返回。
- 所有用户程序编译产物写入 `/tmp`，不得写入或修改 `linux/`。
- 跟踪实验优先使用 `perf record`，避免重置其他使用者的全局 tracefs 状态；记录可能包含其他进程的命令名和路径，应限制采集时长与文件权限。
- 已静态检查本章本地链接目标与行号范围、逐字比对 C 源码摘录，并对 shell 代码块执行 `bash -n`；`lifecycle.c` 已通过宿主 Clang 的语法与警告检查。
- 当前宿主为 macOS，dev 虚拟机尚未创建；两份示例均未在目标 Linux 环境中编译运行，`pending-signals.c` 尚未获得 Linux 编译器验证。未验证 Mermaid 在目标阅读器中的显示效果。

## 待完善

- [ ] 在启动目标 v6.18.60 内核的 dev 虚拟机中执行两份示例，补充真实输出、工具版本和配置记录。
- [ ] 实测 pidfd、非组长线程 exec、子收割者与命名空间 PID 1 退出的边界场景；当前正文给出源码结论，不编造运行记录。
- [ ] 在目标渲染器核验图 2-1～图 2-4。
- [ ] 为 core dump、作业控制和 ptrace 补充专门的扩展分析；本章仅解释它们在主生命周期与信号路径中的接口位置。
