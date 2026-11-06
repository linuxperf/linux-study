# 1.10 initramfs 与根文件系统

> **本节源码**：[`fs/namespace.c`](../../linux/fs/namespace.c)、[`init/initramfs.c`](../../linux/init/initramfs.c)、[`init/do_mounts.c`](../../linux/init/do_mounts.c)、[`init/main.c`](../../linux/init/main.c)、[`drivers/base/devtmpfs.c`](../../linux/drivers/base/devtmpfs.c)
> **关键函数**：`init_mount_tree()`、`populate_rootfs()`、`do_populate_rootfs()`、`wait_for_initramfs()`、`prepare_namespace()`、`mount_root()`
> **前置知识**：1.9 initcall、文件系统挂载与设备文件的基本概念

## 概述

要执行用户程序，内核必须先能解析路径并找到可执行文件；而挂载磁盘根分区又可能依赖尚未装入的驱动或用户空间存储工具。初始内存文件系统（Initial RAM Filesystem，initramfs）通过把一个文件树提前带入内存，打破这类依赖。内核先在 rootfs 上解包初始文件，再决定直接执行其中的 `/init`，还是自行挂载 `root=` 指定的根。本节区分 rootfs、initramfs、传统 initrd 和最终根文件系统，避免把它们统称为“内存盘”。

## 核心对象

| 对象 | 本节中的含义 | 关键区别 |
|---|---|---|
| rootfs | 最初挂载到根位置的内核文件系统实例 | 不是“磁盘根分区”的同义词 |
| initramfs | 解包到 rootfs 的 cpio 文件集合，可有内置与外部来源 | 不要求先构造一个块设备再挂载其中的文件系统 |
| initrd | 交接字段和历史机制使用的名称，也可指传统初始 RAM 磁盘（Initial RAM Disk） | 启动参数字段叫 initrd，不代表载荷一定是旧块设备格式 |
| `initrd_start` / `initrd_end` | 外部镜像在内核中可访问的起止地址 | 解包后原镜像内存通常可以释放 |
| `ramdisk_execute_command` | 初始根环境要运行的程序，默认 `/init` | 可由 `rdinit=` 改写 |
| `saved_root_name` | `root=` 的保存值 | 主要供内核直接挂载根的路径使用 |

