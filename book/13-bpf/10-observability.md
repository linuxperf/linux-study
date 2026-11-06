# 13.10 可观测性与最小加载实验

> **本节源码**：[`kernel/bpf/syscall.c`](../../linux/kernel/bpf/syscall.c)、[`include/linux/filter.h`](../../linux/include/linux/filter.h)、[`net/core/filter.c`](../../linux/net/core/filter.c)、[`net/bpf/test_run.c`](../../linux/net/bpf/test_run.c)、[`tools/bpf/bpftool/prog.c`](../../linux/tools/bpf/bpftool/prog.c)、[`tools/bpf/bpftool/map.c`](../../linux/tools/bpf/bpftool/map.c)
> **关键函数**：`bpf_prog_get_info_by_fd()`、`bpf_prog_show_fdinfo()`、`bpf_map_show_fdinfo()`、`bpf_prog_get_stats()`、`bpf_enable_stats()`、`bpf_prog_test_run_skb()`
> **前置知识**：13.2 生命周期、13.3 验证器、13.4 map、13.5 运行统计

## 概述

BPF 故障可能发生在用户态准备、权限、验证、编译、挂载、触发或数据消费任一阶段。观测时应先画出程序、map 与挂载的关系，再确定失败发生在哪一层。程序 ID、验证日志、运行统计和应用自己的计数器回答的问题不同，不能用其中一个替代全部。本节用一个不依赖内核 BTF、也不挂载网络接口的程序，串起加载、执行、状态读取和释放的最短路径。

## 核心数据结构

程序执行统计使用 per-CPU 的 `struct bpf_prog_stats`：

