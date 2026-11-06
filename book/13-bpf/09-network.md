# 13.9 网络 BPF：XDP、TC 与 cgroup

> **本节源码**：[`net/core/dev.c`](../../linux/net/core/dev.c)、[`net/core/filter.c`](../../linux/net/core/filter.c)、[`drivers/net/virtio_net.c`](../../linux/drivers/net/virtio_net.c)、[`net/sched/cls_bpf.c`](../../linux/net/sched/cls_bpf.c)、[`kernel/bpf/tcx.c`](../../linux/kernel/bpf/tcx.c)、[`kernel/bpf/cgroup.c`](../../linux/kernel/bpf/cgroup.c)、[`include/trace/events/xdp.h`](../../linux/include/trace/events/xdp.h)
> **关键函数**：`virtnet_xdp_handler()`、`do_xdp_generic()`、`bpf_xdp_link_attach()`、`cls_bpf_classify()`、`tcx_run()`、`cgroup_bpf_link_attach()`、`__cgroup_bpf_run_filter_skb()`
> **前置知识**：13.3 指针边界、13.4 map、13.7 挂载关系；以太网、socket 与网络设备基础

## 概述

网络 BPF 的设计问题不是单纯“如何执行更快”，而是把程序放在什么位置、交给它什么对象，以及如何解释处理结果。快速数据路径（eXpress Data Path，XDP）可以在驱动接收路径处理数据，流量控制（Traffic Control，TC）BPF 面向 `skb` 分类或动作，cgroup BPF 则在 socket 和控制组语义下实施策略。不同位置提供的上下文与可执行操作不同，不能只换一个返回码就相互替代。本节用代表性路径说明接入关系，不展开完整协议栈。

## 核心数据结构

| 接入点 | 程序视图与内部对象 | 主要语义 |
|---|---|---|
| XDP | `struct xdp_md` / `struct xdp_buff` | 包数据与接收元数据，返回 `XDP_*` 动作 |
| 传统 TC / TCX | `struct __sk_buff` / `struct sk_buff` | 已存在 skb，按相应的分类或动作协议处理 |
| cgroup skb | 同样使用 skb 视图，但关联 socket 的 cgroup | 对控制组关联流量实施许可与特定拥塞语义 |
| socket filter | skb 受限视图 | 返回保留或截取的长度，不等同于 TC 放行码 |

