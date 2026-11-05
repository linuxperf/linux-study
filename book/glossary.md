# 术语表

按中文拼音排序。"首次出现"指全书第一次使用该术语的小节。

| 中文 | 英文 | 说明 | 首次出现 |
|---|---|---|---|
| balance 回调 | Balance Callback | 调度类持 rq 锁时登记、在上下文切换完成后释放 rq 锁前执行的推 / 拉迁移操作 | 3.2 |
| BH 工作队列 | BH Workqueue | 在软中断上下文执行工作项的工作队列（`WQ_BH`，v6.9），用于替代 tasklet | 5.10 |
| CPU 带宽控制 | CFS Bandwidth Control | 以 quota / period 为任务组设置 CPU 时间上限 | 3.10 |
| fork / exec 均衡 | Fork / Exec Balancing | 新任务第一次入队前（`WF_FORK`）或 `execve()` 时（`WF_EXEC`）为任务选择 CPU | 3.4 |
| I/O APIC | I/O Advanced Programmable Interrupt Controller | 把传统中断引脚翻译为中断消息的芯片组中断控制器 | 5.5 |
| managed 中断 | Managed Interrupt | 亲和性由内核按拓扑计算且不可修改的多队列设备中断 | 5.8 |
| misfit 任务 | Misfit Task | 非对称算力系统上利用率超出所在 CPU 算力、应迁往更大核的任务 | 3.8 |
| per-CPU 页缓存 | Per-CPU Pageset，PCP | 每 CPU、每 zone 的小阶空闲页缓存，`struct per_cpu_pages` | 4.3 |
| steal 时间 | Steal Time | 虚拟机中 vCPU 可运行却被宿主机调度走的时间 | 3.1 |
| tick 依赖 | Tick Dependency | 阻止 `NO_HZ_FULL` CPU 停 tick 的原因位掩码，如 `TICK_DEP_BIT_SCHED` | 3.5 |
| 按需分配 | Demand Paging | mmap 只建立 VMA，物理页在首次访问的缺页中分配 | 4.7 |
| 保护页 | Guard Page | vmalloc 区域末尾不映射的一页，用于捕获越界访问 | 4.6 |
| 保留模式 | Reservation Mode | 中断分配时只记账、激活时才占用真实向量的向量分配策略 | 5.8 |
| 本地 APIC | Local APIC，LAPIC | 每个 CPU 内的中断控制器，负责接收与投递中断、发送 IPI、提供本地定时器 | 5.1 |
| 比较并交换 | Compare-And-Swap，CAS | 若内存值等于预期则写入新值的原子操作，x86 为 `cmpxchg` | 7.2 |
| 并发管理工作队列 | Concurrency Managed Workqueue，CMWQ | 所有工作队列共享 worker 池、按运行中 worker 数动态调整并发的工作队列实现 | 5.12 |
| 不可屏蔽中断 | Non-Maskable Interrupt，NMI | 不受 `RFLAGS.IF` 屏蔽的中断，向量 2 | 5.1 |
| 布隆过滤器 | Bloom Filter | MGLRU 老化时记录值得扫描的 PMD，跳过冷区域 | 4.10 |
| 层 | Tier | MGLRU 代内按文件描述符访问次数的细分 | 4.10 |
| 程序序 | Program Order | 任务迁移或睡眠唤醒前后，其操作保持程序顺序的保证 | 3.4 |
| 处理器间中断 | Inter-Processor Interrupt，IPI | 一个 CPU 经本地 APIC 向其他 CPU 投递的中断 | 4.6 |
| 次缺页 | Minor Fault | 无需 I/O 即可完成的缺页，计入 `min_flt` | 4.8 |
| 存储缓冲区 | Store Buffer | CPU 内暂存待提交写操作的缓冲，导致 x86 上的写后读重排 | 7.3 |
| 错误码 | Error Code | 部分异常由 CPU 额外压栈的 32 位信息，如 #PF 的访问类型 | 5.1 |
| 大页 | Huge Page | 以 PMD（2 MB）或 PUD（1 GB）直接映射的页 | 4.13 |
| 代 | Generation | MGLRU 中按年龄划分的页集合，以 `max_seq`/`min_seq` 编号 | 4.10 |
| 代理执行 | Proxy Execution | 让持锁任务借用等待者的调度上下文运行（`CONFIG_SCHED_PROXY_EXEC`） | 3.1 |
| 等待队列 | Wait Queue | 以 `wait_queue_head` 组织的睡眠任务链表，由 `wake_up()` 系列逐个调用唤醒函数 | 3.2 |
| 调度策略 | Scheduling Policy | `SCHED_NORMAL`、`SCHED_FIFO` 等，用户态可见 | 3.1 |
| 调度队列 | Dispatch Queue，DSQ | sched_ext 中承载待运行任务的队列 | 3.11 |
| 调度类 | Scheduling Class | `struct sched_class`，实现某类调度策略的方法集合 | 3.1 |
| 调度器 | Scheduler | — | 3.1 |
| 调度实体 | Scheduling Entity | 调度类操作的对象，如 `sched_entity`，可代表任务或任务组 | 3.1 |
| 调度时钟中断 | Scheduler Tick | 以 `HZ` 频率触发、驱动 `sched_tick()` 的周期性时钟中断，正文简称 tick | 3.1 |
| 调度域 | Scheduling Domain | 按硬件拓扑组织的 CPU 层级，`struct sched_domain` | 3.9 |
| 调度组 | Scheduling Group | 调度域内参与负载比较的 CPU 子集，`struct sched_group` | 3.9 |
| 丢失唤醒 | Lost Wakeup | 等待方检查条件与睡眠之间错过唤醒的竞态 | 3.2 |
| 动态抢占 | Dynamic Preemption | `CONFIG_PREEMPT_DYNAMIC`，用 static call / static key 在启动或运行时切换抢占模型 | 3.5 |
| 抖动 | Thrashing | 工作集超过内存，频繁回收与重新读入导致系统几乎停滞 | 4.10 |
| 读拷贝更新 | Read-Copy Update，RCU | 读者无锁、写者发布新版本并在宽限期后回收旧版本的同步机制 | 7.9 |
| 读写锁 | Reader-Writer Lock | 允许多个读者或一个写者的自旋锁，`rwlock_t` | 7.5 |
| 读写信号量 | Reader-Writer Semaphore，rwsem | 可睡眠的读写锁，`struct rw_semaphore` | 7.7 |
| 多尺寸 THP | Multi-size THP，mTHP | 介于 4 KB 与 2 MB 之间的匿名大 folio（v6.8） | 4.8 |
| 多代 LRU | Multi-Gen LRU，MGLRU | v6.1 起的多代页面回收算法（`CONFIG_LRU_GEN`） | 4.10 |
| 惰性抢占 | Lazy Preemption | lazy 抢占模型下以 `TIF_NEED_RESCHED_LAZY` 标记、推迟到返回用户态或下一个 tick 才执行的抢占 | 3.2 |
| 惰性 TLB | Lazy TLB | 内核线程借用上一任务地址空间而不切换页表 | 3.2 |
| 返回栈缓冲区 | Return Stack Buffer，RSB | CPU 预测 `ret` 目标地址的硬件栈，上下文切换时需填充以缓解推测执行攻击 | 3.2 |
| 反向映射 | Reverse Mapping，rmap | 由物理页找到所有映射它的页表项 | 4.9 |
| 非对称算力 | Asymmetric CPU Capacity | 各 CPU 最大算力不同的系统，如大小核 | 3.12 |
| 非自愿上下文切换 | Involuntary Context Switch | 因抢占发生的切换，计入 `nivcsw` | 3.2 |
| 分层中断域 | Hierarchical IRQ Domain | 以父子关系串联多级中断控制器的 `irq_domain` 结构 | 5.6 |
| 复合页 | Compound Page | 多个连续页组成的整体，由头页与尾页构成 | 4.1 |
| 负载均衡 | Load Balancing | 在 CPU 之间迁移任务以纠正负载不均 | 3.9 |
| 负载均值 | Load Average | `/proc/loadavg` 中可运行与不可中断睡眠任务数的 1、5、15 分钟指数平均 | 3.5 |
| 高精度定时器 | High-Resolution Timer，hrtimer | 纳秒精度的内核定时器，模拟的周期 tick 与 hrtick 都基于它实现 | 3.2 |
| 工作队列 | Workqueue | 在内核线程中执行可睡眠延迟工作的机制 | 4.3 |
| 工作项 | Work Item | 提交给工作队列的函数及其数据，`struct work_struct` | 5.12 |
| 故障 / 陷阱 / 终止 | Fault / Trap / Abort | 按返回地址与可恢复性划分的三类异常 | 5.1 |
| 规范地址 | Canonical Address | 高位为最高有效位符号扩展的 x86_64 虚拟地址 | 4.6 |
| 过度利用 | Overutilized | EAS 中有 CPU 利用率超出其算力的状态，此时唤醒放置交还给负载均衡 | 3.12 |
| 合格 | Eligible | EEVDF 中 lag ≥ 0（`vruntime ≤ V`）的实体才可被选中 | 3.3 |
| 核心调度 | Core Scheduling | 控制 SMT 兄弟线程上可同时运行的任务（`CONFIG_SCHED_CORE`） | 3.1 |
| 恒定带宽服务器 | Constant Bandwidth Server，CBS | 限制每个 Deadline 实体带宽的预算机制 | 3.7 |
| 后备存储 | Backing Store | 页内容的持久化位置：文件页为文件，匿名页为交换空间 | 4.11 |
| 互斥锁 | Mutex | 可睡眠的互斥锁，`struct mutex`，支持乐观自旋 | 7.6 |
| 唤醒队列 | Wake List | `TTWU_QUEUE` 机制下挂在目标 CPU 上、经 IPI 处理的待唤醒任务链表 | 3.4 |
| 唤醒亲和 | Wake Affine | 唤醒时在唤醒者所在 CPU 与任务原 CPU 之间选定搜索空闲 CPU 的区域 | 3.12 |
| 回收 | Reclaim | 释放已用页以补充空闲内存，执行者为 kswapd 与直接回收 | 4.4 |
| 伙伴系统 | Buddy System | 以 2 的幂次块管理空闲物理页的分配器 | 4.1 |
| 获取 / 释放 | Acquire / Release | 单向内存序：获取之后的访问不提前，释放之前的访问不推后 | 7.3 |
| 计费 | Charge | 把内存用量计入某个 memcg | 4.14 |
| 机器检查异常 | Machine Check Exception，#MC | CPU 检测到硬件错误时触发的异常，向量 18 | 5.4 |
| 基于对象的反向映射 | Object-based Reverse Mapping | 经 `folio->mapping` 与 `index` 计算虚拟地址的 rmap 方案 | 4.9 |
| 交换表 | Swap Table | v6.18 起每 cluster 一页的交换缓存数组 | 4.11 |
| 交换槽位 | Swap Slot | 交换设备上存放一页的位置 | 4.11 |
| 交换缓存 | Swap Cache | 交换槽位到 folio 的映射，同步换入换出 | 4.11 |
| 交换空间 | Swap Space | 存放被换出匿名页的块设备或文件 | 4.11 |
| 交换项 | Swap Entry | 写入不存在 PTE 中的（设备号，偏移）编码 | 4.11 |
| 节点 | Node | 一组访问延迟相同的内存，`pg_data_t` | 4.1 |
| 阶段公平 | Phase-Fair | 读写锁唤醒策略：读阶段让所有等待的读者一起进入 | 7.7 |
| 节流 | Throttle | 实体或任务组用尽预算后暂停运行 | 3.7 |
| 进程上下文标识符 | Process-Context Identifier，PCID | x86 TLB 项的地址空间标签，切换 CR3 时可保留其他地址空间的 TLB 项 | 3.2 |
| 静止状态 | Quiescent State | RCU 术语，CPU 不处于读临界区的时刻 | 3.2 |
| 救援线程 | Rescuer | `WQ_MEM_RECLAIM` 工作队列专属的 worker，保证内存回收路径前进 | 5.12 |
| 局部向量表 | Local Vector Table，LVT | 本地 APIC 中为定时器、性能计数器等本地中断源配置向量的寄存器组 | 5.5 |
| 空闲核 | Idle Core | 所有 SMT 兄弟线程都空闲的物理核 | 3.12 |
| 宽限期 | Grace Period | 所有 CPU 都至少经历一次静止状态的时间段 | 7.9 |
| 扩展静止状态 | Extended Quiescent State，EQS | CPU 处于 idle 或 NO_HZ_FULL 用户态，RCU 不关注它 | 7.10 |
| 老化 | Aging | MGLRU 中新建一代并提升近期访问页的过程 | 4.10 |
| 乐观自旋 | Optimistic Spinning | 睡眠锁的持有者在运行时，等待者先自旋而非睡眠 | 7.6 |
| 灵活返回与事件投递 | Flexible Return and Event Delivery，FRED | 取代 IDT 事件投递与 IRET 的新架构（`CONFIG_X86_FRED`，v6.9） | 5.1 |
| 零松弛 | Zero-Laxity | 延迟 dl_server 的激活时刻 `deadline − runtime`，即再推迟就无法在截止时间前用完预算 | 3.7 |
| 零页 | Zero Page | 全局共享的全零只读页，匿名读缺页映射到它 | 4.8 |
| 流控处理函数 | Flow Handler | 按触发方式完成应答、屏蔽、EOI 的中断处理函数，`desc->handle_irq` | 5.6 |
| 门描述符 | Gate Descriptor | IDT 中的 16 字节表项，记录入口地址、IST 下标与 DPL | 5.1 |
| 末级缓存 | Last Level Cache，LLC | 一组 CPU 共享的最后一级缓存，调度器以此划分 `sd_llc` 域 | 3.4 |
| 内存保护键 | Protection Keys，PKEY | PTE 中 4 位键与 PKRU 寄存器配合的访问控制 | 4.8 |
| 内存不足杀手 | Out-Of-Memory Killer，OOM Killer | 内存耗尽时选择并杀死进程 | 4.12 |
| 内存规整 | Memory Compaction | 迁移可移动页以合并出连续空闲块 | 4.12 |
| 内存控制组 | Memory Cgroup，memcg | 按 cgroup 计费、限制与保护内存 | 4.14 |
| 内存模型 | Memory Model | 规定并发访问可观察结果的规则；内核使用 Linux 内核内存模型（LKMM） | 7.3 |
| 内存屏障 | Memory Barrier | 约束编译器与 CPU 对内存访问重排的指令或原语 | 7.3 |
| 内存域 | Zone | 节点内按地址范围划分的区域，如 `ZONE_DMA32` | 4.1 |
| 能效感知调度 | Energy Aware Scheduling，EAS | 在非对称算力平台上按能耗选择 CPU | 3.8 |
| 票号锁 | Ticket Lock | 按取号顺序获取的公平自旋锁，x86 上已被 qspinlock 取代 | 7.4 |
| 迁移类型 | Migrate Type | 按页块记录的可移动性分类（不可移动、可移动、可回收等） | 4.1 |
| 迁移项 | Migration Entry | 页迁移期间替代 PTE 的特殊不存在项 | 4.11 |
| 抢占 | Preemption | 在任务未主动让出时强制切换到其他任务 | 3.1 |
| 抢占模型 | Preemption Model | none / voluntary / full / lazy，决定哪些抢占点生效、惰性请求设置哪个标志位 | 3.5 |
| 抢占点 | Preemption Point | 检查重调度标志并可能进入调度器的位置，如 `preempt_enable()`、中断返回、返回用户态 | 3.2 |
| 强制线程化 | Forced Threading | 把普通中断处理函数放到中断线程中执行（`threadirqs`） | 5.7 |
| 亲和性范围 | Affinity Scope | unbound 工作队列按 CPU 拓扑划分 pod 的粒度（v6.5） | 5.12 |
| 区间树 | Interval Tree | 按区间重叠查询的增强红黑树，用于 `i_mmap` 与 anon_vma | 4.9 |
| 驱逐 | Eviction | MGLRU 中从最老一代回收页的过程 | 4.10 |
| 全序存储 | Total Store Order，TSO | x86 的内存模型，仅允许写后读重排 | 7.3 |
| 权重 | Load Weight | 公平调度类按其比例分配 CPU 时间，由 nice 值决定 | 3.1 |
| 缺页异常 | Page Fault，#PF | 访问未映射或权限不符的地址时 CPU 触发的异常 | 4.8 |
| 任务优先级寄存器 | Task Priority Register，TPR | 本地 APIC 中屏蔽低优先级类中断的寄存器，Linux 固定为 `0x10` | 5.5 |
| 任务状态段 | Task State Segment，TSS | x86_64 上提供 `sp0` 与 IST 栈指针的结构 | 5.1 |
| 入口栈 | Entry Stack | cpu_entry_area 中供用户态进入内核时使用的跳板栈 | 4.6 |
| 软锁死 / 硬锁死 | Soft Lockup / Hard Lockup | CPU 长时间不可调度 / 长时间不响应中断 | 7.11 |
| 软中断 | Softirq | 静态定义的 per-CPU 下半部，在硬中断退出或 ksoftirqd 中执行 | 7.1 |
| 上半部 / 下半部 | Top Half / Bottom Half | 中断处理中必须立即完成的部分与可延迟执行的部分 | 5.9 |
| 上下文跟踪 | Context Tracking | 记录 CPU 处于用户态、内核态或 idle，供 RCU 与时间统计使用 | 7.10 |
| 上下文切换 | Context Switch | — | 3.1 |
| 时间片 | Slice | EEVDF 中任务每次请求的运行长度，`se->slice` | 3.3 |
| 时间片保护 | Run to Parity | `RUN_TO_PARITY`，选中后在保护期内不被轻易切走 | 3.3 |
| 实时内核 | Real-Time Kernel，PREEMPT_RT | 开启 `CONFIG_PREEMPT_RT` 的内核：`spinlock_t` 等换成基于 rt_mutex 的可睡眠锁，中断强制线程化 | 3.2 |
| 首次适配 | First Fit | 在空闲区间中选择第一个足够大的区间 | 4.6 |
| 数据竞争 | Data Race | 至少一方为普通写的并发访问，LKMM 中行为未定义 | 7.3 |
| 双重故障 | Double Fault，#DF | 投递异常过程中再次发生异常，x86_64 上常由栈溢出引起 | 5.1 |
| 水位线 | Watermark | zone 的 min/low/high 空闲页阈值 | 4.4 |
| 顺序锁 | Sequence Lock | 读者凭序号检测并发写入后重试的读写同步机制，`seqlock_t` / `seqcount_t` | 7.5 |
| 碎片化指数 | Fragmentation Index | 判断分配失败源于内存不足还是碎片化的指标 | 4.12 |
| 锁持有者抢占 | Lock Holder Preemption | 虚拟机中持锁 vCPU 被宿主机调度出去，其他 vCPU 空转 | 7.4 |
| 锁类 | Lock Class | lockdep 中按初始化位置归类的一组锁 | 7.11 |
| 锁依赖验证器 | Lock Dependency Validator，lockdep | 运行时记录加锁顺序并检测潜在死锁的调试工具 | 7.11 |
| 提交记账 | Commit Accounting | 统计私有可写映射承诺的内存量，`Committed_AS` | 4.7 |
| 透明大页 | Transparent Huge Page，THP | 缺页时自动尝试分配大页，失败回退 | 4.13 |
| 偷锁 | Lock Stealing | 新来者绕过等待队列直接获取锁 | 7.7 |
| 头页 / 尾页 | Head Page / Tail Page | 复合页的第一页与其余页 | 4.1 |
| 推 / 拉 | Push / Pull | 实时与 Deadline 类在 CPU 间主动迁移任务的两种操作 | 3.6 |
| 外部碎片 | External Fragmentation | 空闲页总量足够但缺少连续块 | 4.12 |
| 完全公平调度器 | Completely Fair Scheduler，CFS | v2.6.23～v6.5 的公平调度算法 | 3.3 |
| 完全抢占 | Full Preemption | full 抢占模型：`preempt_count` 为 0 的内核代码随时可被抢占（`CONFIG_PREEMPT`） | 3.5 |
| 未接受内存 | Unaccepted Memory | 机密虚拟机中须显式接受后才能使用的内存 | 4.2 |
| 伪共享 | False Sharing | 不同 CPU 频繁写入同一缓存行中的不同数据，导致该缓存行在 CPU 间反复迁移 | 3.1 |
| 伪唤醒 | Spurious Wakeup | 被唤醒时等待条件并不成立的唤醒，等待方须在循环中重新检查条件 | 3.2 |
| 伪中断 | Spurious Interrupt | 本地 APIC 在竞态下投递的无对应中断源的中断，向量 `0xff` | 5.1 |
| 无 tick 模式 | Dynticks，NO_HZ | 不需要时停掉周期性 tick：空闲时停（`NO_HZ_IDLE`），或单任务运行时也停（`NO_HZ_FULL`） | 3.5 |
| 稀疏内存模型 | SPARSEMEM | 按 section 分配 `struct page` 的物理内存模型 | 4.1 |
| 线程化中断 | Threaded IRQ | 主处理函数唤醒专属内核线程执行主体工作的中断处理方式 | 5.7 |
| 向量 | Vector | x86 标识中断与异常的 0～255 编号，用于索引 IDT | 5.1 |
| 消息信号中断 | Message Signaled Interrupts，MSI / MSI-X | 设备通过写特定地址触发中断的机制 | 5.5 |
| 写时复制 | Copy-On-Write，COW | 共享只读页，写入时再复制 | 4.9 |
| 信号量 | Semaphore | 允许至多 N 个持有者的睡眠锁，`struct semaphore` | 7.7 |
| 性能域 | Performance Domain | 共享同一频率的一组 CPU，EAS 以它为单位估算能耗 | 3.12 |
| 修正唤醒规则 | Revised Wakeup Rule | 受约束 Deadline 任务唤醒时削减预算而非重开周期 | 3.7 |
| 虚拟截止时间 | Virtual Deadline | `vruntime + slice / weight` | 3.3 |
| 虚拟内存区域 | Virtual Memory Area，VMA | 进程地址空间中属性相同的一段区间，`vm_area_struct` | 4.7 |
| 虚拟时间 | Virtual Time，V | 所有实体 vruntime 的加权平均 | 3.3 |
| 虚拟运行时间 | Virtual Runtime，vruntime | 按权重折算后的运行时间 | 3.3 |
| 延迟出队 | Delayed Dequeue | EEVDF 中负 lag 任务睡眠时暂留队列的机制 | 3.1 |
| 页表隔离 | Page Table Isolation，PTI | 用户态使用不映射大部分内核的页表，缓解 Meltdown | 4.6 |
| 页块 | Pageblock | 记录迁移类型的页组，x86_64 上为 2 MB | 4.1 |
| 页迁移 | Page Migration | 把页内容与映射转移到新物理页 | 4.12 |
| 页帧 | Page Frame | 物理内存中的一页，用 `struct page` 描述 | 4.1 |
| 页帧号 | Page Frame Number，PFN | 物理地址右移 `PAGE_SHIFT` 得到的页编号 | 4.1 |
| 异常 | Exception | 由当前执行的指令引起的同步事件 | 5.1 |
| 异常表 | Exception Table | 记录允许出错的内核指令及其修复地址的表，`__ex_table` | 5.3 |
| 移交 | Handoff | 解锁者把锁直接交给指定的等待者，防止饥饿 | 7.6 |
| 硬件中断号 | Hardware IRQ Number，hwirq | 中断控制器本地的输入编号，在 `irq_domain` 内唯一 | 5.6 |
| 硬中断栈 | Hardirq Stack | 每 CPU 16 KB 的中断处理栈，`hardirq_stack_ptr` | 5.1 |
| 有界拖延 | Bounded Tardiness | 多核全局 EDF 下任务完成时间超过截止时间的量有上界 | 3.7 |
| 优先级反转 | Priority Inversion | 高优先级任务因等待低优先级任务持有的资源而被中优先级任务间接阻塞 | 3.6 |
| 优先级继承 | Priority Inheritance，PI | 持锁的低优先级任务临时继承等待者的优先级 | 3.1 |
| 原子操作 | Atomic Operation | 对其他 CPU 不可分割的读改写操作 | 7.2 |
| 原子上下文 | Atomic Context | 不允许睡眠的执行环境：持自旋锁、关抢占、中断等 | 7.1 |
| 运行队列 | Runqueue，rq | 每 CPU 一个的 `struct rq` | 3.1 |
| 增强红黑树 | Augmented RB-tree | 节点额外维护子树聚合信息的红黑树 | 3.3 |
| 直接回收 | Direct Reclaim | 分配者在慢速路径中自行回收内存 | 4.4 |
| 直接映射区 | Direct Map | 线性映射全部物理内存的内核虚拟地址区域，起始于 `PAGE_OFFSET` | 4.1 |
| 指数加权移动平均 | Exponentially Weighted Moving Average，EWMA | util_est 用于平滑利用率下降 | 3.8 |
| 中断 | Interrupt | 来自 CPU 外部或其他 CPU 的异步事件 | 5.1 |
| 中断重映射 | Interrupt Remapping | IOMMU 以表项转换中断消息的机制，`CONFIG_IRQ_REMAP` | 5.5 |
| 中断结束 | End Of Interrupt，EOI | 通知中断控制器本次中断处理完毕 | 5.2 |
| 中断门 / 陷阱门 | Interrupt Gate / Trap Gate | 进入时是否自动清除 `IF` 的两种 IDT 门类型 | 5.1 |
| 中断描述符 | IRQ Descriptor | 每个 Linux 中断号一个的 `struct irq_desc` | 5.6 |
| 中断描述符表 | Interrupt Descriptor Table，IDT | 以向量为下标的门描述符数组 | 5.1 |
| 中断亲和性 | IRQ Affinity | 允许接收某中断的 CPU 集合，`/proc/irq/N/smp_affinity` | 5.5 |
| 中断芯片 | IRQ Chip | 中断控制器的操作集合，`struct irq_chip` | 5.6 |
| 中断域 | IRQ Domain | hwirq 与 Linux 中断号的映射及控制器层级，`struct irq_domain` | 5.6 |
| 中断栈表 | Interrupt Stack Table，IST | TSS 中 7 个备用栈指针，供 NMI、#DF 等使用 | 5.1 |
| 主动规整 | Proactive Compaction | kcompactd 依据碎片化分数在后台规整 | 4.12 |
| 驻留集大小 | Resident Set Size，RSS | 进程当前占用的物理页量 | 4.7 |
| 主缺页 | Major Fault | 需要 I/O 的缺页，计入 `maj_flt` | 4.8 |
| 逐实体负载跟踪 | Per-Entity Load Tracking，PELT | 几何衰减的负载 / 利用率平均值 | 3.8 |
| 准入控制 | Admission Control | 设置 Deadline 参数时检查总带宽是否超限 | 3.7 |
| 自动 NUMA 平衡 | Automatic NUMA Balancing | 以 PROTNONE 缺页采样访问，把页迁到访问者所在节点（`CONFIG_NUMA_BALANCING`） | 4.12 |
| 自旋锁 | Spinlock | 获取失败时原地循环等待的锁，`spinlock_t` / `raw_spinlock_t` | 7.4 |
| 自愿抢占 | Voluntary Preemption | voluntary 抢占模型：内核态只在 `cond_resched()`、`might_sleep()` 等显式抢占点让出 CPU | 3.5 |
| 自愿上下文切换 | Voluntary Context Switch | 任务主动睡眠引起的切换，计入 `nvcsw` | 3.2 |
| 组调度 | Group Scheduling | 以任务组为单位分配 CPU（`CONFIG_FAIR_GROUP_SCHED`） | 3.10 |
| 组合树 | Combining Tree | Tree RCU 中汇总静止状态报告的 `rcu_node` 层级结构 | 7.10 |
| 最近最少使用 | Least Recently Used，LRU | 以最近访问时间近似页面冷热的回收策略 | 4.10 |
| 最早合格虚拟截止时间优先 | Earliest Eligible Virtual Deadline First，EEVDF | v6.6 起的公平调度算法 | 3.3 |
| 最早截止时间优先 | Earliest Deadline First，EDF | Deadline 类的选取规则 | 3.7 |

## 不翻译的术语

`EEVDF`、`CFS`、`PELT`、`RCU`、`per-CPU`、`softirq`、`tasklet`、`slab`、`page cache`、`vruntime`、`lag`、`next buddy`、`nice`、`idle`、`dl_server`、`sched_ext`、`uclamp`、`PSI`、`folio`、`memblock`、`vmemmap`、`vmalloc`、`SLUB`、`kswapd`、`kcompactd`、`khugepaged`、`shrinker`、`zswap`、`zram`、`anon_vma`、`lruvec`、`hugetlbfs`、`GFP`、`qspinlock`、`qrwlock`、`seqcount`、`mutex`、`rwsem`、`rt_mutex`、`OSQ`、`futex`、`SRCU`、`lockdep`、`irq_work`、`wake_q`、`kworker`、`ksoftirqd`、`NAPI`、`x2APIC`、`hwirq`、`tick`、`hrtick`、`static call`、`static key`、`housekeeping`
