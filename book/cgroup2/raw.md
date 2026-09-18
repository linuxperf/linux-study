**同一个文件可以有属于多个 memcg 的 page cache，但一个 inode 在某一时刻只关联一个回写 wb。** 因此，页面的内存费用与它被写回磁盘时的 I/O 费用，可能属于不同 cgroup。

下面以支持 cgwb 的文件系统、普通 buffered write 为例，假设 A、B 分别有自己的有效 memcg 和 blkcg。

**1．先把“归属”拆开**

这里至少有三种不同含义：

| 归属                | 保存在哪里           | 决定什么                                      |
| ------------------- | -------------------- | --------------------------------------------- |
| 页面内存归属        | `folio_memcg(folio)` | 这块 page cache 消耗哪个 memcg 的内存         |
| inode 回写归属      | `inode->i_wb`        | 谁负责组织这个文件的回写                      |
| 一次回写 I/O 的归属 | `bio->bi_blkg`       | 这个 bio 计入哪个 I/O cgroup、受哪些 I/O 限制 |

另外还有“正在执行 write 的进程属于哪个 cgroup”。**进程的归属，不一定与上述三个归属相同。**

这里的 inode 归属仅指回写关系，与文件的 UID/GID 所有者无关。

**2．例子一：A、B 写同一个文件的不同页面**

假设开始时文件相关范围没有 page cache，A、B 分别触发页面分配：

```text
同一个 inode
│
├── folio 0：由 A 分配并计费 → memcg A
├── folio 1：由 A 分配并计费 → memcg A
├── folio 2：由 B 分配并计费 → memcg B
└── folio 3：由 B 分配并计费 → memcg B

inode->i_wb = wb_A
```

page cache 插入路径在 [`filemap_add_folio()`](/Users/jinqinghui/linux-6.18/mm/filemap.c:968) 调用 `mem_cgroup_charge()`，因此不同 folio 可以有不同的 memcg。

假设 inode 最初关联到 A 的 wb。之后 B 写入时，下面这段代码不会覆盖已有归属：

```c
static inline void inode_attach_wb(struct inode *inode,
                                   struct folio *folio)
{
    if (!inode->i_wb)
        __inode_attach_wb(inode, folio);
}
```

见 [`inode_attach_wb()`](/Users/jinqinghui/linux-6.18/include/linux/writeback.h:222)。

这时，B 的 folio 变脏，会出现两组不同的统计：

```text
folio 2 变脏
    │
    ├── memcg 脏页统计 → B
    │
    └── wb 脏页统计    → wb_A，因为 inode->i_wb == wb_A
```

[`folio_account_dirtied()`](/Users/jinqinghui/linux-6.18/mm/page-writeback.c:2647) 正好体现了这种区别：

```c
/* 按 folio 的归属统计 */
__lruvec_stat_mod_folio(folio, NR_FILE_DIRTY, nr);

/* 按 inode 的回写归属统计 */
wb = inode_to_wb(inode);
wb_stat_mod(wb, WB_RECLAIMABLE, nr);
```

随后开始回写：

```text
wbc->wb = inode->i_wb = wb_A

folio 0、1、2、3 的数据
    → 文件系统构建 bio
    → wbc_init_bio()
    → bio 关联 wb_A->blkcg_css
```

所以此时：

| 页面       | 内存计费 | 本次正常 cgwb 回写 I/O 计费 |
| ---------- | -------- | --------------------------- |
| folio 0、1 | A        | A                           |
| folio 2、3 | B        | A                           |

这是因为 [`wbc_init_bio()`](/Users/jinqinghui/linux-6.18/include/linux/writeback.h:256) 使用的是：

```c
bio_associate_blkg_from_css(bio, wbc->wb->blkcg_css);
```

**它没有逐个查询 bio 中 folio 的 memcg，再分别决定 I/O 归属。**

从 `wb_A` 的角度看，folio 2、3 就是 **foreign pages：内存归属与当前回写归属不一致的页面**。

**3．内核怎样发现 B 已经成为主要写入方？**

内核在回写过程中观察“写出的数据来自哪个 memcg 的 folio”，据此估计 inode 是否应该换一个 wb。

注意这个措辞：它观察的是 **folio 的 memcg**，没有记录每次 write 的实际执行者。

文件系统把数据加入 bio 时，会调用：

