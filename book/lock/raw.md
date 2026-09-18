# Linux 锁与同步机制：总览及源码分析

分析基线：本地源码 `/Users/jinqinghui/linux-6.18`，`Makefile` 版本 **6.18.52**，Git 提交 **8f3741e6feb0**。源码树没有 `.config`，因此下文分析的是源码中可选择的实现，不能据此断言某个运行内核启用了哪些路径。默认讨论 SMP、非 `PREEMPT_RT`；RT、UP、架构相关差异另行说明。

覆盖范围：通用锁原语、构成锁的底层算法、常用无锁同步、等待与生命周期机制，以及主要专用锁的源码入口。“所有”按机制分类，不逐个枚举驱动、文件系统中成千上万个锁对象，也不把同一种锁的每个包装函数算作新算法。自旋锁、mutex、rwsem、序列锁、RCU、futex 展开核心路径；子系统专用锁提供实现概要和继续阅读入口。

以下代码段均为标明用途的简化示意，不是完整源码摘录。链接指向本地实际文件及对应行；若后续切换提交，行号可能变化。未修改内核实现，未编译或运行内核。

## 1. 先区分四类问题

| 问题 | 需要的保证 | 主要机制 |
|---|---|---|
| 谁可以同时操作共享状态？ | 互斥或读写排斥 | spinlock、mutex、rwlock、rwsem、rt_mutex、ww_mutex |
| 读到的一组字段是否属于同一次更新？ | 一致快照；冲突时重试 | seqcount、seqlock、seqcount_latch |
| 读者使用对象时，对象会不会被释放？ | 生命周期保护、延迟回收 | RCU、SRCU、refcount、kref、percpu_ref |
| 条件何时满足，等待者如何不丢事件？ | 等待、通知和可见性协议 | waitqueue、completion、futex、wait_bit |

原子操作与内存屏障是上述机制的基础。它们本身通常不提供一整个临界区的互斥。`local_irq_disable()`、`preempt_disable()` 则限制本 CPU 的执行上下文，不提供跨 CPU 互斥。

RCU、completion、引用计数经常被放进“锁机制”一起学习，但不能用它们直接替换 mutex：解决的问题不同。

## 2. 全景对照表

表中“阻塞”指等待时可能进入调度器；“临界区可睡眠”仍以调用者没有持有其他原子上下文约束为前提。

| 机制 | 竞争时的主要行为 | 关键保证 | 典型用途 | 核心实现 |
|---|---|---|---|---|
| `raw_spinlock_t` | 忙等 | 独占；持有时禁止抢占 | 调度器、底层中断、极短临界区 | [spinlock API][spin-api]、[qspinlock][qspin-slow] |
| `spinlock_t` | 非 RT 忙等；RT 可阻塞 | 独占，RT 下仍有使用限制 | 普通内核共享状态 | [接口映射][spin-wrap]、[RT 实现][spin-rt] |
| `rwlock_t` | 非 RT 忙等；RT 可阻塞 | 多读单写 | 很短的读多写少临界区 | [qrwlock][qrw-fast]、[RT 读写基础][rwbase-rt] |
| ticket / MCS / qspinlock | 排队自旋 | 为上层锁实现互斥和排队 | 架构锁、通用自旋锁内部 | [ticket][ticket]、[MCS][mcs]、[qspinlock][qspin-slow] |
| OSQ | 可退出的排队自旋 | 控制乐观自旋者数量 | mutex / rwsem 内部优化 | [osq_lock][osq] |
| bit spinlock | 忙等 | 一个 bit 表示独占 | 极度节省锁空间 | [bit_spin_lock][bit-spin] |
| `mutex` | 快路径→可选自旋→睡眠 | 有 owner 的任务级独占 | 可睡眠的普通临界区 | [mutex.c][mutex-common] |
| `rt_mutex` | 按优先级等条件排队、阻塞 | 独占与优先级继承 PI | RT 锁、PI futex | [rtmutex.c][rt-block] |
| `ww_mutex` | 冲突时等待或回退重试 | 一批动态对象的死锁避免协议 | GPU / DRM 多缓冲区操作 | [ww_mutex.h][ww-alg] |
| `semaphore` | 计数不足则睡眠 | N 个许可，无严格 owner | 资源数量限制、遗留接口 | [semaphore.c][sem-down] |
| `rw_semaphore` | 快路径、可选自旋、睡眠 | 可睡眠的多读单写 | 地址空间、文件系统结构 | [rwsem.c][rwsem-count] |
| `percpu_rw_semaphore` | 读快路径分散计数；写者等待 | 多读单写，降低读侧共享写入 | 读极多、写很少 | [percpu-rwsem.c][percpu-write] |
| `local_lock_t` | 非 RT 限制本地上下文；RT 用每 CPU 锁 | 当前 CPU 保护域内的序列化 | per-CPU 数据 | [local_lock_internal.h][local-lock] |
| `seqcount_t` | 读者校验并重试 | 多字段一致性，写者需外部串行化 | 小型统计、状态快照 | [seqlock.h][seq-begin] |
| `seqlock_t` | 写者加锁；读者通常重试 | 自带写侧 spinlock 的 seqcount | 读多写少且更新很短 | [write_seqlock][seqlock-write] |
| `seqcount_latch_t` | 双副本、读者校验 | 读者可打断写者的快照协议 | 时间维护、特定中断/NMI 读取 | [latch][seq-latch] |
| RCU / SRCU | 读侧记录；回收方等宽限期 | 生命周期、发布、延迟回收 | 查找表、链表、配置对象 | [RCU API][rcu-read]、[SRCU][srcu-read] |
| atomic / refcount / lockref | 原子读改写，必要时退回锁 | 单变量更新或引用生命周期 | 计数、状态机、对象引用 | [atomic 规则][atomic-doc]、[lockref][lockref] |
| waitqueue / completion | 条件未满足则睡眠 | 不丢失等待事件的协议 | 驱动事件、线程协作 | [waitqueue][wait-prepare]、[completion][complete] |
| futex | 用户态原子快路径；内核阻塞/唤醒 | 用户态同步的等待基础 | pthread mutex 等库实现的基础 | [waitwake.c][futex-protocol] |
| folio / buffer 位锁 | 位操作快路径，竞争可睡眠 | 页缓存/I/O 相关状态的独占 | 页缓存、buffer I/O | [folio_lock][folio-lock]、[buffer][buffer-lock] |
| hwspinlock | 硬件 trylock 与轮询/超时 | 跨异构处理器硬件仲裁 | CPU 与远端处理器共享资源 | [hwspinlock_core.c][hwspin] |
| 文件锁 / DLM | 冲突检测、等待或远端协议 | 文件范围或集群资源的逻辑锁定 | `flock`、POSIX/OFD 锁、集群 FS | [fs/locks.c][file-lock]、[DLM][dlm] |

## 3. 分析锁之前，先画出并发关系

### 3.1 任务、软中断、硬中断、NMI 与其他 CPU

同一数据可能被进程上下文、softirq、hardirq、NMI 或其他 CPU 访问。要分别问两个问题：有没有跨 CPU 竞争？有没有同一 CPU 上的重入？

例如普通内核中：任务持有 `spin_lock(&L)`，随后本 CPU 硬中断进入并尝试获取 `L`。中断一直自旋，任务没有机会执行解锁，形成自死锁。任务侧必须按真实访问上下文选择 `_irqsave()` 等接口。

| 操作 | 非 RT 下的效果 | 不提供的保证 |
|---|---|---|
| `preempt_disable()` | 阻止当前任务被普通内核抢占 | 不阻止 IRQ/NMI，也不排斥其他 CPU |
| `migrate_disable()` | 任务固定在当前 CPU，可被抢占 | 不排斥本 CPU 的其他任务 |
| `local_irq_disable()` | 关闭本 CPU 可屏蔽中断 | 不关闭其他 CPU 中断，不屏蔽 NMI |
| `local_bh_disable()` | 禁止本地 softirq 执行；非 RT 也影响抢占 | 不屏蔽硬中断和 NMI |
| `spin_lock()` | 跨 CPU 独占，并禁止抢占 | 默认不关闭本地硬中断 |
| `spin_lock_bh()` | 自旋锁与本地 BH 禁止的组合 | 不屏蔽硬中断 |
| `spin_lock_irqsave()` | 保存 IRQ 状态、关 IRQ、获取锁 | 不屏蔽 NMI；RT 下 IRQ 语义不同 |

`_irq()` 与 `_irqsave()` 的差别是退出时启用 IRQ，还是恢复进入前的 IRQ 状态。不能拿一份 `flags` 覆盖多个嵌套保存结果。IRQ-safe 不等于 NMI-safe；NMI 访问同一把被打断上下文持有的锁仍会死锁。

### 3.2 锁同时承担内存顺序

正确交接同一把锁时，前任持有者在解锁前的写入，要对后任获取锁后的访问可见：

```text
CPU A                              CPU B
lock(L)
data = new_value
unlock(L) -- release / acquire --> lock(L)
                                   use(data)
                                   unlock(L)
```

