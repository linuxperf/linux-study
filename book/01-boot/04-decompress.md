# 1.4 解压、重定位与 KASLR

> **本节源码**：[`arch/x86/boot/compressed/head_64.S`](../../linux/arch/x86/boot/compressed/head_64.S)、[`arch/x86/boot/compressed/misc.c`](../../linux/arch/x86/boot/compressed/misc.c)、[`arch/x86/boot/compressed/kaslr.c`](../../linux/arch/x86/boot/compressed/kaslr.c)、[`drivers/firmware/efi/libstub/x86-stub.c`](../../linux/drivers/firmware/efi/libstub/x86-stub.c)
> **关键函数**：`extract_kernel()`、`choose_random_location()`、`decompress_kernel()`、`parse_elf()`、`handle_relocations()`
> **前置知识**：1.2 启动协议、1.3 CPU 模式切换、分页基础

## 概述

引导加载器装入的通常是压缩镜像，不能直接当作最终内核执行。解压阶段既要避免输出覆盖仍在运行的解压器，又要通过重定位（Relocation）适配实际运行布局。内核地址空间布局随机化（Kernel Address Space Layout Randomization，KASLR）还要求物理放置位置与虚拟运行位置满足各自约束。本节先分析传统解压入口，再说明 EFI 路径共享了哪些机制。

## 核心数据与地址

| 数据或符号 | 含义 | 易错点 |
|---|---|---|
| `input_data` / `input_len` | 压缩载荷地址与长度 | 不是最终内核的代码入口 |
| `output_len` | 解压输出长度 | 不一定覆盖运行时全部 BSS 与预留空间 |
| `kernel_total_size` | 根据最终内核符号计算的总占用 | 选物理内存孔洞时还需考虑它 |
| `output` | 解压目标的物理放置地址，在当前恒等映射下访问 | 不等于最终内核虚拟地址 |
| `virt_addr` | 相对内核映射基准使用的虚拟布局偏移 | 不能简单当作 `output` 的副本 |
| `entry_offset` | ELF 入口相对加载基准的偏移 | 最终物理入口为 `output + entry_offset` |

