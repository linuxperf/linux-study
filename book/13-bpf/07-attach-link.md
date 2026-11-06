# 13.7 挂载机制：link、trampoline 与 struct_ops

> **本节源码**：[`kernel/bpf/syscall.c`](../../linux/kernel/bpf/syscall.c)、[`include/linux/bpf.h`](../../linux/include/linux/bpf.h)、[`kernel/bpf/trampoline.c`](../../linux/kernel/bpf/trampoline.c)、[`arch/x86/net/bpf_jit_comp.c`](../../linux/arch/x86/net/bpf_jit_comp.c)、[`kernel/bpf/bpf_struct_ops.c`](../../linux/kernel/bpf/bpf_struct_ops.c)
> **关键函数**：`link_create()`、`bpf_link_prime()`、`bpf_link_settle()`、`bpf_tracing_prog_attach()`、`bpf_trampoline_update()`、`bpf_struct_ops_map_update_elem()`、`bpf_struct_ops_link_create()`
> **前置知识**：13.2 生命周期、13.5 JIT、13.6 BTF；struct_ops 可对照 3.11 sched_ext

## 概述

加载后的程序还需要回答“由谁、在什么位置调用”。不同子系统历史上使用 perf event、netlink、cgroup attach 等接口管理挂载，生命周期并不统一。BPF link 把一次挂载关系封装成可引用的对象，但实际接入机制仍由目标子系统实现。trampoline 负责部分函数挂载的调用桥接，struct_ops 则把 BPF 回调接入已开放的子系统操作表；三者不能当作同一个概念。

## 核心数据结构

### link 负责挂载对象，不负责定义所有钩子

| `struct bpf_link` 字段 | 含义 |
|---|---|
| `refcnt` | 挂载对象的引用计数 |
| `id` / `type` | 查询 ID 与 link 类型 |
| `ops` | 目标相关的释放、回收、信息填充、更新等操作 |
| `prog` | 普通程序 link 关联的程序；部分 link 通过其他对象间接关联程序 |
| `flags` / `attach_type` | 挂载语义及标志 |
| `sleepable` | link 目标的回收语义，可能与程序本身是否可睡眠不同 |

