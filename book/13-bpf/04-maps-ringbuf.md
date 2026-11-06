# 13.4 BPF map 与环形缓冲区

> **本节源码**：[`include/linux/bpf.h`](../../linux/include/linux/bpf.h)、[`kernel/bpf/syscall.c`](../../linux/kernel/bpf/syscall.c)、[`kernel/bpf/helpers.c`](../../linux/kernel/bpf/helpers.c)、[`kernel/bpf/arraymap.c`](../../linux/kernel/bpf/arraymap.c)、[`kernel/bpf/hashtab.c`](../../linux/kernel/bpf/hashtab.c)、[`kernel/bpf/ringbuf.c`](../../linux/kernel/bpf/ringbuf.c)
> **关键函数**：`map_create()`、`map_lookup_elem()`、`array_map_lookup_elem()`、`htab_map_lookup_elem()`、`__bpf_ringbuf_reserve()`、`bpf_ringbuf_commit()`
> **前置知识**：13.2 对象生命周期、13.3 验证器、7.3 内存屏障、7.9 RCU

## 概述

BPF 程序通常按事件执行，局部寄存器与栈不能替代跨事件状态。map 把状态从程序执行实例中独立出来，允许程序之间、程序与用户态之间共享数据。map 并不都遵循普通键值表语义：程序数组、重定向目标和环形缓冲区各有专用协议。本节先看 array 与 hash 的访问，再看 BPF 环形缓冲区（BPF ring buffer）如何把内核产生的事件发布给用户态。

## 核心数据结构

