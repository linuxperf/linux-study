# 1.6 setup_arch：建立平台与内存视图

> **本节源码**：[`arch/x86/kernel/setup.c`](../../linux/arch/x86/kernel/setup.c)、[`arch/x86/kernel/e820.c`](../../linux/arch/x86/kernel/e820.c)、[`arch/x86/mm/init.c`](../../linux/arch/x86/mm/init.c)、[`mm/memblock.c`](../../linux/mm/memblock.c)
> **关键函数**：`setup_arch()`、`early_reserve_memory()`、`e820__memory_setup()`、`e820__memblock_setup()`、`init_mem_mapping()`、`initmem_init()`
> **前置知识**：1.5 早期页表、物理内存范围与保留区的概念

## 概述

固件提供的内存表只是平台描述，还不能直接作为内核分配器的空闲页列表。内核需要清洗这些信息、保留自身及启动数据、建立可访问的映射，并识别处理器与中断控制器。`setup_arch()` 把这些 x86 特有的准备集中起来，为后续通用内存管理和调度初始化提供基础。本节按依赖关系拆解该函数，而不把平台分支逐一平铺。

## 核心数据结构

### 三种内存视图回答不同问题

| 视图 | 回答的问题 | 不应混淆为 |
|---|---|---|
| E820 表 | 固件及早期策略把某个物理范围标成什么类型？ | 最终空闲页列表 |
| `memblock.memory` / `memblock.reserved` | 哪些范围属于内存，哪些不能供早期分配覆盖？ | 互斥的两种物理地址类别；保留区可以位于内存范围内部 |
| `iomem_resource` 资源树 | 哪些物理地址范围被 RAM、内核或设备资源占用？ | 页表，或能直接计算出剩余可分配页数的统计表 |