```c
wbc_account_cgroup_owner(wbc, folio, bytes);
```

例如 ext4 的调用在 [`io_submit_add_bh()`](/Users/jinqinghui/linux-6.18/fs/ext4/page-io.c:436)。

[`wbc_account_cgroup_owner()`](/Users/jinqinghui/linux-6.18/fs/fs-writeback.c:977) 维护三组信息：

| 字段                             | 含义                                              |
| -------------------------------- | ------------------------------------------------- |
| `wb_id` / `wb_bytes`             | 当前 inode 所属 memcg，以及本轮写出的该归属字节数 |
| `wb_lcand_id` / `wb_lcand_bytes` | 上轮获胜者，以及本轮写出的该归属字节数            |
| `wb_tcand_id` / `wb_tcand_bytes` | 本轮通过投票得到的外来候选者及其票值              |

对当前归属的判断很直接：

```c
css = mem_cgroup_css_from_folio(folio);
id = css->id;

if (id == wbc->wb_id) {
    wbc->wb_bytes += bytes;
    return;
}
```

外来归属使用按字节加权的 Boyer–Moore 投票。它只保留有限候选状态，没有为所有 cgroup 建立完整统计表。

因此，`wb_tcand_bytes` 是经过相互抵消后的候选票值；有多个外来 cgroup 时，不能把它理解成候选者写出的精确总字节数。

举一个只有 A、B 的简单例子：

```text
当前 inode 属于 A。

本轮实际写出的页面数据：
    属于 A 的 folio：20 MiB
    属于 B 的 folio：80 MiB

本轮判断：
    B 是获胜者
    本轮记为 foreign 占主导
```

但**单轮获胜不会直接保证切换**。内核还要检查历史，避免偶尔出现一批外来页面就来回切换。

**4．“持续占主导”具体怎样判断？**

本轮回写尝试结束后，[`wbc_detach_inode()`](/Users/jinqinghui/linux-6.18/fs/fs-writeback.c:881) 会：

1. 从当前归属、上轮候选、本轮候选中选出本轮获胜者。
2. 用获胜者的字节数和 wb 的估算写带宽，估算这批数据需要的 I/O 时间。
3. 把本轮是否由外来归属获胜，写入 inode 的历史位图。
4. 历史中外来归属占比足够高时，请求切换。

这里的时间大致是：

```text
估算 I/O 时间 ≈ 获胜者的数据量 / wb 的估算写带宽
```

**它不是测量某个进程已经写了多久，也不是等待 bio 完成后统计实际耗时。**

这份源码的参数是：

```text
历史窗口：约 2 秒的估算 I/O 时间
历史位图：16 位
每位：约 1/8 秒
一轮最多推进：5 位
```

设位图中的 `1` 表示对应记录由外来归属获胜，切换条件是：

```c
if (hweight16(history) > WB_FRN_HIST_THR_SLOTS)
    inode_switch_wbs(inode, max_id);
```

`WB_FRN_HIST_THR_SLOTS` 是 8，因此这里实际要求 **超过 8 位，也就是至少 9 位为 1**。

此外，估算时间太小的回写轮次会被过滤，减少零星写入的影响。参数和设计说明见 [`foreign inode detection`](/Users/jinqinghui/linux-6.18/fs/fs-writeback.c:221)。

还要注意两个细节：

- 这 16 位记录的是“当前 owner 还是外来 owner 获胜”，并不分别记录 B、C 的全部历史。
- 切换目标取本轮获胜者，因此这是一个启发式归属调整算法，不能保证精确按各 cgroup 的写入比例分摊 I/O。

**5．从 A 切换到 B，究竟改变什么？**

切换异步执行，主要路径是：

```text
inode_switch_wbs(inode, B)
    → 查找或创建 wb_B
    → 设置 I_WB_SWITCH
    → 排队异步工作
    → 等待 RCU 宽限期
    → inode_do_switch_wbs()
```

[`inode_do_switch_wbs()`](/Users/jinqinghui/linux-6.18/fs/fs-writeback.c:395) 会做几类事情：

| 操作                                         | 结果                             |
| -------------------------------------------- | -------------------------------- |
| 修改 `inode->i_wb`                           | inode 关联到 `wb_B`              |
| 移动 inode 的回写链表位置                    | 后续由新 wb 组织回写             |
| 转移 `WB_RECLAIMABLE`、`WB_WRITEBACK` 等统计 | 当前该 inode 的回写状态转到新 wb |
| 清理 foreign history                         | 重新积累归属判断历史             |