变量定义与计算见 [`arch/x86/boot/compressed/misc.c:333-364`](../../linux/arch/x86/boot/compressed/misc.c#L333-L364)、[`arch/x86/boot/compressed/misc.c:407-537`](../../linux/arch/x86/boot/compressed/misc.c#L407-L537)。

## 主流程分析

下列树包含汇编控制转移；64 位加载器可以直接进入第二个入口。

```text
startup_32〔可选〕                     arch/x86/boot/compressed/head_64.S
├── verify_cpu()
├── 建立早期页表，设置 PAE / EFER.LME / CR0.PG
└── startup_64
    ├── configure_5level_paging()
    ├── 将压缩镜像移到安全解压位置
    └── .Lrelocated
        ├── initialize_identity_maps()
        └── extract_kernel()          arch/x86/boot/compressed/misc.c
            ├── choose_random_location()  arch/x86/boot/compressed/kaslr.c
            └── decompress_kernel()
                ├── __decompress()
                ├── parse_elf()
                └── handle_relocations()
        → 跳转到未压缩内核入口
```

### 32 位入口建立长模式条件

`startup_32` 检查 CPU，启用 `CR4.PAE`，构造覆盖早期 4 GB 的页表，并将其装入 `CR3`。随后设置 `EFER.LME`，启用分页并通过远返回进入 64 位代码段，见 [`arch/x86/boot/compressed/head_64.S:164-274`](../../linux/arch/x86/boot/compressed/head_64.S#L164-L274)。只设置 `EFER.LME` 并不自动让当前代码按 64 位指令语义执行。

直接进入 `startup_64` 时，这些初始条件由引导方建立。入口仍会统一数据段、重新计算地址和准备栈，不能假定先前执行过 `startup_32` 中的计算。

### 为什么先搬移解压器

传统路径把压缩镜像移到工作缓冲区较高的一端，使向目标区域写出的内核不破坏尚未消费的压缩输入和正在执行的代码。移动使用从高地址向低地址复制的方向设置，随后跳到搬移后的 `.Lrelocated`，见 [`arch/x86/boot/compressed/head_64.S:416-476`](../../linux/arch/x86/boot/compressed/head_64.S#L416-L476)。

所需空间由镜像头的 `init_size` 表达，构建期计算会比较压缩执行环境和未压缩内核的需求，见 [`arch/x86/boot/header.S:496-523`](../../linux/arch/x86/boot/header.S#L496-L523)。因此，“压缩文件能放下”不是“启动工作区足够”的充分条件。

### 页表层数必须在安全条件下切换

`configure_5level_paging()` 用于统一当前页表层数与目标选择。不能在活动长模式下直接翻转 `CR4.LA57`；需要低地址跳板临时退出相关模式再切换。调用位置及约束见 [`arch/x86/boot/compressed/head_64.S:335-409`](../../linux/arch/x86/boot/compressed/head_64.S#L335-L409)。当前 `CONFIG_PGTABLE_LEVELS=5` 表示具备支持能力，是否实际采用五级页表仍由运行环境决定。

### KASLR 先解决“放哪里”

`extract_kernel()` 把所需空间取为 `output_len` 与 `kernel_total_size` 的较大值，再按 x86_64 映射粒度对齐。`choose_random_location()` 避开输入、initrd 等不可覆盖范围，根据可用内存选择物理地址，并为 x86_64 选择虚拟布局偏移，见 [`arch/x86/boot/compressed/misc.c:462-511`](../../linux/arch/x86/boot/compressed/misc.c#L462-L511)、[`arch/x86/boot/compressed/kaslr.c:861-908`](../../linux/arch/x86/boot/compressed/kaslr.c#L861-L908)。

当前 `CONFIG_RANDOMIZE_BASE=y`、`CONFIG_RELOCATABLE=y`。`nokaslr` 会使传统选择函数提前返回；未找到合适物理候选也不等于虚拟随机化必须一起失败，代码对二者分别处理。

### 解压之后还要解析 ELF 与重定位

共享的核心函数如下：

[`arch/x86/boot/compressed/misc.c:344-363`](../../linux/arch/x86/boot/compressed/misc.c#L344-L363)（v6.18.60）

```c
unsigned long decompress_kernel(unsigned char *outbuf, unsigned long virt_addr,
				void (*error)(char *x))
{
	unsigned long entry;

	if (!free_mem_ptr) {
		free_mem_ptr     = (unsigned long)boot_heap;
		free_mem_end_ptr = (unsigned long)boot_heap + sizeof(boot_heap);
	}

	if (__decompress(input_data, input_len, NULL, NULL, outbuf, output_len,
			 NULL, error) < 0)
		return ULONG_MAX;

	entry = parse_elf(outbuf);
	handle_relocations(outbuf, output_len, virt_addr);

	return entry;
}
```

当前选择 Zstd，算法实现通过 `CONFIG_KERNEL_ZSTD` 对应的包含分支进入，见 [`arch/x86/boot/compressed/misc.c:63-89`](../../linux/arch/x86/boot/compressed/misc.c#L63-L89)。`parse_elf()` 验证 ELF 签名，遍历 `PT_LOAD` 段并将其移动到应有位置，返回入口偏移，见同文件 [`arch/x86/boot/compressed/misc.c:281-331`](../../linux/arch/x86/boot/compressed/misc.c#L281-L331)。

`handle_relocations()` 根据虚拟布局调整需要修正的引用。对于 x86_64，代码明确把重定位差值转为由 `virt_addr` 决定，不能把“物理搬移”与“修正所有绝对引用”视为同一操作，见 [`arch/x86/boot/compressed/misc.c:205-273`](../../linux/arch/x86/boot/compressed/misc.c#L205-L273)。

## 关键细节 / 设计考量

### EFI 共享解压核心，但不共享全部选址过程

EFI stub 使用 `efi_random_alloc()` 选择物理分配位置，使用固件随机种子参与虚拟偏移选择，然后调用同一个 `decompress_kernel()`。它不调用传统 `choose_random_location()`，见 [`drivers/firmware/efi/libstub/x86-stub.c:735-796`](../../linux/drivers/firmware/efi/libstub/x86-stub.c#L735-L796)。因此分析 KASLR 失败原因时必须先识别启动路径。

### 早期失败与普通内核错误不同

此时没有完整调度、驱动与通用分配器。内存不足、ELF 损坏、对齐错误等会走解压器自己的错误路径；不能期待已有用户空间收集崩溃日志。机密虚拟机的内存加密与未接受内存处理也会影响本阶段，但其专用协议不在本节展开。

## 可观测性

| 入口 | 能看到什么 | 条件与源码 |
|---|---|---|
| 解压器普通地址与阶段输出 | `input_data`、`output`、所需空间与入口偏移等 | 依赖 `CONFIG_X86_VERBOSE_BOOTUP`，当前为 **n**；宏会变为空操作，见 [`arch/x86/boot/compressed/misc.h:72-91`](../../linux/arch/x86/boot/compressed/misc.h#L72-L91) |
| 解压器错误输出 | ELF、对齐、目标越界等错误 | 错误输出不受上述普通调试宏控制，见 [`arch/x86/boot/compressed/misc.h:65-70`](../../linux/arch/x86/boot/compressed/misc.h#L65-L70) |
| 早期串口捕获 | 传统路径的直接串口输出 | 压缩镜像串口支持依赖 `CONFIG_EARLY_PRINTK=y`，见 [`arch/x86/boot/compressed/Makefile:90-93`](../../linux/arch/x86/boot/compressed/Makefile#L90-L93) |
| `/sys/kernel/boot_params/data` 的 `hdr.loadflags` | `KASLR_FLAG` 表示启用了相应随机化路径，不给出实际随机地址，也不证明随机源质量 | 标志定义见 [`arch/x86/include/uapi/asm/bootparam.h:12-18`](../../linux/arch/x86/include/uapi/asm/bootparam.h#L12-L18)；数据导出见 [`arch/x86/kernel/ksysfs.c:30-45`](../../linux/arch/x86/kernel/ksysfs.c#L30-L45)，`CONFIG_SYSFS=y` |

解压器是独立程序，不是常驻内核中的同名调用目标。登录后用 kprobe 挂 `extract_kernel()`，不能补抓本次解压过程。调试绝对地址时可在隔离虚拟机中临时使用 `nokaslr`，但会降低安全性，不应作为日常配置；本章也不修改只读源码树来开启额外输出。

## 小结

- 传统解压入口既准备 CPU 环境，也解决输入、输出与自身代码的覆盖问题。
- 物理放置、虚拟布局与重定位是有关联但不同的任务。
- 解压输出仍需 ELF 段整理和引用修正，最终才得到可跳转入口。
- EFI 路径共享 `decompress_kernel()`，不共享传统路径的全部内存选址逻辑。
- 当前配置关闭普通解压调试输出，不能承诺一定看到 `Decompressing Linux`。

## 参考

- [`Documentation/arch/x86/boot.rst`](../../linux/Documentation/arch/x86/boot.rst)。
- [`Documentation/admin-guide/kernel-parameters.txt`](../../linux/Documentation/admin-guide/kernel-parameters.txt)：`nokaslr` 等参数。
- [1.5 内核 64 位入口与早期页表](05-head64.md)。
