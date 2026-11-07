# 10.11 分区存储设备：zone、append 与写入暂存

> **本节源码**：[`block/blk-zoned.c`](../../linux/block/blk-zoned.c)、[`block/blk-mq.c`](../../linux/block/blk-mq.c)、[`block/blk-core.c`](../../linux/block/blk-core.c)、[`include/uapi/linux/blkzoned.h`](../../linux/include/uapi/linux/blkzoned.h)、[`include/trace/events/block.h`](../../linux/include/trace/events/block.h)
> **关键函数**：`blkdev_report_zones()`、`blkdev_zone_mgmt()`、`blk_zone_plug_bio()`、`blk_zone_write_plug_init_request()`、`blk_zone_write_plug_finish_request()`
> **前置知识**：[10.3 bio](03-bio.md)、[10.6 合并与派发](06-merge-dispatch.md)、[10.8 完成协议](08-completion.md)、[10.10 范围操作](10-range-ops.md)

## 概述

普通块设备的地址模型容易让人假设任意位置都可以独立覆盖写入，分区存储设备（Zoned Block Device，ZBD）则可能要求按 zone 的写指针顺序写。这里的 zone 不是分区表中的 partition，也不是内存管理中的 zone。块层既提供报告和管理接口，也用 zone 写入暂存（Zone Write Plugging）协调普通顺序写与模拟 append。本节以 `CONFIG_BLK_DEV_ZONED=y` 为前提，区分设备约束、软件跟踪与使用者必须遵守的协议。

## 核心数据结构

### 1. zone 描述符与地址单位

