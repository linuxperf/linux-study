# 13.8 跟踪程序：tracepoint、kprobe 与 fentry

> **本节源码**：[`kernel/trace/bpf_trace.c`](../../linux/kernel/trace/bpf_trace.c)、[`kernel/events/core.c`](../../linux/kernel/events/core.c)、[`include/trace/bpf_probe.h`](../../linux/include/trace/bpf_probe.h)、[`include/trace/perf.h`](../../linux/include/trace/perf.h)、[`kernel/bpf/syscall.c`](../../linux/kernel/bpf/syscall.c)、[`include/trace/events/sched.h`](../../linux/include/trace/events/sched.h)
> **关键函数**：`perf_event_attach_bpf_prog()`、`trace_call_bpf()`、`bpf_probe_register()`、`__bpf_trace_run()`、`bpf_tracing_prog_attach()`
> **前置知识**：13.3 验证器、13.6 BTF、13.7 挂载机制、5.1 执行现场

## 概述

跟踪程序把内核事件转成可过滤、聚合和关联的数据，但不同挂载机制交给 BPF 的并不是同一种现场。普通 tracepoint 处理已经定义好的事件记录，raw tracepoint 面向原始参数，kprobe 以探针现场为基础，fentry/fexit 则借助 BTF 和 trampoline 适配函数调用。选择挂载点时，应先确定需要观察的语义，再权衡稳定性、可用性和开销。把这些机制统一称为“给函数加 BPF”，容易导致参数解释和生命周期错误。

## 核心数据结构

| 机制 | 典型程序类型 | 上下文及约束 |
|---|---|---|
| 普通 tracepoint | `BPF_PROG_TYPE_TRACEPOINT` | 事件记录布局；从目标运行内核的 `format` 核对字段 |
| raw tracepoint | `BPF_PROG_TYPE_RAW_TRACEPOINT` | `struct bpf_raw_tracepoint_args` 风格的原始参数数组 |
| kprobe / kretprobe | `BPF_PROG_TYPE_KPROBE` | 架构相关寄存器现场；返回探针与入口探针的可用信息不同 |
| fentry / fexit | `BPF_PROG_TYPE_TRACING` | 与目标 BTF 原型关联的参数槽；fexit 还可观察返回结果，受目标支持约束 |

