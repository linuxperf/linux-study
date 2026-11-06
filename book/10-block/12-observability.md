# 10.12 可观测性与 null_blk 实验

> **本节源码**：[`include/trace/events/block.h`](../../linux/include/trace/events/block.h)、[`block/genhd.c`](../../linux/block/genhd.c)、[`block/blk-mq.c`](../../linux/block/blk-mq.c)、[`block/blk-mq-debugfs.c`](../../linux/block/blk-mq-debugfs.c)、[`drivers/block/null_blk/main.c`](../../linux/drivers/block/null_blk/main.c)
> **关键函数**：`part_stat_show()`、`diskstats_show()`、`blk_account_io_start()`、`blk_account_io_done()`、`null_queue_rq()`
> **前置知识**：10.4～10.10，tracefs 与 shell 的基本用法

## 概述

块层性能分析的第一步不是立即画时延直方图，而是确认测量对象和边界。应用操作、bio、request、驱动命令尝试和持久化序列都有各自的计数与时间轴。静态事件帮助观察流转，设备统计提供长期趋势，debugfs 与动态探针补充资源等待和对象关联。`null_blk` 可把真实介质的不确定性暂时移除，用来验证块层路径，而不是预测实际硬盘性能。

## 核心数据结构

### 1. 请求时间戳与计数

