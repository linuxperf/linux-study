# 9.12 目录操作与 ramfs

> **本节源码**：[`fs/namei.c`](../../linux/fs/namei.c)、[`fs/libfs.c`](../../linux/fs/libfs.c)、[`fs/ramfs/inode.c`](../../linux/fs/ramfs/inode.c)、[`fs/ramfs/file-mmu.c`](../../linux/fs/ramfs/file-mmu.c)、[`include/linux/fs.h`](../../linux/include/linux/fs.h)
> **关键函数**：`vfs_create()`、`vfs_mkdir()`、`vfs_unlink()`、`vfs_rename()`、`lock_rename()`、`ramfs_get_inode()`、`ramfs_mknod()`、`simple_unlink()`、`simple_rename()`
> **前置知识**：[9.2 注册与挂载](02-mount.md)、[9.3 路径名查找](03-path-walk.md)、[9.4 dcache](04-dcache.md)、[9.5 inode](05-inode.md)、[9.6 文件打开](06-open.md)、第 7 章同步与锁

## 概述

目录操作修改的是“父目录、名字、对象”之间的关系，不是简单修改某个打开文件的数据。VFS 需要在路径查找之后完成权限检查、目录加锁、文件系统回调和缓存更新，并防止并发删除、重命名把目录树变成不一致状态。硬链接（Hard Link）与已打开文件的生命周期又要求名字消失后，对象仍可能继续存在。

