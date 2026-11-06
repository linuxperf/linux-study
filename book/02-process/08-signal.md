# 2.8 信号生成、排队与线程选择

> **本节源码**：[`kernel/signal.c`](../../linux/kernel/signal.c)、[`include/linux/sched/signal.h`](../../linux/include/linux/sched/signal.h)、[`include/linux/signal_types.h`](../../linux/include/linux/signal_types.h)、[`include/trace/events/signal.h`](../../linux/include/trace/events/signal.h)、[`fs/signalfd.c`](../../linux/fs/signalfd.c)
> **关键函数**：`check_kill_permission()`、`__send_signal_locked()`、`complete_signal()`、`signal_wake_up_state()`、`dequeue_signal()`、`get_signal()`
> **前置知识**：2.3 TID 与 TGID；2.5 信号资源分层；2.7 线程组退出

## 概述

信号需要在执行主体之间异步传递通知，同时保留用户选择忽略、阻塞或捕获的能力。生成端不能直接在目标上下文调用用户函数，而是修改待决状态并促使目标检查。多线程进程还要求区分发给一个线程和发给整个进程的信号。标准信号允许合并，实时信号可携带排队信息，因此“发送成功”“排队一个实例”和“处理器执行一次”不是相同结论。

## 核心数据结构

信号处理方式、待决队列与屏蔽掩码分开存储：

