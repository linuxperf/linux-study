# 1.1 启动全景：从固件到用户空间

> **本节源码**：[`arch/x86/boot/compressed/head_64.S`](../../linux/arch/x86/boot/compressed/head_64.S)、[`arch/x86/kernel/head64.c`](../../linux/arch/x86/kernel/head64.c)、[`init/main.c`](../../linux/init/main.c)
> **关键函数**：`extract_kernel()`、`x86_64_start_kernel()`、`start_kernel()`、`rest_init()`、`kernel_init()`
> **前置知识**：C 语言、x86 寄存器、物理地址与虚拟地址的基本概念

## 概述

内核运行所依赖的内存分配、异常处理、调度和设备访问能力，启动之初都还没有建立。启动代码必须依靠有限的栈、静态数据和固件信息逐步建立这些条件。阅读启动过程的关键不是背出所有初始化函数，而是判断“此时哪些条件已成立，下一步为什么需要它们”。本章终点是 PID 1 装入用户程序并准备返回用户态，不包含用户空间服务管理器的完整启动流程。

## 核心状态与阶段边界

### 固件、引导加载器与内核的分工

固件（Firmware）提供平台初始环境；引导加载器（Boot Loader）把内核镜像、命令行和可选的初始文件系统镜像放进内存。内核接收这些输入，接管 CPU 与内存，并初始化自身。其边界是一份启动协议，而不是约定内核必然从 CPU 复位向量开始执行，见 [`Documentation/arch/x86/boot.rst`](../../linux/Documentation/arch/x86/boot.rst)。

本章主线针对普通 x86_64 系统，区分两种交接方式：

- 传统协议入口：引导加载器可执行实模式 setup，也可自行准备参数，直接进入压缩镜像的 32 位或 64 位入口。
- EFI stub：固件按 PE 可执行镜像接口进入内核内嵌的适配代码；本版本由 stub 解压后直接跳到未压缩内核。实现见 [`drivers/firmware/efi/libstub/x86-stub.c:735-950`](../../linux/drivers/firmware/efi/libstub/x86-stub.c#L735-L950)。

### 四类“已经启动”不能混用

| 阶段边界 | 已经具备 | 尚不能据此推断 |
|---|---|---|
| 进入未压缩内核 | 内核指令可取指，CPU 已处于 64 位模式 | 最终页表、通用分配器和完整异常处理已就绪 |
| `start_kernel()` 后段 | 基本内存管理、调度器、中断与时间基础已建立 | 全部 CPU 和驱动已初始化 |
| `kernel_init_freeable()` 结束 | SMP 与分级初始化已进行，初始根环境可访问 | 用户空间 init 已成功执行 |
| `kernel_init()` 成功执行 init | PID 1 已装入用户程序 | 用户空间全部服务已启动 |

这些边界可分别在 [`arch/x86/kernel/head64.c:222-310`](../../linux/arch/x86/kernel/head64.c#L222-L310)、[`init/main.c:869-1071`](../../linux/init/main.c#L869-L1071)、[`init/main.c:1434-1583`](../../linux/init/main.c#L1434-L1583) 中核对。

## 主流程分析

下面是跨阶段控制流树，包含汇编跳转，不全是 C 函数调用。EFI 分支在 1.2 单独展开。

```text
传统启动协议入口                         arch/x86/boot/
├── main()〔可选：实模式 setup〕           arch/x86/boot/main.c
└── startup_32 / startup_64〔压缩镜像〕    arch/x86/boot/compressed/head_64.S
    └── extract_kernel()                 arch/x86/boot/compressed/misc.c
        └── startup_64〔未压缩内核〕      arch/x86/kernel/head_64.S
            └── x86_64_start_kernel()    arch/x86/kernel/head64.c
                └── x86_64_start_reservations()
                    └── start_kernel()  init/main.c
                        ├── setup_arch()        arch/x86/kernel/setup.c
                        ├── mm_core_init()      mm/mm_init.c
                        ├── sched_init()        kernel/sched/core.c
                        └── rest_init()         init/main.c
                            ├── user_mode_thread(kernel_init)  kernel/fork.c
                            ├── kernel_thread(kthreadd)        kernel/fork.c
                            └── cpu_startup_entry()            kernel/sched/idle.c
```

### 从单一执行流到可调度任务

`start_kernel()` 在启动 CPU 的初始任务上下文中运行。`sched_init()` 建立调度器基础，并不意味着这里已经创建了 PID 1。真正的分叉发生在 `rest_init()`：它先创建执行 `kernel_init()` 的任务，再创建 `kthreadd`，最后使原始任务进入 idle 循环，见 [`init/main.c:672-719`](../../linux/init/main.c#L672-L719)。

PID 1 创建后先在内核态继续初始化。它等待 `kthreadd` 就绪，然后启动其他 CPU、运行 initcall 并准备根文件系统。只有在后续 `kernel_execve()` 成功后，才通过架构返回路径进入用户态；“PID 1 存在”不等于“PID 1 已在用户态运行”。

### 从一次性初始化到长期运行

启动期函数常带 `__init`，相关代码在初始化完成后可以回收。`kernel_init()` 先等待异步初始化结束，清理探针与跟踪设施对初始化内存的引用，再调用 `free_initmem()`，见 [`init/main.c:1443-1463`](../../linux/init/main.c#L1443-L1463)。因此，启动函数不是登录以后还能任意调用或挂探针的常驻接口。

## 关键细节 / 设计考量

### 状态机不是单一的“开机完成”标志

`system_state` 区分 `SYSTEM_BOOTING`、`SYSTEM_SCHEDULING`、`SYSTEM_FREEING_INITMEM` 与 `SYSTEM_RUNNING` 等状态，定义见 [`include/linux/kernel.h:168-195`](../../linux/include/linux/kernel.h#L168-L195)。`rest_init()` 切入 `SYSTEM_SCHEDULING`，`kernel_init()` 在释放初始化内存前后更新另外两个状态。这些状态描述内核内部的执行条件，不是服务管理器的就绪通知。

### 观测工具也有启动顺序

实模式代码和解压器尚不能依赖内核的 tracefs、BPF 或完整 `printk()` 环境。`start_kernel()` 中也先有 `ftrace_init()`、`early_trace_init()`，随后才有完整事件初始化 `trace_init()`，见 [`init/main.c:931-978`](../../linux/init/main.c#L931-L978)。不能用一个登录后才启动的跟踪会话解释已经过去的早期阶段。

### 每一层只建立下一层所需的最小条件

早期页表只需保证内核和必要输入可访问；`setup_arch()` 才逐步形成平台内存视图。调度器基础先于中断启用，完整 SMP 调度初始化又晚于其他 CPU 启动。相同子系统出现多次初始化入口，通常是在不同阶段补齐不同条件，而不是重复做同一件事。

## 小结

- 启动过程可以按“交接契约、执行环境、并行初始化、用户空间接管”分层阅读。
- EFI stub 与传统解压入口是不同路径，在未压缩内核入口汇合。
- `start_kernel()` 建立通用基础，`rest_init()` 才创建 PID 1 与 `kthreadd`。
- PID 1 先执行内核初始化函数，成功装入程序后才进入用户空间。
- 观测能力随启动逐步建立；一次性启动代码可能在登录前已被回收。

## 参考

- [`Documentation/arch/x86/boot.rst`](../../linux/Documentation/arch/x86/boot.rst)：启动交接协议。
- [`init/main.c`](../../linux/init/main.c)：通用启动主线。
- [1.2 内核镜像、启动协议与 EFI stub](02-boot-protocol.md)、[1.7 start_kernel](07-start-kernel.md)。
