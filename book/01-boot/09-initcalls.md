# 1.9 initcall：分级初始化与启动跟踪

> **本节源码**：[`include/linux/init.h`](../../linux/include/linux/init.h)、[`include/asm-generic/vmlinux.lds.h`](../../linux/include/asm-generic/vmlinux.lds.h)、[`init/main.c`](../../linux/init/main.c)、[`include/trace/events/initcall.h`](../../linux/include/trace/events/initcall.h)
> **关键函数**：`do_pre_smp_initcalls()`、`do_basic_setup()`、`do_initcalls()`、`do_initcall_level()`、`do_one_initcall()`
> **前置知识**：1.7 通用启动、1.8 SMP、链接段的基本概念

## 概述

大量子系统和驱动都有启动函数，若全部集中写进 `start_kernel()`，依赖关系与维护成本都会失控。初始化调用（Initialization Call，initcall）机制让各源码文件声明自己的函数，由链接脚本收集成分级表，再由启动主线统一遍历。级别表达大粒度先后顺序，不是一套自动求解所有依赖的图调度器。本节同时分析如何测量它，以及为什么 initcall 耗时不等于整个设备准备时间。

## 核心数据结构

### 函数类型与表项不是同一概念

初始化函数类型为 `int (*)(void)`。但当前启用 `CONFIG_HAVE_ARCH_PREL32_RELOCATIONS=y`，表项 `initcall_entry_t` 是相对偏移，不是一个普通 64 位函数指针：

