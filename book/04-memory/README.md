# 第 4 章 内存管理

本章分析 Linux v6.18.60 在 x86_64 上的内存管理子系统，源码主要位于 `mm/` 与 `arch/x86/mm/`。

内存管理可以分为两个相对独立又紧密协作的部分：**物理内存管理**回答"空闲的物理页在哪里、如何分配和回收"，**虚拟内存管理**回答"进程的每个虚拟地址对应什么、何时建立映射"。本章沿"物理内存的组织 → 内核内存分配器 → 进程地址空间 → 缺页与映射 → 回收与交换 → 碎片与 OOM → 大页 → 资源控制"的主线展开。

```
                 ┌────────────────────── 用户进程 ──────────────────────┐
                 │  mmap / brk / fork / 访问内存                         │
                 └──────────────┬────────────────────────────┬───────────┘
                     4.7 VMA 与 mmap                  4.8 缺页异常
                                │                            │
                 4.9 写时复制与反向映射 ◀──────────── 4.11 交换 / 4.13 大页
                                │                            │
  4.5 SLUB ── 4.6 vmalloc ──────┴──────── 4.3 伙伴系统 ◀── 4.4 水位线与慢速路径
                                                │                │
                               4.1 node/zone/page/folio   4.10 回收 · 4.12 规整与 OOM
                               4.2 memblock 初始化         4.14 memcg
```

*图 4-0：本章各小节的关系*

## 阅读建议

- 4.1～4.4 是物理内存管理的基础，建议按顺序阅读；4.3 伙伴系统是全章的核心。
- 4.5、4.6 分别介绍小对象分配器和虚拟连续分配器，可独立阅读。
- 4.7～4.9 构成进程虚拟内存的主线：VMA → 缺页 → 写时复制与反向映射。
- 4.10～4.12 讨论内存紧张时的应对：回收、交换、规整、OOM，依赖 4.4 与 4.9。
- 4.13、4.14 是较为独立的专题。

## 本章配置

本章以 `linux/.config` 为准，与内存管理相关的主要配置项如下：

| 配置项 | 取值 | 影响 |
|---|---|---|
| `CONFIG_NUMA` / `CONFIG_NODES_SHIFT` | y / 10 | NUMA 支持，最多 1024 个节点 |
| `CONFIG_PGTABLE_LEVELS` | 5 | 支持五级页表，运行时依 CPU 决定是否启用 |
| `CONFIG_SPARSEMEM_VMEMMAP` | y | `pfn_to_page()` 为指针运算 |
| `CONFIG_ZONE_DMA` / `CONFIG_ZONE_DMA32` / `CONFIG_ZONE_DEVICE` | y / y / y | x86_64 的 zone 组成 |
| `CONFIG_RANDOMIZE_MEMORY` | y | 直接映射区、vmalloc、vmemmap 基址随机化 |
| `CONFIG_SHUFFLE_PAGE_ALLOCATOR` | y | 空闲页随机化（需启动参数开启） |
| `CONFIG_SLUB` / `CONFIG_SLUB_CPU_PARTIAL` | y / y | 唯一的 slab 实现 |
| `CONFIG_SLAB_FREELIST_HARDENED` / `CONFIG_SLAB_FREELIST_RANDOM` / `CONFIG_RANDOM_KMALLOC_CACHES` | y / y / y | 堆加固 |
| `CONFIG_INIT_ON_ALLOC_DEFAULT_ON` | y | 新分配页默认清零 |
| `CONFIG_PER_VMA_LOCK` | y | 缺页可只锁单个 VMA |
| `CONFIG_LRU_GEN` / `CONFIG_LRU_GEN_ENABLED` | y / y | 默认使用 MGLRU |
| `CONFIG_SWAP` / `CONFIG_ZSWAP` | y / y | 交换与压缩交换缓存（zswap 默认关闭） |
| `CONFIG_COMPACTION` / `CONFIG_MIGRATION` | y / y | 内存规整与页迁移 |
| `CONFIG_TRANSPARENT_HUGEPAGE` / `CONFIG_TRANSPARENT_HUGEPAGE_MADVISE` | y / y | THP，默认 `madvise` 策略 |
| `CONFIG_HUGETLBFS` / `CONFIG_HUGETLB_PAGE_OPTIMIZE_VMEMMAP` | y / y | hugetlbfs 与 HVO（HVO 默认关闭） |
| `CONFIG_MEMCG` / `CONFIG_MEMCG_V1` | y / n | 仅 cgroup v2 内存控制器 |
| `CONFIG_DEFERRED_STRUCT_PAGE_INIT` | n | 启动时同步初始化全部 `struct page` |
| `CONFIG_KFENCE` / `CONFIG_KASAN` | y / n | KFENCE 已编译（默认采样间隔为 0） |

## 小节索引

| 小节 | 状态 |
|---|---|
| [4.1 物理内存模型：node、zone、page 与 folio](01-physical-model.md) | 已完成 |
| [4.2 内存初始化：从 memblock 到伙伴系统](02-init.md) | 已完成 |
| [4.3 伙伴系统与 per-CPU 页缓存](03-buddy.md) | 已完成 |
| [4.4 水位线与页分配慢速路径](04-slowpath.md) | 已完成 |
| [4.5 SLUB 分配器](05-slub.md) | 已完成 |
| [4.6 内核虚拟地址空间与 vmalloc](06-vmalloc.md) | 已完成 |
| [4.7 进程地址空间：mm_struct、VMA 与 mmap](07-address-space.md) | 已完成 |
| [4.8 页表与缺页异常](08-page-fault.md) | 已完成 |
| [4.9 写时复制与反向映射](09-cow-rmap.md) | 已完成 |
| [4.10 页面回收：LRU、MGLRU 与 kswapd](10-reclaim.md) | 已完成 |
| [4.11 交换机制](11-swap.md) | 已完成 |
| [4.12 页迁移、内存规整与 OOM Killer](12-compaction-oom.md) | 已完成 |
| [4.13 大页：hugetlbfs 与透明大页](13-hugepage.md) | 已完成 |
| [4.14 内存控制组（memcg）](14-memcg.md) | 已完成 |

## 待完善

- [ ] 各小节"动手实验"尚未在 dev 虚拟机中实测，需补充真实输出示例（注明内核版本与配置）。
- [ ] 4.2 图 4-4、4.3 图 4-6、4.4 图 4-8、4.8 图 4-13、4.9 图 4-14、4.11 图 4-18、4.12 图 4-20 的 Mermaid 图需在目标渲染器中确认显示效果。
- [ ] 部分历史版本号（如各特性的引入版本）依据社区资料整理，后续可补充对应的提交号以便核对。
- [ ] page cache、预读与脏页写回的实现留待第 9 章（虚拟文件系统）展开，4.8、4.10 中相关内容届时补充交叉引用。