**folio 的内存 charge 不会随着这次操作转给 B。**

切换后可以变成：

```text
folio 0、1：内存仍属于 A
folio 2、3：内存仍属于 B

inode->i_wb：变成 wb_B
```

后续新建立的回写上下文使用 `wb_B`，连 A 的页面也可能按 B 的 I/O 归属写出。

切换也不会追溯修改已经提交的 bio。已经建立的 `wbc` 持有自己的 wb 引用；inode 后来换了 wb，不代表已有回写上下文和 bio 同时全部改为新归属。关联代码见 [`wbc_attach_and_unlock_inode()`](/Users/jinqinghui/linux-6.18/fs/fs-writeback.c:793)。

所以不能把它理解成“把过去错误计给 A 的 I/O，重新结算给 B”。

**6．例子二：B 修改的是 A 已经缓存的页面**

这是更难处理的情况。

```text
时刻 T1：
    A 读取文件，将 folio X 带入 page cache
    folio_memcg(X) = A

时刻 T2：
    B 修改 folio X
    folio_memcg(X) 仍然是 A

时刻 T3：
    回写 X
    wbc_account_cgroup_owner() 看到的仍然是 A
```

即使实际修改数据的一直是 B，检测算法也可能一直认为这批数据属于 A。

原因是：

- B 复用的是同一个 page cache folio。
- 修改已有 folio 不会自动重新执行内存 charge。
- 内核没有在 folio 中维护“最后一次修改来自哪个 cgroup”的独立字段供 cgwb 使用。
- 回写把页面变干净，也不会因此解除页面的内存 charge；它还可能继续留在 page cache 中。

因此，**B 持续覆盖 A 的缓存页面，并不能保证 inode 最终切换到 B。**

同一 folio 内不同字节由不同 cgroup 修改时，问题更明显：folio 的内存 charge 本身也只有一个归属，无法表达这些字节各自由谁写入。

内核文档明确指出，多 cgroup 同时写同一 inode，尤其是重叠范围，不能获得准确的回写归属。[官方说明](https://docs.kernel.org/6.18/admin-guide/cgroup-v2.html#writeback)

**7．页面属于 B，inode 属于 A，会不会导致 B 被节流却没人回写？**

会出现这种风险，因此还有一套 **foreign dirty flushing** 机制。

假设：

```text
B 的内存额度较小，脏页比例很高
A 的内存额度较大，脏页比例还很低

B 的脏页都在一个由 wb_A 管理的 inode 中
```

B 的写入进程进入脏页平衡时，使用当前任务对应的 wb：

```text
balance_dirty_pages_ratelimited_flags()
    → wb_get_create_current()
    → wb_B
```

但真正挂着脏 inode 的是 `wb_A`。仅推动 `wb_B`，未必能清掉 B 的那些脏页。

内核因此做了两步：

```text
B 的 folio 在 wb_A 管理的 inode 中变脏
    → mem_cgroup_track_foreign_dirty()
    → 在 B 中记录：有脏页由 wb_A 负责

B 因脏页压力进入节流
    → mem_cgroup_flush_foreign()
    → 请求 wb_A 开始回写
```

设计动机在 [`Foreign dirty flushing` 注释](/Users/jinqinghui/linux-6.18/mm/memcontrol.c:3410) 中讲得很清楚。

**这个机制解决的是“推动谁回写才能释放脏页压力”，不会把这次 I/O 自动改计给 B。** 如果回写上下文仍绑定 `wb_A`，bio 仍使用 A 的有效 blkcg。

因此，分析共享文件的异常统计或限速时，要分别核对：

```text
谁执行 write
    ↓
被修改的 folio charge 给谁
    ↓
inode 当前关联哪个 wb
    ↓
本次 wbc 固定的是哪个 wb
    ↓
bio 最终关联哪个 blkg
```

分别打开文件、使用不同文件描述符或写不同偏移，都不会为同一个 inode 创建独立的回写归属。如果需要 A、B 长期拥有清晰的回写 I/O 归属，使用不同 inode 的文件更符合 cgwb 的管理方式。