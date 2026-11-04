# 术语表

按中文拼音排序。"首次出现"指全书第一次使用该术语的小节。

| 中文 | 英文 | 说明 | 首次出现 |
|---|---|---|---|
| CPU 带宽控制 | CFS Bandwidth Control | 以 quota / period 为任务组设置 CPU 时间上限 | 3.10 |
| per-CPU 页缓存 | Per-CPU Pageset，PCP | 每 CPU、每 zone 的小阶空闲页缓存，`struct per_cpu_pages` | 4.3 |
| 按需分配 | Demand Paging | mmap 只建立 VMA，物理页在首次访问的缺页中分配 | 4.7 |
| 保护页 | Guard Page | vmalloc 区域末尾不映射的一页，用于捕获越界访问 | 4.6 |
| 布隆过滤器 | Bloom Filter | MGLRU 老化时记录值得扫描的 PMD，跳过冷区域 | 4.10 |
| 层 | Tier | MGLRU 代内按文件描述符访问次数的细分 | 4.10 |
| 程序序 | Program Order | 任务迁移或睡眠唤醒前后，其操作保持程序顺序的保证 | 3.4 |
| 次缺页 | Minor Fault | 无需 I/O 即可完成的缺页，计入 `min_flt` | 4.8 |
| 大页 | Huge Page | 以 PMD（2 MB）或 PUD（1 GB）直接映射的页 | 4.13 |
| 代 | Generation | MGLRU 中按年龄划分的页集合，以 `max_seq`/`min_seq` 编号 | 4.10 |
| 代理执行 | Proxy Execution | 让持锁任务借用等待者的调度上下文运行（`CONFIG_SCHED_PROXY_EXEC`） | 3.1 |
| 调度策略 | Scheduling Policy | `SCHED_NORMAL`、`SCHED_FIFO` 等，用户态可见 | 3.1 |
| 调度队列 | Dispatch Queue，DSQ | sched_ext 中承载待运行任务的队列 | 3.11 |
| 调度类 | Scheduling Class | `struct sched_class`，实现某类调度策略的方法集合 | 3.1 |
| 调度器 | Scheduler | — | 3.1 |
| 调度实体 | Scheduling Entity | 调度类操作的对象，如 `sched_entity`，可代表任务或任务组 | 3.1 |
| 调度域 | Scheduling Domain | 按硬件拓扑组织的 CPU 层级，`struct sched_domain` | 3.9 |
| 调度组 | Scheduling Group | 调度域内参与负载比较的 CPU 子集，`struct sched_group` | 3.9 |
| 丢失唤醒 | Lost Wakeup | 等待方检查条件与睡眠之间错过唤醒的竞态 | 3.4 |
| 抖动 | Thrashing | 工作集超过内存，频繁回收与重新读入导致系统几乎停滞 | 4.10 |
| 多尺寸 THP | Multi-size THP，mTHP | 介于 4 KB 与 2 MB 之间的匿名大 folio（v6.8） | 4.8 |
| 多代 LRU | Multi-Gen LRU，MGLRU | v6.1 起的多代页面回收算法（`CONFIG_LRU_GEN`） | 4.10 |
| 惰性抢占 | Lazy Preemption | 对公平任务延迟到返回用户态或下一 tick 才执行的抢占（`CONFIG_PREEMPT_LAZY`） | 3.2 |
| 惰性 TLB | Lazy TLB | 内核线程借用上一任务地址空间而不切换页表 | 3.2 |
| 反向映射 | Reverse Mapping，rmap | 由物理页找到所有映射它的页表项 | 4.9 |
| 非自愿上下文切换 | Involuntary Context Switch | 因抢占发生的切换，计入 `nivcsw` | 3.2 |
| 复合页 | Compound Page | 多个连续页组成的整体，由头页与尾页构成 | 4.1 |
| 负载均衡 | Load Balancing | 在 CPU 之间迁移任务以纠正负载不均 | 3.9 |
| 规范地址 | Canonical Address | 高位为最高有效位符号扩展的 x86_64 虚拟地址 | 4.6 |
| 合格 | Eligible | EEVDF 中 lag ≥ 0（`vruntime ≤ V`）的实体才可被选中 | 3.3 |
| 核心调度 | Core Scheduling | 控制 SMT 兄弟线程上可同时运行的任务（`CONFIG_SCHED_CORE`） | 3.1 |
| 恒定带宽服务器 | Constant Bandwidth Server，CBS | 限制每个 Deadline 实体带宽的预算机制 | 3.7 |
| 后备存储 | Backing Store | 页内容的持久化位置：文件页为文件，匿名页为交换空间 | 4.11 |
| 唤醒队列 | Wake List | `TTWU_QUEUE` 机制下挂在目标 CPU 上、经 IPI 处理的待唤醒任务链表 | 3.4 |
| 回收 | Reclaim | 释放已用页以补充空闲内存，执行者为 kswapd 与直接回收 | 4.4 |
| 伙伴系统 | Buddy System | 以 2 的幂次块管理空闲物理页的分配器 | 4.1 |
| 计费 | Charge | 把内存用量计入某个 memcg | 4.14 |
| 基于对象的反向映射 | Object-based Reverse Mapping | 经 `folio->mapping` 与 `index` 计算虚拟地址的 rmap 方案 | 4.9 |
| 交换表 | Swap Table | v6.18 起每 cluster 一页的交换缓存数组 | 4.11 |
| 交换槽位 | Swap Slot | 交换设备上存放一页的位置 | 4.11 |
| 交换缓存 | Swap Cache | 交换槽位到 folio 的映射，同步换入换出 | 4.11 |
| 交换空间 | Swap Space | 存放被换出匿名页的块设备或文件 | 4.11 |
| 交换项 | Swap Entry | 写入不存在 PTE 中的（设备号，偏移）编码 | 4.11 |
| 节点 | Node | 一组访问延迟相同的内存，`pg_data_t` | 4.1 |
| 节流 | Throttle | 实体或任务组用尽预算后暂停运行 | 3.7 |
| 静止状态 | Quiescent State | RCU 术语，CPU 不处于读临界区的时刻 | 3.2 |
| 老化 | Aging | MGLRU 中新建一代并提升近期访问页的过程 | 4.10 |
| 零松弛 | Zero-Laxity | 延迟 dl_server 的激活时刻 `deadline − runtime`，即再推迟就无法在截止时间前用完预算 | 3.7 |
| 零页 | Zero Page | 全局共享的全零只读页，匿名读缺页映射到它 | 4.8 |
| 末级缓存 | Last Level Cache，LLC | 一组 CPU 共享的最后一级缓存，调度器以此划分 `sd_llc` 域 | 3.4 |
| 内存保护键 | Protection Keys，PKEY | PTE 中 4 位键与 PKRU 寄存器配合的访问控制 | 4.8 |
| 内存不足杀手 | Out-Of-Memory Killer，OOM Killer | 内存耗尽时选择并杀死进程 | 4.12 |
| 内存规整 | Memory Compaction | 迁移可移动页以合并出连续空闲块 | 4.12 |
| 内存控制组 | Memory Cgroup，memcg | 按 cgroup 计费、限制与保护内存 | 4.14 |
| 内存域 | Zone | 节点内按地址范围划分的区域，如 `ZONE_DMA32` | 4.1 |
| 能效感知调度 | Energy Aware Scheduling，EAS | 在非对称算力平台上按能耗选择 CPU | 3.4 |
| 迁移类型 | Migrate Type | 按页块记录的可移动性分类（不可移动、可移动、可回收等） | 4.1 |
| 迁移项 | Migration Entry | 页迁移期间替代 PTE 的特殊不存在项 | 4.11 |
| 抢占 | Preemption | 在任务未主动让出时强制切换到其他任务 | 3.1 |
| 抢占模型 | Preemption Model | none / voluntary / full / lazy，决定内核态何时可被抢占 | 3.5 |
| 区间树 | Interval Tree | 按区间重叠查询的增强红黑树，用于 `i_mmap` 与 anon_vma | 4.9 |
| 驱逐 | Eviction | MGLRU 中从最老一代回收页的过程 | 4.10 |
| 权重 | Load Weight | 公平调度类按其比例分配 CPU 时间，由 nice 值决定 | 3.1 |
| 缺页异常 | Page Fault，#PF | 访问未映射或权限不符的地址时 CPU 触发的异常 | 4.8 |
| 上下文切换 | Context Switch | — | 3.1 |
| 时间片 | Slice | EEVDF 中任务每次请求的运行长度，`se->slice` | 3.3 |
| 时间片保护 | Run to Parity | `RUN_TO_PARITY`，选中后在保护期内不被轻易切走 | 3.3 |
| 首次适配 | First Fit | 在空闲区间中选择第一个足够大的区间 | 4.6 |
| 水位线 | Watermark | zone 的 min/low/high 空闲页阈值 | 4.4 |
| 碎片化指数 | Fragmentation Index | 判断分配失败源于内存不足还是碎片化的指标 | 4.12 |
| 提交记账 | Commit Accounting | 统计私有可写映射承诺的内存量，`Committed_AS` | 4.7 |
| 透明大页 | Transparent Huge Page，THP | 缺页时自动尝试分配大页，失败回退 | 4.13 |
| 头页 / 尾页 | Head Page / Tail Page | 复合页的第一页与其余页 | 4.1 |
| 推 / 拉 | Push / Pull | 实时与 Deadline 类在 CPU 间主动迁移任务的两种操作 | 3.6 |
| 外部碎片 | External Fragmentation | 空闲页总量足够但缺少连续块 | 4.12 |
| 完全公平调度器 | Completely Fair Scheduler，CFS | v2.6.23～v6.5 的公平调度算法 | 3.3 |
| 未接受内存 | Unaccepted Memory | 机密虚拟机中须显式接受后才能使用的内存 | 4.2 |
| 稀疏内存模型 | SPARSEMEM | 按 section 分配 `struct page` 的物理内存模型 | 4.1 |
| 写时复制 | Copy-On-Write，COW | 共享只读页，写入时再复制 | 4.9 |
| 修正唤醒规则 | Revised Wakeup Rule | 受约束 Deadline 任务唤醒时削减预算而非重开周期 | 3.7 |
| 虚拟截止时间 | Virtual Deadline | `vruntime + slice / weight` | 3.3 |
| 虚拟内存区域 | Virtual Memory Area，VMA | 进程地址空间中属性相同的一段区间，`vm_area_struct` | 4.7 |
| 虚拟时间 | Virtual Time，V | 所有实体 vruntime 的加权平均 | 3.3 |
| 虚拟运行时间 | Virtual Runtime，vruntime | 按权重折算后的运行时间 | 3.3 |
| 延迟出队 | Delayed Dequeue | EEVDF 中负 lag 任务睡眠时暂留队列的机制 | 3.1 |
| 页块 | Pageblock | 记录迁移类型的页组，x86_64 上为 2 MB | 4.1 |
| 页迁移 | Page Migration | 把页内容与映射转移到新物理页 | 4.12 |
| 页帧 | Page Frame | 物理内存中的一页，用 `struct page` 描述 | 4.1 |
| 页帧号 | Page Frame Number，PFN | 物理地址右移 `PAGE_SHIFT` 得到的页编号 | 4.1 |
| 有界拖延 | Bounded Tardiness | 多核全局 EDF 下任务完成时间超过截止时间的量有上界 | 3.7 |
| 优先级反转 | Priority Inversion | 高优先级任务因等待低优先级任务持有的资源而被中优先级任务间接阻塞 | 3.6 |
| 优先级继承 | Priority Inheritance，PI | 持锁的低优先级任务临时继承等待者的优先级 | 3.1 |
| 运行队列 | Runqueue，rq | 每 CPU 一个的 `struct rq` | 3.1 |
| 增强红黑树 | Augmented RB-tree | 节点额外维护子树聚合信息的红黑树 | 3.3 |
| 直接回收 | Direct Reclaim | 分配者在慢速路径中自行回收内存 | 4.4 |
| 直接映射区 | Direct Map | 线性映射全部物理内存的内核虚拟地址区域，起始于 `PAGE_OFFSET` | 4.1 |
| 指数加权移动平均 | Exponentially Weighted Moving Average，EWMA | util_est 用于平滑利用率下降 | 3.8 |
| 主动规整 | Proactive Compaction | kcompactd 依据碎片化分数在后台规整 | 4.12 |
| 驻留集大小 | Resident Set Size，RSS | 进程当前占用的物理页量 | 4.7 |
| 主缺页 | Major Fault | 需要 I/O 的缺页，计入 `maj_flt` | 4.8 |
| 逐实体负载跟踪 | Per-Entity Load Tracking，PELT | 几何衰减的负载 / 利用率平均值 | 3.8 |
| 准入控制 | Admission Control | 设置 Deadline 参数时检查总带宽是否超限 | 3.7 |
| 自动 NUMA 平衡 | Automatic NUMA Balancing | 以 PROTNONE 缺页采样访问，把页迁到访问者所在节点（`CONFIG_NUMA_BALANCING`） | 4.12 |
| 自愿上下文切换 | Voluntary Context Switch | 任务主动睡眠引起的切换，计入 `nvcsw` | 3.2 |
| 组调度 | Group Scheduling | 以任务组为单位分配 CPU（`CONFIG_FAIR_GROUP_SCHED`） | 3.10 |
| 最近最少使用 | Least Recently Used，LRU | 以最近访问时间近似页面冷热的回收策略 | 4.10 |
| 最早合格虚拟截止时间优先 | Earliest Eligible Virtual Deadline First，EEVDF | v6.6 起的公平调度算法 | 3.3 |
| 最早截止时间优先 | Earliest Deadline First，EDF | Deadline 类的选取规则 | 3.7 |

## 不翻译的术语

`EEVDF`、`CFS`、`PELT`、`RCU`、`per-CPU`、`softirq`、`tasklet`、`slab`、`page cache`、`vruntime`、`lag`、`nice`、`idle`、`dl_server`、`sched_ext`、`uclamp`、`PSI`、`folio`、`memblock`、`vmemmap`、`vmalloc`、`SLUB`、`kswapd`、`kcompactd`、`khugepaged`、`shrinker`、`zswap`、`zram`、`anon_vma`、`lruvec`、`hugetlbfs`、`GFP`
