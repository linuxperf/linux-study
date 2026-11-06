# 13.5 执行引擎与 x86_64 JIT

> **本节源码**：[`include/uapi/linux/bpf.h`](../../linux/include/uapi/linux/bpf.h)、[`include/linux/filter.h`](../../linux/include/linux/filter.h)、[`kernel/bpf/core.c`](../../linux/kernel/bpf/core.c)、[`arch/x86/net/bpf_jit_comp.c`](../../linux/arch/x86/net/bpf_jit_comp.c)
> **关键函数**：`bpf_prog_select_runtime()`、`bpf_int_jit_compile()`、`do_jit()`、`emit_prologue()`、`bpf_jit_binary_pack_finalize()`、`__bpf_prog_run()`
> **前置知识**：13.3 验证器、x86_64 寄存器与函数调用约定、7.3 内存序

## 概述

BPF 指令集为不同程序类型提供共同的执行模型，但热点路径不能为每条指令都付出解释调度成本。即时编译（Just-In-Time Compilation，JIT）把验证后的 BPF 指令转换为本机机器码，让钩子通过程序入口直接执行。通用层负责选择执行方式、管理代码内存和提供统计，x86_64 后端负责寄存器映射、指令编码与调用序列。当前配置启用了 `CONFIG_BPF_JIT_ALWAYS_ON=y`，因此不能把 JIT 失败简单理解成“自动改用解释器”。

## 核心数据结构

### 指令与调用约定

基本 BPF 指令占 8 字节；宽立即数等编码使用两个指令槽。基本结构如下：