类型映射见 [`include/linux/bpf_types.h:4-19`](../../linux/include/linux/bpf_types.h#L4-L19)。上下文中出现相同字段名，不意味着允许访问的范围、helper 集合和返回值也相同。

## 主流程分析

### XDP：原生驱动与通用路径

```text
驱动原生 XDP，以 virtio-net 为例
接收缓冲处理                             drivers/net/virtio_net.c
└── virtnet_xdp_handler()
    ├── bpf_prog_run_xdp()                include/net/xdp.h
    └── 按 XDP 动作处理
        ├── XDP_PASS：继续接收主路径
        ├── XDP_TX：驱动发送
        ├── XDP_REDIRECT：xdp_do_redirect()
        └── XDP_DROP / 异常：释放或丢弃

通用 XDP
接收核心路径                             net/core/dev.c
└── do_xdp_generic()
    └── netif_receive_generic_xdp()
        └── bpf_prog_run_generic_xdp()
            └── 从现有 skb 准备 xdp_buff，再运行程序
```

驱动动作分派见 [`drivers/net/virtio_net.c:1795-1854`](../../linux/drivers/net/virtio_net.c#L1795-L1854)，通用路径准备现有 skb 的代码见 [`net/core/dev.c:5410-5450`](../../linux/net/core/dev.c#L5410-L5450)，动作处理见 [`net/core/dev.c:5622-5655`](../../linux/net/core/dev.c#L5622-L5655)。

所以“XDP 一定发生在 skb 分配前”并不准确：这适合解释典型原生接收路径的优势，却不适用于通用 XDP。驱动能力、挂载模式、分片与发送支持也会影响实际行为，不能从 `CONFIG_BPF_JIT=y` 推导网卡支持原生 XDP 或硬件卸载。

`XDP_REDIRECT` 表示交给重定向处理，不是仅靠程序返回一个数字就完成远端投递。驱动还要执行相应重定向及批处理刷新协议，机制与驱动要求见 [`Documentation/bpf/redirect.rst`](../../linux/Documentation/bpf/redirect.rst)。

### 传统 TC BPF：分类与直接动作

传统分类器通过 netlink 管理 filter，在 `cls_bpf_classify()` 中调用程序，见 [`net/sched/cls_bpf.c:81-139`](../../linux/net/sched/cls_bpf.c#L81-L139)。它至少要区分两种语义：

- **分类模式**：结果用于判定匹配及 class 信息，随后还可能执行动作扩展。
- **直接动作模式**：程序返回值由 `cls_bpf_exec_opcode()` 解释为允许的 `TC_ACT_*` 动作。

这两种模式共用部分程序执行代码，但结果解释不同。入口路径对二层头部还有专门处理，因此分析包指针时要以该挂载点实际准备的上下文为准，而不是仅凭“都是 skb”推断数据起点。

### TCX：同一程序大类，另一种挂载管理

本版本还提供 TCX 挂载：使用 `BPF_PROG_TYPE_SCHED_CLS`，通过 `BPF_TCX_INGRESS` 或 `BPF_TCX_EGRESS` 管理入口与出口程序链，link 分派见 [`kernel/bpf/syscall.c:5764-5771`](../../linux/kernel/bpf/syscall.c#L5764-L5771)。其控制路径使用多程序管理基础设施，见 [`kernel/bpf/tcx.c:10-56`](../../linux/kernel/bpf/tcx.c#L10-L56)。

接收入口在有需要时先执行 `tcx_run()`，结果仍要求继续时再进入传统 TC 路径：

```text
sch_handle_ingress()                     net/core/dev.c
├── tcx_run()
│   └── 遍历多程序入口，直到不再返回 TCX_NEXT
└── tc_run()
    └── tcf_classify()
        └── cls_bpf_classify()            net/sched/cls_bpf.c
```

真实分支见 [`net/core/dev.c:4435-4484`](../../linux/net/core/dev.c#L4435-L4484)。因此不能把现代 TC BPF 简化为唯一的 `cls_bpf` 模块调用链，也不能把传统 filter 的持有方式套到 TCX link 上。

### cgroup：沿 socket 归属找到有效程序

`cgroup_bpf_link_attach()` 从目标 FD 获取 cgroup，并用通用 link 发布协议建立挂载，见 [`kernel/bpf/cgroup.c:1540-1586`](../../linux/kernel/bpf/cgroup.c#L1540-L1586)。实际运行时还涉及 cgroup 层次形成的有效程序集合，而不仅是当前目录上直接挂了什么。

对于 skb 钩子，`__cgroup_bpf_run_filter_skb()` 从 socket 的 cgroup 数据取得归属，准备数据指针，执行有效程序并转换结果，见 [`kernel/bpf/cgroup.c:1624-1685`](../../linux/kernel/bpf/cgroup.c#L1624-L1685)。不能简单按“此刻处理包的任务属于哪个 cgroup”推断策略归属，转发流量与 socket 流量也不能直接等同。

出口程序的返回值还区分是否附带拥塞通知：源码列出 `0`～`3` 的语义，再转换成内核网络发送结果，见 [`kernel/bpf/cgroup.c:1651-1673`](../../linux/kernel/bpf/cgroup.c#L1651-L1673)。程序返回值与外层 C 函数返回值并不是同一编码。

## 关键细节 / 设计考量

### 返回 0 的四种含义

| 程序或模式 | 常见含义 |
|---|---|
| XDP | `XDP_ABORTED`，不是普通放行 |
| 传统 TC 直接动作 | `TC_ACT_OK`，允许当前动作结果 |
| cgroup skb | 通常拒绝；出口还有其他返回位语义 |
| socket filter | 保留长度为零，即过滤掉数据 |

动作定义与检查应分别核对 [`include/uapi/linux/bpf.h`](../../linux/include/uapi/linux/bpf.h)、[`include/uapi/linux/pkt_cls.h`](../../linux/include/uapi/linux/pkt_cls.h)、[`net/sched/cls_bpf.c:65-79`](../../linux/net/sched/cls_bpf.c#L65-L79) 和上述 cgroup 结果转换。这个对照只针对列出的类型与模式，不能推广成全部 BPF 返回协议。

### 包边界证明有生命周期

通过一次 `data_end` 检查，只能证明当时相应指针和长度下的访问。调用调整头部、尾部或 skb 线性数据布局的 helper 后，应按接口约束重新建立指针和范围；内核的网络上下文验证与转换集中在 [`net/core/filter.c`](../../linux/net/core/filter.c)。

### AF_XDP 与 XDP 程序不是同一个对象

`CONFIG_XDP_SOCKETS=y` 只表明提供 AF_XDP 基础能力。用户态 socket、UMEM、队列绑定、XSK map 与驱动零拷贝支持仍需分别准备；本节不把“创建 XDP 程序”描述成“已经建立用户态零拷贝通道”。相关 map 类型的配置登记见 [`include/linux/bpf_types.h:127-135`](../../linux/include/linux/bpf_types.h#L127-L135)。

## 可观测性

本配置启用 `CONFIG_NET=y`、`CONFIG_BPF_SYSCALL=y`、`CONFIG_CGROUP_BPF=y`、`CONFIG_NET_INGRESS=y`、`CONFIG_NET_EGRESS=y`；传统分类器依赖 `CONFIG_NET_CLS_BPF=m`，相关模块须可用。XDP 跟踪点依赖已启用的 `CONFIG_TRACEPOINTS=y`，经 tracefs 读取还依赖 `CONFIG_TRACING=y`。

| 入口 | 看什么与如何解读 |
|---|---|
| `bpftool net`、`ip -details link show` | 接口、挂载模式与 XDP 程序；XDP link 入口见 [`net/core/dev.c:10626-10676`](../../linux/net/core/dev.c#L10626-L10676) |
| `tc -s filter show dev <接口> ingress` | 传统 TC filter、直接动作与统计，不代表所有 TCX link；实现见 [`net/sched/cls_bpf.c`](../../linux/net/sched/cls_bpf.c) |
| `bpftool link show` | 补充 TCX、XDP 和 cgroup 等 link 管理的挂载 |
| `bpftool cgroup tree` | 沿控制组层次检查挂载，工具实现见 [`tools/bpf/bpftool/cgroup.c`](../../linux/tools/bpf/bpftool/cgroup.c) |
| `xdp:xdp_exception` | `prog_id`、`act`、`ifindex`，定义见 [`include/trace/events/xdp.h:29-52`](../../linux/include/trace/events/xdp.h#L29-L52)；不能拿它统计全部普通 `XDP_DROP` |
| `xdp:xdp_redirect_err` | 结合 `err`、目标接口、`map_id` 与 `map_index` 定位重定向失败；事件及 map 重定向宏见 [`include/trace/events/xdp.h:92-170`](../../linux/include/trace/events/xdp.h#L92-L170) |

本版本 map 重定向错误也调用 `xdp_redirect_err`，不要照搬其他资料中的 `xdp:xdp_redirect_map_err` 作为这里的事件名。程序自己的动作计数 map、XDP 异常事件和设备接收丢包计数分别覆盖不同阶段，不能相互替代。网络实验应放在专用网络命名空间和虚拟接口中，避免在管理网卡上试验丢包或重定向程序；本章的最小实验则完全不挂载网络接口。

## 小结

- XDP、TC、TCX 和 cgroup BPF 的位置、对象与返回协议不同。
- 原生 XDP 与通用 XDP 的分配成本和驱动条件不能混为一谈。
- 传统 TC 和 TCX 在本版本共存，需要分别核对控制路径与查询入口。
- cgroup 网络策略常按 socket 归属运行，不等于当前任务归属。
- 先确认挂载模式和动作计数，再沿设备、重定向与协议栈分层排查。

## 参考

- [`Documentation/bpf/redirect.rst`](../../linux/Documentation/bpf/redirect.rst)
- [`Documentation/networking/af_xdp.rst`](../../linux/Documentation/networking/af_xdp.rst)
- [`Documentation/admin-guide/cgroup-v2.rst`](../../linux/Documentation/admin-guide/cgroup-v2.rst)