| 对象 / 字段 | 含义 | 源码 |
|---|---|---|
| `sighand_struct.action[]` | 默认、忽略或捕获处理器，以及 `sa_mask`、`sa_flags` 等 | [`include/linux/sched/signal.h:22-27`](../../linux/include/linux/sched/signal.h#L22-L27) |
| `sighand_struct.siglock` | 保护主要信号状态，包含线程组共享状态的同步 | [`include/linux/sched/signal.h:88-97`](../../linux/include/linux/sched/signal.h#L88-L97) |
| `task_struct.pending` | 线程定向的私有待决信号 | [`include/linux/sched.h:1197-1204`](../../linux/include/linux/sched.h#L1197-L1204) |
| `signal_struct.shared_pending` | 进程定向的共享待决信号 | [`include/linux/sched/signal.h:105-113`](../../linux/include/linux/sched/signal.h#L105-L113) |
| `task_struct.blocked` | 本线程阻塞掩码，同线程组中可以不同 | [`include/linux/sched.h:1200-1203`](../../linux/include/linux/sched.h#L1200-L1203) |
| `sigpending.signal` / `list` | 哪些信号待决的位图，以及可携带 `siginfo` 的实例链表 | [`include/linux/signal_types.h`](../../linux/include/linux/signal_types.h) |

阻塞（Blocked）表示暂不按普通投递路径处理，忽略（Ignored）表示采用忽略动作；忽略不等于“把信号留在队列等以后再处理”。线程组共享处理器，不代表共享阻塞掩码。

## 主流程分析

```text
用户发送端                            kernel/signal.c
kill() → kill_something_info() → 按 PID / 进程组选择目标
 tgkill() → do_tkill() → do_send_specific()
相应发送路径
├── 目标查找与 check_kill_permission() 等检查
└── send_signal_locked()
    └── __send_signal_locked()
        ├── prepare_signal()
        ├── 选择 pending 或 shared_pending
        ├── 标准信号合并 / 分配 sigqueue
        ├── 设置待决位、通知 signalfd
        └── complete_signal()
            └── signal_wake_up() / signal_wake_up_state()

接收端                                kernel/signal.c
get_signal()
├── 优先检查同步异常信号
├── dequeue_signal()：私有队列，再共享队列
├── 忽略、停止、core dump 或线程组退出
└── 返回需要建立用户信号帧的 ksignal
```

### 一、选择进程还是线程

`kill()` 按正 PID、0、负 PID 等参数解释进程或进程组目标，见 [`kernel/signal.c:1622-1660`](../../linux/kernel/signal.c#L1622-L1660)。给进程发送信号，不表示给其中每个线程各放一个普通处理实例。

`tgkill(tgid, tid, sig)` 指定线程并验证线程组身份，避免只凭 TID 找到已被重用为其他进程线程的任务，见 [`kernel/signal.c:4230-4256`](../../linux/kernel/signal.c#L4230-L4256)。线程定向信号也可能触发默认的整个进程终止动作；“定向线程”只限定目标和待决位置，不把默认动作改为只结束该线程。

发送检查包含凭据、同会话 `SIGCONT` 特例及 LSM 钩子，见 [`kernel/signal.c:823-857`](../../linux/kernel/signal.c#L823-L857)。使用 pidfd 解决身份重用问题，但同样不能绕过发送权限。

### 二、待决位置由发送类型决定

[`kernel/signal.c:1099-1108`](../../linux/kernel/signal.c#L1099-L1108)（v6.18.60）

```c
	pending = (type != PIDTYPE_PID) ? &t->signal->shared_pending : &t->pending;
	/*
	 * Short-circuit ignored signals and support queuing
	 * exactly one non-rt signal, so that we can get more
	 * detailed information about the cause of the signal.
	 */
	result = TRACE_SIGNAL_ALREADY_PENDING;
	if (legacy_queue(pending, sig))
		goto ret;
```

`legacy_queue()` 检查非实时信号是否已在所选待决集合中，见 [`kernel/signal.c:1061-1064`](../../linux/kernel/signal.c#L1061-L1064)。同一个标准信号在同一个集合中已有待决位时，后续发送不会无限增加实例。私有集合与共享集合又是两个不同位置，不能据“标准信号合并”推导进程所有队列合计永远只有一个实例。

标准信号（Standard Signal）与实时信号（Real-Time Signal）的关键差别是排队语义；实时信号需要时通过 `sigqueue` 保存信息和多个实例。内存或 `RLIMIT_SIGPENDING` 压力下，具体发送接口可能返回 `EAGAIN`，也可能保留信号但丢失详细信息；实现分支见 [`kernel/signal.c:1111-1177`](../../linux/kernel/signal.c#L1111-L1177)。因此要检查 `sigqueue()` 返回值，不能把实时信号当作无限容量消息队列。

### 三、进程定向信号选择一个适合的接收线程

`complete_signal()` 先检查建议任务是否愿意接收；否则在同线程组查找满足条件的线程，并更新 `curr_target`，见 [`kernel/signal.c:987-1020`](../../linux/kernel/signal.c#L987-L1020)。若暂时没有合适线程，共享待决信号仍可保留，等待掩码等状态变化。

选择线程并没有把共享实例永久移动到该线程私有队列。其他合格线程也可能在后续检查中取得它；被唤醒线程只是促使处理发生的目标。

对于不需 core dump 的致命默认动作，路径可以提前建立组退出状态，给组员设置致命待决状态并唤醒，见 [`kernel/signal.c:1023-1058`](../../linux/kernel/signal.c#L1023-L1058)。这与普通用户处理器的一次线程选择要分开分析。

### 四、唤醒只是请求目标检查

`signal_wake_up_state()` 设置 `TIF_SIGPENDING`，尝试唤醒匹配状态的任务；若未唤醒则调用 `kick_process()`，见 [`kernel/signal.c:745-760`](../../linux/kernel/signal.c#L745-L760)。这不等于发送端执行目标的处理器，也不保证普通不可中断等待立即结束。

用户接口不能阻塞或捕获 `SIGKILL`、`SIGSTOP`；掩码设置会去掉它们，见 [`kernel/signal.c:3281-3290`](../../linux/kernel/signal.c#L3281-L3290)、[`kernel/signal.c:3400-3417`](../../linux/kernel/signal.c#L3400-L3417)。但纯 `TASK_UNINTERRUPTIBLE` 的任务不因此立刻恢复执行，必须等待允许继续的内核条件。

### 五、消费与动作选择

`get_signal()` 对由指令执行引起的同步信号有优先处理；普通 `dequeue_signal()` 先查线程私有集合，再查共享集合，见 [`kernel/signal.c:2993-3001`](../../linux/kernel/signal.c#L2993-L3001)、[`kernel/signal.c:642-665`](../../linux/kernel/signal.c#L642-L665)。不存在简单的“所有信号按全局发送时间先来先处理”队列。

处理方式为用户函数时，`get_signal()` 返回一个 `ksignal`，架构代码才会构造用户信号帧。默认动作可能直接忽略、协调停止、执行 core dump 或进入组退出，见 [`kernel/signal.c:3013-3125`](../../linux/kernel/signal.c#L3013-L3125)。

## 关键细节 / 设计考量

### 同步等待和 signalfd 是其他消费路径

`sigwaitinfo()` / `sigtimedwait()` 类接口和 signalfd 可以取走已阻塞信号，而不运行普通用户信号处理器。同步等待实现见 [`kernel/signal.c:3814-3899`](../../linux/kernel/signal.c#L3814-L3899)，signalfd 的读取见 [`fs/signalfd.c`](../../linux/fs/signalfd.c)。依赖 `CONFIG_SIGNALFD=y`。

多线程程序使用 signalfd 时，一般应先在可能竞争接收的线程中阻塞相应信号，再集中读取。只在读取线程阻塞，不能保证其他未阻塞线程不先按普通路径消费进程定向信号。

### 生成和投递事件不能一一配对

信号可能合并、忽略、被调试器改变，或被同步等待等路径消费。`signal_generate` 的 `result` 是排队路径的结果，名字中的 `TRACE_SIGNAL_DELIVERED` 也不保证用户处理器已执行。相关枚举和注释见 [`include/trace/events/signal.h:26-48`](../../linux/include/trace/events/signal.h#L26-L48)、[`include/trace/events/signal.h:83-94`](../../linux/include/trace/events/signal.h#L83-L94)。

## 可观测性

| 入口 | 看什么、怎么解读 | 源码与配置 |
|---|---|---|
| `signal:signal_generate` | `sig`、目标 `pid`、`group`、`result`；`group` 区分共享 / 私有，事件上下文是生成方 | [`include/trace/events/signal.h:50-80`](../../linux/include/trace/events/signal.h#L50-L80)，发出点 [`kernel/signal.c:1197-1200`](../../linux/kernel/signal.c#L1197-L1200)，`CONFIG_EVENT_TRACING=y` |
| `signal:signal_deliver` | `sig`、`code`、`sa_handler`、`sa_flags`；可包含默认 / 忽略动作，不是用户函数入口事件 | [`include/trace/events/signal.h:96-120`](../../linux/include/trace/events/signal.h#L96-L120)、[`kernel/signal.c:3015-3018`](../../linux/kernel/signal.c#L3015-L3018)，`CONFIG_EVENT_TRACING=y` |
| `/proc/<pid>/task/<tid>/status` | `SigPnd` 对应线程私有集合，`ShdPnd` 对应共享集合，`SigBlk` 是线程掩码，`SigIgn` / `SigCgt` 对应共享处理方式 | [`fs/proc/array.c:262-302`](../../linux/fs/proc/array.c#L262-L302)，`CONFIG_PROC_FS=y` |
| 同一 status 的 `SigQ` | 用户排队信号的计数与当前限制，不是本线程队列长度 | [`fs/proc/array.c:284-294`](../../linux/fs/proc/array.c#L284-L294)，`CONFIG_PROC_FS=y` |

## 动手实验：标准信号合并与实时信号排队

[pending-signals.c](examples/pending-signals.c) 阻塞 `SIGUSR1` 与一个实时信号，给自己分别发送 3 次，停留 5 秒后通过 `sigtimedwait()` 非阻塞取出。dev 中运行：

```bash
cd /root/linux
work=$(mktemp -d /tmp/process-signal.XXXXXX)
cc -O2 -Wall -Wextra -std=c11 book/02-process/examples/pending-signals.c \
    -o "$work/pending-signals"
"$work/pending-signals"
rm -r -- "$work"
```

在停留窗口内从另一个终端读取程序打印的 PID：

```bash
pid=1234
grep -E '^(SigQ|SigPnd|ShdPnd|SigBlk|SigIgn|SigCgt):' /proc/"$pid"/status
```

预期通过 `kill()` 和 `sigqueue()` 发给自己的进程定向信号出现在共享集合；标准信号消费 1 次，实时信号消费 3 次且带上不同的值。位图只能表示“该信号存在”，看不出实时信号的实例数。结果以程序实际检查为准；本实验未在目标内核上实测。

## 小结

- 线程定向与进程定向决定待决位置，不能与默认动作的影响范围混淆。
- 标准信号在同一待决集合中合并，实时信号排队受资源限制。
- 进程定向普通信号由合格线程消费，不是每个线程各处理一次。
- 设置待决标志与唤醒目标不等于立即执行用户处理器。
- 生成、动作选择、同步消费和用户处理器执行是不同观测边界。

## 参考

- [`include/trace/events/signal.h`](../../linux/include/trace/events/signal.h)。
- [`fs/signalfd.c`](../../linux/fs/signalfd.c)。
- [2.9 x86_64 信号投递与系统调用重启](09-signal-frame.md)。