定义见 [`include/linux/bpf.h:1751-1818`](../../linux/include/linux/bpf.h#L1751-L1818)。struct_ops 外层 link 关联 map，不能假定所有 link 的 `prog` 都非空；创建时传入空程序指针的实际代码见 [`kernel/bpf/bpf_struct_ops.c:1378-1380`](../../linux/kernel/bpf/bpf_struct_ops.c#L1378-L1380)。

### 三个层次

- **挂载对象**：link 表示一次关系及其引用。
- **调用桥**：BPF 蹦床（BPF trampoline）适配目标函数与 BPF 程序的调用、参数、返回和统计路径。
- **子系统接口**：struct_ops 根据 BTF 描述和已注册类型，把回调程序组织成子系统认可的操作表。

trampoline 的通用更新实现在 [`kernel/bpf/trampoline.c:407-511`](../../linux/kernel/bpf/trampoline.c#L407-L511)，struct_ops 的回调解析在 [`kernel/bpf/bpf_struct_ops.c:688-838`](../../linux/kernel/bpf/bpf_struct_ops.c#L688-L838)。

## 主流程分析

### 创建 link：先准备资源，再公开

```text
bpf(BPF_LINK_CREATE)                     kernel/bpf/syscall.c
└── link_create()
    ├── cgroup_bpf_link_attach()         kernel/bpf/cgroup.c
    ├── bpf_tracing_prog_attach()        kernel/bpf/syscall.c
    ├── bpf_raw_tp_link_attach()         kernel/bpf/syscall.c
    ├── bpf_xdp_link_attach()            net/core/dev.c
    ├── tcx_link_attach()                kernel/bpf/tcx.c
    ├── bpf_perf_link_attach()           kernel/bpf/syscall.c
    └── bpf_struct_ops_link_create()     kernel/bpf/bpf_struct_ops.c

典型 link 发布协议
bpf_link_prime() → 执行目标挂载 → bpf_link_settle()
                     └── 失败：bpf_link_cleanup()
```

`link_create()` 先检查程序类型与挂载类型，再选择实现，见 [`kernel/bpf/syscall.c:5694-5803`](../../linux/kernel/bpf/syscall.c#L5694-L5803)。struct_ops 是提前按挂载类型分流的特殊路径，不能机械地理解为所有 link 都先从 `prog_fd` 获取程序。

`bpf_link_prime()` 预留 FD、ID 和匿名文件，`bpf_link_settle()` 在挂载成功后发布 ID 并安装 FD。这样可以避免目标已经完成昂贵挂载后，才因无法分配 FD 而进行复杂回滚，见 [`kernel/bpf/syscall.c:3423-3482`](../../linux/kernel/bpf/syscall.c#L3423-L3482)。

### trampoline：把程序串到目标调用路径

对基于 BTF 的函数挂载，`bpf_tracing_prog_attach()` 建立目标关系，再调用 `bpf_trampoline_link_prog()`，见 [`kernel/bpf/syscall.c:3571-3765`](../../linux/kernel/bpf/syscall.c#L3571-L3765)。trampoline 更新时收集目标上的程序，决定是否需要调用原函数、保留寄存器以及处理返回值，然后生成新映像。

```text
bpf_trampoline_link_prog()               kernel/bpf/trampoline.c
└── 内部更新路径
    └── bpf_trampoline_update()
        ├── 收集 fentry / fexit / modify_return 程序
        ├── arch_bpf_trampoline_size()
        ├── arch_prepare_bpf_trampoline() arch/x86/net/bpf_jit_comp.c
        ├── arch_protect_bpf_trampoline()
        └── register_fentry() 或 modify_fentry()
```

实际映像生成、注册与替换见 [`kernel/bpf/trampoline.c:407-504`](../../linux/kernel/bpf/trampoline.c#L407-L504)，x86 生成入口见 [`arch/x86/net/bpf_jit_comp.c:3465-3641`](../../linux/arch/x86/net/bpf_jit_comp.c#L3465-L3641)。旧映像可能仍有执行者，不能在换上新指针后立即释放；回收协议见 [`kernel/bpf/trampoline.c:260-324`](../../linux/kernel/bpf/trampoline.c#L260-L324)。

当前配置虽启用了 `CONFIG_BPF_JIT=y` 和 `CONFIG_DYNAMIC_FTRACE_WITH_DIRECT_CALLS=y`，但没有内核 BTF，因此不能据此直接运行对内核函数的常规 fentry/fexit 挂载。

### struct_ops：不是给任意结构体写函数地址

struct_ops 要求目标子系统先开放特定操作类型。用户态创建 `BPF_MAP_TYPE_STRUCT_OPS` 对象并提交回调程序 FD；内核核对 BTF 类型、成员序号和程序类型，生成对应回调桥，而不是相信用户提供的内核地址。

`bpf_struct_ops_map_update_elem()` 逐成员检查：非回调字段由类型实现处理或要求为零；回调字段解析成程序 FD，并验证程序的 `attach_btf_id` 与成员位置，见 [`kernel/bpf/bpf_struct_ops.c:744-818`](../../linux/kernel/bpf/bpf_struct_ops.c#L744-L818)。因此 struct_ops 不是通用的内核结构体写入接口。

在 link 管理模式下，注册流程为：

```text
bpf_struct_ops_link_create()             kernel/bpf/bpf_struct_ops.c
├── 获取 struct_ops map 并检查注册状态
├── 创建外层 link，bpf_link_prime()
├── st_ops->reg(kvalue, link)            具体子系统实现
├── 让 link 持有 map
└── bpf_link_settle()
```

实现见 [`kernel/bpf/bpf_struct_ops.c:1354-1405`](../../linux/kernel/bpf/bpf_struct_ops.c#L1354-L1405)。传统 map 驱动的注册方式与 link 模式存在差别，不能把“创建 struct_ops map”统一理解成“子系统已经接管”。本节分析通用框架，sched_ext 的注册与调度语义见 [3.11](../03-scheduler/11-sched-ext.md)；当前没有启用 `CONFIG_SCHED_CLASS_EXT`，也缺少其所需内核 BTF。

## 关键细节 / 设计考量

### 卸载对象与卸载程序是两回事

关闭未固定且没有其他引用的 link FD，最终会触发相应挂载关系的释放；程序仍可能因其他 FD、pin 或其他挂载而存在。反之，只关闭程序 FD，也不代表 link 已消失。link 的释放和延迟回收见 [`kernel/bpf/syscall.c:3224-3318`](../../linux/kernel/bpf/syscall.c#L3224-L3318)。

传统 TC filter、旧式 cgroup attach 和部分其他接口仍由子系统持有程序引用，不一定出现在 `bpftool link show` 中。挂载 API 与加载 API 分离，是排查“找不到 link 但程序仍运行”的关键。

### 更新能力取决于 link 类型

`BPF_LINK_UPDATE` 最终依赖具体类型的更新操作，struct_ops 还可能更新 map，而不是直接替换一个程序。分派见 [`kernel/bpf/syscall.c:5806-5902`](../../linux/kernel/bpf/syscall.c#L5806-L5902)。不能假定所有 link 都支持同一种热替换方式，目标类型、旧对象校验和子系统状态也需要满足约束。

## 可观测性

依赖 `CONFIG_BPF_SYSCALL=y`；`fdinfo` 依赖 `CONFIG_PROC_FS=y`，当前均已启用。

| 入口 | 看什么 |
|---|---|
| `bpftool link show` | link 类型、ID、关联程序与目标；查询分派见 [`kernel/bpf/syscall.c:5376-5417`](../../linux/kernel/bpf/syscall.c#L5376-L5417) |
| `/proc/<pid>/fdinfo/<link-fd>` | 通用字段加类型专用 `show_fdinfo()`；见 [`kernel/bpf/syscall.c:3343-3385`](../../linux/kernel/bpf/syscall.c#L3343-L3385) |
| `bpftool net`、TC 或 cgroup 查询工具 | 补充 link 枚举无法代表的传统挂载；工具实现在 [`tools/bpf/bpftool/net.c`](../../linux/tools/bpf/bpftool/net.c)、[`tools/bpf/bpftool/cgroup.c`](../../linux/tools/bpf/bpftool/cgroup.c) |
| `bpf_trampoline_update()` | 可作为观察挂载控制路径的候选动态探针；需内核 BTF 功能实际可用且运行时可探测，不应把它当作每次目标函数调用入口 |

struct_ops 的 link 可能经 map 持有多个回调，查询时应同时检查 link、map 和程序，不能只对一个 `prog_id` 做生命周期推断。

## 小结

- link 建模挂载关系，trampoline 生成调用桥，struct_ops 开放子系统操作接口。
- link 的准备与发布分阶段进行，降低挂载失败的回滚复杂度。
- 热替换要求保留仍被执行的旧映像，不能只考虑新指针可见性。
- struct_ops 受已注册类型、BTF 和回调校验约束，不是任意操作表替换。
- 程序保活、挂载保活与目标子系统状态应分别观察。

## 参考

- [`Documentation/bpf/prog_lsm.rst`](../../linux/Documentation/bpf/prog_lsm.rst)
- [`Documentation/scheduler/sched-ext.rst`](../../linux/Documentation/scheduler/sched-ext.rst)
- [`kernel/bpf/bpf_struct_ops.c`](../../linux/kernel/bpf/bpf_struct_ops.c)