程序类型登记见 [`include/linux/bpf_types.h:39-52`](../../linux/include/linux/bpf_types.h#L39-L52)，类型专用验证操作见 [`kernel/trace/bpf_trace.c:1356-1365`](../../linux/kernel/trace/bpf_trace.c#L1356-L1365)、[`kernel/trace/bpf_trace.c:1462-1471`](../../linux/kernel/trace/bpf_trace.c#L1462-L1471) 和 [`kernel/trace/bpf_trace.c:1778-1803`](../../linux/kernel/trace/bpf_trace.c#L1778-L1803)。表中不包含所有变体，例如基于 BTF 的 raw tracepoint 还属于 tracing 路径，不能与普通 raw tracepoint 完全等同。

## 主流程分析

### 普通 tracepoint 与 perf event

```text
挂载侧
perf_event_attach_bpf_prog()             kernel/trace/bpf_trace.c
├── 检查事件与程序
├── 复制并更新事件的 BPF 程序数组
└── 发布 event->tp_event->prog_array

事件侧
静态事件触发 → perf 事件处理             kernel/events/core.c
└── trace_call_bpf()                     kernel/trace/bpf_trace.c
    ├── 递归保护
    ├── RCU 读取程序数组
    └── bpf_prog_run_array(..., bpf_prog_run)
```

挂载时建立数组关系的实现见 [`kernel/trace/bpf_trace.c:1906-1949`](../../linux/kernel/trace/bpf_trace.c#L1906-L1949)，事件处理检查并调用 BPF 的位置见 [`kernel/events/core.c:10989-11002`](../../linux/kernel/events/core.c#L10989-L11002)。`trace_call_bpf()` 在 RCU 保护下遍历程序数组，并在递归冲突时增加遗漏统计，见 [`kernel/trace/bpf_trace.c:109-154`](../../linux/kernel/trace/bpf_trace.c#L109-L154)。

这里 BPF 看到的是该事件约定的记录，不是自动获得“触发点所有 C 局部变量”。例如 `sched:sched_switch` 的记录字段由 [`include/trace/events/sched.h:220-269`](../../linux/include/trace/events/sched.h#L220-L269) 定义，记录中的状态值还可能经过事件专用编码。

### raw tracepoint：绕过事件记录布局

raw tracepoint 查找原始事件映射并注册回调；`bpf_probe_register()` 会先核对程序访问的参数范围，见 [`kernel/trace/bpf_trace.c:2128-2157`](../../linux/kernel/trace/bpf_trace.c#L2128-L2157)。回调包装把参数打包成 `u64` 数组，再交给 `__bpf_trace_run()`：

```text
目标 tracepoint
└── 生成的 BPF probe 包装                include/trace/bpf_probe.h
    └── bpf_trace_runN()                 kernel/trace/bpf_trace.c
        └── __bpf_trace_run()
            ├── 程序级递归保护和 cookie
            ├── RCU 读侧保护
            └── bpf_prog_run(prog, args)
```

包装宏与执行实现见 [`include/trace/bpf_probe.h`](../../linux/include/trace/bpf_probe.h) 和 [`kernel/trace/bpf_trace.c:2059-2126`](../../linux/kernel/trace/bpf_trace.c#L2059-L2126)。raw 方式减少了对事件记录构造的依赖，但参数次序和对象生命周期仍必须按 tracepoint 的 `TP_PROTO` 分析；`format` 文件不能直接代替原始参数说明。

### kprobe：从寄存器现场推导参数

kprobe 的参数读取依赖架构、目标函数原型和入口位置。用户态库中的寄存器访问宏只是适配工具，不会替读者证明选中的符号和参数语义正确。被内联的函数可能没有独立入口，`notrace`、探针黑名单、编译优化和当前配置也会影响可挂载性。

本配置启用 `CONFIG_KPROBES=y`、`CONFIG_KPROBE_EVENTS=y`、`CONFIG_UPROBE_EVENTS=y`，但未启用 `CONFIG_KPROBE_EVENTS_ON_NOTRACE`。程序类型准入与可访问现场由 [`kernel/trace/bpf_trace.c:1356-1365`](../../linux/kernel/trace/bpf_trace.c#L1356-L1365) 的操作集约束；具体加载还须通过 perf 或多探针 link 等挂载路径，不能只依函数名判断。

### fentry/fexit：通过 BTF 对接目标函数

基于 BTF 的函数跟踪用 `attach_btf_id` 描述目标，加载阶段先核对目标类型，挂载时再建立 trampoline。tracing 程序在 [`kernel/bpf/syscall.c:5731-5755`](../../linux/kernel/bpf/syscall.c#L5731-L5755) 分流到相应实现，调用桥见上一节。

fentry/fexit 不要求按 kprobe 风格手工从 `pt_regs` 还原全部参数，但仍受函数是否可跟踪、原型是否受支持和内核配置限制。当前未启用 `CONFIG_DEBUG_INFO_BTF`，所以本节对这条路径只做源码分析，不提供默认可运行的 fentry 实验。

## 关键细节 / 设计考量

### 稳定的是哪一层

静态 tracepoint 通常比内部函数名更接近子系统事件语义，但也不应无条件当作永久用户 ABI。kprobe 与 fentry 依赖内部函数及其调用关系，源码重构后即使还能加载，事件含义和触发频率也可能变化。

跟踪前必须区分：观察的是“某个操作的语义事件”，还是“当前版本某个实现函数的进入”。对调度延迟分析而言，前者与后者往往不是同一测量区间。

### 递归遗漏与传输丢失要分开

`trace_call_bpf()` 和 raw 路径都有递归保护，见 [`kernel/trace/bpf_trace.c:115-127`](../../linux/kernel/trace/bpf_trace.c#L115-L127) 与 [`kernel/trace/bpf_trace.c:2067-2070`](../../linux/kernel/trace/bpf_trace.c#L2067-L2070)。由于递归保护没有运行程序，与程序已运行但 ringbuf 预留失败，是不同的丢失来源；过滤掉的事件又是第三种情况。

高频路径应优先在 map 中聚合，只发送必要事件。给每次系统调用或调度切换都输出一行文本，会把测量对象变成输出通道的吞吐和锁竞争。

## 可观测性

以下跟踪能力依赖 `CONFIG_BPF_EVENTS=y`、`CONFIG_PERF_EVENTS=y`，当前已启用；tracefs 还依赖当前已启用的 `CONFIG_TRACING=y`。

| 入口 | 观察方法 |
|---|---|
| `/sys/kernel/tracing/events/sched/sched_switch/format` | 核对普通 tracepoint 的实际记录布局；静态字段来源见 [`include/trace/events/sched.h:220-269`](../../linux/include/trace/events/sched.h#L220-L269) |
| 同目录的 `sched_process_exec/format` | 进程执行事件的记录与动态字符串布局，定义见 [`include/trace/events/sched.h:424-449`](../../linux/include/trace/events/sched.h#L424-L449) |
| `bpftool prog show`、程序 FD 的 `fdinfo` | 观察 `run_cnt`、`run_time_ns` 与 `recursion_misses`；生成位置见 [`kernel/bpf/syscall.c:2505-2533`](../../linux/kernel/bpf/syscall.c#L2505-L2533) |
| `bpftool link show` | 对照 link 的目标与 cookie；传统 perf/其他挂载还需核对其持有的对象 |
| 应用的 ringbuf 失败计数 | 与递归遗漏分开统计，不能由 `recursion_misses` 推导全部事件丢失 |

`run_time_ns` 和 `run_cnt` 需要运行统计开关；`recursion_misses` 的递增路径另见 [`kernel/bpf/syscall.c:2460-2473`](../../linux/kernel/bpf/syscall.c#L2460-L2473)。不要把计数器为零统一解释为没有触发；还应检查统计开关、挂载关系、过滤条件和探测目标是否真的执行。

## 小结

- 普通 tracepoint、raw tracepoint、kprobe 与 fentry 使用不同的上下文契约。
- 普通事件记录看 `format`，原始参数看 `TP_PROTO`，函数参数看目标原型与架构。
- 内核 BTF 缺失不阻止全部跟踪，但限制了基于内核类型的函数挂载。
- 程序未执行、业务过滤和事件传输失败是不同的观测结果。
- 先选事件语义，再设计最小聚合与传输方案，避免输出放大扰动。

## 参考

- [`Documentation/trace/tracepoints.rst`](../../linux/Documentation/trace/tracepoints.rst)
- [`Documentation/trace/kprobes.rst`](../../linux/Documentation/trace/kprobes.rst)
- [`Documentation/bpf/bpf_design_QA.rst`](../../linux/Documentation/bpf/bpf_design_QA.rst)
