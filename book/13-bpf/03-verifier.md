# 13.3 验证器：类型、边界与状态探索

> **本节源码**：[`kernel/bpf/verifier.c`](../../linux/kernel/bpf/verifier.c)、[`include/linux/bpf_verifier.h`](../../linux/include/linux/bpf_verifier.h)、[`include/linux/bpf.h`](../../linux/include/linux/bpf.h)、[`kernel/bpf/tnum.c`](../../linux/kernel/bpf/tnum.c)
> **关键函数**：`bpf_check()`、`check_cfg()`、`do_check()`、`check_mem_access()`、`check_helper_call()`、`check_kfunc_call()`、`is_state_visited()`
> **前置知识**：13.2 程序加载、C 整数与指针运算、控制流图基础

## 概述

BPF 程序的机器码最终会在内核环境执行，仅检查指令编码正确远远不够。验证器（verifier）在加载期对程序做抽象解释（Abstract Interpretation），跟踪每条可达路径上的寄存器、栈、指针范围和资源状态。它需要确认内存访问、函数调用和退出行为满足程序类型的约束，并在有限的验证工作量内完成检查。验证通过不是“逐条指令都很简单”，而是对所有纳入分析的可达行为建立了安全约束。

## 核心数据结构

### 寄存器状态比 C 类型更细