[`include/linux/filter.h:702-707`](../../linux/include/linux/filter.h#L702-L707)（v6.18.60）

```c
struct bpf_prog_stats {
	u64_stats_t cnt;
	u64_stats_t nsecs;
	u64_stats_t misses;
	struct u64_stats_sync syncp;
} __aligned(2 * sizeof(u64));
```

`cnt` 与 `nsecs` 在启用统计的执行路径中增加；`misses` 由相应递归保护等路径更新。汇总函数 `bpf_prog_get_stats()` 对各 CPU 的数据做一致性读取，见 [`kernel/bpf/syscall.c:2477-2502`](../../linux/kernel/bpf/syscall.c#L2477-L2502)。这些统计不是业务 map 的一部分。

## 主流程分析

```text
对象查询
bpf(BPF_OBJ_GET_INFO_BY_FD)              kernel/bpf/syscall.c
└── bpf_obj_get_info_by_fd()
    ├── bpf_prog_get_info_by_fd()
    ├── bpf_map_get_info_by_fd()
    └── bpf_link_get_info_by_fd()

测试执行
bpf(BPF_PROG_TEST_RUN)                  kernel/bpf/syscall.c
└── bpf_prog_test_run()
    └── prog->aux->ops->test_run()
        └── bpf_prog_test_run_skb()     net/bpf/test_run.c
            ├── 准备测试 skb 与上下文
            ├── bpf_test_run()
            └── 回传 retval 与 duration
```

查询分派见 [`kernel/bpf/syscall.c:5420-5459`](../../linux/kernel/bpf/syscall.c#L5420-L5459)。测试执行通过程序类型的操作集选择实现，不是任意程序都可测试，入口见 [`kernel/bpf/syscall.c:4706-4732`](../../linux/kernel/bpf/syscall.c#L4706-L4732)。socket filter 的测试回调登记在 [`net/core/filter.c:11172-11174`](../../linux/net/core/filter.c#L11172-L11174)。

## 关键细节 / 设计考量

### 三种“次数”不要混用

- `verified_insns`：加载期验证工作量，不是运行次数。
- `run_cnt`：运行统计启用后，相应执行路径记录的调用次数。
- 应用 map 中的计数：程序按自己定义的逻辑更新，可统计进入、匹配或失败等业务事件。

字段查询见 [`kernel/bpf/syscall.c:5053-5057`](../../linux/kernel/bpf/syscall.c#L5053-L5057)，通用运行计数更新见 [`include/linux/filter.h:732-757`](../../linux/include/linux/filter.h#L732-L757)。平均耗时可用同一采样区间内的 `Δrun_time_ns / Δrun_cnt` 估算，但不代表某个业务请求的端到端延迟，也不保证覆盖外部传输开销。

### 统计开关也有生命周期

`BPF_ENABLE_STATS` 的 `BPF_STATS_RUN_TIME` 返回 stats FD，持有它会增加统计 static key 的引用，关闭后减少引用，见 [`kernel/bpf/syscall.c:5983-6035`](../../linux/kernel/bpf/syscall.c#L5983-L6035)。另一个入口是 `kernel.bpf_stats_enabled` sysctl。后者也参与引用管理，不能假设写回 0 就会关闭其他观测者通过 stats FD 开启的统计。

## 可观测性

以下基础接口依赖当前已启用的 `CONFIG_BPF_SYSCALL=y`、`CONFIG_PROC_FS=y`、`CONFIG_SYSCTL=y`；JIT 查询还依赖 `CONFIG_BPF_JIT=y`。权限、地址隐藏与工具构建选项可能限制具体输出。

| 问题 | 首选入口 | 解读与源码 |
|---|---|---|
| 加载了什么 | `bpftool prog show` / `map show` / `link show` | 先建立 ID 与依赖关系；查询见 [`kernel/bpf/syscall.c:4997-5459`](../../linux/kernel/bpf/syscall.c#L4997-L5459) |
| 谁持有对象 | `/proc/<pid>/fd`、`fdinfo`、pin 路径与目标子系统 | `fdinfo` 生成见 [`kernel/bpf/syscall.c:2505-2533`](../../linux/kernel/bpf/syscall.c#L2505-L2533)、[`kernel/bpf/syscall.c:3343-3385`](../../linux/kernel/bpf/syscall.c#L3343-L3385) |
| 为什么被拒绝 | libbpf 调试输出与验证日志 | 先排除用户态准备失败，再分析寄存器和状态，见 13.3 |
| 最终执行什么 | `prog dump xlated` / `prog dump jited` | 区分验证后指令与本机机器码，见 13.5 |
| 是否运行、成本多少 | `run_cnt`、`run_time_ns`、`recursion_misses` | 先检查统计开关；字段与生成见 [`kernel/bpf/syscall.c:2505-2533`](../../linux/kernel/bpf/syscall.c#L2505-L2533) |
| 为什么没有业务事件 | 应用计数、ringbuf 失败计数、目标事件 | 没有统一的内核“全部 BPF 丢失数”，应分阶段记录 |
| 权限或加固限制 | `kernel.unprivileged_bpf_disabled` 与 JIT sysctl | 注册见 [`kernel/bpf/syscall.c:6544-6567`](../../linux/kernel/bpf/syscall.c#L6544-L6567)、[`net/core/sysctl_net_core.c:440-473`](../../linux/net/core/sysctl_net_core.c#L440-L473) |

没有单一 tracepoint 覆盖全部 BPF 生命周期、运行和数据丢失。对加载耗时可考虑探测 `bpf_check()`、`bpf_int_jit_compile()` 等非内联控制路径；当前内核是否允许挂探针仍须检查，不能探测就依赖日志和已有统计，不应强行绕过限制。

## 动手实验

### 环境、权限与边界

在 `dev` 虚拟机内以 root 运行，仓库位于 `/root/linux`。请先确认 `uname -r`：虚拟机默认启动的发行版内核不一定是本书内核；若不是，应把结果标为工具链实验，不冒充 v6.18.60 的运行结果。

本实验只创建临时 BPF 对象、短时开启统计并调用测试执行接口，不挂载 socket、网络设备、tracepoint 或 cgroup。它依赖 `CONFIG_BPF_SYSCALL=y`、`CONFIG_BPF_JIT=y`、`CONFIG_NET=y` 及本节观测配置，不依赖 `CONFIG_DEBUG_INFO_BTF`。命令尚未在该虚拟机执行，下面只说明检查方法和应验证的不变量，不提供虚构的输出或耗时。

示例已在宿主机使用 Clang 23.1.5、仓库 UAPI / libbpf 头完成 `bpfel` 目标编译与反汇编检查，临时产物未写入 `linux/`。这只验证编译产物，不代表已经通过目标内核验证器或完成运行测试。

先进入 root shell；后续代码块在同一个 Bash 会话中执行：

```bash
sudo -i
```

安装用户态依赖并检查目标。发行版的 bpftool 包名若不同，应安装提供该命令的包，或在只读源码树之外构建匹配版本；不要在 `linux/` 中生成构建产物。

```bash
apt-get update
apt-get install -y clang llvm gcc libbpf-dev bpftool python3
uname -r
bpftool version
clang --version
bpftool feature probe kernel
sysctl kernel.unprivileged_bpf_disabled kernel.bpf_stats_enabled
```

命令参数以仓库工具文档为准：[`tools/bpf/bpftool/Documentation/bpftool-prog.rst`](../../linux/tools/bpf/bpftool/Documentation/bpftool-prog.rst)、[`tools/bpf/bpftool/Documentation/bpftool-map.rst`](../../linux/tools/bpf/bpftool/Documentation/bpftool-map.rst)。若系统工具较旧，先核对其 `help`，不要把未知参数错误当成内核能力缺失。

### 示例程序：只做一次 map 计数

实验文件为 [`examples/socket-counter.bpf.c`](examples/socket-counter.bpf.c)，是本书示例，不是内核代码节选。它定义一个只有一个元素的 array map，每次调用先查找该元素，再用原子加法递增，最后返回 `0xffffffffU`，按 socket filter 的长度协议表示不主动截短数据。

示例没有读取内核内部类型，没有 CO-RE 重定位，也没有依赖内核 BTF 的挂载目标。map 声明自带的 BTF 不改变这个事实。判空对应验证器规则，原子加法对应共享 value 的并发更新需求；即使当前测试是串行执行，也应保留这个示范。

### 创建临时目录并编译

下面的退出清理只删除本实验创建的文件，并恢复原来的 sysctl 值；不要并行运行其他会修改相同统计 sysctl 的实验。若 bpffs 已有挂载，则复用且不卸载它。

```bash
set -euo pipefail
mountpoint -q /sys/fs/bpf || mount -t bpf bpf /sys/fs/bpf
work=$(mktemp -d /tmp/book13.XXXXXX)
pin=$(mktemp -d /sys/fs/bpf/book13_XXXXXX)
stats_old=$(sysctl -n kernel.bpf_stats_enabled)
cleanup() {
    sysctl -q -w kernel.bpf_stats_enabled="$stats_old"
    rm -f "$pin/prog" "$pin/maps/calls"
    rmdir "$pin/maps" "$pin"
    rm -f "$work/socket-counter.o" "$work/frame.bin"
    rmdir "$work"
}
trap cleanup EXIT
mkdir "$pin/maps"
clang -O2 -g -target bpf \
    -I"/usr/include/$(gcc -dumpmachine)" \
    -c /root/linux/book/13-bpf/examples/socket-counter.bpf.c \
    -o "$work/socket-counter.o"
llvm-objdump -d "$work/socket-counter.o"
```

`-g` 保留对象类型与调试元数据，不等于要求当前内核也启用了内核 BTF。示例使用安装的 UAPI 和 libbpf 头文件；书稿分析仍以仓库源码为准，不要求把 `linux/` 作为可写构建目录。

### 加载、查看对象和验证后指令

```bash
bpftool -d prog load "$work/socket-counter.o" "$pin/prog" \
    pinmaps "$pin/maps"
bpftool prog show pinned "$pin/prog"
bpftool map show pinned "$pin/maps/calls"
bpftool map lookup pinned "$pin/maps/calls" key hex 00 00 00 00
bpftool prog dump xlated pinned "$pin/prog"
bpftool prog dump jited pinned "$pin/prog" || \
    printf '%s\n' 'JIT 反汇编不可用；请结合 prog show 核对 JIT 状态。' >&2
```

这里没有 `autoattach`，成功加载与固定后程序尚未挂到任何事件源。初始 array value 应为零；若工具使用 BTF 以结构化形式输出，应按值解读，不要机械匹配某行文本。JIT 反汇编需要工具具备相应支持；“不能显示反汇编”与“程序没有 JIT”不是同一个问题。

### 测试执行，不向真实网络发包

创建一个 64 字节测试帧，设置未知但合法的以太网协议号，避免依赖 IPv4/IPv6 头部内容。测试路径要求至少有以太网头，见 [`net/bpf/test_run.c:978-1006`](../../linux/net/bpf/test_run.c#L978-L1006)。

```bash
python3 - "$work/frame.bin" <<'PY'
import sys
frame = bytearray(64)
frame[0:6] = bytes.fromhex("020000000001")
frame[6:12] = bytes.fromhex("020000000002")
frame[12:14] = bytes.fromhex("88b5")
with open(sys.argv[1], "wb") as out:
    out.write(frame)
PY
sysctl -q -w kernel.bpf_stats_enabled=1
bpftool prog run pinned "$pin/prog" data_in "$work/frame.bin" repeat 10
bpftool map lookup pinned "$pin/maps/calls" key hex 00 00 00 00
bpftool prog show pinned "$pin/prog"
```

应核对三件事：

1. 成功完成 10 次测试执行后，`calls[0]` 从 0 增加到 10；它是业务 map 计数。
2. 程序返回值按无符号 32 位解释为 `4294967295`，不是 TC 或 XDP 动作。
3. 统计开启的相应执行路径记录运行次数和耗时；只在测试调用前后比较增量，不把累计时间当成单次时延。

测试循环与调用逻辑见 [`net/bpf/test_run.c:389-433`](../../linux/net/bpf/test_run.c#L389-L433)，skb 测试执行与结果回传见 [`net/bpf/test_run.c:1115-1158`](../../linux/net/bpf/test_run.c#L1115-L1158)。`repeat` 的 duration 是测试接口给出的重复执行平均时间，不代表真实网络的端到端时延。测试会更新真实 map，所以不能把它当成完全无副作用的模拟。

### 释放与排错

```bash
cleanup
trap - EXIT
```

本实验同时固定了程序与 map，必须移除两类 pin。程序持有 map 引用，因此只删除 map pin 不会让 map 在程序仍存活时失效；同样，保留 map pin 会让 map 在程序释放后继续存在。对象释放还可能经过 RCU，不能把某个瞬时快照当成泄漏证据。

| 现象 | 优先检查 |
|---|---|
| Clang 提示找不到 `asm/types.h` | 是否安装开发头、multiarch include 路径是否正确 |
| 找不到 `bpf/bpf_helpers.h` | 是否安装 `libbpf-dev`，不要误判为内核拒绝加载 |
| `EPERM` | root/capability、LSM、容器限制与非特权 BPF 策略，不要先关闭安全保护 |
| map 或程序 BTF 加载失败 | 查看 libbpf 与 BTF 日志；不能只因内核未启用 BTF 就断言程序 BTF 不可用 |
| 验证器拒绝 | 从完整日志检查 map 指针判空、原子访问及指令支持 |
| 测试执行不受支持 | 确认实际程序类型和运行内核的 `test_run()` 能力 |
| map 增长但 `run_cnt` 不增长 | 检查统计开关和具体执行路径，不要否认业务计数已证明的执行事实 |
| 程序消失但 map 仍存在 | 检查是否保留 map pin 或有其他 FD 引用 |

## 小结

- BPF 排错应按准备、加载、挂载、触发和消费逐层定位。
- 查询 ID、FD、pin 与挂载目标，才能建立完整的对象关系。
- 验证成本、运行成本和业务计数是不同维度。
- `BPF_PROG_TEST_RUN` 不需要真实挂载，但会真实执行程序并修改 map。
- 最小实验优先验证不变量，不把未测量的输出和耗时写成事实。

## 参考

- [`Documentation/bpf/bpf_prog_run.rst`](../../linux/Documentation/bpf/bpf_prog_run.rst)
- [`Documentation/bpf/test_debug.rst`](../../linux/Documentation/bpf/test_debug.rst)
- [`tools/bpf/bpftool/Documentation/bpftool-prog.rst`](../../linux/tools/bpf/bpftool/Documentation/bpftool-prog.rst)
- [`tools/bpf/bpftool/Documentation/bpftool-map.rst`](../../linux/tools/bpf/bpftool/Documentation/bpftool-map.rst)