`request` 提供 `alloc_time_ns`、`start_time_ns`、`io_start_time_ns`；定义见 [`include/linux/blk-mq.h:129-136`](../../linux/include/linux/blk-mq.h#L129-L136)。它们分别服务于不同统计需求，不能只凭变量名假设覆盖整个系统调用。分配起始时间依赖 `CONFIG_BLK_RQ_ALLOC_TIME=y`，还受队列运行时标志控制；设置与使用见 [`block/blk-mq.c:389-399`](../../linux/block/blk-mq.c#L389-L399)、[`block/blk-mq.c:552-565`](../../linux/block/blk-mq.c#L552-L565)。

`disk_stats` 保存各操作组的完成次数、合并次数、扇区数、纳秒累计及活动计时。输出由 `part_stat_show()` 汇总为 17 项，见 [`block/genhd.c:1066-1107`](../../linux/block/genhd.c#L1066-L1107)。

### 2. 跟踪事件中不一定有唯一身份

请求事件内部接收 `struct request *`，但普通 tracefs 记录的是设备号、扇区、长度、操作字符串和部分进程信息，不导出该指针，见 [`include/trace/events/block.h:118-154`](../../linux/include/trace/events/block.h#L118-L154) 与 [`include/trace/events/block.h:191-228`](../../linux/include/trace/events/block.h#L191-L228)。重复访问相同设备范围时，不能把 `(dev, sector)` 当成唯一请求 ID。

## 主流程分析

下面按诊断顺序列出观察链，而不是函数调用链。

```text
确定设备身份与拓扑
├── /sys/class/block/<dev>/dev、size、分区 start
├── /sys/block/<disk>/queue/*：限制、调度器、WBT
└── /sys/block/<disk>/mq/*：CPU 映射与标签容量
观察数据流
├── submit_bio / blk-throttle：到达与限速
├── block_bio_queue / block_split / block_bio_*merge
├── block_getrq / block_rq_insert / block_rq_issue
├── 驱动命令事件、重试 / 重排
└── block_rq_complete / 最终请求结束
核对长期趋势
├── /sys/block/<disk>/stat、inflight
├── /proc/diskstats
└── cgroup io.stat、io.max 与控制器状态
```

### 1. 分段定义比总时延更重要

| 时间段 | 可以回答什么 | 不能直接推出什么 |
|---|---|---|
| 上层入口 → 取得请求 | 节流、请求资源分配等前段等待 | 所有时间都在 I/O 调度器中 |
| 请求插入 → issue | 该插入路径之后的排队与派发 | 每条请求都有此插入事件；包含全部 plug 等待 |
| issue → 本次完成 | 驱动接收后的执行与完成处理时间 | 纯介质时间、完整系统调用时间 |
| 首次进入 → 最终结束 | 若用可靠对象身份关联，可测完整请求生命周期 | 对应某个应用调用的完整时延 |

`block_rq_complete` 可以表示部分完成，重排后也可能再次 issue；flush 还会有多个阶段。测量工具必须明确定义首次 / 最近一次 issue、是否跨重试累计、何时删除对象状态。仅靠文本事件做相邻行相减，不能给出一般并发场景下的准确时延。

### 2. 设备 stat 的 17 项怎样读

以下编号从 1 开始，只针对 `stat` 文件本身；`/proc/diskstats` 前面另有主设备号、次设备号和设备名。

| 项号 | 内容 | 单位 / 解读 |
|---|---|---|
| 1～4 | 读完成、读合并、读扇区、读累计时间 | 次、次、512 字节扇区、ms |
| 5～8 | 写完成、写合并、写扇区、写累计时间 | 同上 |
| 9 | 已进入设备 I/O 记账但未结束的数量 | 请求数，见下文版本细节 |
| 10 | `io_ticks` | 活动时间的累计估计，ms |
| 11 | 各类请求累计时间之和 | 可超过墙上时间，ms |
| 12～15 | discard 完成、合并、扇区、累计时间 | 次、次、扇区、ms |
| 16～17 | flush 完成、累计时间 | 整盘的 flush 请求统计 |

源码中的重要边界是：完成数与 `now - start_time_ns` 在 `blk_account_io_done()` 累计，扇区在字节完成时累计，合并次数在合并时增加，分别见 [`block/blk-mq.c:1082-1102`](../../linux/block/blk-mq.c#L1082-L1102)、[`block/blk-mq.c:869-884`](../../linux/block/blk-mq.c#L869-L884)、[`block/blk-merge.c:926-933`](../../linux/block/blk-merge.c#L926-L933)。

**本版本第 9 项不能照搬“已发给驱动”的常见解释。** `stat` 和 `/proc/diskstats` 调用 `bdev_count_inflight()`，统计从 `blk_account_io_start()` 开始的未结束请求；该函数在 bio 转成 request 时已经调用，早于普通派发，见 [`block/genhd.c:128-169`](../../linux/block/genhd.c#L128-L169)、[`block/blk-mq.c:2697-2720`](../../linux/block/blk-mq.c#L2697-L2720)。

相比之下，单独的 `/sys/block/<disk>/inflight` 对 blk-mq 选择驱动侧统计分支，报告已经开始驱动处理的读 / 写在途量，见 [`block/genhd.c:1114-1125`](../../linux/block/genhd.c#L1114-L1125)。该分支遍历标签并检查 `MQ_RQ_IN_FLIGHT`，见 [`block/blk-mq.c:93-113`](../../linux/block/blk-mq.c#L93-L113)。两者在排队较多时可以不同。仓库的 [`Documentation/block/stat.rst`](../../linux/Documentation/block/stat.rst) 对 in-flight 的描述更概括，本章以实际调用分支为准。

对多队列设备，活动时间占满一个采样窗口不能单独证明所有硬件资源都已饱和。应同时观察吞吐、尾延迟、深度、CPU 开销与控制器状态。

## 关键细节 / 设计考量

- 设备统计应做两次采样取差值，而不是把开机累计值当成当前速率；设备重建、计数回绕与热拔除需要处理。
- 堆叠设备与下层设备可能分别记账，不能把所有设备计数直接求和当作应用实际字节数。
- 请求事件的 `comm` 是事件发生时的当前任务，不保证是最初提交 I/O 的应用。
- 动态探针可通过请求指针关联 `blk_mq_start_request()` 与最终结束路径，但须覆盖批量结束、重试、部分完成和对象复用；原型工具不能未经验证就作为精确统计来源。

## 可观测性

本配置启用 `CONFIG_SYSFS=y`、`CONFIG_PROC_FS=y`、`CONFIG_TRACEPOINTS=y`、`CONFIG_TRACING=y`、`CONFIG_BLK_DEV_IO_TRACE=y`、`CONFIG_DEBUG_FS=y`、`CONFIG_BLK_DEBUG_FS=y`。

| 入口 | 最适合观察的问题 | 定义 |
|---|---|---|
| tracefs `events/block/*` | 提交、拆分、合并、插入、执行和完成的相对顺序 | [`include/trace/events/block.h`](../../linux/include/trace/events/block.h) |
| `stat`、`/proc/diskstats` | 长期吞吐、合并与累计时延趋势 | [`block/genhd.c:1066-1107`](../../linux/block/genhd.c#L1066-L1107)、[`block/genhd.c:1361-1418`](../../linux/block/genhd.c#L1361-L1418) |
| debugfs `block/<disk>/hctx<n>/dispatch`、`tags`、`sched_tags` | 遗留派发、驱动 / 调度器标签压力 | [`block/blk-mq-debugfs.c:588-601`](../../linux/block/blk-mq-debugfs.c#L588-L601) |
| `io.stat`、`io.max`、`wbt:*` | 归属与策略等待 | [10.10](10-blkcg-qos.md) |
| `null_queue_rq()` | 模拟设备的接收、故障注入与定时完成入口 | [`drivers/block/null_blk/main.c:1669-1723`](../../linux/drivers/block/null_blk/main.c#L1669-L1723) |

## 动手实验

### 实验环境与安全边界

以下均在 **`dev` 虚拟机内，以 root 执行**；需要与运行内核匹配的 `null_blk` 模块、`fio` 和已挂载的 tracefs。先用 `uname -r` 核对，宿主机上存在源码不代表虚拟机正在运行这份内核。以下脚本为本书编写，**不是内核代码原文，也未在本次写作中实测**。

只操作本实验新建的 `/dev/nullb0`，不要替换为 `/dev/sda`、系统盘或生产逻辑卷。默认非内存后备模式不保存数据，不做数据正确性校验、文件系统或掉电持久化实验。

### 1. 建立可控的模拟设备

参数依据：[`drivers/block/null_blk/main.c:130-233`](../../linux/drivers/block/null_blk/main.c#L130-L233)（v6.18.60）、[`Documentation/block/null_blk.rst`](../../linux/Documentation/block/null_blk.rst)。实验脚本：

```bash
set -eu
if [ -d /sys/module/null_blk ] || [ -e /sys/class/block/nullb0 ]; then
    echo '已有 null_blk / nullb0，停止，避免影响其他实验。' >&2
    exit 1
fi
modprobe null_blk nr_devices=1 queue_mode=2 gb=1 bs=4096 \
    submit_queues=2 hw_queue_depth=64 irqmode=2 completion_nsec=1000000
udevadm settle
test -b /dev/nullb0
lsblk -o NAME,MAJ:MIN,SIZE,TYPE,MOUNTPOINTS /dev/nullb0
cat /sys/block/nullb0/queue/scheduler
cat /sys/block/nullb0/queue/logical_block_size
for f in /sys/block/nullb0/mq/*/cpu_list; do
    printf '%s: ' "$f"; cat "$f"
done
```

这里选择 blk-mq、4 KB 逻辑块、两条提交队列与 1 ms 定时完成。实际可用队列及 CPU 映射应以 sysfs 为准。`irqmode=2` 模拟完成延迟，不模拟真实介质并行和带宽，也不保证每条 I/O 的用户可见时延恰为 1 ms。

### 2. 在独立 trace 实例中观察读请求

事件依据：[`include/trace/events/block.h:118-258`](../../linux/include/trace/events/block.h#L118-L258)、[`include/trace/events/block.h:364-412`](../../linux/include/trace/events/block.h#L364-L412)、[`include/trace/events/block.h:479-516`](../../linux/include/trace/events/block.h#L479-L516)（v6.18.60）。过滤值按内核 `MKDEV()` 的 20 位次设备号编码计算，依据 [`include/linux/kdev_t.h:7-12`](../../linux/include/linux/kdev_t.h#L7-L12)。以下脚本在子 shell 中运行，退出时删除自己建立的实例，不改全局 tracing 开关。

```bash
(
set -eu
T=/sys/kernel/tracing
mountpoint -q "$T" || mount -t tracefs tracefs "$T"
I="$T/instances/ch10-$$"
mkdir "$I"
trap 'echo 0 > "$I/tracing_on"; rmdir "$I"' EXIT
IFS=: read -r major minor < /sys/class/block/nullb0/dev
dev=$((major * 1048576 + minor))
for e in block_bio_queue block_split block_bio_backmerge \
         block_getrq block_rq_insert block_rq_issue block_rq_complete; do
    echo "dev == $dev" > "$I/events/block/$e/filter"
    echo 1 > "$I/events/block/$e/enable"
done
echo 1 > "$I/tracing_on"
fio --name=ch10-read --filename=/dev/nullb0 --readonly \
    --rw=randread --bs=4k --direct=1 --ioengine=libaio \
    --iodepth=32 --numjobs=1 --size=128m --time_based=1 --runtime=5
echo 0 > "$I/tracing_on"
cat "$I/trace" > /tmp/ch10-block-trace.txt
cat "$I"/per_cpu/cpu*/stats
)
```

读取 `/tmp/ch10-block-trace.txt`，检查：

- `block_getrq`、`block_rq_issue` 和完成之间的关系；有没有直接派发导致的普通插入事件缺席。
- 完成是否跨 CPU；事件当前任务名是否与提交进程一致。
- 缓冲区是否覆盖或丢事件。若发生覆盖，缩短运行时间、调低负载或增加实例缓冲区，不应在不完整记录上声称精确计数。
- 这个随机小读负载可能很少合并，也可能没有拆分；这不是事件配置失败。

脚本只过滤带 `dev` 字段的所选事件。`block_plug` / `block_unplug` 的格式不同，不能直接套用同一设备过滤表达式。

### 3. 比较调度策略，不预设赢家

属性依据：[`block/elevator.c:785-865`](../../linux/block/elevator.c#L785-L865)（v6.18.60）。实验脚本保存原策略并在退出时恢复：

```bash
(
set -eu
S=/sys/block/nullb0/queue/scheduler
old=$(awk -F'[][]' '{print $2}' "$S")
trap 'printf "%s\n" "$old" > "$S"' EXIT
for policy in none mq-deadline; do
    printf '%s\n' "$policy" > "$S"
    cat "$S"
    cat /sys/block/nullb0/stat
    fio --name="ch10-$policy" --filename=/dev/nullb0 --readonly \
        --rw=randread --bs=4k --direct=1 --ioengine=libaio \
        --iodepth=64 --numjobs=1 --size=128m --time_based=1 --runtime=10
    cat /sys/block/nullb0/stat
done
)
```

记录吞吐、CPU 使用率、完成时延分布和统计差值；重复运行并交换策略顺序，避免用一次结果下结论。这里的固定定时完成设备不能代表 HDD 寻道，也不能代表实际 NVMe 的内部并行。若 mq-deadline 不在可用列表中，应先核对运行内核配置，而不是继续向真实磁盘写调度属性。

### 4. 拆分与限速的扩展观测

若要看限制影响，可在同一专用设备上先保存 `queue/max_sectors_kb`，调小后以更大 `--bs` 做只读测试，最后恢复属性；接口见 [`block/blk-sysfs.c:277-286`](../../linux/block/blk-sysfs.c#L277-L286)。上层也可能提前切分，因此不能保证“应用块大小 / 限制”正好等于 `block_split` 事件数。

若要观察 `io.max`，应在已正确委派 `io` 控制器的实验 cgroup 中运行同一只读负载，同时记录 `io.stat` 和块事件。不要直接修改系统服务所在组或为了启用控制器迁移宿主系统进程；cgroup 的创建和委派要求见 [`Documentation/admin-guide/cgroup-v2.rst`](../../linux/Documentation/admin-guide/cgroup-v2.rst)。

### 5. 清理

确认 fio 已退出、没有其他使用者，且实验确实由本节创建模块后执行：

```bash
modprobe -r null_blk
```

不使用强制卸载。tracefs 若原本已挂载则保留；脚本建立的实例已删除，`/tmp/ch10-block-trace.txt` 可按需保存或移除。

## 小结

- 先确认设备身份、统计层级和时间边界，再计算吞吐或时延。
- 普通事件文本没有可靠的请求唯一 ID，不能普遍按扇区匹配并发完成。
- 本版本 `stat` 第 9 项与独立 `inflight` 属性的起算边界不同。
- `null_blk` 适合验证提交、调度和完成路径，不代表真实介质或持久化行为。
- 实验应隔离设备与 trace 实例，保存可恢复设置，并明确未实测与真实测量结果的区别。

## 参考

- [`Documentation/block/null_blk.rst`](../../linux/Documentation/block/null_blk.rst)
- [`Documentation/block/stat.rst`](../../linux/Documentation/block/stat.rst)
- [`Documentation/admin-guide/iostats.rst`](../../linux/Documentation/admin-guide/iostats.rst)
- [`Documentation/trace/events.rst`](../../linux/Documentation/trace/events.rst)