[`include/uapi/linux/blkzoned.h:106-118`](../../linux/include/uapi/linux/blkzoned.h#L106-L118)（v6.18.60）

```c
struct blk_zone {
	__u64	start;		/* Zone start sector */
	__u64	len;		/* Zone length in number of sectors */
	__u64	wp;		/* Zone write pointer position */
	__u8	type;		/* Zone type */
	__u8	cond;		/* Zone condition */
	...
	__u64	capacity;	/* Zone capacity in number of sectors */
	...
};
```

`start`、`len`、`capacity`、`wp` 均用 **512 字节扇区**表示，与设备逻辑块大小无关。`len` 是 zone 的地址跨度，`capacity` 是可用容量，二者不能无条件画等号；`wp` 是写指针（Write Pointer）。`type` 区分 conventional 与顺序写类型，`cond` 区分 EMPTY、OPEN、CLOSED、FULL、READONLY 等状态，定义见 [`include/uapi/linux/blkzoned.h:22-104`](../../linux/include/uapi/linux/blkzoned.h#L22-L104)。不是所有 zone 都有顺序写约束。

### 2. blk_zone_wplug 是软件跟踪状态

| 字段 | 含义 |
|---|---|
| `disk`、`zone_no` | 关联磁盘与 zone 编号 |
| `wp_offset` | 相对 zone 起点的软件写指针偏移，单位为扇区 |
| `bio_list`、`bio_work` | 等待的 bio 与负责继续提交的工作项 |
| `flags` | 是否已暂存、是否需要重新获取写指针、是否失效 |
| `lock`、`ref` | 保护并发状态与在途引用 |

定义与标志说明见 [`block/blk-zoned.c:40-97`](../../linux/block/blk-zoned.c#L40-L97)。`wp_offset` 在准备提交时即可推进，因此不能把它当成“设备已经持久化到这个位置”的计数。

任务的 `current->plug` 用于批处理，`blk_zone_wplug` 用于同一 zone 的写入协调，两者作用域和目的不同。后者还跨越异步完成，不随某个提交函数返回就消失。

## 主流程分析

以下以 blk-mq 设备的普通顺序写或模拟 append 为主。首次提交与完成推动的后续提交分开阅读。

[`block/blk-mq.c:3183-3236`](../../linux/block/blk-mq.c#L3183-L3236)、[`block/blk-zoned.c:891-1007`](../../linux/block/blk-zoned.c#L891-L1007)、[`block/blk-zoned.c:1174-1238`](../../linux/block/blk-zoned.c#L1174-L1238)、[`block/blk-zoned.c:1303-1377`](../../linux/block/blk-zoned.c#L1303-L1377)（v6.18.60）

```text
首次提交
blk_mq_submit_bio()                       block/blk-mq.c
├── 按限制拆分、尝试普通合并
├── blk_zone_plug_bio()                   block/blk-zoned.c
│   └── blk_zone_wplug_handle_write()
│       ├── 取得目标 zone 的 plug
│       ├── 可继续：准备写入并设置 PLUGGED
│       └── 需等待：加入 plug 的 bio_list
└── 对可继续的 bio 创建 request
    └── blk_zone_write_plug_init_request()  尝试并入等待中的相邻 bio
请求最终收尾
blk_zone_write_plug_finish_request()      block/blk-zoned.c
└── disk_zone_wplug_unplug_bio()
    └── 调度工作项，稍后执行 blk_zone_wplug_bio_work()
        └── blk_mq_submit_bio()           重新进入请求构造阶段
```

### 1. 普通顺序写必须匹配软件写指针

`blk_zone_wplug_handle_write()` 要求 bio 不跨 zone。blk-mq 已在此前处理必要拆分；自定义 bio 驱动则负责满足这一前提。conventional zone 的普通写不需要此暂存机制，见 [`block/blk-zoned.c:1009-1043`](../../linux/block/blk-zoned.c#L1009-L1043)。

对顺序写 zone，若已有写正在执行或等待，新的 bio 进入 `bio_list`。可继续时由 `blk_zone_wplug_prepare_bio()` 检查当前位置、软件写指针和满区状态，准备后再让请求层处理，见 [`block/blk-zoned.c:952-1007`](../../linux/block/blk-zoned.c#L952-L1007)、[`block/blk-zoned.c:1064-1098`](../../linux/block/blk-zoned.c#L1064-L1098)。它不会自动把任意乱序地址重新排成合法写入序列。

`blk_zone_plug_bio()` 返回 `true` 表示该 bio 已被处理、调用者不应继续构造请求；实际可能是延后提交，也可能已经以错误完成。不能只按返回值统计“暂存成功次数”。

### 2. append 把落点选择交给设备或模拟层

`REQ_OP_ZONE_APPEND` 的输入地址是 zone 起点，不是使用者预选的最终落点。公共检查限制大小不得跨 zone、不得超过 `max_zone_append_sectors`，并加上 `REQ_NOMERGE`，见 [`block/blk-core.c:594-624`](../../linux/block/blk-core.c#L594-L624)。`bio_split()` 拒绝拆分 append；否则各片段可能落到不连续位置，见 [`block/bio.c:1702-1704`](../../linux/block/bio.c#L1702-L1704)。

- **原生 append**：设备决定实际写入位置，不走普通写的暂存队列。块层还会处理已有软件 plug 的失效，见 [`block/blk-zoned.c:1100-1141`](../../linux/block/blk-zoned.c#L1100-L1141)。
- **模拟 append**：块层在锁保护下按 `wp_offset` 选位置，把操作改成普通 WRITE，并保留 `BIO_EMULATES_ZONE_APPEND` 以便完成时恢复，见 [`block/blk-zoned.c:977-991`](../../linux/block/blk-zoned.c#L977-L991)。

完成时，`blk_zone_append_update_request_bio()` 把请求记录的实际位置写回 `bio->bi_iter.bi_sector`，见 [`block/blk-zoned.c:1240-1252`](../../linux/block/blk-zoned.c#L1240-L1252)。因此同一个字段在提交与完成阶段具有不同用途，观测工具不能只保留第一次看到的扇区。

### 3. 暂存 bio 可以合并，但不能丢掉引用

`blk_zone_write_plug_init_request()` 给请求设置 `RQF_ZONE_WRITE_PLUGGING`，再尝试把等待列表中紧邻请求末尾、属性兼容的 bio 合并进来。每合并一个原本暂存的 bio，还要归还它额外持有的队列使用引用，见 [`block/blk-zoned.c:891-945`](../../linux/block/blk-zoned.c#L891-L945)。所以“按 zone 协调”不意味着每次只允许一个 bio 成为一个独立设备命令。

未被合并的 bio 在当前请求收尾后由工作项推进。它已持有队列使用引用、已完成前段准备，重新进入 `blk_mq_submit_bio()` 时走专用分支直达请求构造，不重复普通提交准备，见 [`block/blk-mq.c:3151-3162`](../../linux/block/blk-mq.c#L3151-L3162)、[`block/blk-zoned.c:1326-1377`](../../linux/block/blk-zoned.c#L1326-L1377)。

### 4. 写错误后不能继续猜测写指针

`blk_zone_write_plug_bio_endio()` 在错误完成时中止等待项，并设置 `BLK_ZONE_WPLUG_NEED_WP_UPDATE`，见 [`block/blk-zoned.c:1279-1288`](../../linux/block/blk-zoned.c#L1279-L1288)。后续准备不会盲目沿旧偏移推进；使用者需要通过报告 zone、reset 或 finish 等适当操作恢复可依赖的状态，契约见 [`block/blk-zoned.c:959-967`](../../linux/block/blk-zoned.c#L959-L967)。其中报告是查询，reset / finish 则会改变状态，不能当作无副作用的恢复探针。

原生 append 与普通写并发混用也没有自动排序保证。源码会在发现已有等待普通写与原生 append 冲突时警告并中止等待项，见 [`block/blk-zoned.c:1117-1136`](../../linux/block/blk-zoned.c#L1117-L1136)。

### 5. 管理操作不替使用者同步在途写入

`blkdev_zone_mgmt()` 检查设备模型、只读状态、范围与 zone 对齐，再构造 reset、open、close 或 finish 操作；全盘 reset 另走专用分支，见 [`block/blk-zoned.c:206-258`](../../linux/block/blk-zoned.c#L206-L258)。

reset / finish 不被写入暂存机制当成普通写一样串行化。调用者必须避免它们与在途写冲突，源码明确把这个责任交给使用者，见 [`block/blk-zoned.c:1181-1197`](../../linux/block/blk-zoned.c#L1181-L1197)。zone 管理不是“给普通块设备发一个 discard”的同义表达。

## 关键细节 / 设计考量

- **三个 zone 含义不能混用。** 本节的存储 zone 与磁盘 partition、内存 zone 无关。
- **暂存不等于持久化。** flush 序列在请求层运行于 zone 暂存之后；顺序约束与掉电保障仍是两套协议，见 [`block/blk-zoned.c:1181-1188`](../../linux/block/blk-zoned.c#L1181-L1188)。
- **`REQ_NOWAIT` 不等于立即执行。** 该路径可以暂存 NOWAIT bio，以避免继续执行时因资源问题返回 AGAIN，见 [`block/blk-zoned.c:1067-1075`](../../linux/block/blk-zoned.c#L1067-L1075)。加入暂存列表时会清除轮询标志，并将 NOWAIT bio 转交可阻塞的工作项，见 [`block/blk-zoned.c:811-826`](../../linux/block/blk-zoned.c#L811-L826)。不要把“不在当前线程等待”解释成“设备已接收”。
- **open / active 限额是设备资源限制。** 软件 plug 的数量与设备开放 zone 的数量不是可直接替换的统计值。

## 可观测性

本配置 `CONFIG_BLK_DEV_ZONED=y`、`CONFIG_SYSFS=y`；事件还依赖 `CONFIG_TRACEPOINTS=y`、`CONFIG_EVENT_TRACING=y`、`CONFIG_TRACING=y`，见 [`linux/.config`](../../linux/.config)。

| 入口 | 看什么 | 源码 |
|---|---|---|
| `/sys/block/<disk>/queue/zoned`、`nr_zones` | 设备模型与 zone 数量 | [`block/blk-sysfs.c:578-579`](../../linux/block/blk-sysfs.c#L578-L579) |
| 同目录 `zone_append_max_bytes`、`zone_write_granularity`、`max_open_zones`、`max_active_zones` | append 上限、写入粒度与 zone 资源上限 | [`block/blk-sysfs.c:575-581`](../../linux/block/blk-sysfs.c#L575-L581) |
| `BLKREPORTZONE` ioctl | `start`、`len`、`capacity`、`wp`、`cond` | [`block/blk-zoned.c:277-310`](../../linux/block/blk-zoned.c#L277-L310)；只读报告，不发 reset |
| `block:disk_zone_wplug_add_bio` | 某个 bio 加入 zone 等待列表 | [`include/trace/events/block.h:637-670`](../../linux/include/trace/events/block.h#L637-L670)、[`block/blk-zoned.c:840-842`](../../linux/block/blk-zoned.c#L840-L842)；记录 `dev`、`zno`、`sector`、`nr_sectors` |
| `block:blk_zone_wplug_bio` | 工作项取出等待 bio、准备继续执行 | [`include/trace/events/block.h:672-678`](../../linux/include/trace/events/block.h#L672-L678)、[`block/blk-zoned.c:1348-1352`](../../linux/block/blk-zoned.c#L1348-L1352)；不是设备最终完成 |
| `block:blkdev_zone_mgmt` | 通用助手进入 zone 管理提交 | [`include/trace/events/block.h:611-635`](../../linux/include/trace/events/block.h#L611-L635)；没有最终完成状态 |

最后一个事件尤其要按实现解读：虽然跟踪点接收总范围参数 `nr_sectors`，当前赋值却读取 `bio_sectors(bio)`；调用时传入的又是链尾管理 bio。因此其范围字段不能直接当作整个管理调用的范围，见 [`block/blk-zoned.c:243-255`](../../linux/block/blk-zoned.c#L243-L255)、[`include/trace/events/block.h:624-629`](../../linux/include/trace/events/block.h#L624-L629)。

等待项也可能被合并进新请求而不经过工作项逐个取出，不能要求 add 与 work 事件条数相等。需要测量 zone 等待时，应同时覆盖合并、错误中止和完成路径；本章普通 `null_blk` 实验未启用 zoned 模式，不应期待它产生这些事件。

## 小结

- zone 描述符用 512 字节扇区报告范围和写指针，可用容量不必等于地址跨度。
- 普通顺序写按软件写指针准备，append 则由设备或模拟层选择实际位置。
- zone 写入暂存与任务 plug 不同，还涉及队列引用、请求合并和异步工作项。
- 写错误后必须恢复可靠的写指针状态，不能按旧偏移继续模拟 append。
- reset / finish 与在途写的同步由使用者负责，观察事件也必须核对实际字段赋值。

## 参考

- [`include/uapi/linux/blkzoned.h`](../../linux/include/uapi/linux/blkzoned.h)
- [`block/blk-zoned.c`](../../linux/block/blk-zoned.c)
- [`Documentation/ABI/stable/sysfs-block`](../../linux/Documentation/ABI/stable/sysfs-block)