[`include/linux/init.h:109-118`](../../linux/include/linux/init.h#L109-L118)（v6.18.60）

```c
typedef int (*initcall_t)(void);
typedef void (*exitcall_t)(void);

#ifdef CONFIG_HAVE_ARCH_PREL32_RELOCATIONS
typedef int initcall_entry_t;

static inline initcall_t initcall_from_entry(initcall_entry_t *entry)
{
	return offset_to_ptr(entry);
}
```

遍历时必须通过 `initcall_from_entry()` 取得函数，不能用“所有平台都是一张函数指针数组”解释实际存储布局。

### 注册宏、链接脚本与运行期遍历

| 层次 | 职责 | 源码 |
|---|---|---|
| `core_initcall()` 等宏 | 将表项放入指定输入段 | [`include/linux/init.h:254-310`](../../linux/include/linux/init.h#L254-L310) |
| `INIT_CALLS_LEVEL` / `INIT_CALLS` | 收集各级表项并生成起止符号 | [`include/asm-generic/vmlinux.lds.h:929-946`](../../linux/include/asm-generic/vmlinux.lds.h#L929-L946) |
| `initcall_levels[]` | 给出各数字级别的边界 | [`init/main.c:1263-1285`](../../linux/init/main.c#L1263-L1285) |
| `do_initcall_level()` | 解析本级参数并逐项调用 | [`init/main.c:1293-1306`](../../linux/init/main.c#L1293-L1306) |

## 主流程分析

```text
kernel_init_freeable()                 init/main.c
├── do_pre_smp_initcalls()
│   └── do_one_initcall()〔early〕
├── smp_init()
└── do_basic_setup()
    ├── driver_init()                  drivers/base/init.c
    ├── init_irq_proc()
    ├── do_ctors()
    └── do_initcalls()
        └── do_initcall_level()
            └── do_one_initcall()
                └── 注册的初始化函数
```

### 级别与特殊段的真实顺序

| 执行顺序 | 注册方式 | 说明 |
|---|---|---|
| SMP 启动前 | `early_initcall()` | 由 `do_pre_smp_initcalls()` 单独执行 |
| 0 | `pure_initcall()` | 不依赖其他普通初始化级别的基础工作 |
| 1 | `core_initcall()` | 核心初始化 |
| 2 | `postcore_initcall()` | 核心之后 |
| 3 | `arch_initcall()` | 架构相关 |
| 4 | `subsys_initcall()` | 子系统基础 |
| 5 | `fs_initcall()` | 文件系统相关 |
| 5 与 6 之间 | `rootfs_initcall()` | 链接脚本插入的 rootfs 表项 |
| 6 | `device_initcall()` | 设备等普通初始化；内建 `module_init()` 通常归入这里 |
| 7 | `late_initcall()` | 后期初始化 |

顺序由前述链接脚本和运行期表共同决定。`rootfs` **不是** `initcall_level_names[]` 中独立的第九个数字级别：它的表项落在 `__initcall5_start` 到 `__initcall6_start` 的遍历区间内，因此不能要求事件里一定有单独的 `level=rootfs`。

`console_initcall()` 使用单独的控制台段，不按上表普通级别遍历。带 `_sync` 的表项排在本级普通表项后，但后缀不意味着框架会自动等待任意异步任务完成；链接布局见 [`include/asm-generic/vmlinux.lds.h:929-949`](../../linux/include/asm-generic/vmlinux.lds.h#L929-L949)。

### 参数按级别再解析

`do_initcalls()` 每进入一级都会恢复一份可修改的 `saved_command_line` 副本。`do_initcall_level()` 先按当前级别调用 `parse_args()`，再遍历函数，见 [`init/main.c:1293-1325`](../../linux/init/main.c#L1293-L1325)。这说明启动参数解析和分级初始化是配合工作的，不是主入口里一次解析后所有模块都天然获得了最终状态。

### 单个 initcall 的执行边界

`do_one_initcall()` 记录原抢占计数，检查黑名单，发出开始事件，调用函数，再发出完成事件。随后检查抢占计数与中断状态是否被破坏；若发现异常，会修复并告警，见 [`init/main.c:1233-1260`](../../linux/init/main.c#L1233-L1260)。

返回错误码会被记录，但普通遍历循环不会统一把非零返回值升级为系统 panic。初始化失败可能只使某个功能不可用，也可能在后面挂载根文件系统时才表现为致命问题。它不是驱动依赖管理的完整替代品。

## 关键细节 / 设计考量

### 同级次序不是稳定依赖接口

同级函数的排列受链接输入和构建规则影响。若一个函数必须依赖另一子系统，应该使用合理级别或子系统自身的依赖机制，而不是依赖两个文件当前在构建列表中的偶然顺序。

### 内建初始化与模块装载不同

内建场景中 `module_init()` 映射到 `__initcall()`；模块场景下则导出模块入口。定义差异见 [`include/linux/module.h:82-89`](../../linux/include/linux/module.h#L82-L89)、[`include/linux/module.h:110-140`](../../linux/include/linux/module.h#L110-L140)。启动遍历不能直接执行尚未装载的 `.ko`。

### `__init` 表示生命周期，不表示自动登记

`__init` 把函数放入初始化代码段；`subsys_initcall()` 等宏才负责登记调用。直接调用的 `__init` 函数未必有 initcall 表项。初始化完成后这类代码可被回收，因此常驻回调不能继续指向它。相关标记见 [`include/linux/init.h:43-69`](../../linux/include/linux/init.h#L43-L69)，回收点见 [`init/main.c:1443-1454`](../../linux/init/main.c#L1443-L1454)。

## 可观测性

### 日志与 tracepoint

| 入口 | 关键字段和解读 | 定义或生成位置 |
|---|---|---|
| `initcall_debug` | 打印函数、执行任务 PID、返回码与微秒耗时 | [`init/main.c:1170-1210`](../../linux/init/main.c#L1170-L1210) |
| `initcall:initcall_level` | `level` 是当前遍历级别名；另有 SMP 前的 `early` | [`include/trace/events/initcall.h:10-24`](../../linux/include/trace/events/initcall.h#L10-L24) |
| `initcall:initcall_start` | `func` 是初始化函数地址，输出可解析为符号 | 同文件 [`include/trace/events/initcall.h:26-45`](../../linux/include/trace/events/initcall.h#L26-L45) |
| `initcall:initcall_finish` | `func` 与 `ret`，配对开始事件计算时间跨度 | 同文件 [`include/trace/events/initcall.h:47-69`](../../linux/include/trace/events/initcall.h#L47-L69) |

当前 `CONFIG_PRINTK=y`、`CONFIG_TRACEPOINTS=y`、`CONFIG_EVENT_TRACING=y`、`CONFIG_FTRACE=y`，因此日志回调和事件跟踪可用。`initcall_debug` 的日志使用 debug 级别，若想在控制台直接看到，应同时检查日志级别，不要仅因屏幕没有显示就判断函数未执行。

事件跨度包含函数阻塞、被抢占等经过时间，不是纯 CPU 执行时间；回调启动异步任务后立即返回时，又不会包含全部后台工作。例如 `populate_rootfs()` 可很快返回，而实际解包仍未完成。

### 必须在事件发生前开启

`trace_event=` 的解析在 [`kernel/trace/trace_events.c:4365`](../../linux/kernel/trace/trace_events.c#L4365) 注册。使用启动参数 `trace_event=initcall:*` 才能覆盖本次普通启动初始化；登录后向 `events/initcall/enable` 写入 `1` 不会追回已发生事件。

## 动手实验

仅在具有可恢复启动项的 dev 虚拟机中临时追加下列参数；这是启动参数示例，不是 shell 命令：

```text
initcall_debug loglevel=8 trace_event=initcall:* trace_buf_size=4M
```

重启后以 root 执行：

```bash
uname -r
mountpoint -q /sys/kernel/tracing || mount -t tracefs tracefs /sys/kernel/tracing
printf '0\n' > /sys/kernel/tracing/tracing_on
grep -E 'initcall_(level|start|finish)' /sys/kernel/tracing/trace | head -n 80
dmesg | grep -E 'calling |initcall .* returned' | head -n 80
```

`trace_buf_size` 的含义见 [`Documentation/admin-guide/kernel-parameters.txt`](../../linux/Documentation/admin-guide/kernel-parameters.txt)，缓冲区仍可能覆盖旧记录。不要在读取前清空 `trace`。本实验未实测，输出需记录目标构建、参数及丢失事件情况；比较耗时还要考虑串口打印与跟踪本身的扰动。

## 小结

- initcall 通过“注册宏 → 链接段 → 分级遍历”执行分散的初始化函数。
- 表项可能是相对偏移，必须通过架构约定解码。
- early、rootfs 和 console 有特殊安排，不能仅列数字级别解释全部顺序。
- `__init` 管生命周期，initcall 宏管调用登记，两者不是同一个机制。
- 日志与事件测得的是回调边界，不覆盖所有早期初始化与异步完成时间。

## 参考

- [`Documentation/trace/boottime-trace.rst`](../../linux/Documentation/trace/boottime-trace.rst)。
- [`Documentation/admin-guide/kernel-parameters.txt`](../../linux/Documentation/admin-guide/kernel-parameters.txt)。
- [1.10 initramfs 与根文件系统](10-rootfs.md)。