ramfs 把目录树与数据直接保存在 VFS 对象和 page cache 中，没有磁盘目录格式，因而适合观察这些通用机制的责任边界。本节先解释目录操作协议，再分析 ramfs 如何复用它，而不把这个最小实现当作可直接照搬的磁盘文件系统模板。其设计说明见 [`fs/ramfs/inode.c:11-24`](../../linux/fs/ramfs/inode.c#L11-L24)。

## 核心数据结构

### 父目录的操作表

创建、删除和重命名经父目录 inode 的 `i_op` 分派。`create()` 接收待实例化的 dentry；`unlink()` 删除一个名字；`rename()` 同时接收旧、新父目录与 dentry，定义见 [`include/linux/fs.h:2341-2362`](../../linux/include/linux/fs.h#L2341-L2362)。这与文件数据读写走 `file->f_op`、缓存页走 `mapping->a_ops` 是不同的接口层。

本版本尤其要核实 `mkdir()` 的返回类型：它返回 `struct dentry *`，不是旧式的 `int`。返回 `NULL` 表示成功且继续使用传入的 dentry，非错误指针可提供替代 dentry，错误则返回 `ERR_PTR()`；VFS 的处理见 [`fs/namei.c:4461-4500`](../../linux/fs/namei.c#L4461-L4500)。

### 名字、链接数与引用计数

| 状态 | 回答的问题 |
|---|---|
| `dentry->d_parent`、`d_name`、`d_inode` | 这个名字位于哪个目录、关联什么对象？ |
| `inode->i_nlink` | 文件系统命名空间还保留多少硬链接？ |
| inode、dentry、`struct file` 的引用 | 当前还有哪些内核或用户访问持有对象？ |
| 目录 `i_rwsem` | 哪些并发操作可以修改当前目录的名字集合？ |
| `super_block->s_vfs_rename_mutex` | 如何序列化跨目录重命名的拓扑与锁顺序判定？ |

dentry 与 inode 定义见 [`include/linux/dcache.h:92-127`](../../linux/include/linux/dcache.h#L92-L127)、[`include/linux/fs.h:793-890`](../../linux/include/linux/fs.h#L793-L890)。目录锁协议见 [`Documentation/filesystems/directory-locking.rst:5-66`](../../linux/Documentation/filesystems/directory-locking.rst#L5-L66)。链接数不是 inode 的内存引用计数，删除最后一个名字也不意味着立即释放对象。

### ramfs 的最小操作表

[`fs/ramfs/inode.c:190-202`](../../linux/fs/ramfs/inode.c#L190-L202)（v6.18.60）

```c
static const struct inode_operations ramfs_dir_inode_operations = {
	.create		= ramfs_create,
	.lookup		= simple_lookup,
	.link		= simple_link,
	.unlink		= simple_unlink,
	.symlink	= ramfs_symlink,
	.mkdir		= ramfs_mkdir,
	.rmdir		= simple_rmdir,
	.mknod		= ramfs_mknod,
	.rename		= simple_rename,
	.tmpfile	= ramfs_tmpfile,
};
```

ramfs 只实现少量对象分配逻辑，大量目录语义直接复用 `libfs` 助手。磁盘文件系统必须另外维护可恢复的目录记录、空间分配和事务，不能只装上这张表就获得持久化能力。

## 主流程分析

### 调用链总览

以下把创建目录、删除名字与重命名列为独立入口，不表示它们相互嵌套。入口与分派见 [`fs/namei.c:4502-4536`](../../linux/fs/namei.c#L4502-L4536)、[`fs/namei.c:4682-4800`](../../linux/fs/namei.c#L4682-L4800)、[`fs/namei.c:5291-5425`](../../linux/fs/namei.c#L5291-L5425)。

```text
mkdirat() → do_mkdirat()                     fs/namei.c
├── filename_create()                        fs/namei.c
│   └── 解析父目录、加锁、查找末级名字
└── vfs_mkdir()                              fs/namei.c
    └── ramfs_mkdir()                        fs/ramfs/inode.c
        └── ramfs_mknod()                    fs/ramfs/inode.c
            ├── ramfs_get_inode()            fs/ramfs/inode.c
            └── d_instantiate()、dget()

unlinkat() → do_unlinkat()                   fs/namei.c
└── vfs_unlink()                             fs/namei.c
    ├── simple_unlink()                      fs/libfs.c
    └── d_delete_notify()                    include/linux/fsnotify.h

renameat2() → do_renameat2()                 fs/namei.c
├── lock_rename()                            fs/namei.c
└── vfs_rename()                             fs/namei.c
    ├── simple_rename()                      fs/libfs.c
    └── d_move() / d_exchange()              fs/dcache.c
```

*图 9-13：ramfs 对通用目录操作路径的复用*

### 1. 创建：锁住父目录，再把负 dentry 变成正 dentry

创建不能只完成一次无锁路径查找后就假定名字仍不存在。`filename_create()` 及其助手在父目录锁保护下重新处理末级名字，并取得挂载写引用；`do_mkdirat()` 在此基础上执行路径安全检查和 `vfs_mkdir()`，见 [`fs/namei.c:4228-4297`](../../linux/fs/namei.c#L4228-L4297) 与 [`fs/namei.c:4502-4531`](../../linux/fs/namei.c#L4502-L4531)。

`vfs_mkdir()` 检查创建权限、操作支持、模式、安全钩子和最大链接数，再调用父目录的 `mkdir()`。成功后处理返回的 dentry 并发送目录创建通知，见 [`fs/namei.c:4461-4500`](../../linux/fs/namei.c#L4461-L4500)。普通文件的 `vfs_create()` 采用类似的权限检查与分派边界，见 [`fs/namei.c:3506-3530`](../../linux/fs/namei.c#L3506-L3530)。

ramfs 的实际创建由 `ramfs_mknod()` 完成：分配 inode，执行安全初始化，用 `d_instantiate()` 绑定 dentry，然后额外取得一个 dentry 引用，见 [`fs/ramfs/inode.c:98-120`](../../linux/fs/ramfs/inode.c#L98-L120)。关键两行如下。

[`fs/ramfs/inode.c:113-114`](../../linux/fs/ramfs/inode.c#L113-L114)（v6.18.60）

```c
		d_instantiate(dentry, inode);
		dget(dentry);	/* Extra count - pin the dentry in core */
```

这个额外引用不是性能缓存：ramfs 没有可供重新查找的磁盘目录，它必须让仍存在的名字保留在内存里。`ramfs_mkdir()` 再调整父目录链接数，并以 `ERR_PTR(0)` 即 `NULL` 表示沿用原 dentry 的成功，见 [`fs/ramfs/inode.c:122-135`](../../linux/fs/ramfs/inode.c#L122-L135)。

### 2. 查找与枚举：dcache 在这里就是目录内容

`simple_lookup()` 在通用查找没有找到已有名字时，把传入 dentry 建成负目录项，不会再访问磁盘，见 [`fs/libfs.c:65-84`](../../linux/fs/libfs.c#L65-L84)。这之所以成立，依赖已创建名字被额外引用保住，而不是因为所有文件系统都能从 dcache 推断某个名字不存在。

ramfs 目录使用 `simple_dir_operations`；其枚举实现从 dentry 子项中报告有效名字，见 [`fs/libfs.c:86-238`](../../linux/fs/libfs.c#L86-L238)。因此 dcache 在普通磁盘文件系统里通常是可丢弃缓存，在 ramfs 中却承担了命名空间的主要存储职责。

### 3. 硬链接与删除：修改链接数，不提前结束打开文件

`simple_link()` 增加 inode 的 `i_nlink`、获取 inode 引用，并把新 dentry 绑定到同一 inode；还获取新名字的 dentry 引用，见 [`fs/libfs.c:752-764`](../../linux/fs/libfs.c#L752-L764)。两个名字于是共享 inode 和文件数据，不是复制了文件内容。

删除时，`do_unlinkat()` 先解析父目录，在父目录锁下查找目标，`vfs_unlink()` 再锁定目标 inode，做权限、安全和挂载点检查后调用文件系统，见 [`fs/namei.c:4682-4800`](../../linux/fs/namei.c#L4682-L4800)。

[`fs/libfs.c:787-797`](../../linux/fs/libfs.c#L787-L797)（v6.18.60）

```c
int simple_unlink(struct inode *dir, struct dentry *dentry)
{
	struct inode *inode = d_inode(dentry);

	inode_set_mtime_to_ts(dir,
			      inode_set_ctime_to_ts(dir, inode_set_ctime_current(inode)));
	drop_nlink(inode);
	dput(dentry);
	return 0;
}
EXPORT_SYMBOL(simple_unlink);
```

这里的 `dput()` 对应释放名字额外持有的引用；后续 VFS 执行 dcache 删除与通知。`do_unlinkat()` 还保留一个临时 inode 引用，离开父目录锁之后才释放，避免耗时截断长期阻塞目录操作。

当最后一个名字消失但仍有打开文件时，`i_nlink` 可为零，而 `struct file` 仍持有对象，读写可继续。`simple_rmdir()` 则要求目录为空，还需调整目录及其父目录的链接数，见 [`fs/libfs.c:766-809`](../../linux/fs/libfs.c#L766-L809)。

### 4. 重命名：文件系统更新语义，VFS 更新缓存位置

`do_renameat2()` 解析两个父目录，要求两条路径位于同一挂载，并通过 `lock_rename()` 协调父目录锁。跨挂载返回 `-EXDEV`，不能把用户工具的“复制再删除”看成同一次 VFS 重命名，见 [`fs/namei.c:5291-5348`](../../linux/fs/namei.c#L5291-L5348)。

`vfs_rename()` 检查源和目标、权限、安全与链接数，再按规则锁定参与的子对象，然后调用文件系统 `rename()`。对未声明 `FS_RENAME_DOES_D_MOVE` 的文件系统，成功后由 VFS 执行 `d_move()` 或 `d_exchange()`，见 [`fs/namei.c:5126-5288`](../../linux/fs/namei.c#L5126-L5288)。

ramfs 的 `simple_rename()` 主要负责时间戳、覆盖目标及目录链接数。它支持 `RENAME_NOREPLACE`、`RENAME_EXCHANGE`，拒绝其他标志；实际是否允许覆盖目标，还需前面的 VFS 入口检查，不能孤立地只读该助手，见 [`fs/libfs.c:837-886`](../../linux/fs/libfs.c#L837-L886)。原子命名空间切换不等于崩溃后的持久化事务；ramfs 本身更不提供后者。

## 关键细节 / 设计考量

### 目录锁为何不能统一按 inode 地址排序

跨目录重命名可能改变祖先关系。只把所有目录按 inode 地址排序，不能保证拓扑检查和随后加锁期间关系不变。通用协议先用 `s_vfs_rename_mutex` 稳定跨目录判定，再按祖先优先等规则取得目录锁；需要同时锁多个非目录 inode 时才采用 inode 地址顺序，见 [`Documentation/filesystems/directory-locking.rst:5-66`](../../linux/Documentation/filesystems/directory-locking.rst#L5-L66)。

| 操作 | 主要锁规则摘要 |
|---|---|
| 读取目录 | 共享锁定所读目录 |
| 创建对象 | 独占锁定父目录 |
| 删除对象 | 先独占父目录，再锁目标 |
| 创建硬链接 | 先独占目标名字所在父目录，再锁源非目录 inode |
| 同目录重命名 | 独占父目录，再按类型和操作锁定需要的子对象 |
| 跨目录重命名 | 先取文件系统重命名锁，按祖先关系锁父目录，再按规则锁子对象 |

同目录情况下，`lock_rename()` 不获取文件系统重命名锁；跨目录才进入 `lock_two_directories()`，后者还拒绝当前没有共同祖先的情况，见 [`fs/namei.c:3353-3397`](../../linux/fs/namei.c#L3353-L3397)。表格是阅读入口，不能替代完整的锁协议与特殊文件系统约束。

### ramfs 的文件数据为何不能当普通缓存回收

`ramfs_get_inode()` 为 mapping 安装 `ram_aops` 并设置不可驱逐状态，普通文件复用通用读写与文件映射，见 [`fs/ramfs/inode.c:57-90`](../../linux/fs/ramfs/inode.c#L57-L90) 与 [`fs/ramfs/file-mmu.c:41-55`](../../linux/fs/ramfs/file-mmu.c#L41-L55)。

`ram_aops` 只有零填充读、简单写入和 `noop_dirty_folio()`，没有磁盘 `writepages()`；`noop_fsync()` 直接成功返回，见 [`fs/libfs.c:921-1016`](../../linux/fs/libfs.c#L921-L1016)、[`fs/libfs.c:1629-1633`](../../linux/fs/libfs.c#L1629-L1633)。文件数据没有磁盘副本，不能把它当成“随时丢掉、下次再读”的普通缓存。不可驱逐不等于文件删除后也无法释放内存。

ramfs 把 `drop_inode()` 设为总是返回 `1` 的 `inode_just_drop()`，因此最后一个内存引用释放后选择驱逐 inode；仍有名字的对象由前述 dentry 引用保住。卸载时，`ramfs_kill_sb()` 调用 `kill_litter_super()`，后者先通过 `d_genocide()` 清理固定的目录项，再销毁匿名超级块，见 [`fs/ramfs/inode.c:215-219`](../../linux/fs/ramfs/inode.c#L215-L219)、[`fs/inode.c:1854-1858`](../../linux/fs/inode.c#L1854-L1858)、[`fs/ramfs/inode.c:313-317`](../../linux/fs/ramfs/inode.c#L313-L317)、[`fs/super.c:1304-1310`](../../linux/fs/super.c#L1304-L1310)。

ramfs 在 [`fs/Makefile:73`](../../linux/fs/Makefile#L73) 中无条件构建，没有需要查找的 `CONFIG_RAMFS` 开关。MMU 文件操作由 `CONFIG_MMU=y` 选择，见 [`fs/ramfs/Makefile:5-10`](../../linux/fs/ramfs/Makefile#L5-L10)、[`linux/.config:353`](../../linux/.config#L353)。

### ramfs 不等于 tmpfs，也没有有效的 size 挂载上限

ramfs 只明确解析根目录的 `mode`，还保留了忽略某些未知挂载选项的兼容行为。因此传入 `size=...` 并不表示真的获得容量限制，见 [`fs/ramfs/inode.c:221-263`](../../linux/fs/ramfs/inode.c#L221-L263)。其数据不能换出到交换空间；tmpfs 才提供相应的容量限制与交换能力，两者区别见 [`Documentation/filesystems/ramfs-rootfs-initramfs.rst:62-73`](../../linux/Documentation/filesystems/ramfs-rootfs-initramfs.rst#L62-L73)。

这也是实验必须限制写入总量、禁止给不可信程序无限制写权限的原因。`fsync()` 在 ramfs 上成功，只表示这个内存文件系统没有待完成的持久存储动作，不表示数据能跨越重启保留。

## 可观测性

`/proc` 依赖 `CONFIG_PROC_FS=y`，见 [`linux/.config:11777`](../../linux/.config#L11777)。系统调用事件依赖 `CONFIG_FTRACE_SYSCALLS=y`，动态函数探测依赖 `CONFIG_KPROBES=y`、`CONFIG_KPROBE_EVENTS=y`，见 [`linux/.config:12963-12970`](../../linux/.config#L12963-L12970) 与 [`linux/.config:881`](../../linux/.config#L881)。

| 入口 | 看什么、怎样解释 | 源码依据 |
|---|---|---|
| `/proc/filesystems` | ramfs 是否已注册；`nodev` 表示挂载不要求块设备，不表示不能保存文件内容 | [`fs/filesystems.c:241-263`](../../linux/fs/filesystems.c#L241-L263)、[`fs/ramfs/inode.c:313-331`](../../linux/fs/ramfs/inode.c#L313-L331) |
| `/proc/self/mountinfo` | 确认实验挂载类型与挂载位置；与宿主或其他命名空间区分 | [`fs/proc_namespace.c:135-193`](../../linux/fs/proc_namespace.c#L135-L193) |
| `stat()` / `fstat()` | 比较硬链接的 inode 号和 `st_nlink`，删除名字后用打开描述符继续观察；ramfs 普通文件的 `simple_getattr()` 经 `generic_fillattr()` 从 `i_ino`、`i_nlink` 填充输出 | [`fs/libfs.c:32-41`](../../linux/fs/libfs.c#L32-L41)、[`fs/stat.c:82-94`](../../linux/fs/stat.c#L82-L94) |
| `syscalls:sys_enter_mkdirat`、`syscalls:sys_exit_mkdirat`，以及 `unlinkat`、`renameat2` 的对应事件 | 系统调用边界、参数与返回值；用户库也可能使用 `mkdir`、`unlink`、`rename` 等入口，须以实际事件为准 | [`fs/namei.c:4533-4541`](../../linux/fs/namei.c#L4533-L4541)、[`fs/namei.c:4803-4816`](../../linux/fs/namei.c#L4803-L4816)、[`fs/namei.c:5427-5446`](../../linux/fs/namei.c#L5427-L5446) |
| 动态探针 `vfs_mkdir()`、`vfs_unlink()`、`vfs_rename()` | 统一的文件系统分派层；注意 `vfs_mkdir()` 的返回值是指针或错误指针，不能用整数零非零规则粗判成功 | [`fs/namei.c:4461-4500`](../../linux/fs/namei.c#L4461-L4500)、[`fs/namei.c:4682-4725`](../../linux/fs/namei.c#L4682-L4725)、[`fs/namei.c:5126-5288`](../../linux/fs/namei.c#L5126-L5288) |
| `CONFIG_PROVE_LOCKING` | 本配置未启用，不应预期实验自动提供完整 lockdep 锁顺序诊断 | [`linux/.config:12864`](../../linux/.config#L12864) |

对 `ramfs_mknod()` 等静态函数，可先检查运行内核是否保留了可探测符号；不要仅因为源码有定义就假定能挂探针。系统调用跟踪能说明操作发生过，但不能直接证明锁持有时间或 dcache 引用数。

## 动手实验

在 `dev` 虚拟机中，以管理员权限创建独立挂载命名空间，再把挂载传播改为私有。实验只写入几个字节，退出时卸载并清理目录；不要用 ramfs 做无界写入压力测试。需要 `unshare`、`mount` 和 Python 3。

```bash
sudo unshare --mount --fork bash <<'SH'
set -eu
mount --make-rprivate /
mnt=$(mktemp -d /mnt/ramfs-study.XXXXXX)
trap 'umount "$mnt"; rmdir "$mnt"' EXIT
mount -t ramfs -o mode=0700 ramfs "$mnt"
python3 - "$mnt" <<'PY'
import os, sys
root = sys.argv[1]
os.mkdir(root + "/left")
os.mkdir(root + "/right")
a, b = root + "/left/a", root + "/left/b"
c = root + "/right/c"
fd = os.open(a, os.O_CREAT | os.O_RDWR | os.O_EXCL, 0o600)
os.write(fd, b"ramfs\n")
os.link(a, b)
print("same inode:", os.stat(a).st_ino == os.stat(b).st_ino)
print("two names:", os.fstat(fd).st_nlink)
os.rename(b, c)
os.unlink(a)
os.unlink(c)
print("no names:", os.fstat(fd).st_nlink)
print("still readable:", os.pread(fd, 6, 0))
os.fsync(fd)
os.close(fd)
os.rmdir(root + "/left")
os.rmdir(root + "/right")
PY
SH
```

预期两个硬链接的 inode 号相同，链接数先为 `2`；跨目录重命名保持文件身份不变，删除两个名字后链接数变成 `0`，但打开文件仍能读取原数据。这些是源码推导的预期，尚未在项目虚拟机中采集真实输出。若需记录调用，可在 Python 命令前加 `strace -f -e trace=openat,mkdir,mkdirat,link,linkat,rename,renameat,renameat2,unlink,unlinkat,rmdir,fsync`，再与上述 VFS 分派入口对照。

## 小结

- 目录操作以父目录 inode 为分派中心，先稳定名字和锁，再调用文件系统。
- 本版本 `mkdir()` 返回 dentry 指针，成功与错误约定不能套用旧版整数接口。
- `i_nlink` 表示名字数量，不表示内存引用；无名字的打开文件仍可使用。
- 跨目录重命名必须处理拓扑与锁顺序，VFS 和文件系统各自承担不同更新职责。
- ramfs 的 dcache 与 page cache 是主要存储，不是可任意丢弃的磁盘副本。
- ramfs 的同步成功不代表持久化，也不提供 tmpfs 那样的容量与交换语义。

## 参考

- [`Documentation/filesystems/directory-locking.rst`](../../linux/Documentation/filesystems/directory-locking.rst)：目录操作类别、跨目录重命名与死锁规避。
- [`Documentation/filesystems/locking.rst`](../../linux/Documentation/filesystems/locking.rst)：inode 操作表的锁要求。
- [`Documentation/filesystems/ramfs-rootfs-initramfs.rst`](../../linux/Documentation/filesystems/ramfs-rootfs-initramfs.rst)：ramfs、tmpfs、rootfs 与 initramfs 的区别。
- [`fs/ramfs/inode.c`](../../linux/fs/ramfs/inode.c)、[`fs/libfs.c`](../../linux/fs/libfs.c)：最小目录语义与通用助手实现。