图中假设 B 在 A 之后成功取得 L；关键关系是 `A:unlock(L)` 与 `B:lock(L)` 的交接。

- acquire：阻止临界区内后续访问越过加锁向前移动。
- release：阻止临界区内先前访问越过解锁向后移动。
- 不能由此推导任意 `unlock(); lock();` 都是完整的 `smp_mb()`；额外顺序要求必须按算法使用对应屏障。
- `READ_ONCE()` / `WRITE_ONCE()` 约束编译器和访问形式，不等价于 acquire / release，更不等价于互斥。
- 一个 CPU 加锁、另一个 CPU 裸访问，并不会自动安全；双方必须遵守同一同步协议。

依据：[内存屏障定义][barriers]、[qspinlock acquire/release][qspin-fast]。MMIO、DMA 的排序还必须遵循设备 API，不能把普通内存锁的语义无限外推。

## 4. 自旋锁：API 包装、qspinlock、MCS、ticket

### 4.1 从 spin_lock 一直走到底层

非 RT、SMP 情形的概念调用链：

```text
spin_lock(lock)
  → raw_spin_lock(&lock->rlock)
    → _raw_spin_lock / __raw_spin_lock
      → preempt_disable + lockdep 注解
      → do_raw_spin_lock / 架构获取操作
        → arch_spin_lock
          → queued_spin_lock         （选择 qspinlock 的架构）
```

内联和调试配置会改变中间跳转，但层次不变：`spinlock_t` 包装 → raw 接口 → 架构锁算法。入口：[spin_lock][spin-wrap]、[__raw_spin_lock][spin-api]。

`raw_spinlock_t` 在 RT 下仍忙等，适用于不能睡眠的底层临界区。持锁时不能调用可能睡眠的函数。普通 `spinlock_t` 的 RT 行为见第 7 节；“RT 获取时允许阻塞”不代表持有它时可以随意调用睡眠函数。

### 4.2 qspinlock 用 32 位状态表达三种竞争程度

结构位于 [qspinlock_types.h][qspin-type]。常见 `CONFIG_NR_CPUS < 16384` 配置：

```text
31                  18 17 16 15       9 8 7             0
+---------------------+-----+-----------+-+---------------+
| tail CPU（CPU+1）    | idx | reserved  |P| locked byte   |
+---------------------+-----+-----------+-+---------------+
```

高 CPU 数配置会扩大 tail 的有效范围，不能无条件套用上述位布局。tail 包含 CPU 编号和本 CPU 队列节点编号；它不是一个直接存储的指针。

**无竞争快路径**：[queued_spin_lock][qspin-fast] 用 `atomic_try_cmpxchg_acquire()` 将整个状态从 0 改为 `_Q_LOCKED_VAL`，成功即获得锁。

**轻竞争 pending 路径**：[queued_spin_lock_slowpath][qspin-slow] 在状态允许时设置 pending，等 `locked` 清零，再完成 pending → locked 的转换。这样一个等待者不一定要构造完整排队关系。

**重竞争队列路径**：

1. 从本 CPU 的 `qnodes` 选节点，初始化 `locked`、`next`。
2. 通过 `xchg_tail()` 发布自身，取得前驱的编码。
3. 存在前驱时，把自己接到 `prev->next`，在自己的节点状态上等候。
4. 到达队首后，等待锁状态中的 locked/pending 清空，再获取实际锁。
5. 若有后继，通知后继成为新的队首；当前节点可以归还，即使当前 CPU 还在锁保护的临界区里。

第 5 点特别关键：**MCS 队列通行权不等于目标锁已经释放**。后继成为队首后，仍须等待目标锁的 `locked` 清零。

**解锁**：[queued_spin_unlock][qspin-unlock] 用 `smp_store_release(&lock->locked, 0)` 清除 locked 字节。普通原生路径不需要通过调度器唤醒任务；等待者仍在执行等待循环。

qspinlock 的价值是小体积与较低的重竞争缓存流量，不保证所有路径严格 FIFO。pending、乐观重试、节点不足回退、虚拟化锁窃取等均使真实行为比简单 FIFO 模型更复杂。

### 4.3 MCS、ticket、OSQ 的区别

| 算法 | 核心状态/动作 | 扩展性与代价 |
|---|---|---|
| ticket lock | 原子取号 `next++`，等待 `owner == ticket`；解锁推进 owner | 次序直观，但大家观察同一 owner，竞争时共享缓存行压力高 |
| MCS lock | 原子交换尾指针，链接前驱，每个等待者等自己的节点 | 减少所有等待者集中轮询同一缓存行；需要管理节点 |
| qspinlock | 紧凑锁字、pending、MCS 风格队列组合 | 无竞争路径短；重竞争排队；队列节点与实际锁所有权分离 |
| OSQ | MCS 风格双向关系，允许等待者退出 | 用于睡眠锁的自旋阶段，遇到调度需求可撤销排队 |

源码：[ticket_spin_lock][ticket]、[mcs_spin_lock][mcs]、[osq_lock][osq]。

OSQ 只控制乐观自旋资格，拿到 OSQ 并不等于拿到 mutex。常规自旋者经 OSQ 限流，但 mutex 等待队列头也可直接自旋，因此不能简单说“任何时刻只有一个 CPU 观察 mutex owner”。

### 4.4 架构、UP 和虚拟化边界

本树 [x86][x86-spin] 与 [arm64][arm64-spin] 的相关入口包含 qspinlock/qrwlock。其他架构可能选 ticket 或专用实现。x86 文件顶部仍有 ticket 的历史说明，判断当前实现应沿实际 include 和宏映射追踪。

UP 非调试配置可以省掉部分跨 CPU 原子互斥，但抢占、IRQ、编译器顺序等要求仍存在，不能把锁 API 当作完全无作用。PV qspinlock 还会处理虚拟 CPU 被宿主调度出去的情况，不能把原生自旋时序原样套用到虚拟化路径。

## 5. 位自旋锁：节省空间的代价

[bit_spin_lock][bit-spin] 的核心是：禁止抢占 → `test_and_set_bit_lock()` → 成功后进入临界区；失败时先恢复抢占，轮询该 bit，准备重新尝试时再禁止抢占。解锁用 `clear_bit_unlock()` 并恢复抢占。

这说明“自旋锁的整个等待过程一定全程关抢占”不是所有实现都成立。这里持有临界区期间关抢占，竞争轮询期间可以恢复抢占，但它没有睡眠等待队列。

位锁的空间成本小，但多个位可能争用同一个缓存字；调试能力、RT 替换能力也受限制。`hlist_bl` 是将 bit lock 融入链表头的使用方式。不能把 `bit_spin_lock()` 与 `wait_on_bit_lock()` 混为一谈：后者可通过等待队列睡眠。

## 6. rwlock：读者并发，也仍有共享写入

分析 [qrwlock.h][qrw-fast] 和 [qrwlock.c][qrw-slow]：

```text
cnts = reader_count << 9 | writer_flags
writer_flags: _QW_LOCKED = 0x0ff, _QW_WAITING = 0x100
另有 wait_lock，组织慢路径竞争者
```

读快路径 `atomic_add_return_acquire(_QR_BIAS)` 增加读者计数；没有 writer 标志便成功。写快路径以 CAS 将 0 改为 `_QW_LOCKED`。

普通读慢路径撤销读者计数，排入 `wait_lock`，再增加读者计数并等待写者结束。写慢路径先拿 `wait_lock`，设置 `_QW_WAITING` 阻止普通新读者不断绕过，然后等旧读者退出并转换为 `_QW_LOCKED`。

**中断读者是特殊路径**：如果写者只是等待、尚未持有写锁，中断读者可不排队，源码只等待 `_QW_LOCKED` 清除。这是实现中处理上下文约束的特殊安排，不能被当作所有锁都允许插队或递归的依据。

读解锁使用 release 原子减法；写解锁 release 清除 `wlocked`。多个读者可以并行运行，但都修改共享计数缓存行，因此 `rwlock_t` 未必比简单 spinlock 快。它适合非常短的读临界区；长读操作考虑 rwsem、RCU 或其他设计。

不支持把“持有读锁后再申请写锁”当作通用升级操作；递归读锁也可能遇到等待写者造成死锁，必须遵循具体 API 规则。

## 7. mutex 与 RT-mutex

### 7.1 mutex 的状态不是简单 0/1

非 RT 的 [struct mutex][mutex-type] 包含：

```text
owner       = task_struct 指针 | WAITERS | HANDOFF | PICKUP
wait_lock   = 保护内部等待队列的 raw_spinlock
wait_list   = 等待任务列表
osq         = CONFIG_MUTEX_SPIN_ON_OWNER 下的自旋队列
```

标志来自 [mutex.h][mutex-flags]：WAITERS 为 bit 0，HANDOFF 为 bit 1，PICKUP 为 bit 2。owner 指针低位编码利用指针对齐。判断 owner 必须先去掉标志，不能直接解引用原始整数。

### 7.2 获取：快路径 → 乐观自旋 → 阻塞