E820 的工作表、固件表和 kexec 相关副本在 [`arch/x86/kernel/e820.c:1224-1238`](../../linux/arch/x86/kernel/e820.c#L1224-L1238) 建立；memblock 的范围组织见 [`include/linux/memblock.h:66-112`](../../linux/include/linux/memblock.h#L66-L112)。其中 `memblock_region.base` / `size` 表示范围，`flags` 表示属性，`nid` 在 `CONFIG_NUMA=y` 时记录节点；`memblock_type` 管理范围数组，`memblock.current_limit` 约束当前分配上限。

### 平台差异通过操作表收敛

`x86_init` 等操作表使普通 PC、虚拟机和特定平台能够替换内存探测、资源初始化等行为。例如 `e820__memory_setup()` 并非直接写死调用 BIOS，而是调用 `x86_init.resources.memory_setup()`，见 [`arch/x86/kernel/e820.c:1224-1238`](../../linux/arch/x86/kernel/e820.c#L1224-L1238)。运行在 EFI 或虚拟机中仍出现带 BIOS 字样的历史日志，不足以证明调用过 BIOS 实模式服务。

## 主流程分析

以下只列主线；`setup_arch()` 中还有大量平台修正与条件分支。

```text
start_kernel()                         init/main.c
└── setup_arch()                       arch/x86/kernel/setup.c
    ├── early_cpu_init()
    ├── early_ioremap_init()
    ├── parse_boot_params()
    ├── early_reserve_memory()
    ├── e820__memory_setup()           arch/x86/kernel/e820.c
    ├── parse_setup_data()
    ├── parse_early_param()            init/main.c
    ├── efi_init()〔EFI 启动〕
    ├── kernel_randomize_memory()
    ├── reserve_brk()
    ├── e820__memblock_setup()         arch/x86/kernel/e820.c
    ├── init_mem_mapping()             arch/x86/mm/init.c
    ├── reserve_initrd()
    ├── initmem_init()                 arch/x86/mm/init_64.c
    ├── acpi_boot_init()
    ├── init_apic_mappings()
    └── topology_init_possible_cpus()
```

### 先保护仍在使用的数据

`setup_arch()` 在把内存大量交给 memblock 之前调用 `early_reserve_memory()`，先保护内核镜像、initrd 等仍需使用的区域。调用处明确说明：之后未保留且未标为非 RAM 的范围可能被分配覆盖，见 [`arch/x86/kernel/setup.c:950-964`](../../linux/arch/x86/kernel/setup.c#L950-L964)。

这也解释了为什么“内存表已经有这段 RAM”不代表可以先分配再补保留。保留是分配行为的前置约束，而不是事后记账。

### 从固件范围表生成工作视图

`e820__memory_setup()` 取得初始范围表并保存副本，`parse_setup_data()` 再处理扩展输入。随后 `parse_early_param()` 使 `mem=`、`memmap=` 等早期策略有机会影响可用范围，EFI 路径也会处理相应内存映射与保留信息。主序列见 [`arch/x86/kernel/setup.c:964-1003`](../../linux/arch/x86/kernel/setup.c#L964-L1003)。

`max_pfn` 等边界来自处理后的表，并用于后续地址空间布局与映射规划。`kernel_randomize_memory()` 的调用早于这些区域的实际使用，见 [`arch/x86/kernel/setup.c:1034-1052`](../../linux/arch/x86/kernel/setup.c#L1034-L1052)。它受 `CONFIG_RANDOMIZE_MEMORY=y` 支持，与 1.4 中镜像本身的 KASLR 不应混为一个步骤。

### 结束 brk 使用，再扩展 memblock 与页表

brk 是启动期间的一段预留分配空间。`reserve_brk()` 必须早于 `e820__memblock_setup()`，否则后续 memblock 分配可能覆盖它，见 [`arch/x86/kernel/setup.c:1074-1090`](../../linux/arch/x86/kernel/setup.c#L1074-L1090)。

`e820__memblock_setup()` 才逐步把适合的内存范围交给 memblock，并允许范围表扩容，见 [`arch/x86/kernel/e820.c:1240-1315`](../../linux/arch/x86/kernel/e820.c#L1240-L1315)。此时还要遵守当前已映射地址的上限，不能因为物理内存存在就通过任意虚拟地址访问它。

### 扩展直接映射，接替早期缺页处理

`init_mem_mapping()` 建立更完整的物理内存直接映射。紧接着 `cpu_init_replace_early_idt()` 接替 64 位早期页故障入口；若运行环境支持并启用灵活返回与事件投递（Flexible Return and Event Delivery，FRED），也可能切入相应事件投递机制。顺序见 [`arch/x86/kernel/setup.c:1130-1149`](../../linux/arch/x86/kernel/setup.c#L1130-L1149)。当前 `CONFIG_X86_FRED=y` 只表示支持，不表示虚拟机一定使用 FRED。

接下来 `reserve_initrd()` 完成初始镜像的检查与必要处理，`initmem_init()` 通过 `x86_numa_init()` 建立 NUMA 内存组织基础，见 [`arch/x86/mm/init_64.c:825-835`](../../linux/arch/x86/mm/init_64.c#L825-L835)（当前 `CONFIG_NUMA=y`）。至于普通页何时全面进入伙伴系统，由后续通用 `mm_core_init()` 等阶段完成，详见 [4.2 内存初始化](../04-memory/02-init.md)。

### 识别 CPU 不等于已经让 CPU 执行

后半段解析 ACPI 与平台 SMP 信息、建立 APIC 映射并确定可能的 CPU 集合，见 [`arch/x86/kernel/setup.c:1234-1251`](../../linux/arch/x86/kernel/setup.c#L1234-L1251)。真正启动其他 CPU 在 PID 1 上下文的 `smp_init()` 阶段，不能把这里的拓扑枚举当作 AP 已上线。

## 关键细节 / 设计考量

- **两次 initrd 保留有不同职责**：早期保留避免被分配覆盖，后续处理还需结合已建立的内存映射。实现见 [`arch/x86/kernel/setup.c:350-392`](../../linux/arch/x86/kernel/setup.c#L350-L392)。
- **低地址资源很特殊**：实模式跳板需要低地址空间，且部分固件可能破坏低端 RAM。预留发生在完整内存映射建立之前，原因见 [`arch/x86/kernel/setup.c:1112-1135`](../../linux/arch/x86/kernel/setup.c#L1112-L1135)。
- **架构初始化不是一个可无条件并行的清单**：早期保留、brk 收尾、范围导入与映射建立都有先后约束，随意调换可能导致静默覆盖，而不只是返回一个错误码。

## 可观测性

| 入口 | 看什么、怎么解读 | 实现与配置 |
|---|---|---|
| 启动日志中的 E820 范围 | 固件输入及经过处理的范围；注意多个阶段可能各自打印一份表 | [`arch/x86/kernel/e820.c:1224-1238`](../../linux/arch/x86/kernel/e820.c#L1224-L1238)；`CONFIG_PRINTK=y` |
| `memblock=debug` | 早期范围增删与保留活动，需在重启前设置，日志量较大 | [`mm/memblock.c:2111-2118`](../../linux/mm/memblock.c#L2111-L2118) |
| `/sys/kernel/debug/memblock/memory`、`reserved` | 当前保留的 memblock 元数据，列出范围、节点和标志；不是逐页空闲情况 | [`mm/memblock.c:2710-2767`](../../linux/mm/memblock.c#L2710-L2767)；`CONFIG_DEBUG_FS=y`、`CONFIG_ARCH_KEEP_MEMBLOCK=y`，且需挂载 debugfs |
| `/proc/iomem` | `iomem_resource` 层次，适合对照 System RAM 与 Kernel code 等资源 | [`kernel/resource.c:149-160`](../../linux/kernel/resource.c#L149-L160)；`CONFIG_PROC_FS=y`，权限策略可能隐藏真实地址 |

## 动手实验

在目标 dev 虚拟机中以 root 执行以下只读检查。未实测，不给出虚构范围；若需要 memblock 早期日志，应在可恢复启动项中临时追加 `memblock=debug` 后重启。

```bash
uname -r
mountpoint -q /sys/kernel/debug || mount -t debugfs debugfs /sys/kernel/debug
head -n 20 /sys/kernel/debug/memblock/memory
head -n 20 /sys/kernel/debug/memblock/reserved
head -n 30 /proc/iomem
dmesg | grep -Ei 'e820|memblock|initrd'
```

重点比较“可描述的 RAM”“启动保留范围”和“当前资源树”，不要用三者任一直接替代 `/proc/meminfo` 中的可用内存估计。

## 小结

- `setup_arch()` 把引导输入转换为内核可用的平台、内存和 CPU 视图。
- E820、memblock 和资源树各有职责，不是三个名字的同一张表。
- 先保留关键区域，再开放分配，是防止启动数据被覆盖的关键顺序。
- 完整映射、通用页分配器和其他 CPU 上线分别发生在不同阶段。

## 参考

- [`Documentation/core-api/boot-time-mm.rst`](../../linux/Documentation/core-api/boot-time-mm.rst)。
- [`Documentation/arch/x86/x86_64/mm.rst`](../../linux/Documentation/arch/x86/x86_64/mm.rst)。
- [4.2 内存初始化：从 memblock 到伙伴系统](../04-memory/02-init.md)。
