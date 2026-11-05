# 第 7 章 同步与锁

本章分析 Linux v6.18.60 在 x86_64 上的同步机制，源码主要位于 `kernel/locking/`、`kernel/rcu/`、`kernel/futex/`、`include/linux/` 与 `arch/x86/include/asm/`。

内核是一个高度并发的程序：多个 CPU 真正并行执行，同一 CPU 上又有抢占、软中断、硬中断和 NMI 相互打断。本章沿"并发从哪里来 → 最底层的原子操作与内存序 → 自旋类锁 → 睡眠类锁 → 无锁读的 RCU → 调试工具"的主线展开，既分析每种原语的实现，也说明它们各自适用的场景。

```
                          ┌───────────── 7.11 lockdep 与锁调试 ─────────────┐
                          │                                                 │
   睡眠锁  7.6 mutex ── 7.7 semaphore / rwsem / percpu-rwsem ── 7.8 rt_mutex / futex
              │  乐观自旋（OSQ）             │                     │ 优先级继承
              ▼                             ▼                     ▼
   自旋锁  7.4 spinlock（qspinlock） ── 7.5 rwlock（qrwlock）/ seqlock
              │                                                    
              ▼                                   无锁读  7.9 RCU 原理 ── 7.10 Tree RCU
   基础    7.2 原子操作 / refcount ── 7.3 内存屏障与 LKMM                     │
              │                                                             │
              └────────── 7.1 执行上下文、preempt_count、per-CPU ──────────────┘
```

*图 7-0：本章各小节的关系*

## 阅读建议

- 7.1～7.3 是全章基础：7.1 回答"代码处于什么上下文、需要防谁"，7.2、7.3 提供构建一切锁的原子操作与内存序语义。建议按顺序阅读。
- 7.4、7.5 是自旋类锁，7.4 的 qspinlock 是本章分析最细的锁实现。
- 7.6～7.8 是睡眠类锁，7.6 的 mutex 是 7.7 rwsem 的基础；7.8 与第 3 章的实时调度类联系紧密。
- 7.9、7.10 讨论 RCU，7.9 偏使用，7.10 偏实现，可以只读 7.9。
- 7.11 介绍 lockdep 与生产环境中的锁问题诊断，适合在遇到实际问题时查阅。

## 本章配置

本章以 `linux/.config` 为准，与同步相关的主要配置项如下：

| 配置项 | 取值 | 影响 |
|---|---|---|
| `CONFIG_SMP` / `CONFIG_NR_CPUS` | y / 8192 | 多核；qspinlock pending 域占 8 位、Tree RCU 编译期为三层 |
| `CONFIG_PREEMPT_LAZY` / `CONFIG_PREEMPT_DYNAMIC` | y / y | 内核可抢占（3.5 节），`preempt_count` 生效 |
| `CONFIG_PREEMPT_RT` | n | `spinlock_t`、`mutex` 等不替换为 rt_mutex |
| `CONFIG_QUEUED_SPINLOCKS` / `CONFIG_QUEUED_RWLOCKS` | y / y | 自旋锁、读写锁分别由 qspinlock、qrwlock 实现 |
| `CONFIG_PARAVIRT_SPINLOCKS` | y | 虚拟机中可使用 `pv_wait` / `pv_kick` |
| `CONFIG_UNINLINE_SPIN_UNLOCK` | y | `_raw_spin_unlock()` 不内联 |
| `CONFIG_MUTEX_SPIN_ON_OWNER` / `CONFIG_RWSEM_SPIN_ON_OWNER` | y / y | mutex、rwsem 乐观自旋 |
| `CONFIG_RT_MUTEXES` / `CONFIG_FUTEX_PI` | y / y | rt_mutex 与 PI futex |
| `CONFIG_FUTEX_PRIVATE_HASH` | y | 进程私有 futex 哈希表 |
| `CONFIG_TREE_RCU` / `CONFIG_PREEMPT_RCU` | y / y | 可抢占的 Tree RCU |
| `CONFIG_RCU_NOCB_CPU` / `CONFIG_RCU_LAZY` | y / y | 支持回调卸载与惰性 RCU（惰性默认关闭） |
| `CONFIG_RCU_BOOST` | n | 无 RCU 读者优先级提升 |
| `CONFIG_TREE_SRCU` / `CONFIG_TASKS_*RCU` | y | SRCU 与 Tasks RCU 系列 |
| `CONFIG_PROVE_LOCKING` / `CONFIG_DEBUG_LOCK_ALLOC` / `CONFIG_LOCK_STAT` | n / n / n | lockdep 未开启，锁调试钩子为空 |
| `CONFIG_DEBUG_ATOMIC_SLEEP` | n | `might_sleep()` 不检查原子上下文 |
| `CONFIG_KCSAN` | n | 无数据竞争检测 |
| `CONFIG_DETECT_HUNG_TASK` / `CONFIG_DETECT_HUNG_TASK_BLOCKER` | y / y | hung task 报告附带阻塞锁信息 |
| `CONFIG_SOFTLOCKUP_DETECTOR` / `CONFIG_HARDLOCKUP_DETECTOR` | y / y | 软 / 硬锁死检测 |