快路径：[__mutex_trylock_fast][mutex-fast] 将 `owner:0 → current`，使用 acquire CAS。

慢路径主线：[__mutex_lock_common][mutex-common]：

1. 检查睡眠上下文，登记 lockdep。
2. 重新尝试获取；条件合适时走 [mutex_optimistic_spin][mutex-spin]。
3. owner 仍在 CPU 上运行、当前不需要调度等条件支持短暂自旋；owner 不运行或需要调度时停止自旋。
4. 取得 `wait_lock`，再次尝试，关闭“等待拿内部锁时目标锁刚好释放”的窗口。
5. 将栈上的 waiter 加入等待队列，设置当前任务状态。
6. 释放内部锁后调度；醒来重新判断获取、交接、信号等情况。
7. 成功或取消后，清理 waiter、任务状态、调试和阻塞记录。

`wait_lock` 只保护 mutex 内部元数据，不是用户临界区的锁；不能持有它去调度。当前树还维护 `task->blocked_on` 相关记录，阅读时不能只按旧版伪代码忽略这些辅助路径。

### 7.3 解锁：唤醒、抢锁与定向交接

快路径：[__mutex_unlock_fast][mutex-unlock-fast] 以 release CAS 将纯 owner 从 current 改成 0。

有标志或竞争时进入 [__mutex_unlock_slowpath][mutex-unlock]：普通情况先释放 owner，再选择等待者加入 wake queue；若存在 HANDOFF，则通过 `__mutex_handoff()` 把 owner 指向指定任务并设置 PICKUP。指定任务在 acquire 路径确认并清除 PICKUP，完成交接。

因此：普通唤醒表示任务获得再次竞争的机会；handoff 才是明确把所有权保留给指定等待者。mutex 通过抢锁优化和交接机制兼顾吞吐与防止长期等待，不能概括成严格 FIFO。

`wake_q` 用于把唤醒动作移到内部锁外，缩短内部锁持有时间。

### 7.4 使用约束与返回值

- 同一任务加锁和解锁；不支持递归，不能从硬中断或 softirq 上下文调用。
- `mutex_lock()` 无返回值，通常不可中断等待；`mutex_lock_interruptible()` 成功为 0，被信号打断为 `-EINTR`。
- `mutex_trylock()` 成功为 1，失败为 0；不睡眠不等于可以任意搬到中断上下文。
- `mutex_is_locked()` 只是状态观察，不代表当前任务持有，也不能把“检查未锁”与之后的访问组成互斥。
- mutex 所在对象必须至少活到 `mutex_unlock()` 返回；解锁函数内部可能还访问 mutex。生命周期不能只靠“已经释放 owner”推断。

### 7.5 RT-mutex：解决优先级反转

情景：低优先级 L 持锁，高优先级 H 等锁，中优先级 M 持续运行，导致 L 没机会解锁。PI 将 L 的有效调度优先级提升到足以为 H 推进工作的水平；如果 L 又等另一把锁，继承关系沿链传播。

[struct rt_mutex_base][rt-type] 主要包含 `wait_lock`、缓存最左节点的红黑树 `waiters` 和 `owner`。owner 的最低位表示 HAS_WAITERS。任务同时维护自己的 PI 等待关系。

核心不是“一棵红黑树”，而是两层组织：锁的 waiters 管竞争者顺序；owner 的 `pi_waiters` 汇总其所持锁的最高优先级等待者。`pi_blocked_on` 指向当前任务正在等待的锁节点，用于沿依赖链传播。

[task_blocks_on_rt_mutex][rt-block] 将 waiter 入树，更新 owner 的 PI 信息；需要传播时调用 [rt_mutex_adjust_prio_chain][rt-chain]。实际排序还考虑调度类别、deadline 等规则，不能把所有等待者简单等价为按一个整数排序。

解锁的 [mark_wakeup_next_waiter][rt-wake] 更新 PI/deboost 状态、保留等待者标志并安排唤醒，使低优先级新来者不能随意抢在最高等待者之前。PI 不能解决锁顺序环，不能抢占关抢占/关 IRQ 区域，也不能对没有明确 owner 的计数信号量直接提供同类继承。

### 7.6 PREEMPT_RT 改变了哪些语义

| 类型 | 非 RT | PREEMPT_RT |
|---|---|---|
| `raw_spinlock_t` | 忙等，关抢占 | 保持严格忙等语义 |
| `spinlock_t` | raw spinlock 包装 | RT-mutex 基础，可阻塞，持有时保持 CPU 绑定 |
| `rwlock_t` | 忙等多读单写 | RT 读写基础，公平性不同 |
| `mutex` | owner + wait_list + OSQ | rt_mutex_base 变体 |
| `rw_semaphore` | 通用 rwsem | RT 读写基础 |
| `local_lock_t` | 本地抢占/中断保护域 | 每 CPU `spinlock_t` |
| bit spinlock / semaphore | 原实现 | 不自动获得 RT-mutex 替换或 PI |

[rt_spin_lock][spin-rt] 的获取路径先处理 rtmutex，再进入 RCU 读侧并禁止迁移；释放顺序也受对象生命周期约束。竞争时保存/恢复任务状态，避免获取锁的睡眠吞掉原本等待的事件唤醒。

RT 下普通 `spin_lock_irqsave()` 不再直接改变硬 IRQ 开关状态；`_bh()` 仍承担 BH 相关序列化。硬中断底层必须按真实上下文选可用原语，不能因为名字还叫 spinlock 就沿用非 RT 假设。

RT 读写锁不能将等待写者的优先级同时继承给多个活跃读者，故不可假定与非 RT 一样的写者公平性。实现解释见 [rwbase_rt.c][rwbase-rt]。

通用嵌套方向：可睡眠锁 → `spinlock_t/rwlock_t/local_lock` → `raw_spinlock_t/bit spinlock`；同类锁还须满足全局锁顺序。持有 raw spinlock 后再获取 RT spinlock 是错误的。

