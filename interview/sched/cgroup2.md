# cgroup v2 怎样分配和隔离 CPU

> **源码与配置**：本项目 [Linux 6.18.52](../../linux/Makefile#L2)，x86-64、SMP，以 [`linux/.config`](../../linux/.config#L216) 为准。与本文结论相关的配置如下：
>
> | 配置 | 取值 | 对本文的影响 |
> | ---- | ---- | ------------ |
> | `CGROUP_SCHED`、`FAIR_GROUP_SCHED`、`GROUP_SCHED_WEIGHT` | y，见 [.config](../../linux/.config#L216) | 有 cpu 控制器、分层公平调度和 `cpu.weight` |
> | `CFS_BANDWIDTH`、`GROUP_SCHED_BANDWIDTH` | y，见 [.config](../../linux/.config#L218) | 有 `cpu.max` / `cpu.max.burst` 及带宽限流 |
> | `RT_GROUP_SCHED` | 未开启，见 [.config](../../linux/.config#L221) | RT 任务不按 cgroup 分组限额 |
> | `CPUSETS` / `CPUSETS_V1` | y / 未开启，见 [.config](../../linux/.config#L228) | 只讨论 v2 的 cpuset |
> | `SCHED_AUTOGROUP` | y，见 [.config](../../linux/.config#L245) | 运行时默认启用，影响 cpu 状态仍在根组的任务，见第 3.5 节 |
> | `NO_HZ_FULL`、`CPU_ISOLATION` | y，见 [.config](../../linux/.config#L108) | 需启动参数 `nohz_full=` 才对具体 CPU 生效，见第 5.9 节 |
> | `JUMP_LABEL` | y，见 [.config](../../linux/.config#L847) | 带宽热路径由 static key 开关 |
> | `PREEMPT_VOLUNTARY`、非 `PREEMPT_RT` | 见 [.config](../../linux/.config#L136) | 普通数据中心抢占模型 |
> | `SCHED_CORE`、`SCHED_PROXY_EXEC`、`IRQ_TIME_ACCOUNTING` | 未开启，见 [.config](../../linux/.config#L143) | 选人走非 core scheduling 路径；任务时钟不扣中断时间 |
>
> `SCHED_CLASS_EXT` 不在 `.config` 中，它依赖的 `DEBUG_INFO_BTF` 也未开启，因此不讨论 sched_ext。EEVDF 的选人公式见同目录 [eevdf.md](eevdf.md)，运行队列与负载均衡全景见 [sched.md](sched.md)。

## 1. 先建立整体认识

### 1.1 “给容器分 CPU”其实是三件事

| 需求 | 要回答的问题 | 主要接口 | 例子 |
| ---- | ------------ | -------- | ---- |
| 争用时多分一点时间 | 本组相对其他组占多少份额？ | `cpu.weight` | A、B 权重为 100、300，持续争用一颗 CPU 时约分到 1/4、3/4 |
| 限制全组执行预算 | 每个周期最多给全组多少执行时间？ | `cpu.max` | `50000 100000`：每 100 ms 新增 50 ms 全组额度 |
| 规定任务去哪颗 CPU | 本组能用哪些 CPU？其他组还能不能来？ | `cpuset.cpus`、`cpuset.cpus.partition` | 把 CPU 4-7 划成有效分区，从父组可用集合中移除 |

前两项管**时间**，由 cpu 控制器实现，接口注册在 [`cpu_files[]`](../../linux/kernel/sched/core.c#L10252)；第三项管**位置**，由 cpuset 控制器实现，接口注册在 [`dfl_files[]`](../../linux/kernel/cgroup/cpuset.c#L3559)。三者同时生效：A 允许使用 CPU 4-7 并设置 `cpu.max = 50000 100000`，它可以在四颗 CPU 上并行运行，但四颗 CPU 共享同一份 50 ms 预算；只写 `cpuset.cpus = 4-7` 也不会挡住其他组，独占需要建立**有效分区**。

### 1.2 在内核中的位置

用户配置的是组，真正执行任务的是每颗 CPU 的调度器。下图从上到下表示“配置怎样一路落到每 CPU 调度”，箭头表示数据或约束的流向：

```mermaid
flowchart TD
    FILE["用户接口：目录、cgroup.procs、控制器文件"] --> CORE["cgroup 核心<br/>组树、任务归属、控制器状态"]
    CORE --> CPU["cpu 控制器：task_group<br/>权重、全组预算"]
    CORE --> SET["cpuset 控制器：cpuset<br/>有效 CPU 集合、分区"]
    CPU --> FAIR["每 CPU 的公平队列<br/>按组逐层竞争、逐层记账"]
    SET --> AFF["任务亲和性 cpus_mask<br/>唤醒与迁移的候选 CPU"]
    SET --> DOM["调度域 sched_domain<br/>负载均衡的边界"]
    AFF --> FAIR
    DOM --> FAIR
    FAIR --> RUN["选中一个 task_struct 执行"]
```

这张图对应两种工作节奏：

- **配置路径**：`mkdir`、写控制器文件、写 `cgroup.procs` 进入 cgroup 核心，再通过 [`cpu_cgrp_subsys`](../../linux/kernel/sched/core.c#L10304) 和 [`cpuset_cgrp_subsys`](../../linux/kernel/cgroup/cpuset.c#L3884) 的回调建立或更新对象。
- **运行路径**：唤醒、选人、tick 记账、负载均衡只使用已经建立的对象，不解析 cgroup 目录。

一次调度可以拆成三个问题：任务被允许去哪颗 CPU（cpuset）？在那颗 CPU 上它所属的组怎样与其他组竞争（`cpu.weight`）？执行之后哪些组的预算要扣除（`cpu.max`）？后文的数据结构都为回答这三个问题服务。

### 1.3 本文的边界

- 权重和配额只作用于**公平调度类**，即 `SCHED_NORMAL/BATCH/IDLE` 任务，公平类定义见 [`fair_sched_class`](../../linux/kernel/sched/fair.c#L14095)；`SCHED_FIFO/RR`、`SCHED_DEADLINE` 不进入这里的公平队列和带宽记账，调度类选择见 [`__setscheduler_class()`](../../linux/kernel/sched/core.c#L7304)。cpuset 的放置约束对所有调度类都有效。
- CPU 编号指**逻辑 CPU**。要独占物理核，应把 SMT 兄弟一起分配，见 [`topology_sibling_cpumask()`](../../linux/arch/x86/include/asm/topology.h#L196) 和 sysfs 的 [`thread_siblings_list`](../../linux/drivers/base/topology.c#L80)。分区不隔离共享缓存和内存带宽。
- 读者只需知道 EEVDF 在每层队列中挑选一个实体；具体公式不在本文展开。

阅读顺序：第 2 章建立两个控制器共用的 cgroup 骨架；第 3～5 章讲 cpu 控制器（数据结构 → 权重 → 配额）；第 6 章讲 cpuset；第 7 章用一个示例把三种机制叠在一起；第 8 章给出验证方法。第一次阅读可以跳过第 4.4～4.5、5.7～5.9 节，以及 6.4 节的远端分区和 6.8 节。

## 2. 公共骨架：组、控制器状态与任务归属

本章回答一个问题：**一个任务怎样找到它应当使用的 cpu 配置和 cpuset 配置？** 难点在于 cgroup v2 中“任务属于哪个目录”和“任务使用哪一组控制器状态”可能指向不同的组。

### 2.1 `cgroup` 与 css：目录和控制器状态分开存放

用户每建一个目录，内核就有一个 [`struct cgroup`](../../linux/include/linux/cgroup-defs.h#L472)。每个控制器在组上的状态由 css（cgroup subsystem state，[`struct cgroup_subsys_state`](../../linux/include/linux/cgroup-defs.h#L179)）表示。下面只保留本文用到的成员，中文注释为阅读说明：

```c
struct cgroup_subsys_state {
    struct cgroup *cgroup;           /* 这个状态属于哪个组 */
    struct cgroup_subsys *ss;        /* 属于哪个控制器；cgroup.self 中为 NULL */
    struct percpu_ref refcnt;        /* 状态对象的引用计数 */
    /* 省略 */
    struct list_head sibling;        /* 挂到父 css 的 children 链表 */
    struct list_head children;
    /* 省略 */
    struct cgroup_subsys_state *parent; /* 同一控制器的父状态 */
    /* 省略 */
};

struct cgroup {
    struct cgroup_subsys_state self; /* 内嵌：组自身，连接目录树 */
    /* 省略 */
    struct kernfs_node *kn;          /* 对应的 kernfs 目录节点 */
    /* 省略 */
    u16 subtree_control;             /* 用户要求向子组启用的控制器 */
    u16 subtree_ss_mask;             /* 实际向子组启用的控制器，可含依赖项 */
    /* 省略 */
    struct cgroup_subsys_state __rcu *subsys[CGROUP_SUBSYS_COUNT];
                                     /* 本组各控制器自己的 css，可为 NULL */
};
```

字段位置见 [`css.parent`](../../linux/include/linux/cgroup-defs.h#L244)、[`cgroup.kn`](../../linux/include/linux/cgroup-defs.h#L524)、[`subtree_control` 的说明](../../linux/include/linux/cgroup-defs.h#L531) 和 [`cgroup.subsys[]`](../../linux/include/linux/cgroup-defs.h#L544)。

css 是**公共成员**：每种控制器把它内嵌在自己的对象中，cgroup 核心只管理 css，控制器再用 `container_of()` 找回外层对象。cpu 控制器的外层对象是 [`struct task_group`](../../linux/kernel/sched/sched.h#L472)，转换函数是 [`css_tg()`](../../linux/kernel/sched/sched.h#L558)；cpuset 的外层对象是 `struct cpuset`，转换函数是 [`css_cs()`](../../linux/kernel/cgroup/cpuset-internal.h#L185)。

```mermaid
flowchart LR
    subgraph CG["struct cgroup A"]
        SELF["self：组自身的 css，ss = NULL"]
        SLOT["subsys[cpu_cgrp_id]：指针"]
    end
    subgraph TG["struct task_group A"]
        CSS["css：cpu 控制器的 css"]
        POLICY["权重、预算、每 CPU 对象"]
    end
    SLOT -->|"指向内嵌成员"| CSS
    CSS -->|"cgroup 指针"| CG
```

图中有两个 css，容易混淆：`cgroup A.self` 表示 A 这个组，`parent` 指向父组的 `self`，见 [`cgroup` 定义处注释](../../linux/include/linux/cgroup-defs.h#L473) 和 [`cgroup_parent()`](../../linux/include/linux/cgroup.h#L518)；`task_group A.css` 表示 A 的 cpu 状态，`parent` 指向父组的 cpu 状态。

### 2.2 `css_set`：任务一次找到各控制器的有效状态

任务不直接保存各控制器指针，而是引用一个 [`struct css_set`](../../linux/include/linux/cgroup-defs.h#L272)。归属与控制器状态组合相同的任务共享同一个集合，减少每任务存储和 fork / exit 时的维护，见 [定义前的说明](../../linux/include/linux/cgroup-defs.h#L265)。

```c
struct task_struct {
    /* 省略 */
    struct css_set __rcu *cgroups;   /* 本任务引用的状态集合 */
    struct list_head cg_list;        /* 挂入 css_set 的任务链表 */
    /* 省略 */
};

struct css_set {
    struct cgroup_subsys_state *subsys[CGROUP_SUBSYS_COUNT];
                                     /* 各控制器实际生效的 css */
    refcount_t refcount;             /* 集合自身的引用计数 */
    /* 省略 */
    struct cgroup *dfl_cgrp;         /* 任务在 v2 层级中属于哪个目录 */
    int nr_tasks;
    struct list_head tasks;
    /* 省略 */
};
```

字段见 [`task_struct.cgroups`](../../linux/include/linux/sched.h#L1323) 和 [`dfl_cgrp / nr_tasks`](../../linux/include/linux/cgroup-defs.h#L291)。集合中的两类出口分别回答两个问题：`dfl_cgrp` 回答“任务在哪个目录”，`subsys[]` 回答“任务使用哪个控制器状态”。读取有效 css 的接口是 [`task_css()`](../../linux/include/linux/cgroup.h#L456)，它展开为 `task_css_set_check(task)->subsys[id]`，RCU 与锁检查封装在 [`task_css_set_check()`](../../linux/include/linux/cgroup.h#L415) 中。

### 2.3 父组启用控制器，子组才有自己的状态

控制器开关作用于**直接子组**：普通非根组能否拥有某个控制器状态，取决于父组的 `subtree_control`，见 [`cgroup_control()`](../../linux/kernel/cgroup/cgroup.c#L479)。因此会出现“目录比状态更深一层”的情况。假设任务放在 B 中：

```text
根组：subtree_control = cpu
└── A：拥有 task_group A；subtree_control 为空
    └── B：没有自己的 cpu 状态

B 中任务的 css_set：
  dfl_cgrp             → cgroup B           （目录归属）
  subsys[cpu_cgrp_id]  → task_group A.css   （有效 cpu 状态）
cgroup B.subsys[cpu_cgrp_id] → NULL
```

| 配置 | B 的 `subsys[cpu_cgrp_id]` | B 中任务的有效 cpu 状态 |
| ---- | -------------------------- | ----------------------- |
| 根组启用 cpu，A 未向子组启用 | `NULL` | `task_group A.css` |
| 根组、A 都向子组启用 cpu | `task_group B.css` | `task_group B.css` |

**`cgroup.subsys[]` 查本组自己的状态，`css_set.subsys[]` 查任务实际使用的状态**；后者允许指向祖先，见 [`css_set` 的注释](../../linux/include/linux/cgroup-defs.h#L312)。构造新集合时，核心用 [`cgroup_e_css_by_mask()`](../../linux/kernel/cgroup/cgroup.c#L547) 自下而上找最近一个拥有该控制器的组：

```c
static struct cgroup_subsys_state *cgroup_e_css_by_mask(struct cgroup *cgrp,
                                                      struct cgroup_subsys *ss)
{
    lockdep_assert_held(&cgroup_mutex);

    if (!ss)
        return &cgrp->self;

    /* 状态可能正在更新，按控制器掩码判断，不直接测试 css 指针 */
    while (!(cgroup_ss_mask(cgrp) & (1 << ss->id))) {
        cgrp = cgroup_parent(cgrp);
        if (!cgrp)
            return NULL;
    }

    return cgroup_css(cgrp, ss);
}
```

结果写入集合模板的位置见 [`template[i]`](../../linux/kernel/cgroup/cgroup.c#L1125)。一个推论：**有独立目录不等于有独立的 cpu 调度对象**；若一路向上都没有启用 cpu，任务的有效 cpu 状态就是根组，这时还会受 autogroup 影响（第 3.5 节）。

### 2.4 控制器回调与任务迁移

cgroup 核心在生命周期事件发生时回调控制器，控制器内部从不解析目录。本文涉及的回调如下：

| 阶段 | cpu 控制器 | cpuset 控制器 |
| ---- | ---------- | ------------- |
| 建组 | `css_alloc`：[`cpu_cgroup_css_alloc()`](../../linux/kernel/sched/core.c#L9257)，根组返回 `root_task_group`，否则 `sched_create_group()`；`css_online`：[`cpu_cgroup_css_online()`](../../linux/kernel/sched/core.c#L9275)，挂入调度器的组树 | `css_alloc`：[`cpuset_css_alloc()`](../../linux/kernel/cgroup/cpuset.c#L3643) 分配 `struct cpuset` 和各掩码；`css_online`：[`cpuset_css_online()`](../../linux/kernel/cgroup/cpuset.c#L3666) 从父组复制有效集合 |
| 删组 | `css_released`：[`cpu_cgroup_css_released()`](../../linux/kernel/sched/core.c#L9305)；`css_free`：[`cpu_cgroup_css_free()`](../../linux/kernel/sched/core.c#L9312) | `css_killed`：[`cpuset_css_killed()`](../../linux/kernel/cgroup/cpuset.c#L3756) 把有效分区复位为 member；`css_free` 释放掩码 |
| `can_attach` | [`cpu_cgroup_can_attach()`](../../linux/kernel/sched/core.c#L9322)：RT 组调度检查编译在 `CONFIG_RT_GROUP_SCHED` 内，本配置未开启 | [`cpuset_can_attach_check()`](../../linux/kernel/cgroup/cpuset.c#L3178)：目标有效集合不能为空 |
| `attach` | [`cpu_cgroup_attach()`](../../linux/kernel/sched/core.c#L9340)：逐任务 `sched_move_task()` | [`cpuset_attach()`](../../linux/kernel/cgroup/cpuset.c#L3316)：按需更新任务亲和性 |

两个控制器描述表都设置了 `.threaded = true`，可以在线程模式子树中使用。本文示例只用普通 domain 组；启用规则的完整检查见 [`cgroup_vet_subtree_control_enable()`](../../linux/kernel/cgroup/cgroup.c#L3503)。

迁移任务有两个入口：写 `cgroup.procs` 迁移**整个线程组**，见 [`cgroup_procs_write()`](../../linux/kernel/cgroup/cgroup.c#L5437)；写 `cgroup.threads` 只迁移单个线程，见 [`cgroup_threads_write()`](../../linux/kernel/cgroup/cgroup.c#L5448)，且受 [资源域边界检查](../../linux/kernel/cgroup/cgroup.c#L5384) 约束。无论哪个入口，核心都是**先替换任务的 `css_set`，再调用各控制器的 `attach`**；cpu 控制器改变任务的竞争位置，cpuset 改变任务的允许 CPU，二者在同一次迁移中各做各的。

## 3. cpu 控制器的核心数据结构

cpu 控制器要同时满足两点：配置（权重、预算）属于**整个组**，竞争却发生在**每颗 CPU 的运行队列**里。因此它采用“一个全组对象 + 每 CPU 一套调度对象”的结构。

下文用以下名称指代对象：**任务组对象**指 `struct task_group`；**组实体**指代表整个组参加父层竞争的 `struct sched_entity`；**任务实体**指线程内嵌的 `struct sched_entity`；**组内队列**指组在某颗 CPU 上的 `struct cfs_rq`。源码仍沿用 `cfs_rq`、`CFS_BANDWIDTH` 等历史名称，本版本公平类的选人算法是 EEVDF。

### 3.1 `task_group`：一个组、一份配置、每 CPU 一套对象

摘自 [`struct task_group`](../../linux/kernel/sched/sched.h#L472)：

```c
struct task_group {
    struct cgroup_subsys_state css;  /* 内嵌 cpu 控制器的 css */
    int idle;                        /* cpu.idle */
    struct sched_entity **se;        /* se[cpu]：本组在该 CPU 父队列中的组实体 */
    struct cfs_rq **cfs_rq;          /* cfs_rq[cpu]：本组在该 CPU 上的组内队列 */
    unsigned long shares;            /* cpu.weight 换算后的组权重，内部尺度 */
    atomic_long_t load_avg ____cacheline_aligned; /* 各 CPU 上报负荷的汇总 */
    /* 省略 */
    struct task_group *parent;
    struct list_head siblings;
    struct list_head children;       /* 调度器自己维护的组树 */
    struct autogroup *autogroup;     /* 非 NULL 表示这是 autogroup 创建的组 */
    struct cfs_bandwidth cfs_bandwidth; /* 内嵌：全组唯一的预算池，第 5 章 */
    /* 省略 */
};
```

成员位置见 [`se / cfs_rq / shares / load_avg`](../../linux/kernel/sched/sched.h#L480)、[`parent / children`](../../linux/kernel/sched/sched.h#L506) 和 [`cfs_bandwidth`](../../linux/kernel/sched/sched.h#L514)。`se` 与 `cfs_rq` 是**指针数组**，按 CPU 编号索引，每个元素指向独立分配的对象，见 [`alloc_fair_sched_group()`](../../linux/kernel/sched/fair.c#L13833)。

### 3.2 `sched_entity` 与 `cfs_rq`：每 CPU 上的分层竞争

**`sched_entity`：本层的一个竞争者。** 它可以代表一个任务，也可以代表一个组。摘自 [`struct sched_entity`](../../linux/include/linux/sched.h#L570)：

```c
struct sched_entity {
    struct load_weight load;         /* weight：本实体在所在队列中的权重 */
    struct rb_node run_node;         /* 本层 EEVDF 红黑树节点 */
    u64 deadline;                    /* 本层虚拟截止期 */
    /* 省略 */
    unsigned char on_rq;             /* 是否计入所在队列（不等于在树上） */
    unsigned char sched_delayed;     /* 是否处于延迟出队状态 */
    /* 省略 */
    u64 sum_exec_runtime;            /* 累计实际执行时间 */
    /* 省略 */
    u64 vruntime;                    /* 按权重推进的虚拟运行时间 */
    /* 省略 */
    int depth;
    struct sched_entity *parent;     /* 同一 CPU 上的上一层组实体 */
    struct cfs_rq *cfs_rq;           /* 我在哪个队列里竞争 */
    struct cfs_rq *my_q;             /* 我代表哪个组内队列；任务实体为 NULL */
    /* 省略 */
    struct sched_avg avg;            /* PELT 负荷历史 */
};
```

PELT（Per-Entity Load Tracking）是按时间衰减的负荷历史：`avg.load_avg` 反映带权重的可运行需求，`util_avg` 反映实际运行比例，见 [定义说明](../../linux/include/linux/sched.h#L464)。层级指针见 [`depth / parent / cfs_rq / my_q`](../../linux/include/linux/sched.h#L598)，权重结构见 [`struct load_weight`](../../linux/include/linux/sched.h#L455)。任务与组的区分就是 `my_q` 是否为空，见 [`entity_is_task()`](../../linux/kernel/sched/sched.h#L923)。

**`cfs_rq`：一颗 CPU 上、一个组的一层公平队列。** 摘自 [`struct cfs_rq`](../../linux/kernel/sched/sched.h#L676)，带宽相关成员留到第 5.2 节：

```c
struct cfs_rq {
    struct load_weight load;         /* 本层实体权重之和（即时值） */
    unsigned int nr_queued;          /* 本层实体数，含组实体和 curr */
    unsigned int h_nr_queued;        /* 子树中计入队列的任务数，含 delayed */
    unsigned int h_nr_runnable;      /* 子树中可运行任务数，不含 delayed */
    unsigned int h_nr_idle;
    /* 省略 */
    struct rb_root_cached tasks_timeline; /* 本层 EEVDF 红黑树 */
    struct sched_entity *curr;       /* 本层当前实体，可能是组实体 */
    /* 省略 */
    struct sched_avg avg;
    /* 省略 */
    unsigned long tg_load_avg_contrib; /* 上次向 tg->load_avg 上报的贡献 */
    /* 省略 */
    struct rq *rq;                   /* 所在 CPU 的总运行队列 */
    /* 省略 */
    struct task_group *tg;           /* 拥有本队列的组 */
    int idle;                        /* tg->idle 的本地副本 */
    /* 省略带宽成员 */
};
```

字段位置见 [`tg_load_avg_contrib`](../../linux/kernel/sched/sched.h#L718) 和 [`rq / tg`](../../linux/kernel/sched/sched.h#L734)。每颗 CPU 的 [`struct rq`](../../linux/kernel/sched/sched.h#L1120) 内嵌[根公平队列 `cfs`](../../linux/kernel/sched/sched.h#L1148)，并通过 [`sd`](../../linux/kernel/sched/sched.h#L1209) 指向负载均衡域；根组没有组实体，直接使用 `rq->cfs`，见 [根组初始化](../../linux/kernel/sched/core.c#L8764)。

“在队列上”比“在红黑树里”范围更大：`se.on_rq = 1` 表示实体仍计入这一层，它可能在树中等待，也可能正被 `cfs_rq->curr` 引用。实体被选中运行时由 [`set_next_entity()`](../../linux/kernel/sched/fair.c#L5636) 从树上取下，但 `on_rq` 不变；让出 CPU 后若仍可运行，由 [`put_prev_entity()`](../../linux/kernel/sched/fair.c#L5712) 放回。

### 3.3 `task_struct`：用 `sched_task_group` 接入竞争层级

摘自 [`task_struct`](../../linux/include/linux/sched.h#L866) 及其 [组成员](../../linux/include/linux/sched.h#L881)：

```c
struct task_struct {
    /* 省略 */
    struct sched_entity se;          /* 内嵌任务实体 */
    /* 省略 */
    struct task_group *sched_task_group; /* 调度器使用的组指针副本 */
    struct callback_head sched_throttle_work; /* 第 5.5 节 */
    struct list_head throttle_node;
    bool throttled;
    /* 省略 */
    struct css_set __rcu *cgroups;   /* 第 2.2 节 */
    /* 省略 */
};
```

为什么已有 `cgroups` 还要一个 `sched_task_group`？因为 cgroup 核心**先**改 `p->cgroups`，**后**调用 `attach`。在这两步之间，若调度器按 `task_css()` 找组，组身份就会与任务实体实际所在的队列不一致。源码注释说明了这一点，见 [`task_group()`](../../linux/kernel/sched/sched.h#L2159)：调度器只读副本，副本由 `sched_move_task()` 在任务 rq 锁内更新。这里复制的是**指针**，不是对象。

换组或换 CPU 时，[`set_task_rq()`](../../linux/kernel/sched/sched.h#L2178) 用这个副本把任务实体接到目标组在目标 CPU 上的那一对对象：

```c
p->se.cfs_rq = tg->cfs_rq[cpu];
p->se.parent = tg->se[cpu];
p->se.depth  = tg->se[cpu] ? tg->se[cpu]->depth + 1 : 0;
```

### 3.4 对象关系与不变量

假设业务组是根组的直接子组，下图画出它在 CPU 0 上的连接字段。CPU 1 上另有一套组实体与组内队列，由同一个任务组对象的 `se[1]`、`cfs_rq[1]` 引用：

```mermaid
flowchart TD
    TG["业务组 task_group<br/>shares、cfs_bandwidth"] -->|"se[0]"| ASE["业务组在 CPU 0 的组实体<br/>sched_entity"]
    TG -->|"cfs_rq[0]"| AQ["业务组在 CPU 0 的组内队列<br/>cfs_rq：runtime_remaining"]
    RQ["CPU 0 的 rq"] -->|"内嵌 cfs"| ROOT["CPU 0 根公平队列 cfs_rq"]
    ASE -->|"cfs_rq：在这里竞争"| ROOT
    ASE -->|"my_q：代表的队列"| AQ
    TASK["工作线程 task_struct"] -->|"内嵌 se"| TSE["任务实体 sched_entity"]
    TASK -->|"sched_task_group"| TG
    TSE -->|"cfs_rq"| AQ
    TSE -->|"parent"| ASE
    AQ -->|"tg"| TG
    AQ -->|"rq"| RQ
```

| 不变量 | 依据 |
| ------ | ---- |
| `tg->se[cpu]->my_q == tg->cfs_rq[cpu]`，`tg->se[cpu]->cfs_rq == tg->parent->cfs_rq[cpu]`（父为根时是 `rq->cfs`） | [`init_tg_cfs_entry()`](../../linux/kernel/sched/fair.c#L13926) |
| 任务实体的 `cfs_rq`、`parent` 来自 `p->sched_task_group` 在 `task_cpu(p)` 上的对象 | [`set_task_rq()`](../../linux/kernel/sched/sched.h#L2178) |
| 一个组实体只在其组内队列（经子孙）有可运行实体时才进入父队列 | 入队向上传播，第 4.2 节 |
| 根组没有组实体，`root_task_group.se[cpu] == NULL` | [根组初始化](../../linux/kernel/sched/core.c#L8764) |

这些对象由不同的锁保护，读源码时先判断当前路径持有哪一把：

| 保护对象 | 同步手段 | 依据 |
| -------- | -------- | ---- |
| `p->cgroups`、`css_set` 的任务链表 | `css_set_lock`；读者可用 RCU | [字段注释](../../linux/include/linux/sched.h#L1322) |
| 组的加入与移除（`task_group.children` 等） | `task_group_lock`；遍历组树用 RCU | [`task_group_lock`](../../linux/kernel/sched/core.c#L9083)、[限流遍历](../../linux/kernel/sched/fair.c#L6169) |
| 每 CPU 的 `cfs_rq`、`sched_entity`、`throttle_count`、limbo 链表 | 所在 CPU 的 rq 锁 | 第 4、5 章各函数均在 rq 锁内执行 |
| `p->sched_task_group` | `p->pi_lock` 与 rq 锁（`task_rq_lock`） | [源码注释](../../linux/kernel/sched/sched.h#L2169) |
| 共享预算池 `cfs_bandwidth` 的余额与链表 | `cfs_b->lock`；需要时在 rq 锁内嵌套获取 | [`assign_cfs_rq_runtime()`](../../linux/kernel/sched/fair.c#L5847) |
| 写 `cpu.weight` / `cpu.max` 的配置串行化 | `shares_mutex` / `cfs_constraints_mutex` | [`shares_mutex`](../../linux/kernel/sched/fair.c#L13957)、[`cfs_constraints_mutex`](../../linux/kernel/sched/core.c#L9560) |
| cpuset 的配置与有效掩码 | `cpuset_full_lock()`：CPU 热插拔读锁 + `cpuset_mutex` | [`cpuset_full_lock()`](../../linux/kernel/cgroup/cpuset.c#L276) |

**组对象是全组的，队列、组实体和本地预算余额是每 CPU 的；任务通过组指针接入当前 CPU 的竞争层级。** 第 4、5 章的所有算法都在这张图上读写字段。

### 3.5 生命周期：建组、删除与换组

**建组。** [`sched_create_group()`](../../linux/kernel/sched/core.c#L9125) 分配 `task_group`，再由 [`alloc_fair_sched_group()`](../../linux/kernel/sched/fair.c#L13833) 为每颗 possible CPU（含离线 CPU）分配一对 `cfs_rq` / `sched_entity`，默认 `shares = NICE_0_LOAD`（对应 `cpu.weight = 100`），并用 [`init_cfs_bandwidth()`](../../linux/kernel/sched/fair.c#L6695) 把预算池初始化为无限。`css_online` 阶段的 [`sched_online_group()`](../../linux/kernel/sched/core.c#L9149) 把组链入 `parent->children`，再由 [`online_fair_sched_group()`](../../linux/kernel/sched/fair.c#L13874) 建立负荷关联并同步祖先的限流计数。分配了对象不等于已经排队：组实体只有在该 CPU 上真有可运行子孙时才入队。

**删除。** `css_released` 中的 [`sched_release_group()`](../../linux/kernel/sched/core.c#L9180) 先把组从调度器树上摘下；`css_free` 中的 [`sched_unregister_group()`](../../linux/kernel/sched/core.c#L9113) 取消带宽定时器、解除队列统计关联，再经一次 RCU 宽限期释放每 CPU 对象。之所以延后，是因为带宽定时器和统计读取可能仍在遍历组树。

**换组。** cgroup 核心换好 `css_set` 后调用 [`cpu_cgroup_attach()`](../../linux/kernel/sched/core.c#L9340)，真正搬家的是 [`sched_move_task()`](../../linux/kernel/sched/core.c#L9232)：

1. 持任务 rq 锁并更新时钟。
2. 进入 `sched_change` 作用域：若任务在队列上，以 `DEQUEUE_SAVE | DEQUEUE_MOVE` 出队；若正在运行，先 `put_prev_task()`，见 [`sched_change_begin()`](../../linux/kernel/sched/core.c#L10911)。
3. [`sched_change_group()`](../../linux/kernel/sched/core.c#L9203) 从新的有效 cpu css 取出 `task_group`，经 autogroup 处理后写入 `p->sched_task_group`。
4. 公平类的 [`task_change_group_fair()`](../../linux/kernel/sched/fair.c#L13801) 从旧队列解除 PELT 负荷、调用 `set_task_rq()`、再向新队列附加负荷。这里的 attach / detach 维护的是负荷统计，不是入队出队。
5. 离开作用域时 [`sched_change_end()`](../../linux/kernel/sched/core.c#L10933) 按原状态重新入队并恢复当前任务；原先在运行则请求重新调度。睡眠任务保持睡眠，下次唤醒时已在新组中。

尚未被首次唤醒的 `TASK_NEW` 任务只更新组指针，`task_change_group_fair()` [直接返回](../../linux/kernel/sched/fair.c#L13807)；它的队列指针在 fork 时由 [`sched_cgroup_fork()`](../../linux/kernel/sched/core.c#L4766) 设定组、并在[首次唤醒选核](../../linux/kernel/sched/core.c#L4849)时经 `__set_task_cpu()` 接好。

**autogroup。** 本配置开启了 `SCHED_AUTOGROUP`，且 [`sysctl_sched_autogroup_enabled`](../../linux/kernel/sched/autogroup.c#L10) 默认为 1（启动参数 [`noautogroup`](../../linux/kernel/sched/autogroup.c#L225) 可关闭）。第 3 步中的 [`autogroup_task_group()`](../../linux/kernel/sched/autogroup.h#L33) 只在**有效 cpu 组是根任务组**时才把任务改挂到所属会话的 autogroup 组，见 [`task_wants_autogroup()`](../../linux/kernel/sched/autogroup.c#L131)。因此：任务一旦处在拥有自己 cpu 状态的非根组，autogroup 不起作用；若 cpu 控制器没有沿路径启用，任务实际在会话级 autogroup 组中竞争，而不是直接在 `rq->cfs` 上。

## 4. `cpu.weight`：分层公平调度

### 4.1 目标：一颗 CPU 上先分组，再分任务

单 CPU，无配额，所有线程持续可运行。业务组与批处理组的 `cpu.weight` 为 100、300；业务组有两个 nice 0 线程，批处理组有一个：

| 竞争发生在哪一层 | 比较什么 | 长期份额 |
| ---------------- | -------- | -------- |
| 根公平队列 | 业务组实体 vs 批处理组实体，100 : 300 | 业务组 1/4，批处理组 3/4 |
| 业务组的组内队列 | 两个线程的任务实体，1024 : 1024 | 每个线程 1/8 整颗 CPU |
| 批处理组的组内队列 | 只有一个任务实体 | 3/4 整颗 CPU |

由此可得三个结论：

- 业务组多建线程，只会继续切分本组的 1/4，不会让本组整体变大。
- 批处理线程睡眠后，业务组可以用掉整颗 CPU；权重**不是预留，也不是上限**。
- 100、300 是接口值，1024 是 nice 0 的任务权重，二者处在不同层，不能相加比较。内部还统一乘了 [`scale_load()`](../../linux/kernel/sched/sched.h#L148) 的放大系数，比例不变。

多 CPU 时，每颗 CPU 上的组实体权重还要按本地负荷修正，见第 4.4 节。

### 4.2 入队向上，选人向下

**入队向上。** 业务组在 CPU 0 的队列原本为空。工作线程 1 被唤醒时，[`enqueue_task_fair()`](../../linux/kernel/sched/fair.c#L7080) 沿 `se->parent` 向上：先把任务实体放进业务组队列，再把业务组实体放进根队列；遇到已在队列上的祖先就停止插树，但[第二段循环](../../linux/kernel/sched/fair.c#L7148)仍继续向上更新 PELT、组权重和层级计数。之后工作线程 2 被唤醒，只增加业务组队列中的竞争者。

**选人向下。** 调度入口 [`__schedule()`](../../linux/kernel/sched/core.c#L6904) 经 `pick_next_task()` 进入 [`__pick_next_task()`](../../linux/kernel/sched/core.c#L5989)；当本 CPU 只有公平类任务时走快速路径直接调用公平类，否则按调度类优先级逐个询问。到了公平类，[`pick_task_fair()`](../../linux/kernel/sched/fair.c#L9104) 从 `rq->cfs` 开始逐层挑选。下面保留完整控制流程，注释改写为阅读说明：

```c
static struct task_struct *pick_task_fair(struct rq *rq)
{
    struct sched_entity *se;
    struct cfs_rq *cfs_rq;
    struct task_struct *p;
    bool throttled;

again:
    cfs_rq = &rq->cfs;                 /* 每次都从本 CPU 根队列开始 */
    if (!cfs_rq->nr_queued)
        return NULL;

    throttled = false;

    do {
        /* 当前实体可能尚未 put_prev，先结算它已用的时间 */
        if (cfs_rq->curr && cfs_rq->curr->on_rq)
            update_curr(cfs_rq);

        throttled |= check_cfs_rq_runtime(cfs_rq); /* 第 5.5 节 */

        se = pick_next_entity(rq, cfs_rq, true);   /* 本层 EEVDF 选择 */
        if (!se)
            goto again;                /* 选到 delayed 实体并完成出队，重来 */
        cfs_rq = group_cfs_rq(se);     /* 即 se->my_q；任务实体为 NULL */
    } while (cfs_rq);

    p = task_of(se);                   /* 最后一个 se 必然是任务实体 */
    if (unlikely(throttled))
        task_throttle_setup_work(p);
    return p;
}
```

用三层示例走一遍。目录为 `根 → 业务组 → 请求处理组`，请求处理组中有工作线程 1、2，所有对象都在 CPU 0 上：

| 轮次 | 当前队列 | 选中的实体 | 沿 `my_q` 进入 |
| ---- | -------- | ---------- | -------------- |
| 1 | `rq->cfs` | 业务组的组实体 | 业务组的组内队列 |
| 2 | 业务组的组内队列 | 请求处理组的组实体 | 请求处理组的组内队列 |
| 3 | 请求处理组的组内队列 | 工作线程 2 的任务实体 | `NULL`，循环结束 |

每层只比较**本层**的实体。EEVDF 先筛出 **eligible**（合格）实体，即相对本层加权平均虚拟时间没有多拿服务、lag ≥ 0 的实体，见 [`vruntime_eligible()` 及其推导](../../linux/kernel/sched/fair.c#L802)，再在其中选虚拟截止期最早者，见 [`pick_eevdf()`](../../linux/kernel/sched/fair.c#L1015)。批处理组的线程不会与请求处理组内的线程直接比较；一个线程要运行，它的每一层祖先组实体都必须在各自那一层胜出。遍历走的是当前 CPU 上已经建好的 `my_q` 指针，不扫描 cgroup 目录或 `task_group.children`。

循环中有三个分支，会改变“直线向下”的路径：

| 情况 | 源码行为 | 影响 |
| ---- | -------- | ---- |
| 选中的实体带 `sched_delayed` | [`pick_next_entity()`](../../linux/kernel/sched/fair.c#L5687) 完成延迟出队并返回 `NULL` | 出队可能沿祖先传播，路径失效，`goto again` 从根重选 |
| 某层带宽检查返回真 | 记入 `throttled`，仍继续向下 | 选到任务后安排限流 work，见第 5.5 节 |
| 根队列 `nr_queued == 0` | 返回 `NULL` | [`pick_next_task_fair()` 的 idle 分支](../../linux/kernel/sched/fair.c#L9201) 先尝试 newidle 均衡，仍无任务才交给 idle 类 |

### 4.3 选中之后：各层 `curr` 与逐层记账

`pick_task_fair()` 只决定“选谁”。随后 [`pick_next_task_fair()`](../../linux/kernel/sched/fair.c#L9170) 从旧、新任务实体出发向上，用 `depth` 对齐层级，用 [`is_same_group()`](../../linux/kernel/sched/fair.c#L410) 找到两条路径汇合的队列，只在汇合点以下调用 [`put_prev_entity()`](../../linux/kernel/sched/fair.c#L5700) / [`set_next_entity()`](../../linux/kernel/sched/fair.c#L5632)。旧任务来自其他调度类时，走 [`set_next_task_fair()`](../../linux/kernel/sched/fair.c#L13778) 沿 `parent` 一路设置。

工作线程 2 运行时，CPU 0 上的状态是：

| 对象 | `curr` 指向 |
| ---- | ----------- |
| `rq->curr`（`task_struct *`） | 工作线程 2 |
| `rq->cfs.curr` | 业务组的组实体 |
| 业务组队列的 `curr` | 请求处理组的组实体 |
| 请求处理组队列的 `curr` | 工作线程 2 的任务实体 |

这些 `curr` 是**同一个任务在各层的代表**。因此执行时间要逐层记账：tick 时 [`task_tick_fair()`](../../linux/kernel/sched/fair.c#L13588) 沿 `parent` 对每层调用 `entity_tick()` → [`update_curr()`](../../linux/kernel/sched/fair.c#L1302)，每层推进本层实体的 `vruntime`，并[扣本层队列的预算](../../linux/kernel/sched/fair.c#L1324)。

### 4.4 从 `cpu.weight` 到每 CPU 组实体权重

**第一步：接口值换成组权重。** 普通组的范围是 1～10000，默认 100，见 [`CGROUP_WEIGHT_MIN`](../../linux/include/linux/cgroup.h#L39)。[`cpu_weight_write_u64()`](../../linux/kernel/sched/core.c#L10140) 用 [`sched_weight_from_cgroup()`](../../linux/kernel/sched/sched.h#L259) 计算 `round(w × 1024 / 100)`，再 `scale_load()` 后写入 `tg->shares`。x86-64 上：

```text
cpu.weight = 100 → 1024 → scale_load(1024) = 1048576 = tg->shares
```

根组没有 `cpu.weight` 文件。写入后，[`__sched_group_set_shares()`](../../linux/kernel/sched/fair.c#L13976) 遍历每颗 possible CPU，对组实体及其祖先调用 `update_cfs_group()`。

**第二步：按本地负荷占比分摊。** 同一个组可能同时在多颗 CPU 上有负荷，若每个组实体都拿完整的 `tg->shares`，该组在多 CPU 上会被高估。理想关系是：

```text
本 CPU 组实体权重 ≈ tg->shares × 本 CPU 组内负荷 / 全组各 CPU 负荷之和
```

每次计算都去读其他 CPU 的队列代价太高，所以用已上报的汇总近似分母，推导见 [源码说明](../../linux/kernel/sched/fair.c#L4014)。实现是 [`calc_group_shares()`](../../linux/kernel/sched/fair.c#L4086)，参数 `cfs_rq` 是**组内队列**（省略原注释，注释为阅读说明）：

```c
static long calc_group_shares(struct cfs_rq *cfs_rq)
{
    long tg_weight, tg_shares, load, shares;
    struct task_group *tg = cfs_rq->tg;

    tg_shares = READ_ONCE(tg->shares);

    /* 本地负荷：即时权重（降尺度）与 PELT 历史取较大者 */
    load = max(scale_load_down(cfs_rq->load.weight), cfs_rq->avg.load_avg);

    /* 分母：全组汇总里，把本 CPU 的旧贡献换成此刻的 load */
    tg_weight = atomic_long_read(&tg->load_avg);
    tg_weight -= cfs_rq->tg_load_avg_contrib;
    tg_weight += load;

    shares = (tg_shares * load);
    if (tg_weight)
        shares /= tg_weight;

    return clamp_t(long, shares, MIN_SHARES, tg_shares);
}
```

| 步骤 | 说明 |
| ---- | ---- |
| 统一尺度 | `load.weight` 是 `scale_load()` 后的内部尺度，PELT 的 `load_avg` 是降尺度后的值，所以先 `scale_load_down()` 再取 `max()` |
| 取较大者 | 刚唤醒时即时权重已上升、PELT 还没涨，取即时值能及时反映需求；负荷下降时则由历史值平滑 |
| 换掉旧贡献 | `tg->load_avg` 已含本 CPU 上次上报的 `tg_load_avg_contrib`，减去它再加上此刻的 `load`，避免本 CPU 被重复或过时计入 |
| 截断 | 结果落在 `[MIN_SHARES, tg_shares]`。这里的 [`MIN_SHARES = 2`](../../linux/kernel/sched/sched.h#L538) 是未缩放的原始值，以便小权重组能分摊到多颗 CPU，见 [下限说明](../../linux/kernel/sched/fair.c#L4105) |

共享汇总 `tg->load_avg` 由 [`update_tg_load_avg()`](../../linux/kernel/sched/fair.c#L4258) 差量更新；为减少跨 CPU 写共享数据，它要求距上次上报至少 1 ms 且变化超过旧贡献的 1/64，见 [上报条件](../../linux/kernel/sched/fair.c#L4273)。`calc_group_shares()` 中的减旧加新只作用于局部变量，不写回汇总。

**数值例子。** 业务组 `cpu.weight = 100`，CPU 0 上有 1 个、CPU 1 上有 3 个持续运行的 nice 0 线程；取 PELT 已稳定、贡献已上报的快照：

| | CPU 0 组内队列 | CPU 1 组内队列 |
| --- | --- | --- |
| `scale_load_down(load.weight)` | 1024 | 3072 |
| `avg.load_avg` = `tg_load_avg_contrib` | 1024 | 3072 |
| 本地 `load` | 1024 | 3072 |
| 分母 `tg_weight` | 4096 − 1024 + 1024 = 4096 | 4096 − 3072 + 3072 = 4096 |
| 组实体权重 | 1048576 × 1/4 = **262144** | 1048576 × 3/4 = **786432** |

变的是**代表业务组去根队列竞争的组实体**；组内线程的任务实体仍是 nice 0 的 1048576。再看刚唤醒的边界：其他 CPU 贡献为 0，本 CPU 旧贡献 100，即时负荷已是 1024，则 `tg_weight = 100 − 100 + 1024`，本 CPU 立即拿到完整 `tg->shares`。由于各 CPU 独立修正、整数截断和下限，各组实体权重之和**不保证时刻等于** `tg->shares`，见 [近似边界说明](../../linux/kernel/sched/fair.c#L4054)。

**第三步：写回组实体。** [`update_cfs_group()`](../../linux/kernel/sched/fair.c#L4124) 把计算与写入连起来（注释为阅读说明）：

```c
static void update_cfs_group(struct sched_entity *se)
{
    struct cfs_rq *gcfs_rq = group_cfs_rq(se);   /* se->my_q：组内队列 */
    long shares;

    /* 组变空时保留原权重，照顾延迟出队 */
    if (!gcfs_rq || !gcfs_rq->load.weight)
        return;

    shares = calc_group_shares(gcfs_rq);
    if (unlikely(se->load.weight != shares))
        reweight_entity(cfs_rq_of(se), se, shares); /* se->cfs_rq：父队列 */
}
```

注意输入来自 `my_q`（组内队列），输出写到 `se` 并更新 `cfs_rq_of(se)`（父队列）。任务实体的 `my_q` 为空，直接返回，所以这里不会改线程的 nice 权重。除写入权重外，以下路径也会反复触发这条计算：

| 触发点 | 原因 |
| ------ | ---- |
| [`__sched_group_set_shares()`](../../linux/kernel/sched/fair.c#L13976) | 配置变了 |
| [`enqueue_entity()`](../../linux/kernel/sched/fair.c#L5441)、[`dequeue_entity()`](../../linux/kernel/sched/fair.c#L5610) | 组内队列的成员变了 |
| [`entity_tick()`](../../linux/kernel/sched/fair.c#L5734) | 运行中 PELT 历史在变 |

多层组按同样方式逐层进行：子组实体的权重成为父组内队列 `load.weight` 的一部分，父组再据此算自己的组实体权重。

### 4.5 `reweight_entity()`：换权重时维护父层账本

[`reweight_entity()`](../../linux/kernel/sched/fair.c#L3949) 的第一个参数是**组实体所在的父队列**。若实体仍在队列上，它要保证父队列的总权重和 EEVDF 账本不因换权重而错乱：

| 步骤 | 做什么 |
| ---- | ------ |
| 移除旧贡献 | 先结算父队列当前实体；记下待改实体的 lag 与相对截止期；[从父队列总权重减去旧值](../../linux/kernel/sched/fair.c#L3971)；必要时暂时摘树；移除旧 PELT 贡献 |
| 缩放虚拟时间 | [`rescale_entity()`](../../linux/kernel/sched/fair.c#L3975) 按新旧权重之比缩放 `vlag`、相对截止期和保护边界，推导见 [源码说明](../../linux/kernel/sched/fair.c#L3920) |
| 写入新权重 | [`update_load_set()`](../../linux/kernel/sched/fair.c#L177) 赋值 `weight`，把 `inv_weight` 置 0 使倒数缓存失效 |
| 恢复新贡献 | 按新权重重算 PELT，[把新值加回父队列](../../linux/kernel/sched/fair.c#L3992)，恢复虚拟时间位置并重新入树 |

新权重最终通过 [`calc_delta_fair()`](../../linux/kernel/sched/fair.c#L290) 影响父层竞争：组实体执行同样时长，`vruntime` 的增量约为 `执行时长 × NICE_0_LOAD / se->load.weight`，权重越大推进越慢，越容易保持资格并获得较早的截止期，见 [`update_curr()`](../../linux/kernel/sched/fair.c#L1306)。完整链路是：**`cpu.weight` → `tg->shares` → 按本地负荷算组实体 `load.weight` → 父层虚拟时间推进速度**。

### 4.6 层级计数与延迟出队

本层实体数与子树任务数是两种计数：

| 计数 | 含义 | 业务组有两个可运行线程时 |
| ---- | ---- | ------------------------ |
| `nr_queued` | 本层直接排队或运行的实体数 | 业务组队列 2；根队列若只有业务组实体则为 1 |
| `h_nr_queued` | 子树中计入队列的任务数，含 delayed | 业务组队列与根队列都是 2 |
| `h_nr_runnable` | 子树中可运行任务数，不含 delayed | 2 |

本版本有**延迟出队**（delayed dequeue）：任务睡眠时，若其实体此刻不 eligible（已多拿了服务，lag 为负），实体会暂留在队列上继续“还账”，直到被选中时才真正出队，见 [`dequeue_entity()` 的判断](../../linux/kernel/sched/fair.c#L5571) 和 [`dequeue_entities()`](../../linux/kernel/sched/fair.c#L7209)。工作线程 1 睡眠后，[`set_delayed()`](../../linux/kernel/sched/fair.c#L5503) 先减少 runnable 计数：

| 状态 | `nr_queued` | `h_nr_queued` | `h_nr_runnable` |
| ---- | ----------- | ------------- | --------------- |
| 两个线程都可运行 | 2 | 2 | 2 |
| 线程 1 睡眠，实体标记 delayed | 2 | 2 | 1 |
| 线程 1 的实体被选中、完成出队 | 1 | 1 | 1 |

所以“最后一个任务睡眠”不一定立即让整条祖先链从树上消失；组内队列变空时，`update_cfs_group()` 也保留组实体原权重。

### 4.7 `cpu.idle`：组级的 SCHED_IDLE

写入 1 后，[`sched_group_set_idle()`](../../linux/kernel/sched/fair.c#L14009) 置位 `tg->idle` 和各 CPU 的 `cfs_rq->idle`，把 `shares` 设为 `scale_load(WEIGHT_IDLEPRIO)`（未缩放值 [3](../../linux/kernel/sched/sched.h#L2351)），并把组内任务计入祖先的 `h_nr_idle`。它比“很小的权重”多两点行为：唤醒时非 idle 实体可以抢占 idle 实体、反之不行，见 [`check_preempt_wakeup_fair()`](../../linux/kernel/sched/fair.c#L8970) 与 [`se_is_idle()`](../../linux/kernel/sched/fair.c#L465)；选核时只跑 idle 任务的 CPU 被视为可接收普通任务，见 [`sched_idle_rq()`](../../linux/kernel/sched/fair.c#L7032)。

它仍是公平类内部的低优先级，不保证只在其他任务都睡眠时才运行。idle 组写 `cpu.weight` 返回 `-EINVAL`，见 [`sched_group_set_shares()`](../../linux/kernel/sched/fair.c#L13995)；改回 0 时权重恢复为默认 `NICE_0_LOAD`，**不恢复**此前的自定义值。

## 5. `cpu.max`：CFS 带宽控制

`cpu.weight` 只决定争用时怎么分；`cpu.max` 增加一道独立约束：**本组还能执行多久，额度用完时任务怎样暂停，补充后又怎样回来。** 权重再大也不会增加预算。

### 5.1 限制对象：全组在所有 CPU 上的累计执行时间

`cpu.max` 的格式是 `quota period`，单位都是 μs。`50000 100000` 表示每 100 ms 给全组新增 50 ms 执行额度，由组内所有 CPU 上的公平类执行**共同消耗**。从一个完整周期开始，线程持续运行时：

| 并行线程数（各占一颗 CPU） | 每线程执行 | 用完预算时已过墙钟 | 距下次补充还需等待 |
| -------------------------- | ---------- | ------------------ | ------------------ |
| 1 | 50 ms | 约 50 ms | 约 50 ms |
| 2 | 25 ms | 约 25 ms | 约 75 ms |
| 4 | 12.5 ms | 约 12.5 ms | 约 87.5 ms |

并行度越高，预算越早耗尽，集中等待也越长。`quota / period = 0.5` 相当于半颗逻辑 CPU 的执行时间，不规定线程数和 CPU 位置。

| 写法 | 含义 |
| ---- | ---- |
| `max 100000` | 本组不限，仍受祖先约束；新组的默认值 |
| `50000 100000` | 约 0.5 CPU |
| `200000 100000` | 约 2 CPU，quota 可以大于 period |

省略 period 时保留原值，见 [`cpu_max_write()`](../../linux/kernel/sched/core.c#L10237) 和 [`cpu_period_quota_parse()`](../../linux/kernel/sched/core.c#L10208)；默认周期见 [`default_bw_period_us()`](../../linux/kernel/sched/sched.h#L439)。根组没有 `cpu.max` / `cpu.max.burst`，见 [文件注册](../../linux/kernel/sched/core.c#L10273)。

### 5.2 数据结构：共享池、本地余额与限流状态

如果每次记账都改同一个全组计数，多颗 CPU 会频繁争用一把锁。内核因此把预算拆成两级账本：**全组共享池负责分配，每 CPU 队列负责扣账**。

```mermaid
flowchart TD
    TG["业务组 A：task_group"] -->|"内嵌"| BW["cfs_bandwidth：共享池<br/>quota / period / runtime"]
    TG -->|"cfs_rq[0]"| Q0["A 在 CPU 0 的 cfs_rq<br/>runtime_remaining"]
    TG -->|"cfs_rq[1]"| Q1["A 在 CPU 1 的 cfs_rq<br/>runtime_remaining"]
    BW -.->|"按需分配额度"| Q0
    BW -.->|"按需分配额度"| Q1
    P0["CPU 0 上的任务"] -->|"执行时扣账"| Q0
    P1["CPU 1 上的任务"] -->|"执行时扣账"| Q1
```

实线是对象关系，虚线是额度流动。每颗 CPU 都向**同一个池**领取，没有为每颗 CPU 复制一份 quota。

**共享池 `cfs_bandwidth`**，摘自 [`struct cfs_bandwidth`](../../linux/kernel/sched/sched.h#L445)：

```c
struct cfs_bandwidth {
    raw_spinlock_t lock;             /* 保护分配、归还、补充 */
    ktime_t period;                  /* 补充周期，ns */
    u64 quota;                       /* 每周期新增额度；RUNTIME_INF 表示无限 */
    u64 runtime;                     /* 池中尚未分配出去的额度 */
    u64 burst;                       /* 允许累积到 quota 之上的额度 */
    u64 runtime_snap;                /* 上次补充后的余额，用于 burst 统计 */
    s64 hierarchical_quota;          /* 祖先收紧后的比例，第 5.8 节 */
    u8 idle;                         /* 上个周期是否无人领取 */
    u8 period_active;
    u8 slack_started;
    struct hrtimer period_timer;     /* 周期补充 */
    struct hrtimer slack_timer;      /* 延后再分配归还的额度 */
    struct list_head throttled_cfs_rq; /* 因本组预算而限流的各 CPU 队列 */
    /* 省略统计成员 */
};
```

**每 CPU 队列的带宽成员**，摘自 [`struct cfs_rq`](../../linux/kernel/sched/sched.h#L751)：

```c
struct cfs_rq {
    /* 省略 */
    int runtime_enabled;             /* 本队列是否按本组有限 quota 记账 */
    s64 runtime_remaining;           /* 本地余额，可以为负（欠账） */
    /* 省略 */
    u64 throttled_clock;             /* 统计：自身限流起点 */
    /* 省略 */
    u64 throttled_clock_self;        /* 统计：自身或祖先限流起点 */
    u64 throttled_clock_self_time;
    bool throttled:1;                /* 本层是否因本组预算而限流 */
    bool pelt_clock_throttled:1;
    int throttle_count;              /* 本层及祖先共有几层限流 */
    struct list_head throttled_list; /* 挂入共享池的 throttled_cfs_rq */
    struct list_head throttled_csd_list; /* 跨 CPU 异步解限流 */
    struct list_head throttled_limbo_list; /* 被摘下的任务 */
};
```

**任务侧**的三个成员见第 3.3 节：`sched_throttle_work` 是返回用户态前执行的回调，`throttle_node` 挂入 limbo 链表，`throttled` 表示任务已被摘下。本文把“任务仍可运行、但被暂时移出公平竞争”的状态称为 **limbo**。

两组概念最容易混淆：

| 概念对 | 区别 |
| ------ | ---- |
| 共享池 `runtime` vs 本地 `runtime_remaining` | 领取只是把额度从池移到本地；只有执行扣账才消耗预算 |
| 队列 `cfs_rq->throttled` vs `throttle_count` | 前者只看**本层**；后者数**整条祖先链**。本组 `runtime_enabled == 0` 也可能因祖先限流而 `throttle_count > 0`，见 [`throttled_hierarchy()`](../../linux/kernel/sched/fair.c#L5897) |
| 队列限流 vs 任务限流 | 队列被标记后，任务还要等 task work 执行才进入 limbo，`p->throttled` 此时才置位 |

两张链表分别服务于恢复路径的两步：

| 链表头 | 挂入的节点 | 用途 |
| ------ | ---------- | ---- |
| `cfs_bandwidth.throttled_cfs_rq` | 各 CPU 的 `cfs_rq.throttled_list` | 有额度时找到要恢复的队列 |
| `cfs_rq.throttled_limbo_list` | 任务的 `throttle_node` | 层级限流解除后，把任务重新入队 |

### 5.3 主流程：领取不等于消耗，标记不等于暂停

先看一个余额快照：A 的池中剩 10 ms，两颗 CPU 的本地余额都是 0，忽略补充与祖先：

| 事件 | 池 `runtime` | CPU 0 本地 | CPU 1 本地 | 累计执行 |
| ---- | ------------ | ---------- | ---------- | -------- |
| 起点 | 10 ms | 0 | 0 | 0 |
| CPU 0 领取 5 ms | 5 ms | 5 ms | 0 | 0 |
| CPU 1 领取 5 ms | 0 | 5 ms | 5 ms | 0 |
| 两颗 CPU 各执行 2 ms | 0 | 3 ms | 3 ms | 4 ms |

池已经为空，两颗 CPU 却还能各执行 3 ms。完整的暂停与恢复流程如下（假设 A 没有受限祖先）：

```mermaid
flowchart TD
    RUN["任务执行：扣本地 runtime_remaining"] --> CHECK{"本地余额 > 0？"}
    CHECK -->|"是"| RUN
    CHECK -->|"否"| GET["向共享池领取"]
    GET -->|"本地余额转正"| RUN
    GET -->|"仍不足"| MARK["调度路径复查后<br/>标记本 CPU 队列 throttled"]
    MARK --> PICK["向下选到任务<br/>安排 sched_throttle_work"]
    PICK --> STOP["返回用户态前 work 执行<br/>任务出队进入 limbo"]
    STOP -.->|"等待额度"| REFILL["周期补充共享池<br/>或其他 CPU 归还额度"]
    REFILL --> DIST["给受限队列还欠账<br/>余额转正后解除限流"]
    DIST --> READY["throttle_count 归零<br/>limbo 任务重新入队"]
    READY -->|"再次被选中"| RUN
```

补充定时器独立于任务运行，任何阶段都可能遇到补充或配置变化，所以后续函数在关键点都会复查状态。下面按“扣账 → 限流 → 恢复”的顺序展开。

### 5.4 扣账与领取

**扣哪种时间。** [`update_curr()`](../../linux/kernel/sched/fair.c#L1302) 用 [`update_se()`](../../linux/kernel/sched/fair.c#L1232) 取得本次执行时长 `delta_exec`，一份经 `calc_delta_fair()` 按权重换算后推进 `vruntime`，另一份**原样**交给 `account_cfs_rq_runtime()` 扣预算。所以权重翻倍不会让同样执行 1 ms 只扣 0.5 ms。`delta_exec` 来自任务时钟，包含用户态和内核态执行、自旋等待，不含睡眠和排队。本配置未开启 `IRQ_TIME_ACCOUNTING`，中断时间不从任务时钟中扣除；开启了 `PARAVIRT_TIME_ACCOUNTING`，运行时启用 steal 记账时会扣除宿主机拿走的时间，见 [`update_rq_clock_task()`](../../linux/kernel/sched/core.c#L787)。

**扣账。** [`account_cfs_rq_runtime()`](../../linux/kernel/sched/fair.c#L5877) 在全局 `cfs_bandwidth_used()` 为假或本队列 `runtime_enabled == 0` 时直接返回。需要记账时进入 [`__account_cfs_rq_runtime()`](../../linux/kernel/sched/fair.c#L5859)：

```c
static void __account_cfs_rq_runtime(struct cfs_rq *cfs_rq, u64 delta_exec)
{
    cfs_rq->runtime_remaining -= delta_exec;   /* 先扣，允许变负 */

    if (likely(cfs_rq->runtime_remaining > 0))
        return;

    if (cfs_rq->throttled)                     /* 已限流：只记欠账 */
        return;

    /* 领不到额度就请求重新调度，让调度路径去标记限流 */
    if (!assign_cfs_rq_runtime(cfs_rq) && likely(cfs_rq->curr))
        resched_curr(rq_of(cfs_rq));
}
```

记账是**执行后分段结算**，不是每一纳秒前检查。上次剩 0.2 ms、这次结算 0.5 ms，余额就是 −0.3 ms，这笔欠账后续领取时要先还清。

**领取。** [`assign_cfs_rq_runtime()`](../../linux/kernel/sched/fair.c#L5847) 持池锁调用 [`__assign_cfs_rq_runtime()`](../../linux/kernel/sched/fair.c#L5819)，目标是把本地余额**补到** `sched_cfs_bandwidth_slice()`，默认 5 ms，来自 [`sysctl_sched_cfs_bandwidth_slice`](../../linux/kernel/sched/fair.c#L125)，可经 [`sched_cfs_bandwidth_slice_us`](../../linux/kernel/sched/fair.c#L137) 调整：

```text
需要 = 目标余额 − 当前本地余额
实际 = min(池余额, 需要)
池余额 −= 实际；本地余额 += 实际
成功条件：本地余额 > 0
```

| 领取前本地 | 池余额 | 需要 | 实际 | 领取后本地 | 结果 |
| ---------- | ------ | ---- | ---- | ---------- | ---- |
| 0 | 10 ms | 5 ms | 5 ms | 5 ms | 成功 |
| −1 ms | 10 ms | 6 ms | 6 ms | 5 ms | 先还欠账再补满 |
| −1 ms | 2 ms | 6 ms | 2 ms | 1 ms | 未补满但已转正，成功 |
| −1 ms | 0.5 ms | 6 ms | 0.5 ms | −0.5 ms | 失败，欠账保留 |

这个 5 ms 是额度批发的粒度，与 EEVDF 的请求 slice 无关。有限 quota 的领取还会[按需启动周期定时器](../../linux/kernel/sched/fair.c#L5832)，并在成功转出额度时清除池的 `idle` 标记。

**空队列重新有任务时也要检查。** 若 A 在 CPU 0 上带着欠账，第一个实体入队时不能等它跑一段才发现。[`enqueue_entity()`](../../linux/kernel/sched/fair.c#L5469) 在 `nr_queued == 1` 时调用 [`check_enqueue_throttle()`](../../linux/kernel/sched/fair.c#L6565)，以 `delta_exec = 0` 走一遍记账与领取，仍不足就尝试标记限流。

### 5.5 限流：先标记队列，再让任务进入 limbo

本版本的关键点是：**`throttle_cfs_rq()` 不会立即把组实体摘下**。它只标记受限层级；调度器仍能沿这条路径选到任务，再让任务在返回用户态前把自己摘下。

```mermaid
sequenceDiagram
    participant S as 调度路径（持 rq 锁）
    participant Q as 本 CPU 的组内队列
    participant P as 被选中的用户任务
    S->>Q: check_cfs_rq_runtime() → throttle_cfs_rq()
    Note over Q: 最后再领一次 1 ns；失败则<br/>throttled = 1，子树 throttle_count++
    S->>P: pick_task_fair() 选到任务<br/>task_throttle_setup_work()
    Note over P: 仍在运行，尚未进入 limbo
    P->>Q: 返回用户态前执行 work<br/>复查 throttle_count
    Q-->>P: 仍受限：公平出队，挂入 limbo
    Note over P: p->throttled = true，请求重新调度
```

**第一步：标记队列。** 检查点有三处：入队时的 `check_enqueue_throttle()`、[`put_prev_entity()`](../../linux/kernel/sched/fair.c#L5710) 让出当前实体时、[`pick_task_fair()`](../../linux/kernel/sched/fair.c#L9123) 向下选人时。后两处调用 [`check_cfs_rq_runtime()`](../../linux/kernel/sched/fair.c#L6612)，需要时进入 [`throttle_cfs_rq()`](../../linux/kernel/sched/fair.c#L6141)：

1. 持池锁[再领一次](../../linux/kernel/sched/fair.c#L6147)，目标只是 +1 ns。检查与标记之间可能恰好有补充，只要此刻能还清欠账并转正，就放弃限流。
2. 失败才把本队列[挂入池的 `throttled_cfs_rq`](../../linux/kernel/sched/fair.c#L6160)。
3. 从 A 向下遍历组树，对**本 CPU 上** A 及所有后代队列执行 [`tg_throttle_down()`](../../linux/kernel/sched/fair.c#L6118)，`throttle_count` 加一；空队列在此冻结 PELT 时钟。
4. 置 `cfs_rq->throttled = 1`。

这里没有 `dequeue_entity()`，也只影响 A 在**这一颗 CPU** 上的队列；其他 CPU 仍按各自余额运行。

| 某队列在本 CPU 上的约束 | `throttled` | `throttle_count` |
| ----------------------- | ----------- | ---------------- |
| 自己与祖先都未限流 | 0 | 0 |
| 仅一个祖先限流 | 0 | 1 |
| 仅自己限流 | 1 | 1 |
| 自己和一个祖先都限流 | 1 | 2 |

**第二步：给任务挂 work。** `pick_task_fair()` 用 `throttled |=` 记住路径上任一层受限，选到任务后调用 [`task_throttle_setup_work()`](../../linux/kernel/sched/fair.c#L6092)：已挂过则跳过；内核线程和 `PF_EXITING` 任务不会返回用户态，也跳过；否则以 `TWA_RESUME` 挂上 `sched_throttle_work`。普通任务在 [`resume_user_mode_work()`](../../linux/include/linux/resume_user_mode.h#L41) 中执行它，见 [`TWA_RESUME` 的说明](../../linux/kernel/task_work.c#L40)；进入 KVM guest 前也会处理，见 [虚拟化入口](../../linux/kernel/entry/virt.c#L16)。因此预算耗尽后，任务还可能在内核里继续执行一段，这段时间照样扣账，扩大欠账。

**第三步：work 复查后进入 limbo。** [`throttle_cfs_rq_work()`](../../linux/kernel/sched/fair.c#L5913) 运行时，任务可能已换组、换调度类或预算已恢复，所以它持任务 rq 锁复查。下面摘录加锁后的主体，注释改写为阅读说明：

```c
    scoped_guard(task_rq_lock, p) {
        se = &p->se;
        cfs_rq = cfs_rq_of(se);

        if (p->sched_class != &fair_sched_class)
            return;                    /* 已不是公平类 */

        if (!cfs_rq->throttle_count)
            return;                    /* 已补充或已迁出受限层级 */
        rq = scope.rq;
        update_rq_clock(rq);
        WARN_ON_ONCE(p->throttled || !list_empty(&p->throttle_node));
        dequeue_task_fair(rq, p, DEQUEUE_SLEEP | DEQUEUE_THROTTLE);
        list_add(&p->throttle_node, &cfs_rq->throttled_limbo_list);
        p->throttled = true;           /* 必须在出队之后置位 */
        resched_curr(rq);
    }
```

见 [复查与出队](../../linux/kernel/sched/fair.c#L5942)。`DEQUEUE_THROTTLE` [跳过延迟出队](../../linux/kernel/sched/fair.c#L5566)，确保实体真正离开队列。任务总是挂在**自己所属队列**的 limbo 链表上：即使约束来自父组 P，A 的任务也挂在 A 的队列上。

**limbo 中的任务仍可能显示为 R。** work 直接调用公平类出队，并没有把任务变成阻塞睡眠。稳定状态下：

| 状态 | `p->throttled` | `p->on_rq` | `p->se.on_rq` | 能否被选中 |
| ---- | -------------- | ---------- | ------------- | ---------- |
| 正常排队或运行 | 0 | 1 | 1 | 能 |
| 限流停在 limbo | 1 | 1 | 0 | 不能 |
| 普通睡眠且已出队 | 0 | 0 | 0 | 不能 |

limbo 任务已经从 `rq->nr_running` 中减去，见 [计数更新](../../linux/kernel/sched/fair.c#L7292)。所以 `/proc` 里的 `State: R` 不能说明任务还在公平队列里竞争。

### 5.6 恢复：补充、分发、解除限流

恢复同样分两层：先让**队列**余额转正、解除层级限流，再让 **limbo 任务**重新入队。

**周期补充。** `period_timer` 回调 [`sched_cfs_period_timer()`](../../linux/kernel/sched/fair.c#L6640) 调用 [`do_sched_cfs_period_timer()`](../../linux/kernel/sched/fair.c#L6393)，先由 [`__refill_cfs_bandwidth_runtime()`](../../linux/kernel/sched/fair.c#L5795) 补充：

```text
池 runtime = min(旧 runtime + quota, quota + burst)
```

补充**不清零**各 CPU 的本地余额：正余额继续可用，负余额仍是欠账。一次 `do_sched_cfs_period_timer()` 只补一次 quota，即使跨过多个周期（`overrun > 1`）也不按倍数补。

**分发。** 池中有余额且有受限队列时，[`distribute_cfs_runtime()`](../../linux/kernel/sched/fair.c#L6305) 遍历 `throttled_cfs_rq`，每个队列只给到 **+1 ns**：

```c
        runtime = -cfs_rq->runtime_remaining + 1;
        if (runtime > cfs_b->runtime)
            runtime = cfs_b->runtime;
        cfs_b->runtime -= runtime;
        /* ... */
        cfs_rq->runtime_remaining += runtime;
```

见 [分发计算](../../linux/kernel/sched/fair.c#L6336)。本地欠 1 ms 就需要 1 ms + 1 ns；池里只有 0.5 ms 时分完仍欠 0.5 ms，继续等待。三处“领取”的目标不同：

| 场景 | 目标余额 | 目的 |
| ---- | -------- | ---- |
| 正常执行时领取 | +5 ms | 批量领取，减少访问共享池 |
| 标记限流前最后一次领取 | +1 ns | 确认此刻确实不能继续 |
| 给受限队列分发 | +1 ns | 先还清欠账、取得恢复资格，后续运行再按需领取 |

**解除限流。** 余额转正后由 [`unthrottle_cfs_rq()`](../../linux/kernel/sched/fair.c#L6182) 处理。它先[复查余额](../../linux/kernel/sched/fair.c#L6188)（异步路径中其他实体可能又把余额花光），然后清 `throttled`、从池链表摘下，并对本 CPU 的子树执行 [`tg_unthrottle_up()`](../../linux/kernel/sched/fair.c#L6047)：

```c
    if (--cfs_rq->throttle_count)
        return 0;                      /* 还有其他层的限流，任务继续等 */
    /* 省略：结算 PELT 冻结时长与 throttled_clock_self */
    list_for_each_entry_safe(p, tmp, &cfs_rq->throttled_limbo_list, throttle_node) {
        list_del_init(&p->throttle_node);
        p->throttled = false;
        enqueue_task_fair(rq_of(cfs_rq), p, ENQUEUE_WAKEUP);
    }
```

**只有 `throttle_count` 减到 0 的队列才取回 limbo 任务**，见 [恢复循环](../../linux/kernel/sched/fair.c#L6073)。重新入队只恢复竞争资格；若当前 CPU 正在跑 idle，会[请求重新调度](../../linux/kernel/sched/fair.c#L6230)。

**跨 CPU 异步恢复。** 定时器在一颗 CPU 上执行，受限队列却分布在多颗 CPU 上。其他 CPU 的队列挂到目标 rq 的 `cfsb_csd_list`，通过 IPI 让目标 CPU 自己解除，见 [`__unthrottle_cfs_rq_async()`](../../linux/kernel/sched/fair.c#L6274)；本 CPU 的队列在遍历结束后[单独处理](../../linux/kernel/sched/fair.c#L6367)。

**空闲队列归还额度。** 某层最后一个实体出队时，[`return_cfs_rq_runtime()`](../../linux/kernel/sched/fair.c#L6520) 把超过 [`min_cfs_rq_runtime`](../../linux/kernel/sched/fair.c#L6448)（1 ms）的部分退回池，见 [`__return_cfs_rq_runtime()`](../../linux/kernel/sched/fair.c#L6497)。例如本地剩 4 ms，退回 3 ms，留 1 ms。归还时持有本 CPU rq 锁，不能直接去解除其他 rq 的限流，所以当池余额超过一个 slice 且有受限队列时，启动 `slack_timer` 延后 5 ms 再分发，见 [源码说明](../../linux/kernel/sched/fair.c#L6531)。距周期刷新不足 7 ms 时 [`start_cfs_slack_bandwidth()`](../../linux/kernel/sched/fair.c#L6478) 不启动，回调执行时距刷新不足 2 ms 也放弃，见 [`do_sched_cfs_slack_timer()`](../../linux/kernel/sched/fair.c#L6540)。因此队列可以在周期中途恢复，但一次归还不保证立即恢复其他 CPU。

### 5.7 层级预算、burst 与跨周期余额

**父子各有账本，一次执行同时扣每一层。** 设 P 为 `100000 100000`，子组 A、B 各为 `80000 100000`。A 的任务执行时，A 与 P 在当前 CPU 上的队列都在各自的 `update_curr()` 中扣本层余额，也各自向本层的池领取。假设三组从满额开始、无补充：

| 已发生的执行 | A 剩余 | B 剩余 | P 剩余 |
| ------------ | ------ | ------ | ------ |
| A 执行 60 ms，B 执行 40 ms | 20 ms | 40 ms | 0 |

A、B 自己还有额度，但 P 已耗尽，二者都要等待。父组不会把额度复制给每个子组；子组 quota 之和可以超过父组，配置检查也不累加兄弟，见 [`tg_cfs_schedulable_down()`](../../linux/kernel/sched/core.c#L9695)。A 写成 `max` 只取消 A 自身的限制。

恢复要逐层解除。A 与 P 在同一 CPU 上都受限时，A 队列的 `throttle_count = 2`：A 先恢复只能减到 1，任务仍在 limbo；P 也恢复后减到 0，任务才重新入队，见 [`tg_unthrottle_up()`](../../linux/kernel/sched/fair.c#L6053)。父子组各有自己的 `period_timer`，初始化时还会[随机错开相位](../../linux/kernel/sched/fair.c#L6708)，不共用一个周期。

**burst 允许累积未用额度。** `cpu.max.burst`（μs，默认 0，[接口注册](../../linux/kernel/sched/core.c#L10281)）只改变补充时的截断上限 `quota + burst`。设 quota 20 ms、burst 10 ms：

| 补充前池余额 | 加 quota | 截断后 |
| ------------ | -------- | ------ |
| 0 | 20 ms | 20 ms |
| 5 ms | 25 ms | 25 ms |
| 18 ms | 38 ms | 30 ms |

只有之前没花完的额度才能让池余额超过一个 quota；burst 不是每周期额外发放。

**池余额可能暂时超过截断值。** 归还路径直接 [`cfs_b->runtime += slack_runtime`](../../linux/kernel/sched/fair.c#L6505)，不再截断。quota 10 ms、burst 0，补充后池有 10 ms；某 CPU 此时退回早先领到的 3 ms，池就暂时变成 13 ms。这 3 ms 是旧额度回流，不是新增。再加上本地余额可以跨周期保留，**一个周期内的实际执行量可以超过本周期新增的 quota**。

**周期预算不是任意滑动窗口的上限。** quota 50 ms、period 100 ms：周期 1 只在 [50, 100) ms 执行 50 ms，周期 2 补充后在 [100, 150) ms 执行 50 ms，则 [50, 150) ms 这个 100 ms 窗口看到了 100 ms 执行。内核只按周期定时器补充，不为任意起点的窗口记账。

### 5.8 写 `cpu.max` 后怎样生效

```text
写 cpu.max
  → cpu_max_write()：取旧 period / burst，解析 quota 和可选的 period
  → tg_set_bandwidth()：校验
  → tg_set_cfs_bandwidth()：更新层级比例、共享池与每 CPU 状态
```

入口见 [`cpu_max_write()`](../../linux/kernel/sched/core.c#L10237)、[`tg_set_bandwidth()`](../../linux/kernel/sched/core.c#L9835)、[`tg_set_cfs_bandwidth()`](../../linux/kernel/sched/core.c#L9564)。校验规则见 [`tg_set_bandwidth()` 的检查](../../linux/kernel/sched/core.c#L9841) 与 [范围常量](../../linux/kernel/sched/core.c#L9801)：

| 对象 | 要求 |
| ---- | ---- |
| period | 1 ms ～ 1 s |
| quota | 至少 1 ms，可以大于 period，另有防溢出上限 |
| burst | 不超过 quota；`quota + burst` 不超过上限 |
| 目标组 | 不能是根任务组 |

`cpu.max` 写入会[保留当前 burst](../../linux/kernel/sched/core.c#L10244)。原 quota 20 ms、burst 10 ms 时直接把 quota 改成 5 ms 会失败，需先调小 `cpu.max.burst`。

**三层开关。** 编译开启 `CFS_BANDWIDTH` 只是前提；新组默认无限，每 CPU 的 `runtime_enabled` 为 0，见 [`init_cfs_rq_runtime()`](../../linux/kernel/sched/fair.c#L6716)。全局还有一个 static key，见 [`cfs_bandwidth_used()`](../../linux/kernel/sched/fair.c#L5756)：只要整机有一个组启用有限 quota，热路径的带宽检查才打开。`tg_set_cfs_bandwidth()` 的更新顺序是：

1. [`__cfs_schedulable()`](../../linux/kernel/sched/core.c#L9733) 遍历组树更新 `hierarchical_quota`。
2. 本组从无限变有限时，[先增加 static key 计数](../../linux/kernel/sched/core.c#L9591)，再改字段。
3. 持池锁写入 `period / quota / burst`，调用补充函数；有限 quota 再 `start_cfs_bandwidth()`，见 [池更新](../../linux/kernel/sched/core.c#L9600)。
4. 遍历在线 CPU，在各 rq 锁内设置 `runtime_enabled`，把 `runtime_remaining` 置为 1 ns；已限流的队列立即尝试解除，见 [每 CPU 更新](../../linux/kernel/sched/core.c#L9615)。
5. 从有限变无限时，最后[减少 static key 计数](../../linux/kernel/sched/core.c#L9627)。

**`hierarchical_quota`** 保存的是“本组与所有祖先中最严格的 `quota / period` 比例”，由 [`normalize_cfs_quota()`](../../linux/kernel/sched/core.c#L9675) 和 `tg_cfs_schedulable_down()` 计算。它不是余额，不参与扣账，只用来回答“这个任务所在层级是否受带宽约束”，使用者是 [`cfs_task_bw_constrained()`](../../linux/kernel/sched/fair.c#L6842)。

### 5.9 边界路径

| 场景 | 处理 | 源码 |
| ---- | ---- | ---- |
| 限流期间的 PELT | 受限等待不应被算成负荷自然衰减，所以冻结 PELT 时钟：空队列在 `tg_throttle_down()` 冻结，非空队列在[最后一个实体出队](../../linux/kernel/sched/fair.c#L5615)时冻结；解除时把冻结时长累进 `throttled_clock_pelt_time` | [`cfs_rq_clock_pelt()`](../../linux/kernel/sched/pelt.h#L174)、[`tg_unthrottle_up()`](../../linux/kernel/sched/fair.c#L6056) |
| limbo 任务换组、改亲和性 | 出队时从旧 limbo 链表摘下；带 `p->throttled` 入队且目标层级仍受限时，直接挂到新队列的 limbo 链表 | [`dequeue_throttled_task()`](../../linux/kernel/sched/fair.c#L5975)、[`enqueue_throttled_task()`](../../linux/kernel/sched/fair.c#L6035) |
| 负载均衡 | 不把任务迁到其所在层级在目标 CPU 上已受限的地方 | [`can_migrate_task()`](../../linux/kernel/sched/fair.c#L9672) |
| 新任务 | 初始化限流 work 与链表节点 | [`init_cfs_throttle_work()`](../../linux/kernel/sched/fair.c#L5958)，由 [`__sched_fork()`](../../linux/kernel/sched/core.c#L4476) 调用 |
| 新组上线 | 复制同 CPU 父队列的 `throttle_count`，立即继承祖先限流 | [`sync_throttle()`](../../linux/kernel/sched/fair.c#L6584) |
| CPU 下线 / 上线 | 下线时关闭 `runtime_enabled` 并解除限流；上线时按 quota 重新打开 | [`unthrottle_offline_cfs_rqs()`](../../linux/kernel/sched/fair.c#L6797)、[`update_runtime_enabled()`](../../linux/kernel/sched/fair.c#L6778) |
| 组销毁 | 取消两个 hrtimer，处理未完成的异步解限流 | [`destroy_cfs_bandwidth()`](../../linux/kernel/sched/fair.c#L6736) |
| 周期定时器停用 | 一个周期内无人领取且无受限队列，下个周期补充后停用；再次领取时重启 | [`do_sched_cfs_period_timer()`](../../linux/kernel/sched/fair.c#L6397)、[`period_active` 清除](../../linux/kernel/sched/fair.c#L6688) |
| 定时器过载 | 一次回调连续处理超过 3 个到期点时，尝试把 `period / quota / burst` 一起乘 2（新 period 须小于 1 s），读回的 `cpu.max` 也会变 | [`sched_cfs_period_timer()`](../../linux/kernel/sched/fair.c#L6657) |

**与 nohz_full 的关系。** 本配置编译开启了 `NO_HZ_FULL`，但只有启动参数 [`nohz_full=`](../../linux/kernel/sched/isolation.c#L198) 列出的 CPU 才会停 tick。在这些 CPU 上，若唯一的公平任务受带宽约束（本组 `runtime_enabled` 或 `hierarchical_quota` 非无限），调度器会保留 tick，使带宽记账能按时进行，见 [`sched_fair_update_stop_tick()`](../../linux/kernel/sched/fair.c#L6858) 和 [`sched_can_stop_tick()`](../../linux/kernel/sched/core.c#L1350)。

## 6. cpuset：限定位置与建立独占分区

cpu 控制器回答“在一颗 CPU 上怎样分时间”，cpuset 回答“任务可以在哪些 CPU 上运行”。它要满足两类需求：**把本组任务限制在一组 CPU 上**，以及**让一组 CPU 只给本组使用**。本章只讲 CPU 部分，不展开 `cpuset.mems` 的内存节点约束和 `SCHED_DEADLINE` 的带宽核算。

与本章有关的配置条件：

- 未开启 `CPUSETS_V1`，[`cpuset_v2()`](../../linux/kernel/cgroup/cpuset.c#L335) 恒为真，源码中的 v1 分支不会执行，下文不再提及。
- `CPUMASK_OFFSTACK` 不在 `.config` 中。x86 上它只能由 `MAXSMP` 选中，或在开启 `DEBUG_PER_CPU_MAPS` 后手动打开，见 [lib/Kconfig](../../linux/lib/Kconfig#L397) 与 [x86 Kconfig](../../linux/arch/x86/Kconfig#L989)，而 [`MAXSMP`](../../linux/.config#L427) 与 [`DEBUG_PER_CPU_MAPS`](../../linux/.config#L10598) 均未开启。因此 `cpumask_var_t` 是单元素数组，见 [类型定义](../../linux/include/linux/cpumask_types.h#L60)，下文的掩码都内嵌在所属结构体中。
- 开启了 [`HOTPLUG_CPU`](../../linux/.config#L528)，6.8 节的热插拔路径可以在运行时触发。

### 6.1 问题与概览：请求、结果与三处落实

**用户写请求，内核算结果。** cpuset 在每个启用了它的非根组上提供三个可写文件：

| 文件 | 用户表达的意思 |
| ---- | -------------- |
| `cpuset.cpus` | 本组任务希望使用哪些 CPU |
| `cpuset.cpus.exclusive` | 本组希望独占哪些 CPU，可以不写 |
| `cpuset.cpus.partition` | `member`、`root` 或 `isolated`：是否把本组变成分区根 |

请求不一定能满足：父组没有的 CPU 给不了，已被兄弟独占的 CPU 拿不到，不在 active 状态的 CPU 用不上。**active CPU** 指已经可以接收普通任务的 CPU，热插拔过程中它与 online 不完全相同，见 [`top_cpuset` 前的说明](../../linux/kernel/cgroup/cpuset.c#L196)。内核据此为每个组算出**有效集合**，读 `cpuset.cpus.effective` 看到的就是它。

**两种约束强度。** 只写 `cpuset.cpus` 得到的是**放置约束**：本组任务被限制在有效集合内，但这些 CPU 仍对其他组开放。把 `cpuset.cpus.partition` 写成 `root` 或 `isolated` 才建立**分区**（partition）：分区从父分区的有效集合中**划出**一组独占 CPU，分区外的普通任务从此不能再使用它们。8 颗 CPU 的机器上：

| 组 A 的配置 | A 中任务可用 | 根组和其他组的任务可用 |
| ----------- | ------------ | ---------------------- |
| `cpuset.cpus = 4-7`，保持 `member` | 4-7 | 0-7 |
| 再写 `cpuset.cpus.partition = root` | 4-7 | 0-3 |

**结果落实到三处。**

| 落实到 | 回答的问题 | 章节 |
| ------ | ---------- | ---- |
| 每个任务的 `cpus_mask` | 这个任务允许在哪些 CPU 上运行？ | 6.6 |
| 调度域 `sched_domain` 与 `root_domain` | 调度器在哪些 CPU 之间做负载均衡？ | 6.7 |
| 全局隔离集合 `isolated_cpus` | 哪些 CPU 不再承担 unbound 工作队列等内核工作？ | 6.7 |

下面的概念图从左到右表示“事件 → 重算 → 落实”，箭头表示数据流向：

```mermaid
flowchart LR
    W["写 cpuset.cpus<br/>cpuset.cpus.exclusive<br/>cpuset.cpus.partition"] --> CALC
    HP["CPU 热插拔"] --> CALC
    CALC["重算有效集合与分区状态"]
    CALC --> AFF["任务 cpus_mask"]
    CALC --> SD["调度域与 root_domain"]
    CALC --> ISO["isolated_cpus"]
    MV["任务迁入、fork"] -->|"读取有效集合"| AFF
    SA["sched_setaffinity()"] -->|"与有效集合求交"| AFF
```

配置写入和热插拔会改变有效集合，因此要遍历受影响的组及其中的任务；任务迁入、fork 和用户设置亲和性只读取已经算好的结果。唤醒选核和负载均衡只看任务的 `cpus_ptr` 和 CPU 的调度域，不访问 cpuset；只有任务掩码中找不到可用 CPU 时，兜底的 [`select_fallback_rq()`](../../linux/kernel/sched/core.c#L3508) 才回头查询 cpuset。

### 6.2 核心数据结构：每组四个掩码与全局账本

**`struct cpuset`。** 摘自 [`struct cpuset`](../../linux/kernel/cgroup/cpuset-internal.h#L74)，只保留与 CPU 有关的成员，中文注释为阅读说明：

```c
struct cpuset {
    struct cgroup_subsys_state css;  /* 内嵌 css，css_cs() 由它找回外层对象 */
    unsigned long flags;             /* CS_CPU_EXCLUSIVE、CS_SCHED_LOAD_BALANCE 等 */
    cpumask_var_t cpus_allowed;      /* 请求：cpuset.cpus */
    /* 省略内存节点 */
    cpumask_var_t effective_cpus;    /* 结果：本组任务可用的 CPU */
    /* 省略 */
    cpumask_var_t effective_xcpus;   /* 结果：本组的有效独占集合 */
    cpumask_var_t exclusive_cpus;    /* 请求：cpuset.cpus.exclusive */
    /* 省略 */
    int attach_in_progress;          /* 正在迁入本组的批次数 */
    /* 省略 */
    int nr_subparts;                 /* 有效本地子分区的个数 */
    int partition_root_state;        /* 分区状态 */
    /* 省略 DEADLINE 统计 */
    enum prs_errcode prs_err;        /* 分区无效的原因 */
    struct cgroup_file partition_file; /* 分区状态变化时通知用户态 */
    struct list_head remote_sibling; /* 远端分区挂入全局 remote_children */
    /* 省略 */
};
```

字段位置见 [四个掩码](../../linux/kernel/cgroup/cpuset-internal.h#L99)、[`attach_in_progress`](../../linux/kernel/cgroup/cpuset-internal.h#L149)、[分区成员](../../linux/kernel/cgroup/cpuset-internal.h#L158) 和 [`prs_err` 至 `remote_sibling`](../../linux/kernel/cgroup/cpuset-internal.h#L172)。四个掩码两两成对：

| 字段 | 接口文件 | 谁写 | 含义 |
| ---- | -------- | ---- | ---- |
| `cpus_allowed` | `cpuset.cpus` | 用户 | 放置请求；为空表示沿用父组 |
| `exclusive_cpus` | `cpuset.cpus.exclusive` | 用户 | 显式的独占请求；为空时由 `cpus_allowed` 代替。代替后的结果本章称为**独占请求**，由 [`user_xcpus()`](../../linux/kernel/cgroup/cpuset.c#L573) 给出 |
| `effective_cpus` | `cpuset.cpus.effective` | 内核 | 本组任务实际可用的 CPU，正常只含 active CPU |
| `effective_xcpus` | `cpuset.cpus.exclusive.effective` | 内核 | 有效独占集合：分区实际拥有的、或可以继续向下传递的独占 CPU，可含离线 CPU |

文件与字段的对应见 [`cpuset_common_seq_show()`](../../linux/kernel/cgroup/cpuset.c#L3466)。热插拔只改变结果，不改变请求，见 [v2 行为说明](../../linux/kernel/cgroup/cpuset.c#L341)。按 [字段注释](../../linux/kernel/cgroup/cpuset-internal.h#L116)，`effective_xcpus` 只在显式写了 `cpuset.cpus.exclusive`、或本组成为本地分区根时才设置；远端分区则在 [启用时](../../linux/kernel/cgroup/cpuset.c#L1617) 直接写入。

**分区状态。** [`partition_root_state`](../../linux/kernel/cgroup/cpuset.c#L109) 的取值：

| 值 | 宏 | 读 `cpuset.cpus.partition` 显示 | 从父分区划出 CPU | 本组 CPU 的负载均衡 |
| -- | -- | ------------------------------ | ---------------- | ------------------ |
| 0 | `PRS_MEMBER` | `member` | 否 | 随所在分区 |
| 1 | `PRS_ROOT` | `root` | 是 | 在本分区内部均衡 |
| 2 | `PRS_ISOLATED` | `isolated` | 是 | 不均衡 |
| −1 | `PRS_INVALID_ROOT` | `root invalid (原因)` | 否，按 member 计算有效集合 | 随父组 |
| −2 | `PRS_INVALID_ISOLATED` | `isolated invalid (原因)` | 同上 | 同上 |

无效状态是有效状态取负，见 [`make_partition_invalid()`](../../linux/kernel/cgroup/cpuset.c#L176)。取负保留了用户想要的分区类型：条件恢复时把符号翻回即可，见 [有效性翻转](../../linux/kernel/cgroup/cpuset.c#L2052)。显示逻辑见 [`cpuset_partition_show()`](../../linux/kernel/cgroup/cpuset.c#L3499)。这里的“分区根”不是目录树的根：根组的 `top_cpuset` 内部恒为 `PRS_ROOT`，但没有 `cpuset.cpus.partition` 文件。

**两个标志位。** `CS_CPU_EXCLUSIVE` 在建立分区时置位，分区失效或撤销时清除，见 [`update_partition_exclusive_flag()`](../../linux/kernel/cgroup/cpuset.c#L1252)；置位后，兄弟组的独占请求不得与本组重叠，检查见 [`cpus_excl_conflict()`](../../linux/kernel/cgroup/cpuset.c#L613)。`CS_SCHED_LOAD_BALANCE` 在建组时默认置位，见 [`cpuset_css_alloc()`](../../linux/kernel/cgroup/cpuset.c#L3654)；有效 isolated 分区清除它，其他有效分区置位，非分区组跟随父组，见 [`update_partition_sd_lb()`](../../linux/kernel/cgroup/cpuset.c#L1273)。v2 生成调度域时只看 `partition_root_state`（6.7 节），这个位用于让新组和非分区后代继承父组的均衡属性，见 [上线时继承](../../linux/kernel/cgroup/cpuset.c#L3681) 与 [重算时继承](../../linux/kernel/cgroup/cpuset.c#L2374)。

**全局状态。** 有些信息不属于任何一个组：

| 对象 | 含义 | 依据 |
| ---- | ---- | ---- |
| `top_cpuset` | 根组的 cpuset，静态分配，状态恒为 `PRS_ROOT`，带 `CS_CPU_EXCLUSIVE` 与 `CS_SCHED_LOAD_BALANCE` | [定义](../../linux/kernel/cgroup/cpuset.c#L210) |
| `subpartitions_cpus` | 根分区划给子分区（本地与远端）的全部独占 CPU | [定义](../../linux/kernel/cgroup/cpuset.c#L73) |
| `isolated_cpus` | 处在 isolated 分区中的独占 CPU，另含启动时隔离的 CPU | [定义](../../linux/kernel/cgroup/cpuset.c#L79)、[启动初始化](../../linux/kernel/cgroup/cpuset.c#L3932) |
| `remote_children` | 远端分区链表，节点是各组的 `remote_sibling` | [定义](../../linux/kernel/cgroup/cpuset.c#L90) |
| `force_sd_rebuild` | 本次操作结束前是否要重建调度域 | [定义与说明](../../linux/kernel/cgroup/cpuset.c#L93) |

根组的掩码由内核维护：`cpus_allowed` 与 `effective_xcpus` 是全部 possible CPU（本机可能出现的所有 CPU，含尚未上线的），见 [`cpuset_bind()`](../../linux/kernel/cgroup/cpuset.c#L3779)；`effective_cpus` 保持为“active CPU − `subpartitions_cpus`”，建立和撤销分区时由 6.4 节的 `partition_xcpus_add()/del()` 增删，热插拔时整体重算，见 [热插拔重算](../../linux/kernel/cgroup/cpuset.c#L4125)。根组没有可写的 cpuset 文件（`CFTYPE_NOT_ON_ROOT`），写入函数也对它直接返回 `-EACCES`，见 [`cpuset_write_resmask()`](../../linux/kernel/cgroup/cpuset.c#L3410)。

**对象关系。** 下图中实线表示指针或链表连接，虚线表示由 css 换算外层对象或按值复制：

```mermaid
flowchart LR
    TASK["task_struct"] -->|"cgroups"| CSET["css_set"]
    CSET -->|"subsys[cpuset_cgrp_id]"| CSS["cpuset.css"]
    CSS -.->|"css_cs()"| CS["struct cpuset"]
    CS -->|"css.parent，经 parent_cs()"| PCS["父组 struct cpuset"]
    CS -->|"remote_sibling，仅远端分区"| RC["全局 remote_children"]
    CS -.->|"有效集合按值写入"| MASK["task_struct.cpus_mask"]
```

查找路径见 [`task_cs()`](../../linux/kernel/cgroup/cpuset-internal.h#L191) 与 [`parent_cs()`](../../linux/kernel/cgroup/cpuset-internal.h#L196)。要注意最后一条边：任务从 cpuset 得到的是**值的副本**，`cpus_mask` 内嵌在 `task_struct` 中，见 [`cpus_mask`](../../linux/include/linux/sched.h#L920)，并不指向 cpuset 的掩码。从结构上看，代价是有效集合一变就要遍历组内任务逐个改写；换来的是唤醒和选核只读任务自己的字段，不必经 `css_set` 找 cpuset，也不必获取 cpuset 的锁。

**锁。** [源码注释](../../linux/kernel/cgroup/cpuset.c#L218) 规定了两级锁协议，修改路径还要持 CPU 热插拔读锁：

| 锁 | 类型 | 谁持有 | 作用 |
| -- | ---- | ------ | ---- |
| `cpus_read_lock()` | 热插拔锁的读端 | 所有修改路径 | 修改过程中 CPU 不会上下线 |
| `cpuset_mutex` | 互斥锁 | 修改路径，以及 attach、fork | 串行化修改；持有时可以睡眠和分配内存 |
| `callback_lock` | 关中断自旋锁 | 修改者真正写入掩码与状态的瞬间；只读者 | 让只读者看到一致的掩码 |

修改者先用 [`cpuset_full_lock()`](../../linux/kernel/cgroup/cpuset.c#L276) 取前两把，做完检查和内存分配，再短暂持 `callback_lock` 写入。只读者只取 `callback_lock`，例如读文件的 [`cpuset_common_seq_show()`](../../linux/kernel/cgroup/cpuset.c#L3464) 和供 `sched_setaffinity()` 使用的 [`cpuset_cpus_allowed()`](../../linux/kernel/cgroup/cpuset.c#L4244)。改写任务亲和性要取任务的 `pi_lock` 与 rq 锁，还可能等待迁移完成（6.6 节），所以 cpuset 总是在**释放 `callback_lock` 之后**才更新任务。

**不变量。** 后文的算法都在维护下面这些关系：

| 不变量 | 依据 |
| ------ | ---- |
| member 与无效分区：`effective_cpus = cpus_allowed ∩ 父组 effective_cpus`，结果为空时取父组 `effective_cpus` | [`compute_effective_cpumask()`](../../linux/kernel/cgroup/cpuset.c#L1227)、[空集继承](../../linux/kernel/cgroup/cpuset.c#L2284)、[`reset_partition_data()`](../../linux/kernel/cgroup/cpuset.c#L1330) |
| 有效分区：`effective_cpus = (独占候选 ∩ active) − 各有效子分区的 effective_xcpus`，其中独占候选 = 独占请求 ∩ 父组 `effective_xcpus`（6.3 节） | [`compute_partition_effective_cpumask()`](../../linux/kernel/cgroup/cpuset.c#L2158) |
| 有效本地分区的独占 CPU 已从父分区的 `effective_cpus` 中删除 | [`partition_xcpus_add()`](../../linux/kernel/cgroup/cpuset.c#L1377) |
| 同一父组下，有效分区的独占请求与每个兄弟的独占请求互不重叠 | [`cpus_excl_conflict()`](../../linux/kernel/cgroup/cpuset.c#L616) |
| 有任务的父分区不能被划空；有任务的分区必须含 active CPU | [`tasks_nocpu_error()`](../../linux/kernel/cgroup/cpuset.c#L1303) |
| 根组 `effective_cpus` = active CPU − `subpartitions_cpus` | 见上文“全局状态” |

### 6.3 算法一：自上而下重算有效集合

**目标与输入输出。** 某个组的请求或分区状态变化后，要重新算出它的子树中每个组的 `effective_cpus`，并更新组内任务。输入是各组的请求、分区状态和 `cpu_active_mask`，输出是新的有效集合和任务亲和性。实现是 [`update_cpumasks_hier()`](../../linux/kernel/cgroup/cpuset.c#L2227)。

**先父后子。** 子组的结果依赖父组的 `effective_cpus`，所以遍历采用先序 [`cpuset_for_each_descendant_pre`](../../linux/kernel/cgroup/cpuset-internal.h#L266)：访问一个组时，它的父组已经算完。每个组按状态选择公式，见 [公式选择](../../linux/kernel/cgroup/cpuset.c#L2266)。下面是简化逻辑：

```text
（伪代码，省略远端分区、分区失效与恢复、锁）
for cp in 先序遍历(起点组的子树):
    if cp 是有效分区 且 父组也是有效分区:
        new = compute_excpus(cp) ∩ active            # 分区公式
        new -= cp 下各有效子分区的 effective_xcpus
    else:
        new = cp.cpus_allowed ∩ 父组.effective_cpus  # member 公式
        if new 为空:
            new = 父组.effective_cpus                # 空请求或无交集时继承
    if cp 是 member 且 new 未变 且 未要求强制 且 均衡属性与父组相同:
        跳过 cp 的整棵子树                            # 后代的结果不会变
        continue
    持 callback_lock：写入 cp.effective_cpus，按需更新 cp.effective_xcpus
    cpuset_update_tasks_cpumask(cp)                  # 已释放 callback_lock
```

源码见 [空集继承](../../linux/kernel/cgroup/cpuset.c#L2284)、[跳过子树](../../linux/kernel/cgroup/cpuset.c#L2293)、[写回](../../linux/kernel/cgroup/cpuset.c#L2350) 与 [更新任务](../../linux/kernel/cgroup/cpuset.c#L2372)。“跳过子树”只对 member 生效：有效或无效分区总要继续检查，因为它们可能需要与父组交换 CPU，或改变有效性。

**独占候选。** 分区公式中的 [`compute_excpus()`](../../linux/kernel/cgroup/cpuset.c#L1528)：

```c
static int compute_excpus(struct cpuset *cs, struct cpumask *excpus)
{
    struct cpuset *parent = parent_cs(cs);

    cpumask_and(excpus, user_xcpus(cs), parent->effective_xcpus);

    if (!cpumask_empty(cs->exclusive_cpus))
        return 0;

    return rm_siblings_excl_cpus(parent, cs, excpus);
}
```

独占候选 = 本组独占请求 ∩ 父组有效独占集合。没有显式写 `cpuset.cpus.exclusive` 时，还要去掉兄弟已申请或已占用的独占 CPU，见 [`rm_siblings_excl_cpus()`](../../linux/kernel/cgroup/cpuset.c#L1486)，返回值是发生重叠的兄弟个数。根组的有效独占集合是全部 possible CPU，所以根组的直接子组可以申请任意 possible CPU。

**数值例子。** 8 颗 CPU 全部 active。中间一列是所有组都为 member 时的结果，右列是 A 成为有效 root 分区之后的结果：

```text
组               请求            全为 member         A 成为有效 root 分区后
根组             —               0-7                 0-3
├── A            cpus = 4-7      4-7                 4-7（分区公式）
│   └── A1       cpus = 2-5      4-5（2-5 ∩ 4-7）    4-5
└── B            未写            0-7（继承根组）      0-3（继承根组）
    └── B1       cpus = 6-7      6-7                 0-3（6-7 ∩ 0-3 为空，继承 B）
```

B1 体现了 member 公式最容易误解的一点：**请求落空不会报错，而是退回父组的集合**。`cpuset.cpus` 只是请求，`cpuset.cpus.effective` 才是任务实际受到的约束。

**写入时的硬性检查。** 有两种请求会被直接拒绝：CPU 列表不是 possible CPU 的子集时返回 `-EINVAL`，见 [`parse_cpuset_cpulist()`](../../linux/kernel/cgroup/cpuset.c#L2461)；本组或其后代有任务、或正有任务迁入时，把非空的 `cpuset.cpus` 改为空返回 `-ENOSPC`，见 [`validate_change()`](../../linux/kernel/cgroup/cpuset.c#L679) 与 [`cpuset_is_populated()`](../../linux/kernel/cgroup/cpuset.c#L355)。

**牵连兄弟。** 分区的建立、撤销和调整会改变**父组**的 `effective_cpus`，父组的其他子组随之要重算。[`update_sibling_cpumasks()`](../../linux/kernel/cgroup/cpuset.c#L2413) 对非分区兄弟先按 member 公式试算，结果不变就跳过；远端分区兄弟不依赖父组集合，直接跳过；其余兄弟以自己为起点调用 `update_cpumasks_hier()`。上例中 A 成为分区后，B 和 B1 就是经这条路径从 0-7、6-7 变成 0-3 的。

### 6.4 算法二：分区的建立、失效与撤销

**核心操作：划出与归还。** 分区的各种状态迁移最终都落到一对函数上：建立分区时把独占 CPU 从父分区的 `effective_cpus` 中删去，撤销或失效时再加回去。摘自 [`partition_xcpus_add()`](../../linux/kernel/cgroup/cpuset.c#L1358)，中文注释为阅读说明：

```c
static bool partition_xcpus_add(int new_prs, struct cpuset *parent,
                                struct cpumask *xcpus)
{
    bool isolcpus_updated;

    WARN_ON_ONCE(new_prs < 0);
    lockdep_assert_held(&callback_lock);
    if (!parent)
        parent = &top_cpuset;           /* 远端分区：直接向根分区要 CPU */

    if (parent == &top_cpuset)           /* 根分区划出的 CPU 记入全局集合 */
        cpumask_or(subpartitions_cpus, subpartitions_cpus, xcpus);

    /* 子分区与父分区的隔离属性不同时，才调整 isolated_cpus */
    isolcpus_updated = (new_prs != parent->partition_root_state);
    if (isolcpus_updated)
        isolated_cpus_update(parent->partition_root_state, new_prs,
                             xcpus);

    cpumask_andnot(parent->effective_cpus, parent->effective_cpus, xcpus);
    return isolcpus_updated;
}
```

[`partition_xcpus_del()`](../../linux/kernel/cgroup/cpuset.c#L1390) 做相反的操作，但只把其中的 active CPU 加回父组，见 [与 active 求交](../../linux/kernel/cgroup/cpuset.c#L1408)。

这里有两处容易忽略：`subpartitions_cpus` 只记录**根分区**划出的 CPU，分区内部再嵌套的子分区不改变它；`isolated_cpus` 按“子分区与父分区状态是否不同”维护，见 [`isolated_cpus_update()`](../../linux/kernel/cgroup/cpuset.c#L1340)。例如 A 是根组下 CPU 4-7 的 root 分区，A 的子组 A2 申请 6-7 建 isolated 分区：A 的 `effective_cpus` 变为 4-5，`nr_subparts` 加一，6-7 进入 `isolated_cpus`，而 `subpartitions_cpus` 仍是 4-7。

**状态机。** 下图是 `partition_root_state` 的迁移。用户写入触发的是从 `member` 出发、两种有效状态互换和回到 `member` 这几条边；有效与无效之间的往返大多由其他事件触发：

```mermaid
stateDiagram-v2
    state "member" as M
    state "有效分区（root / isolated）" as V
    state "无效分区（状态取负）" as I
    [*] --> M
    M --> V: 写 root / isolated，检查通过
    M --> I: 写 root / isolated，检查失败
    V --> V: root 与 isolated 互换
    V --> I: 兄弟冲突、父组失效、CPU 不足、热插拔
    I --> V: 被重算时条件已恢复
    V --> M: 写 member 或删除目录
    I --> M: 写 member
```

**建立：检查什么。** [`update_prstate()`](../../linux/kernel/cgroup/cpuset.c#L3050) 先置 `CS_CPU_EXCLUSIVE`，再按父组状态选路径：父组是有效分区时建立**本地分区**，主要检查在 [`update_parent_effective_cpumask()`](../../linux/kernel/cgroup/cpuset.c#L1811) 的 `partcmd_enable` / `partcmd_enablei` 分支中进行；否则尝试**远端分区**（本节末尾）。本地分区的检查依次为：

| 检查 | 失败码 | 读回时括号中的原因 | 依据 |
| ---- | ------ | ------------------ | ---- |
| 兄弟的独占请求与本组重叠 | `PERR_NOTEXCL` | Cpu list in cpuset.cpus not exclusive | [置位与检查](../../linux/kernel/cgroup/cpuset.c#L3069) |
| `cpuset.cpus` 与 `cpuset.cpus.exclusive` 都为空 | `PERR_CPUSEMPTY` | cpuset.cpus and cpuset.cpus.exclusive are empty | [检查](../../linux/kernel/cgroup/cpuset.c#L3077) |
| 父组是根组，而本组 exclusive 请求与根分区已划出的 CPU 重叠 | `PERR_REMOTE` | Have remote partition underneath | [检查](../../linux/kernel/cgroup/cpuset.c#L3082) |
| 独占候选为空 | `PERR_INVCPUS` | Invalid cpu list in cpuset.cpus.exclusive | [检查](../../linux/kernel/cgroup/cpuset.c#L1878) |
| 候选含启动时隔离的 CPU，却要建 root | `PERR_HKEEPING` | partition config conflicts with housekeeping setup | [`prstate_housekeeping_conflict()`](../../linux/kernel/cgroup/cpuset.c#L1763) |
| 建 isolated 会用光 `nohz_full` 下的 housekeeping CPU | `PERR_HKEEPING` | 同上 | [`isolated_cpus_can_update()`](../../linux/kernel/cgroup/cpuset.c#L1425) |
| 划出后有任务的父分区没有 CPU，或有任务的本组没有 active CPU | `PERR_NOCPUS` | Parent unable to distribute cpu downstream | [`tasks_nocpu_error()`](../../linux/kernel/cgroup/cpuset.c#L1303) |

原因字符串见 [`perr_strings[]`](../../linux/kernel/cgroup/cpuset.c#L55)。表中的 **housekeeping CPU** 指仍承担普通内核工作的 CPU：启动参数 `isolcpus=` 默认把 CPU 移出 `HK_TYPE_DOMAIN` 类，`nohz_full=` 把 CPU 移出 `HK_TYPE_KERNEL_NOISE` 类，见 [`housekeeping_nohz_full_setup()`](../../linux/kernel/sched/isolation.c#L190) 与 6.7 节。第一项比较的是双方的 `user_xcpus()`：写过 `cpuset.cpus.exclusive` 就用它，否则用 `cpuset.cpus`。所以只要某个没写 `cpuset.cpus.exclusive` 的兄弟，其 `cpuset.cpus` 与本组重叠，本组就建不成分区；兄弟的两个请求都为空则不构成冲突。

**失败不等于写入失败。** 检查失败时，[`update_prstate()`](../../linux/kernel/cgroup/cpuset.c#L3133) 把目标状态取负、清除 `CS_CPU_EXCLUSIVE`、记下 `prs_err`，然后照常[返回 0](../../linux/kernel/cgroup/cpuset.c#L3167)。写系统调用因此返回成功，只有读回 `cpuset.cpus.partition` 才能看到 `root invalid (...)`。[`cpuset_partition_write()`](../../linux/kernel/cgroup/cpuset.c#L3530) 只在写入的值不认识（`-EINVAL`）、组已下线（`-ENODEV`）或分配临时掩码失败（`-ENOMEM`）时让写入失败。

**建立：改了什么。** 检查通过后，`update_parent_effective_cpumask()` 在 `callback_lock` 内写入新状态、调用 `partition_xcpus_add()` 从父分区划出候选、给父组的 `nr_subparts` 加一，见 [提交](../../linux/kernel/cgroup/cpuset.c#L2098)；释放锁后更新 unbound 工作队列，再让父组的任务和兄弟子树按新的父组集合重算，见 [更新父组与兄弟](../../linux/kernel/cgroup/cpuset.c#L2125)。回到 `update_prstate()` 后，用 `update_cpumasks_hier()` 按分区公式算出本组及后代的集合，见 [调用处](../../linux/kernel/cgroup/cpuset.c#L3153)，最后设置均衡标志、要求重建调度域并通知用户态。6.5 节按时间顺序画出这条路径。

**root 与 isolated 互换。** 两种有效状态之间切换不移动 CPU，只改 `isolated_cpus` 和均衡标志，见 [切换分支](../../linux/kernel/cgroup/cpuset.c#L3107)。

**撤销。** 写 `member` 时，本地分区走 `partcmd_disable` 分支，把有效独占集合还给父分区，见 [归还分支](../../linux/kernel/cgroup/cpuset.c#L1909)；远端分区走 [`remote_partition_disable()`](../../linux/kernel/cgroup/cpuset.c#L1640)。删除目录时，[`cpuset_css_killed()`](../../linux/kernel/cgroup/cpuset.c#L3756) 对有效分区做同样的事。撤销后本组按 member 公式重算；它下面的有效子分区因为“父组不再是分区”而失效，原因是 `PERR_NOTPART`，见 [子分区失效](../../linux/kernel/cgroup/cpuset.c#L2315)。

**被动失效。** 分区建立后，以下事件可以让它失效。失效时 CPU 还给父分区，用户写入的请求保留：

| 事件 | 结果 | 依据 |
| ---- | ---- | ---- |
| 兄弟写 `cpuset.cpus`，与有效分区重叠 | 兄弟的写入**成功**，冲突的分区失效，读回时不带原因 | [`cpus_allowed_validate_change()`](../../linux/kernel/cgroup/cpuset.c#L2504) |
| 兄弟写 `cpuset.cpus.exclusive`，与有效分区重叠 | 兄弟的写入失败，返回 `-EINVAL`，分区不受影响 | [`update_exclusive_cpumask()`](../../linux/kernel/cgroup/cpuset.c#L2662) |
| 本分区改 `cpuset.cpus` 或 `cpuset.cpus.exclusive` | 仍满足条件就按新集合与父分区交换 CPU，否则失效 | [`partition_cpus_change()`](../../linux/kernel/cgroup/cpuset.c#L2550) |
| 父组撤销分区或自身失效 | 子分区失效 | [子分区失效](../../linux/kernel/cgroup/cpuset.c#L2315) |
| 热插拔使分区失去可用 CPU | 见 6.8 节 | [`cpuset_hotplug_update_tasks()`](../../linux/kernel/cgroup/cpuset.c#L3978) |

第一行是 v2 特有的宽松处理：[`validate_change()`](../../linux/kernel/cgroup/cpuset.c#L715) 发现独占冲突返回 `-EINVAL` 后，调用者把它改成“让冲突分区失效、写入继续”。失效走的 `partcmd_invalidate` 分支不写 `prs_err`，见 [失效分支](../../linux/kernel/cgroup/cpuset.c#L1836)，所以读回的只是 `root invalid` 或 `isolated invalid`。

**自动恢复。** 无效分区仍记着用户想要的类型。它被重算时——例如父组重新成为有效分区后 `update_cpumasks_hier()` 遍历到它，或热插拔让 CPU 回来——若父组是有效分区、本组独占请求非空且是父组有效独占集合的子集、与每个兄弟的独占请求互不重叠，并且划出后有任务的父分区和本组都还有可用 CPU，`partcmd_update` 分支就把它转回有效状态，见 [恢复条件](../../linux/kernel/cgroup/cpuset.c#L2020) 与 [有效性翻转](../../linux/kernel/cgroup/cpuset.c#L2065)。分区状态每次变化都由 [`notify_partition_change()`](../../linux/kernel/cgroup/cpuset.c#L185) 通知用户态，管理程序可以 poll 或 inotify `cpuset.cpus.partition`，不必轮询读取。

**远端分区。** 父组不是有效分区时，[`update_prstate()`](../../linux/kernel/cgroup/cpuset.c#L3095) 改走 [`remote_partition_enable()`](../../linux/kernel/cgroup/cpuset.c#L1584)：越过中间各层，直接向根分区要 CPU。条件有三条：

1. 调用者具有 `CAP_SYS_ADMIN`，否则 `PERR_ACCESS`，见 [权限检查](../../linux/kernel/cgroup/cpuset.c#L1592)。
2. 从根组到父组的每一层都写了 `cpuset.cpus.exclusive`。独占候选按“本组独占请求 ∩ 父组有效独占集合”计算，而 member 的有效独占集合只来自它自己写入的 exclusive 请求，见 [`compute_trialcs_excpus()`](../../linux/kernel/cgroup/cpuset.c#L1554)；中间有一层没写，候选就是空集。
3. 候选至少含一颗 active CPU，且不能拿光根组的有效集合，否则 `PERR_INVCPUS`，见 [检查](../../linux/kernel/cgroup/cpuset.c#L1605)；建 isolated 时同样要通过 `isolated_cpus_can_update()`。

```text
根组                       effective = 0-3      ← 4-7 记入 subpartitions_cpus
└── A：member，cpus = 0-7，exclusive = 4-7
    effective = 0-3                             ← A 中任务的可用范围
    exclusive.effective = 4-7                   ← 只用于向下传递候选
    └── B：root（远端），cpus = 4-7
        effective = 4-7
```

B 挂入 `remote_children`，并以 `parent == NULL` 调用 `partition_xcpus_add()` 从根组划走 4-7，见 [划出与入链](../../linux/kernel/cgroup/cpuset.c#L1614)；随后根组任务和根组下的 member 子树按新集合重算，A 因此变成 0-3，见 [传播](../../linux/kernel/cgroup/cpuset.c#L1623)。B 的有效集合走分区公式，不与 A 的 `effective_cpus` 求交，见 [公式选择](../../linux/kernel/cgroup/cpuset.c#L2266)。

### 6.5 实现：一次写入的完整路径

**入口。** 三个可写文件注册在 [`dfl_files[]`](../../linux/kernel/cgroup/cpuset.c#L3559)：`cpuset.cpus` 与 `cpuset.cpus.exclusive` 由 [`cpuset_write_resmask()`](../../linux/kernel/cgroup/cpuset.c#L3403) 处理，`cpuset.cpus.partition` 由 [`cpuset_partition_write()`](../../linux/kernel/cgroup/cpuset.c#L3530) 处理。二者都在 `cpuset_full_lock()` 内工作并先确认组仍在线，整个过程在写文件进程的上下文中同步完成。

**试算副本。** 改掩码时，`cpuset_write_resmask()` 先用 [`dup_or_alloc_cpuset()`](../../linux/kernel/cgroup/cpuset.c#L525) 复制出 `trialcs`，在副本上解析新请求、算独占候选、做检查，通过后才把结果写回真实对象。副本是整个结构体的拷贝，`css.parent` 仍指向真实的父组，所以能直接拿副本与父组和兄弟比较；但遍历兄弟必须以真实对象为游标，见 [`validate_change()` 的说明](../../linux/kernel/cgroup/cpuset.c#L648)。检查失败时只需丢弃副本，真实对象没有被改过。

**`cpuset.cpus` 的处理顺序。** 见 [`update_cpumask()`](../../linux/kernel/cgroup/cpuset.c#L2586)：

```text
（调用链摘要，省略错误返回）
update_cpumask(cs, trialcs, buf)
  → parse_cpuset_cpulist()          解析到副本，必须是 possible CPU 的子集
  → 与原值相同则直接返回
  → compute_trialcs_excpus()        在副本上重算有效独占集合
  → cpus_allowed_validate_change()  检查；兄弟独占冲突时让冲突分区失效，写入继续
  → partition_cpus_change()         本组是分区时：与父分区交换 CPU，或失效
  → 持 callback_lock：写回 cpus_allowed、effective_xcpus
  → update_cpumasks_hier(cs)        重算本组及子树，更新任务
  → update_partition_sd_lb()        本组是分区时：维护均衡标志
```

`cpuset.cpus.exclusive` 由 [`update_exclusive_cpumask()`](../../linux/kernel/cgroup/cpuset.c#L2646) 处理，步骤相同，区别是兄弟冲突直接返回 `-EINVAL`。

**调度域最后统一重建。** 一次写入可能多次改变分区和均衡标志。各函数只调用 [`cpuset_force_rebuild()`](../../linux/kernel/cgroup/cpuset.c#L3964) 置位 `force_sd_rebuild`，写入路径在释放锁之前统一检查一次，见 [掩码写入路径](../../linux/kernel/cgroup/cpuset.c#L3441) 与 [分区写入路径](../../linux/kernel/cgroup/cpuset.c#L3164)。

**完整时序。** 根组下有两个 member 子组：`system.slice` 的 `cpuset.cpus` 为 0-3，`pod-latency` 为 4-7（第 7 章沿用这个配置）。此时向 `pod-latency` 的 `cpuset.cpus.partition` 写入 `isolated`：

```mermaid
sequenceDiagram
    participant U as 写文件的进程
    participant C as cpuset 数据（持 cpuset_mutex）
    participant T as 受影响的任务
    participant S as 工作队列与调度域
    U->>C: cpuset_partition_write("isolated")
    Note over C: cpuset_full_lock()
    C->>C: update_prstate()：置 CS_CPU_EXCLUSIVE，检查兄弟
    C->>C: update_parent_effective_cpumask(partcmd_enablei)<br/>callback_lock 内：根组删去 4-7，<br/>4-7 记入 subpartitions_cpus 与 isolated_cpus
    C->>S: update_isolation_cpumasks()：unbound 工作队列排除 4-7
    C->>T: 根组任务改为 possible − 4-7，原先在 4-7 上的被迁走
    C->>T: update_cpumasks_hier(pod-latency)：子树仍为 4-7
    C->>C: update_partition_sd_lb()：清 CS_SCHED_LOAD_BALANCE，标记重建
    C->>S: rebuild_sched_domains_locked()：只为 0-3 建域
    Note over C: cpuset_full_unlock()
    C-->>U: 返回写入的字节数
```

`system.slice` 的有效集合本来就是 0-3，`update_sibling_cpumasks()` 试算后跳过它。写系统调用返回时，任务掩码、unbound 工作队列和调度域都已更新完毕。

### 6.6 落实一：任务亲和性

**任务侧的字段。** 摘自 [`task_struct`](../../linux/include/linux/sched.h#L917)：

```c
struct task_struct {
    /* 省略 */
    int nr_cpus_allowed;             /* cpus_mask 中的 CPU 数 */
    const cpumask_t *cpus_ptr;       /* 调度器读取允许集合的入口 */
    cpumask_t *user_cpus_ptr;        /* sched_setaffinity() 保存的用户原始请求 */
    cpumask_t cpus_mask;             /* 内嵌：实际生效的亲和性 */
    /* 省略 */
};
```

通常 `cpus_ptr == &p->cpus_mask`。`migrate_disable()` 期间发生切换时，[`migrate_disable_switch()`](../../linux/kernel/sched/core.c#L2383) 让它暂时指向当前 CPU 的单 CPU 掩码，[`___migrate_enable()`](../../linux/kernel/sched/core.c#L2402) 再接回。cpuset 只改 `cpus_mask`。

**cpuset 何时改写任务。**

| 时机 | 函数 | 新掩码 |
| ---- | ---- | ------ |
| 有效集合变化后遍历组内任务 | [`cpuset_update_tasks_cpumask()`](../../linux/kernel/cgroup/cpuset.c#L1192) | 根组：possible − `subpartitions_cpus`，跳过 `PF_NO_SETAFFINITY` 任务；其他组：possible ∩ `effective_cpus` |
| 写 `cgroup.procs` 迁入 | [`cpuset_attach_task()`](../../linux/kernel/cgroup/cpuset.c#L3297) | 非根组：[`guarantee_active_cpus()`](../../linux/kernel/cgroup/cpuset.c#L420) 取有效集合中的 active CPU；根组同上一行 |
| fork | [`cpuset_fork()`](../../linux/kernel/cgroup/cpuset.c#L3856) | 与父任务同在非根组：重新套用父任务的 `cpus_ptr`；用 `clone3()` 的 `CLONE_INTO_CGROUP` 直接进入别的组：同迁入 |

根组任务的掩码包含离线 CPU，热插拔时也不更新，见 [函数说明](../../linux/kernel/cgroup/cpuset.c#L1185)。迁入之前，[`cpuset_can_attach()`](../../linux/kernel/cgroup/cpuset.c#L3193) 要求目标组有效集合非空（否则 `-ENOSPC`），拒绝 `PF_NO_SETAFFINITY` 任务（[`task_can_attach()`](../../linux/kernel/sched/core.c#L8079)），并给目标组的 `attach_in_progress` 加一，防止迁移完成前有人把它的 `cpuset.cpus` 清空。在父组的 `cgroup.subtree_control` 中开启 cpuset 时，子组任务换到新的 css 上，但有效集合不变，[`cpuset_attach()` 会跳过逐任务更新](../../linux/kernel/cgroup/cpuset.c#L3335)。

**改写的过程：`set_cpus_allowed_ptr()`。** 上表三条路径最终都调用它：

```text
（调用链摘要，省略内核线程、migrate_disable 与 SCA_* 标志的分支）
set_cpus_allowed_ptr(p, new_mask)
  → __set_cpus_allowed_ptr()         持 p->pi_lock 与 rq 锁；
                                     有 user_cpus_ptr 且与 new_mask 有交集时，改用交集
  → __set_cpus_allowed_ptr_locked()  new_mask 须是 possible 的子集；
                                     dest_cpu = cpumask_any_and_distribute(active, new_mask)，
                                     找不到则返回 -EINVAL
  → __do_set_cpus_allowed()          出队，set_cpus_allowed_common() 写 cpus_mask 与
                                     nr_cpus_allowed，再入队
  → affine_move_task()               当前 CPU 仍被允许则结束；否则迁到 dest_cpu，
                                     正在运行的任务交给 stopper 线程（migration/N）迁移，
                                     调用者等待完成
```

依据见 [与用户请求求交](../../linux/kernel/sched/core.c#L3168)、[选目标 CPU](../../linux/kernel/sched/core.c#L3128)、[`set_cpus_allowed_common()`](../../linux/kernel/sched/core.c#L2694)、[`affine_move_task()`](../../linux/kernel/sched/core.c#L2916)、[等待完成](../../linux/kernel/sched/core.c#L3050) 与 [stopper 线程名](../../linux/kernel/stop_machine.c#L563)。由此得到两个结论：

- **同步生效。** 调用者可能睡眠等待迁移，这正是 cpuset 不在 `callback_lock` 内更新任务的原因。写入返回时，原先在被删 CPU 上运行或排队的任务已经迁走；睡眠中的任务只改了掩码，下次唤醒时由 [`select_task_rq()`](../../linux/kernel/sched/core.c#L3583) 在新掩码中选核。
- **一次性分散。** [`cpumask_any_and_distribute()`](../../linux/lib/cpumask.c#L134) 用每 CPU 游标轮转挑选。一批任务同时被赶出某些 CPU 时，会大致分散到新集合中，不必等负载均衡，见 [源码注释](../../linux/kernel/sched/core.c#L3128)。

**用户亲和性逃不出 cpuset。** [`__sched_setaffinity()`](../../linux/kernel/sched/syscalls.c#L1158) 把用户掩码与 [`cpuset_cpus_allowed()`](../../linux/kernel/cgroup/cpuset.c#L4239) 求交，交集为空时上面的选核失败，返回 `-EINVAL`；设置之后再读一次 cpuset，若期间 cpuset 已经变化，就改用 cpuset 的集合并返回 `-EINVAL`，见 [复查](../../linux/kernel/sched/syscalls.c#L1185)。根组任务可用的是 possible − `subpartitions_cpus`，所以分区建立后，根组中的进程用 `taskset` 绑到分区 CPU 会失败。

**保存的用户请求。** `sched_setaffinity()` 把用户的**原始**掩码保存为 `user_cpus_ptr`，见 [保存](../../linux/kernel/sched/syscalls.c#L1246)。之后 cpuset 再改亲和性时取“cpuset 集合 ∩ 用户请求”，交集为空才只用 cpuset 集合，并且不清除保存的请求：

| 操作后 | `user_cpus_ptr` | cpuset 有效集合 | 任务 `cpus_mask` |
| ------ | --------------- | --------------- | ---------------- |
| 用户请求只跑 CPU 5 | 5 | 4-7 | 5 |
| cpuset 改为 0-3 | 5 | 0-3 | 0-3 |
| cpuset 改回 4-7 | 5 | 4-7 | 5 |

**per-CPU 内核线程不受 cpuset 管理。** 内核线程被绑定到 CPU 时置上 `PF_NO_SETAFFINITY`，见 [`__kthread_bind_mask()`](../../linux/kernel/kthread.c#L563)；per-CPU 内核线程都带这个标志，[`kthread_set_per_cpu()`](../../linux/kernel/kthread.c#L641) 会对此做检查。这类线程不能迁入其他 cpuset，根组更新时又被跳过，所以 `ksoftirqd/4` 这样的线程会一直留在分区 CPU 上。

### 6.7 落实二：调度域与隔离

**调度域决定均衡边界。** 每颗 CPU 的 `rq->sd` 指向一串由小到大的 `sched_domain`（SMT、缓存、NUMA 等拓扑层），负载均衡以及 fork、唤醒时的选核都只在这串域覆盖的 CPU 之间进行。cpuset 不负责域的内部层次，见 [逐拓扑层建域](../../linux/kernel/sched/topology.c#L2502)；它只决定**把哪些 CPU 分到同一组**：给出若干互不重叠的 CPU 集合，调度器为每个集合建一套域。

**生成规则。** [`rebuild_sched_domains_locked()`](../../linux/kernel/cgroup/cpuset.c#L1097) 先确认没有与热插拔交错，即各有效分区的集合都在 active CPU 内，否则留给热插拔路径重建，见 [交错检查](../../linux/kernel/cgroup/cpuset.c#L1109)；然后调用 [`generate_sched_domains()`](../../linux/kernel/cgroup/cpuset.c#L831)。v2 的规则可以简化为：

```text
（简化逻辑）
若根组没有划出任何 CPU：
    只有一个集合 = 根组 effective_cpus ∩ housekeeping(HK_TYPE_DOMAIN)
否则先序遍历 cpuset 树：
    收集 partition_root_state == PRS_ROOT 且 effective_cpus 非空的组
    既不是有效分区、也没写 cpuset.cpus.exclusive 的组，跳过其子树
    若除根组外没有收集到任何组，退回上面的单集合
    根组的集合 = effective_cpus ∩ housekeeping(HK_TYPE_DOMAIN)，其他组直接用 effective_cpus
```

见 [单集合快路径](../../linux/kernel/cgroup/cpuset.c#L851)、[v2 收集规则](../../linux/kernel/cgroup/cpuset.c#L908)、[退回单集合](../../linux/kernel/cgroup/cpuset.c#L926) 和 [生成集合](../../linux/kernel/cgroup/cpuset.c#L976)。isolated 分区不被收集，它的 CPU 不在任何集合中；但遍历会继续进入它的子树，嵌套在里面的 root 分区照样得到自己的集合。

**调度器怎样应用。** [`partition_sched_domains_locked()`](../../linux/kernel/sched/topology.c#L2774) 比较新旧两组集合：旧集合不再出现就拆掉，其中的 CPU 先挂到空域和默认的 `def_root_domain`，见 [`detach_destroy_domains()`](../../linux/kernel/sched/topology.c#L2714)；新集合以前没有，就调用 [`build_sched_domains()`](../../linux/kernel/sched/topology.c#L2485) 建域，并为它[分配新的 `root_domain`](../../linux/kernel/sched/topology.c#L1561)、[挂到各 CPU 上](../../linux/kernel/sched/topology.c#L2612)；两边都有的集合保持不动。最终每颗 CPU 的状态是：

| CPU 所在 | `rq->sd` | `rq->rd` | 负载均衡范围 |
| -------- | -------- | -------- | ------------ |
| 根分区剩下的 CPU | 覆盖这些 CPU 的域 | 根分区独有的 `root_domain` | 根分区内 |
| 有效 root 分区 | 覆盖本分区的域 | 本分区独有的 `root_domain` | 本分区内 |
| 有效 isolated 分区、启动时移出 `HK_TYPE_DOMAIN` 的 CPU | `NULL` | `def_root_domain` | 无 |

启动时移出 `HK_TYPE_DOMAIN` 的 CPU 从 [运行队列初始化](../../linux/kernel/sched/core.c#L8791) 起就挂在 `def_root_domain` 上，[启动建域](../../linux/kernel/sched/topology.c#L2704) 和之后 cpuset 生成的集合都不包含它们。

`root_domain` 为这样一个与其他 CPU 完全隔开的“CPU 岛”保存全局调度信息，[定义前的说明](../../linux/kernel/sched/sched.h#L978) 指出每个独占的 cpuset 都对应一个。它不只服务于公平类：RT 任务的推送目标在 `rq->rd->cpupri` 中查找，见 [`find_lowest_rq()`](../../linux/kernel/sched/rt.c#L1789)；DEADLINE 的带宽准入按 `rq->rd->dl_bw` 计算，见 [`dl_bw_of()`](../../linux/kernel/sched/deadline.c#L118)。因此 root 分区也切开了 RT 与 DEADLINE 的全局调度范围。

**isolated 对调度的影响。** 隔离 CPU 的 `rq->sd` 为 `NULL`，依赖调度域的路径都不再起作用：

| 路径 | 在 isolated CPU 上的行为 | 依据 |
| ---- | ------------------------ | ---- |
| 周期负载均衡 | 不触发 | [`sched_balance_trigger()`](../../linux/kernel/sched/fair.c#L13263) |
| 即将空闲时拉任务（newidle） | 直接放弃 | [`sched_balance_newidle()`](../../linux/kernel/sched/fair.c#L13122) |
| nohz 空闲均衡 | 不参加 | [`nohz_balance_enter_idle()`](../../linux/kernel/sched/fair.c#L12843) |
| fork 选核 | 没有域可遍历，子任务留在父任务所在的 CPU | [初始 CPU](../../linux/kernel/sched/core.c#L4789)、[`select_task_rq_fair()`](../../linux/kernel/sched/fair.c#L8765) |
| 唤醒选核 | 快速路径找不到末级缓存（LLC）域，留在上次运行的 CPU（`WF_CURRENT_CPU` 唤醒除外） | [`select_idle_sibling()`](../../linux/kernel/sched/fair.c#L8075) |

所以 isolated 分区里的线程不会被自动铺开：只有亲和性变化（包括迁入分区）时才经 `cpumask_any_and_distribute()` 分散一次，之后新建的线程都从父线程所在的 CPU 起步。这与 [cgroup-v2.rst](../../linux/Documentation/admin-guide/cgroup-v2.rst#L2617) 的建议一致：多 CPU 的 isolated 分区中，任务应当逐个绑到具体 CPU 上。

**隔离集合与内核工作。** `isolated_cpus` 变化后，[`update_isolation_cpumasks()`](../../linux/kernel/cgroup/cpuset.c#L1452) 调用 [`workqueue_unbound_exclude_cpumask()`](../../linux/kernel/workqueue.c#L7022)：unbound 工作队列（工作项不固定在某颗 CPU 上执行）的可用集合改为“请求的 unbound 掩码 − `isolated_cpus`”，结果为空时保持请求值，见 [计算](../../linux/kernel/workqueue.c#L7038)。在隔离方面，cpuset 只主动通知工作队列这一个下游；其他子系统通过 [`cpu_is_isolated()`](../../linux/include/linux/sched/isolation.h#L73) 自行避开隔离 CPU，例如 vmstat 的周期刷新 [`vmstat_shepherd()`](../../linux/mm/vmstat.c#L2140) 和 memcg 的每 CPU 缓存回收 [`drain_all_stock()`](../../linux/mm/memcontrol.c#L2006)：

```text
cpu_is_isolated(cpu) = cpu 不在 HK_TYPE_DOMAIN 集合
                    或 cpu 不在 HK_TYPE_TICK 集合
                    或 cpu ∈ isolated_cpus（cpuset_cpu_is_isolated()）
```

isolated 的作用也有明确边界：cpuset 不改中断亲和性，不停 tick（那是 `nohz_full=` 的作用），绑定到具体 CPU 的工作队列和 per-CPU 内核线程照常在这些 CPU 上运行。

**与启动参数的关系。** `isolcpus=` 带 `domain` 标志或不带标志时，相应 CPU 被移出 `HK_TYPE_DOMAIN` 集合，见 [`housekeeping_isolcpus_setup()`](../../linux/kernel/sched/isolation.c#L200)。cpuset 在 [`cpuset_init()`](../../linux/kernel/cgroup/cpuset.c#L3932) 中记下启动时的这个集合，并把其余 CPU 预先放进 `isolated_cpus`。于是：

- 这些 CPU 不进入根分区的调度域，即生成规则中的 `∩ HK_TYPE_DOMAIN`。
- 本地建立 root 分区或调整其 CPU 时，[`prstate_housekeeping_conflict()`](../../linux/kernel/cgroup/cpuset.c#L1763) 拒绝包含它们的候选，它们只能进 isolated 分区。远端启用路径和 isolated → root 的[切换分支](../../linux/kernel/cgroup/cpuset.c#L3107)不做这项检查，不能推广为“所有路径都禁止”。
- `cpuset.cpus.isolated` 在没有任何 isolated 分区时也可能非空。[cgroup-v2.rst](../../linux/Documentation/admin-guide/cgroup-v2.rst#L2568) 写的是没有 isolated 分区时为空，与实现不一致，以实现为准。
- 有 `nohz_full=` 时，建立 isolated 分区不能用光同时属于 `HK_TYPE_KERNEL_NOISE` 与 `HK_TYPE_DOMAIN` 的 active CPU，见 [`isolated_cpus_can_update()`](../../linux/kernel/cgroup/cpuset.c#L1425)。

### 6.8 热插拔与删除

**热插拔。** CPU 进入或退出 active 状态时，[`sched_cpu_activate()`](../../linux/kernel/sched/core.c#L8416) 与 [`sched_cpu_deactivate()`](../../linux/kernel/sched/core.c#L8454) 分别经 [`cpuset_cpu_active()`](../../linux/kernel/sched/core.c#L8368) 和 [`cpuset_cpu_inactive()`](../../linux/kernel/sched/core.c#L8390) 同步调用 [`cpuset_handle_hotplug()`](../../linux/kernel/cgroup/cpuset.c#L4092)：

1. 重算根组：`effective_cpus` = active CPU − `subpartitions_cpus`。若 active CPU 全部落在 `subpartitions_cpus` 中，就清空它，让子分区重新竞争，见 [根组重算](../../linux/kernel/cgroup/cpuset.c#L4125)。
2. 先序遍历所有后代，逐个调用 [`cpuset_hotplug_update_tasks()`](../../linux/kernel/cgroup/cpuset.c#L3978)。它先等本组正在进行的迁入完成，再按公式重算。分区还要重判有效性：远端分区在 `subpartitions_cpus` 被清空、或有任务却没有 CPU 时以 `PERR_HOTPLUG` 失效，见 [远端判定](../../linux/kernel/cgroup/cpuset.c#L4016)；本地分区在父组无效、会让有任务的组没有 CPU、或 `subpartitions_cpus` 已被清空时失效，见 [本地判定](../../linux/kernel/cgroup/cpuset.c#L4025)；反过来，父组有效时无效分区可以转回有效，见 [恢复](../../linux/kernel/cgroup/cpuset.c#L4038)。
3. member 的结果为空时继承父组，有效分区允许为空，见 [`hotplug_update_tasks()`](../../linux/kernel/cgroup/cpuset.c#L3947)；集合有变化才更新组内任务。
4. 按需重建调度域，见 [重建](../../linux/kernel/cgroup/cpuset.c#L4176)。

热插拔从不修改用户写入的请求。CPU 重新上线后各组按原请求恢复，失效的分区也可能自动转回有效。系统挂起与恢复期间 cpuset 保持不变，调度器只是临时退回单一调度域，最后一颗 CPU 恢复上线时再按 cpuset 配置重建，见 [`cpuset_cpu_active()`](../../linux/kernel/sched/core.c#L8370)。

**删除。** 组的创建与释放回调已在第 2.4 节列出。与本章相关的有两点：新组上线时复制父组的 `effective_cpus`，此时 `cpuset.cpus` 为空，按 member 公式算出的也正是父组的集合，见 [`cpuset_css_online()`](../../linux/kernel/cgroup/cpuset.c#L3689)；删除有效分区时，[`cpuset_css_killed()`](../../linux/kernel/cgroup/cpuset.c#L3756) 先把它转为 member，独占 CPU 回到父分区，后续在其他组中可以重新申请。

### 6.9 小结

- **对象**：每组一个 `struct cpuset`，用户请求（`cpus_allowed`、`exclusive_cpus`）与内核结果（`effective_cpus`、`effective_xcpus`）分开存放；`top_cpuset`、`subpartitions_cpus` 和 `isolated_cpus` 记录全局划分。
- **算法**：`update_cpumasks_hier()` 先序遍历重算有效集合，member 取“请求 ∩ 父组集合”、落空则继承；有效分区经 `partition_xcpus_add()/del()` 从父分区划出和归还独占 CPU；条件不满足时转为保留类型的无效状态，写入仍然成功，条件恢复后可以自动转回。
- **落实**：有效集合按值写入每个任务的 `cpus_mask`，由 `set_cpus_allowed_ptr()` 同步完成迁移；root 分区得到独立的调度域和 `root_domain`，isolated 分区的 CPU 不在任何域中、不做负载均衡；`isolated_cpus` 让 unbound 工作队列和 `cpu_is_isolated()` 的使用者避开这些 CPU。

## 7. 综合示例：给延迟敏感容器划出 CPU 4-7

8 颗逻辑 CPU 全部 active，没有启动隔离参数，CPU 4-7 覆盖完整的 SMT 兄弟（需按第 1.3 节核对实际拓扑）。目标是让延迟敏感容器独占 4-7，并减少负载均衡和部分内核工作的干扰：

```text
/sys/fs/cgroup                      根组，内部为 PRS_ROOT，最终 effective = 0-3
  cgroup.subtree_control = cpu cpuset
  ├── system.slice                  cpus = 0-3，partition = member
  └── pod-latency
        cpuset.cpus = 4-7
        cpuset.cpus.partition = isolated
        cpu.max = max 100000        不靠配额限速
```

`system.slice` 保持 member，让 0-3 留在根分区。若把它也设成 root 并拿走 0-3，再把 4-7 划给 pod，就会抽空仍有任务的根分区而失败。

| 步骤 | 内核中的变化 |
| ---- | ------------ |
| 根组启用 `+cpu +cpuset` 并建目录 | 子组各得到 `task_group`（含每 CPU 队列）和 `struct cpuset`；子组中任务不再使用 autogroup |
| pod 写 `cpuset.cpus = 4-7` | 仍是 member：pod 任务只能跑 4-7，根组任务仍可使用 4-7 |
| pod 写 `cpuset.cpus.partition = isolated` | 本地 isolated 分区：`partition_xcpus_add()` 从根组 `effective_cpus` 删除 4-7，记入 `subpartitions_cpus` 与 `isolated_cpus` |
| 更新任务亲和性 | 根组与 `system.slice` 的任务落在 0-3（`PF_NO_SETAFFINITY` 任务除外）；pod 任务落在 4-7，用户 affinity 可更窄 |
| 更新工作队列 | 默认 unbound 工作队列排除 4-7；绑定工作队列不受影响 |
| 重建调度域 | 只为 0-3 建域；4-7 不在任何负载均衡域内 |

只要独占、仍希望 4 颗 CPU 之间互相均衡，把 partition 写成 `root` 即可；只需要共享 CPU 时不建分区，只用 `cpu.weight` / `cpu.max`。

三种机制**同时生效**，各自解决一类问题：

| 机制 | 直接作用 | 不解决的问题 |
| ---- | -------- | ------------ |
| `cpuset.cpus`（member） | 普通任务只在有效集合内运行 | 不排斥其他组；空交集时回退为父组集合 |
| partition `root` / `isolated` | 独占 CPU；isolated 还去掉负载均衡 | 不排除 per-CPU 内核线程、绑定工作队列、中断、NMI |
| `cpu.weight` | 同核争用时的长期份额 | 不是预留，也不是上限 |
| `cpu.max` | 公平类的全组执行预算 | 不管 RT/DL；不决定位置；短时并行可超出窗口 |

两个反例：独占 CPU 后再设很小的 `cpu.max` 仍会限流，因为预算按执行时间扣，与 CPU 是否专用无关；`cpu.max = 200000 100000` 但只允许 1 颗 CPU，100 ms 内也执行不了 200 ms。

## 8. 验证与排查

### 8.1 检查顺序

按“归属 → 有效状态 → 配置 → 统计增量 → 任务掩码”检查：

1. `/proc/<pid>/cgroup` 确认目录，v2 显示为 `0::/path`。本组没有自己的 cpu 状态时，看最近一个启用了 cpu 的祖先；一路到根都没有时，任务在 autogroup 中（`/proc/<pid>/autogroup` 可见）。
2. 读本组及祖先的 `cpu.weight`、`cpu.idle`、`cpu.max`、`cpu.max.burst`。叶子为 `max` 仍要看祖先。
3. 读 `cpuset.cpus.partition` 和各 `.effective` 文件。带 `invalid` 就没有独占；本地分区的父组 `cpuset.cpus.effective` 不应再包含已下发的 CPU。
4. 逐线程核对 `/proc/<pid>/task/<tid>/status` 的 `Cpus_allowed_list`，见 [`task` 目录](../../linux/fs/proc/base.c#L3298) 与 [线程 `status`](../../linux/fs/proc/base.c#L3656)。亲和性属于线程，只看主线程不能代表全部。
5. 两次读取 `cpu.stat` / `cpu.stat.local`，按下一节的口径比较增量。

### 8.2 `cpu.stat` 与 `cpu.stat.local` 的口径

`cpu.stat` 先输出 rstat 的基础使用统计，再输出 cpu 控制器的带宽项，见 [`cpu_stat_show()`](../../linux/kernel/cgroup/cgroup.c#L3952)、[基础项](../../linux/kernel/cgroup/rstat.c#L743) 与 [`cpu_extra_stat_show()`](../../linux/kernel/sched/core.c#L10088)。基础项不要求本组有 cpu 状态；带宽项只有取得本组 cpu css 才输出，见 [css 查找](../../linux/kernel/cgroup/cgroup.c#L3923)。`cpu.stat.local` 由 [`cpu_local_stat_show()`](../../linux/kernel/sched/core.c#L10114) 输出。

| 字段 | 口径 |
| ---- | ---- |
| `usage_usec` | 本组子树的累计执行时间，涵盖各调度类，向父组汇总见 [`cgroup_base_stat_flush()`](../../linux/kernel/cgroup/rstat.c#L572)；根组直接取[全系统统计](../../linux/kernel/cgroup/rstat.c#L678) |
| `nr_periods` | 周期回调累加的 `overrun`；定时器停用期间不计，不能用观察时长 ÷ period 推算 |
| `nr_throttled` | 周期回调入口看到受限队列链表非空时累加的 `overrun`，见 [`do_sched_cfs_period_timer()`](../../linux/kernel/sched/fair.c#L6401)；不是限流次数 |
| `throttled_usec` | 各 CPU 队列因**本组自身**限流记录的时长之和，解除时[累入 `throttled_time`](../../linux/kernel/sched/fair.c#L6206)；4 颗 CPU 各限流 80 ms，增量约 320 ms |
| `nr_bursts`、`burst_usec` | 补充时 `runtime_snap` 高于“余额 + quota”的次数与超出量，见 [补充函数](../../linux/kernel/sched/fair.c#L5802)；改配置时也可能增长 |
| `cpu.stat.local` 的 `throttled_usec` | 各 CPU `throttled_clock_self_time` 之和，见 [`throttled_time_self()`](../../linux/kernel/sched/core.c#L9778)；**祖先限流也计入** |

计时起点见 [`record_throttle_clock()`](../../linux/kernel/sched/fair.c#L6107)：任务因限流出队时才开始，不是队列被标记时。对应的结论：

- 父 P 有限、子 A 为 `max`，P 限流时 A 的 `cpu.stat` 限流项不涨，`cpu.stat.local` 会涨。
- `usage_usec` 增加 200000、墙钟过去 100 ms，表示平均约 2 颗 CPU。`throttled_usec` 是各 CPU 时长之和，不能与 usage 相加。
- 未结束的限流区间尚未计入，短时间采样没有增量也不能排除正在限流。

### 8.3 从现象到源码路径

| 现象 | 优先核对 | 对应章节 |
| ---- | -------- | -------- |
| 调大权重，吞吐不涨 | 是否真有持续争用？是否先被配额或亲和性限制？ | 4.1、5、6.6 |
| 叶子 `cpu.max` 为 `max` 仍受限 | 祖先 `cpu.max` 和本组 `cpu.stat.local` | 5.7、8.2 |
| 限流时长超过观察窗口 | 是否把多 CPU 时长相加？区间是否已结束？ | 8.2 |
| 任务状态为 R 却不运行 | 是否在 limbo：组预算是否耗尽 | 5.5 |
| 整机有空闲 CPU，线程却排队 | `Cpus_allowed_list`、有效集合、isolated 分区内的线程分布 | 6.6、6.7 |
| 写了 CPU 列表，其他组仍来运行 | 是否仍为 member？分区是否 invalid？ | 6.3、6.4 |
| isolated 后仍有内核工作 | 区分 unbound / 绑定工作队列、per-CPU 线程和中断 | 6.7 |

## 9. 回顾

本文的核心对象与关系可以收成三句话：

1. **归属**：任务经 `css_set.subsys[]` 找到每个控制器的**有效** css，它可能属于祖先；cpu 控制器再用 `sched_task_group` 副本接入调度器。
2. **时间**：`task_group` 是全组对象，`se[cpu]` / `cfs_rq[cpu]` 是每 CPU 的竞争层级。`cpu.weight` 经 `calc_group_shares()` 变成每 CPU 组实体权重，影响父层虚拟时间；`cpu.max` 由共享池分配、每 CPU 扣账，耗尽时先标记队列、再让任务进入 limbo，补充后逐层解除。
3. **位置**：cpuset 由请求算出有效集合；member 只限制自己，有效分区从父分区划走 CPU。结果一路写进任务的 `cpus_mask`，一路重建调度域。

| 想回答的问题 | 先看的对象 | 入口函数 | 章节 |
| ------------ | ---------- | -------- | ---- |
| 目录怎样变成调度器中的组？ | `cgroup`、css、`task_group` | [`cpu_cgrp_subsys`](../../linux/kernel/sched/core.c#L10304)、[`sched_create_group()`](../../linux/kernel/sched/core.c#L9125) | 2、3.5 |
| 任务换组后在哪排队？ | `css_set`、`sched_task_group`、`se` | [`sched_move_task()`](../../linux/kernel/sched/core.c#L9232)、[`set_task_rq()`](../../linux/kernel/sched/sched.h#L2178) | 3.3、3.5 |
| 组怎样入队和被选中？ | `se.parent / my_q`、`cfs_rq` | [`enqueue_task_fair()`](../../linux/kernel/sched/fair.c#L7080)、[`pick_task_fair()`](../../linux/kernel/sched/fair.c#L9104) | 4.2～4.3 |
| weight 怎样变成每 CPU 权重？ | `tg->shares / load_avg`、组实体 `load.weight` | [`calc_group_shares()`](../../linux/kernel/sched/fair.c#L4086)、[`reweight_entity()`](../../linux/kernel/sched/fair.c#L3949) | 4.4～4.5 |
| 预算怎样扣除、耗尽和恢复？ | 共享池、本地余额、`throttle_count`、limbo | [`__account_cfs_rq_runtime()`](../../linux/kernel/sched/fair.c#L5859)、[`throttle_cfs_rq_work()`](../../linux/kernel/sched/fair.c#L5913)、[`do_sched_cfs_period_timer()`](../../linux/kernel/sched/fair.c#L6393) | 5.3～5.6 |
| cpuset 请求怎样变成有效范围？ | `cpus_allowed / effective_cpus` | [`update_cpumasks_hier()`](../../linux/kernel/cgroup/cpuset.c#L2227)、[`compute_effective_cpumask()`](../../linux/kernel/cgroup/cpuset.c#L1227) | 6.3 |
| 独占 CPU 怎样从父组移出？ | `effective_xcpus`、`partition_root_state` | [`update_prstate()`](../../linux/kernel/cgroup/cpuset.c#L3050)、[`partition_xcpus_add()`](../../linux/kernel/cgroup/cpuset.c#L1358) | 6.4～6.5 |
| 放置和均衡边界怎样落实？ | `cpus_mask / cpus_ptr`、`rq.sd` | [`cpuset_update_tasks_cpumask()`](../../linux/kernel/cgroup/cpuset.c#L1192)、[`generate_sched_domains()`](../../linux/kernel/cgroup/cpuset.c#L831) | 6.6～6.7 |