## 小节索引

| 小节 | 状态 |
|---|---|
| [7.1 并发来源与执行上下文](01-context.md) | 已完成 |
| [7.2 原子操作、位操作与引用计数](02-atomic.md) | 已完成 |
| [7.3 内存屏障与 Linux 内核内存模型](03-memory-model.md) | 已完成 |
| [7.4 自旋锁与 qspinlock](04-spinlock.md) | 已完成 |
| [7.5 读写锁与顺序锁](05-rwlock-seqlock.md) | 已完成 |
| [7.6 互斥锁与乐观自旋](06-mutex.md) | 已完成 |
| [7.7 信号量与读写信号量](07-rwsem.md) | 已完成 |
| [7.8 rt_mutex、优先级继承与 futex](08-rtmutex-futex.md) | 已完成 |
| [7.9 RCU 原理与使用](09-rcu-basics.md) | 已完成 |
| [7.10 Tree RCU：宽限期检测与回调处理](10-tree-rcu.md) | 已完成 |
| [7.11 lockdep 与锁调试](11-lockdep.md) | 已完成 |

## 待完善

- [ ] 各小节"动手实验"尚未在 dev 虚拟机中实测，需补充真实输出示例（注明内核版本与配置）。
- [ ] 7.1、7.11 的部分实验（irqsoff tracer、`might_sleep` 检查、lockdep ABBA 报告）需要另行编译开启 `CONFIG_IRQSOFF_TRACER`、`CONFIG_DEBUG_ATOMIC_SLEEP`、`CONFIG_PROVE_LOCKING` 的内核。
- [ ] 图 7-4（qspinlock 时序）、图 7-8（RCU 宽限期）、图 7-10（静止状态上报）的 Mermaid 图需在目标渲染器中确认显示效果。
- [ ] 部分特性的引入版本（如 qrwlock、mutex HANDOFF、GP 线程化、`synchronize_rcu()` 由 GP 线程直接唤醒）依据社区资料整理，后续可补充对应的提交号以便核对。
- [ ] 部分代码块（如 [`include/linux/spinlock_api_smp.h:104-161`](../../linux/include/linux/spinlock_api_smp.h#L104-L161)、[`arch/x86/include/asm/cmpxchg.h:158-215`](../../linux/arch/x86/include/asm/cmpxchg.h#L158-L215)）超过 40 行建议值，可进一步拆分讲解。
- [ ] 完成量（completion）、等待队列与 `wait_event()` 系列尚未单独成节（睡眠—唤醒协议见 3.4 节），可考虑在本章或第 3 章补充。
