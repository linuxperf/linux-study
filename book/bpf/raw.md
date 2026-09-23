# Linux BPF 子系统概述：从加载到一次执行

本文依据仓库中的 Linux 6.18.52 源码。BPF 的核心思路是：把一段受约束的程序加载到内核，先由验证器检查，再由某个内核事件触发执行。程序可以读取事件上下文、调用该类型允许的内核函数，并借助 map 保存或交换数据。`CONFIG_BPF_SYSCALL` 将程序和 map 的管理入口开放为 `bpf()` 系统调用；`CONFIG_BPF_JIT` 控制即时编译能力。[版本号](../../linux/Makefile#L1-L5) · [配置入口](../../linux/kernel/bpf/Kconfig#L27-L58)

下文的加载主线是 `BPF_PROG_LOAD` 使用的扩展 BPF（eBPF）。源码中还保留经典 BPF（cBPF）过滤器路径：它先做经典指令检查，不能直接 JIT 时可转换成 eBPF 指令表示。阅读两条路径时不要把各自的加载检查混为一谈。[经典过滤器准备](../../linux/net/core/filter.c#L1338-L1376) · [指令转换](../../linux/net/core/filter.c#L1268-L1317)

## 1. 先认识四个对象

| 对象 | 作用 | 在源码中从哪里看 |
| --- | --- | --- |
| **program** | 保存 BPF 指令、程序类型及运行入口；在事件到来时执行 | [`struct bpf_prog`](../../linux/include/linux/bpf.h#L1701-L1745) |
| **map** | 按类型组织数据，例如 hash、array、ring buffer；程序与用户态都可能访问 | [`struct bpf_map`](../../linux/include/linux/bpf.h#L295-L334)、[map 类型](../../linux/include/uapi/linux/bpf.h#L980-L1037) |
| **link** | 表示一次程序与挂载目标的关系，持有程序引用，并提供独立的文件描述符 | [`struct bpf_link`](../../linux/include/linux/bpf.h#L1747-L1765)、[link 创建](../../linux/kernel/bpf/syscall.c#L5699-L5796) |
| **BTF** | 描述类型信息；加载、验证和按类型定位内核对象时会用到 | [`BPF_BTF_LOAD` 分发](../../linux/kernel/bpf/syscall.c#L6243-L6245)、[BTF 解析](../../linux/kernel/bpf/btf.c#L5773-L5840) |

这四者不应混成“一个 BPF 程序”。例如，用户态可以先创建 map，再加载使用它的程序，最后把程序挂到网卡。**加载成功不等于已经挂载，挂载成功也不等于此刻已经执行。**系统调用把 `BPF_MAP_CREATE`、`BPF_PROG_LOAD`、`BPF_LINK_CREATE` 分成了不同命令。[命令定义](../../linux/include/uapi/linux/bpf.h#L937-L978) · [内核分发](../../linux/kernel/bpf/syscall.c#L6180-L6272)

```text
用户态 ── bpf() ──► map
      ├─ bpf() ──► program（验证、选择运行方式）
      └─ bpf() ──► link（绑定 program 与挂载点）

挂载点事件 ──► 执行 program ── helper ──► 访问 map
```

## 2. 程序究竟是什么

BPF 程序最终是指令数组。公开的 `struct bpf_insn` 给每条指令定义操作码、源/目的寄存器、偏移和立即数；寄存器编号为 `R0` 到 `R10`。验证器把 `R1` 初始化为上下文指针，`R0` 是返回寄存器，`R10` 是只读帧指针。程序使用的具体上下文和返回值含义由程序类型与挂载点决定。[指令与寄存器定义](../../linux/include/uapi/linux/bpf.h#L61-L87) · [寄存器约定](../../linux/kernel/bpf/verifier.c#L74-L83) · [入口状态](../../linux/kernel/bpf/verifier.c#L23665-L23676)

**程序类型决定能在哪里运行。**例如 XDP 面向收包路径，公开上下文类型为 `xdp_md`，内核对应 `xdp_buff`；tracepoint 面向跟踪事件，cgroup skb 面向 cgroup 网络路径。类型与上下文的对应关系集中列在 [`bpf_types.h`](../../linux/include/linux/bpf_types.h#L5-L46)，公开类型号在 [`enum bpf_prog_type`](../../linux/include/uapi/linux/bpf.h#L1040-L1075)。`expected_attach_type` 又为一些程序进一步指定预期挂载位置；创建 link 时内核会检查二者是否匹配。[程序字段](../../linux/include/linux/bpf.h#L1701-L1725) · [挂载类型检查](../../linux/kernel/bpf/syscall.c#L5708-L5713)

**程序类型也影响能调用什么。**普通 helper 有明确的参数和返回值原型。以 `bpf_map_lookup_elem` 为例，其返回类型是“map value 指针或 NULL”；验证器检查 helper 是否对当前程序类型开放、参数类型是否正确，以及调用是否违反可睡眠等约束。因而不能把 helper 当作任意内核函数调用。[map helper 原型](../../linux/kernel/bpf/helpers.c#L35-L72) · [helper 调用检查](../../linux/kernel/bpf/verifier.c#L11513-L11580)

## 3. 加载：从 `bpf()` 到可运行程序

入口 [`SYSCALL_DEFINE3(bpf)`](../../linux/kernel/bpf/syscall.c#L6307-L6310) 将命令交给 `__sys_bpf()`。后者复制属性、调用 `security_bpf()`，再按命令分发。由此看加载流程，比先钻进某个具体 hook 更容易建立全局视图。[`__sys_bpf()`](../../linux/kernel/bpf/syscall.c#L6165-L6203)

以 `BPF_PROG_LOAD` 为主线，可以按以下顺序读 [`bpf_prog_load()`](../../linux/kernel/bpf/syscall.c#L2872-L3124)：

1. 检查属性、程序类型、挂载类型、指令数和当前调用者权限。权限检查同时涉及能力、令牌和程序类型，不能概括成“任何程序一律只看一个 capability”。[前置检查](../../linux/kernel/bpf/syscall.c#L2872-L2950)
2. 分配 `bpf_prog`，从调用方复制指令和 license，设置程序元数据。[分配及复制](../../linux/kernel/bpf/syscall.c#L2989-L3040)
3. 调用 `bpf_check()` 验证程序，再调用 `bpf_prog_select_runtime()` 确定执行入口。**验证发生在返回程序 fd 之前。**[验证、运行时选择及 fd](../../linux/kernel/bpf/syscall.c#L3083-L3124)

验证器并非简单扫描非法操作码。它先处理子程序、map/BTF 引用和控制流，再沿指令路径维护寄存器与栈状态；遇到等价的已探索状态可以剪枝。读取上下文或 map value 时会检查类型、偏移、大小等；调用 helper 时要核对原型与使用条件。验证成功只说明程序满足内核在此处施加的安全与接口约束，不保证业务逻辑正确。[`bpf_check()` 的阶段](../../linux/kernel/bpf/verifier.c#L24943-L25137) · [状态探索与剪枝](../../linux/kernel/bpf/verifier.c#L20379-L20440) · [内存访问检查](../../linux/kernel/bpf/verifier.c#L7588-L7642)

程序通过验证后，`bpf_prog_select_runtime()` 选择解释器或尝试 JIT；若配置或程序特性要求 JIT 而未能得到 JIT 结果，加载会失败。解释器的核心函数按操作码分派并执行指令；常规调用入口 `bpf_prog_run()` 最终调用程序的 `bpf_func`。因此“BPF 一定由 JIT 执行”和“BPF 一定由解释器执行”都不符合这份源码。[运行时选择](../../linux/kernel/bpf/core.c#L2552-L2616) · [解释器](../../linux/kernel/bpf/core.c#L1785-L1816) · [运行入口](../../linux/include/linux/filter.h#L732-L763)

## 4. map：为什么程序需要独立的数据对象

一次程序执行结束后，寄存器和栈上的临时状态不会成为下一次执行的共享状态。map 提供了有类型、有生命周期的数据对象：`struct bpf_map` 记录类型、key/value 大小、最大条目数、操作表和引用计数。创建时 `map_create()` 按 `map_type` 找到 `bpf_map_ops`，调用相应的 `map_alloc()`，最后返回 map fd。[map 结构](../../linux/include/linux/bpf.h#L295-L329) · [按类型创建](../../linux/kernel/bpf/syscall.c#L1409-L1429) · [返回 fd](../../linux/kernel/bpf/syscall.c#L1596-L1619)

访问路径有两面：

- 用户态用 map fd 发起 `BPF_MAP_LOOKUP_ELEM`、`BPF_MAP_UPDATE_ELEM` 等命令；系统调用检查权限、复制 key/value，再调用 map 实现。[命令分发](../../linux/kernel/bpf/syscall.c#L6180-L6202) · [更新路径](../../linux/kernel/bpf/syscall.c#L1783-L1835)
- 程序通过允许的 helper 访问 map。以 lookup/update 为例，helper 最后调用 `map->ops->map_lookup_elem` / `map->ops->map_update_elem`。验证器会知道 lookup 可能返回 NULL，后续解引用必须满足相应的路径检查。[lookup/update helper](../../linux/kernel/bpf/helpers.c#L42-L78) · [map value 访问检查](../../linux/kernel/bpf/verifier.c#L7610-L7642)

`bpf_map_ops` 是理解各种 map 的分界线：公共层定义创建、查找、更新接口，具体语义由 array、hash、ring buffer 等实现决定。不能因为它们都叫 map，就假定每种都支持相同的 key/value 操作。[操作表](../../linux/include/linux/bpf.h#L83-L119) · [类型与实现对应](../../linux/include/linux/bpf_types.h#L87-L108)

## 5. 挂载和执行：以 XDP 收包为例

`BPF_LINK_CREATE` 先根据程序 fd 取得程序，检查挂载类型，再把请求交给相应子系统。XDP 分支走 `bpf_xdp_link_attach()`：它根据网卡索引找到设备，建立 XDP link，调用设备挂载路径，成功后安装 link fd。link 结构保存程序、类型和挂载类型；关闭最后一个 link 引用时，其 `release` 回调负责拆除挂载关系。[link 分发](../../linux/kernel/bpf/syscall.c#L5699-L5778) · [XDP link 挂载](../../linux/net/core/dev.c#L10589-L10642) · [link 释放](../../linux/kernel/bpf/syscall.c#L3258-L3323)

挂载后，网卡收包路径才会调用程序。下面是一个驱动中的实际顺序：调用 `bpf_prog_run_xdp()` 得到动作，再分别处理 `XDP_PASS`、`XDP_TX`、`XDP_REDIRECT`、`XDP_DROP` 等结果。这个例子说明 **BPF 的返回值由挂载点解释**；不能把 XDP 动作值套到 tracepoint 或 cgroup 程序上。[XDP 驱动调用点](../../linux/drivers/net/ethernet/engleder/tsnep_main.c#L1287-L1325)

把前面的对象串成一个最小故事：用户态创建 hash map，加载一个 XDP 程序，创建指向网卡的 link；收到包时驱动调用程序，程序可通过 helper 读写 map，随后返回一个 XDP 动作；用户态仍可通过 map fd 读取统计数据。这里的“创建、加载、挂载、触发、读取”是五个不同动作。[map 创建](../../linux/kernel/bpf/syscall.c#L1370-L1429) · [程序加载](../../linux/kernel/bpf/syscall.c#L2872-L3124) · [link 创建](../../linux/kernel/bpf/syscall.c#L5699-L5778) · [驱动调用](../../linux/drivers/net/ethernet/engleder/tsnep_main.c#L1296-L1318) · [map 查询](../../linux/kernel/bpf/syscall.c#L1718-L1777)

XDP 只是一个入口。tracepoint 路径则在事件发生时从程序数组取出程序并调用 `bpf_prog_run()`。由此可见，共用的是 BPF 程序、验证器和运行机制；**何时触发、传入什么上下文、怎样解释返回值**，由各挂载子系统决定。[tracepoint 执行](../../linux/kernel/trace/bpf_trace.c#L110-L151) · [程序类型映射](../../linux/include/linux/bpf_types.h#L5-L46)

## 6. BTF、CO-RE 与对象生命周期

BTF 为类型提供可供内核与加载器使用的描述；`BPF_BTF_LOAD` 将一份 BTF 作为独立对象加载。map 可记录 key/value 的 BTF 类型 ID，某些程序的挂载目标也通过 BTF ID 表达。[BTF 加载](../../linux/kernel/bpf/syscall.c#L5463-L5493) · [map 的 BTF 字段](../../linux/include/linux/bpf.h#L295-L317) · [程序挂载 BTF 字段](../../linux/kernel/bpf/syscall.c#L2971-L2987)

CO-RE 解决的一个问题是：同一份程序访问的内核结构在目标内核上可能有不同布局。仓库中的 libbpf 加载路径会读取目标 BTF、处理 CO-RE 重定位，然后创建 map、加载程序；这属于**加载准备阶段**，不意味着每次触发程序都重新做结构偏移重定位。[libbpf CO-RE 重定位](../../linux/tools/lib/bpf/libbpf.c#L5976-L6045) · [libbpf 准备及加载顺序](../../linux/tools/lib/bpf/libbpf.c#L8652-L8712)

最后要区分 fd 与对象本身。程序、map、link 都可由 fd 引用；map 和程序还可能被其他内核对象持有。`BPF_OBJ_PIN` / `BPF_OBJ_GET` 提供通过 BPF 文件系统保存与重新取得对象引用的路径。因而关闭一个 fd 不必然立即销毁对象；link 的最后引用释放时才会走拆除挂载和回收流程。[对象命令](../../linux/kernel/bpf/syscall.c#L3147-L3178) · [link 引用释放](../../linux/kernel/bpf/syscall.c#L3300-L3323) · [UAPI 生命周期说明](../../linux/include/uapi/linux/bpf.h#L920-L935)

## 7. 建议的源码阅读顺序

1. 从 [`enum bpf_cmd`](../../linux/include/uapi/linux/bpf.h#L937-L978) 和 [`__sys_bpf()`](../../linux/kernel/bpf/syscall.c#L6165-L6298) 看用户态有哪些动作。
2. 沿 [`bpf_prog_load()`](../../linux/kernel/bpf/syscall.c#L2872-L3124) 走到 [`bpf_check()`](../../linux/kernel/bpf/verifier.c#L24943-L25137)，弄清“加载前检查什么”。
3. 看 [`bpf_prog_select_runtime()`](../../linux/kernel/bpf/core.c#L2552-L2616) 和 [`bpf_prog_run()`](../../linux/include/linux/filter.h#L732-L763)，弄清“验证后怎样执行”。
4. 选一个挂载点，例如 [`XDP link`](../../linux/net/core/dev.c#L10589-L10642) 与 [驱动收包调用](../../linux/drivers/net/ethernet/engleder/tsnep_main.c#L1287-L1325)，追踪“事件怎样来到程序”。
5. 最后再按问题深入具体 map、BTF、tracing 或 cgroup 代码；此时每个文件在整条链路中的位置就清楚了。[map 实现索引](../../linux/include/linux/bpf_types.h#L87-L125) · [BPF 编译对象](../../linux/kernel/bpf/Makefile#L10-L52)
