# 1.2 内核镜像、启动协议与 EFI stub

> **本节源码**：[`arch/x86/boot/Makefile`](../../linux/arch/x86/boot/Makefile)、[`arch/x86/boot/header.S`](../../linux/arch/x86/boot/header.S)、[`arch/x86/include/uapi/asm/bootparam.h`](../../linux/arch/x86/include/uapi/asm/bootparam.h)、[`drivers/firmware/efi/libstub/x86-stub.c`](../../linux/drivers/firmware/efi/libstub/x86-stub.c)
> **关键函数**：`efi_pe_entry()`、`efi_stub_entry()`、`efi_decompress_kernel()`、`enter_kernel()`
> **前置知识**：1.1 启动全景、ELF 文件与物理地址的基本概念

## 概述

内核不能猜测自己被放在什么地址、命令行在哪里、哪些内存可用，因此必须与引导方约定镜像格式和参数布局。传统 Linux/x86 启动协议通过 setup header 与 `boot_params` 传递这些信息。统一可扩展固件接口（Unified Extensible Firmware Interface，UEFI）路径则由 EFI stub 把固件接口转换为内核所需输入。本节首先区分构建产物，再分析两类入口怎样满足同一内核的运行条件。

## 核心数据结构

### 三种容易混淆的镜像

| 名称 | 含义 | 用途 |
|---|---|---|
| 顶层 `vmlinux` | 链接后的内核 ELF | 符号、反汇编和调试的主要依据 |
| `arch/x86/boot/compressed/vmlinux` | 含入口代码、解压器与压缩载荷的 ELF | 是另一个可执行程序，不是顶层内核本体 |
| `arch/x86/boot/bzImage` | setup 部分与由压缩镜像导出的二进制等组成的启动镜像 | 交给引导加载器；启用 EFI stub 时也可经 PE 入口启动 |

