# 13.6 BTF、CO-RE 与内核函数接口

> **本节源码**：[`include/uapi/linux/btf.h`](../../linux/include/uapi/linux/btf.h)、[`include/uapi/linux/bpf.h`](../../linux/include/uapi/linux/bpf.h)、[`include/linux/btf.h`](../../linux/include/linux/btf.h)、[`kernel/bpf/btf.c`](../../linux/kernel/bpf/btf.c)、[`kernel/bpf/verifier.c`](../../linux/kernel/bpf/verifier.c)、[`kernel/bpf/sysfs_btf.c`](../../linux/kernel/bpf/sysfs_btf.c)、[`tools/lib/bpf/libbpf.c`](../../linux/tools/lib/bpf/libbpf.c)、[`tools/lib/bpf/relo_core.c`](../../linux/tools/lib/bpf/relo_core.c)
> **关键函数**：`bpf_object__relocate_core()`、`bpf_core_patch_insn()`、`btf_parse_vmlinux()`、`btf_vmlinux_init()`、`check_kfunc_call()`、`register_btf_kfunc_id_set()`
> **前置知识**：13.3 验证器、13.5 指令与调用约定、ELF 和结构体布局基础

## 概述

BPF 程序需要描述 map 键值、函数参数和内核对象类型，仅有裸指令与数值偏移不足以支持这些需求。BTF 用紧凑的类型图表示类型关系，使加载器和内核能够理解对象的结构。一次编译、到处运行（Compile Once – Run Everywhere，CO-RE）进一步记录源程序访问的类型或字段，让加载器按目标类型布局修正指令。BTF 和 CO-RE 解决类型描述与布局适配问题，不承诺程序在任意内核版本上都具有相同语义。

> **当前配置边界**：`CONFIG_BPF_SYSCALL=y`，但 `CONFIG_DEBUG_INFO_BTF`、`CONFIG_DEBUG_INFO_BTF_MODULES` 均未启用。因此程序自身的 BTF 仍可通过 `BPF_BTF_LOAD` 提交，默认内核却不提供内置 BTF 和 `/sys/kernel/btf/vmlinux`。不能把这两类能力合并成一个“BTF 开关”。

## 核心数据结构

### BTF 是类型图，不是完整调试数据库

BTF 头部给出类型区和字符串区的偏移与长度，偏移相对于头部结束位置。以下为原始定义：

