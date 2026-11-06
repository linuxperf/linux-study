# 2.5 资源继承：共享、复制与写时复制

> **本节源码**：[`kernel/fork.c`](../../linux/kernel/fork.c)、[`mm/mmap.c`](../../linux/mm/mmap.c)、[`mm/memory.c`](../../linux/mm/memory.c)、[`fs/file.c`](../../linux/fs/file.c)、[`include/linux/sched/signal.h`](../../linux/include/linux/sched/signal.h)
> **关键函数**：`copy_mm()`、`dup_mm()`、`dup_mmap()`、`copy_files()`、`copy_fs()`、`copy_sighand()`、`copy_signal()`、`mm_release()`
> **前置知识**：2.4 任务创建；虚拟内存与文件描述符基础

## 概述

如果每次创建任务都立即复制全部内存和文件内容，创建成本会与资源总量绑定；如果所有对象一律共享，独立进程又无法隔离自身修改。Linux 按资源种类分别选择共享对象、复制容器或延迟复制内容。普通 fork 为子任务建立独立地址空间，但普通私有页通过写时复制（Copy-On-Write，COW）推迟实际复制。文件表、信号表和目录上下文则有各自的共享标志与引用计数规则。

## 核心数据结构

| 资源 | 共享选择 | 不共享时的处理 | 源码 |
|---|---|---|---|
| `mm_struct` | `CLONE_VM` | `dup_mm()`、`dup_mmap()` 建立独立映射关系 | [`kernel/fork.c:1479-1556`](../../linux/kernel/fork.c#L1479-L1556) |
| `files_struct` | `CLONE_FILES` | `dup_fd()` 复制表，底层 `struct file` 仍取引用 | [`kernel/fork.c:1579-1608`](../../linux/kernel/fork.c#L1579-L1608)、[`fs/file.c:434-464`](../../linux/fs/file.c#L434-L464) |
| `fs_struct` | `CLONE_FS` | `copy_fs_struct()` 复制根、工作目录等上下文 | [`kernel/fork.c:1558-1577`](../../linux/kernel/fork.c#L1558-L1577) |
| `sighand_struct` | `CLONE_SIGHAND` | 分配并复制 `action[]`，可按 `CLONE_CLEAR_SIGHAND` 重置 | [`kernel/fork.c:1610-1633`](../../linux/kernel/fork.c#L1610-L1633) |
| `signal_struct` | `CLONE_THREAD` | 分配线程组公共状态，初始化共享待决集合、等待队列和计数 | [`kernel/fork.c:1659-1700`](../../linux/kernel/fork.c#L1659-L1700) |
| 私有待决信号 | 不作为普通继承内容 | 新任务重新初始化 `pending` | [`kernel/fork.c:2079-2087`](../../linux/kernel/fork.c#L2079-L2087) |

“复制”需要指出层级：复制 fd 表不等于复制打开文件描述，复制页表关系也不等于复制全部物理页内容。

## 主流程分析

```text
copy_process()                          kernel/fork.c
├── copy_files()
│   ├── CLONE_FILES：增加 files_struct 引用
│   └── 否则 dup_fd()                   fs/file.c
├── copy_fs()
├── copy_sighand() / copy_signal()
└── copy_mm()
    ├── CLONE_VM：mmget(oldmm)
    └── 否则 dup_mm()
        └── dup_mmap()                  mm/mmap.c
            ├── 复制 VMA 与相关引用
            └── copy_page_range()      mm/memory.c
                └── 普通 COW PTE 的写保护与共享
```

### 一、地址空间共享与复制的分支

[`kernel/fork.c:1541-1555`](../../linux/kernel/fork.c#L1541-L1555)（v6.18.60）

```c
	if (clone_flags & CLONE_VM) {
		mmget(oldmm);
		mm = oldmm;
	} else {
		mm = dup_mm(tsk, current->mm);
		if (!mm)
			return -ENOMEM;
	}

	tsk->mm = mm;
	tsk->active_mm = mm;
	sched_mm_cid_fork(tsk);
	return 0;
```

`CLONE_VM` 直接共享同一个地址空间对象，因而一方的映射操作可以影响另一方。普通 fork 则建立新的 `mm_struct`；父子后续 mmap / munmap 操作不直接修改对方的映射树。

### 二、普通 fork 复制映射关系，不立即复制全部页内容

`dup_mmap()` 复制映射树与虚拟内存区域（Virtual Memory Area，VMA），设置新 VMA 的 `vm_mm` 并取得文件等引用，见 [`mm/mmap.c:1745-1834`](../../linux/mm/mmap.c#L1745-L1834)。该函数位于本版本的 `mm/mmap.c`，不能沿用其他版本把它定位在 `kernel/fork.c` 的行号。

对于需要 COW 的可写私有映射，页表复制路径对父映射写保护，并把子映射也设置为不可写：

[`mm/memory.c:1097-1118`](../../linux/mm/memory.c#L1097-L1118)（v6.18.60）

```c
static __always_inline void __copy_present_ptes(struct vm_area_struct *dst_vma,
		struct vm_area_struct *src_vma, pte_t *dst_pte, pte_t *src_pte,
		pte_t pte, unsigned long addr, int nr)
{
	struct mm_struct *src_mm = src_vma->vm_mm;

	/* If it's a COW mapping, write protect it both processes. */
	if (is_cow_mapping(src_vma->vm_flags) && pte_write(pte)) {
		wrprotect_ptes(src_mm, addr, src_pte, nr);
		pte = pte_wrprotect(pte);
	}

	/* If it's a shared mapping, mark it clean in the child. */
	if (src_vma->vm_flags & VM_SHARED)
		pte = pte_mkclean(pte);
	pte = pte_mkold(pte);

	if (!userfaultfd_wp(dst_vma))
		pte = pte_clear_uffd_wp(pte);

	set_ptes(dst_vma->vm_mm, addr, dst_pte, pte, nr);
}
```

之后写访问进入写保护缺页路径，可能独占复用原页，也可能由 `wp_page_copy()` 复制，见 [`mm/memory.c:4063-4155`](../../linux/mm/memory.c#L4063-L4155)。所以“fork 后每次写都必然复制一页”也过于绝对。

共享映射没有因此变成私有映射。`VM_DONTCOPY` 的区域被跳过，`VM_WIPEONFORK` 的区域不复制原内容，相关分支见 [`mm/mmap.c:1753-1800`](../../linux/mm/mmap.c#L1753-L1800)、[`mm/mmap.c:1833-1834`](../../linux/mm/mmap.c#L1833-L1834)。页表和反向映射细节留到 [4.9](../04-memory/09-cow-rmap.md)。

### 三、fd 表独立不等于文件位置独立

普通 fork 的 `dup_fd()` 对每个有效槽位执行 `get_file()`，然后把相同 `struct file` 指针写入新表，见 [`fs/file.c:455-463`](../../linux/fs/file.c#L455-L463)。父子可以分别关闭自己的 fd 槽位，但仍可能共享文件位置、文件状态标志等打开文件描述（Open File Description）的状态。

若使用 `CLONE_FILES`，连 fd 表本身都共享：一方关闭或新建槽位会影响共同的表。`CLONE_FS` 又是另一件事，它共享根目录、当前目录和 umask，而不是共享 fd 表。

### 四、信号处理方式、线程组状态与线程掩码分层

`sighand_struct.action[]` 保存处理方式；`signal_struct.shared_pending` 保存线程组共享待决信号；每个任务还有自己的 `pending` 和 `blocked`。结构定义见 [`include/linux/sched/signal.h:22-27`](../../linux/include/linux/sched/signal.h#L22-L27)、[`include/linux/sched/signal.h:95-121`](../../linux/include/linux/sched/signal.h#L95-L121)、[`include/linux/sched.h:1197-1204`](../../linux/include/linux/sched.h#L1197-L1204)。

新线程的阻塞掩码继承创建者当时的值，但之后各线程可分别调整；这与所有线程共享信号处理方式不矛盾。普通新任务的私有待决信号被清空。进程组广播信号与 fork 并发还有延迟信号收集协议，不能把“待决集合不继承”扩展成“创建期间不可能出现任何新待决信号”，见 [`kernel/fork.c:2010-2027`](../../linux/kernel/fork.c#L2010-L2027)、[`kernel/fork.c:2392`](../../linux/kernel/fork.c#L2392)。

## 关键细节 / 设计考量

### vfork 暂停的是调用线程，不是整个线程组

`vfork()` 共享地址空间，并通过 `vfork_done` 完成量使创建者等待。`kernel_clone()` 在子任务唤醒前初始化完成量，之后等待，见 [`kernel/fork.c:2633-2655`](../../linux/kernel/fork.c#L2633-L2655)。其他线程并不因此自动暂停。

子任务 exec 或退出、解除旧地址空间关联时，`mm_release()` 完成该通知，见 [`kernel/fork.c:1424-1467`](../../linux/kernel/fork.c#L1424-L1467)。这不是等待子进程整个生命周期结束；exec 成功切换后父侧就可以继续。子侧不应把 vfork 当成普通 fork 去修改共享栈、调用任意复杂库代码或从调用函数返回。

### 线程退出通知与父进程 wait 是两条协议

`CLONE_CHILD_CLEARTID` 设置 `clear_child_tid`。正常线程退出关联的 `mm_release()` 在满足 `mm_users>1` 时，向该用户地址写 0 并执行 futex 唤醒，见 [`kernel/fork.c:1436-1446`](../../linux/kernel/fork.c#L1436-L1446)。这是线程库实现加入等待的重要内核基础，但不是完整的 `pthread_join()` 实现，也不等于父进程的 `wait4()`。

不要把源码注释与实际条件混为一谈：本版本此处的显式条件是 `clear_child_tid` 和 `mm_users`；core dump 等更早路径可能影响这些状态，应按实际调用链解释。

## 可观测性

| 入口 | 看什么、怎么解读 | 源码与配置 |
|---|---|---|
| `task:task_newtask` | `clone_flags` 表示共享请求，不直接证明两个任务之后一直共享 | [`include/trace/events/task.h:9-32`](../../linux/include/trace/events/task.h#L9-L32)，`CONFIG_EVENT_TRACING=y` |
| `/proc/<pid>/maps` | VMA 布局；父子布局相似不证明 `mm` 相同，更不证明物理页全部独立 | [`fs/proc/task_mmu.c`](../../linux/fs/proc/task_mmu.c)，`CONFIG_PROC_FS=y`、`CONFIG_MMU=y` |
| `/proc/<pid>/fd`、`fdinfo` | fd 指向对象与文件位置等；路径相同并不足以证明是同一打开文件描述 | [`fs/proc/fd.c`](../../linux/fs/proc/fd.c)，`CONFIG_PROC_FS=y` |
| `kcmp()` 的 `KCMP_VM` / `KCMP_FILES` | 在权限允许时比较资源对象身份，比比较 maps 文本更直接 | [`kernel/kcmp.c`](../../linux/kernel/kcmp.c)、[`include/uapi/linux/kcmp.h`](../../linux/include/uapi/linux/kcmp.h)，`CONFIG_KCMP=y` |
| `dup_mm()`、`dup_mmap()`、`mm_release()` 探针 | 复制映射开销与旧地址空间交接；后者不是单纯对象引用减一 | 定义见本节主流程，`CONFIG_KPROBES=y`；static 函数是否可挂取决于构建后的符号 |

## 小结

- 克隆标志逐类决定资源关系，不存在一个统一的“共享全部进程资源”开关。
- 普通 fork 复制地址空间关系，COW 推迟普通私有页内容复制。
- 独立 fd 表仍可指向同一个 `struct file`，共享打开文件位置。
- 信号处理方式、线程组公共状态与各线程掩码必须分层理解。
- vfork 的旧 mm 交接、线程清 TID 通知和父进程 wait 是不同协议。

## 参考

- [`Documentation/mm/process_addrs.rst`](../../linux/Documentation/mm/process_addrs.rst)。
- [4.7 进程地址空间](../04-memory/07-address-space.md)、[4.9 写时复制与反向映射](../04-memory/09-cow-rmap.md)。
- [9.6 文件打开、文件描述符与 struct file](../09-vfs/06-open.md)。