构建规则见 [`arch/x86/boot/Makefile:60-73`](../../linux/arch/x86/boot/Makefile#L60-L73)。其中 setup 部分会按规则补齐；不要把 `bzImage` 理解为一个只需通用解压命令就能运行的压缩文件，名称中的 `bz` 也不代表当前采用 bzip2。当前配置实际使用 `CONFIG_KERNEL_ZSTD=y`。

### `setup_header` 与 `boot_params`

`setup_header` 是镜像中的协议头，`boot_params` 是运行时参数块。后者也称启动参数页（Boot Parameters，常称 zeropage），**不是匿名内存的共享零页**。在实模式路径中，`copy_boot_params()` 明确检查其大小为 4096 字节，见 [`arch/x86/boot/main.c:30-56`](../../linux/arch/x86/boot/main.c#L30-L56)。

关键字段按原始名称列出，完整布局见 [`arch/x86/include/uapi/asm/bootparam.h:39-182`](../../linux/arch/x86/include/uapi/asm/bootparam.h#L39-L182)。

| 字段 | 所属结构 | 含义 |
|---|---|---|
| `header` / `version` | `setup_header` | 协议签名与版本；不是 Linux 发布版本号 |
| `code32_start` | `setup_header` | 32 位入口地址，允许引导方调整 |
| `cmd_line_ptr` | `setup_header` | 命令行地址低 32 位 |
| `ramdisk_image` / `ramdisk_size` | `setup_header` | 外部初始镜像地址与长度的低 32 位 |
| `kernel_alignment` / `relocatable_kernel` | `setup_header` | 对齐要求与可重定位能力 |
| `xloadflags` | `setup_header` | 64 位、高地址加载、EFI 等扩展能力 |
| `init_size` | `setup_header` | 初始化期间所需线性内存空间，不只是压缩文件大小 |
| `ext_cmd_line_ptr` / `ext_ramdisk_image` / `ext_ramdisk_size` | `boot_params` | 相应地址或长度的高 32 位 |
| `e820_entries` / `e820_table` | `boot_params` | 固件内存范围列表 |
| `efi_info` / `acpi_rsdp_addr` | `boot_params` | EFI 与 ACPI 信息的交接入口 |

高低位必须组合使用，例如命令行读取见 [`arch/x86/kernel/head64.c:185-192`](../../linux/arch/x86/kernel/head64.c#L185-L192)。只看 `hdr.cmd_line_ptr` 会把 4 GB 以上的地址截断。

## 主流程分析

传统交接有三种入口选择，不能机械串联为每次都执行的流程：

```text
引导加载器
├── 16 位 setup → main()               arch/x86/boot/main.c
│   └── go_to_protected_mode()         arch/x86/boot/pm.c
│       └── 压缩镜像 startup_32        arch/x86/boot/compressed/head_64.S
├── 直接进入压缩镜像 startup_32        arch/x86/boot/compressed/head_64.S
└── 直接进入压缩镜像 startup_64        arch/x86/boot/compressed/head_64.S
```

### 入口状态是协议的一部分

压缩镜像的 32 位入口相对偏移为 `0`，64 位入口为 `0x200`。前者要求相关启动对象位于 4 GB 以下；后者允许高地址，但要求引导方提供覆盖必要对象的恒等映射（Identity Mapping），并通过 `%rsi` 传入参数地址。入口注释与实现见 [`arch/x86/boot/compressed/head_64.S:81-106`](../../linux/arch/x86/boot/compressed/head_64.S#L81-L106)、[`arch/x86/boot/compressed/head_64.S:276-291`](../../linux/arch/x86/boot/compressed/head_64.S#L276-L291)。

这些偏移相对于压缩部分的入口基准，不能直接当作整个 `bzImage` 文件中的偏移。直接 32 / 64 位交接时，原本由 setup 探测的数据必须由引导方按协议提供。

### EFI stub 是另一条解压路径

EFI stub（EFI Boot Stub）使内核镜像具备固件能够进入的 PE 入口。当前启用 `CONFIG_EFI_STUB=y`，64 位 EFI 主线如下：

```text
EFI 固件 / EFI 引导程序
└── efi_pe_entry()                     drivers/firmware/efi/libstub/x86-stub.c
    └── efi_stub_entry()
        ├── efi_allocate_bootparams()〔没有传入参数块时〕
        ├── efi_decompress_kernel()
        │   ├── efi_random_alloc()
        │   └── decompress_kernel()   arch/x86/boot/compressed/misc.c
        ├── efi_load_initrd()
        ├── exit_boot() → efi_exit_boot_services()
        ├── efi_5level_switch()
        └── enter_kernel()
            └── startup_64〔未压缩内核〕 arch/x86/kernel/head_64.S
```

`efi_decompress_kernel()` 使用 EFI 分配接口取得输出空间，调用与传统路径共享的 `decompress_kernel()`，并形成最终物理入口地址，见 [`drivers/firmware/efi/libstub/x86-stub.c:735-796`](../../linux/drivers/firmware/efi/libstub/x86-stub.c#L735-L796)。它**不经过**传统 `extract_kernel()` 的完整包装流程。

退出固件引导服务（ExitBootServices）之后，stub 完成必要的模式处理，直接跳转到未压缩内核。最终交接代码很短：

[`drivers/firmware/efi/libstub/x86-stub.c:798-805`](../../linux/drivers/firmware/efi/libstub/x86-stub.c#L798-L805)（v6.18.60）

```c
static void __noreturn enter_kernel(unsigned long kernel_addr,
				    struct boot_params *boot_params)
{
	/* enter decompressed kernel with boot_params pointer in RSI/ESI */
	asm("jmp *%0"::"r"(kernel_addr), "S"(boot_params));

	unreachable();
}
```

这里的 `"S"` 约束把参数放进 `%rsi` / `%esi`；它不是按普通 C 调用约定把参数传给 `startup_64`。当前还启用了 `CONFIG_EFI_HANDOVER_PROTOCOL=y` 与 `CONFIG_EFI_MIXED=y`，这些兼容入口不能等同于上面的 64 位 PE 入口；本章不展开其跨位宽汇编。

## 关键细节 / 设计考量

- **同一内核支持多种引导环境**：运行时依据收到的参数判断 EFI 启动，而不是看编译配置里有没有 `CONFIG_EFI`。判断入口见 [`arch/x86/kernel/setup.c:532-564`](../../linux/arch/x86/kernel/setup.c#L532-L564)。
- **退出引导服务不等于所有 EFI 服务消失**：EFI runtime services 是另一类接口，不能将 `ExitBootServices` 解释为禁用整个 EFI 子系统。
- **外部镜像不一定已经由引导方加载**：stub 可通过 EFI 机制装载 initrd，并写回高低位地址与长度，见 [`drivers/firmware/efi/libstub/x86-stub.c:883-903`](../../linux/drivers/firmware/efi/libstub/x86-stub.c#L883-L903)。

## 可观测性

| 入口 | 看什么、如何解读 | 源码与配置 |
|---|---|---|
| `/sys/kernel/boot_params/version` | `boot_params.hdr.version`，是协议版本，不是 `uname -r` | [`arch/x86/kernel/ksysfs.c:22-28`](../../linux/arch/x86/kernel/ksysfs.c#L22-L28)；`CONFIG_SYSFS=y` |
| `/sys/kernel/boot_params/data` | 内核保留的参数块，二进制布局按本节结构解码；可能已经过内核清洗或修改 | [`arch/x86/kernel/ksysfs.c:30-45`](../../linux/arch/x86/kernel/ksysfs.c#L30-L45)，注册见同文件 [`arch/x86/kernel/ksysfs.c:372-401`](../../linux/arch/x86/kernel/ksysfs.c#L372-L401)；`CONFIG_SYSFS=y` |
| `/proc/cmdline` | `saved_command_line`，是内核保留的命令行，可包含 bootconfig 合成内容，不保证逐字等于固件原始输入 | [`fs/proc/cmdline.c:7-24`](../../linux/fs/proc/cmdline.c#L7-L24)；`CONFIG_PROC_FS=y` |
| EFI stub 屏幕输出 | 引导服务仍有效时的错误与诊断；不应假定它会进入后续内核日志缓冲区 | [`drivers/firmware/efi/libstub/printk.c:118-151`](../../linux/drivers/firmware/efi/libstub/printk.c#L118-L151)；`CONFIG_EFI_STUB=y` |

## 动手实验

在已经启动目标内核的 dev 虚拟机中，可以只读检查协议版本。此步骤未实测，不预设具体输出：

```bash
uname -r
python3 - <<'PY'
from pathlib import Path
p = Path('/sys/kernel/boot_params')
print((p / 'version').read_text().strip())
b = (p / 'data').read_bytes()
print('参数块字节数:', len(b))
print('命令行:', Path('/proc/cmdline').read_text().strip())
PY
```

先确认 `uname -r` 与目标构建一致；Vagrant 镜像自带内核并不会因为仓库存在源码就自动替换。

## 小结

- `bzImage`、压缩目录下的 `vmlinux` 与顶层 `vmlinux` 具有不同职责。
- 启动参数是稳定的交接 ABI，必须区分协议头、完整参数块和高地址扩展。
- 32 位与 64 位入口对 CPU 状态、地址范围和页表各有要求。
- 本版本 EFI stub 自行分配空间、解压内核，再直接进入未压缩内核入口。

## 参考

- [`Documentation/arch/x86/boot.rst`](../../linux/Documentation/arch/x86/boot.rst)。
- [`Documentation/arch/x86/zero-page.rst`](../../linux/Documentation/arch/x86/zero-page.rst)。
- [`Documentation/admin-guide/efi-stub.rst`](../../linux/Documentation/admin-guide/efi-stub.rst)。