`struct bpf_reg_state` 不仅区分标量与指针，还记录指针来源、偏移与数值范围，见 [`include/linux/bpf_verifier.h:37-134`](../../linux/include/linux/bpf_verifier.h#L37-L134)。

| 字段 | 分析含义 |
|---|---|
| `type` | 未初始化、标量、上下文、map value、包数据、栈或带 BTF 类型的指针 |
| `off` | 指针的固定偏移；某些关联标量也使用该字段记录差值 |
| `var_off` | 用 `struct tnum` 表示按位已知与未知的信息 |
| `smin_value` / `smax_value` | 64 位有符号值范围 |
| `umin_value` / `umax_value` | 64 位无符号值范围 |
| `s32_min_value` 等 | 32 位子寄存器范围，不能与 64 位范围混为一谈 |
| `id` | 关联同源指针或标量，让一个分支检查可以收紧相关副本的状态 |
| `ref_obj_id` | 跟踪需要释放或转移的对象引用 |

后两个字段的语义见 [`include/linux/bpf_verifier.h:134-196`](../../linux/include/linux/bpf_verifier.h#L134-L196)。`struct bpf_verifier_state` 管理调用帧等路径状态，见 [`include/linux/bpf_verifier.h:364-485`](../../linux/include/linux/bpf_verifier.h#L364-L485)。

指针类型的实际枚举在 [`include/linux/bpf.h:961-1012`](../../linux/include/linux/bpf.h#L961-L1012)。例如 `PTR_TO_MAP_VALUE_OR_NULL` 是带 `PTR_MAYBE_NULL` 标志的 map value 指针，而不是可直接解引用的普通地址。

## 主流程分析

### 验证阶段总览

```text
bpf_check()                                kernel/bpf/verifier.c
├── 选择程序类型对应的 bpf_verifier_ops
├── 处理日志、FD 数组、BTF 与子程序信息
├── resolve_pseudo_ldimm64()
├── check_cfg()
├── 计算遍历次序、活跃性与强连通分量
├── check_attach_btf_id()
├── do_check_main()
│   └── do_check_common()
│       └── do_check()
│           ├── is_state_visited()
│           └── do_check_insn()
│               ├── check_mem_access()
│               ├── check_helper_call()
│               └── check_kfunc_call()
├── do_check_subprogs()
├── 栈深检查、优化、上下文访问转换与调用修正
└── 保存验证统计和对象依赖
```

这棵树按阶段概括主流程，不表示所有指令处理分支只有一层调用。实际入口顺序见 [`kernel/bpf/verifier.c:25203-25484`](../../linux/kernel/bpf/verifier.c#L25203-L25484)。本版本包含循环、迭代器与较复杂的控制流处理，不能照搬早期文档“验证器拒绝所有循环”的概括。

### 从未知值到可证明范围

验证器不会运行真实输入，而是传播可能值的集合。读取一个字节后，值的高位已知为零；与常量比较后，两个分支获得不同的上下界。固定偏移、变量偏移和访问宽度必须共同落在对象允许的范围内，检查入口见 [`kernel/bpf/verifier.c:7688-7905`](../../linux/kernel/bpf/verifier.c#L7688-L7905)。

`tnum` 用已知位与未知位补充区间信息。例如“值小于 64”描述上界，“低两位为 0”描述对齐；只有区间并不足以表达后者。相关运算实现在 [`kernel/bpf/tnum.c`](../../linux/kernel/bpf/tnum.c)，按位表示与范围结合的说明见 [`Documentation/bpf/verifier.rst`](../../linux/Documentation/bpf/verifier.rst)。

### map 查询后为什么必须判空

`bpf_map_lookup_elem()` 的原型把结果声明为 `RET_PTR_TO_MAP_VALUE_OR_NULL`，见 [`kernel/bpf/helpers.c:49-56`](../../linux/kernel/bpf/helpers.c#L49-L56)。非空分支可将该值收紧为可访问的 map value 指针，并结合 map 的 `value_size` 限定范围；判空不是为了让 C 编译器满意，而是给验证器提供路径事实。

以下是分析用伪代码，不是内核源码，也不是完整可加载程序：

```text
p = map_lookup(map, &key)
if p == NULL:
    return 0
# 只有这个分支可以在 value_size 范围内使用 p
value = load_u64(p + 0)
```

相同原则也适用于包边界检查：计算 `data + 长度` 后与 `data_end` 比较，为后续访问建立安全范围。改变包布局的 helper 可能使旧的包指针证明失效，需要重新取指针、重新检查。

### helper、kfunc 与资源约束

helper 由程序类型的 `get_func_proto()` 决定是否开放；验证器检查参数类型、可访问内存、常量要求和返回类型，见 [`kernel/bpf/verifier.c:11680-11840`](../../linux/kernel/bpf/verifier.c#L11680-L11840)。函数能够在内核中找到，并不意味着 BPF 程序可以调用。

kfunc 的检查还需要 BTF 和已注册接口集合，见 [`kernel/bpf/verifier.c:14165-14335`](../../linux/kernel/bpf/verifier.c#L14165-L14335)。对返回引用、锁、ringbuf 预留记录等资源，验证器不仅检查“地址有效”，还检查退出前是否释放、提交、丢弃或按允许的方式转移。引用泄漏检查入口为 [`kernel/bpf/verifier.c:11496-11553`](../../linux/kernel/bpf/verifier.c#L11496-L11553)。

### 状态剪枝与复杂度上限

路径分支会产生不同状态；若到达同一指令的新状态已经被先前安全状态覆盖，就没有必要重新分析后续路径。`do_check()` 在剪枝点调用 `is_state_visited()`，命中后进入该路径的退出处理，见 [`kernel/bpf/verifier.c:20642-20669`](../../linux/kernel/bpf/verifier.c#L20642-L20669)。状态剪枝（State Pruning）是控制验证成本的关键，不是简单按指令地址去重。

同一条指令可能在不同状态下分析很多次，因此 `insn_processed` 不等于程序长度。验证工作量超出 `BPF_COMPLEXITY_LIMIT_INSNS` 时返回 `-E2BIG`，当前常量为 1,000,000，见 [`include/linux/bpf.h:2092`](../../linux/include/linux/bpf.h#L2092) 与 [`kernel/bpf/verifier.c:20640-20647`](../../linux/kernel/bpf/verifier.c#L20640-L20647)。一个短程序也可能因为状态爆炸而被拒绝。

## 关键细节 / 设计考量

### 循环不是只看次数是否写成常量

本版本允许能够满足验证规则的循环，但分支、标量范围与资源状态会影响终止判断和验证成本。状态比较中还包含无限循环检测，见 [`kernel/bpf/verifier.c:20073-20095`](../../linux/kernel/bpf/verifier.c#L20073-L20095)。不能把一个很大的有限循环直接等同于可被高效验证，也不能以 C 源码形式代替对生成指令的分析。

### 验证通过后的指令可能变化

`bpf_check()` 后段执行死代码处理、上下文访问转换、调用修正与子寄存器处理，见 [`kernel/bpf/verifier.c:25354-25400`](../../linux/kernel/bpf/verifier.c#L25354-L25400)。所以定位错误时，应区分原始对象指令、验证日志中的指令和最终 `xlated` 指令，不能拿不同阶段的编号直接对应。

### 程序返回值也是类型约束的一部分

验证器通过 `check_return_code()` 检查退出时的返回范围，见 [`kernel/bpf/verifier.c:17711-17890`](../../linux/kernel/bpf/verifier.c#L17711-L17890)。这也是不能只修改 ELF section 名称就把某类程序转换为另一类程序的原因之一。

## 可观测性

验证日志通过 `BPF_PROG_LOAD` 的 `log_level`、`log_buf`、`log_size` 返回，而不是默认打印到 `dmesg`。初始化与日志最终长度回传见 [`kernel/bpf/verifier.c:25248-25255`](../../linux/kernel/bpf/verifier.c#L25248-L25255) 与 [`kernel/bpf/verifier.c:25401-25413`](../../linux/kernel/bpf/verifier.c#L25401-L25413)；依赖 `CONFIG_BPF_SYSCALL=y`，当前已启用。

| 观测项 | 如何解读 |
|---|---|
| 失败指令与寄存器类型 | 优先确认指针来源、是否判空、偏移是否收紧，而不是只看最后一行错误 |
| `processed ... insns` | 验证分析的工作量，不是运行时执行次数 |
| `max_states_per_insn`、`total_states`、`peak_states` | 判断分支和状态探索成本；统计打印见 [`kernel/bpf/verifier.c:24011-24034`](../../linux/kernel/bpf/verifier.c#L24011-L24034) |
| `verified_insns` | 成功加载后的验证工作量，保存与查询见 [`kernel/bpf/verifier.c:25399-25401`](../../linux/kernel/bpf/verifier.c#L25399-L25401)、[`kernel/bpf/syscall.c:5053-5057`](../../linux/kernel/bpf/syscall.c#L5053-L5057) |

加载工具的调试输出可同时包含 libbpf 和验证器信息，但 CO-RE 在用户态准备阶段失败时，内核验证器可能根本尚未接收到该程序。排错应先确定失败阶段。

## 小结

- 验证器跟踪指针来源、整数范围、按位信息、调用帧与资源状态。
- 判空、边界检查和接口配对是给验证器建立可证明事实。
- 状态剪枝控制路径探索成本，验证工作量不同于静态指令数。
- 当前内核不能概括为“不支持循环”，但循环仍受安全与复杂度约束。
- 验证日志用于解释拒绝原因，`verified_insns` 用于观察成功程序的验证成本。

## 参考

- [`Documentation/bpf/verifier.rst`](../../linux/Documentation/bpf/verifier.rst)
- [`Documentation/bpf/kfuncs.rst`](../../linux/Documentation/bpf/kfuncs.rst)
- [`tools/testing/selftests/bpf/progs`](../../linux/tools/testing/selftests/bpf/progs)
