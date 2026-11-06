# 13.2 bpf 系统调用与对象生命周期

> **本节源码**：[`kernel/bpf/syscall.c`](../../linux/kernel/bpf/syscall.c)、[`kernel/bpf/inode.c`](../../linux/kernel/bpf/inode.c)、[`kernel/bpf/token.c`](../../linux/kernel/bpf/token.c)、[`include/linux/bpf.h`](../../linux/include/linux/bpf.h)
> **关键函数**：`__sys_bpf()`、`map_create()`、`bpf_prog_load()`、`bpf_prog_new_fd()`、`bpf_obj_pin_user()`、`bpf_prog_put()`
> **前置知识**：13.1 BPF 全景、7.2 引用计数、7.9 RCU

## 概述

BPF 程序可以由一个进程加载、由另一个进程查询，并在加载器退出后继续挂载。内核因此不能用加载进程的存活时间代表对象寿命，而要分别管理对象引用、进程内的文件描述符（File Descriptor，FD）和全局查询 ID。`bpf()` 使用命令与 `union bpf_attr` 承载这些操作。固定（pinning）到 BPF 文件系统（BPF filesystem，bpffs）则提供了不依赖加载器 FD 的引用入口，而不是把对象保存成磁盘文件。

## 核心数据结构

| 字段或接口 | 作用 |
|---|---|
| `bpf_prog_aux.refcnt` | 程序引用计数，FD、挂载关系等可保持程序存活 |
| `bpf_prog_aux.id` | 内核对象查询 ID，不是持久引用 |
| `bpf_prog_aux.used_maps` / `used_map_cnt` | 程序持有的 map 依赖 |
| `bpf_map.refcnt` / `usercnt` | map 的对象引用与用户侧引用跟踪，不能简单等同于进程 FD 数 |
| `bpf_link.refcnt` / `prog` | 挂载对象的引用及关联程序；struct_ops 等特殊 link 可经其他对象持有程序 |
| `union bpf_attr` | 按命令解释的输入输出属性；不相关字段必须满足对应检查 |

