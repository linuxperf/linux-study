# 0.3 源码布局与 clangd 导航

> **本节源码**：[`init/main.c`](../../linux/init/main.c)、[`kernel/Makefile`](../../linux/kernel/Makefile)、[`scripts/clang-tools/gen_compile_commands.py`](../../linux/scripts/clang-tools/gen_compile_commands.py)、[`Makefile`](../../linux/Makefile)
> **关键函数**：`start_kernel()`、`sched_init()`、`version_proc_show()`、`process_line()`（数据库生成脚本）
> **前置知识**：[0.1 阅读方法与分析边界](01-overview.md)、[0.2 实验环境与只读构建](02-environment.md)

## 概述

内核里的一个名字可能对应宏、内联函数、架构实现或操作表回调，全文搜索并不能直接确定当前构建使用哪一个。可靠导航需要把源码目录、构建规则和编译参数结合起来。语言服务器协议（Language Server Protocol，LSP）让编辑器能向 clangd 查询定义、类型、引用和调用关系；它的准确性依赖匹配的编译数据库（Compilation Database）与生成头文件。本节以启动入口为练习对象，建立“先语义导航，再检查构建条件，最后人工核实”的流程。

## 核心信息：目录、构建规则与编译命令

### 目录地图

| 目录或文件 | 主要用途 | 导航时的注意事项 |
|---|---|---|
| [`init/`](../../linux/init) | 通用初始化与启动配置 | 不是全部架构入口的所在位置 |
| [`arch/x86/`](../../linux/arch/x86) | x86 平台、入口、页表及架构实现 | 与通用层交替阅读，不把其他架构结果混入 x86_64 主线 |
| [`kernel/`](../../linux/kernel) | 调度、同步、时间、跟踪等核心机制 | 子系统常有自己的内部头文件 |
| [`mm/`](../../linux/mm) | 内存管理 | 分配、映射、回收等路径需要按问题切分 |
| [`fs/`](../../linux/fs)、[`block/`](../../linux/block)、[`net/`](../../linux/net) | 文件系统、块设备层和网络协议栈 | 通用层与具体实现往往通过操作表连接 |
| [`drivers/`](../../linux/drivers) | 设备驱动及相关框架 | 驱动是否编译受配置、总线与设备类型影响 |
| [`include/linux/`](../../linux/include/linux) | 通用内核头文件 | 定义、声明与内联实现可能同时存在 |
| [`include/uapi/`](../../linux/include/uapi)、[`arch/x86/include/uapi/`](../../linux/arch/x86/include/uapi) | 用户空间 API（Userspace API，UAPI） | 面向用户程序的接口，不等于所有内核内部类型 |
| [`Documentation/`](../../linux/Documentation) | 设计、接口、工具与使用文档 | 先看仓库同版本文档，再参考外部资料 |
| [`Kconfig`](../../linux/Kconfig)、[`Makefile`](../../linux/Makefile) | 配置入口与构建入口 | 与各子目录的同名文件一起决定实际构建范围 |
| [`MAINTAINERS`](../../linux/MAINTAINERS)、[`tools/`](../../linux/tools) | 子系统维护范围、配套工具与测试 | 可用于找相邻实现和验证方法，不代表稳定的内核内部 API |

`include/generated/`、`include/config/` 和架构生成头文件应从构建输出取得，不要手工补到仓库原树。UAPI 的导出边界可进一步参考 [`Documentation/kbuild/headers_install.rst`](../../linux/Documentation/kbuild/headers_install.rst)。

### 三类信息怎样配合

配置决定机制是否可用，构建规则决定哪些对象参与编译，编译命令决定每个源文件如何被解析。例如：