[`include/uapi/linux/bpf.h:80-86`](../../linux/include/uapi/linux/bpf.h#L80-L86)（v6.18.60）

```c
struct bpf_insn {
	__u8	code;		/* opcode */
	__u8	dst_reg:4;	/* dest register */
	__u8	src_reg:4;	/* source register */
	__s16	off;		/* signed offset */
	__s32	imm;		/* signed immediate constant */
};
```

BPF 有 `R0`～`R9` 通用寄存器和 `R10` 帧指针。`R0` 保存返回值，`R1`～`R5` 传递参数，`R6`～`R9` 为被调用者保存寄存器，`R10` 用于栈访问。调用约定和编码见 [`Documentation/bpf/standardization/instruction-set.rst`](../../linux/Documentation/bpf/standardization/instruction-set.rst)；普通 BPF 栈边界常量 `MAX_BPF_STACK=512` 见 [`include/linux/filter.h:100`](../../linux/include/linux/filter.h#L100)。不要把这个常量理解成整个内核调用栈大小。

### 程序执行字段

| 字段 | 含义 |
|---|---|
| `bpf_prog.insnsi` | BPF 指令序列 |
| `bpf_prog.bpf_func` | 最终执行入口，不一定仍指向解释器 |
| `bpf_prog.jited` / `jited_len` | 是否成功 JIT，以及机器码长度 |
| `bpf_prog.stats` | per-CPU 运行统计 |
| `bpf_prog_aux.stack_depth` | 验证分析得到的栈需求 |
| `bpf_prog_aux.jit_data` | 后端在多轮编译中使用的临时状态 |

字段定义见 [`include/linux/bpf.h:1592-1740`](../../linux/include/linux/bpf.h#L1592-L1740)。BPF 指令槽数、JIT 字节数和验证工作量是三个不同指标。

## 主流程分析

### 通用层选择与最终执行

```text
加载路径
bpf_prog_load()                          kernel/bpf/syscall.c
└── bpf_prog_select_runtime()            kernel/bpf/core.c
    ├── 判断是否必须 JIT
    ├── bpf_int_jit_compile()            arch/x86/net/bpf_jit_comp.c
    │   ├── bpf_jit_blind_constants()    kernel/bpf/core.c
    │   ├── 多轮 do_jit()
    │   └── 发布 bpf_func、jited、jited_len
    ├── bpf_prog_lock_ro()
    └── bpf_check_tail_call()

运行路径：以通用执行封装为例
钩子 → bpf_prog_run()                    include/linux/filter.h
      └── __bpf_prog_run()
          └── dispatcher → prog->bpf_func(ctx, insnsi)
```

实际通用层见 [`kernel/bpf/core.c:2562-2625`](../../linux/kernel/bpf/core.c#L2562-L2625)。已有 BPF 子程序编译准备的情况会进入最终处理，硬件卸载则走另外的编译分支；不能把所有程序都归入一条固定的本机 JIT 调用链。

若配置要求 JIT，或程序使用必须由 JIT 支持的接口，而编译后 `jited` 仍为假，加载返回 `-ENOTSUPP`。本配置的解释器代码受 `CONFIG_BPF_JIT_ALWAYS_ON` 排除，相关选择见 [`kernel/bpf/core.c:2531-2549`](../../linux/kernel/bpf/core.c#L2531-L2549) 与 [`kernel/bpf/core.c:2572-2596`](../../linux/kernel/bpf/core.c#L2572-L2596)。

### x86_64 寄存器映射

后端尽量让 BPF 调用约定接近 x86_64 的函数调用约定，从而减少搬运。例如：

| BPF 寄存器 | x86_64 对应 |
|---|---|
| `R0` | `RAX` |
| `R1`～`R5` | `RDI`、`RSI`、`RDX`、`RCX`、`R8` |
| `R6`～`R9` | `RBX`、`R13`、`R14`、`R15` |

映射表和对应的寄存器保存结构偏移见 [`arch/x86/net/bpf_jit_comp.c:181-210`](../../linux/arch/x86/net/bpf_jit_comp.c#L181-L210)。这张表解释的是后端寄存器分配，不是说 BPF 程序可以任意读写真实 CPU 的同名寄存器。

### 为什么多轮生成机器码

BPF 跳转使用指令槽偏移，x86 跳转使用字节距离；不同距离又可能选择不同长度的跳转编码。一条指令长度改变后，后续地址和跳转长度也会变化，所以后端维护 `addrs[]`，反复调用 `do_jit()` 直到长度稳定，再分配并生成最终代码。主循环见 [`arch/x86/net/bpf_jit_comp.c:3724-3799`](../../linux/arch/x86/net/bpf_jit_comp.c#L3724-L3799)。

最终代码完成后，后端处理可执行映像并填写 `bpf_func`、`jited`、`jited_len`，见 [`arch/x86/net/bpf_jit_comp.c:3802-3839`](../../linux/arch/x86/net/bpf_jit_comp.c#L3802-L3839)。代码内存存在可写构建副本与执行映像的区分，不应理解为长期开放一块普通可写内存供内核直接执行；通用提交入口见 [`kernel/bpf/core.c:1212-1231`](../../linux/kernel/bpf/core.c#L1212-L1231)。

## 关键细节 / 设计考量

### 常量加固不是验证器的替代品

常量盲化（Constant Blinding）改写立即数相关指令，降低利用可预测机器码的风险；x86 后端在正式编译前调用 `bpf_jit_blind_constants()`，见 [`arch/x86/net/bpf_jit_comp.c:3669-3681`](../../linux/arch/x86/net/bpf_jit_comp.c#L3669-L3681)。它解决的是代码生成加固问题，不负责证明访问安全，也不能替代 capability 或 LSM 检查。

### 子程序调用与尾调用不同

BPF 子程序调用具有返回关系；尾调用（Tail Call）通过程序数组切换到另一个程序，成功后不回到调用点，失败则继续执行原程序的后续指令。必须保留合理的失败路径，不能把尾调用 helper 后的代码当作永远不可达。

当前常量为 `MAX_TAIL_CALL_CNT=33`，见 [`include/linux/bpf.h:2093`](../../linux/include/linux/bpf.h#L2093)。它是限制链式执行的内核边界，不是允许程序无界递归；接口语义见 [`include/uapi/linux/bpf.h:2122-2167`](../../linux/include/uapi/linux/bpf.h#L2122-L2167)，执行方式兼容性检查见 [`kernel/bpf/core.c:2617-2624`](../../linux/kernel/bpf/core.c#L2617-L2624)。

### 运行统计由开关控制

`__bpf_prog_run()` 通过 static key 决定是否读取时钟并更新 per-CPU 的调用次数和耗时，见 [`include/linux/filter.h:732-757`](../../linux/include/linux/filter.h#L732-L757)。默认没有统计不表示程序没有执行；统计开启后也会带来开销，不宜把它当作完全无扰动的测量。

## 可观测性

依赖 `CONFIG_BPF_SYSCALL=y`、`CONFIG_BPF_JIT=y`，当前都已启用；sysctl 入口还依赖当前已启用的 `CONFIG_SYSCTL=y`。

| 入口 | 看什么与注意事项 |
|---|---|
| `bpftool prog dump xlated` | 查看验证器处理后的 BPF 指令，与 ELF 原始反汇编区分 |
| `bpftool prog dump jited` | 查看 JIT 机器码；权限和工具的反汇编支持影响可见性 |
| `bpftool prog show` | 比较 BPF 指令长度、JIT 长度与运行统计；信息查询见 [`kernel/bpf/syscall.c:4997-5282`](../../linux/kernel/bpf/syscall.c#L4997-L5282) |
| `/proc/sys/net/core/bpf_jit_enable` | 当前 `ALWAYS_ON` 配置不能将它关闭，检查见 [`net/core/sysctl_net_core.c:341-367`](../../linux/net/core/sysctl_net_core.c#L341-L367) |
| `/proc/sys/net/core/bpf_jit_harden`、`bpf_jit_kallsyms` | 加固与符号导出配置，注册见 [`net/core/sysctl_net_core.c:440-473`](../../linux/net/core/sysctl_net_core.c#L440-L473)；不要为查看地址而在生产环境降低保护 |
| `BPF_ENABLE_STATS` | 以 stats FD 的生命周期控制统计引用，见 [`kernel/bpf/syscall.c:5983-6035`](../../linux/kernel/bpf/syscall.c#L5983-L6035) |

`bpf_prog_run()` 是内联封装，不能假定能对这个名字直接挂 kprobe；JIT 入口也不是普通静态编译的 C 函数。要观测编译延迟，可以在目标内核允许探测时观察 `bpf_int_jit_compile()` 的进入与返回，而不是给每条生成指令加探针。

## 小结

- BPF 指令模型独立于 CPU，JIT 后端再落实真实寄存器、栈与机器码。
- 当前配置要求成功 JIT，编译失败不是可用解释器的保证。
- 多轮代码生成用于收敛跳转距离与指令长度。
- 子程序调用与尾调用具有不同的返回和资源约束。
- 原始指令、`xlated`、JIT 机器码和运行统计分别对应不同阶段。

## 参考

- [`Documentation/bpf/standardization/instruction-set.rst`](../../linux/Documentation/bpf/standardization/instruction-set.rst)
- [`Documentation/bpf/classic_vs_extended.rst`](../../linux/Documentation/bpf/classic_vs_extended.rst)
- [`Documentation/admin-guide/sysctl/net.rst`](../../linux/Documentation/admin-guide/sysctl/net.rst)
