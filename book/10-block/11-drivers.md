# 10.11 驱动衔接：NVMe 与 virtio-blk

> **本节源码**：[`drivers/nvme/host/pci.c`](../../linux/drivers/nvme/host/pci.c)、[`drivers/nvme/host/core.c`](../../linux/drivers/nvme/host/core.c)、[`drivers/nvme/host/nvme.h`](../../linux/drivers/nvme/host/nvme.h)、[`drivers/block/virtio_blk.c`](../../linux/drivers/block/virtio_blk.c)
> **关键函数**：`nvme_queue_rq()`、`nvme_prep_rq()`、`nvme_handle_cqe()`、`nvme_complete_rq()`、`virtio_queue_rq()`、`virtblk_done()`
> **前置知识**：10.5 标签与映射、10.6 派发、10.8 完成、5.5 MSI、DMA 基本概念

## 概述

通用块层把请求交给 `mq_ops`，但并不负责构造每种设备协议的命令。驱动要把请求范围转换为协议地址，把内存向量映射为设备可访问的数据描述，并把完成项重新对应到请求。NVMe PCI 通过提交 / 完成队列与 doorbell 和控制器交互，virtio-blk 则通过 virtqueue 与虚拟化后端交互。两者共享 blk-mq 生命周期，但完成身份与底层通知机制不同。

## 核心数据结构

| 层次 | NVMe PCI | virtio-blk |
|---|---|---|
| 块层硬件上下文 | `hctx->driver_data` 关联 `nvme_queue` | 由 `hctx->queue_num` 等选择 `virtio_blk_vq` |
| 请求私有区 | `nvme_iod`，保存命令及 DMA 映射状态 | `virtblk_req`，保存协议头、状态与散集表 |
| 设备命令组织 | 提交队列（Submission Queue，SQ）与完成队列（Completion Queue，CQ） | virtqueue 描述符与已用缓冲区 |
| 找回请求 | command ID 中的标签与代数 | virtqueue 返回的 `virtblk_req` 指针 cookie |

