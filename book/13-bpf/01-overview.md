# 13.1 BPF 全景：程序、状态与挂载点

> **本节源码**：[`include/uapi/linux/bpf.h`](../../linux/include/uapi/linux/bpf.h)、[`include/linux/bpf.h`](../../linux/include/linux/bpf.h)、[`include/linux/bpf_types.h`](../../linux/include/linux/bpf_types.h)、[`kernel/bpf/syscall.c`](../../linux/kernel/bpf/syscall.c)、[`tools/lib/bpf/libbpf.c`](../../linux/tools/lib/bpf/libbpf.c)
> **关键函数**：`__sys_bpf()`、`bpf_prog_load()`、`bpf_check()`、`bpf_prog_select_runtime()`、`bpf_object__load()`
> **前置知识**：C 指针与函数指针、系统调用、7.1 并发来源与执行上下文

## 概述

内核需要可扩展的观测与策略接口，但不能把任意用户代码直接当成内核函数执行。BPF 的核心做法是使用受约束的指令集、加载期验证和类型相关的接口约定，让程序在明确的钩子上运行。程序通过 BPF 映射（BPF map）保存状态，通过挂载对象或子系统接口决定何时被调用。本章讨论的重点是内核如何落实这些约束，而不是某个 BPF 工具的命令语法。

本章以 eBPF 为主。BPF 已经是独立术语，不宜把现代 BPF 子系统限定为“伯克利包过滤器”；历史命名与 cBPF/eBPF 的区分见 [`Documentation/bpf/standardization/instruction-set.rst`](../../linux/Documentation/bpf/standardization/instruction-set.rst)。

## 核心数据结构

### 四类对象，四种职责

| 对象 | 内核表示 | 职责 |
|---|---|---|
| 程序 | `struct bpf_prog`、`struct bpf_prog_aux` | 指令、执行入口、类型、引用的 map、类型信息与统计 |
| map | `struct bpf_map` 及各类型容器 | 状态、程序跳转表、重定向目标、事件传输等 |
| BTF | `struct btf` | BPF 类型格式（BPF Type Format，BTF）描述的类型图，服务于类型检查和重定位 |
| link | `struct bpf_link` | 把一次挂载关系建模为可引用、可查询、可释放的对象 |

