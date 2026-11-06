# 1.3 实模式准备与保护模式切换

> **本节源码**：[`arch/x86/boot/header.S`](../../linux/arch/x86/boot/header.S)、[`arch/x86/boot/main.c`](../../linux/arch/x86/boot/main.c)、[`arch/x86/boot/memory.c`](../../linux/arch/x86/boot/memory.c)、[`arch/x86/boot/pm.c`](../../linux/arch/x86/boot/pm.c)、[`arch/x86/boot/pmjump.S`](../../linux/arch/x86/boot/pmjump.S)
> **关键函数**：`main()`、`copy_boot_params()`、`detect_memory()`、`go_to_protected_mode()`、`protected_mode_jump()`
> **前置知识**：1.2 启动协议、x86 分段与控制寄存器的基本概念

## 概述

传统 BIOS 环境提供的若干硬件查询接口依赖实模式（Real Mode）。Linux 的 setup 代码趁这些接口仍可用时收集平台信息，再切换到保护模式（Protected Mode）。这段代码拥有自己的入口、栈、堆和输出函数，不能使用完整内核的运行环境。它是可选的兼容启动路径：EFI stub 或直接使用 32 / 64 位协议的加载器不会必然执行本节流程。

## 核心数据结构

### 一个独立的早期执行环境