[`include/uapi/linux/btf.h:11-22`](../../linux/include/uapi/linux/btf.h#L11-L22)（v6.18.60）

```c
struct btf_header {
	__u16	magic;
	__u8	version;
	__u8	flags;
	__u32	hdr_len;

	/* All offsets are in bytes relative to the end of this header */
	__u32	type_off;	/* offset of type section	*/
	__u32	type_len;	/* length of type section	*/
	__u32	str_off;	/* offset of string section	*/
	__u32	str_len;	/* length of string section	*/
};
```

`struct btf_type` 用 `name_off` 指向类型名，用 `info` 编码 kind 与成员数量，再按 kind 解释尺寸或引用的类型 ID，见 [`include/uapi/linux/btf.h:31-55`](../../linux/include/uapi/linux/btf.h#L31-L55)。类型 ID 只在对应的 BTF 对象及其基类型关系中有意义，不能把两个对象中相同的数字当作同一种类型。

### CO-RE 重定位记录

[`include/uapi/linux/bpf.h:7615-7620`](../../linux/include/uapi/linux/bpf.h#L7615-L7620)（v6.18.60）

```c
struct bpf_core_relo {
	__u32 insn_off;
	__u32 type_id;
	__u32 access_str_off;
	enum bpf_core_relo_kind kind;
};
```

- `insn_off`：需要处理的指令字节偏移，不是指令序号。
- `type_id`：本地 BTF 中的根类型。
- `access_str_off`：字段或类型访问描述字符串在本地字符串区的位置。
- `kind`：字段偏移、尺寸、存在性、枚举值或类型匹配等查询类别，定义见 [`include/uapi/linux/bpf.h:7551-7565`](../../linux/include/uapi/linux/bpf.h#L7551-L7565)。

## 主流程分析

### 本地类型与目标类型之间的桥梁

```text
Clang 生成 BPF ELF
├── .BTF：程序、map、相关结构体的本地类型
└── .BTF.ext：函数、行号与 CO-RE 等扩展信息

bpf_object__load()                         tools/lib/bpf/libbpf.c
└── bpf_object_load()
    └── bpf_object_prepare()
        ├── bpf_object__load_vmlinux_btf()
        ├── bpf_object__resolve_externs()
        ├── bpf_object__relocate()
        │   └── bpf_object__relocate_core()
        │       ├── bpf_core_resolve_relo()
        │       └── bpf_core_patch_insn()   tools/lib/bpf/relo_core.c
        ├── bpf_object__sanitize_and_load_btf()
        └── bpf_object__create_maps()
    后续：bpf_object__load_progs()
```

这描述本章使用的普通 libbpf 对象加载路径，次序见 [`tools/lib/bpf/libbpf.c:8658-8720`](../../linux/tools/lib/bpf/libbpf.c#L8658-L8720)。轻量加载器生成等路径存在差异，不应把“CO-RE 永远只有一个用户态入口”作为接口保证。

`bpf_object__relocate_core()` 遍历扩展记录、定位程序与指令、解析目标结果，再调用指令修补函数，见 [`tools/lib/bpf/libbpf.c:5976-6085`](../../linux/tools/lib/bpf/libbpf.c#L5976-L6085)。目标布局变化时，加载期可以修正相关偏移，而不要求用户重新按目标结构体定义编译整个程序。

### 内核 BTF 从哪里来

启用 `CONFIG_DEBUG_INFO_BTF` 时，构建过程借助 pahole 把 DWARF 类型信息转换为 BTF，选项约束见 [`lib/Kconfig.debug:377-391`](../../linux/lib/Kconfig.debug#L377-L391)。内核通过 `btf_parse_vmlinux()` 解析内置类型数据，见 [`kernel/bpf/btf.c:6308-6344`](../../linux/kernel/bpf/btf.c#L6308-L6344)。

sysfs 路径由 `btf_vmlinux_init()` 在 `btf` 目录下建立 `vmlinux` 二进制属性，见 [`kernel/bpf/sysfs_btf.c:46-69`](../../linux/kernel/bpf/sysfs_btf.c#L46-L69)。没有内核 BTF 时，单独安装 bpftool 不会补出当前运行内核缺失的类型图。

对于只需加载期布局适配的 CO-RE，libbpf 也接受显式指定、与目标内核匹配的 BTF 文件；相关分支见 [`tools/lib/bpf/libbpf.c:5992-6003`](../../linux/tools/lib/bpf/libbpf.c#L5992-L6003)。但外部文件不能代替内核内部对 fentry 目标、kfunc 或 struct_ops 类型的认识。

### helper 与 kfunc：两种接口契约

helper 使用数值 ID 和 `struct bpf_func_proto` 描述接口。以 map 查询为例，原型规定常量 map 指针、键指针和可空返回值，见 [`kernel/bpf/helpers.c:49-56`](../../linux/kernel/bpf/helpers.c#L49-L56)。程序类型通过 `get_func_proto()` 决定可用集合，验证器随后检查调用约束。

内核函数接口（kernel function，kfunc）借助 BTF 描述函数原型，并通过注册集合及标志附加安全语义。它不是允许 BPF 直接调用任意有符号名的内核函数；调用检查入口见 [`kernel/bpf/verifier.c:14165-14335`](../../linux/kernel/bpf/verifier.c#L14165-L14335)，注册接口见 [`include/linux/btf.h:582-584`](../../linux/include/linux/btf.h#L582-L584)。

| 标志 | 验证含义 |
|---|---|
| `KF_ACQUIRE` | 返回需要管理的对象引用 |
| `KF_RELEASE` | 释放相应引用，后续不能继续按有效引用使用 |
| `KF_RET_NULL` | 返回值可能为空，需要建立非空分支事实 |
| `KF_SLEEPABLE` | 调用可能睡眠，只能出现在允许的可睡眠程序中 |

定义与说明见 [`include/linux/btf.h:18-20`](../../linux/include/linux/btf.h#L18-L20)、[`include/linux/btf.h:70`](../../linux/include/linux/btf.h#L70) 和 [`Documentation/bpf/kfuncs.rst`](../../linux/Documentation/bpf/kfuncs.rst)。接口是否存在、是否向该程序类型开放、是否满足引用和上下文约束，是三个不同问题。

## 关键细节 / 设计考量

### CO-RE 不生成稳定的内核内部 ABI

字段存在性重定位可以让程序选择兼容分支，但无法保证字段含义不变，也无法创造目标内核没有的 helper、kfunc、程序类型或挂载点。应分别检查“能找到类型”“能完成重定位”“能通过验证”“业务含义正确”。

`vmlinux.h` 是从 BTF 生成的 C 类型声明，方便编译阶段描述内核类型，不是运行时适配器。只有相应访问携带了 CO-RE 信息并在加载期正确处理，布局变化才有机会被适配。

### 程序 BTF 不要求内核也有完整 BTF

普通 map 的键值 BTF 在 `map_create()` 中通过用户提交的 BTF FD 获取，见 [`kernel/bpf/syscall.c:1536-1562`](../../linux/kernel/bpf/syscall.c#L1536-L1562)。因此本章最小实验可以用现代 BTF map 定义，却不访问内核内部结构体。区别在于：程序自带类型足以描述自己的 map，不足以推导运行内核的 `task_struct` 布局。

## 可观测性

| 入口 | 能力与当前配置 |
|---|---|
| `bpftool btf show`、按 BTF ID dump | 查询内核中已经加载的 BTF 对象；`CONFIG_BPF_SYSCALL=y` 下可用，内核信息接口见 [`kernel/bpf/btf.c`](../../linux/kernel/bpf/btf.c) |
| `bpftool btf dump file <对象文件> format raw` | 离线检查 ELF 自带类型，不依赖运行内核 BTF；解析实现见 [`tools/bpf/bpftool/btf.c`](../../linux/tools/bpf/bpftool/btf.c) |
| `/sys/kernel/btf/vmlinux` | 依赖 `CONFIG_DEBUG_INFO_BTF`，当前未启用，不能预期该文件存在 |
| `/sys/kernel/btf/<module>` | 依赖模块 BTF，当前未启用 `CONFIG_DEBUG_INFO_BTF_MODULES`；相关模块 BTF 处理见 [`kernel/bpf/btf.c:6348-6444`](../../linux/kernel/bpf/btf.c#L6348-L6444) |
| libbpf 调试日志 | 区分没有目标 BTF、类型候选不匹配、字段不存在与指令修补失败；关键日志见 [`tools/lib/bpf/libbpf.c:5992-6083`](../../linux/tools/lib/bpf/libbpf.c#L5992-L6083) |

`bpftool feature probe` 是用户态探测结果汇总，不应把它当作单个内核布尔配置的直接打印。目标内核、权限、容器限制和实际探测方式都会影响结果，工具实现见 [`tools/bpf/bpftool/feature.c`](../../linux/tools/bpf/bpftool/feature.c)。

## 小结

- BTF 描述类型关系，CO-RE 使用本地与目标类型信息完成加载期适配。
- 程序 BTF、内核 BTF、模块 BTF 和外部目标 BTF 各有不同作用。
- 当前配置可以加载自带 BTF 的简单对象，但没有默认的内核 BTF 导出。
- helper 与 kfunc 都受程序类型和验证规则约束，不能据函数名任意调用。
- CO-RE 解决部分布局差异，不保证跨版本行为、权限和接口都兼容。

## 参考

- [`Documentation/bpf/btf.rst`](../../linux/Documentation/bpf/btf.rst)
- [`Documentation/bpf/libbpf/libbpf_build.rst`](../../linux/Documentation/bpf/libbpf/libbpf_build.rst)
- [`Documentation/bpf/libbpf/libbpf_overview.rst`](../../linux/Documentation/bpf/libbpf/libbpf_overview.rst)
- [`Documentation/bpf/kfuncs.rst`](../../linux/Documentation/bpf/kfuncs.rst)