[`kernel/Makefile:67-70`](../../linux/kernel/Makefile#L67-L70)（v6.18.60）

```makefile
obj-$(CONFIG_SMP) += smp.o
ifneq ($(CONFIG_SMP),y)
obj-y += up.o
endif
```

当前 `CONFIG_SMP=y`，因此这段规则选择 `smp.o`，而非 `up.o`。一般而言，`obj-y` 列出内建对象，`obj-m` 列出模块对象；配置为 `n` 时，对应的条件对象不进入该列表。具体链接与复合对象规则见 [`Documentation/kbuild/makefiles.rst`](../../linux/Documentation/kbuild/makefiles.rst)。

还要区分 `.config` 中的 `y`、`m`、`n`：支持模块的配置取 `m`，表示构建模块，不代表模块已经加载；布尔配置没有 `m` 状态。配置类型和依赖见 [`Documentation/kbuild/kconfig-language.rst:55-68`](../../linux/Documentation/kbuild/kconfig-language.rst#L55-L68)。

### 编译数据库包含什么

clangd 需要知道源文件所属的翻译单元（Translation Unit）及其真实编译参数，包括目标架构、宏、头文件搜索路径和工作目录。内核生成脚本从构建的 `.cmd` 文件提取命令，匹配规则见 [`scripts/clang-tools/gen_compile_commands.py:21-22`](../../linux/scripts/clang-tools/gen_compile_commands.py#L21-L22)。生成的每条记录包含：

| JSON 字段 | 含义 |
|---|---|
| `directory` | 执行编译命令的工作目录；独立构建时是输出目录 |
| `file` | 源文件的规范化绝对路径 |
| `command` | 包含宏、路径与架构参数的实际编译命令 |

字段构造见 [`scripts/clang-tools/gen_compile_commands.py:169-183`](../../linux/scripts/clang-tools/gen_compile_commands.py#L169-L183)。脚本会检查源文件存在性，不是把源码目录的所有 `.c` 文件无条件写入数据库。

## 主流程分析

### 1. 在构建所在环境使用数据库

本章 [0.2](02-environment.md) 生成的数据库位于 `/var/tmp/linux-study-work/build/compile_commands.json`，指向仓库外源码副本和虚拟机内的绝对路径。最直接的方式是在虚拟机内运行编辑器与 clangd，或通过远程编辑让 clangd 仍在虚拟机执行。

在编辑器中打开 `/var/tmp/linux-study-work/src`，让其启动 clangd 时附加 `--compile-commands-dir=/var/tmp/linux-study-work/build`。可以把缓存放在工作区，并用 `--background-index` 开启后台索引。以下是启动参数示意，不是需要另外常驻启动的服务命令；多数编辑器由 LSP 客户端管理 clangd：

```bash
XDG_CACHE_HOME=/var/tmp/linux-study-work/cache \
clangd --compile-commands-dir=/var/tmp/linux-study-work/build \
    --background-index
```

不要把数据库直接拷到宿主机后就期待可用：其中的源文件路径、工作目录和生成头文件路径在宿主机上可能都不存在。也不要在 `linux/` 内建立软链接或写入 `.clangd` 来规避只读约定。

若只阅读仓库原文件，可以把它作为带行号的对照窗口；语义导航在副本中完成，再按同版本的相对路径回到原文件。副本一旦加入实验补丁，就必须重新核对行号，不能继续用它冒充书稿分析基线。

### 2. 检查数据库，再检查 clangd

以下命令在虚拟机内执行，使用 0.2 的工作目录；它们不修改仓库源码：

```bash
python3 - <<'PY'
import json
from pathlib import Path
out = Path('/var/tmp/linux-study-work/build')
src = Path('/var/tmp/linux-study-work/src')
db = json.loads((out / 'compile_commands.json').read_text())
print('entries:', len(db))
entry = next(item for item in db
             if Path(item['file']) == src / 'init/main.c')
for key in ('directory', 'file', 'command'):
    print(key + ':', entry[key])
PY
XDG_CACHE_HOME=/var/tmp/linux-study-work/cache \
clangd --check=/var/tmp/linux-study-work/src/init/main.c \
    --compile-commands-dir=/var/tmp/linux-study-work/build
```

没有找到 `init/main.c` 时，先检查构建是否完成以及数据库是否来自正确输出目录。`clangd --check` 的诊断只是解析检查，不代替内核编译成功，也不证明所有跨文件引用已索引。

顶层数据库目标依赖 `vmlinux.a`、相应库和模块列表，见 [`Makefile:2110-2118`](../../linux/Makefile#L2110-L2118)。只运行配置或 `prepare` 并不能自动得到完整数据库；调用这个目标本身也可能触发编译。

### 3. 从 `start_kernel()` 建立第一条导航链

先打开 [`init/main.c:869-870`](../../linux/init/main.c#L869-L870)，再依次进行：

1. **悬停与类型查询**：确认函数返回类型、参数和 `__init` 等注解；遇到宏继续找宏定义。
2. **转到定义**：从 `setup_arch()`、`mm_core_init()`、`sched_init()` 调用点跳转，核对是否分别落在 x86、内存与调度代码中。
3. **调用层次**：先看当前入口调用了谁，再看目标函数由谁调用，只保留当前问题需要的分支。
4. **查找引用**：寻找初始化、状态修改与使用位置，不把每次出现符号都当成直接调用。
5. **返回调用点**：用输入、输出和执行条件总结函数职责，再决定是否继续深入。

这三个直接调用点见 [`init/main.c:892`](../../linux/init/main.c#L892)、[`init/main.c:931`](../../linux/init/main.c#L931)、[`init/main.c:944`](../../linux/init/main.c#L944)。读完后，应能独立重建 [图 0-1](01-overview.md)，并解释为什么它只是主线摘录。

### 4. 对间接关系使用“注册点 + 调用点”双向核实

遇到 `ops->method()`，先确认 `ops` 的类型，再找候选操作表的初始化位置，最后检查运行时究竟绑定了哪张表。找到同名成员的多个赋值不等于这些实现都会执行。

宏也可能掩盖注册关系。例如 `fs_initcall(proc_version_init)` 把函数交给初始化机制，不能只靠“谁直接调用 `proc_version_init()`”定位它的执行来源；注册位置见 [`fs/proc/version.c:19-27`](../../linux/fs/proc/version.c#L19-L27)。初始化机制在 [1.9](../01-boot/09-initcalls.md) 展开。

## 关键细节 / 设计考量

### 语义索引有边界

- 只编译当前 x86_64 配置涉及的文件，不代表已经索引所有架构与配置变体。
- 头文件通常借用相关翻译单元的参数解析；同一个头文件在不同包含上下文中可能有不同条件分支。
- 宏展开、汇编入口、链接脚本和运行时回调绑定需要人工检查，不能把调用图视为完整执行图。
- 数据库中的编译命令是重要证据，但汇编 `.S` 条目存在也不代表 clangd 能像 C 代码一样完整理解它。
- 编译器或 clangd 版本不匹配时，先检查不支持的参数、错误工作目录和缺失的生成头文件，不要把所有诊断都解释成内核源码错误。

### 文本搜索是补充，不是最终答案

没有可用 LSP 时，仍可先搜索，再阅读完整定义及上下文。以下命令在仓库根目录只读执行，搜索结果仍需人工核实，不能代替语义导航和上下文检查：

```bash
rg -n 'void start_kernel\(|setup_arch\(|mm_core_init\(|sched_init\(' \
    linux/init/main.c
rg -n 'void __init setup_arch|void __init mm_core_init|void __init sched_init' \
    linux/arch/x86/kernel/setup.c linux/mm/mm_init.c linux/kernel/sched/core.c
rg -n 'CONFIG_SMP|smp\.o|up\.o' linux/kernel/Makefile linux/.config
```

搜索结果只是候选位置。随后应打开文件，检查函数签名、条件编译、上下文与构建规则；行号记录使用从 1 开始的行号，不能直接抄用从 0 开始的 LSP 坐标。

## 可观测性

源码导航工具看到的“定义”不等于运行内核中的“可探测函数”。把静态导航结果映射到运行时，可使用以下入口：

| 入口 | 用途 | 配置与限制 |
|---|---|---|
| 输出目录 `vmlinux`、`llvm-nm`、`llvm-objdump` | 检查构建产物中是否保留符号及其机器码 | 只描述该产物，不证明它已经启动；内联与优化会改变符号边界 |
| `/proc/kallsyms` | 查看运行内核的符号名 | 基线 `CONFIG_KALLSYMS=y`、`CONFIG_PROC_FS=y`；注册见 [`kernel/kallsyms.c:930-933`](../../linux/kernel/kallsyms.c#L930-L933)，地址可见性另受 [`kernel/ksyms_common.c:28-43`](../../linux/kernel/ksyms_common.c#L28-L43) 约束 |
| `/sys/kernel/tracing/available_filter_functions` | 检查函数跟踪器可用的候选函数 | 基线 `CONFIG_FUNCTION_TRACER=y`、`CONFIG_DYNAMIC_FTRACE=y`；不是任意探针类型的通用许可列表，说明见 [`Documentation/trace/ftrace.rst`](../../linux/Documentation/trace/ftrace.rst) |

`static inline` 可能没有独立符号，`notrace` 等注解会限制某些跟踪方式，初始化代码还有存活时间窗口。不要因为编辑器能跳到定义，就假定登录后一定能对它挂 kprobe 或函数跟踪。挂载与检查 tracefs 的具体步骤见下一节。

## 小结

- 路径定位、配置选择和编译参数必须一起看，才能确定当前实现。
- clangd 使用真实构建数据库和生成头文件，不应依赖猜测的通用 C 参数。
- 在数据库对应的环境中运行语言服务器，避免宿主机与虚拟机绝对路径混用。
- 直接调用用语义工具追踪，回调和宏注册补查类型、赋值与绑定条件。
- 静态定义、构建符号和运行时探测能力是不同层次的证据。

## 参考

- [`scripts/clang-tools/gen_compile_commands.py`](../../linux/scripts/clang-tools/gen_compile_commands.py)：数据库生成实现。
- [`Documentation/kbuild/makefiles.rst`](../../linux/Documentation/kbuild/makefiles.rst)：对象、模块与目录构建规则。
- [`Documentation/kbuild/kconfig-language.rst`](../../linux/Documentation/kbuild/kconfig-language.rst)：配置类型与依赖。
- [`Documentation/trace/ftrace.rst`](../../linux/Documentation/trace/ftrace.rst)：运行时函数与事件跟踪入口。
