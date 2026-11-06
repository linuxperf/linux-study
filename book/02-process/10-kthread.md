# 2.10 内核线程：创建、运行与停止

> **本节源码**：[`kernel/kthread.c`](../../linux/kernel/kthread.c)、[`include/linux/kthread.h`](../../linux/include/linux/kthread.h)、[`kernel/fork.c`](../../linux/kernel/fork.c)、[`arch/x86/kernel/process.c`](../../linux/arch/x86/kernel/process.c)
> **关键函数**：`kthread_create_on_node()`、`kthreadd()`、`create_kthread()`、`kthread()`、`kthread_should_stop()`、`kthread_stop()`、`kthread_use_mm()`
> **前置知识**：2.4 创建与首次入口；2.5 地址空间共享；2.7 退出协议

## 概述

内核中的后台工作需要可调度、可睡眠的执行主体，但不需要从用户程序入口开始运行。内核线程（Kernel Thread）复用任务创建与调度机制，通过 `PF_KTHREAD`、内核函数入口和专用生命周期协议表达这种用途。通用 kthread 接口把创建请求交给 `kthreadd()`，避免随意继承调用者的执行环境。停止机制采用协作请求和退出确认，不是强制在任意位置撤销任务。

## 核心数据结构

`task_struct.worker_private` 可关联 kthread 私有状态，`struct kthread` 与创建请求定义见 [`kernel/kthread.c:41-86`](../../linux/kernel/kthread.c#L41-L86)。本节关注以下字段关系：

| 对象 / 字段 | 作用 |
|---|---|
| `kthread_create_info` | 创建者提交的入口、参数、NUMA 节点、结果和完成通知 |
| `struct kthread.flags` | 停止、park 等控制请求 |
| `threadfn` / `data` | 内核线程的主体函数与参数 |
| `exited` / `result` | 停止者等待的完成量与退出结果 |
| `task_struct.flags` 中的 `PF_KTHREAD` | 内核线程属性，不等价于“当前正在内核态” |
| `task_struct.mm` / `active_mm` | 普通 kthread 不拥有用户 mm，可借用地址空间上下文 |

任务字段见 [`include/linux/sched.h:957-958`](../../linux/include/linux/sched.h#L957-L958)、[`include/linux/sched.h:1107-1108`](../../linux/include/linux/sched.h#L1107-L1108)。

## 主流程分析

```text
调用者：kthread_create()                include/linux/kthread.h
└── kthread_create_on_node()            kernel/kthread.c
    └── __kthread_create_on_node()
        ├── 入队 kthread_create_info
        ├── wake_up_process(kthreadd_task)
        └── 等待创建完成

kthreadd()                             kernel/kthread.c
└── create_kthread()
    └── kernel_thread(kthread, create, ...)  kernel/fork.c
        └── kernel_clone() → copy_process() → copy_thread()

新任务：ret_from_fork()                 arch/x86/kernel/process.c
└── kthread()                          kernel/kthread.c
    ├── 发布创建结果、等待启动
    ├── 处理亲和性和 park 状态
    ├── threadfn(data)
    └── kthread_exit()

停止者：kthread_stop()                 kernel/kthread.c
├── 设置 KTHREAD_SHOULD_STOP、唤醒
└── 等待 exited，读取 result
```

### 一、为什么通过 `kthreadd()` 创建

`__kthread_create_on_node()` 将请求加入全局创建队列，唤醒 `kthreadd_task` 并等待完成，见 [`kernel/kthread.c:481-524`](../../linux/kernel/kthread.c#L481-L524)。`kthreadd()` 先设置干净的信号和亲和性等环境，再循环处理请求，见 [`kernel/kthread.c:786-823`](../../linux/kernel/kthread.c#L786-L823)。

这种间接创建让普通调用者无需直接把自己的用户 mm、凭据或信号环境变成后台线程的继承起点。不能简单把 kthread 描述为“任意用户进程执行 clone 后留在内核”。

### 二、创建成功与主体函数启动分离

`kthread()` 先设置自身状态，向创建者发布任务指针并完成通知，然后调度等待启动，见 [`kernel/kthread.c:380-435`](../../linux/kernel/kthread.c#L380-L435)。`kthread_create()` 返回不意味着 `threadfn()` 已运行；接口使用的“stopped”也不是作业控制的 `TASK_STOPPED`。

`kthread_run()` 是创建后立即唤醒的便利宏：

[`include/linux/kthread.h:69-76`](../../linux/include/linux/kthread.h#L69-L76)（v6.18.60）

```c
#define kthread_run(threadfn, data, namefmt, ...)			   \
({									   \
	struct task_struct *__k						   \
		= kthread_create(threadfn, data, namefmt, ## __VA_ARGS__); \
	if (!IS_ERR(__k))						   \
		wake_up_process(__k);					   \
	__k;								   \
})
```

如果需要绑定 CPU 或先完成共享数据初始化，应使用创建与启动分离的接口，且先检查错误指针。CPU 绑定相关同步见 [`kernel/kthread.c:571-614`](../../linux/kernel/kthread.c#L571-L614)。

### 三、停止是一种必须配合的协议

`kthread_stop()` 取得任务引用，设置 `KTHREAD_SHOULD_STOP`，解除 park、请求通知并唤醒目标，随后等待 `exited`，见 [`kernel/kthread.c:746-767`](../../linux/kernel/kthread.c#L746-L767)。主体函数应在合适的循环位置检查 `kthread_should_stop()`；该检查本身读取控制位，见 [`kernel/kthread.c:142-155`](../../linux/kernel/kthread.c#L142-L155)。

若目标无限计算而不检查，或者卡在不能被本协议终止的等待中，停止者可能长期阻塞。可靠的工作循环必须把“有工作”和“请求停止”都纳入等待条件；不能只在得到工作后才检查停止。

锁的所有权同样重要：停止者如果持有目标退出所必需的锁，再调用同步 `kthread_stop()`，会形成死锁。共享数据也必须一直有效到目标退出确认之后。

### 四、park 和 stop 不是同义词

park 保留线程，让它进入可再次解除的停放状态；stop 要求退出并等待完成。`kthread_park()`、`kthread_parkme()` 与 `kthread_unpark()` 的协作逻辑见 [`kernel/kthread.c`](../../linux/kernel/kthread.c)。CPU 热插拔等场景需要的“暂时不运行”不能直接替换为线程销毁。

## 关键细节 / 设计考量

### 普通 kthread 没有用户 mm，不代表没有地址空间上下文

从 `kthreadd()` 创建的普通线程通常 `mm=NULL`，调度时可通过 `active_mm` 借用上下文；详见 [3.2](../03-scheduler/02-schedule.md)。需要显式访问某个用户地址空间的特殊场景可使用 `kthread_use_mm()`，随后必须配对 `kthread_unuse_mm()`，见 [`kernel/kthread.c:1569-1644`](../../linux/kernel/kthread.c#L1569-L1644)。

这是受约束的地址空间借用，不意味着任意裸用户指针自动安全，也不是内核线程自动取得用户进程全部身份。

### 内核执行状态不等于内核线程属性

PID 1 最初执行 `kernel_init()`，但本版本以 `user_mode_thread()` 创建；普通 `kernel_thread()` 则设置 `.kthread=1`，见 [`kernel/fork.c:2663-2691`](../../linux/kernel/fork.c#L2663-L2691)。`kernel_execve()` 拒绝 `PF_KTHREAD`，见 [`fs/exec.c:1892-1904`](../../linux/fs/exec.c#L1892-L1904)。不能把历史注释中的笼统“kernel thread”措辞当成今天的实际标志规则。

### 不要对已失效的任务指针请求停止

若主体允许自行退出，创建者必须明确任务对象的引用所有权，不能等任意长时间后拿缓存裸指针调用 `kthread_stop()`。本版本提供 `kthread_stop_put()` 处理调用者额外持有引用的情形，见 [`kernel/kthread.c:770-784`](../../linux/kernel/kthread.c#L770-L784)。函数内部取得引用不能挽救调用前就已失效的指针。

## 可观测性

| 入口 | 看什么、怎么解读 | 源码与配置 |
|---|---|---|
| `/proc/<pid>/status` 的 `Kthread` | 直接对应 `PF_KTHREAD`；不靠命令名方括号猜测 | [`fs/proc/array.c:222`](../../linux/fs/proc/array.c#L222)，`CONFIG_PROC_FS=y` |
| `sched:sched_kthread_stop` / `sched:sched_kthread_stop_ret` | 停止目标、同步停止的返回值；事件间隔包括等待退出的时间 | [`include/trace/events/sched.h:16-53`](../../linux/include/trace/events/sched.h#L16-L53)、[`kernel/kthread.c:751-765`](../../linux/kernel/kthread.c#L751-L765)，`CONFIG_EVENT_TRACING=y` |
| `sched:sched_process_fork` / `sched:sched_process_exit` | kthread 同样走任务生命周期，可与用户线程共用事件 | [`include/trace/events/sched.h:335-419`](../../linux/include/trace/events/sched.h#L335-L419)，`CONFIG_EVENT_TRACING=y` |
| `/proc/<pid>/stack`、`wchan` | 停止为何等待、主体睡在哪；受权限和瞬时状态影响 | [`fs/proc/base.c:412-490`](../../linux/fs/proc/base.c#L412-L490)，`CONFIG_PROC_FS=y`、`CONFIG_STACKTRACE=y` |

不必为了验证创建协议就在工作虚拟机中加载实验模块。优先观察现有后台线程；完整模块实验需要额外处理模块卸载、线程引用与停止失败风险，本章不把未验证模块当作可直接投产示例。

## 小结

- kthread 复用任务机制，但具备专用属性、入口和停止协议。
- 创建者通过 `kthreadd()` 提交请求，普通创建与主体启动可以分离。
- `kthread_stop()` 是同步协作停止，不是任意位置强杀。
- park 用于保留并暂挂线程，不等于退出。
- 用户 mm 借用、任务引用和共享数据生命周期必须分别管理。

## 参考

- [`Documentation/driver-api/basics.rst`](../../linux/Documentation/driver-api/basics.rst)：kthread 接口文档。
- [`Documentation/admin-guide/kernel-per-CPU-kthreads.rst`](../../linux/Documentation/admin-guide/kernel-per-CPU-kthreads.rst)。
- [1.11 PID 1：从内核初始化到用户程序](../01-boot/11-userspace.md)。