分类及 RT 语义也可对照 [Linux 6.18 官方锁类型文档](https://docs.kernel.org/6.18/locking/locktypes.html)；本篇实现细节以本地源码为准。

## 8. ww_mutex：对动态多对象获取进行死锁避免

普通锁顺序要求 A→B，但一批运行时才确定的 GPU buffer 未必方便排序。`ww_mutex` 用一批操作共享的 `ww_acquire_ctx` 和时间戳/序号，决定冲突时谁等待、谁回退。

[算法代码][ww-alg] 同时包含两种策略：

| 策略 | 老事务遇到年轻 owner | 年轻事务遇到老 owner |
|---|---|---|
| Wait-Die | 可以等待 | 在形成相关多锁冲突时回退 |
| Wound-Wait | 标记年轻事务 wounded，促使其回退 | 可以等待 |

“wound”不是强行从另一个任务手里抢走已持有的锁，也不是杀死进程；参与者必须遵守回退协议。

典型流程，依据 [ww_mutex API][ww-api]：

```text
ww_acquire_init(ctx, class)
  → 对整批对象依次 ww_mutex_lock(lock, ctx)
  → 若返回 -EDEADLK：
       释放本批已取得的全部锁
       对导致冲突的锁 ww_mutex_lock_slow(lock, ctx)
       保留同一个 ctx，重新获取其余对象
  → 全部成功后 ww_acquire_done(ctx)
  → 操作资源
  → 释放全部锁，ww_acquire_fini(ctx)
```

`-EALREADY` 表示该 ctx 已取得同一锁，需要按批次的去重逻辑处理；不能当作获得第二份锁并多解锁一次。RT 变体可结合 RT-mutex，但“死锁避免”和“优先级继承”是两套不同职责。

## 9. semaphore：许可计数，不要求 owner

[down][sem-down] 在内部 `raw_spin_lock_irqsave()` 下检查 `count`：大于 0 就减一；没有许可则构造 waiter 入链、设置任务状态，释放内部自旋锁并调度。

[up][sem-up] 有两个分支：

- 无等待者：增加 `count`。
- 有等待者：从等待链选取 waiter，设置 `waiter.up = true`，安排唤醒；把这个许可直接交给等待者，无须先把 count 加一再让所有任务抢。

无严格 owner 是 semaphore 与 mutex 的根本区别。释放许可的上下文可以没有获取过它，因此可用于跨上下文的许可交接，但无法像 rt_mutex 那样确定“应该提升谁”。新的纯互斥场景优先用 mutex；等待某事结束优先考虑 completion。

特别容易写反的返回值：`down_trylock()` **0 表示成功，1 表示失败**，与 `mutex_trylock()`、`spin_trylock()` 相反。`down_timeout()` 超时为 `-ETIME`；普通获取接口的可睡眠上下文要求仍须遵守。源码明确允许 `down_trylock()` 和 `up()` 用于相应中断场景，但不能把这一点推广到所有 sleeping-lock trylock。

## 10. rw_semaphore：可睡眠的多读单写

### 10.1 计数编码及快路径

[rwsem 的 count 定义][rwsem-count]：

```text
bit 0       WRITER_LOCKED
bit 1       WAITERS
bit 2       HANDOFF
bit 3..7    reserved
bit 8 起    reader count（最高位另作 READFAIL 保护位）
```

另有 owner、自旋队列、raw wait_lock 和 wait_list。owner 在读者模式下只是辅助追踪与自旋判断，不能当成所有活跃读者的完整名单。

- [读快路径][rwsem-read-fast]：以 acquire 原子加法加入一个 `RWSEM_READER_BIAS`，检查 writer、waiters、handoff 等阻挡标志。
- [写快路径][rwsem-write-fast]：以 acquire CAS 把 `count:0 → WRITER_LOCKED`。
- 获取失败走慢路径，读者可能需要撤销预先加入的计数；不能只看某一时刻 count 中有读者位就断言已经授予这些读锁。

### 10.2 等待与公平性

[读慢路径][rwsem-read-slow] 包含受限的乐观抢读，以及进入 wait_list 等待授予的路径；[写慢路径][rwsem-write-slow] 可乐观自旋，随后排队、睡眠、重试。关键设计是不让不断到来的读者无限绕过等待写者，同时利用 handoff 限制长期抢锁。

[rwsem_mark_wake][rwsem-wake] 区分写者与读者：写者被唤醒后通常仍需竞争；读者可批量授予读锁，且受批量上限约束。源码会整理和批量处理读者，不能简单画成“永远严格 FIFO、只唤醒紧挨队首的读者”。

其公平性目标是防止饥饿，实际包含 lock stealing、读批处理和 handoff，不能给出严格到达顺序或等待时间上界。

### 10.3 释放与降级

[__up_read][rwsem-up-read] 用 release 原子减法减少读者计数；最后一批读者退出且有等待者时触发唤醒。[__up_write][rwsem-up-write] release 清写锁状态并处理等待者。

`downgrade_write()` 将写所有权原子地转换为读所有权，避免中间无保护窗口。它不意味着存在对称的安全 `upgrade_read()`：两个读者同时等升级可以互相卡住，通常要释放读锁、申请写锁后重新验证条件。

`down_read_non_owner()` / `up_read_non_owner()` 是明确标注的特殊接口；不能据此认为所有读写锁都允许随意跨任务解锁。RT 版本转到 [rwbase_rt.c][rwbase-rt]，不继承非 RT 全部公平性性质。

## 11. percpu_rw_semaphore：把常见读开销分散到各 CPU

传统 rwsem 的每次读加解锁都修改同一个 count，高核数下容易造成缓存行迁移。percpu_rwsem 将常见读计数分散到 `read_count`，用写侧协调成本换读侧吞吐。

[结构与读路径][percpu-read]：`rss` 管理 RCU 同步状态，`read_count` 为 per-CPU 计数，`block` 控制阻塞，`writer` 是写者等待点，`waiters` 管理阻塞读写者。

**读快路径**：短暂禁止抢占，检查 `rcu_sync_is_idle()`；空闲则 `this_cpu_inc()`，再恢复抢占。临界区本身并没有全程禁止抢占，可睡眠/迁移。读退出可能在另一 CPU 减计数，正确性依赖全 CPU 计数的模加总，不要求每 CPU 计数永远非负或局部配对。

**写获取**：[percpu_down_write][percpu-write]：

1. `rcu_sync_enter()` 关闭无屏障读快路径，并完成必要的宽限期协调。
2. 获取 `block`，实现写者之间的独占并迫使新读者阻塞或走慢路径。
3. 汇总全部 CPU 的 read_count，等待已进入的读者退出。
4. 总计数稳定为零后进入写临界区。

读慢路径在增加计数后通过屏障检查 block。如果读者没观察到 block，写者就必须观察到它的计数；如果写者没观察到计数，读者就应看到 block 并撤销。源码 A/B/C/D 标注展示这些内存顺序配对。

**写释放**：[percpu_up_write][percpu-unlock] release 清 block、唤醒等待者，再 `rcu_sync_exit()`，经协调后恢复读快路径。写解锁并不是简单地立即把系统切回无屏障读模式。

适用条件是读非常频繁且写很少、允许较高写延迟。它不是“更快的通用 rwsem”，写侧可能需要宽限期和跨 CPU 计数扫描。

## 12. local_lock 与 per-CPU 并发控制

[local_lock 的非 RT 实现][local-lock] 将命名保护域映射为抢占/中断控制，并配合 lockdep 记录；[RT 分支][local-lock-rt] 则使用每 CPU spinlock 和迁移控制。

它的关键价值是把“这段代码靠本 CPU 上不重入来保护什么”明确表达出来。禁抢占本身没有对象名称；local_lock 可以命名并校验保护范围。

三个常见误区：

1. 同一个 local_lock 名称在不同 CPU 上不构成全局互斥；访问其他 CPU 的数据需要额外协议。
2. `migrate_disable()` 只防迁移，不能阻止另一个任务抢占当前任务并访问同一 CPU 数据。
3. 在非 RT 上两个不同 local_lock 都可能最终关 IRQ，但 RT 下它们是不同锁，不能依赖它们恰好保护同一对象。

本树还提供 local_trylock 及 BH 相关变体；这些改变获取方式或适用上下文，不是新的一种全局锁算法。

## 13. seqcount、seqlock 与 latch：校验读快照

### 13.1 普通 seqcount 的协议

```text
写者（已由外部锁串行化）        读者
sequence++，变奇数             等到读出偶数 sequence
写屏障                         复制相关字段
修改多个字段                   读屏障、再次读取 sequence
写屏障                         前后相等才接受结果，否则重试
sequence++，变偶数
```

当前树 [read_seqcount_begin][seq-begin] 通过 `seqprop_sequence()` 做 acquire 读取；[read_seqcount_retry][seq-retry] 在最终比较前使用 `smp_rmb()`。写者在开始/结束递增 sequence，两侧用 `smp_wmb()` 排序。不要机械套用旧源码“begin 必然是普通读后接 rmb”的逐行描述。

简化示例，假设已初始化，写者已按正确上下文串行化：

```c
/* 读：只在校验成功后使用副本 */
do {
        seq = read_seqcount_begin(&s);
        x = READ_ONCE(data.x);
        y = READ_ONCE(data.y);
} while (read_seqcount_retry(&s, seq));
consume(x, y);

/* 写：外部锁必须保护整个 begin/end 区间 */
write_seqcount_begin(&s);
WRITE_ONCE(data.x, new_x);
WRITE_ONCE(data.y, new_y);
write_seqcount_end(&s);
```

这是一致性重试，不是读写互斥。读者可能暂时看到中间状态，只是最后丢弃；因此读循环里不能产生无法回滚的副作用，也不能用未经校验的索引直接做危险访问。

### 13.2 必须满足的三个前提

**写者互斥**：裸 `seqcount_t` 不串行化多个写者。`seqcount_spinlock_t`、`seqcount_mutex_t` 等关联类型能进行锁持有校验并处理部分抢占要求，但 `write_seqcount_begin()` 不会替调用者获取关联写锁。

**写者能够继续运行**：裸 seqcount 写者不能被会在同一计数器上持续等待的读者打断而失去执行机会。非 RT 写侧要禁止抢占，读者来自 IRQ 时还须处理 IRQ；否则读者等奇数变偶数，而写者正被读者压住，构成活锁/死锁。

**生命周期独立安全**：读者追踪一个指针时，对象若已被写者释放，之后发现 sequence 改变也救不了已经发生的 UAF。指针回收必须另用 RCU、引用计数或其他生命周期协议。

### 13.3 seqlock 与 RT 关联类型

[seqlock_t][seqlock-write] 自带 spinlock，`write_seqlock()` 先锁住写侧再递增 sequence，`write_sequnlock()` 反向结束。普通读者用 `read_seqbegin()` / `read_seqretry()`；部分接口允许读者在重试策略中改为锁定读取，减少持续写入时的反复失败。

RT 下部分关联 seqcount 类型检测奇数时会对关联锁进行 lock/unlock，给被抢占的写者推进机会，见 [SEQCOUNT_LOCKNAME][seq-associated]。因此不能笼统宣称“所有 seqcount 读者在任何配置都完全不获取锁”。裸 seqcount 也不会凭空获得这层保护。

### 13.4 seqcount_latch：双副本供打断写者的读者使用

[latch 实现][seq-latch] 保留 `data[2]`，读者按 sequence 最低位选择副本，最后比较完整 sequence：

```text
切换读者到 data[1] → 修改 data[0]
切换读者到 data[0] → 修改 data[1]
```

所以读者即使打断写者，也有一个当前选定的稳定副本可读，无须像普通 seqcount 那样原地等待奇数结束。代价是双份存储和双份更新。写者之间仍要外部串行化；动态对象发布与释放仍需要生命周期保护。

`u64_stats_sync` 是相关专用封装：32 位上用 seqcount 避免 64 位计数撕裂；64 位上同步部分可为空操作，因此不能保证几个不同计数器之间必定构成同一时刻快照。依据：[u64_stats_sync.h][u64-stats]。

## 14. RCU 与 SRCU：允许旧读者继续，推迟旧对象回收

### 14.1 RCU 不存在通用 rcu_write_lock

[rcu_read_lock][rcu-read] 标记读侧临界区；读者间不互斥，读者和更新者也不是 rwlock 式排斥。写者之间仍需要 mutex、spinlock、CAS 等协议协调。

典型替换流程：

```text
创建并初始化新对象
   → 发布新指针
   → 新读者可能看到新对象，旧读者可继续用旧对象
   → 等待所需宽限期
   → 回收已从数据结构移除的旧对象
```

宽限期不是“等整个系统再也没有 RCU 读者”。它保证相关先前读者已结束；新的读者可以不断进入。

### 14.2 发布、读取、回收是三件事

| 接口 | 职责 |
|---|---|
| `rcu_assign_pointer(p, n)` | 按 RCU 发布语义公布初始化完成的新对象 |
| `rcu_dereference(p)` | 按 RCU 规则读取指针，维护所需依赖顺序与检查 |
| `synchronize_rcu()` | 调用者阻塞等待所需宽限期 |
| `call_rcu(head, callback)` | 排队，宽限期后异步执行回调 |
| `kfree_rcu()` | 常见的宽限期后释放封装 |
| `rcu_barrier()` | 等待先前排队的相关 RCU 回调完成；与等宽限期不同 |

来源：[指针发布][rcu-assign]、[synchronize_rcu][rcu-sync]、[call_rcu][rcu-call]。

不能把 `rcu_dereference()` 简单替换成任意 C 指针读，也不必把它机械解释成与通用 `smp_load_acquire()` 完全同义；RCU 有专门的依赖和编译器约束。

### 14.3 最小对象替换示例

下面的示意假定 `global_cfg` 带 `__rcu` 标记，所有更新者共用 `cfg_mutex`；对象发布后字段不可变，不存在绕过该协议的引用。

```c
/* 读取：把需要的数据复制出去，不能直接带着 p 离开保护区使用 */
rcu_read_lock();
p = rcu_dereference(global_cfg);
if (p)
        value = p->value;
rcu_read_unlock();

/* 更新：new 已初始化，包含 struct rcu_head rcu */
mutex_lock(&cfg_mutex);
old = rcu_dereference_protected(global_cfg,
                                lockdep_is_held(&cfg_mutex));
rcu_assign_pointer(global_cfg, new);
mutex_unlock(&cfg_mutex);
if (old)
        kfree_rcu(old, rcu);
```

RCU 不保证一个仍在原地更新的对象的多个字段自动一致。上例依靠“新建完整对象再整体发布”；如果原地更新多个字段，需要额外锁或 seqcount 等协议。

### 14.4 Tree RCU 的源码层次

[tree.h][rcu-tree-type] 中：`rcu_data` 记录每 CPU 状态和回调信息；`rcu_node` 组成层次树，`qsmask` 跟踪需要报告的静止状态；全局状态维护宽限期序列等信息。

[tree.c][rcu-gp] 的主线是启动宽限期、汇总静止状态、必要时请求推进、结束宽限期；[rcu_do_batch][rcu-batch] 等路径执行已就绪回调。PREEMPT_RCU 还必须跟踪被抢占的读者，不能只根据“某 CPU 发生过一次上下文切换”就宣布其全部旧读者已完成。

普通 RCU 读区不允许任意主动睡眠；PREEMPT_RCU 允许被抢占，与主动阻塞不同。RT 下获取具有 PI 的特定 spinlock 是文档说明的特殊情况，不能据此在 RCU 读区里做任意等待。

`rcu_read_lock_bh()`、`rcu_read_lock_sched()`、Tasks RCU、Tasks Trace RCU 等有各自的上下文/静止点定义。不要把接口的不同名字一概解释成几套完全互不相干的通用读写锁。

### 14.5 SRCU：需要读区睡眠时

SRCU 用独立 `srcu_struct` 组织一个保护域，读区可以睡眠。通常使用 `idx = srcu_read_lock(ssp)`，退出时把 idx 原样传给 `srcu_read_unlock(ssp, idx)`。

**本树实际实现值得单独看**：[__srcu_read_lock][srcu-read] 读取 `srcu_ctrp`，增加所选每 CPU 计数器的 locks，执行屏障并返回计数器身份编码；退出增加对应计数器的 unlocks。退出可以发生在另一 CPU，但须由匹配的任务/上下文完成，不能随意跨任务解锁。

宽限期状态机切换所使用的计数器代际，并检查各 CPU 的进入/退出累计值，确认旧读者完成；入口是 [synchronize_srcu][srcu-sync]。本树还存在 `srcu_read_lock_fast()` 等变体，应按各自契约使用，不能直接套用普通接口的内部步骤。

禁止在同一个 SRCU 读区里直接或间接等待该保护域的宽限期。例如读者等 mutex，而 mutex owner 正等该 SRCU 宽限期，就形成依赖环。

## 15. waitqueue 与 completion：等待事件，不是占有资源

### 15.1 waitqueue 为什么不会把“检查条件”和“睡眠”断开

错误流程是：检查条件未满足 → 另一个 CPU 改条件并唤醒 → 当前任务此时才入队睡眠。唤醒时还没人排队，导致之后无期限等待。

[wait_event 的内部循环][wait-event] 与 [prepare_to_wait_event][wait-prepare] 采用的主线：

```text
加入等待队列并设置任务状态
  → 重新检查条件
    → 已满足：退出并清理
    → 未满足：schedule
  → 醒来继续检查
  → finish_wait
```

条件必须由调用者维护，用正确的锁、原子访问或内存顺序协议。waitqueue 内部锁主要保护队列/唤醒关系，不会自动锁住你的业务数据。生产者应先改变条件，再通知等待者；不能在唤醒后才写条件。

单次发布、ready 不再被清零的示意：

```c
/* 生产者 */
result = compute();
smp_store_release(&ready, true);
wake_up(&wq);

/* 消费者；允许睡眠 */
wait_event(wq, smp_load_acquire(&ready));
use(result);
```

循环复用、多个生产者或资源消耗语义需要更完整的协议。不要把这个单次发布样例直接改成复杂无锁队列。`wake_up()` 也不应被当成在所有情况下都提供同样屏障效果的通用发布原语。

`swait` 是约束更强的简单等待队列，`rcuwait` 面向单等待者场景；它们都不是锁所有权机制。

### 15.2 completion：保存已经发生的完成事件

结构主要是 `done` 和简单等待队列。源码在 [completion.c][complete]：

- `complete()` 在内部 raw 锁下增加 done 并唤醒一个等待者。
- 等待者发现 done 非零，消耗一个完成计数后返回；因此先 complete 后 wait 也不会丢事件。
- `complete_all()` 设置饱和值并唤醒全部，之后等待者继续可通过，直到按协议重新初始化。
- 复用时 `reinit_completion()` 需要保证上一轮等待/完成活动已按协议收尾。

超时返回不代表另一个执行者停止了。尤其是栈上 completion：若等待超时后函数返回，异步执行者稍后仍 `complete()`，就可能访问已失效的栈内存。必须先协调异步工作的结束或使用足够长的对象生命周期。

RT 下 `complete_all()` 有额外上下文检查，不能把所有 completion 操作都概括成“任何中断上下文均可调用”。

## 16. futex：把用户态竞争者接入内核等待队列

### 16.1 futex 本身不等于完整 mutex

普通用户态锁可以用原子操作完成无竞争获取/释放；真正需要等待时调用 futex。具体锁字编码、递归性、公平性、取消协议属于用户态库，Linux futex 提供按用户地址对应的 key 等待与唤醒等基础。

主要操作：WAIT/WAKE、REQUEUE、PI lock/unlock、等待向量等。PI futex 使用内核 RT-mutex 跟踪 owner；普通 FUTEX_WAIT 不自动提供 PI。

### 16.2 WAIT：必须把“比较预期值”和“入队”串起来

当前树的概念链：

```text
futex_wait()
  → __futex_wait()
    → futex_wait_setup()
      → 取得 futex key 和哈希桶
      → 登记等待者、获取桶锁
      → 再次读取用户地址，比较 expected
      → 不相等：返回 -EWOULDBLOCK
      → 相等：设置任务状态、futex_queue()
    → futex_do_wait()
    → 唤醒/信号/超时后处理出队与重试
```

入口：[futex_wait_setup][futex-wait]。地址访问可能发生异常，因此还有解锁、处理缺页后重试等路径。源码用了新的桶引用管理包装；其背后的原理仍是保证 key/桶的有效性和比较入队的正确同步。

### 16.3 WAKE：只让任务继续运行，不自动取得用户态锁

[futex_wake][futex-wake] 在对应桶中查找匹配 key/bitset 的 waiter，移出/标记并安排 wake queue，最后唤醒。哈希冲突意味着不同 futex 可共享桶，但应根据 key 区分；不是一个桶等于一个锁。

为了在无 waiter 时跳过桶锁，代码还使用 waiter 计数和配对屏障。文件开头的 [完整竞争说明][futex-protocol] 给出关键保证：**等待方不能同时漏看锁字变化，唤醒方又漏看已经入队的等待者。**

普通 futex 被唤醒后，用户态仍要重新检查条件/重新获取锁。虚假唤醒、信号、超时、竞争者抢先等都要求循环协议。

### 16.4 PI 与 robust 是不同能力

[PI futex][futex-pi] 将用户态锁字的 owner 状态与内核 `futex_pi_state` / RT-mutex 对齐，提供优先级继承与交接；不能随意把 PI 与普通 WAKE 混用。

robust futex 通过线程退出时处理登记的 robust list，向等待者反映 owner 死亡等状态；它不自动修复 owner 死亡时可能损坏的业务数据。PI 解决优先级反转，robust 解决 owner 退出后的通知/恢复入口，两者不可等同。

## 17. 原子变量、引用计数、lockref 与无锁结构

### 17.1 atomic 的保证粒度

`atomic_t`、`atomic_long_t`、`atomic64_t` 保证规定操作的原子性，不会把多步业务逻辑自动合成事务。例如：

```c
if (atomic_read(&count) > 0)
        atomic_dec(&count);  /* 两步之间其他 CPU 可改变 count */
```

若要求“仅在大于零时扣减”，需要恰当的条件 RMW、CAS 循环或锁；正确接口还须符合返回值与排序要求。

[atomic 内存顺序规则][atomic-doc] 的基础区别：无返回值 RMW 通常不提供对其他地址的完整排序；带返回值 RMW 的默认语义通常更强；`_relaxed/_acquire/_release` 明确选择排序；条件操作失败一般不带成功路径的排序保证。`atomic_read()` 不是通用 acquire。

具体架构可能用原子指令、LL/SC 或其他实现。某些 `atomic64_t` 通用后备实现会用锁，故“API 叫 atomic”不意味着实现必然 lock-free、wait-free 或适用于任意特殊上下文。

### 17.2 引用计数保护存活，不保护对象内部字段

`refcount_t` 提供引用语义与饱和等安全约束；`kref` 在引用释放到零时调用释放函数；它们不能代替字段修改的互斥。

`refcount_inc_not_zero()` 也必须建立在计数器所在内存仍可安全访问的基础上。若手里只剩悬空指针，试图加引用本身就是 UAF；通常需要先在 RCU、锁或已有引用的保护下找到对象并取得引用。

`percpu_ref` 用 per-CPU 累加降低热引用开销，销毁时切换、汇总并阻止新的 live 引用。它不自动替代查找路径所需的 RCU 宽限期。依据：[refcount.h][refcount]、[percpu-refcount.h][percpu-ref]。

### 17.3 lockref：将引用更新和锁状态放在一起判断

[lockref_get][lockref] 在配置和架构允许时，对组合的 lock/count 做 64 位 CAS：锁未被占用就修改 count，失败或不可用时退回 spinlock。它减少常见引用操作真正加锁的次数，但不是对任意对象字段开放无锁访问。

[lockref_put_or_lock][lockref-put] 返回 true 表示成功减引用；返回 false 则是临界引用场景且锁已被获取，调用者必须处理该状态并按约定解锁，不能把 false 一律当作“未做任何操作”。

`rcuref`、`percpu_ref` 等是其他针对生命周期/性能场景的协议；`llist`、环形缓冲区等无锁结构则组合原子操作、发布顺序和受限生产者/消费者模型。CAS 成功只能证明那一步状态转换成功，不能自动解决 ABA、对象回收和整个算法的进展性。

## 18. 专用锁：辨认包装与独立协议

### 18.1 folio/page lock、buffer lock、wait_bit

[folio_lock][folio-lock] 先 `folio_trylock()` 尝试设置 `PG_locked`，失败后经 `__folio_lock()` 到 [folio_wait_bit_common][folio-wait]。后者组织等待队列，区分独占获取、仅等待状态改变等模式。

[folio_unlock][folio-unlock] 清锁状态并在需要时唤醒等待者。这类锁可以由 I/O 完成路径等不同上下文释放，不具备 mutex 的严格任务 owner 约束。它保护指定页缓存状态及操作，并不排斥对映射页的全部写入或 DMA。

[buffer lock][buffer-lock] 使用 `BH_Lock` 位；慢路径调用 `wait_on_bit_lock_io()`，解锁 `clear_bit_unlock()` 后按协议执行屏障并 `wake_up_bit()`。

[wait_bit.c][wait-bit] 将地址和 bit 映射到等待队列，结合重新检查/尝试置位来睡眠等待。关键区分是：**状态用一个 bit 表示，不代表等待方式一定是自旋。**

### 18.2 mmap_lock、VMA 锁、inode、dentry、XArray

| 名称 | 本质/组合 | 读源码时的重点 |
|---|---|---|
| `mm->mmap_lock` | rw_semaphore | [mmap_read_lock / mmap_write_lock][mmap-lock] 包装，附带跟踪和序列协调 |
| per-VMA lock | VMA 引用状态、序列与 RCU 等组合协议 | [mmap_lock.h][vma-lock]；本树不能按旧版“一把独立 rwsem”解释 |
| inode `i_rwsem` | rw_semaphore | `inode_lock()` 等包装与目录/文件操作的锁顺序 |
| dentry 锁与引用 | spinlock/lockref 与 RCU 等 | `d_lock` / `d_lockref` 如何配合查找、引用和回收 |
| 页表锁 | spinlock，可能按页表拆分 | 保护的是页表更新；不同于 folio 锁与 mmap_lock |
| XArray `xa_lock` | spinlock | 普通接口与 `__xa_*` 调用者持锁接口的边界 |
| 设备对象锁 | 通常是对象中的 mutex 包装 | 设备生命周期、回调时是否仍持锁 |

per-VMA 写侧必须与 mmap 写锁协议协调，`vma_start_write()` 排斥相关读者直至 mmap 写锁释放/降级。不能以“锁了这个 VMA”为由跳过页表或地址空间其他层级要求。上述锁有的按对象分片，有的包裹更多状态转换；名字不同不代表底层算法都不同。

### 18.3 socket 锁：任务所有权与 BH 自旋锁的组合

[lock_sock_nested][sock-lock] 先取 `sk_lock.slock` 并禁 BH，已有用户持有者时等待，然后设置 `owned` 并释放内部自旋锁。任务后续拥有 socket 的逻辑所有权，但并非一直持有 `slock`。

`release_sock()` 在内部锁下处理 backlog/释放回调、清理 ownership、唤醒等待者。它展示了“一个外部可睡眠锁协议，由短内部自旋锁 + owner 标志 + 等待队列组成”的典型方式。BH 路径须遵守 backlog 等规则，不能当作普通 mutex 的完全同义包装。

### 18.4 hwspinlock：跨异构处理器的硬件仲裁

[__hwspin_trylock][hwspin] 在常规模式下先取得本地软件锁，再调用设备 `ops->trylock()`；硬件获取失败就撤销本地获取。成功后执行额外 `mb()`，因为本地软件锁的屏障发生得太早，不能代替硬件锁交接的排序。

超时接口反复尝试并检查时间限制；释放路径按顺序执行屏障和硬件 unlock。RAW/IN_ATOMIC 等模式对本地锁定的处理不同，调用者必须遵守具体 API 契约。远端固件、缓存一致性和设备访问规则也是协议的一部分；一把 Linux 内存中的 spinlock 不能自动同步不共享该协议的远端处理器。

### 18.5 BPF spin lock

[BPF helper][bpf-spin] 对四字节锁状态使用架构自旋锁或原子后备循环，并配合本地 IRQ 状态处理。BPF verifier 还约束使用场景与持锁期间操作，所以它不是把全部内核 `spinlock_t` API 原封不动暴露给程序。

### 18.6 文件锁：flock、POSIX 记录锁、OFD 锁与 lease

[fs/locks.c][file-lock] 处理文件范围和锁 owner 的逻辑冲突，内部再用自旋锁、percpu_rwsem、等待队列等保护自己的数据。

- `flock`：整文件共享/独占协议。
- POSIX `fcntl` 记录锁：按字节区间判断冲突，传统 owner 规则与进程相关。
- OFD 锁：按 open file description 的所有权处理，不能按单个 fd 数字或传统进程锁生命周期理解。
- lease：围绕冲突访问通知/解除租约的文件层协议，并非用于包住任意内核 C 临界区。

`posix_lock_inode()` 在文件锁上下文中查找冲突；非阻塞请求失败返回，阻塞请求登记依赖并等待，区间更新还涉及拆分/合并。`locks_lock_inode_wait()` 分派 POSIX/flock 等等待路径。

这些机制通常是协作式文件访问协议，不能简单宣称“持锁后任何 read/write 都被内核强制阻止”。文件系统和网络文件协议还可能提供专门实现；本篇未展开各协议的全部远程锁语义。

### 18.7 System V 信号量与分布式锁

[ipc/sem.c][sysv-sem] 管理用户态 IPC 信号量集合、成组 `semop` 的提交/回退、等待与 `SEM_UNDO` 等语义。这不同于内核 `struct semaphore`，也不是 POSIX 用户态 semaphore 的 libc 实现源码。

[DLM][dlm] 以 lockspace、资源对象、锁模式及锁请求组织集群资源仲裁；`dlm_lock()` 经参数检查进入 request/convert 路径，必要时向资源 master 发送消息，以回调交付完成/阻塞通知。API 返回请求已被接受不一定表示已经拿到资源锁，必须结合完成状态理解。

NFS lockd、文件系统自己的远程锁等另有协议实现。这些锁处理的是跨节点资源和故障恢复，不能靠一个本机 acquire/release 屏障完成。

## 19. lockdep、统计与动态检查

### 19.1 lockdep 检查“类之间的依赖”

[lock_acquire][lockdep-acquire] / `__lock_acquire()` 更新任务当前持锁集合和锁类依赖。任务持有 A 又获取 B，就形成 A→B；未来观察到 B→A，即使实际死锁尚未发生，也可能报告潜在环。

锁类不是锁地址的简单同义词：许多 inode 实例中的同类锁可属于同一个 class。动态初始化、嵌套子类与 lockdep annotation 必须反映真实的对象层级，不能为消除报告随便换 class。

[check_noncircular][lockdep-cycle] 等检查依赖环；[check_wait_context][lockdep-context] 检查持锁/上下文允许的等待类型，另外还有 IRQ-safe/unsafe 关系校验。

lockdep 依赖运行时覆盖和正确注解，不能证明所有未来路径绝无死锁，也不能替代对象生命周期与无锁内存顺序证明。

### 19.2 不同工具回答不同问题

| 工具/配置 | 用途 | 不能替代什么 |
|---|---|---|
| `CONFIG_PROVE_LOCKING` / lockdep | 锁顺序、上下文、依赖合法性 | 所有路径覆盖和业务协议证明 |
| `DEBUG_SPINLOCK`、`DEBUG_MUTEXES`、`DEBUG_RWSEMS` | 对应锁状态与使用检查 | 通用数据竞争检测 |
| `might_sleep()`、原子上下文检查 | 发现不允许睡眠的上下文调用 | 非阻塞但有逻辑错误的协议 |
| KCSAN | 检测被执行路径中的数据竞争 | 所有内存模型结果的形式验证 |
| lockstat / `/proc/lock_stat` | 获取、争用、等待/持有时间等统计 | 自动给出正确的重构方案 |
| lock contention tracepoints / perf lock | 根据构建、工具版本观测锁热点 | 未采集路径的行为 |
| locktorture / rcutorture | 锁或 RCU 实现压力测试 | 具体驱动的完整锁顺序测试 |
| LKMM / litmus tests | 推理小型并发协议允许的内存结果 | 无限制大程序的自动正确性保证 |

对应资料：[lockdep 设计][lockdep-doc]、[lockstat][lockstat-doc]、[locktorture][locktorture-doc]。此源码工作区没有运行中的目标内核和 `.config`，本次只完成静态源码分析与文档链接校验，没有采集上述运行数据。

## 20. 选型与常见错误

### 20.1 按需求选择

| 需求 | 通常先考虑 | 关键限制 |
|---|---|---|
| 任务上下文，保护可睡眠操作 | mutex | 正确 owner、错误路径释放、锁顺序 |
| 底层原子上下文，极短独占 | raw spinlock 或该子系统既定原语 | 极短、不可睡眠、处理 IRQ/NMI 关系 |
| 普通共享状态，需兼容 RT | spinlock_t 及正确后缀 | RT 下上下文要求与 IRQ 语义变化 |
| 读写临界区可能睡眠 | rwsem | 读写比例不足时未必优于 mutex |
| 很短的多读单写 | rwlock_t | 读计数缓存竞争、RT 公平性 |
| 读者极多、写者极少 | percpu_rwsem | 写延迟和扫描/宽限期成本 |
| 小型多字段快照、写很短、可重试 | seqcount/seqlock | 写者串行化和进展性、对象生命周期 |
| 指针查找多、可接受旧版本、延迟释放 | RCU | 更新者协调、发布和回收都要正确 |
| RCU 风格但读区要睡眠 | SRCU | 不得形成等待自身宽限期的环 |
| 等一次操作完成 | completion | 超时后的异步对象生命周期 |
| 等待某个可反复变化条件 | waitqueue | 条件与数据的同步、醒来重新检查 |
| 一批动态对象不能简单排序 | ww_mutex | 必须完整实现回退/重试协议 |
| 需要优先级继承 | rt_mutex / PI futex | PI 不消除依赖环或长临界区 |
| 只保护当前 CPU 数据 | local_lock / 正确 per-CPU 协议 | 不能当全局互斥 |
| 只更新一个计数或状态转换 | 合适的 atomic API | 多步不自动原子，排序需明确 |

选型之后再量测争用。按对象、CPU、哈希桶分片，缩短临界区，把可移出的计算/I/O 移出锁，通常比仅换锁名字更直接。必须先确认哪些字段必须一起保持不变量，不能为了降低等待时间拆掉必要的原子性。

### 20.2 必须逐项审视的错误模式

| 错误写法/判断 | 为什么错 | 应对方式 |
|---|---|---|
| 持有非 RT spinlock/raw 锁做可睡眠分配、阻塞 I/O | 原子上下文调度 | 移出操作，或重设计锁范围/类型 |
| 任务和硬中断共用普通 spin_lock，任务不处理 IRQ 重入 | 中断等被打断任务解锁 | 按上下文用 IRQ-safe 协议 |
| 关本地 IRQ 后认为其他 CPU 不能访问 | 只限制本 CPU | 加入跨 CPU 同步 |
| 获取 A→B 和 B→A | 形成等待环 | 建立顺序、回退或适当多锁协议 |
| 被唤醒就假定已持锁 | 通常只是变为可运行 | 重新检查/获取，区分 handoff |
| seqcount 读失败后重试就可安全解引用已释放对象 | 重试发生在 UAF 之后 | 独立生命周期协议 |
| RCU 读锁等同于“写者不能动数据” | RCU 不排斥更新者 | 版本替换或额外数据一致性协议 |
| trylock 失败还执行受保护操作 | 未获得互斥 | 失败后退回安全路径 |
| 用 is_locked 证明当前持锁 | 状态可变，也不表明 owner | 正确加锁与 lockdep_assert_held |
| completion 超时就释放异步方仍使用的对象 | 超时没有取消生产者 | 取消/同步结束或延长生命周期 |
| 持锁等待某工作结束，而该工作又需此锁 | 隐式依赖环 | 分离等待与锁持有阶段 |
| 在锁仍被引用时重初始化、复制或销毁 | 锁元数据/等待者失效 | 建立退出与生命周期协议 |
| RT 下 raw 锁内再拿普通 spinlock | 后者可能阻塞 | 遵循锁类型嵌套方向 |

## 21. 建议的源码阅读路线

1. **语义层**：[locktypes.rst][locktypes-local]，先掌握上下文与 RT 差异。
2. **最小阻塞模型**：[semaphore.c][sem-down] 和 [completion.c][complete]，理解内部自旋锁、waiter、任务状态、释放内部锁后调度。
3. **任务级互斥**：[mutex.c][mutex-common]，把快路径、OSQ、自旋、等待队列和 handoff 连起来。
4. **忙等扩展性**：[qspinlock.h][qspin-fast] → [qspinlock.c][qspin-slow] → [MCS][mcs] → [OSQ][osq]。
5. **读写权衡**：[qrwlock.c][qrw-slow] → [rwsem.c][rwsem-count] → [percpu-rwsem.c][percpu-write]。
6. **一致性与回收**：[seqlock.h][seq-begin] → [rcupdate.h][rcu-read] → [Tree RCU][rcu-gp] → [SRCU][srcu-read]。
7. **优先级与多锁**：[rtmutex.c][rt-block] → [ww_mutex.h][ww-alg] → [futex PI][futex-pi]。
8. **把原理映射到调用者**：沿 mmap、folio、socket、文件锁实例，画出被保护字段、所有访问方、锁顺序和销毁路径。

阅读任意一把锁时，固定记录六件事：**保护的不变量、访问上下文、状态编码、成功获取的原子步骤、等待与唤醒协议、对象生命周期**。只有这六项连起来，才算从“认识函数名”进入到源码层面的正确性分析。

[arm64-spin]: /Users/jinqinghui/linux-6.18/arch/arm64/include/asm/spinlock.h:8
[atomic-doc]: /Users/jinqinghui/linux-6.18/Documentation/atomic_t.txt:160
[barriers]: /Users/jinqinghui/linux-6.18/Documentation/memory-barriers.txt:474
[bit-spin]: /Users/jinqinghui/linux-6.18/include/linux/bit_spinlock.h:16
[bpf-spin]: /Users/jinqinghui/linux-6.18/kernel/bpf/helpers.c:285
[buffer-lock]: /Users/jinqinghui/linux-6.18/fs/buffer.c:69
[complete]: /Users/jinqinghui/linux-6.18/kernel/sched/completion.c:21
[dlm]: /Users/jinqinghui/linux-6.18/fs/dlm/lock.c:3373
[file-lock]: /Users/jinqinghui/linux-6.18/fs/locks.c:1146
[folio-lock]: /Users/jinqinghui/linux-6.18/include/linux/pagemap.h:1137
[folio-unlock]: /Users/jinqinghui/linux-6.18/mm/filemap.c:1518
[folio-wait]: /Users/jinqinghui/linux-6.18/mm/filemap.c:1258
[futex-pi]: /Users/jinqinghui/linux-6.18/kernel/futex/pi.c:983
[futex-protocol]: /Users/jinqinghui/linux-6.18/kernel/futex/waitwake.c:13
[futex-wait]: /Users/jinqinghui/linux-6.18/kernel/futex/waitwake.c:595
[futex-wake]: /Users/jinqinghui/linux-6.18/kernel/futex/waitwake.c:155
[hwspin]: /Users/jinqinghui/linux-6.18/drivers/hwspinlock/hwspinlock_core.c:92
[local-lock]: /Users/jinqinghui/linux-6.18/include/linux/local_lock_internal.h:116
[local-lock-rt]: /Users/jinqinghui/linux-6.18/include/linux/local_lock_internal.h:220
[lockdep-acquire]: /Users/jinqinghui/linux-6.18/kernel/locking/lockdep.c:5827
[lockdep-context]: /Users/jinqinghui/linux-6.18/kernel/locking/lockdep.c:4852
[lockdep-cycle]: /Users/jinqinghui/linux-6.18/kernel/locking/lockdep.c:2149
[lockdep-doc]: /Users/jinqinghui/linux-6.18/Documentation/locking/lockdep-design.rst:1
[lockref]: /Users/jinqinghui/linux-6.18/lib/lockref.c:42
[lockref-put]: /Users/jinqinghui/linux-6.18/lib/lockref.c:108
[lockstat-doc]: /Users/jinqinghui/linux-6.18/Documentation/locking/lockstat.rst:86
[locktorture-doc]: /Users/jinqinghui/linux-6.18/Documentation/locking/locktorture.rst:2
[locktypes-local]: /Users/jinqinghui/linux-6.18/Documentation/locking/locktypes.rst:6
[mcs]: /Users/jinqinghui/linux-6.18/kernel/locking/mcs_spinlock.h:57
[mmap-lock]: /Users/jinqinghui/linux-6.18/include/linux/mmap_lock.h:309
[mutex-common]: /Users/jinqinghui/linux-6.18/kernel/locking/mutex.c:562
[mutex-fast]: /Users/jinqinghui/linux-6.18/kernel/locking/mutex.c:150
[mutex-flags]: /Users/jinqinghui/linux-6.18/kernel/locking/mutex.h:32
[mutex-spin]: /Users/jinqinghui/linux-6.18/kernel/locking/mutex.c:429
[mutex-type]: /Users/jinqinghui/linux-6.18/include/linux/mutex_types.h:41
[mutex-unlock]: /Users/jinqinghui/linux-6.18/kernel/locking/mutex.c:915
[mutex-unlock-fast]: /Users/jinqinghui/linux-6.18/kernel/locking/mutex.c:163
[osq]: /Users/jinqinghui/linux-6.18/kernel/locking/osq_lock.c:93
[percpu-read]: /Users/jinqinghui/linux-6.18/include/linux/percpu-rwsem.h:48
[percpu-ref]: /Users/jinqinghui/linux-6.18/include/linux/percpu-refcount.h:3
[percpu-unlock]: /Users/jinqinghui/linux-6.18/kernel/locking/percpu-rwsem.c:262
[percpu-write]: /Users/jinqinghui/linux-6.18/kernel/locking/percpu-rwsem.c:227
[qrw-fast]: /Users/jinqinghui/linux-6.18/include/asm-generic/qrwlock.h:78
[qrw-slow]: /Users/jinqinghui/linux-6.18/kernel/locking/qrwlock.c:21
[qspin-fast]: /Users/jinqinghui/linux-6.18/include/asm-generic/qspinlock.h:107
[qspin-slow]: /Users/jinqinghui/linux-6.18/kernel/locking/qspinlock.c:130
[qspin-type]: /Users/jinqinghui/linux-6.18/include/asm-generic/qspinlock_types.h:14
[qspin-unlock]: /Users/jinqinghui/linux-6.18/include/asm-generic/qspinlock.h:123
[rcu-assign]: /Users/jinqinghui/linux-6.18/include/linux/rcupdate.h:588
[rcu-batch]: /Users/jinqinghui/linux-6.18/kernel/rcu/tree.c:2528
[rcu-call]: /Users/jinqinghui/linux-6.18/kernel/rcu/tree.c:3241
[rcu-gp]: /Users/jinqinghui/linux-6.18/kernel/rcu/tree.c:2259
[rcu-read]: /Users/jinqinghui/linux-6.18/include/linux/rcupdate.h:863
[rcu-sync]: /Users/jinqinghui/linux-6.18/kernel/rcu/tree.c:3341
[rcu-tree-type]: /Users/jinqinghui/linux-6.18/kernel/rcu/tree.h:41
[refcount]: /Users/jinqinghui/linux-6.18/include/linux/refcount.h:132
[rt-block]: /Users/jinqinghui/linux-6.18/kernel/locking/rtmutex.c:1203
[rt-chain]: /Users/jinqinghui/linux-6.18/kernel/locking/rtmutex.c:678
[rt-type]: /Users/jinqinghui/linux-6.18/include/linux/rtmutex.h:23
[rt-wake]: /Users/jinqinghui/linux-6.18/kernel/locking/rtmutex.c:1312
[rwbase-rt]: /Users/jinqinghui/linux-6.18/kernel/locking/rwbase_rt.c:4
[rwsem-count]: /Users/jinqinghui/linux-6.18/kernel/locking/rwsem.c:119
[rwsem-read-fast]: /Users/jinqinghui/linux-6.18/kernel/locking/rwsem.c:249
[rwsem-read-slow]: /Users/jinqinghui/linux-6.18/kernel/locking/rwsem.c:993
[rwsem-up-read]: /Users/jinqinghui/linux-6.18/kernel/locking/rwsem.c:1349
[rwsem-up-write]: /Users/jinqinghui/linux-6.18/kernel/locking/rwsem.c:1371
[rwsem-wake]: /Users/jinqinghui/linux-6.18/kernel/locking/rwsem.c:410
[rwsem-write-fast]: /Users/jinqinghui/linux-6.18/kernel/locking/rwsem.c:264
[rwsem-write-slow]: /Users/jinqinghui/linux-6.18/kernel/locking/rwsem.c:1111
[sem-down]: /Users/jinqinghui/linux-6.18/kernel/locking/semaphore.c:91
[sem-up]: /Users/jinqinghui/linux-6.18/kernel/locking/semaphore.c:220
[seq-associated]: /Users/jinqinghui/linux-6.18/include/linux/seqlock.h:144
[seq-begin]: /Users/jinqinghui/linux-6.18/include/linux/seqlock.h:296
[seq-latch]: /Users/jinqinghui/linux-6.18/include/linux/seqlock.h:696
[seq-retry]: /Users/jinqinghui/linux-6.18/include/linux/seqlock.h:408
[seqlock-write]: /Users/jinqinghui/linux-6.18/include/linux/seqlock.h:874
[sock-lock]: /Users/jinqinghui/linux-6.18/net/core/sock.c:3717
[spin-api]: /Users/jinqinghui/linux-6.18/include/linux/spinlock_api_smp.h:130
[spin-rt]: /Users/jinqinghui/linux-6.18/kernel/locking/spinlock_rt.c:46
[spin-wrap]: /Users/jinqinghui/linux-6.18/include/linux/spinlock.h:349
[srcu-read]: /Users/jinqinghui/linux-6.18/kernel/rcu/srcutree.c:752
[srcu-sync]: /Users/jinqinghui/linux-6.18/kernel/rcu/srcutree.c:1532
[sysv-sem]: /Users/jinqinghui/linux-6.18/ipc/sem.c:719
[ticket]: /Users/jinqinghui/linux-6.18/include/asm-generic/ticket_spinlock.h:33
[u64-stats]: /Users/jinqinghui/linux-6.18/include/linux/u64_stats_sync.h:6
[vma-lock]: /Users/jinqinghui/linux-6.18/include/linux/mmap_lock.h:205
[wait-bit]: /Users/jinqinghui/linux-6.18/kernel/sched/wait_bit.c:85
[wait-event]: /Users/jinqinghui/linux-6.18/include/linux/wait.h:302
[wait-prepare]: /Users/jinqinghui/linux-6.18/kernel/sched/wait.c:290
[ww-alg]: /Users/jinqinghui/linux-6.18/kernel/locking/ww_mutex.h:165
[ww-api]: /Users/jinqinghui/linux-6.18/include/linux/ww_mutex.h:220
[x86-spin]: /Users/jinqinghui/linux-6.18/arch/x86/include/asm/spinlock.h:27