对象字段见 [`include/linux/bpf.h:295-349`](../../linux/include/linux/bpf.h#L295-L349)、[`include/linux/bpf.h:1592-1690`](../../linux/include/linux/bpf.h#L1592-L1690) 与 [`include/linux/bpf.h:1751-1775`](../../linux/include/linux/bpf.h#L1751-L1775)。map 依赖移交到程序辅助结构的过程见 [`kernel/bpf/verifier.c:25418-25432`](../../linux/kernel/bpf/verifier.c#L25418-L25432)。

## 主流程分析

### 系统调用入口

```text
bpf()                                     kernel/bpf/syscall.c
└── __sys_bpf()
    ├── 检查属性尾部、复制并补零
    ├── security_bpf()
    ├── BPF_MAP_CREATE → map_create()
    │   └── map->ops->map_alloc()          各 map 实现
    ├── BPF_PROG_LOAD → bpf_prog_load()
    │   ├── 权限、类型、目标与输入检查
    │   ├── bpf_check()                    kernel/bpf/verifier.c
    │   ├── bpf_prog_select_runtime()      kernel/bpf/core.c
    │   ├── bpf_prog_alloc_id()
    │   └── bpf_prog_new_fd()
    └── BPF_OBJ_PIN → bpf_obj_pin()
        └── bpf_obj_pin_user()             kernel/bpf/inode.c
```

`__sys_bpf()` 首先检查用户属性中超出内核已知结构的尾部，清零本地属性，再复制输入。随后调用安全钩子并按命令分派，见 [`kernel/bpf/syscall.c:6163-6212`](../../linux/kernel/bpf/syscall.c#L6163-L6212)。这种处理允许接口扩展，但不表示旧内核会理解任意新增字段。

### 程序加载：先检查，再公开

加载前半段确认程序类型、指令数量、权限、挂载目标信息和许可证，随后复制指令并初始化程序。核心次序如下：

[`kernel/bpf/syscall.c:3085-3101`](../../linux/kernel/bpf/syscall.c#L3085-L3101)（v6.18.60）

```c
	err = security_bpf_prog_load(prog, attr, token, uattr.is_kernel);
	if (err)
		goto free_prog_sec;

	/* run eBPF verifier */
	err = bpf_check(&prog, attr, uattr, uattr_size);
	if (err < 0)
		goto free_used_maps;

	prog = bpf_prog_select_runtime(prog, &err);
	if (err < 0)
		goto free_used_maps;

	err = bpf_prog_alloc_id(prog);
	if (err)
		goto free_used_maps;
```

分配 ID 后，对象已经可能被其他进程按 ID 找到，失败处理因此必须走引用释放，而不能再当作尚未公开的私有内存直接销毁。源码在通知 perf、审计并创建程序 FD 前特别说明了这个并发窗口，见 [`kernel/bpf/syscall.c:3102-3123`](../../linux/kernel/bpf/syscall.c#L3102-L3123)。

`bpf_prog_new_fd()` 使用匿名 inode 创建文件对象，文件的 `private_data` 指向程序；关闭文件时 `bpf_prog_release()` 调用 `bpf_prog_put()`，见 [`kernel/bpf/syscall.c:2449-2456`](../../linux/kernel/bpf/syscall.c#L2449-L2456) 与 [`kernel/bpf/syscall.c:2535-2555`](../../linux/kernel/bpf/syscall.c#L2535-L2555)。

### FD、ID、pin 的区别

- **FD** 属于进程的文件表，引用一个文件对象；复制 FD 或跨进程传递 FD 不会创建新的 BPF 程序。
- **ID** 用于枚举与查询，不会仅因用户记住这个数字而保活对象。ID 来自可循环分配的 IDR，因此不能用作永久身份标识。
- **pin 路径** 在 bpffs inode 中保存对象引用，可通过 `BPF_OBJ_GET` 再取得 FD；它既不自动重新加载，也不会跨重启恢复。

程序 ID 的分配与移除见 [`kernel/bpf/syscall.c:2346-2382`](../../linux/kernel/bpf/syscall.c#L2346-L2382)；pin/get 路径见 [`kernel/bpf/inode.c:436-554`](../../linux/kernel/bpf/inode.c#L436-L554)。程序固定并不等于挂载固定：想保留由 link 表示的挂载，通常应固定那个 link，而不是只固定程序。

### 最后一个引用消失后

`__bpf_prog_put()` 在引用归零后进入释放流程。若处在中断或关中断环境，它先调度工作项；后续移除 ID、清理辅助资源，并按普通或可睡眠程序选择 RCU 回收方式，见 [`kernel/bpf/syscall.c:2384-2447`](../../linux/kernel/bpf/syscall.c#L2384-L2447)。

所以 `close()` 的直接效果是放下该文件持有的引用，不是保证该程序的机器码此刻已从内存消失。其他 FD、pin、挂载关系或对象依赖都可能延长寿命；并发执行路径也要求延迟回收。

## 关键细节 / 设计考量

### capability 与 token 不是“只检查 root”

加载路径通过 `bpf_token_capable()` 检查 `CAP_BPF`；网络管理类型还检查 `CAP_NET_ADMIN`，性能观测类型还检查 `CAP_PERFMON`。具体分支见 [`kernel/bpf/syscall.c:2914-2946`](../../linux/kernel/bpf/syscall.c#L2914-L2946)。非特权创建受 `kernel.unprivileged_bpf_disabled` 控制，但持有现有对象 FD 后的操作仍按各自权限检查，不能把这个 sysctl 理解成总开关。

BPF 令牌（BPF token）用于受限委托。程序提交 token FD 后，内核先检查它允许的命令、程序类型和挂载类型；不满足时可能退回系统 capability 检查，而非自动提权。token 下的 capability 检查使用其用户命名空间并保留 LSM 检查，见 [`kernel/bpf/syscall.c:2897-2912`](../../linux/kernel/bpf/syscall.c#L2897-L2912) 与 [`kernel/bpf/token.c:11-27`](../../linux/kernel/bpf/token.c#L11-L27)。

### map 与程序分别创建

`map_create()` 按 map 类型选择操作集，检查权限，再分配对象并关联可选 BTF，见 [`kernel/bpf/syscall.c:1370-1562`](../../linux/kernel/bpf/syscall.c#L1370-L1562)。程序加载时引用已创建的 map，不意味着每次挂载都重新创建一份 map。多个程序共享 map 是有意的对象复用，但共享写入需要另外的并发协议。

## 可观测性

以下接口依赖 `CONFIG_BPF_SYSCALL=y`；FD 信息还依赖当前已启用的 `CONFIG_PROC_FS=y`。

| 入口 | 看什么 | 源码对应 |
|---|---|---|
| `bpftool prog show` | 程序 ID、类型、名字、map 依赖 | [`kernel/bpf/syscall.c:4997-5282`](../../linux/kernel/bpf/syscall.c#L4997-L5282) |
| `/proc/<pid>/fdinfo/<fd>`，程序 FD | `prog_id`、`prog_jited`、`verified_insns` | [`kernel/bpf/syscall.c:2505-2533`](../../linux/kernel/bpf/syscall.c#L2505-L2533) |
| 同上，map FD | `map_id`、键值尺寸、容量、冻结状态 | [`kernel/bpf/syscall.c:1010-1050`](../../linux/kernel/bpf/syscall.c#L1010-L1050) |
| 同上，link FD | link 类型、ID、关联程序及类型特有目标 | [`kernel/bpf/syscall.c:3343-3385`](../../linux/kernel/bpf/syscall.c#L3343-L3385) |
| `/proc/sys/kernel/unprivileged_bpf_disabled` | 非特权创建限制，不是现有对象数量 | [`kernel/bpf/syscall.c:6546-6552`](../../linux/kernel/bpf/syscall.c#L6546-L6552) |

排查加载器退出后程序仍在运行时，先查 link、pin 和其他进程持有的 FD，不要仅依据 `bpftool prog show` 中对象仍存在就判断泄漏。动态探针可从 `bpf_prog_load()`、`bpf_prog_put()` 入手，但静态函数是否保留独立符号、是否可探测，须在目标内核确认；不要对热路径的每次引用变动无界打印。

## 小结

- `bpf()` 使用统一命令入口，但 map、程序、BTF 和 link 有各自的创建流程。
- 只有验证与执行引擎选择完成后，程序才进入公开查询阶段。
- FD 提供文件引用，ID 提供查询编号，bpffs pin 提供独立的保活入口。
- 关闭加载器 FD 不等于卸载挂载关系，最后引用消失也不等于立即释放机器码。
- capability、token、LSM 与目标检查共同组成权限边界。

## 参考

- [`Documentation/bpf/syscall_api.rst`](../../linux/Documentation/bpf/syscall_api.rst)
- [`Documentation/userspace-api/ebpf/syscall.rst`](../../linux/Documentation/userspace-api/ebpf/syscall.rst)
- [`kernel/bpf/inode.c`](../../linux/kernel/bpf/inode.c)