程序与 link 的定义见 [`include/linux/bpf.h:1592-1775`](../../linux/include/linux/bpf.h#L1592-L1775)，map 的公共头见 [`include/linux/bpf.h:295-349`](../../linux/include/linux/bpf.h#L295-L349)，BTF 编码见 [`include/uapi/linux/btf.h:11-55`](../../linux/include/uapi/linux/btf.h#L11-L55)。这些对象经常一起使用，但并非缺一不可：简单程序可以不用 map，部分传统挂载也没有 link 对象。

程序的 `type` 与 `expected_attach_type` 不是注释信息。前者选择程序操作集和验证操作集，后者进一步约束挂载语义；`bpf_verifier_ops` 中的 `get_func_proto()`、`is_valid_access()`、`convert_ctx_access()` 分别规定可调用的辅助函数（BPF helper）、可访问的上下文字段和实际访问转换，见 [`include/linux/bpf.h:1054-1080`](../../linux/include/linux/bpf.h#L1054-L1080)。

### 上下文不是任意内核地址

BPF 程序看到的上下文，经常只是一个受约束的视图。例如网络程序使用 `struct __sk_buff`，内核运行路径中传入的却可能是 `struct sk_buff`；XDP 的用户接口类型是 `struct xdp_md`，内部类型是 `struct xdp_buff`。这些对应关系集中登记在 [`include/linux/bpf_types.h:4-19`](../../linux/include/linux/bpf_types.h#L4-L19)。验证器检查并转换上下文访问，不能把 UAPI 结构体偏移直接当成内部对象偏移。

## 主流程分析

### 加载与运行是两条路径

下面是程序加载的主干。map、BTF 通常由用户态加载器另行创建；程序挂载也不是 `BPF_PROG_LOAD` 的隐含步骤。

```text
bpf()                                   kernel/bpf/syscall.c
└── __sys_bpf()
    ├── BPF_MAP_CREATE → map_create()
    ├── BPF_BTF_LOAD → bpf_btf_load()
    ├── BPF_PROG_LOAD → bpf_prog_load()
    │   ├── find_prog_type()
    │   ├── bpf_check()                  kernel/bpf/verifier.c
    │   ├── bpf_prog_select_runtime()    kernel/bpf/core.c
    │   └── bpf_prog_new_fd()
    └── BPF_LINK_CREATE → link_create()
        └── 按程序类型进入具体挂载实现
```

分派逻辑见 [`kernel/bpf/syscall.c:6163-6313`](../../linux/kernel/bpf/syscall.c#L6163-L6313)，加载的验证、编译与发布次序见 [`kernel/bpf/syscall.c:3073-3122`](../../linux/kernel/bpf/syscall.c#L3073-L3122)。程序成功加载，只说明内核接受了这个程序对象；若没有挂载或显式测试执行，它不会自行开始处理事件。

运行时则由网络、跟踪等子系统调用程序入口。通用执行封装最终传入上下文与 `prog->bpf_func`，见 [`include/linux/filter.h:732-763`](../../linux/include/linux/filter.h#L732-L763)。钩子解释程序的返回值：socket filter 使用截取长度，XDP 使用动作码，cgroup 网络钩子又有自己的许可与拥塞语义。不能把“返回 0”统一理解为“执行成功”。

### 用户态加载器做什么

Clang 通常生成 ELF 对象，而内核加载接口接收的是指令数组、属性和对象 FD，不负责解析整个 ELF。libbpf 在用户态解析对象、处理重定位、加载 BTF、创建 map，再提交程序；实际准备次序见 [`tools/lib/bpf/libbpf.c:8658-8689`](../../linux/tools/lib/bpf/libbpf.c#L8658-L8689)。

这里要区分三种转换：

1. **编译**：Clang 把源程序转换为 BPF 指令与元数据。
2. **加载准备**：libbpf 根据目标环境解析类型、重定位和对象依赖。
3. **内核处理**：验证器检查并可能改写指令，JIT 再生成本机机器码。

因此，ELF 反汇编、`bpftool prog dump xlated` 和 `bpftool prog dump jited` 展示的不是同一阶段的产物。后两者的信息由 [`kernel/bpf/syscall.c:4997-5282`](../../linux/kernel/bpf/syscall.c#L4997-L5282) 的程序信息查询路径提供，且受权限限制。

## 关键细节 / 设计考量

### 安全边界不是单独一个验证器

验证器负责证明指令访问和接口使用满足约束，但系统仍需要权限检查、LSM 检查、对象访问权限和挂载目标检查。`__sys_bpf()` 首先调用 `security_bpf()`；程序加载再检查 capability、程序类型和加载标志，见 [`kernel/bpf/syscall.c:6163-6182`](../../linux/kernel/bpf/syscall.c#L6163-L6182) 与 [`kernel/bpf/syscall.c:2872-2946`](../../linux/kernel/bpf/syscall.c#L2872-L2946)。

权限较高也不意味着可跳过所有验证；反过来，验证通过也不意味着程序有权挂载到任意目标。

### BPF 不改变钩子的执行上下文

通常的网络程序不能睡眠；某些跟踪、LSM 或 struct_ops 路径可以支持特定的可睡眠程序，但必须满足程序类型与挂载点约束。加载标志 `BPF_F_SLEEPABLE` 被记录到 `prog->sleepable`，见 [`kernel/bpf/syscall.c:3004-3006`](../../linux/kernel/bpf/syscall.c#L3004-L3006)。这一标志不是把任意钩子转成进程上下文的开关，helper 与 kfunc 的准入检查还会限制实际操作。

### 内核 BTF 缺失不等于 BPF 不可用

当前配置没有启用 `CONFIG_DEBUG_INFO_BTF`，但 `CONFIG_BPF_SYSCALL=y`、`CONFIG_BPF_JIT=y`。因此简单 socket filter、普通 map 和不依赖内核 BTF 的跟踪程序仍可作为实验起点；不能据此推断 fentry、基于内核类型的 kfunc 或 struct_ops 也可运行。内核 BTF 导出文件的编译条件见 [`kernel/bpf/Makefile:40`](../../linux/kernel/bpf/Makefile#L40)，具体配置边界见[本章导读](README.md)。

## 小结

- 程序、map、BTF 与 link 分别管理代码、状态、类型和挂载关系。
- `BPF_PROG_LOAD` 不等于挂载，挂载也不等于立刻有事件触发。
- 程序类型和挂载类型共同限定上下文、接口与返回值。
- 用户态重定位、内核验证和 JIT 属于不同阶段，排错时应分开观察。
- 当前配置下优先从不依赖内核 BTF 的最小程序开始实验。

## 参考

- [`Documentation/bpf/bpf_design_QA.rst`](../../linux/Documentation/bpf/bpf_design_QA.rst)
- [`Documentation/bpf/classic_vs_extended.rst`](../../linux/Documentation/bpf/classic_vs_extended.rst)
- [`Documentation/bpf/libbpf/libbpf_overview.rst`](../../linux/Documentation/bpf/libbpf/libbpf_overview.rst)