块层通过 `cmd_size` 为请求私有区（Protocol Data Unit，PDU，此处指驱动附加上下文）预留空间，分配见 [`block/blk-mq.c:3609-3677`](../../linux/block/blk-mq.c#L3609-L3677)。两种驱动的操作表见 [`drivers/nvme/host/pci.c:2075-2087`](../../linux/drivers/nvme/host/pci.c#L2075-L2087) 与 [`drivers/block/virtio_blk.c:1227-1235`](../../linux/drivers/block/virtio_blk.c#L1227-L1235)。

## 主流程分析

### 1. NVMe PCI：构造命令，再通知控制器

```text
nvme_queue_rq()                           drivers/nvme/host/pci.c
├── 就绪与队列状态检查
├── nvme_prep_rq()                        drivers/nvme/host/pci.c
│   ├── nvme_setup_cmd()                  drivers/nvme/host/core.c
│   ├── nvme_map_data()                   drivers/nvme/host/pci.c
│   └── nvme_start_request()              drivers/nvme/host/nvme.h
│       └── blk_mq_start_request()
├── nvme_sq_copy_cmd()
└── nvme_write_sq_db()
```

命令准备与派发代码见 [`drivers/nvme/host/pci.c:1194-1259`](../../linux/drivers/nvme/host/pci.c#L1194-L1259)。普通读写命令中，块层 512 字节扇区位置转换为命名空间逻辑块地址（Logical Block Address，LBA）；长度按命名空间块大小计算，并编码为“块数减一”，见 [`drivers/nvme/host/core.c:988-1027`](../../linux/drivers/nvme/host/core.c#L988-L1027)。`REQ_FUA` 同时转换为协议控制位。

设备不能直接使用 `struct page *` 或 CPU 虚拟地址。`nvme_map_data()` 建立 DMA 映射，并按控制器能力使用物理区域页（Physical Region Page，PRP）或散集列表（Scatter-Gather List，SGL）等数据描述；实际实现见 [`drivers/nvme/host/pci.c:1066-1093`](../../linux/drivers/nvme/host/pci.c#L1066-L1093)。这里的“物理”也不能简单理解为未经 DMA / IOMMU 转换的 CPU 地址。

命令写入 SQ 后更新 doorbell；`bd->last` 允许批量通知，而不是每条命令都立即进行一次 MMIO。`queue_rqs()` 还提供批量提交路径，所以逐条 `queue_rq()` 探针不是请求总数的可靠计数器。

### 2. NVMe：用完成项定位请求，并决定是否最终结束

```text
nvme_irq() / nvme_poll()                  drivers/nvme/host/pci.c
└── nvme_poll_cq()
    └── nvme_handle_cqe()
        ├── nvme_find_rq()                drivers/nvme/host/nvme.h
        ├── 完成路由 / 批量完成尝试
        └── nvme_pci_complete_rq()        drivers/nvme/host/pci.c
            ├── DMA 解除映射
            └── nvme_complete_rq()        drivers/nvme/host/core.c
                ├── 最终结束 → nvme_end_req() → blk_mq_end_request()
                ├── 重试
                └── 故障转移等协议恢复分支
```

CQ 消费先检查 phase，再用 `dma_rmb()` 保证观察到对应完成项其余内容，见 [`drivers/nvme/host/pci.c:1333-1340`](../../linux/drivers/nvme/host/pci.c#L1333-L1340) 与 [`drivers/nvme/host/pci.c:1404-1423`](../../linux/drivers/nvme/host/pci.c#L1404-L1423)。这不是普通 CPU 线程间的“碰巧可见”，而是与设备 DMA 交互的内存顺序要求。

本版本普通 NVMe command ID 不是无条件等于 `rq->tag`：

[`drivers/nvme/host/nvme.h:656-664`](../../linux/drivers/nvme/host/nvme.h#L656-L664)（v6.18.60）

```c
#define nvme_genctr_mask(gen)			(gen & 0xf)
#define nvme_cid_install_genctr(gen)		(nvme_genctr_mask(gen) << 12)
#define nvme_genctr_from_cid(cid)		((cid & 0xf000) >> 12)
#define nvme_tag_from_cid(cid)			(cid & 0xfff)

static inline u16 nvme_cid(struct request *rq)
{
	return nvme_cid_install_genctr(nvme_req(rq)->genctr) | rq->tag;
}
```

`nvme_find_rq()` 提取标签并核对代数，降低旧完成项被误认作已复用请求的风险，见 [`drivers/nvme/host/nvme.h:666-685`](../../linux/drivers/nvme/host/nvme.h#L666-L685)。某些控制器 quirks 另有处理，不能把该编码推导成所有控制器的一般硬件规范。

`nvme_complete_rq()` 还会决定最终结束、重试、故障转移或认证恢复，见 [`drivers/nvme/host/core.c:455-494`](../../linux/drivers/nvme/host/core.c#L455-L494)。只有结束分支才进入 `blk_mq_end_request()`，见 [`drivers/nvme/host/core.c:447-453`](../../linux/drivers/nvme/host/core.c#L447-L453)。

### 3. virtio-blk：以 virtqueue cookie 找回驱动私有区

```text
virtio_queue_rq()                         drivers/block/virtio_blk.c
├── virtblk_prep_rq()
│   ├── virtblk_setup_cmd()
│   ├── virtblk_map_data()
│   └── blk_mq_start_request()
├── virtblk_add_req()
└── virtqueue 通知后端
完成回调（异步触发）
virtblk_done()                            drivers/block/virtio_blk.c
├── virtqueue_get_buf() 返回 virtblk_req
├── blk_mq_rq_from_pdu() 找回 request
└── blk_mq_complete_request()
    └── virtblk_request_done()
        └── blk_mq_end_request()
```

提交路径见 [`drivers/block/virtio_blk.c:403-463`](../../linux/drivers/block/virtio_blk.c#L403-L463)，完成路径见 [`drivers/block/virtio_blk.c:334-376`](../../linux/drivers/block/virtio_blk.c#L334-L376)。这里完成关联主要依赖驱动作为 cookie 交给 virtqueue 的私有对象，不是由块层扫描扇区位置寻找请求。

virtqueue 没有空间时，驱动停止相应硬件队列并返回设备资源不足；完成取回缓冲区后重启停止队列，见 [`drivers/block/virtio_blk.c:442-453`](../../linux/drivers/block/virtio_blk.c#L442-L453) 与 [`drivers/block/virtio_blk.c:371-374`](../../linux/drivers/block/virtio_blk.c#L371-L374)。它对应的正是 [10.6](06-merge-dispatch.md) 中的背压协议。

## 关键细节 / 设计考量

- **hctx 不等于 SQ / virtqueue 内存。** 它负责与驱动关联和调度，设备协议环由驱动与传输层维护。
- **数据描述不等于数据复制。** DMA 映射和散集描述允许直接引用数据页，但是否需要额外映射、反弹或转换取决于平台能力。
- **虚拟机完成不自动证明宿主持久化。** 持久化要求仍要通过 flush / FUA 及后端正确实现传递，见 [10.9](09-flush-discard.md)。
- **本节不展开 NVMe 多路径。** 当前 `CONFIG_NVME_MULTIPATH=y`，启用后可能增加重映射与故障转移，不能把单路径事件数直接套用到多路径设备。

## 可观测性

| 入口 | 内容与解释 | 配置 / 源码 |
|---|---|---|
| `nvme:nvme_setup_cmd` | 控制器、队列、command ID 与 opcode | [`drivers/nvme/host/trace.h:47-83`](../../linux/drivers/nvme/host/trace.h#L47-L83)，`CONFIG_BLK_DEV_NVME=m`、`CONFIG_NVME_CORE=m`，驱动需加载，另依赖 `CONFIG_TRACING=y` |
| `nvme:nvme_complete_rq` | 协议完成结果；其后可能重试，不等于上层最终完成 | [`drivers/nvme/host/trace.h:85-115`](../../linux/drivers/nvme/host/trace.h#L85-L115)、[`drivers/nvme/host/core.c:455-494`](../../linux/drivers/nvme/host/core.c#L455-L494) |
| `/sys/block/<disk>/mq/<n>/cpu_list` | 比较驱动映射与提交 CPU；不是设备内部并行度测量 | [`block/blk-mq-sysfs.c:82-115`](../../linux/block/blk-mq-sysfs.c#L82-L115)，`CONFIG_SYSFS=y` |
| `virtio_queue_rq()`、`virtblk_done()` 等动态探针 | virtqueue 提交与取回；批量路径另需覆盖 | [`drivers/block/virtio_blk.c:350-463`](../../linux/drivers/block/virtio_blk.c#L350-L463)，`CONFIG_VIRTIO_BLK=y`，探针须检查符号与可挂载性 |

跨层诊断时，先用块事件确定请求执行边界，再用驱动事件检查是否拆成多次协议尝试。不要把控制器编号、硬件队列号和块设备主次设备号混为一个身份。

## 小结

- 驱动把请求转换为协议命令及 DMA 数据描述，块层不替驱动维护设备环。
- NVMe 完成以队列、标签和代数关联请求，command ID 并非始终等于裸标签。
- virtio-blk 通过 virtqueue cookie 找回请求私有区，描述符资源不足需要停队与重启。
- 协议完成可能转为重试，只有最终结束才通知上层 bio。
- 批量提交和批量完成都会影响探针计数，观测必须覆盖实际运行分支。

## 参考

- [`Documentation/block/blk-mq.rst`](../../linux/Documentation/block/blk-mq.rst)
- [`drivers/nvme/host/pci.c`](../../linux/drivers/nvme/host/pci.c)
- [`drivers/block/virtio_blk.c`](../../linux/drivers/block/virtio_blk.c)