`struct bpf_map` 是公共头，具体容器再嵌入它。相关字段见 [`include/linux/bpf.h:295-349`](../../linux/include/linux/bpf.h#L295-L349)。

| 字段 | 本节含义 |
|---|---|
| `ops` | 分配、查询、更新、映射和释放等类型操作 |
| `map_type` | 决定实际数据结构与访问协议 |
| `key_size` / `value_size` | 普通键值访问的尺寸约束 |
| `max_entries` | 类型相关容量；ringbuf 中是数据区字节数，不是记录条数 |
| `map_flags` | 访问、分配及类型特有标志 |
| `btf`、`btf_key_type_id`、`btf_value_type_id` | 可选的键值类型信息 |
| `refcnt` / `usercnt` | 对象与用户侧引用跟踪 |
| `frozen` | 用户态更新冻结状态，不应与所有程序访问都只读混为一谈 |

ringbuf 的内部状态包括 `consumer_pos`、`producer_pos` 和 `pending_pos`，并把位置计数器放在不同页上以实施不同映射权限；记录头包含长度与页偏移，见 [`kernel/bpf/ringbuf.c:27-88`](../../linux/kernel/bpf/ringbuf.c#L27-L88)。

## 主流程分析

### 系统调用访问与程序内访问

```text
用户态查询
bpf(BPF_MAP_LOOKUP_ELEM)               kernel/bpf/syscall.c
└── map_lookup_elem()
    └── bpf_map_copy_value()
        └── map->ops->map_lookup_elem() 或类型专用查询

BPF 程序内查询
bpf_map_lookup_elem()                 kernel/bpf/helpers.c
└── map->ops->map_lookup_elem()
    ├── array_map_lookup_elem()       kernel/bpf/arraymap.c
    └── htab_map_lookup_elem()        kernel/bpf/hashtab.c
```

前者把值复制到用户缓冲区；后者返回受验证器约束的内核侧 value 指针。源码分别见 [`kernel/bpf/syscall.c:305-355`](../../linux/kernel/bpf/syscall.c#L305-L355)、[`kernel/bpf/syscall.c:1718-1780`](../../linux/kernel/bpf/syscall.c#L1718-L1780) 与 [`kernel/bpf/helpers.c:43-56`](../../linux/kernel/bpf/helpers.c#L43-L56)。不能把用户态查询返回的副本地址当作程序内查询结果。

这也是逻辑调用链，不保证每次运行都会经过所有 helper 层。array 与 hash 提供了查询指令生成入口，验证器可将部分访问改写成更直接的路径，见 [`kernel/bpf/arraymap.c:221-250`](../../linux/kernel/bpf/arraymap.c#L221-L250) 与 [`kernel/bpf/hashtab.c:781-822`](../../linux/kernel/bpf/hashtab.c#L781-L822)。

### array 与 per-CPU array

array 使用 32 位索引定位预分配元素。普通查询先检查上界，再按元素尺寸和索引计算地址：

[`kernel/bpf/arraymap.c:166-176`](../../linux/kernel/bpf/arraymap.c#L166-L176)（v6.18.60）

```c
/* Called from syscall or from eBPF program */
static void *array_map_lookup_elem(struct bpf_map *map, void *key)
{
	struct bpf_array *array = container_of(map, struct bpf_array, map);
	u32 index = *(u32 *)key;

	if (unlikely(index >= array->map.max_entries))
		return NULL;

	return array->value + (u64)array->elem_size * (index & array->index_mask);
}
```

per-CPU array 的查询返回 `this_cpu_ptr()` 对应的值，见 [`kernel/bpf/arraymap.c:252-263`](../../linux/kernel/bpf/arraymap.c#L252-L263)。它适合分散热点计数，但“每 CPU 一份”不是全局一致快照：用户态需要按 CPU 槽位读取并汇总，内存开销也随可能 CPU 数增长。per-CPU value 的系统调用复制走专门路径，见 [`kernel/bpf/syscall.c:305-335`](../../linux/kernel/bpf/syscall.c#L305-L335)。

### hash 查询与并发

hash 先计算散列、选择桶，再查找元素；成功时返回元素的 value 区域，见 [`kernel/bpf/hashtab.c:751-779`](../../linux/kernel/bpf/hashtab.c#L751-L779)。普通 hash、per-CPU hash 与 LRU hash 共享部分基础设施，但淘汰和 value 布局并不相同，不宜把一种实现的内存与更新代价推广到全部类型。

map 的容器并发协议保护桶、节点和生命周期，不自动把程序对 value 的多次读改写变成原子事务。共享计数器需要合适的原子指令，复合状态需要受支持的锁或其他协议；RCU 保护也不等于数值更新互斥。hash 更新的替换与锁处理可从 [`kernel/bpf/hashtab.c:1177-1347`](../../linux/kernel/bpf/hashtab.c#L1177-L1347) 阅读。

### ringbuf：预留、填充、发布

ringbuf 创建要求 `key_size=0`、`value_size=0`，容量是按页对齐的 2 的幂；普通 lookup/update/delete 不受支持，见 [`kernel/bpf/ringbuf.c:190-262`](../../linux/kernel/bpf/ringbuf.c#L190-L262)。它应通过专用 helper 和映射消费协议使用。

```text
BPF 程序
bpf_ringbuf_reserve()                  kernel/bpf/ringbuf.c
└── __bpf_ringbuf_reserve()
    ├── 检查记录大小与剩余空间
    ├── 串行化预留位置，设置 BUSY 标志
    └── 发布新的 producer_pos
程序填充记录
├── bpf_ringbuf_submit() → bpf_ringbuf_commit(..., false)
└── bpf_ringbuf_discard() → bpf_ringbuf_commit(..., true)
用户态消费者
└── 按记录顺序检查 BUSY / DISCARD，推进 consumer_pos
```

预留过程见 [`kernel/bpf/ringbuf.c:407-463`](../../linux/kernel/bpf/ringbuf.c#L407-L463)。这个实现区分已经预留的位置和真正完成的记录：`producer_pos` 前移不等于数据已经可读，消费者还要观察记录头的 `BUSY` 位。

提交用原子交换清除 `BUSY` 并按需置 `DISCARD`，再根据消费位置与唤醒标志安排 `irq_work`，见 [`kernel/bpf/ringbuf.c:484-509`](../../linux/kernel/bpf/ringbuf.c#L484-L509)。共享环实现多生产者、单消费者（Multi-Producer Single-Consumer，MPSC）协议；较晚预留的记录即使先完成，也不能让消费者跳过前面仍忙的记录。

## 关键细节 / 设计考量

### ringbuf 的顺序是什么

共享环提供的是预留位置所确定的记录次序，不是对多个 CPU 上所有业务事件建立天然的时间全序。需要时间分析时仍应在记录中携带时间戳和业务标识。慢生产者会延后后续记录的消费，业务层不要在预留后执行不必要的长路径。

### 空间不足不是自动阻塞

`__bpf_ringbuf_reserve()` 在空间不足或无法取得相应锁时返回空指针，见 [`kernel/bpf/ringbuf.c:413-449`](../../linux/kernel/bpf/ringbuf.c#L413-L449)。BPF 程序应显式记录预留失败，成功取得记录后则必须提交或丢弃；对应的资源释放语义在 [`kernel/bpf/ringbuf.c:474-535`](../../linux/kernel/bpf/ringbuf.c#L474-L535) 的 helper 原型中声明。

ringbuf 不能自动告诉用户态“业务事件总共丢了多少”；没有应用自定义失败计数，仅凭消费者收到的条数无法推导丢失率。

### 三种不同的环

BPF ringbuf、perf event 缓冲区和 tracefs 跟踪缓冲区是不同机制。`bpf_printk()` 的输出不会因为程序创建了 ringbuf map 就自动写入该 map。选择时应先区分“调试文本”“按 CPU 的 perf 数据”“应用自定义结构化事件”，不要只按名称中有没有 ring 判断。

## 可观测性

依赖 `CONFIG_BPF_SYSCALL=y`，当前已启用；`/proc` 接口还依赖当前已启用的 `CONFIG_PROC_FS=y`。

| 入口 | 观察与限制 |
|---|---|
| `bpftool map show` | 查询类型、键值尺寸、容量与 BTF ID；内核信息填充见 [`kernel/bpf/syscall.c:5287-5372`](../../linux/kernel/bpf/syscall.c#L5287-L5372) |
| `bpftool map lookup` / `bpftool map dump` | 适合支持普通键值查询的 map；ringbuf 不支持这种遍历消费 |
| `/proc/<pid>/fdinfo/<map-fd>` | `max_entries` 要按 map 类型解读，生成函数见 [`kernel/bpf/syscall.c:1010-1050`](../../linux/kernel/bpf/syscall.c#L1010-L1050) |
| 应用自定义 map 计数器 | 区分进入程序、业务匹配、ringbuf 预留失败、成功提交；不是内核统一提供的固定计数项 |
| ringbuf 的查询 helper | 可查看可用数据量与位置，但快照会随并发生产变化，见 [`kernel/bpf/ringbuf.c:566-590`](../../linux/kernel/bpf/ringbuf.c#L566-L590) |

对 map helper 直接挂探针未必覆盖全部查询，因为最终程序可能使用已优化的访问路径。定位热点时，应结合 `xlated`、JIT 指令和程序级统计，而不是把 helper 探针次数视为完整访问次数。

## 小结

- map 是独立对象，不仅包括普通键值表，也包括具有专用协议的容器。
- 用户态 lookup 取得副本，程序内 lookup 取得受验证器约束的 value 指针。
- per-CPU 降低共享热点，但增加内存和汇总成本，不能替代全部并发控制。
- ringbuf 将预留与提交分开，通过记录状态位和内存序保证消费正确性。
- 结构化事件应显式统计生产失败；消费者输出不等于完整事件总量。

## 参考

- [`Documentation/bpf/maps.rst`](../../linux/Documentation/bpf/maps.rst)
- [`Documentation/bpf/map_array.rst`](../../linux/Documentation/bpf/map_array.rst)
- [`Documentation/bpf/map_hash.rst`](../../linux/Documentation/bpf/map_hash.rst)
- [`Documentation/bpf/ringbuf.rst`](../../linux/Documentation/bpf/ringbuf.rst)
