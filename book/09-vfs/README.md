# 第 9 章 虚拟文件系统

本章以 Linux v6.18.60、x86_64 为基准，分析虚拟文件系统（Virtual File System，VFS）如何把统一的文件接口连接到不同文件系统。阅读主线是：**文件系统注册与挂载 → 路径名查找 → 文件打开 → 读写分派 → page cache → 预读、写回与文件映射**，最后用目录操作与 ramfs 串联通用接口。

先区分四类对象：超级块（Superblock，`struct super_block`）描述文件系统实例，`struct inode` 描述文件对象，目录项（Directory Entry，dentry）关联名字与 inode，`struct file` 保存一次打开的状态。对象定义分别见 [`include/linux/fs.h:1446`](../../linux/include/linux/fs.h#L1446)、[`include/linux/fs.h:793`](../../linux/include/linux/fs.h#L793)、[`include/linux/dcache.h:92`](../../linux/include/linux/dcache.h#L92) 与 [`include/linux/fs.h:1211`](../../linux/include/linux/fs.h#L1211)。

当前 9.1～9.12 均已有初稿。新增的 9.9～9.12 已按仓库源码核对主流程、引用与可观测性入口；实验尚未在 `dev` 虚拟机验证。9.1～9.8 仍需按写作规范进一步校核。

## 阅读建议

- **先建立对象关系**：从 9.1 入手，区分 inode、dentry、打开文件与挂载，避免把文件描述符等同于文件对象。
- **沿名字找到对象**：9.2 介绍文件系统实例和挂载树，9.3 介绍路径名查找（Pathname Lookup）。遇到缓存和引用计数细节时，可对照 9.4 的目录项缓存（Dentry Cache，dcache）与 9.5 的 inode 缓存。
- **沿描述符找到数据**：9.6 介绍文件描述符（File Descriptor，fd）与打开文件描述（Open File Description）的关系，9.7 介绍读写接口的分派，9.8 进入 page cache 的组织和缓冲读实现。
- **对照相邻章节**：第 4 章提供 folio、内存回收和文件映射的背景，第 7 章提供锁、引用计数和 RCU 的背景，第 10 章接续分析块 I/O。并非所有文件系统都依赖块设备，本章不以某一种磁盘文件系统的布局为前提。
- **沿缓存追到一致性**：9.9 说明访问模式如何转化为预读窗口，9.10 连接脏页、后台工作、限速与文件同步，9.11 解释文件缺页、共享写与截断。9.12 再用 ramfs 展示目录操作和通用助手的边界。

## 小节索引

| 小节 | 主要内容 | 状态 |
|---|---|---|
| [9.1 VFS 概述：通用文件模型与核心对象](01-overview.md) | 核心对象、操作表、读路径概览与缓存初始化 | 初稿，待校核 |
| [9.2 文件系统注册与挂载](02-mount.md) | 文件系统类型、上下文、超级块、挂载树与卸载 | 初稿，待校核 |
| [9.3 路径名查找](03-path-walk.md) | 路径分量、RCU / 引用查找、符号链接与挂载点穿越 | 初稿，待校核 |
| [9.4 目录项缓存](04-dcache.md) | dentry 分配、查找、别名、引用释放与回收 | 初稿，待校核 |
| [9.5 inode 与 inode 缓存](05-inode.md) | inode 查找、初始化协议、引用计数与驱逐 | 初稿，待校核 |
| [9.6 文件打开、文件描述符与 struct file](06-open.md) | 打开流程、fd 表安装、引用获取与关闭 | 初稿，待校核 |
| [9.7 读写路径：kiocb、iov_iter 与缓冲 I/O](07-read-write.md) | 读写分派、迭代器、缓冲写与同步语义入口 | 初稿，待校核 |
| [9.8 page cache：address_space 与 filemap](08-page-cache.md) | 缓存索引、folio 状态、缓冲读、等待与截断 | 初稿，待校核 |
| [9.9 文件预读：访问模式与预读窗口](09-readahead.md) | 同步与异步预读、顺序访问检测、窗口增长、大 folio 与访问提示 | 初稿，实验待验证 |
| [9.10 脏页写回与文件同步](10-writeback.md) | 脏状态与队列、后台写回、前台限速、同步语义及错误游标 | 初稿，实验待验证 |
| [9.11 文件内存映射：缺页、共享写与截断](11-mmap.md) | 映射回调、文件缺页、邻近页映射、私有与共享写、截断和同步 | 初稿，实验待验证 |
| [9.12 目录操作与 ramfs](12-directory-ramfs.md) | 创建、硬链接、删除、重命名、目录锁及内存文件系统的存储边界 | 初稿，实验待验证 |

## 源码导航

| 主题 | 主要源码 |
|---|---|
| 核心对象与操作表 | [`include/linux/fs.h`](../../linux/include/linux/fs.h)、[`include/linux/dcache.h`](../../linux/include/linux/dcache.h)、[`include/linux/fdtable.h`](../../linux/include/linux/fdtable.h) |
| 注册、超级块与挂载 | [`fs/filesystems.c`](../../linux/fs/filesystems.c)、[`fs/fs_context.c`](../../linux/fs/fs_context.c)、[`fs/super.c`](../../linux/fs/super.c)、[`fs/namespace.c`](../../linux/fs/namespace.c) |
| 路径名查找与对象缓存 | [`fs/namei.c`](../../linux/fs/namei.c)、[`fs/dcache.c`](../../linux/fs/dcache.c)、[`fs/inode.c`](../../linux/fs/inode.c) |
| 文件打开、描述符与释放 | [`fs/open.c`](../../linux/fs/open.c)、[`fs/file.c`](../../linux/fs/file.c)、[`fs/file_table.c`](../../linux/fs/file_table.c) |
| 读写分派与 page cache | [`fs/read_write.c`](../../linux/fs/read_write.c)、[`lib/iov_iter.c`](../../linux/lib/iov_iter.c)、[`mm/filemap.c`](../../linux/mm/filemap.c)、[`mm/truncate.c`](../../linux/mm/truncate.c) |
| 预读、写回、同步与 ramfs | [`mm/readahead.c`](../../linux/mm/readahead.c)、[`mm/page-writeback.c`](../../linux/mm/page-writeback.c)、[`fs/fs-writeback.c`](../../linux/fs/fs-writeback.c)、[`fs/sync.c`](../../linux/fs/sync.c)、[`fs/ramfs/`](../../linux/fs/ramfs/) |

## 待完善

- [x] 补写 9.9～9.12，同步本章索引、总目录与术语表，并加入独立的可观测性说明。
- [ ] 在 `dev` 虚拟机验证 9.9～9.12 的新增实验，记录内核版本、文件系统类型、实际事件与输出；现有预期不作为实测结论。
- [ ] 为 9.2～9.8 补充独立的「可观测性」一节；现有「动手实验」不能替代观测入口、字段含义、源码位置及配置依赖的说明。
- [ ] 逐节复核源码摘录、行号、调用链和历史版本说明，将正文与参考中的纯文本源码位置统一为仓库内相对链接。
- [ ] 逐节核对依赖配置的行为，以 `linux/.config` 的实际取值为准，避免把编译支持等同于运行时启用。
- [ ] 在 `dev` 虚拟机中验证现有实验，补充环境、真实输出与解释；挂载、卸载和缓存回收实验应在隔离环境中执行。
- [ ] 在最终渲染器中检查图表布局与编号，并继续补齐本章术语登记。

## 小结

- 本章从挂载、查找与打开进入读写，再沿 page cache 分析预读、写回和映射，最后回到目录语义与 ramfs。
- 9.1～9.8 不能视为已完成规范校核；可观测性、引用格式和实验验证仍需完善。
- 9.9～9.12 已补齐源码分析与观测入口，实验步骤及源码推导的预期仍待虚拟机验证。

## 参考

- [`Documentation/filesystems/vfs.rst`](../../linux/Documentation/filesystems/vfs.rst)：VFS 对象与操作表。
- [`Documentation/filesystems/mount_api.rst`](../../linux/Documentation/filesystems/mount_api.rst)：文件系统上下文与挂载 API。
- [`Documentation/filesystems/path-lookup.rst`](../../linux/Documentation/filesystems/path-lookup.rst)：路径名查找及并发规则。
- [`Documentation/filesystems/locking.rst`](../../linux/Documentation/filesystems/locking.rst)：VFS 操作表的加锁约定。
- [`Documentation/filesystems/directory-locking.rst`](../../linux/Documentation/filesystems/directory-locking.rst)：目录操作的锁顺序。