`boot_params` 在 setup 程序中有独立实例，`HEAP` 和 `heap_end` 管理一段有限的临时空间，定义见 [`arch/x86/boot/main.c:18-23`](../../linux/arch/x86/boot/main.c#L18-L23)。`init_heap()` 依据 `CAN_USE_HEAP`、`hdr.heap_end_ptr` 与当前栈边界设置可用范围，避免堆向栈覆盖，见同文件 [`arch/x86/boot/main.c:118-131`](../../linux/arch/x86/boot/main.c#L118-L131)。这里没有 `kmalloc()` 或伙伴系统。

保护模式切换使用全局描述符表（Global Descriptor Table，GDT）。`boot_gdt` 提供平坦的 32 位代码段、数据段和为虚拟化兼容准备的任务状态段（Task State Segment，TSS）描述符，见 [`arch/x86/boot/pm.c:65-91`](../../linux/arch/x86/boot/pm.c#L65-L91)。它不是最终每 CPU 的 GDT。

内存探测结果写入 `boot_params.e820_table`，表项的地址、大小和类型供后续 `setup_arch()` 使用。它描述固件报告的范围，并不意味着其中每一段 RAM 都可以立即拿来分配。

## 主流程分析

以下控制流从 setup 入口开始，包含汇编跳转：

```text
start_of_setup                         arch/x86/boot/header.S
└── main()                             arch/x86/boot/main.c
    ├── copy_boot_params()
    ├── console_init()                 arch/x86/boot/early_serial_console.c
    ├── init_heap()
    ├── validate_cpu()
    ├── detect_memory()                arch/x86/boot/memory.c
    ├── set_video()                    arch/x86/boot/video.c
    └── go_to_protected_mode()         arch/x86/boot/pm.c
        ├── realmode_switch_hook()
        ├── enable_a20()               arch/x86/boot/a20.c
        ├── setup_idt() / setup_gdt()
        └── protected_mode_jump()      arch/x86/boot/pmjump.S
            └── hdr.code32_start      压缩镜像的 32 位入口
```

### 入口先修复段与栈，再调用 C

`start_of_setup` 规范化段寄存器、校正栈地址与对齐，检查 setup 签名并清零自己的 BSS，最后 `calll main`，见 [`arch/x86/boot/header.S:548-600`](../../linux/arch/x86/boot/header.S#L548-L600)。这里出现 C 函数不表示 CPU 已经进入 64 位模式；CPU 模式由段属性与控制寄存器决定，而不是由源码文件扩展名决定。

`main()` 首先把镜像中的 `hdr` 复制到参数块，兼容旧命令行格式，然后初始化早期控制台、临时堆并验证 CPU，见 [`arch/x86/boot/main.c:133-155`](../../linux/arch/x86/boot/main.c#L133-L155)。复制 setup header 和后续内核重新保存启动参数，是不同阶段的两次操作。

### 在离开 BIOS 环境前探测内存

`detect_memory_e820()` 通过 BIOS `INT 0x15` 的 `E820` 接口循环获取内存项，用返回的续接值继续遍历，并检查签名和错误状态，见 [`arch/x86/boot/memory.c:18-70`](../../linux/arch/x86/boot/memory.c#L18-L70)。`detect_memory()` 还执行旧接口探测，为后续回退提供信息，见同文件 [`arch/x86/boot/memory.c:116-121`](../../linux/arch/x86/boot/memory.c#L116-L121)。

键盘、显示及部分历史平台信息也在此时收集，随后不再依赖这些 BIOS 实模式服务。完整调用顺序见 [`arch/x86/boot/main.c:133-181`](../../linux/arch/x86/boot/main.c#L133-L181)。

### 切换前先封闭旧的中断入口

`go_to_protected_mode()` 先执行切换钩子，通常会关闭可屏蔽中断并屏蔽不可屏蔽中断（Non-Maskable Interrupt，NMI），再打开 A20、复位协处理器接口并屏蔽传统 PIC。它随后加载空的中断描述符表（Interrupt Descriptor Table，IDT）和启动 GDT，见 [`arch/x86/boot/pm.c:103-125`](../../linux/arch/x86/boot/pm.c#L103-L125)。此时不能把旧 BIOS 中断入口继续当作保护模式处理程序。

A20 是传统 PC 地址兼容机制；若它仍关闭，高地址访问可能出现回绕。`enable_a20()` 失败会打印错误并停止，而不会带着不可靠的地址行为继续启动。

### 设置 `CR0.PE` 后必须重装代码段

关键切换指令如下：

[`arch/x86/boot/pmjump.S:34-45`](../../linux/arch/x86/boot/pmjump.S#L34-L45)（v6.18.60）

```asm
	movw	$__BOOT_DS, %cx
	movw	$__BOOT_TSS, %di

	movl	%cr0, %edx
	orb	$X86_CR0_PE, %dl	# Protected mode
	movl	%edx, %cr0

	# Transition to 32-bit mode
	.byte	0x66, 0xea		# ljmpl opcode
2:	.long	.Lin_pm32		# offset
	.word	__BOOT_CS		# segment
```

设置 `CR0.PE` 是打开保护模式；远跳转进一步装载描述符表中的代码段，使后续指令按目标段属性执行。`.Lin_pm32` 再设置平坦数据段并清理寄存器，最后跳到 `%eax` 中的目标地址，见 [`arch/x86/boot/pmjump.S:49-75`](../../linux/arch/x86/boot/pmjump.S#L49-L75)。`%esi` 保留参数块的物理地址。

## 关键细节 / 设计考量

- **保护模式不是长模式**：本节只到 32 位保护模式。长模式（Long Mode）的页表、`EFER.LME` 与分页启用由后续压缩镜像入口完成。
- **临时描述符不能长期沿用**：启动 GDT、空 IDT 只解决过渡问题；后续还要建立异常处理、每 CPU 描述符表与内核栈。
- **不能把所有 x86 启动画成 BIOS 调用链**：跳过 setup 的加载器有责任填好参数。运行时看到 E820 信息，不足以证明本次执行过 Linux 的 `detect_memory_e820()`。

## 可观测性

本阶段尚无 tracefs 和 BPF，也没有可以登录后重放的实模式函数调用记录。可用入口主要是早期串口、直接屏幕输出和后续保存的数据。

| 入口 | 解读 | 源码与条件 |
|---|---|---|
| `debug` 与 setup 控制台 | 经过实模式 `main()` 时输出 `early console in setup code`，用于确认路径；未看到也可能是控制台不可用 | [`arch/x86/boot/main.c:142-145`](../../linux/arch/x86/boot/main.c#L142-L145) |
| `earlyprintk=serial,ttyS0,115200` | setup 的专用解析器配置串口，不是完整内核的 `printk()` 调用链 | [`arch/x86/boot/early_serial_console.c:46-99`](../../linux/arch/x86/boot/early_serial_console.c#L46-L99)；必须有对应 UART 或虚拟串口 |
| A20 与 CPU 检查错误 | 输出后可能直接停止，后续 `dmesg` 不一定留存 | [`arch/x86/boot/pm.c:105-112`](../../linux/arch/x86/boot/pm.c#L105-L112)、[`arch/x86/boot/main.c:149-152`](../../linux/arch/x86/boot/main.c#L149-L152) |
| `/sys/kernel/boot_params/data` | 可检查保存的 E820 项，但只能看到交接数据，不能还原 BIOS 调用时间线 | [`arch/x86/kernel/ksysfs.c:30-45`](../../linux/arch/x86/kernel/ksysfs.c#L30-L45)；`CONFIG_SYSFS=y` |

setup 自身编入早期串口代码，见 [`arch/x86/boot/Makefile:24`](../../linux/arch/x86/boot/Makefile#L24)。压缩镜像是否编入对应串口代码另受 `CONFIG_EARLY_PRINTK` 控制，当前为 y。不要把 `earlyprintk`、后续的 `earlycon` 和最终 `console` 当作同一个生命周期的控制台。

## 小结

- 实模式 setup 在旧固件接口仍可用时收集数据，并使用自己的有限执行环境。
- GDT、A20 和中断屏蔽共同为保护模式切换提供条件。
- `CR0.PE` 与远跳转配合完成模式过渡，长模式仍在后面建立。
- `%esi` 把启动参数地址传给下一个入口；参数内容比某条固定启动路径更稳定。

## 参考

- [`Documentation/arch/x86/boot.rst`](../../linux/Documentation/arch/x86/boot.rst)。
- [`arch/x86/boot/a20.c`](../../linux/arch/x86/boot/a20.c)。
- [1.4 解压、重定位与 KASLR](04-decompress.md)。