当前 `CONFIG_BLK_DEV_INITRD=y`、各 `CONFIG_RD_*` 解压支持为 y，`CONFIG_INITRAMFS_SOURCE=""`。这表示没有配置自定义内置文件树，不表示启动时没有外部 initramfs，也不表示内置归档完全没有基本目录。空配置选择默认列表的规则见 [`usr/Makefile:19-26`](../../linux/usr/Makefile#L19-L26)，列表 [`usr/default_cpio_list`](../../linux/usr/default_cpio_list) 中包含 `/dev/console`。

## 主流程分析

```text
start_kernel()                         init/main.c
└── vfs_caches_init()                  fs/dcache.c
    └── mnt_init()                     fs/namespace.c
        ├── init_rootfs()              init/do_mounts.c
        └── init_mount_tree()

kernel_init_freeable()                 init/main.c
├── do_basic_setup()
│   └── rootfs_initcall(populate_rootfs) init/initramfs.c
│       └── 异步安排 do_populate_rootfs()
│           ├── unpack_to_rootfs()〔内置归档〕
│           └── unpack_to_rootfs()〔外部镜像，如有〕
├── wait_for_initramfs()
├── console_on_rootfs()
└── 检查 ramdisk_execute_command
    ├── 可访问：保留初始根，稍后执行 /init 或 rdinit 指定程序
    └── 不可访问：prepare_namespace()  init/do_mounts.c
        ├── wait_for_device_probe()
        ├── mount_root()
        ├── devtmpfs_mount()
        └── 移动挂载与 init_chroot()
```

其中 `rootfs_initcall()` 表示框架登记与异步任务调度，不是 `kernel_init_freeable()` 直接调用解包函数。

### rootfs 先于归档解包存在

`init_mount_tree()` 创建 rootfs 挂载实例，建立初始挂载命名空间，并把当前任务的根目录与工作目录都指向它，见 [`fs/namespace.c:6104-6128`](../../linux/fs/namespace.c#L6104-L6128)。随后 initramfs 解包只是向这个已存在的文件系统填充目录、普通文件、符号链接和设备节点。

当前 `CONFIG_TMPFS=y`，但 rootfs 不一定总使用 tmpfs。`init_rootfs()` 会参考 `root=` 与 `rootfstype=`；不选择 tmpfs 时使用 ramfs，见 [`init/do_mounts.c:498-521`](../../linux/init/do_mounts.c#L498-L521)。

### 先解包内置归档，再尝试外部镜像

`do_populate_rootfs()` 先解包内置归档，再根据 `initrd_start` 与配置判断是否处理外部镜像，见 [`init/initramfs.c:718-742`](../../linux/init/initramfs.c#L718-L742)。外部镜像解包失败时，只有内建 `CONFIG_BLK_DEV_RAM=y` 的分支才把它保存为 `/initrd.image`，供传统 initrd 路径处理；其他情况打印解包失败错误。保存分支使用 `#ifdef CONFIG_BLK_DEV_RAM`，见 [`init/initramfs.c:696-742`](../../linux/init/initramfs.c#L696-L742)。

当前 `CONFIG_BLK_DEV_RAM=m`，**不会编入上述保存分支**。这和使用 `IS_ENABLED(CONFIG_BLK_DEV_RAM)` 选择的尝试解包日志不是同一条件，不能凭日志里的 initrd 字样推断旧格式启动可行。内核直接挂载根所需的块设备与文件系统支持必须在使用前实际可用；现代 initramfs 则可以先由 `/init` 装入模块。

### 异步解包必须有明确等待点

当前解包默认可异步执行。注册函数本身只是调度工作：

[`init/initramfs.c:782-791`](../../linux/init/initramfs.c#L782-L791)（v6.18.60）

```c
static int __init populate_rootfs(void)
{
	initramfs_cookie = async_schedule_domain(do_populate_rootfs, NULL,
						 &initramfs_domain);
	usermodehelper_enable();
	if (!initramfs_async)
		wait_for_initramfs();
	return 0;
}
rootfs_initcall(populate_rootfs);
```

`initramfs_async=0` 使它在这里等待，但默认情况下解包可与后续初始化重叠。`kernel_init_freeable()` 在访问初始根之前还会显式调用 `wait_for_initramfs()`，见 [`init/main.c:1557-1571`](../../linux/init/main.c#L1557-L1571)。因此 `populate_rootfs()` 的 initcall 耗时可能只是排队时间，不能代表完整解包耗时。

### `/init` 决定谁负责挂载最终根

等待完成后，内核打开初始 `/dev/console`，并通过 `init_eaccess()` 检查 `ramdisk_execute_command`。若不可访问，清空该变量并调用 `prepare_namespace()`，见 [`init/main.c:1557-1571`](../../linux/init/main.c#L1557-L1571)。这里是路径访问性检查，不是已经验证执行一定成功；实现仅按 `MAY_ACCESS` 检查路径权限，见 [`fs/init.c:113-125`](../../linux/fs/init.c#L113-L125)。

- **有可用 `/init` 或 `rdinit=` 程序**：后续先尝试执行它。装载驱动、解锁加密卷、组装存储和切换最终根等工作可交给早期用户空间（Early Userspace）。
- **没有可用程序**：内核走直接挂载路径，解析 `root=` 并挂载根，然后尝试普通 init 候选。

所以“传了 `root=` 就一定由内核先挂载该分区”是错误的；有 initramfs `/init` 时，它也可能只作为用户空间脚本的输入策略。

### 内核直接挂载路径的等待与切换

`prepare_namespace()` 先处理 `rootdelay=`，等待已知设备探测完成，解析根设备，再依据 `rootwait` 等策略等待根设备，最终调用 `mount_root()`。挂载后执行设备文件系统挂载、挂载移动与根目录切换，见 [`init/do_mounts.c:464-496`](../../linux/init/do_mounts.c#L464-L496)。

`rootdelay=` 是无条件延时，`rootwait` 是等待设备出现，不能把它们当作同一种计时器。无限等待还可能掩盖错误设备名，应结合日志检查根设备解析结果。

## 关键细节 / 设计考量

### devtmpfs 自动挂载有调用路径限制

当前 `CONFIG_DEVTMPFS=y`、`CONFIG_DEVTMPFS_MOUNT=y`。自动挂载发生在内核直接挂载根的 `prepare_namespace()` 路径中，见 [`drivers/base/devtmpfs.c:357-377`](../../linux/drivers/base/devtmpfs.c#L357-L377)。执行 initramfs `/init` 时不能因此假定 `/dev` 已由这一调用挂上 devtmpfs，用户空间通常需要自行处理。

### 原镜像与解包文件不是同一份对象

解包后文件内容已经进入 rootfs，相应外部原始归档范围可以释放。清理位置见 [`init/initramfs.c:744-760`](../../linux/init/initramfs.c#L744-L760)。日志中的“释放 initrd 内存”并不等于把解包后的 `/init` 删除。

## 可观测性

| 入口 | 含义与限制 | 源码与条件 |
|---|---|---|
| 解包与解包失败日志 | 区分开始处理外部归档与失败；普通开始日志不能单独证明已完成 | [`init/initramfs.c:727-742`](../../linux/init/initramfs.c#L727-L742)，`CONFIG_PRINTK=y`、`CONFIG_BLK_DEV_INITRD=y` |
| `initcall` 的 `populate_rootfs` 事件 | 可定位异步任务发起时间，不等于工作线程全部完成 | [`init/initramfs.c:782-791`](../../linux/init/initramfs.c#L782-L791)，`CONFIG_EVENT_TRACING=y` |
| `/proc/1/mountinfo`、`/proc/self/mountinfo` | 分别观察 PID 1 与当前任务的挂载命名空间；登录后通常已看不到最早的根布局 | [`fs/proc/base.c:3334`](../../linux/fs/proc/base.c#L3334)、[`fs/proc_namespace.c:135-190`](../../linux/fs/proc_namespace.c#L135-L190)，`CONFIG_PROC_FS=y` |
| `/proc/cmdline` | 核实 `root=`、`rootwait`、`rdinit=` 与 `initramfs_async=` 是否实际生效于本次启动输入 | [`fs/proc/cmdline.c:7-24`](../../linux/fs/proc/cmdline.c#L7-L24)，`CONFIG_PROC_FS=y` |

本节解包与根切换入口多数带 `__init`，不适合登录后才安装探针。若要观察早期 rootfs，应使用一次性实验 initramfs 或启动期跟踪，不能把当前容器里的挂载视图直接当作宿主机启动时的视图。

## 动手实验

先在目标 dev 虚拟机做只读检查；未实测：

```bash
uname -r
findmnt /
findmnt /dev
head -n 15 /proc/1/mountinfo
dmesg | grep -Ei 'initramfs|initrd|VFS:|devtmpfs|Run .* as init'
```

这些结果用于确定最终状态与日志顺序，不能直接证明早期 `/init` 看到的所有目录。完整的一次性 initramfs 实验见 [1.11](11-userspace.md)。

## 小结

- rootfs 先被创建，initramfs 再把归档文件解包到其中。
- 启动交接中的 initrd 字段也可承载现代 cpio initramfs。
- `/init` 是否可访问，决定优先交给早期用户空间还是进入内核直接挂载路径。
- 异步解包必须在访问初始文件之前等待，回调返回时间不是完整解包时间。
- devtmpfs 自动挂载与原始归档释放都有明确路径和生命周期边界。

## 参考

- [`Documentation/filesystems/ramfs-rootfs-initramfs.rst`](../../linux/Documentation/filesystems/ramfs-rootfs-initramfs.rst)。
- [`Documentation/driver-api/early-userspace/early_userspace_support.rst`](../../linux/Documentation/driver-api/early-userspace/early_userspace_support.rst)。
- [`Documentation/admin-guide/initrd.rst`](../../linux/Documentation/admin-guide/initrd.rst)。
