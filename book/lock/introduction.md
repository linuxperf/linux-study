# 锁机制基础：并发问题、底层原语与加锁协议

两个 CPU 同时给一个计数器加一，结果可能只加了一次；一个链表节点已经摘下，却还不能立即释放；任务持有自旋锁时，本 CPU 的中断处理函数再去申请同一把锁，系统就停在那里。这三个现象分别对应三类问题：一次更新是否完整，一组字段是否一致，以及持锁者和对象能否继续存活、继续运行。

本章不深入某一种锁的竞争算法，而是回答学习所有锁之前都要先回答的问题：

1. 并发访问会破坏什么，访问者从哪里来？
2. 锁和相关同步机制分别提供哪一种保证？
3. 锁是由哪些底层原语（原子操作、内存顺序、本地上下文控制）拼起来的？
4. 一次 `spin_lock_irqsave()` 和一次 `mutex_lock()` 是怎样把这些原语组合成协议的？

后续章节再分别展开 qspinlock、mutex、rwsem、RCU 等机制的内部实现。

## 0. 分析基线

本章依据仓库内 [Makefile 第 2～4 行](../../linux/Makefile#L2)标记的 **6.18.52** 版本，架构为 **x86-64**。讨论对象是普通内存上的 CPU 间同步，不涉及设备寄存器、DMA 顺序和用户态 futex。读者需要了解 C 指针、链表和进程调度；中断与软中断的执行路径可先阅读[中断子系统概述](../interrupt/overview.md)和 [softirq 机制](../interrupt/softirq.md)。

与本章结论有关的配置如下。它们是构建条件，不能据此断言某次运行一定经过某条竞争路径。

| 配置 | 对本章的影响 | 依据 |
| --- | --- | --- |
| `CONFIG_X86_64=y`、`CONFIG_SMP=y` | 既要考虑跨 CPU 并行，也要考虑本 CPU 上的交错 | [.config#L333](../../linux/.config#L333)、[.config#L362](../../linux/.config#L362) |
| `CONFIG_PREEMPT_RT` 未设置 | `spinlock_t` 就是对 `raw_spinlock_t` 的包装，mutex 走非 RT 实现 | [.config#L139](../../linux/.config#L139)、[spinlock_types.h#L14](../../linux/include/linux/spinlock_types.h#L14)、[mutex_types.h#L11](../../linux/include/linux/mutex_types.h#L11) |
| `CONFIG_PREEMPT_VOLUNTARY=y`、`CONFIG_PREEMPT_DYNAMIC=y`（因此 `CONFIG_PREEMPTION=y`、`CONFIG_PREEMPT_COUNT=y`） | 抢占计数总是维护；抢占模型在启动时选择，缺省为自愿抢占（见 3.4 节） | [.config#L136-L142](../../linux/.config#L136-L142)、[Kconfig.preempt#L126](../../linux/kernel/Kconfig.preempt#L126) |
| `CONFIG_PREEMPT_RCU=y` | 普通 RCU 读侧临界区可以被抢占，但不能主动阻塞 | [.config#L168](../../linux/.config#L168) |
| `CONFIG_QUEUED_SPINLOCKS=y`、`CONFIG_QUEUED_RWLOCKS=y`、`CONFIG_NR_CPUS=512` | 自旋锁底层是 qspinlock，32 位锁字采用 `NR_CPUS < 16K` 的布局 | [.config#L1123](../../linux/.config#L1123)、[.config#L1125](../../linux/.config#L1125)、[.config#L431](../../linux/.config#L431) |
| `CONFIG_PARAVIRT_SPINLOCKS=y`、`CONFIG_PARAVIRT_XXL=y` | 自旋锁慢路径、解锁和本地中断开关都经过半虚拟化分派，原生指令只是其中一种运行结果 | [.config#L378](../../linux/.config#L378)、[.config#L380](../../linux/.config#L380) |
| `CONFIG_MUTEX_SPIN_ON_OWNER=y` | mutex 在睡眠前可能先乐观自旋 | [.config#L1119](../../linux/.config#L1119) |
| `CONFIG_DEBUG_SPINLOCK`、`CONFIG_DEBUG_MUTEXES`、`CONFIG_DEBUG_LOCK_ALLOC`、`CONFIG_LOCK_STAT` 未设置 | 锁结构中没有调试字段，加锁路径没有统计分支 | [.config#L10660-L10666](../../linux/.config#L10660-L10666) |

## 1. 锁要解决什么问题

### 1.1 三个场景，三种被破坏的东西

**场景一：更新丢失。** `count` 初值为 0，两个执行者各做一次 `count++`。把自增拆成“读出、计算、写回”三步来看（这是概念拆分，不是断言编译器一定生成三条指令）：

| 时刻 | 执行者 A | 执行者 B |
| --- | --- | --- |
| 1 | 读到 0 | |
| 2 | | 读到 0 |
| 3 | 写回 1 | |
| 4 | | 写回 1 |

每一次读、每一次写都是完整的，丢的是“读—改—写”整体的不可分割性。对单个计数器，内核提供原子读改写接口，例如 x86 的 [arch_atomic_inc()](../../linux/arch/x86/include/asm/atomic.h#L51)用带 `LOCK_PREFIX` 的 `incl` 完成一次自增。

**场景二：多字段关系被撕开。** 双向链表要求“后继的 `prev` 指回自己，前驱的 `next` 指向自己”。插入一个节点要改四个指针：

```c
if (!__list_add_valid(new, prev, next))
	return;

next->prev = new;
new->next = next;
new->prev = prev;
WRITE_ONCE(prev->next, new);
```

来源：[__list_add()，include/linux/list.h 第 161～167 行](../../linux/include/linux/list.h#L161)。（当前配置启用了 [CONFIG_LIST_HARDENED](../../linux/.config#L10064)和 [CONFIG_BUG_ON_DATA_CORRUPTION](../../linux/.config#L10065)，检查失败时 [CHECK_DATA_CORRUPTION()](../../linux/include/linux/bug.h#L95)直接 `BUG()`，上面的 `return` 实际不会执行。）

四次赋值之间存在“前驱已改、后继未改”的中间状态。即使把其中某个字段改成原子变量，也无法让四次赋值整体不可分割。这里需要保护的是**结构不变量**：插入、删除以及依赖这些指针的遍历必须遵守同一把锁。

**场景三：查到了对象，却没保证对象还活着。** 假设查找和删除都正确地持有集合锁 `L`：

```text
执行者 A：加锁 L → 从集合取得指针 p → 解锁 L
执行者 B：加锁 L → 摘除 p → 解锁 L → 释放 p
执行者 A：访问 p->field                       ← 访问已释放内存
```

集合锁没有失效，它保护了查找和摘除这两个动作。错误在于 A 把裸指针带出了锁的范围，却没有取得“继续使用对象”的资格。这是**生命周期**问题，需要引用计数或 RCU 这类机制，第 4.3 节再讨论。

三个场景对应三个不同目标：一次更新不可分割、多字段不变量成立、对象在使用期内存活。判断一个机制是否合适，要看它证明的是哪个目标，而不是看名字里有没有“锁”。

### 1.2 访问者从哪里来

内核里的并发有两个维度：

- **跨 CPU 并行**：两个处理器在同一时刻访问同一数据。
- **同 CPU 交错**：同一个 CPU 上，任务被抢占、被软中断或硬中断打断，打断者也访问同一数据。即使只有一个 CPU，这类问题依然存在。

同 CPU 上的执行上下文由 `preempt_count` 的不同位段区分（[preempt.h 第 27～31 行](../../linux/include/linux/preempt.h#L27)）：

| 位段 | 掩码 | 含义 |
| --- | --- | --- |
| bit 0–7 | `PREEMPT_MASK` = `0x000000ff` | 禁止抢占的嵌套深度 |
| bit 8–15 | `SOFTIRQ_MASK` = `0x0000ff00` | 正在处理 softirq（`SOFTIRQ_OFFSET`）或关闭了下半部（`SOFTIRQ_DISABLE_OFFSET`） |
| bit 16–19 | `HARDIRQ_MASK` = `0x000f0000` | 硬中断嵌套深度 |
| bit 20–23 | `NMI_MASK` = `0x00f00000` | NMI 嵌套深度 |

| 执行上下文 | 可能打断它的本 CPU 活动 | 对锁的约束 |
| --- | --- | --- |
| 进程上下文 | 抢占（取决于抢占模型）、softirq、hardirq、NMI | 只有在当前允许睡眠时才能使用会阻塞的锁 |
| softirq | hardirq、NMI；本 CPU 不会再嵌套进入另一次 softirq 处理 | 不能睡眠；不同 CPU 仍可同时运行同一种 softirq |
| hardirq | NMI（普通 IRQ 处理期间本地 IRQ 关闭） | 不能睡眠，不能等待被它打断的持锁者 |
| NMI | 不受本地 IRQ 开关约束，可以在任意时刻到来 | 不能用普通的 `_irqsave` 自旋锁与被打断路径共享数据 |

softirq 不会在本 CPU 上嵌套，依据是计数检查：[handle_softirqs()](../../linux/kernel/softirq.c#L579)先经 [softirq_handle_begin()](../../linux/kernel/softirq.c#L461)加上 `SOFTIRQ_OFFSET`，再[打开本地 IRQ](../../linux/kernel/softirq.c#L606)执行回调；这期间 [in_interrupt()](../../linux/include/linux/preempt.h#L143)为真，[do_softirq()](../../linux/kernel/softirq.c#L515)和 [__irq_exit_rcu()](../../linux/kernel/softirq.c#L722-L723)都因此不再启动新一轮处理。`ksoftirqd` 线程调用的是[同一个 handle_softirqs()](../../linux/kernel/softirq.c#L1063)，回调仍然处于 softirq 计数之下，线程身份并不放宽约束。NMI 可以在任意时刻到来，见本地文档 [kernel-stacks.rst](../../linux/Documentation/arch/x86/kernel-stacks.rst#L81)。

**同 CPU 交错为什么会死锁？** 用一把普通自旋锁 `L` 说明：

```text
任务：     spin_lock(L) 成功
            ↓ 本地 IRQ 到达，任务被打断
硬中断：   spin_lock(L) —— 自旋等待 L 释放
            ↓ 硬中断不返回，任务就无法继续
任务：     spin_unlock(L) 永远得不到执行机会
```

这里不需要第二个 CPU。根因是**等待者阻止了持锁者运行**。[__raw_spin_lock()](../../linux/include/linux/spinlock_api_smp.h#L130)只禁止抢占，不关本地 IRQ；[__raw_spin_lock_irqsave()](../../linux/include/linux/spinlock_api_smp.h#L104)在争锁前先保存并关闭本地 IRQ，因此能排除普通硬中断重入，但仍挡不住 NMI。

### 1.3 五种保证

“线程安全”是一个过于笼统的标签。分析同步代码时，应先说清需要哪一种保证：

| 问题 | 需要建立的保证 | 典型手段 |
| --- | --- | --- |
| 互斥 | 同一时刻只有遵守协议的一个持有者进入临界区 | 自旋锁、mutex；不遵守协议的访问者不受保护 |
| 数据一致性 | 读到的多个字段满足不变量，或来自同一次更新 | 加锁；或序列计数（读者校验快照，冲突则重试） |
| 内存顺序 | 观察到同步标志后，一定能观察到它之前发布的数据 | 锁的 acquire/release 语义、成对的发布与读取、内存屏障 |
| 对象生命周期 | 使用指针期间，对象内存一直有效 | 覆盖使用期的锁、引用计数、RCU 延迟回收 |
| 事件等待 | 条件不满足时睡眠，条件变化后被唤醒并重新检查 | 等待队列、completion；被唤醒不等于取得独占权 |

这些机制并不互相替代。例如 [read_seqcount_retry()](../../linux/include/linux/seqlock.h#L394)让读者丢弃冲突快照，但[seqcount_t 的说明](../../linux/include/linux/seqlock_types.h#L10)指出，如果写者让某个指针失效，读者可能在重试前已经沿着这个指针访问了内存，事后重试救不回来。又如 [wait_event()](../../linux/include/linux/wait.h#L345)的核心循环 [___wait_event()](../../linux/include/linux/wait.h#L302)负责入队、检查条件和调度，它并不为业务数据加锁。

### 1.4 全景：锁处在哪一层

下图把本章涉及的机制按依赖关系分层。箭头表示“由……构建”，不是调用顺序。

```mermaid
flowchart BT
    HW["硬件：带 lock 前缀的读改写指令、cli/sti、x86 内存模型"]
    ATOM["原子操作<br/>atomic_t / cmpxchg"]
    ORD["内存顺序接口<br/>barrier / smp_mb / acquire / release"]
    LOCAL["本地上下文控制<br/>preempt / local_irq / local_bh"]
    SPIN["自旋类锁<br/>spinlock_t / raw_spinlock_t / rwlock_t"]
    SLEEP["睡眠类锁<br/>mutex / rw_semaphore / semaphore"]
    OTHER["相邻机制<br/>seqcount / RCU / refcount / completion"]
    USER["子系统的访问协议<br/>“哪个对象的哪些操作必须持哪把锁”"]
    HW --> ATOM & ORD & LOCAL
    ATOM & ORD & LOCAL --> SPIN
    ATOM & ORD & SPIN --> SLEEP
    ATOM & ORD & SPIN --> OTHER
    SPIN & SLEEP & OTHER --> USER
```

两点需要注意。第一，睡眠类锁内部也用自旋锁：mutex 的等待链表由内部的 `wait_lock` 保护（第 2 节）。第二，最上层的“访问协议”永远由使用者定义；锁只提供获取与释放的能力，并不知道自己保护哪些数据。

下表是常用类型的速查，“可睡眠”指调用者所处环境允许调度，并且没有外层自旋锁、关中断等约束。

| 类型 | 竞争时行为 | 适用上下文与限制 | 源码入口 |
| --- | --- | --- | --- |
| `spinlock_t` | 自旋等待，持有期间禁止抢占 | 短、不睡眠的临界区；与 softirq/hardirq 共享时用 `_bh`/`_irqsave` 变体 | [spin_lock()](../../linux/include/linux/spinlock.h#L349) |
| `raw_spinlock_t` | 同上；RT 内核下也保持严格自旋 | 核心底层代码、低层中断处理；名字里的 raw 不代表能用于 NMI | [locktypes.rst#L235](../../linux/Documentation/locking/locktypes.rst#L235) |
| `rwlock_t` | 多读者或单写者，竞争时自旋 | 读区、写区都不能睡眠 | [queued_read_lock()](../../linux/include/asm-generic/qrwlock.h#L78) |
| `struct mutex` | 快路径 → 乐观自旋 → 睡眠 | 可睡眠的进程上下文；不可递归，只能由持有者解锁 | [mutex_lock()](../../linux/kernel/locking/mutex.c#L269) |
| `struct rw_semaphore` | 可睡眠的多读者或单写者 | 可睡眠的进程上下文 | [down_read()](../../linux/kernel/locking/rwsem.c#L1534)、[down_write()](../../linux/kernel/locking/rwsem.c#L1587) |
| `struct semaphore` | 许可耗尽时睡眠，没有 owner | `down()` 需可睡眠；`up()`、`down_trylock()` 可在中断上下文调用 | [semaphore.h#L40](../../linux/include/linux/semaphore.h#L40)、[down()](../../linux/kernel/locking/semaphore.c#L80) |
| `seqcount_t` / `seqlock_t` | 读者不阻塞，冲突时重试 | 写者必须串行；读者不能跟随可能失效的指针 | [write_seqlock()](../../linux/include/linux/seqlock.h#L861) |
| RCU | 读者几乎无开销；回收者等待宽限期 | 不提供写者之间的互斥；读侧不能主动阻塞 | [rcu_read_lock()](../../linux/include/linux/rcupdate.h#L856)、[synchronize_rcu()](../../linux/kernel/rcu/tree.c#L3341) |
| `refcount_t` | 原子计数，不等待 | 递增前调用者必须已保证内存有效 | [refcount_inc_not_zero()](../../linux/include/linux/refcount.h#L320) |
| completion | 未完成时睡眠 | 等待需可睡眠；`complete()` 可在中断或原子上下文调用（[completion.rst#L264](../../linux/Documentation/scheduler/completion.rst#L264)） | [complete()](../../linux/kernel/sched/completion.c#L50)、[wait_for_completion()](../../linux/kernel/sched/completion.c#L151) |

## 2. 核心数据结构

锁机制里有三类容易混淆的东西：**被保护的数据**、**锁自身的状态**、**等待者**。下面先看本章反复出现的结构体，只列与本章有关的字段。

| 结构 | 关键字段 | 含义 |
| --- | --- | --- |
| `atomic_t` | `int counter` | 一个整数的存储位置。“原子”来自操作它的接口，不是给周围对象附加的属性。定义见 [types.h#L181](../../linux/include/linux/types.h#L181) |
| `struct list_head` | `next`、`prev` | 嵌入在业务对象中的链表节点；链表成员关系本身不增加对象引用。见 [types.h#L199](../../linux/include/linux/types.h#L199) |
| `spinlock_t` | `rlock` | 非 RT 下是 `union`，唯一成员是 `struct raw_spinlock rlock`（调试字段未启用）。见 [spinlock_types.h#L17-L29](../../linux/include/linux/spinlock_types.h#L17-L29) |
| `raw_spinlock_t` | `raw_lock` | 嵌入架构锁 `arch_spinlock_t`。见 [spinlock_types_raw.h#L14](../../linux/include/linux/spinlock_types_raw.h#L14) |
| `arch_spinlock_t`（`struct qspinlock`） | `val` / `locked` / `pending` / `tail` | 同一个 32 位锁字的不同视图。见 [qspinlock_types.h#L14-L44](../../linux/include/asm-generic/qspinlock_types.h#L14-L44) |
| `struct mutex` | `owner`、`wait_lock`、`osq`、`wait_list` | 持有者及状态标志、保护等待链表的内部自旋锁、乐观自旋队列、等待者链表。见 [mutex_types.h#L41-L54](../../linux/include/linux/mutex_types.h#L41-L54) |
| `struct mutex_waiter` | `list`、`task` | 等待节点，位于等待任务的内核栈上。见 [mutex.h#L10-L21](../../linux/kernel/locking/mutex.h#L10-L21) |
| `refcount_t` | `atomic_t refs` | 有效引用的数量，饱和而不回绕。见 [refcount_types.h#L7-L17](../../linux/include/linux/refcount_types.h#L7-L17) |
| `struct completion` | `done`、`wait` | 已发生但尚未被消费的完成次数，以及嵌入的简单等待队列。见 [completion.h#L26-L29](../../linux/include/linux/completion.h#L26-L29) |

### 2.1 对象关系

下图中实线表示“嵌入”（包含在同一块内存里），虚线表示“通过链表节点连接”。可以看到三层自旋锁结构都是嵌入关系，没有一个指针指向受保护的数据；mutex 和 completion 各有自己的等待队列，二者互不相干。

```mermaid
flowchart TB
    S[spinlock_t] -->|嵌入 rlock| R[raw_spinlock_t]
    R -->|嵌入 raw_lock| Q["arch_spinlock_t（qspinlock）"]
    M[struct mutex] -->|嵌入 wait_lock| W[raw_spinlock_t]
    M -->|嵌入 osq| OSQ[optimistic_spin_queue]
    M -->|嵌入 wait_list| H[list_head]
    H -.->|连接 waiter.list| T["mutex_waiter（在等待者栈上）"]
    T -->|task 指针| TS[task_struct]
    C[struct completion] -->|嵌入 wait| SW[swait_queue_head]
    SW -->|嵌入 lock| SL[raw_spinlock_t]
```

`swait_queue_head` 的定义见 [swait.h#L43](../../linux/include/linux/swait.h#L43)。

### 2.2 两个“一个字表达多种状态”的设计

**qspinlock 的 32 位锁字。** 当前 `CONFIG_NR_CPUS=512`，小于 16K，位布局采用 [qspinlock_types.h 第 54～59 行](../../linux/include/asm-generic/qspinlock_types.h#L54-L59)的第一种：

```text
 31                    18 17 16 15        9  8  7            0
 +-----------------------+-----+-----------+---+--------------+
 |   tail cpu (+1)       | idx |  未使用    | P |  locked byte |
 +-----------------------+-----+-----------+---+--------------+
   tail：排队队尾的 CPU 与嵌套层级      pending  持有标志
```

`val`、`locked`、`pending`、`tail` 是 `union` 中同一份状态的不同视图（x86 为小端，选用 [第 23 行起的分支](../../linux/include/asm-generic/qspinlock_types.h#L23)），不是几份独立的计数。空闲锁的 `val` 为 0（[__ARCH_SPIN_LOCK_UNLOCKED](../../linux/include/asm-generic/qspinlock_types.h#L49)），无竞争加锁就是把 0 改成 `_Q_LOCKED_VAL`。

**mutex 的 `owner`。** `owner` 是 `atomic_long_t`，存放持有者的 `task_struct` 指针，0 表示未被持有。由于 `task_struct` 至少按缓存行对齐，低 3 位可以借来存标志（[mutex.h 第 23～36 行](../../linux/kernel/locking/mutex.h#L23-L36)）：

| 位 | 宏 | 含义 |
| --- | --- | --- |
| bit 0 | `MUTEX_FLAG_WAITERS` | 等待链表非空，解锁时必须唤醒 |
| bit 1 | `MUTEX_FLAG_HANDOFF` | 解锁时应把锁直接交给队首等待者 |
| bit 2 | `MUTEX_FLAG_PICKUP` | 已完成交接，等待队首来取 |

因此“读到 `owner` 非零”只说明锁被某个任务持有或正在交接，并不表示当前调用者有权访问受保护的数据。

### 2.3 生命周期

**锁对象本身也有生命周期协议。** [spin_lock_init()](../../linux/include/linux/spinlock.h#L341)把锁写成空闲值；[__mutex_init()](../../linux/kernel/locking/mutex.c#L46-L57)把 `owner` 清零，初始化 `wait_lock`、`wait_list` 和 `osq`。初始化必须在锁对其他访问者可见之前完成，不能通过重新初始化一把正在使用的锁来“解除竞争”。锁通常嵌在业务对象里，随对象一起分配和释放。mutex 的[使用约束](../../linux/include/linux/mutex_types.h#L13-L27)明确禁止递归获取、非持有者解锁、释放仍被持有的锁所在的内存，以及在硬中断、软中断上下文中使用。当前配置下 [mutex_destroy() 是空函数](../../linux/include/linux/mutex.h#L48)，它不会等待使用者退出。

**等待节点的生命周期很短。** mutex 慢路径里的 [mutex_waiter 是局部变量](../../linux/kernel/locking/mutex.c#L567)。如果乐观自旋成功，或者入队前再次尝试就拿到了锁，它根本不会进入链表；一旦被 [__mutex_add_waiter()](../../linux/kernel/locking/mutex.c#L190-L200)挂上，函数无论成功还是出错返回前都会把它摘下（见 4.2 节）。链表里的节点因此始终对应一个仍在等待的任务。

**completion 的状态迁移。** [init_completion()](../../linux/include/linux/completion.h#L84-L88)把 `done` 清零。[complete_with_flags()](../../linux/kernel/sched/completion.c#L21-L31)在内部锁下把 `done` 加一（已经是 `UINT_MAX` 时不加），再唤醒一个等待者；[等待路径](../../linux/kernel/sched/completion.c#L107-L108)消费一次 `done`。[complete_all()](../../linux/kernel/sched/completion.c#L79)把 `done` 写成 `UINT_MAX`，表示永久完成，之后的等待者不再消费它。

## 3. 构成锁的底层原语

锁不是一个不可分解的魔法。下面依次看三种原语：原子读改写、内存顺序、本地上下文控制。第 4 节再把它们组装起来。

### 3.1 原子读改写与比较交换

读—改—写（read-modify-write，RMW）指读取旧值、计算新值并写回。x86 上 [arch_atomic_read()](../../linux/arch/x86/include/asm/atomic.h#L17-L24)和 [arch_atomic_set()](../../linux/arch/x86/include/asm/atomic.h#L26-L29)只是一次普通读、一次普通写；[arch_atomic_add()](../../linux/arch/x86/include/asm/atomic.h#L31-L36)和 `arch_atomic_inc()` 才用带锁前缀的指令把 RMW 合成一步。`LOCK_PREFIX` 在 SMP 构建中展开为 `lock` 前缀，并把前缀地址登记到 `.smp_locks` 段，供运行时按处理器数量改写（[alternative.h#L39](../../linux/arch/x86/include/asm/alternative.h#L39)）。

下面两段是**接口用法示意**，不是内核源码：

```c
/* 错误：两次调用之间可以插入另一个执行者。 */
int n = atomic_read(&count);
atomic_set(&count, n + 1);

/* 正确：一次原子读改写。 */
atomic_inc(&count);
```

第二段只解决了单变量问题。如果还要维持“`count` 等于链表节点数”，链表更新和计数更新之间仍需要同一把锁。

不同返回形式适合不同的判断：

| 接口 | 返回值 |
| --- | --- |
| `atomic_add(i, v)`、`atomic_inc(v)` | 无返回值 |
| `atomic_add_return(i, v)` | 更新后的值 |
| `atomic_fetch_add(i, v)` | 更新前的值 |
| `atomic_cmpxchg(v, old, new)` | 操作时读到的值；等于 `old` 才说明替换成功 |
| `atomic_try_cmpxchg(v, &old, new)` | 成功与否；失败时把实际值写回 `old` |

依据见 [atomic_add_return()](../../linux/include/linux/atomic/atomic-instrumented.h#L108)、[atomic_fetch_add()](../../linux/include/linux/atomic/atomic-instrumented.h#L182)、[atomic_cmpxchg()](../../linux/include/linux/atomic/atomic-instrumented.h#L1178)、[atomic_try_cmpxchg()](../../linux/include/linux/atomic/atomic-instrumented.h#L1260)的注释。

**比较交换**（compare-and-swap，CAS）把“如果仍是预期值，就写入新值”合成一步，是几乎所有锁快路径的核心。x86 的 [arch_atomic_cmpxchg()](../../linux/arch/x86/include/asm/atomic.h#L99)最终落到 [cmpxchgl 分支](../../linux/arch/x86/include/asm/cmpxchg.h#L109)，并由 [__cmpxchg](../../linux/arch/x86/include/asm/cmpxchg.h#L133)加上锁前缀。

下面用“扣减有限许可”演示 CAS 循环。这是**简化示意**，假设 `remaining` 发布前已初始化为非负数，所有修改都用原子接口：

```c
/* 示意：只扣减数量，不传递其他数据的所有权。 */
bool take_one(atomic_t *remaining)
{
    int old = atomic_read(remaining);

    for (;;) {
        if (old <= 0)
            return false;
        if (atomic_try_cmpxchg_relaxed(remaining, &old, old - 1))
            return true;
        /* 失败时 old 已更新为当前值，重新检查边界后再试。 */
    }
}
```

两个 CPU 同时读到 1 时，只有一个 CAS 能把 1 改成 0；另一个失败，拿到新的 `old == 0`，随后返回 `false`。循环保证的是竞争下的正确性，不保证有限次内一定成功。本地 [atomic_t.txt](../../linux/Documentation/atomic_t.txt#L302)给出了同样的循环形式。

这里使用 `_relaxed` 是有意的：示意只管数量。如果“拿到许可”还意味着要读取另一个 CPU 准备好的数据，就需要下一节的内存顺序。

### 3.2 `READ_ONCE()` / `WRITE_ONCE()`：约束编译器，不组成临界区

并发分析不能只考虑 CPU，编译器也可能把一次读取缓存在寄存器里复用、合并两次写入，或者重新读取一个值。`READ_ONCE()`、`WRITE_ONCE()` 用 [volatile 读](../../linux/include/asm-generic/rwonce.h#L44)和 [volatile 写](../../linux/include/asm-generic/rwonce.h#L55)禁止这类变换，并在编译期[检查访问宽度](../../linux/include/asm-generic/rwonce.h#L35)。[rwonce.h 开头的注释](../../linux/include/asm-generic/rwonce.h#L3)把它们定位为约束编译器的工具，需要和屏障或原子指令配合使用。

三条边界：

1. `WRITE_ONCE(x, READ_ONCE(x) + 1)` 仍然是一次读加一次写，不是原子 RMW。
2. 分别读两个字段，不会得到同一次更新的快照。
3. 标记一次访问，不会为其他地址建立 acquire、release 或全屏障顺序。

### 3.3 内存顺序

原子性回答“对一个位置的操作是否不可分割”；内存顺序回答“对**多个**位置的访问，别的 CPU 以什么顺序观察到”。这是两个独立的问题。

**屏障与单向语义：**

| 工具 | 保证 | 不能推出 |
| --- | --- | --- |
| `barrier()` | 编译器不能跨越它重排内存访问 | 不产生 CPU 屏障指令 |
| `smp_rmb()` / `smp_wmb()` | 屏障前后的读与读、写与写之间的顺序 | 不约束“先写后读” |
| `smp_mb()` | 屏障两侧所有读写的全序 | 不把多个写变成原子事务 |
| release 操作 | 它**之前**的访问不会被排到它之后 | 不约束它之后的访问 |
| acquire 操作 | 它**之后**的访问不会被排到它之前 | 不约束它之前的访问 |

[barrier()](../../linux/include/linux/compiler.h#L82)是带 `memory` clobber 的空汇编。x86 上 [__smp_mb()](../../linux/arch/x86/include/asm/barrier.h#L53)使用 `lock addl $0`，而 [__smp_rmb()](../../linux/arch/x86/include/asm/barrier.h#L55)和 [__smp_wmb()](../../linux/arch/x86/include/asm/barrier.h#L56)都只是编译器屏障：x86 实现认为普通内存访问在这两种情况下不需要额外的硬件屏障指令。可见“接口强弱”要看契约，不能数汇编里有几条屏障。acquire/release 的单向保证见[内存屏障文档](../../linux/Documentation/memory-barriers.txt#L474)。

**原子接口的顺序契约。** 名字里有 `atomic` 不等于全屏障，规则来自 [atomic_t.txt 的 ORDERING 一节](../../linux/Documentation/atomic_t.txt#L160)：

| 接口类别 | 顺序保证 |
| --- | --- |
| `atomic_read()`、`atomic_set()` | relaxed，不为其他地址排序 |
| 无返回值的 `atomic_add()`、`atomic_inc()` | relaxed |
| 无后缀、带返回值的 RMW，如 `atomic_add_return()` | 完全有序 |
| `_relaxed` / `_acquire` / `_release` 变体 | 按后缀提供 |
| 条件 RMW，如 `atomic_cmpxchg()` | 成功时按接口提供；失败时不保证顺序 |

x86 的 `lock` 指令往往比接口契约更强，但通用代码不能依赖这份额外强度。

**例一：两个 CPU 都读到旧值。** `x`、`y` 初始为 0，CPU 0 先写 `x` 再读 `y`，CPU 1 先写 `y` 再读 `x`。这个模式称为存储缓冲（store buffering）。下图只表示各 CPU 自己的程序顺序，两条线之间没有同步关系：

```mermaid
sequenceDiagram
    participant A as CPU 0
    participant B as CPU 1
    Note over A,B: 初始 x=0，y=0，两侧没有同步
    par
        A->>A: WRITE_ONCE(x, 1)
        A->>A: r0 = READ_ONCE(y)
    and
        B->>B: WRITE_ONCE(y, 1)
        B->>B: r1 = READ_ONCE(x)
    end
    Note over A,B: 允许出现 r0 == 0 且 r1 == 0
```

本地内存模型用例 [SB+poonceonces.litmus](../../linux/tools/memory-model/litmus-tests/SB+poonceonces.litmus#L4)把“双零”标为 Sometimes；两侧都在写和读之间加 `smp_mb()` 后，[SB+fencembonceonces.litmus](../../linux/tools/memory-model/litmus-tests/SB+fencembonceonces.litmus#L4)把它标为 Never。只改一侧不能得到同样结论；用 release 写加 acquire 读也不够，[内存屏障文档](../../linux/Documentation/memory-barriers.txt#L501)说明 release+acquire 对不构成全屏障。

**例二：用 release/acquire 发布数据。** 这是锁交接最核心的顺序模式。下面是**一次性发布的简化代码**，假设只有一个发布者，`payload` 发布后不再修改，`ready` 不复用：

```c
/* CPU 0：发布者 */
WRITE_ONCE(payload, 42);
smp_store_release(&ready, 1);

/* CPU 1：读取者 */
if (smp_load_acquire(&ready) == 1)
    value = READ_ONCE(payload);   /* 一定读到 42 */
```

```mermaid
sequenceDiagram
    participant A as CPU 0（发布者）
    participant B as CPU 1（读取者）
    A->>A: WRITE_ONCE(payload, 42)
    A->>A: smp_store_release(ready, 1)
    A-->>B: acquire 读到这次 release 写入的 1
    B->>B: READ_ONCE(payload) == 42
```

跨 CPU 的虚线表示“acquire 读到了 release 写入的值”，不是函数调用。release 把准备数据的写排在发布之前，acquire 把使用数据的读排在接收之后，读到同一个值把两端连接起来。对应的 [MP+pooncerelease+poacquireonce.litmus](../../linux/tools/memory-model/litmus-tests/MP+pooncerelease+poacquireonce.litmus#L4)把“看到 `ready==1` 却读到旧数据”标为 Never（判定条件在[第 28 行](../../linux/tools/memory-model/litmus-tests/MP+pooncerelease+poacquireonce.litmus#L28)）。

x86 上 [__smp_store_release()](../../linux/arch/x86/include/asm/barrier.h#L59-L64)是 `barrier()` 加 `WRITE_ONCE()`，[__smp_load_acquire()](../../linux/arch/x86/include/asm/barrier.h#L66-L72)是 `READ_ONCE()` 加 `barrier()`，都没有额外的硬件屏障指令。但通用代码中的接口仍然必要：它约束编译器，也让同一份代码在其他架构上成立。

**把这个模式对应到锁上：** 解锁是对锁字的 release 写，加锁成功是对锁字的 acquire 读改写。上一任持有者在临界区里的修改，因此对下一任持有者可见。这正是第 4.1 节要看到的。

### 3.4 本地上下文控制：禁止抢占、关中断、关下半部

这三组操作控制的是**本 CPU 上还有哪些执行活动能插进来**，与跨 CPU 的锁状态是两个维度。

| 操作 | 非 RT 下的效果 | 不解决的问题 |
| --- | --- | --- |
| `preempt_disable()` / `preempt_enable()` | 增减抢占计数，当前任务不会被抢占调度换走 | 不关中断，不阻止其他 CPU |
| `local_irq_save()` / `local_irq_restore()` | 保存并关闭本地可屏蔽中断，结束时恢复原状态 | 不屏蔽 NMI，不阻止其他 CPU |
| `local_bh_disable()` / `local_bh_enable()` | 本 CPU 的 softirq 处理不会在区间内运行，区间也不可抢占 | 不关硬中断，不阻止其他 CPU 的 softirq |

[locktypes.rst](../../linux/Documentation/locking/locktypes.rst#L58-L62)把这些称为纯 CPU 本地的并发控制，不适合 CPU 之间的同步。它们能保护 per-CPU 数据，前提是先确定访问者只在本 CPU 上。

实现要点：

- [preempt_disable()](../../linux/include/linux/preempt.h#L213-L217)增加计数后加编译器屏障。[preempt_enable()](../../linux/include/linux/preempt.h#L230-L235)减计数，减到零时调用 `__preempt_schedule()`。
- x86 原生的 [native_local_irq_save()](../../linux/arch/x86/include/asm/irqflags.h#L62)先保存标志再执行 `cli`。当前启用了 `CONFIG_PARAVIRT_XXL`，实际调用的 [arch_local_irq_save()](../../linux/arch/x86/include/asm/paravirt.h#L674)经过半虚拟化分派。`flags` 是这次调用前的中断状态，必须用 `restore` 恢复，不能用无条件的 `local_irq_enable()` 代替，否则会打开调用者原本关闭的中断。
- [local_bh_disable()](../../linux/include/linux/bottom_half.h#L18-L21)增加的是 [SOFTIRQ_DISABLE_OFFSET](../../linux/include/linux/preempt.h#L55)（即 `2 * SOFTIRQ_OFFSET`），与真正执行 softirq 时的 `SOFTIRQ_OFFSET` 不同，所以 [interrupt_context_level()](../../linux/include/linux/preempt.h#L97)不会把“仅仅关了下半部”当成 softirq 上下文。[__local_bh_enable_ip()](../../linux/kernel/softirq.c#L427)在重新打开时会检查并处理积压的 softirq。

**动态抢占的影响。** `CONFIG_PREEMPT_DYNAMIC` 让内核按 `CONFIG_PREEMPTION=y` 构建，但抢占模型在启动时才确定（[preempt.h#L509-L516](../../linux/include/linux/preempt.h#L509-L516)）。x86 的 [__preempt_schedule()](../../linux/arch/x86/include/asm/preempt.h#L125-L129)是一个 static call；[setup_preempt_mode()](../../linux/kernel/sched/core.c#L7776)解析 `preempt=` 启动参数，没有该参数时 [preempt_dynamic_init()](../../linux/kernel/sched/core.c#L7794-L7795)按 `CONFIG_PREEMPT_VOLUNTARY` 选择自愿模型。按[模型对照注释](../../linux/kernel/sched/core.c#L7636-L7640)和 [__sched_dynamic_update()](../../linux/kernel/sched/core.c#L7732-L7737)，自愿模型把 `preempt_schedule` 改成 NOP。

结论是：在缺省启动参数下，`preempt_enable()` 里的 `__preempt_schedule()` 调用点存在但不会进入调度；以 `preempt=full` 启动时它才是真正的抢占点。无论哪种模型，抢占计数都在维护，持有自旋锁期间都不会被抢占换出。禁止抢占也不是主动睡眠的许可，可睡眠锁的[调用上下文要求](../../linux/Documentation/locking/locktypes.rst#L26)依然有效。

## 4. 把原语组装成锁

### 4.1 `spin_lock_irqsave()`：先封住本 CPU，再参与跨 CPU 竞争

设想一个小队列同时被任务和硬中断访问，更新不能睡眠。任务端使用 `spin_lock_irqsave()`，目标有两个：在本 CPU 上排除硬中断重入，在 CPU 之间取得独占权。

**算法（简化逻辑，省略 lockdep 和跟踪钩子）：**

```text
加锁：
    flags = 保存当前中断状态并关闭本地 IRQ      ← 封住本 CPU 的硬中断
    抢占计数 +1                                   ← 封住本 CPU 的抢占
    CAS(锁字: 0 → LOCKED)，带 acquire 语义        ← 跨 CPU 竞争的快路径
    若失败：进入排队慢路径，直到取得锁才返回

解锁：
    store_release(locked 字节, 0)                 ← 把临界区修改交给下一任
    按 flags 恢复本地 IRQ
    抢占计数 -1，必要时允许调度
```

注意顺序：本地上下文控制先于争锁，恢复则晚于释放。若反过来，先拿锁再关中断，中间就会出现第 1.2 节的自死锁窗口。

**调用链。** 下图从外到内列出当前配置下的实际层次，每一层只做一件事：

```mermaid
flowchart TD
    A["spin_lock_irqsave(lock, flags)<br/>spinlock.h#L379：取出 rlock"] --> B["raw_spin_lock_irqsave()<br/>spinlock.h#L241：flags = 返回值"]
    B --> C["_raw_spin_lock_irqsave()<br/>spinlock.c#L160：非内联函数"]
    C --> D["__raw_spin_lock_irqsave()<br/>spinlock_api_smp.h#L104：关 IRQ、关抢占"]
    D --> E["do_raw_spin_lock()<br/>spinlock.h#L184"]
    E --> F["arch_spin_lock() = queued_spin_lock()<br/>qspinlock.h#L107：CAS 快路径"]
    F -->|CAS 失败| G["queued_spin_lock_slowpath()<br/>x86 经 pv 分派"]
```

各层依据：

- [spin_lock_irqsave()](../../linux/include/linux/spinlock.h#L379-L382)把 `spinlock_t` 转成内嵌的 `raw_spinlock_t`，[raw_spin_lock_irqsave()](../../linux/include/linux/spinlock.h#L241-L245)把返回的中断状态写回调用者的 `flags`。
- 当前未设置 `CONFIG_INLINE_SPIN_LOCK_IRQSAVE`，所以 [spinlock_api_smp.h#L58-L60](../../linux/include/linux/spinlock_api_smp.h#L58-L60)的内联映射不生效，调用的是 [kernel/locking/spinlock.c#L160](../../linux/kernel/locking/spinlock.c#L160)中的非内联函数，它再调用 `__raw_spin_lock_irqsave()`。
- [__raw_spin_lock_irqsave()](../../linux/include/linux/spinlock_api_smp.h#L104-L113)依次执行 `local_irq_save()`、`preempt_disable()` 和 `LOCK_CONTENDED()`。未启用 `CONFIG_LOCK_STAT` 时，[LOCK_CONTENDED()](../../linux/include/linux/lockdep.h#L467-L468)直接展开为 `do_raw_spin_lock(lock)`。
- [do_raw_spin_lock()](../../linux/include/linux/spinlock.h#L184-L189)调用 `arch_spin_lock()`；x86 的 [spinlock.h](../../linux/arch/x86/include/asm/spinlock.h#L27)包含 qspinlock 头文件，[通用映射](../../linux/include/asm-generic/qspinlock.h#L147)把它定义为 `queued_spin_lock()`。

qspinlock 的快路径只有一次 CAS：

```c
int val = 0;

if (likely(atomic_try_cmpxchg_acquire(&lock->val, &val, _Q_LOCKED_VAL)))
	return;

queued_spin_lock_slowpath(lock, val);
```

来源：[queued_spin_lock()，include/asm-generic/qspinlock.h 第 109～114 行](../../linux/include/asm-generic/qspinlock.h#L109-L114)。

这里同时用上了第 3 节的两种原语：CAS 保证多个竞争者中只有一个能把锁字从 0 改成“已持有”；`_acquire` 保证临界区内的访问不会被排到取锁之前。CAS 失败时 `val` 带回当前锁字，交给慢路径排队；调用者不会收到“加锁失败”，函数返回时一定已经持有锁。当前配置下 [queued_spin_lock_slowpath()](../../linux/arch/x86/include/asm/qspinlock.h#L49-L52)经过 `pv_queued_spin_lock_slowpath()` 分派，排队算法留到 qspinlock 章节展开。

**解锁。** [__raw_spin_unlock_irqrestore()](../../linux/include/linux/spinlock_api_smp.h#L146-L153)按“释放锁 → 恢复中断 → 恢复抢占”的顺序执行。原生解锁 [native_queued_spin_unlock()](../../linux/arch/x86/include/asm/qspinlock.h#L44-L47)只是对 `locked` 字节做一次 `smp_store_release(..., 0)`；当前配置下 [queued_spin_unlock()](../../linux/arch/x86/include/asm/qspinlock.h#L54-L58)经过 `pv_queued_spin_unlock()` 分派，在非虚拟化环境中最终执行原生版本这一点属于运行时条件，本章不作断言。

**这份保证的边界。** 只有当另一个 CPU 随后取得**同一把锁**时，它才能借助 release→acquire 看到上一任临界区内的修改。完全不加锁的读者，或者持有另一把无关锁的读者，得不到这份保证。[内存屏障文档](../../linux/Documentation/memory-barriers.txt#L1993)专门说明了锁的 acquire/release 不等于“锁前锁后都是全屏障”。

**其他变体处理的是不同的本地访问者：**

| 接口 | 加锁前对本 CPU 做什么 | 适用场景 |
| --- | --- | --- |
| `spin_lock()` | 只禁止抢占（[__raw_spin_lock()](../../linux/include/linux/spinlock_api_smp.h#L130-L135)） | 所有访问者都在进程上下文 |
| `spin_lock_bh()` | 关下半部（[__raw_spin_lock_bh()](../../linux/include/linux/spinlock_api_smp.h#L123-L128)） | 与 softirq 共享 |
| `spin_lock_irqsave()` | 保存并关闭本地 IRQ，再禁止抢占 | 与硬中断共享，且不确定调用时中断是否已关 |
| `spin_trylock()` | 禁止抢占后只尝试一次；失败则恢复抢占并返回 0（[__raw_spin_trylock()](../../linux/include/linux/spinlock_api_smp.h#L86-L95)） | 不允许等待，调用者必须处理失败 |

### 4.2 `mutex_lock()`：两层保护

如果临界区里有可能睡眠的操作（分配内存、等待 I/O），就不能持有自旋锁跨越它。mutex 把两件事分开：

- **对业务数据的长期独占**：由 `owner` 字段表示，可以持有任意长时间，持有者可以睡眠。
- **对等待链表的短时修改**：由内部自旋锁 `wait_lock` 保护，持有时间很短，不会睡眠。

**快路径。** 当前未启用 `CONFIG_DEBUG_LOCK_ALLOC`，采用[独立快路径分支](../../linux/kernel/locking/mutex.c#L239)：

```c
void __sched mutex_lock(struct mutex *lock)
{
	might_sleep();

	if (!__mutex_trylock_fast(lock))
		__mutex_lock_slowpath(lock);
}
```

来源：[mutex_lock()，kernel/locking/mutex.c 第 269～275 行](../../linux/kernel/locking/mutex.c#L269-L275)。

[__mutex_trylock_fast()](../../linux/kernel/locking/mutex.c#L150-L161)用 `atomic_long_try_cmpxchg_acquire()` 把 `owner` 从 0 改成 `current`。这与 qspinlock 的快路径同构：一次带 acquire 语义的 CAS。无竞争时 mutex 不会睡眠，但 `might_sleep()` 说明调用者**必须**处于可睡眠的上下文，不能因为“这次没睡”就在原子上下文里调用它。

**慢路径。** [__mutex_lock_slowpath()](../../linux/kernel/locking/mutex.c#L1046-L1050)以 `TASK_UNINTERRUPTIBLE` 进入 `__mutex_lock_common()`。主要步骤与数据结构的对应关系如下：

```mermaid
flowchart TD
    S([进入慢路径]) --> A["preempt_disable()<br/>再试一次，或乐观自旋"]
    A -->|成功| OK1([preempt_enable，返回 0])
    A -->|失败| B["持 wait_lock 后再试一次"]
    B -->|成功| OK2([释放 wait_lock，返回 0])
    B -->|失败| C["局部 waiter 挂到 wait_list 尾部<br/>设置 MUTEX_FLAG_WAITERS<br/>设置任务状态"]
    C --> D{"trylock 成功？"}
    D -->|是| E["摘除 waiter，恢复 RUNNING<br/>释放 wait_lock，返回 0"]
    D -->|否| F{"有待处理信号？<br/>（仅可中断模式）"}
    F -->|是| ERR["摘除 waiter，恢复 RUNNING<br/>释放 wait_lock，返回 -EINTR"]
    F -->|否| G["释放 wait_lock → schedule()"]
    G --> H["被唤醒：trylock 或接受交接<br/>队首可再做乐观自旋"]
    H -->|成功| E
    H -->|失败| I["重新取 wait_lock"] --> D
```

对应源码：

1. [第 597～610 行](../../linux/kernel/locking/mutex.c#L597-L610)：关抢占后先 `__mutex_trylock()`，再尝试 `mutex_optimistic_spin()`。乐观自旋的依据是“持有者正在别的 CPU 上运行，很快会释放”，成功就不必入队。
2. [第 612～621 行](../../linux/kernel/locking/mutex.c#L612-L621)：取得 `wait_lock` 后再试一次。锁可能在等 `wait_lock` 的过程中被释放。
3. [第 630～644 行](../../linux/kernel/locking/mutex.c#L630-L644)：把栈上的 `waiter` 挂到 `wait_list` 尾部（FIFO）。[__mutex_add_waiter()](../../linux/kernel/locking/mutex.c#L197-L199)在它成为第一个等待者时设置 `MUTEX_FLAG_WAITERS`，告诉解锁者“必须走唤醒路径”。随后设置任务状态。
4. [第 646～676 行](../../linux/kernel/locking/mutex.c#L646-L676)：循环中先 trylock，再在持有 `wait_lock` 的情况下[检查信号](../../linux/kernel/locking/mutex.c#L663-L666)，然后[先释放 `wait_lock` 再调度](../../linux/kernel/locking/mutex.c#L674-L676)。持有内部自旋锁睡眠是不允许的，而且解锁者需要拿到 `wait_lock` 才能处理队列。
5. [第 692～707 行](../../linux/kernel/locking/mutex.c#L692-L707)：醒来后用 `__mutex_trylock_or_handoff()` 取锁或接受交接；位于队首的等待者还可以再次乐观自旋。
6. 成功出口 [第 712～740 行](../../linux/kernel/locking/mutex.c#L712-L740)和出错出口 [第 742～753 行](../../linux/kernel/locking/mutex.c#L742-L753)都会把 `waiter` 从链表[摘除](../../linux/kernel/locking/mutex.c#L726)、恢复 `TASK_RUNNING`、释放 `wait_lock` 并恢复抢占。

第 6 步维护了第 2.3 节提到的不变量：`wait_list` 中的节点总是对应一个仍在等待、栈帧仍然有效的任务。如果出错路径漏掉摘除，链表里就会留下指向已退出栈帧的节点。

**返回值。** 普通 `mutex_lock()` 以不可中断方式等待，不会因为信号而失败，所以它没有返回值。[mutex_lock_interruptible()](../../linux/kernel/locking/mutex.c#L991-L999)成功返回 0，被信号打断返回 `-EINTR`；返回 `-EINTR` 时调用者没有持有锁，不能进入临界区。

**解锁快路径**是对称的：[__mutex_unlock_fast()](../../linux/kernel/locking/mutex.c#L163-L168)用 `atomic_long_try_cmpxchg_release()` 把 `owner` 从 `current` 改回 0。只要 `owner` 带有任何标志位（例如有等待者），CAS 就会失败，转入需要唤醒或交接的慢路径。这部分留给 mutex 章节展开。

### 4.3 解锁之后还要用对象：生命周期协议

回到第 1.1 节的场景三。锁只保护“查找”和“摘除”这两个动作，要在锁外继续使用对象，就必须在锁内取得对象的引用。下面是**锁加引用计数的协议示意**，不对应某个具体子系统：

| 阶段 | 必须维持的关系 |
| --- | --- |
| 创建与发布 | 对象在共享前初始化字段和引用计数；集合持有一份引用，在集合锁下插入 |
| 查找 | 持集合锁查找，找到后**在锁内**取得自己的引用，然后才解锁 |
| 锁外使用 | 集合可以变化，但自己的引用保证对象存活；可变字段仍按各自协议访问 |
| 摘除 | 持集合锁摘除，之后的查找再也找不到它；解锁后归还集合那份引用 |
| 最终释放 | 每个使用者结束后归还引用；只有让计数归零的那一方执行释放 |

[refcount_t](../../linux/include/linux/refcount_types.h#L7-L17)在计数饱和时停止变化，避免回绕导致错误释放。[refcount_inc_not_zero()](../../linux/include/linux/refcount.h#L320-L336)的注释明确要求调用者“已经保证对象内存稳定”，例如处在 RCU 读侧或持有集合锁；对一个可能已经释放的指针做递增，已经太晚了。[refcount_dec_and_test()](../../linux/include/linux/refcount.h#L435-L451)在递减时提供 release 语义，在归零时提供 acquire 语义，使释放操作排在所有使用者的最后一次访问之后。它们只做计数，不会替你调用释放函数。

**RCU 是另一条路。** 写者先让新读者无法再找到旧对象，再等待所有可能持有旧指针的读者离开读侧临界区，然后回收。[synchronize_rcu()](../../linux/kernel/rcu/tree.c#L3303)同步等待一个宽限期，[call_rcu()](../../linux/kernel/rcu/tree.c#L3241)把回收交给回调。它等待的只是“已经存在的读者”，[新读者可以与回调并行运行](../../linux/include/linux/rcupdate.h#L833-L843)。

当前配置的 RCU 读侧有一处容易误解的地方：[__rcu_read_lock()](../../linux/kernel/rcu/tree_plugin.h#L412-L420)只增加任务自己的读侧嵌套计数，不增加 `preempt_count`。因此[读侧可以被抢占，但不能主动阻塞](../../linux/include/linux/rcupdate.h#L856-L858)。自愿抢占模型下，读侧代码走到 [__cond_resched()](../../linux/kernel/sched/core.c#L7493-L7498)时仍可能经 [preempt_schedule_common()](../../linux/kernel/sched/core.c#L7127)以 [SM_PREEMPT](../../linux/kernel/sched/core.c#L7145)换出；[rcu_note_context_switch()](../../linux/kernel/rcu/tree_plugin.h#L332)会把这次换出当作抢占处理，宽限期等到该任务离开读区才结束。直接调用 `schedule()` 或在读区内调用 `synchronize_rcu()` 则属于主动阻塞，超出约定。

**completion 也不会自动结束对象生命周期。** 等待者因超时或信号提前返回时，等待路径会经 [__finish_swait()](../../linux/kernel/sched/completion.c#L103)把自己的节点摘下，但不会取消负责调用 `complete()` 的异步工作。本地 [completion 文档](../../linux/Documentation/scheduler/completion.rst#L78)要求 completion 所在内存必须覆盖双方的最后一次使用，包括等待方提前返回之后仍可能到来的 `complete()`。

## 5. 回顾

本章从三个场景出发：共享计数说明单变量更新需要原子性，链表说明多字段关系需要统一的锁，对象释放说明裸指针不能代替有效引用。访问者既可能来自其他 CPU，也可能来自本 CPU 上的抢占、softirq、hardirq 和 NMI，后者决定了持锁者是否还能继续运行。

锁由三种原语组成：

| 原语 | 在锁中的作用 | 本章的例子 |
| --- | --- | --- |
| 原子 CAS | 让多个竞争者中只有一个完成“空闲 → 持有”的状态迁移 | `queued_spin_lock()` 改 `val`，`__mutex_trylock_fast()` 改 `owner` |
| acquire/release | 让上一任持有者的修改对下一任可见 | qspinlock 的 `smp_store_release(&lock->locked, 0)`，mutex 的 `try_cmpxchg_release` |
| 本地上下文控制 | 防止本 CPU 上的打断者与持锁者互相等待 | `spin_lock_irqsave()` 先关 IRQ、关抢占再争锁 |

在此之上，自旋锁直接组合三者；mutex 用 `owner` 表示长期独占，用内部 `wait_lock` 保护等待链表；引用计数、RCU 和 completion 分别补上生命周期与事件等待的需求。

读一段同步代码时，可以依次回答五个问题：

1. **保护什么？** 是一个字段、一组字段之间的关系，还是对象的存活期？
2. **访问者是谁？** 是否跨 CPU，是否会在本 CPU 上重入，能否睡眠？
3. **资格如何取得？** 加锁成功、CAS 成功、读到发布标志、引用递增各意味着什么？失败时还能不能访问？
4. **顺序如何建立？** 双方通过哪一把锁或哪一个变量联系起来，用的是什么 acquire/release 或屏障？
5. **资格如何交还？** 解锁、摘除等待节点、归还引用、延迟回收各发生在哪里？

后续章节深入 qspinlock 排队、mutex 交接、rwsem、RCU 宽限期时，都可以用这五个问题检查对状态变化的理解。
