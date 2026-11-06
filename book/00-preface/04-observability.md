# 0.4 从源码到运行时验证

> **本节源码**：[`include/trace/events/sched.h`](../../linux/include/trace/events/sched.h)、[`kernel/sched/core.c`](../../linux/kernel/sched/core.c)、[`kernel/trace/trace.c`](../../linux/kernel/trace/trace.c)、[`kernel/trace/trace_events.c`](../../linux/kernel/trace/trace_events.c)
> **关键函数**：`__schedule()`、`trace_sched_switch()`（宏生成）、`init_tracer_tracefs()`、`event_trace_init()`、`tracing_stats_read()`
> **前置知识**：[0.2 实验环境与只读构建](02-environment.md)、[0.3 源码布局与 clangd 导航](03-source-navigation.md)

## 概述

源码给出允许发生的行为，观测只记录特定条件下已经发生并被成功采集的行为。两者之间还隔着配置、权限、触发负载、过滤器、时钟和缓冲区。本节用一次短时调度跟踪建立最小验证流程：先定义问题，再核实入口，最后采集、解释并清理。这里不分析调度算法，只验证任务发生上下文切换（Context Switch）时，源码中的跟踪点（Tracepoint）如何对应运行记录。

## 核心数据：事件格式与身份字段

`TRACE_SYSTEM` 在 [`include/trace/events/sched.h:2-3`](../../linux/include/trace/events/sched.h#L2-L3) 定义为 `sched`，`TRACE_EVENT(sched_switch, ...)` 定义事件名，因此运行时事件为 `sched:sched_switch`。它的核心字段如下：

[`include/trace/events/sched.h:229-237`](../../linux/include/trace/events/sched.h#L229-L237)（v6.18.60）

```c
	TP_STRUCT__entry(
		__array(	char,	prev_comm,	TASK_COMM_LEN	)
		__field(	pid_t,	prev_pid			)
		__field(	int,	prev_prio			)
		__field(	long,	prev_state			)
		__array(	char,	next_comm,	TASK_COMM_LEN	)
		__field(	pid_t,	next_pid			)
		__field(	int,	next_prio			)
	),
```

| 字段 | 含义 | 解读时的限制 |
|---|---|---|
| `prev_comm` / `next_comm` | 被切出与将切入任务的名字 | 名字长度有限，也可以改变，不能单独充当唯一身份 |
| `prev_pid` / `next_pid` | 对应任务的 `task_struct->pid` | 按任务识别，不能简单等同于多线程程序的线程组 ID；跨命名空间解释需额外核实 |
| `prev_prio` / `next_prio` | 内核内部优先级值 | 不是可以直接当作 nice 值使用的字段 |
| `prev_state` | 经事件辅助函数编码的切出状态 | 不是原始状态位的无条件复制；抢占标记会影响输出 |

字段赋值见 [`include/trace/events/sched.h:239-248`](../../linux/include/trace/events/sched.h#L239-L248)，状态编码与打印见 [`include/trace/events/sched.h:189-214`](../../linux/include/trace/events/sched.h#L189-L214)、[`include/trace/events/sched.h:250-267`](../../linux/include/trace/events/sched.h#L250-L267)。不要只按列位置解析事件，先读取运行内核的 `events/sched/sched_switch/format`。

## 主流程分析

### 从任务切换到事件记录

```text
__schedule()                           kernel/sched/core.c
├── 选择下一任务，判断是否实际切换
├── trace_sched_switch()               include/trace/events/sched.h（宏生成）
└── context_switch()                    kernel/sched/core.c

用户态读取 tracefs
└── trace / trace_pipe                  kernel/trace/trace.c
    └── 输出已采集的事件，而非再次执行被跟踪机制
```

*图 0-3：任务切换路径与用户态读取路径；两者通过跟踪缓冲区连接，不是同步调用链*

源码中，事件调用紧邻实际上下文切换之前：

[`kernel/sched/core.c:6941-6944`](../../linux/kernel/sched/core.c#L6941-L6944)（v6.18.60）

```c
		trace_sched_switch(preempt, prev, next, prev_state);

		/* Also unlocks the rq: */
		rq = context_switch(rq, prev, next, &rf);
```

这说明事件描述即将发生的任务切换，不是 `context_switch()` 返回后的耗时统计。没有实际切换时会进入其他分支，见 [`kernel/sched/core.c:6945-6950`](../../linux/kernel/sched/core.c#L6945-L6950)。因此 `sched_switch` 的条数不能直接当作 `__schedule()` 的调用次数。

### 把验证问题限制到事件能回答的范围

本次问题是：“让一个用户任务反复短时睡眠，能否在采样窗口看到涉及它的切换记录？”支持判断的证据包括任务 ID、采样起止、事件格式及缓冲区状态。

这个实验不能单独回答“睡眠精确了多少纳秒”“唤醒延迟有多大”或“调度器是否最优”。这些问题还需要定义时间起点、终点，并结合唤醒事件、时钟与负载，不能用相邻两行输出的时间差替代。

## 关键细节 / 设计考量

### 选择成本最低、含义最直接的入口

已有接口能回答问题时，先使用已有接口。计数器适合看汇总变化，事件适合还原状态转移，动态探针适合补充缺少的局部信息；它们没有统一的优先级，取决于问题本身。

初学时不建议一开始就全局开启函数图跟踪或大量动态探针。`__schedule()` 带有 `notrace`，见 [`kernel/sched/core.c:6801`](../../linux/kernel/sched/core.c#L6801)，本节选择预置事件，不尝试绕过跟踪限制。

### 避免干扰其他跟踪会话

tracefs 的顶层缓冲区可能被其他工具使用。清空顶层 `trace`、修改全局过滤器或停止顶层跟踪，可能破坏别人的实验。应创建独立的跟踪实例（Trace Instance），实例目录入口见 [`kernel/trace/trace.c:10421-10428`](../../linux/kernel/trace/trace.c#L10421-L10428)。

独立实例隔离本次缓冲区和许多采集设置，但不意味着没有系统开销：事件记录仍在被观测机器上执行。实验限定短时间、少量事件，不承诺“零开销”。

### 没有记录不等于机制没有发生

排查顺序应是：运行身份 → 配置支持 → 文件系统挂载 → 权限与安全策略 → 事件存在 → 开关与过滤 → 负载触发 → 缓冲区与读取方式。不要首先修改内核或关闭安全策略。

`trace` 适合停止记录后读取非消费式文本；`trace_pipe` 是消费式读取，读取会取走数据，见 [`Documentation/trace/ftrace.rst:125-148`](../../linux/Documentation/trace/ftrace.rst#L125-L148)。多个工具同时消费、窗口过长或缓冲区覆盖，都可能让最后读到的内容不同于预期。

## 可观测性

本节入口的默认条件来自 [`linux/.config`](../../linux/.config)：`CONFIG_PROC_FS=y`、`CONFIG_SYSFS=y`、`CONFIG_TRACEPOINTS=y`、`CONFIG_FTRACE=y`、`CONFIG_TRACING=y`、`CONFIG_EVENT_TRACING=y`。实际运行内核仍须单独核验。tracefs 不是 debugfs 的同义词，本节使用 `/sys/kernel/tracing` 挂载路径，不要求先挂载 debugfs。

| 入口 | 从哪里看、看什么 | 实现与解释 |
|---|---|---|
| `/proc/version` | 当前运行内核身份 | [`fs/proc/version.c:10-27`](../../linux/fs/proc/version.c#L10-L27)，不要只依据磁盘上的源码版本 |
| `/sys/kernel/tracing/available_events` | 是否列出 `sched:sched_switch` | `event_trace_init()` 创建文件，见 [`kernel/trace/trace_events.c:4654-4664`](../../linux/kernel/trace/trace_events.c#L4654-L4664)；存在不代表已开启 |
| 实例内 `events/sched/sched_switch/format`、`enable` | 格式、字段偏移和本实例事件开关 | 事件文件定义见 [`kernel/trace/trace_events.c:2993-3010`](../../linux/kernel/trace/trace_events.c#L2993-L3010) |
| 实例内 `current_tracer`、`tracing_on` | 跟踪器类型与缓冲区记录开关 | `init_tracer_tracefs()` 创建入口，见 [`kernel/trace/trace.c:10443-10487`](../../linux/kernel/trace/trace.c#L10443-L10487)；本节使用 `nop`，仍可记录事件 |
| 实例内 `trace_clock`、`buffer_size_kb` | 时间基准与每 CPU 缓冲区大小 | 同上；时钟选择影响跨 CPU 时间比较，缓冲区容量不是整机只有这一份 |
| 实例内 `trace` | 停止记录后读取事件 | [`kernel/trace/trace.c:10460-10464`](../../linux/kernel/trace/trace.c#L10460-L10464)，本节不用消费式 `trace_pipe` |
| 实例内 `per_cpu/cpu*/stats` | 是否覆盖或丢失事件 | 由 `tracing_stats_read()` 输出，见 [`kernel/trace/trace.c:8872-8925`](../../linux/kernel/trace/trace.c#L8872-L8925)；创建见 [`kernel/trace/trace.c:9216-9217`](../../linux/kernel/trace/trace.c#L9216-L9217) |

`stats` 中，`overrun` 表示覆盖造成的丢失，`dropped events` 表示关闭覆盖时被丢弃的事件，`commit overrun` 反映嵌套记录造成的提交问题；定义见 [`Documentation/trace/ftrace.rst:763-793`](../../linux/Documentation/trace/ftrace.rst#L763-L793)。如果这些计数出现异常，应先缩短窗口、减少事件或调整容量，而不是继续声称得到了完整事件序列。

## 动手实验

### 1. 准备新的 root Bash 会话

只在 dev 虚拟机或同等可恢复的实验机执行。以下命令尚未实测；使用的 `python3`、`findmnt`、`mountpoint` 等用户态工具需要已安装。先运行 `sudo -i`，再启动一个专用于本实验的 `bash`；后续代码块都在这个新会话中执行，最后以 `exit` 触发清理。

```bash
sudo -i
bash
```

初始化记录目录并检查挂载；默认只在未挂载时挂载 tracefs，不改变已有顶层采集设置：

```bash
set -euo pipefail
T=/sys/kernel/tracing
LOG=$(mktemp -d /var/tmp/linux-study-trace.XXXXXX)
uname -a > "$LOG/uname.txt"
cat /proc/version > "$LOG/proc-version.txt"
mkdir -p "$T"
if ! mountpoint -q "$T"; then
    mount -t tracefs tracefs "$T"
fi
test "$(findmnt -rn -M "$T" -o FSTYPE)" = tracefs
grep -Fx 'sched:sched_switch' "$T/available_events"
I=$T/instances/book-preface-$$
mkdir "$I"
cleanup() {
    printf '0\n' > "$I/tracing_on" || true
    printf '0\n' > "$I/events/sched/sched_switch/enable" || true
    rmdir "$I" || true
}
trap cleanup EXIT
printf 'records: %s\n' "$LOG"
```

这里不自动卸载 tracefs，因为实验期间其他进程也可能开始使用它。若本节负责首次挂载，结束后只有在确认没有其他使用者时才手工卸载。若挂载或创建实例被拒绝，停止并检查配置、权限与安全策略，不以关闭加固作为默认解决办法。

### 2. 保存格式，短时采集

保持同一会话及变量不变；`global` 时钟用于本次短实验的跨 CPU 时间顺序，写入失败时应停止并检查支持情况，不静默改用其他时钟：

```bash
printf '0\n' > "$I/tracing_on"
printf 'nop\n' > "$I/current_tracer"
printf 'global\n' > "$I/trace_clock"
printf '256\n' > "$I/buffer_size_kb"
cat "$I/trace_clock" > "$LOG/trace-clock.txt"
cat "$I/events/sched/sched_switch/format" > "$LOG/event-format.txt"
: > "$I/trace"
printf '1\n' > "$I/events/sched/sched_switch/enable"
printf '1\n' > "$I/tracing_on"
python3 - <<'PY' > "$LOG/workload.txt"
import os
import time
print('workload_pid=' + str(os.getpid()), flush=True)
for _ in range(20):
    time.sleep(0.01)
PY
printf '0\n' > "$I/tracing_on"
printf '0\n' > "$I/events/sched/sched_switch/enable"
cat "$I/trace" > "$LOG/trace.txt"
for stats in "$I"/per_cpu/cpu*/stats; do
    printf '\n%s\n' "$stats"
    cat "$stats"
done > "$LOG/buffer-stats.txt"
```

`256` 的单位是每 CPU 的 KB，不是全系统总计 256 KB。这里没有过滤 PID，短窗口内还会出现其他任务的切换；这有助于先确认事件通路可用，再按工作负载任务 ID 检索。真实 Python 进程退出前会经历多少次切换受运行环境影响，本节不规定事件条数。

### 3. 按证据检查，而不是寻找固定输出

```bash
cat "$LOG/workload.txt"
pid=$(awk -F= '/^workload_pid=/ {print $2}' "$LOG/workload.txt")
test -n "$pid"
grep -E "(prev_pid|next_pid)=${pid}([[:space:]]|$)" \
    "$LOG/trace.txt" || true
cat "$LOG/buffer-stats.txt"
printf '保留记录目录：%s\n' "$LOG"
exit
```

正常情况下，应能检索到工作负载任务参与的切换；必须结合以下条件解释结果：

- 用事件负载中的 `prev_pid`、`next_pid` 找任务，不只看输出行前缀中的任务名。
- 睡眠引起的切出通常与阻塞状态相关，但不能要求本次所有切出都有同一个状态；抢占、退出等也可能发生。
- 检查每 CPU 缓冲区是否覆盖或丢失；有丢失时不能据此重建完整时序。
- `sleep(0.01)` 只是产生负载，不是精确 10 ms 调度延迟测量工具。
- 若运行的不是目标内核，把记录标为工具练习；保存事件格式，不能用另一个内核版本的字段假设代替它。

`exit` 会关闭本实例并删除实例目录，记录保留在 `$LOG` 对应的磁盘路径。若会话异常结束，应确认实例是否残留；只清理自己创建的实例，不清空顶层缓冲区。

## 小结

- 验证问题必须与观测入口的语义匹配，不能让一个事件承担全部解释。
- 源码中的事件定义、调用点与运行内核的 `format` 应相互核对。
- 独立实例用于隔离采集配置与缓冲区，但不会消除跟踪开销。
- 空结果需要逐层排查，非空结果也要检查身份、过滤、时钟和丢失。
- 记录与清理都是实验的一部分，不能只保留几行看起来符合预期的输出。

## 参考

- [`Documentation/trace/ftrace.rst`](../../linux/Documentation/trace/ftrace.rst)：tracefs、实例、时钟、缓冲区及读取语义。
- [`Documentation/trace/events.rst`](../../linux/Documentation/trace/events.rst)：事件开关、格式与过滤。
- [`Documentation/trace/tracepoints.rst`](../../linux/Documentation/trace/tracepoints.rst)：跟踪点机制与使用约束。
- [第 3 章 调度器](../03-scheduler/README.md)：调度事件背后的状态、唤醒与切换实现。
