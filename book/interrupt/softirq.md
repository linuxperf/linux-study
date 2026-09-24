# softirq 机制：待处理工作如何被执行

网卡中断处理函数已经返回，接收描述符却还没有全部检查，数据包也没有全部交给协议栈。这些工作保存在哪里？谁会继续处理？如果新数据不断到来，CPU 什么时候才能回到其他任务？

softirq 的源码就是围绕这些问题组织的。理解它时，可以先抓住三个对象：**子系统保存工作的队列、当前 CPU 的 pending 位图、负责扫描位图并调用回调的执行器。** 队列记录具体工作，位图记录哪些类别需要处理，执行器决定何时调用这些类别的处理函数。

本章依据本地 [Linux Makefile](../../linux/Makefile#L2) 标记的 **6.18.52** 版本。主线采用 **x86-64 的 IDT 中断入口、e1000e 网卡的 MSI 接收路径**，假定未启用 `CONFIG_PREEMPT_RT`、未强制线程化 IRQ，NAPI 使用普通 softirq 轮询模式。实时内核、线程化 IRQ 与线程化 NAPI 的差异放在后面讨论。e1000e 源码中不少函数仍使用 `e1000_` 前缀，本文均引用 `drivers/net/ethernet/intel/e1000e/` 下的实现。

建议分三遍阅读：先用第 1～5 节建立触发与执行模型，再用第 6～9 节理解执行上下文和网卡实例，最后阅读生命周期、配置差异和观测方法。

## 1. 从网卡中断返回以后，工作去了哪里

### 1.1 把快速响应与成批处理连接起来

e1000e 的 [`e1000_intr_msi()`](../../linux/drivers/net/ethernet/intel/e1000e/netdev.c#L1750)读取中断原因、处理必要的设备状态，然后通过 [`napi_schedule_prep()` 与 `__napi_schedule()`](../../linux/drivers/net/ethernet/intel/e1000e/netdev.c#L1800)安排接收处理。真正清理接收描述符的操作由后续 [`e1000e_poll()`](../../linux/drivers/net/ethernet/intel/e1000e/netdev.c#L2658)调用。

先忽略预算和并发，整个过程可以缩成下面几步：

```text
e1000e 硬中断处理函数
    │
    ├─ 将 adapter->napi 放入当前 CPU 的 softnet_data.poll_list
    └─ 标记 NET_RX_SOFTIRQ 待处理
                  │
                  ▼
          softirq 核心检查 pending
                  │
                  ▼
           net_rx_action()
                  │  遍历待轮询的 NAPI 实例
                  ▼
           e1000e_poll()
                  │
                  └─ 清理发送完成项、处理接收描述符
```

这条链的接合点分别是 [`____napi_schedule()`](../../linux/net/core/dev.c#L4892)、[`handle_softirqs()`](../../linux/kernel/softirq.c#L579)、[`net_rx_action()`](../../linux/net/core/dev.c#L7800)和 [`__napi_poll()`](../../linux/net/core/dev.c#L7635)。softirq 核心只知道需要调用 `NET_RX_SOFTIRQ` 对应的函数；具体哪块网卡、哪个 NAPI 实例还有工作，由网络子系统管理。

### 1.2 “延后”描述的是处理阶段

在主线配置下，硬中断退出时就可能执行 softirq，然后才恢复被打断的代码。因此，延后处理未必经过一次任务调度，也未必发生在某个专用线程中。硬中断退出检查见 [`__irq_exit_rcu()`](../../linux/kernel/softirq.c#L713)，直接执行与唤醒线程的选择见 [`invoke_softirq()`](../../linux/kernel/softirq.c#L487)。

softirq 的触发操作也很轻量：[`__raise_softirq_irqoff()`](../../linux/kernel/softirq.c#L786)对当前 CPU 的位图做一次按位或。这里没有执行 x86 的软件陷入指令，没有分配 IDT vector，也没有根据 Linux IRQ 号查找驱动。

因此，本章中的 **softirq 编号是工作类别的数组下标**。它与设备使用的 Linux IRQ 号、CPU 使用的 x86 vector 属于不同编号空间。

## 2. 数据模型：全局回调表、每 CPU 位图、子系统队列

### 2.1 全局表回答“这一类工作由谁处理”

[`struct softirq_action`](../../linux/include/linux/interrupt.h#L587)只有一个无参数、无返回值的函数指针：

```c
struct softirq_action
{
    void (*action)(void);
};
```

全局 [`softirq_vec[NR_SOFTIRQS]`](../../linux/kernel/softirq.c#L60)保存各类别的回调。当前源码的[枚举定义](../../linux/include/linux/interrupt.h#L547)共有 10 类，编号从 0 开始：

| 编号 | 类别 | 对应处理入口或用途 |
| --- | --- | --- |
| 0 | `HI_SOFTIRQ` | 高优先级 tasklet 与高优先级 BH workqueue，见 [`tasklet_hi_action()`](../../linux/kernel/softirq.c#L956) |
| 1 | `TIMER_SOFTIRQ` | 普通内核定时器，见 [`run_timer_softirq` 的注册](../../linux/kernel/time/timer.c#L2579) |
| 2 | `NET_TX_SOFTIRQ` | 网络发送侧延后处理，见 [`net_tx_action` 的注册](../../linux/net/core/dev.c#L13231) |
| 3 | `NET_RX_SOFTIRQ` | 网络接收轮询，见 [`net_rx_action` 的注册](../../linux/net/core/dev.c#L13232) |
| 4 | `BLOCK_SOFTIRQ` | 块层完成处理，见 [`blk_done_softirq` 的注册](../../linux/block/blk-mq.c#L5261) |
| 5 | `IRQ_POLL_SOFTIRQ` | IRQ 轮询工作，见 [`irq_poll_softirq` 的注册](../../linux/lib/irq_poll.c#L214) |
| 6 | `TASKLET_SOFTIRQ` | 普通 tasklet 与普通 BH workqueue，见 [`tasklet_action()`](../../linux/kernel/softirq.c#L950) |
| 7 | `SCHED_SOFTIRQ` | 调度域负载均衡，见 [`sched_balance_softirq` 的注册](../../linux/kernel/sched/fair.c#L14194) |
| 8 | `HRTIMER_SOFTIRQ` | 需要在 softirq 中运行的高精度定时器，见 [`hrtimer_run_softirq` 的注册](../../linux/kernel/time/hrtimer.c#L2335) |
| 9 | `RCU_SOFTIRQ` | RCU 处理，例如 [`rcu_core_si` 的注册](../../linux/kernel/rcu/tree.c#L4879) |

枚举中存在某个类别，并不意味着所有配置都注册它，也不意味着所属子系统的全部工作都在这里完成。应当继续检查注册点的配置条件与触发点。

### 2.2 每 CPU 位图回答“本 CPU 哪些类别待处理”

当前 x86 实现将 pending 保存为单独的每 CPU `u16` 变量 [`__softirq_pending`](../../linux/arch/x86/kernel/irq.c#L36)，并通过 [`local_softirq_pending_ref`](../../linux/arch/x86/include/asm/hardirq.h#L68)接入通用接口。不要直接套用其他架构或旧版本中“pending 一定在 `irq_stat` 里”的布局。

三个[访问宏](../../linux/include/linux/interrupt.h#L525)分别读、覆盖和按位或当前 CPU 的值：

```c
#define local_softirq_pending() (__this_cpu_read(local_softirq_pending_ref))
#define set_softirq_pending(x)  (__this_cpu_write(local_softirq_pending_ref, (x)))
#define or_softirq_pending(x)   (__this_cpu_or(local_softirq_pending_ref, (x)))
```

例如，CPU 0 上设置 `BIT(NET_RX_SOFTIRQ)`，只说明 CPU 0 的网络接收处理入口需要运行。CPU 1 有自己独立的 pending 状态。同一个全局回调可以在不同 CPU 上处理不同的本地工作。

### 2.3 pending 不保存工作数量

假设没有消费发生，连续三次执行：

```text
pending |= BIT(NET_RX_SOFTIRQ)
pending |= BIT(NET_RX_SOFTIRQ)
pending |= BIT(NET_RX_SOFTIRQ)
```

结果仍然只有一个置位。位图不能区分“一次通知”和“一百次通知”，也不包含数据包地址。**多个触发合并成一次待处理标记，工作本身必须留在子系统的数据结构中。** 这一性质直接来自 [`or_softirq_pending()`](../../linux/include/linux/interrupt.h#L527)的按位或语义。

对于网络接收，工作又分为两层：

- [`softnet_data.poll_list`](../../linux/include/linux/netdevice.h#L3501)保存本 CPU 待轮询的 NAPI 实例。
- 网卡接收环保存已经完成的描述符，e1000e 在 [`e1000_clean_rx_irq()`](../../linux/drivers/net/ethernet/intel/e1000e/netdev.c#L929)中检查描述符的 `DD` 状态并推进消费位置。

因此，pending 清零并不等于“所有数据包已经处理完”，pending 置位也不能用于估计接收队列深度。

## 3. 注册与触发：安排执行，不直接执行回调

### 3.1 `open_softirq()` 只安装回调

[`open_softirq()`](../../linux/kernel/softirq.c#L793)的实现非常短：

```c
void open_softirq(int nr, void (*action)(void))
{
    softirq_vec[nr].action = action;
}
```

它没有动态分配编号，没有创建设备对象，也没有建立每设备的回调链。网络子系统在[初始化时](../../linux/net/core/dev.c#L13231)注册 `net_tx_action` 和 `net_rx_action`；tasklet 则在 [`softirq_init()`](../../linux/kernel/softirq.c#L1035)中注册两个入口。

这解释了驱动的接入方式：e1000e 通过 [`netif_napi_add()`](../../linux/drivers/net/ethernet/intel/e1000e/netdev.c#L7465)注册自己的 NAPI 回调，由网络层统一调度。它不为每块网卡分配一个 softirq 编号，也不覆盖全局 `NET_RX_SOFTIRQ` 回调。

### 3.2 三种 raise 接口分别承担什么责任

| 接口 | 对本地硬中断状态的要求 | 行为 |
| --- | --- | --- |
| `__raise_softirq_irqoff(nr)` | 调用时必须已关闭 | 记录 raise tracepoint，设置 pending 位 |
| `raise_softirq_irqoff(nr)` | 调用时必须已关闭 | 设置 pending，并在需要时唤醒 `ksoftirqd` |
| `raise_softirq(nr)` | 由接口保存、关闭并恢复 | 包装 `raise_softirq_irqoff()` |

对应实现集中在 [`kernel/softirq.c` 的 raise 接口](../../linux/kernel/softirq.c#L760)。名字中的 `irqoff` 表示调用前提，不能理解为函数会代替调用者关闭硬中断。

主线配置下，`raise_softirq_irqoff()` 在 `!in_interrupt()` 时唤醒本 CPU 的 `ksoftirqd`。这里的 `in_interrupt()` 还覆盖 BH 被禁用的情况，见[上下文判断宏](../../linux/include/linux/preempt.h#L135)。于是：

- 从硬中断触发时，通常留给中断退出路径处理。
- 从正在执行的 softirq 触发时，留给执行循环再次检查。
- 在 `local_bh_disable()` 区间触发时，最外层重新启用 BH 是后续处理机会。
- 从普通、BH 已启用的任务上下文触发时，需要唤醒 `ksoftirqd`，确保存在后续执行者。

最低层 `__raise_softirq_irqoff()` 不负责唤醒，因此调用者必须有明确的后续执行路径。例如 [`net_rx_action()`](../../linux/net/core/dev.c#L7853)在自己仍位于 softirq 执行循环中时，用它安排下一轮网络处理。

### 3.3 唤醒线程不代表回调已经完成

[`wakeup_softirqd()`](../../linux/kernel/softirq.c#L75)取得本 CPU 的线程指针，并调用 `wake_up_process()`。唤醒只是使线程有机会获得 CPU；raise 接口没有等待回调完成的协议。

同样，raise 本身不提供跨 CPU 队列的发布协议。若工作会由另一个 CPU 消费，队列的锁、原子状态和内存顺序必须由所属子系统安排。e1000e/NAPI 使用[原子修改 NAPI 状态](../../linux/net/core/dev.c#L6659)取得调度资格，再在[关闭本地硬中断的区间](../../linux/net/core/dev.c#L6640)修改本 CPU 的 poll 链表。

## 4. 执行入口：哪些时机会检查 pending

### 4.1 硬中断退出：最直接的执行机会

x86-64 的 [`run_irq_on_irqstack_cond()`](../../linux/arch/x86/include/asm/irq_stack.h#L189)把设备中断处理放在 `irq_enter_rcu()` 与 `irq_exit_rcu()` 之间。退出时，通用代码在[扣除硬中断计数之后](../../linux/kernel/softirq.c#L720)检查：

```c
preempt_count_sub(HARDIRQ_OFFSET);
if (!in_interrupt() && local_softirq_pending())
    invoke_softirq();
```

这里的顺序十分关键。扣除的是刚完成的这一层 hardirq；如果它打断的是一个正在执行的 softirq，或者一个 BH 被禁用的区间，`in_interrupt()` 仍不为零，此时不会递归调用 softirq 执行器。

在未强制线程化的非 RT 配置下，[`invoke_softirq()`](../../linux/kernel/softirq.c#L487)直接进入 `__do_softirq()` 或 `do_softirq_own_stack()`。因此可以出现下面的时序：

```text
普通任务被网卡 IRQ 打断
  → e1000_intr_msi() 标记 NET_RX 待处理
  → IRQ 退出路径调用 softirq
  → net_rx_action() 批量处理
  → 最后恢复原来的执行流
```

此时 CPU 借用了被打断任务的执行机会，回调并没有因为 `current` 指向该任务而获得可睡眠的普通任务上下文。

### 4.2 重新启用 BH：普通任务也可能执行 pending

非 RT 的 [`__local_bh_enable_ip()`](../../linux/kernel/softirq.c#L428)在解除最外层 BH 禁用后，若不处于其他中断上下文且存在 pending，就调用 `do_softirq()`。这意味着 `local_bh_enable()` 可能执行一批延后工作，调用成本并非总是一次计数减法。

[`do_softirq()`](../../linux/kernel/softirq.c#L510)先检查 `in_interrupt()`，然后保存本地 IRQ 状态，读取 pending，必要时调用 `do_softirq_own_stack()`，最后恢复 IRQ 状态。它与 `__do_softirq()` 的职责不同：前者处理调用环境，后者直接进入核心分派循环。

### 4.3 `ksoftirqd/N`：获得调度机会后继续执行

第三个入口是每 CPU 的 [`run_ksoftirqd()`](../../linux/kernel/softirq.c#L1055)。它检查本 CPU pending，并调用同一个 `handle_softirqs()`。硬中断退出、BH 重新启用、内核线程，最终共享同一套类别表和待处理位图。

### 4.4 换栈与切换任务是两件事

x86 的 [`do_softirq_own_stack()`](../../linux/arch/x86/include/asm/irq_stack.h#L206)可以切换到本 CPU 的 IRQ 栈再调用 `__do_softirq()`，避免在已经很深的任务栈上继续消耗栈空间。硬中断入口则根据[是否来自用户态、IRQ 栈是否已在使用](../../linux/arch/x86/include/asm/irq_stack.h#L133)选择是否换栈。

这些栈切换不会把当前执行变成 `ksoftirqd`。只有调度器实际运行了该内核线程，`current` 才对应 `ksoftirqd/N`。分析调用栈时，应分别判断“使用哪个栈”和“当前在哪种执行上下文”。

## 5. 核心循环：如何消费 pending，又保留新来的工作

### 5.1 先读一份保留关键顺序的伪代码

下面是 [`handle_softirqs()`](../../linux/kernel/softirq.c#L579)的简化伪代码，省略记账、RCU 和调试辅助操作。进入和返回时本地硬中断均关闭：

```text
end = jiffies + msecs_to_jiffies(2)
max_restart = 10
pending = 读取本 CPU 的 pending
进入 softirq 执行上下文

restart:
    本 CPU 的 pending = 0
    开本地硬中断

    按编号从小到大，处理 pending 快照中每个置位的类别：
        增加该类别的执行次数
        调用 softirq_vec[nr].action()

    关本地硬中断
    pending = 重新读取本 CPU 的 pending

    如果 pending 非零：
        如果未到时间上限、无需重新调度，且还有重启次数：
            跳到 restart
        否则：
            唤醒本 CPU 的 ksoftirqd

退出 softirq 执行上下文
```

阅读这里最容易混淆两个同名概念：局部变量 `pending` 是**当前这一轮的快照**，每 CPU 的 pending 是**执行期间新产生的请求**。它们可以同时非零，也可以一个为零而另一个非零。

### 5.2 为什么先清 pending，再打开硬中断

源码先[读取快照](../../linux/kernel/softirq.c#L596)，再[清空每 CPU pending，最后打开本地硬中断](../../linux/kernel/softirq.c#L602)。于是新来的硬中断可以把新的通知写入每 CPU pending，而当前循环继续消费局部快照。

以下以同一个 `NET_RX_SOFTIRQ` 在处理期间再次被触发为例：

| 时刻 | 本 CPU pending | 局部快照 | 发生的事情 |
| --- | --- | --- | --- |
| A | `NET_RX` | 尚未读取 | 首次收到工作通知 |
| B | 0 | `NET_RX` | 执行器取出快照并清空本地位图 |
| C | 0 | `NET_RX` | 开硬中断，执行 `net_rx_action()` |
| D | `NET_RX` | `NET_RX` | 新中断或回调再次标记接收工作 |
| E | `NET_RX` | 新读取的 `NET_RX` | 本轮返回后，执行器看到新请求 |

只要遵循接口的中断状态和队列同步约定，D 时刻的新置位不会被本轮结束操作覆盖，因为清零已经发生在 B 时刻。相反，如果把清零放在回调之后，新通知就可能被抹掉。

“再次置位”只保证再次获得处理机会，实际工作仍由队列状态决定。新工作有时已被正在运行的回调顺便消费，后续多执行一次回调并不代表位图机制出错。

### 5.3 为什么允许硬中断打断 softirq，却不递归执行 softirq

回调调用前的 [`local_irq_enable()`](../../linux/kernel/softirq.c#L606)使 CPU 能继续响应硬中断。与此同时，非 RT 的 [`softirq_handle_begin()`](../../linux/kernel/softirq.c#L461)增加 `SOFTIRQ_OFFSET`，标记当前正在服务 softirq。

因此，硬中断打断回调并返回时，[`__irq_exit_rcu()`](../../linux/kernel/softirq.c#L722)仍会发现当前处于 softirq 上下文，留下 pending 后恢复外层回调。外层循环在[关闭硬中断后重新检查](../../linux/kernel/softirq.c#L637)，统一决定是否再处理一轮。这避免了同 CPU 上不断递归进入执行器。

### 5.4 编号决定扫描顺序，不构成抢占优先级

循环用 [`ffs(pending)`](../../linux/kernel/softirq.c#L610)找到最低置位，再推进回调指针和位图。一个快照内，编号较小的类别先执行，每个置位类别执行一次。

但如果 `NET_RX_SOFTIRQ` 正在运行时又产生 `HI_SOFTIRQ`，核心不会中断当前网络回调立即改跑 `HI`。新置位进入下一次 pending 检查。`HI` 的“高优先级”应理解为扫描顺序上的优先，不能当作调度器的抢占优先级。

### 5.5 2 毫秒与 10 次究竟限制什么

源码设置 [`MAX_SOFTIRQ_TIME = msecs_to_jiffies(2)` 与 `MAX_SOFTIRQ_RESTART = 10`](../../linux/kernel/softirq.c#L530)。一轮快照中的所有回调返回后，若还有 pending，才检查：

```c
if (time_before(jiffies, end) && !need_resched() &&
    --max_restart)
    goto restart;
```

由[检查位置](../../linux/kernel/softirq.c#L639)可以得出四个结论：

1. **时间限制作用于是否再开始一轮。** 核心不能在某个回调运行到 2 毫秒时强制打断它。
2. **这里按 jiffies 判断时间。** `msecs_to_jiffies(2)` 会受 `HZ` 和换算粒度影响，不是高精度的 2 毫秒定时器。
3. **本次调用最多执行 10 轮快照。** 初始值为 10，每次准备重启时先减一；包括首次执行在内最多 10 轮，并非首次之外再重启 10 次。
4. **`need_resched()` 只阻止下一轮。** 它不会跳过当前快照中尚未调用的类别，也不会立即中止正在运行的回调。

次数限制还用于覆盖 `jiffies` 可能暂时不前进的情况，源码注释举了 `stop_machine()` 的例子。时间、次数和重新调度需求共同决定是否把剩余工作留给线程，而具体回调仍需要自己控制处理量。

### 5.6 回调之外的上下文收尾

核心还会进行软中断时间记账、记录 entry/exit tracepoint、检查回调前后的 `preempt_count()` 是否一致，见[分派循环中的辅助操作](../../linux/kernel/softirq.c#L598)。若回调错误地遗留了抢占计数，核心会报错并恢复计数；这是诊断措施，不能代替回调正确配对禁用和恢复操作。

它还在入口清除借用任务的 `PF_MEMALLOC`，退出时恢复原值，见[任务标志处理](../../linux/kernel/softirq.c#L589)及[退出收尾](../../linux/kernel/softirq.c#L648)。这个细节再次说明：softirq 可以借用当前任务执行，但必须隔离不应继承的任务状态。

## 6. `ksoftirqd`：让剩余工作参与任务调度

### 6.1 每个 CPU 都有自己的处理线程

[`softirq_threads`](../../linux/kernel/softirq.c#L1103)描述了线程的三个关键属性：

```c
static struct smp_hotplug_thread softirq_threads = {
    .store             = &ksoftirqd,
    .thread_should_run = ksoftirqd_should_run,
    .thread_fn         = run_ksoftirqd,
    .thread_comm       = "ksoftirqd/%u",
};
```

[`spawn_ksoftirqd()`](../../linux/kernel/softirq.c#L1152)通过 smpboot 框架注册每 CPU 线程。底层使用 [`kthread_create_on_cpu()`](../../linux/kernel/smpboot.c#L180)创建线程，并标记其 CPU 归属。这里没有把所有 CPU 的 pending 集中到一条公共队列，也不会因为 CPU 0 很忙，就自动让 `ksoftirqd/1` 消费 CPU 0 的位图。

[`ksoftirqd_should_run()`](../../linux/kernel/softirq.c#L1050)返回本 CPU 的 pending 状态；smpboot 的[主循环](../../linux/kernel/smpboot.c#L154)据此决定睡眠还是调用 `run_ksoftirqd()`。

### 6.2 为什么在线程里仍然不能随意睡眠

`run_ksoftirqd()` 在非 RT 下先关闭本地硬中断，再调用 `handle_softirqs(true)`；核心随后打开硬中断、建立 softirq 执行上下文、调用各类回调。处理完毕后，线程才[恢复环境并调用 `cond_resched()`](../../linux/kernel/softirq.c#L1055)。

因此，非 RT 下需要区分两个阶段：

```text
ksoftirqd/N 的线程循环：可以被调度，可以在等待工作时睡眠
    ↓
handle_softirqs() 内的回调：处于 softirq 上下文，不能主动阻塞睡眠
    ↓
退出 softirq 上下文：cond_resched() 提供重新调度机会
```

把回调交给 `ksoftirqd`，主要改变的是取得 CPU 的方式；回调仍遵守同一套 softirq 上下文约束。它不能因为线程名称出现在调用栈上，就调用 `msleep()`、等待完成量或获取可能睡眠的普通互斥锁。

### 6.3 `ksoftirqd` 的出现不一定意味着过载

至少有三类原因会让它参与工作：

1. 核心分派循环还有 pending，但[时间、轮数或重新调度条件](../../linux/kernel/softirq.c#L639)不允许继续。
2. 普通任务上下文调用 [`raise_softirq_irqoff()`](../../linux/kernel/softirq.c#L773)，需要唤醒一个后续执行者。
3. 配置要求在硬中断退出时[转交线程](../../linux/kernel/softirq.c#L487)，例如强制线程化分支。

`ksoftirqd` 采用普通调度策略，源码在[定时器线程设计说明](../../linux/include/linux/interrupt.h#L624)中明确说明其 `SCHED_OTHER` 定位。把剩余工作放到可调度线程，有助于让其他任务获得运行机会；代价是后续处理时间会受线程调度影响。

唤醒 `ksoftirqd` 也不会把 pending 标记为该线程独占。在本文的非 RT、未强制线程化路径中，若另一个硬中断先到达，退出路径仍可能先处理这些 pending。线程真正运行时会[重新检查位图](../../linux/kernel/softirq.c#L1058)，而不会假设自己被唤醒就一定有工作。

## 7. 上下文与并发：BH 禁用到底保护了什么

### 7.1 区分“正在处理”和“暂时禁止处理”

非 RT 下，softirq 状态编码在 `preempt_count()` 的一部分位中。[偏移定义](../../linux/include/linux/preempt.h#L33)给出：

```text
SOFTIRQ_OFFSET         = 1 << 8 = 0x100
SOFTIRQ_DISABLE_OFFSET = 2 * SOFTIRQ_OFFSET = 0x200
```

核心进入回调时加 `0x100`；每嵌套一层 `local_bh_disable()` 加 `0x200`。这样可以用 softirq 计数域的最低位区分执行状态与单纯禁用状态，设计意图见 [`softirq.c` 的计数说明](../../linux/kernel/softirq.c#L91)。

以下只列出 softirq 计数域，忽略其他抢占与中断位：

| 状态 | softirq 计数 | `in_serving_softirq()` | `in_softirq()` |
| --- | --- | --- | --- |
| 普通任务，BH 已启用 | `0x000` | 假 | 假 |
| 普通任务，一层 BH 禁用 | `0x200` | 假 | 真 |
| 普通任务，两层 BH 禁用 | `0x400` | 假 | 真 |
| 正在执行 softirq | `0x100` | 真 | 真 |
| softirq 内又禁用一层 BH | `0x300` | 真 | 真 |

所以，看到 `in_softirq()` 为真，不能据此认定当前一定正在执行 softirq 回调。[头文件](../../linux/include/linux/preempt.h#L118)提供 `in_serving_softirq()` 来表达“正在服务 softirq”，并将含义较宽的 `in_softirq()`、`in_interrupt()` 列为不建议新代码使用的旧接口。

### 7.2 `local_bh_disable()` 不会让硬件停止发中断

[`local_bh_disable()`](../../linux/include/linux/bottom_half.h#L18)增加 BH 禁用计数。非 RT 下，它阻止本 CPU 在临界区内开始执行 softirq，并通过计数保持不可抢占；它不会在整个临界区持续关闭本地硬中断，也不会修改网卡的中断屏蔽寄存器。

因此，下面的过程是允许的：

```text
任务调用 local_bh_disable()
    ↓
硬中断到来，设置 NET_RX pending
    ↓
硬中断退出时发现 BH 仍被禁用，暂不执行 softirq
    ↓
恢复任务，继续原来的临界区
    ↓
最外层 local_bh_enable() 才可能处理 pending
```

嵌套禁用必须配对解除。非 RT 的[重新启用路径](../../linux/kernel/softirq.c#L427)先减去 `cnt - 1`，保留一个抢占计数单位；必要的 softirq 处理结束后，再减去最后一个单位并检查重新调度。它避免在解除 BH 禁用到执行 pending 之间出现不受控的抢占窗口。

### 7.3 同 CPU 不递归，不代表跨 CPU 自动互斥

主线配置下可以总结为：

| 参与者 | 是否可能并发或打断 | 需要注意的保护范围 |
| --- | --- | --- |
| 同 CPU 的另一轮普通 softirq 分派 | 不会递归进入当前 softirq | 当前执行计数阻止 IRQ 退出时再次进入 |
| 同 CPU 的硬中断 | 可以打断开 IRQ 的回调 | 共享数据可能需要 IRQ 级保护 |
| 其他 CPU 的同类 softirq | 可以同时执行 | per-CPU pending 不提供全局互斥 |
| 其他 CPU 的任务或硬中断 | 可以同时访问共享对象 | 仍需要对象锁、原子操作等同步 |

源码开头的[并发设计注释](../../linux/kernel/softirq.c#L37)明确要求各 softirq 实现管理自己的串行化。全局 `softirq_vec` 共享的是回调入口，不是一个全局执行锁。

### 7.4 从访问者决定锁法

对非 RT 配置，先列出共享数据可能在哪些上下文被访问，再选择保护方式：

- **任务与 softirq 共享。** 任务侧通常使用 `spin_lock_bh()`：先禁止本 CPU 的 softirq，再用锁协调其他 CPU。若任务只拿普通自旋锁，本 CPU softirq 打断任务后又尝试拿同一把锁，就可能等待一个无法恢复执行的持锁者。
- **softirq 与硬中断共享。** softirq 侧通常需要 `spin_lock_irqsave()` 一类保护，防止本 CPU 硬中断打断持锁区后再次等待同一把锁。
- **只有不同 CPU 的 softirq 共享。** 需要跨 CPU 互斥；能否使用普通自旋锁，取决于是否还有任务或硬中断访问同一数据。
- **数据确实只在当前 CPU 的任务/BH 路径访问。** 非 RT 下，本地 BH 禁用可用于排除本 CPU 的 softirq；一旦存在硬中断访问或跨 CPU 访问，就需要补充相应同步。

实现上，[`__raw_spin_lock_bh()`](../../linux/include/linux/spinlock_api_smp.h#L123)先调用 BH 禁用函数再获取锁，释放时[先解锁再重新启用 BH](../../linux/include/linux/spinlock_api_smp.h#L163)；[`__raw_spin_lock_irqsave()`](../../linux/include/linux/spinlock_api_smp.h#L104)则先保存并关闭本地 IRQ。局部屏蔽解决本 CPU 的重入，锁解决不同 CPU 的竞争，二者作用范围不同。

这些分析以非 RT 锁语义为前提。实时内核中的 `spinlock_t`、本地锁和 BH 禁用行为需要按第 12 节重新检查。

## 8. e1000e 实例：从 MSI 到 NAPI 完成

### 8.1 先建立两级回调关系

初始化阶段有两次注册：

```text
网络核心：open_softirq(NET_RX_SOFTIRQ, net_rx_action)
驱动：    netif_napi_add(netdev, &adapter->napi, e1000e_poll)
```

前者设置[全局 softirq 入口](../../linux/net/core/dev.c#L13232)，后者把[驱动回调放入 NAPI 实例](../../linux/drivers/net/ethernet/intel/e1000e/netdev.c#L7465)。[`struct napi_struct`](../../linux/include/linux/netdevice.h#L383)同时保存 `poll_list`、`state`、`weight` 和 `poll`，使网络层可以统一管理多个设备的轮询。

默认 [`netif_napi_add()`](../../linux/include/linux/netdevice.h#L2822)使用 `NAPI_POLL_WEIGHT`，本版本[定义为 64](../../linux/include/linux/netdevice.h#L2796)。设备打开时，驱动先 [`napi_enable()`，再启用设备中断](../../linux/drivers/net/ethernet/intel/e1000e/netdev.c#L4693)，使中断到来时已经有可调度的 NAPI 实例。

### 8.2 MSI 处理函数取得调度资格

[`e1000_intr_msi()`](../../linux/drivers/net/ethernet/intel/e1000e/netdev.c#L1750)读取 ICR。驱动在[接收配置](../../linux/drivers/net/ethernet/intel/e1000e/netdev.c#L3234)中设置 IAME 和 IAM，使读取 ICR 能自动屏蔽相应设备中断。随后，正常收发分支执行：

```c
if (napi_schedule_prep(&adapter->napi)) {
    adapter->total_tx_bytes = 0;
    adapter->total_tx_packets = 0;
    adapter->total_rx_bytes = 0;
    adapter->total_rx_packets = 0;
    __napi_schedule(&adapter->napi);
}
```

这段代码见[驱动中的 NAPI 调度点](../../linux/drivers/net/ethernet/intel/e1000e/netdev.c#L1800)。屏蔽设备中断使接下来的接收工作可以成批轮询，直到网络层允许驱动重新启用通知。

[`napi_schedule_prep()`](../../linux/net/core/dev.c#L6659)通过比较交换修改 NAPI 状态：

- 若已设置 `NAPI_STATE_DISABLE`，拒绝新的调度。
- 若尚未设置 `NAPI_STATE_SCHED`，设置它并返回真，调用者取得入队资格。
- 若已经设置 `SCHED`，设置 `NAPI_STATE_MISSED` 并返回假，避免重复插入同一个链表节点，同时保留再次检查的需求。

这里的去重是 **NAPI 实例级别**的状态协议。softirq 位图的去重则是 **CPU 上工作类别级别**的协议。两者不能互相替代。

### 8.3 把 NAPI 实例挂到当前 CPU

[`__napi_schedule()`](../../linux/net/core/dev.c#L6640)在保存、关闭本地 IRQ 后调用 `____napi_schedule()`。普通 softirq 模式下，后者将 NAPI 节点[加入当前 CPU 的 `poll_list`](../../linux/net/core/dev.c#L4917)，再根据 `sd->in_net_rx_action` 决定是否 raise `NET_RX_SOFTIRQ`。

若当前 CPU 已在运行 `net_rx_action()`，新实例可以先入队，由正在运行的网络回调重新检查队列；否则需要设置 pending，通知外层 softirq 执行器。这也说明：**一个工作入队动作不一定对应一次 `softirq_raise` trace 事件。**

### 8.4 `net_rx_action()` 管理一轮网络处理

[`net_rx_action()`](../../linux/net/core/dev.c#L7800)先读取网络预算，建立两个局部链表：

- `list`：本轮准备处理的 NAPI 实例。
- `repoll`：本轮已经轮询过，但仍需要继续处理的实例。

它短暂关闭 IRQ，将 `sd->poll_list` 移到局部 `list`，随后重新打开 IRQ逐个轮询。这样，处理期间新到来的中断仍可以把新 NAPI 实例挂入 `sd->poll_list`。

每次 [`napi_poll()`](../../linux/net/core/dev.c#L7702)先把实例从待处理链表摘下，再通过 [`__napi_poll()`](../../linux/net/core/dev.c#L7635)调用 `n->poll(n, n->weight)`。若普通分支需要继续轮询，就将该实例放入 `repoll`，避免一个繁忙实例在本轮立即反复占用入口。

一轮结束时，网络层按[收尾代码](../../linux/net/core/dev.c#L7853)整理三部分工作：尚未轮询的 `list`、处理期间新到达的 `sd->poll_list`、需要再次轮询的 `repoll`。整理后仍有工作，就再次设置 `NET_RX_SOFTIRQ` pending，把“继续处理”的决定交回外层 softirq 循环。

### 8.5 网络层怎样避免漏掉执行期间的新入队

考虑 `net_rx_action()` 正准备返回，但新中断刚把 NAPI 实例加入 `sd->poll_list` 的情况。因为 `in_net_rx_action` 为真时入队路径可以不 raise，消费者必须承担最后一次队列检查。

源码在[局部队列都为空的分支](../../linux/net/core/dev.c#L7822)中：

1. 将 `sd->in_net_rx_action` 设为假。
2. 使用 `barrier()`约束编译器重排。
3. 再检查 `sd->poll_list`；若非空，回到 `start` 继续处理。

这样，新工作若在标志清除前入队，会被后面的队列检查发现；若在标志清除后入队，调度方就会负责 raise。这里的 `barrier()` 不能当作任意跨 CPU 发布数据所需的内存屏障：这段协议围绕当前 CPU 的 poll 链表和本地中断交错展开。

### 8.6 `e1000e_poll()` 做实际收发处理

在本文的 MSI 分支，[`e1000e_poll()`](../../linux/drivers/net/ethernet/intel/e1000e/netdev.c#L2668)先尝试回收发送完成项，再调用 `adapter->clean_rx()` 处理接收。`clean_rx` 会根据[接收模式](../../linux/drivers/net/ethernet/intel/e1000e/netdev.c#L3190)选择不同实现；以普通接收实现为例：

```text
从 next_to_clean 取得描述符
    ↓
DD 位表示设备已经完成该描述符
    ↓
检查本次预算，增加 work_done
    ↓
dma_rmb() 后读取描述符和缓冲区信息
    ↓
整理 skb、交给后续接收流程、补充接收缓冲区
    ↓
推进 next_to_clean，直到没有完成项或预算耗尽
```

预算与 DMA 读顺序见[描述符循环开始处](../../linux/drivers/net/ethernet/intel/e1000e/netdev.c#L929)，交付数据与补充缓冲区见[循环后半段](../../linux/drivers/net/ethernet/intel/e1000e/netdev.c#L1025)。补充缓冲区使用 `GFP_ATOMIC`，也反映了此处的非睡眠执行约束。

这里的 `work_done` 按该循环处理的描述符增加，其中可能包括随后被丢弃的接收项。分析预算时，应看驱动实际计数位置，不能把返回值机械等同于“成功送到应用的数据包数”。

### 8.7 处理未完成与完成，走不同的返回协议

[`e1000e_poll()` 的返回逻辑](../../linux/drivers/net/ethernet/intel/e1000e/netdev.c#L2674)可以归纳为：

| 驱动看到的情况 | 返回和状态处理 | 后续效果 |
| --- | --- | --- |
| 发送回收未完成，或接收 `work_done == budget` | 返回 `budget`，保留 NAPI 调度状态 | 普通网络轮询路径安排 repoll |
| 工作少于预算，尝试完成 NAPI | 调用 `napi_complete_done()` | 由 NAPI 状态决定是否真正完成 |
| `napi_complete_done()` 返回真，设备未 DOWN | MSI 路径调用 `e1000_irq_enable()` | 恢复设备通知，等待下一次中断 |
| `napi_complete_done()` 返回假 | 不重新启用设备中断 | 继续遵守网络层的轮询或延迟通知安排 |

返回恰好等于预算，表达的是“需要网络核心继续安排处理”的协议。e1000e 可能因发送回收未完成而返回这个值，所以它甚至不一定等于本次真实接收工作量。

[`napi_complete_done()`](../../linux/net/core/dev.c#L6743)在完成时检查 `MISSED`：若处理期间有人尝试再次调度，就保留 `SCHED`、重新入队并返回假；没有这类需求时才释放调度状态。busy polling、GRO 相关超时和延迟硬中断配置还会影响返回结果，见[函数前半段](../../linux/net/core/dev.c#L6701)。驱动必须依据返回值决定能否重新启用中断。

至此，普通接收路径形成一个完整循环：**中断安排轮询，轮询在预算内消费数据，未完成就继续安排，完成后恢复设备中断。** softirq 提供执行机会，NAPI 和驱动共同管理具体对象的状态。

## 9. 三层预算：分别限制不同范围的工作

外层 softirq、网络回调、单个 NAPI 实例各有自己的处理限制：

| 层次 | 预算或停止条件 | 检查位置 | 剩余工作如何继续 |
| --- | --- | --- | --- |
| softirq 核心 | `msecs_to_jiffies(2)`、最多 10 轮、`need_resched()` | 一轮类别快照处理完之后 | 保留 pending，唤醒 `ksoftirqd` |
| `net_rx_action()` | `netdev_budget`、`netdev_budget_usecs` | 每次 `napi_poll()` 返回之后 | 整理队列，重新 raise `NET_RX_SOFTIRQ` |
| 单次 NAPI poll | `napi->weight` 作为传入预算 | 驱动自己的处理循环 | 返回预算值，请求再次轮询 |

三处实现分别见[核心重新开始条件](../../linux/kernel/softirq.c#L639)、[网络预算检查](../../linux/net/core/dev.c#L7838)和 [NAPI 回调调用](../../linux/net/core/dev.c#L7639)。

本版本网络层的[初始值](../../linux/net/core/hotdata.c#L14)是：

```c
.netdev_budget = 300,
.netdev_budget_usecs = 2 * USEC_PER_SEC / HZ,
```

因此不能把 `netdev_budget_usecs` 一律写成 2000 微秒；它的初始值依赖 `HZ`。它与 `netdev_budget` 还通过[网络 sysctl 表](../../linux/net/core/sysctl_net_core.c#L554)暴露配置项，源码初始值不等于目标机器上的当前值。

### 9.1 总预算为何可能被超过

`net_rx_action()` 在 `napi_poll()` 返回后才减去工作量，而 `__napi_poll()` 传给驱动的是该实例的 `weight`。它不会把网络层剩余预算与 `weight` 取最小值后再调用驱动。

例如，假设网络预算只剩 10，而下一个 NAPI 的 weight 为 64，它仍可能处理并返回 64，然后网络层的剩余预算变为负数并结束本轮。因此，300 是轮询之间的停止阈值，不能解释为这一轮绝不超过 300 个计数单位。这一行为可由[减预算的位置](../../linux/net/core/dev.c#L7839)和[回调参数](../../linux/net/core/dev.c#L7649)直接推导。

### 9.2 网络用完预算，不一定立即切到线程

网络回调用完预算，首先回到 `handle_softirqs()`。如果核心本轮快照中还有其他类别，会继续执行它们；只有随后重新检查 pending，才决定立即再来一轮，还是唤醒 `ksoftirqd`。

因此，同一次硬中断退出可能执行多轮 `NET_RX_SOFTIRQ`；反过来，一次 `NET_RX_SOFTIRQ` 也可能轮询多个 NAPI 实例。硬中断次数、softirq 次数、NAPI poll 次数和包数之间都没有固定的一一对应关系。

### 9.3 调大预算的收益与代价来自哪里

从上述执行位置可以推导：增加某层预算，可能让每次进入该层时完成更多工作，也可能延长后续类别或其他任务等待的时间。若单个回调自身运行很久，外层 2 毫秒条件无法提前终止它。

因此，分析性能问题时应先确认耗时集中在哪一层、队列是否积压、是否真的频繁触及预算，再决定调整。仅凭 `ksoftirqd` 占用较高，无法判断应该增加哪一个预算。
