# 5.5 中断控制器：Local APIC、I/O APIC 与 MSI

> **本节源码**：[`arch/x86/include/asm/apic.h`](../../linux/arch/x86/include/asm/apic.h)、[`arch/x86/include/asm/apicdef.h`](../../linux/arch/x86/include/asm/apicdef.h)、[`arch/x86/kernel/apic/apic.c`](../../linux/arch/x86/kernel/apic/apic.c)、[`arch/x86/kernel/apic/x2apic_phys.c`](../../linux/arch/x86/kernel/apic/x2apic_phys.c)、[`arch/x86/kernel/apic/vector.c`](../../linux/arch/x86/kernel/apic/vector.c)、[`arch/x86/include/asm/io_apic.h`](../../linux/arch/x86/include/asm/io_apic.h)、[`arch/x86/kernel/apic/io_apic.c`](../../linux/arch/x86/kernel/apic/io_apic.c)、[`arch/x86/include/asm/msi.h`](../../linux/arch/x86/include/asm/msi.h)、[`arch/x86/kernel/apic/msi.c`](../../linux/arch/x86/kernel/apic/msi.c)、[`arch/x86/kernel/kvm.c`](../../linux/arch/x86/kernel/kvm.c)
> **关键函数**：`setup_local_APIC()`、`apic_eoi()`、`x2apic_send_IPI()`、`local_apic_timer_interrupt()`、`ioapic_ack_level()`、`__eoi_ioapic_pin()`、`ioapic_set_affinity()`、`__irq_msi_compose_msg()`、`msi_set_affinity()`
> **前置知识**：5.1 x86_64 中断与异常体系、5.2 中断入口与出口

## 概述

5.1、5.2 节讨论的是"CPU 收到一个向量之后"的事情。本节回答前一个问题：外部设备的中断信号是怎样变成某个 CPU 上的某个向量的？

现代 x86 平台上，这条路径由三类硬件组成：每个 CPU 核内有一个**本地 APIC**（Local Advanced Programmable Interrupt Controller，LAPIC），负责接收中断、按优先级投递给 CPU、发送处理器间中断（IPI）以及提供本地定时器；芯片组中有若干**I/O APIC**，把传统的中断引脚（例如 ISA IRQ 和 PCI INTx）翻译成发往某个 LAPIC 的消息；PCIe 设备则普遍使用**消息信号中断**（Message Signaled Interrupts，MSI / MSI-X），设备直接向一个特殊的内存地址写入数据，由平台把这次写操作转换为中断消息。在启用 IOMMU 时，中间还会插入一层**中断重映射**（Interrupt Remapping）。

Linux 在 x86 上用 `struct apic` 抽象不同模式的 LAPIC，用分层的 `irq_domain` 把 I/O APIC、MSI、中断重映射与"向量分配"串联起来（5.6、5.8 节）。本节聚焦硬件模型和 x86 特有的驱动代码，通用中断层的数据结构留给 5.6 节。

## 硬件模型

```
  设备 A（传统引脚）     设备 B（PCIe MSI-X）
        │ INTx#                │ 内存写：addr=0xFEExxxxx, data=vector
        ▼                      │
  ┌────────────┐               │        ┌──────────────────┐
  │  I/O APIC  │──── 中断消息 ──┼──────▶│ 中断重映射（可选） │（IOMMU）
  │ 重定向表    │               │        └────────┬─────────┘
  └────────────┘               │                 │
                               ▼                 ▼
      ┌──────────────── 系统总线 / 中断消息 ─────────────────┐
      ▼                    ▼                   ▼            ▼
  ┌────────┐          ┌────────┐          ┌────────┐   ┌────────┐
  │ LAPIC 0│◀── IPI ──│ LAPIC 1│          │ LAPIC 2│   │ LAPIC 3│
  │ IRR/ISR│          │        │          │        │   │        │
  │ 定时器  │          │        │          │        │   │        │
  └───┬────┘          └───┬────┘          └───┬────┘   └───┬────┘
      ▼                   ▼                   ▼            ▼
    CPU 0               CPU 1               CPU 2        CPU 3
```

*图 5-5：x86 中断投递的硬件路径*

从 LAPIC 的角度看，无论中断来自 I/O APIC、MSI 还是其他 CPU 的 IPI，到达时都是"一条带向量号的消息"。LAPIC 内部维护三个 256 位寄存器：

- **IRR**（Interrupt Request Register）：已收到、等待投递给 CPU 的中断；
- **ISR**（In-Service Register）：已投递给 CPU、正在处理、尚未收到 EOI 的中断；
- **TMR**（Trigger Mode Register）：对应向量是否为电平触发。

当 CPU 处于可接受中断的状态（`IF=1`，且向量优先级类高于当前处理中的最高优先级类和任务优先级寄存器 TPR），LAPIC 从 IRR 中选出优先级最高的向量投递给 CPU，同时在 ISR 中置位。处理函数结束时向 EOI 寄存器写 0，LAPIC 清除 ISR 中的最高位，允许同一或更低优先级类的中断继续投递。**忘记发 EOI 会让该优先级类及以下的所有中断都被挡住**，这就是 5.2 节 `common_interrupt()` 在找不到描述符时也要调用 `apic_eoi()` 的原因。

## 本地 APIC

### xAPIC 与 x2APIC

LAPIC 有两种编程接口：

- **xAPIC**：寄存器映射在物理地址 `0xFEE00000` 开始的 4 KB MMIO 区域，APIC ID 为 8 位，最多寻址 255 个 CPU。
- **x2APIC**：寄存器通过 MSR `0x800` 起的一段地址访问（`APIC_BASE_MSR`，[`arch/x86/include/asm/apicdef.h:152`](../../linux/arch/x86/include/asm/apicdef.h#L152)），APIC ID 扩展为 32 位；ICR（中断命令寄存器）合并为一个 64 位 MSR，发送 IPI 只需一次 `wrmsr`。

本书配置开启了 `CONFIG_X86_X2APIC`。KVM 等现代虚拟化平台普遍向客户机提供 x2APIC，因为 MSR 访问比 MMIO 更容易高效地虚拟化。x2APIC 寄存器的 MSR 地址就是 xAPIC 偏移右移 4 位后加上 `0x800`：

[`arch/x86/include/asm/apic.h:206-218`](../../linux/arch/x86/include/asm/apic.h#L206-L218)（v6.18.60）

```c
static inline void native_apic_msr_write(u32 reg, u32 v)
{
	if (reg == APIC_DFR || reg == APIC_ID || reg == APIC_LDR ||
	    reg == APIC_LVR)
		return;

	wrmsrq(APIC_BASE_MSR + (reg >> 4), v);
}

static inline void native_apic_msr_eoi(void)
{
	native_wrmsrq(APIC_BASE_MSR + (APIC_EOI >> 4), APIC_EOI_ACK);
}
```

### struct apic：LAPIC 驱动抽象

平台上可能有多种 LAPIC 工作模式（xAPIC flat、xAPIC physflat、x2APIC physical、x2APIC cluster、SGI UV、AMD Secure AVIC……），内核在启动时探测并选定一种，以 `struct apic` 描述：

[`arch/x86/include/asm/apic.h:274-325`](../../linux/arch/x86/include/asm/apic.h#L274-L325)（v6.18.60）

```c
struct apic {
	/* Hotpath functions first */
	void	(*eoi)(void);
	void	(*native_eoi)(void);
	void	(*write)(u32 reg, u32 v);
	u32	(*read)(u32 reg);

	/* IPI related functions */
	void	(*wait_icr_idle)(void);
	u32	(*safe_wait_icr_idle)(void);

	void	(*send_IPI)(int cpu, int vector);
	void	(*send_IPI_mask)(const struct cpumask *mask, int vector);
	void	(*send_IPI_mask_allbutself)(const struct cpumask *msk, int vec);
	void	(*send_IPI_allbutself)(int vector);
	void	(*send_IPI_all)(int vector);
	void	(*send_IPI_self)(int vector);

	u32	disable_esr		: 1,
		dest_mode_logical	: 1,	// [注] 逻辑目的模式（cluster）或物理模式
		x2apic_set_max_apicid	: 1,
		nmi_to_offline_cpu	: 1;

	u32	(*calc_dest_apicid)(unsigned int cpu);

	/* ICR related functions */
	u64	(*icr_read)(void);
	void	(*icr_write)(u32 low, u32 high);

	/* The limit of the APIC ID space. */
	u32	max_apic_id;

	/* Probe, setup and smpboot functions */
	int	(*probe)(void);
	void	(*setup)(void);
	void	(*teardown)(void);
	int	(*acpi_madt_oem_check)(char *oem_id, char *oem_table_id);

	void	(*init_apic_ldr)(void);
	u32	(*cpu_present_to_apicid)(int mps_cpu);

	u32	(*get_apic_id)(u32 id);

	/* wakeup_secondary_cpu */
	int	(*wakeup_secondary_cpu)(u32 apicid, unsigned long start_eip, unsigned int cpu);
	/* wakeup secondary CPU using 64-bit wakeup point */
	int	(*wakeup_secondary_cpu_64)(u32 apicid, unsigned long start_eip, unsigned int cpu);

	void	(*update_vector)(unsigned int cpu, unsigned int vector, bool set);

	char	*name;
};
```

以物理 x2APIC 为例：

[`arch/x86/kernel/apic/x2apic_phys.c:132-165`](../../linux/arch/x86/kernel/apic/x2apic_phys.c#L132-L165)（v6.18.60）

```c
static struct apic apic_x2apic_phys __ro_after_init = {

	.name				= "physical x2apic",
	.probe				= x2apic_phys_probe,
	.acpi_madt_oem_check		= x2apic_acpi_madt_oem_check,

	.dest_mode_logical		= false,

	.disable_esr			= 0,

	.cpu_present_to_apicid		= default_cpu_present_to_apicid,

	.max_apic_id			= UINT_MAX,
	.x2apic_set_max_apicid		= true,
	.get_apic_id			= x2apic_get_apic_id,

	.calc_dest_apicid		= apic_default_calc_apicid,

	.send_IPI			= x2apic_send_IPI,
	.send_IPI_mask			= x2apic_send_IPI_mask,
	.send_IPI_mask_allbutself	= x2apic_send_IPI_mask_allbutself,
	.send_IPI_allbutself		= x2apic_send_IPI_allbutself,
	.send_IPI_all			= x2apic_send_IPI_all,
	.send_IPI_self			= x2apic_send_IPI_self,
	.nmi_to_offline_cpu		= true,

	.read				= native_apic_msr_read,
	.write				= native_apic_msr_write,
	.eoi				= native_apic_msr_eoi,
	.icr_read			= native_x2apic_icr_read,
	.icr_write			= native_x2apic_icr_write,
};

apic_driver(apic_x2apic_phys);
```

`apic_driver()` 把它放进 `.apicdrivers` 段，启动时按顺序调用各驱动的 `probe()` 选定一个。热路径上的操作并不通过 `apic->eoi` 间接调用，而是用 static call：

[`arch/x86/include/asm/apic.h:402-415`](../../linux/arch/x86/include/asm/apic.h#L402-L415)（v6.18.60）

```c
static __always_inline u32 apic_read(u32 reg)
{
	return static_call(apic_call_read)(reg);
}

static __always_inline void apic_write(u32 reg, u32 val)
{
	static_call(apic_call_write)(reg, val);
}

static __always_inline void apic_eoi(void)
{
	static_call(apic_call_eoi)();
}
```

选定驱动后 `apic_setup_apic_calls()` 把这些 static call 修补为直接调用，避免间接跳转的开销以及 retpoline 的代价。在 KVM 客户机中，`kvm_guest_init()` 还会用 `apic_update_callback(eoi, kvm_guest_apic_eoi_write)`（[`arch/x86/kernel/kvm.c:861`](../../linux/arch/x86/kernel/kvm.c#L861)）替换 EOI 实现：

[`arch/x86/kernel/kvm.c:363-375`](../../linux/arch/x86/kernel/kvm.c#L363-L375)（v6.18.60）

```c
static notrace __maybe_unused void kvm_guest_apic_eoi_write(void)
{
	/**
	 * This relies on __test_and_clear_bit to modify the memory
	 * in a way that is atomic with respect to the local CPU.
	 * The hypervisor only accesses this memory from the local CPU so
	 * there's no need for lock or memory barriers.
	 * An optimization barrier is implied in apic write.
	 */
	if (__test_and_clear_bit(KVM_PV_EOI_BIT, this_cpu_ptr(&kvm_apic_eoi)))
		return;
	apic_native_eoi();
}
```

这就是 **PV EOI**：宿主机在注入中断时，若判断该中断的 EOI 不需要宿主机介入（例如边沿触发且没有更低优先级的挂起中断），就在共享内存中置一个标志位；客户机 EOI 时只需清除该位，省去一次 `wrmsr` 引起的 VM exit。

### LAPIC 初始化

每个 CPU 在启动时调用 `setup_local_APIC()`（BSP 在 `apic_intr_mode_init()` 中，AP 在 `apic_ap_setup()` 中）。其中与中断投递直接相关的几步：

[`arch/x86/kernel/apic/apic.c:1539-1594`](../../linux/arch/x86/kernel/apic/apic.c#L1539-L1594)（v6.18.60）

```c
	/*
	 * Set Task Priority to 'accept all except vectors 0-31'.  An APIC
	 * vector in the 16-31 range could be delivered if TPR == 0, but we
	 * would think it's an exception and terrible things will happen.  We
	 * never change this later on.
	 */
	value = apic_read(APIC_TASKPRI);
	value &= ~APIC_TPRI_MASK;
	value |= 0x10;
	apic_write(APIC_TASKPRI, value);

	apic_clear_isr();

	/*
	 * Now that we are all set up, enable the APIC
	 */
	value = apic_read(APIC_SPIV);
	value &= ~APIC_VECTOR_MASK;
	/*
	 * Enable APIC
	 */
	value |= APIC_SPIV_APIC_ENABLED;
	...
	/*
	 * Set spurious IRQ vector
	 */
	value |= SPURIOUS_APIC_VECTOR;
	apic_write(APIC_SPIV, value);
```

- **TPR 设为 `0x10`**：屏蔽优先级类 0、1（向量 0～31）。这些向量与 CPU 异常重叠，如果某个设备被误配置为投递这些向量，CPU 会把它当作异常处理。Linux 此后**不再修改 TPR**——不像某些操作系统那样用 TPR 实现中断优先级分级，所有设备中断地位相同。
- **`apic_clear_isr()`**：kexec / kdump 启动的新内核可能继承上一个内核未 EOI 的 ISR 位，这里把它们清掉，否则相应优先级的中断永远无法投递。
- **SPIV**：软件使能 LAPIC，并设置伪中断（Spurious Interrupt）向量为 `0xff`（5.1 节的 `SPURIOUS_APIC_VECTOR`）。伪中断发生在 LAPIC 准备投递一个中断、而 CPU 在应答周期中提高了 TPR 等竞态下，它不设置 ISR，也不需要 EOI。

随后设置**局部向量表**（Local Vector Table，LVT）。LVT 是 LAPIC 中一组寄存器，每个对应一个"本地中断源"，指定它的向量号、投递模式和屏蔽位：

| LVT 寄存器 | 偏移 | 中断源 | Linux 中的用途 |
|---|---|---|---|
| `APIC_LVTT` | `0x320` | LAPIC 定时器 | `LOCAL_TIMER_VECTOR`（`0xec`） |
| `APIC_LVTTHMR` | `0x330` | 温度传感器 | `THERMAL_APIC_VECTOR`（`0xfa`） |
| `APIC_LVTPC` | `0x340` | 性能计数器溢出 | 投递模式为 NMI（5.4 节 perf） |
| `APIC_LVT0` | `0x350` | LINT0 引脚 | 通常屏蔽；虚拟线模式下接 8259 PIC（ExtINT） |
| `APIC_LVT1` | `0x360` | LINT1 引脚 | 仅 BSP 接收外部 NMI |
| `APIC_LVTERR` | `0x370` | APIC 内部错误 | `ERROR_APIC_VECTOR`（`0xfe`） |
| `APIC_LVTCMCI` | `0x2f0` | 可纠正机器检查 | `THRESHOLD_APIC_VECTOR`（Intel CMCI） |

*表 5-8：LAPIC 局部向量表*

偏移取自 `arch/x86/include/asm/apicdef.h`。LINT0/LINT1 的配置见 [`arch/x86/kernel/apic/apic.c:1598-1631`](../../linux/arch/x86/kernel/apic/apic.c#L1598-L1631)：只有 BSP 在需要时把 LINT0 设为 ExtINT（兼容 8259 PIC 的"虚拟线模式"），LINT1 默认只在 BSP 上以 NMI 模式打开（可用 `apic_extnmi=` 启动参数修改）。

### LAPIC 定时器

LAPIC 定时器是每个 CPU 私有的时钟事件设备（clock event device），是 x86 上 tick 与高精度定时器的主要硬件来源（8.6 节）。它有两种编程方式：

[`arch/x86/kernel/apic/apic.c:410-431`](../../linux/arch/x86/kernel/apic/apic.c#L410-L431)（v6.18.60）

```c
/*
 * Program the next event, relative to now
 */
static int lapic_next_event(unsigned long delta,
			    struct clock_event_device *evt)
{
	apic_write(APIC_TMICT, delta);
	return 0;
}

static int lapic_next_deadline(unsigned long delta,
			       struct clock_event_device *evt)
{
	u64 tsc;

	/* This MSR is special and need a special fence: */
	weak_wrmsr_fence();

	tsc = rdtsc();
	wrmsrq(MSR_IA32_TSC_DEADLINE, tsc + (((u64) delta) * TSC_DIVISOR));
	return 0;
}
```

- **计数模式**：向初始计数寄存器 `APIC_TMICT` 写入一个值，LAPIC 按总线频率（经分频）递减，到 0 时触发中断。总线频率需要在启动时校准（`calibrate_APIC_clock()`）。
- **TSC-deadline 模式**：向 `MSR_IA32_TSC_DEADLINE` 写入一个 TSC 绝对值，TSC 到达时触发中断。无需校准，精度更高，是现代 CPU 的首选（`deadline_match[]` 列出了需要特定微码版本才能可靠使用该模式的 CPU，[`arch/x86/kernel/apic/apic.c:512`](../../linux/arch/x86/kernel/apic/apic.c#L512)）。

定时器中断经 `sysvec_apic_timer_interrupt()`（5.2 节）调用 `local_apic_timer_interrupt()`：

[`arch/x86/kernel/apic/apic.c:1013-1042`](../../linux/arch/x86/kernel/apic/apic.c#L1013-L1042)（v6.18.60）

```c
static void local_apic_timer_interrupt(void)
{
	struct clock_event_device *evt = this_cpu_ptr(&lapic_events);

	/*
	 * Normally we should not be here till LAPIC has been initialized but
	 * in some cases like kdump, its possible that there is a pending LAPIC
	 * timer interrupt from previous kernel's context and is delivered in
	 * new kernel the moment interrupts are enabled.
	 *
	 * Interrupts are enabled early and LAPIC is setup much later, hence
	 * its possible that when we get here evt->event_handler is NULL.
	 * Check for event_handler being NULL and discard the interrupt as
	 * spurious.
	 */
	if (!evt->event_handler) {
		pr_warn("Spurious LAPIC timer interrupt on cpu %d\n",
			smp_processor_id());
		/* Switch it off */
		lapic_timer_shutdown(evt);
		return;
	}

	/*
	 * the NMI deadlock-detector uses this.
	 */
	inc_irq_stat(apic_timer_irqs);

	evt->event_handler(evt);
}
```

`event_handler` 由时钟事件框架设置，高精度模式下为 `hrtimer_interrupt()`。`apic_timer_irqs` 计数即 `/proc/interrupts` 中 `LOC` 一行的数据。

### 发送 IPI

IPI 通过写 ICR 发出。物理 x2APIC 模式下，发送给单个 CPU 只需一次 `wrmsr`：

[`arch/x86/kernel/apic/x2apic_phys.c:44-73`](../../linux/arch/x86/kernel/apic/x2apic_phys.c#L44-L73)（v6.18.60）

```c
static void x2apic_send_IPI(int cpu, int vector)
{
	u32 dest = per_cpu(x86_cpu_to_apicid, cpu);

	/* x2apic MSRs are special and need a special fence: */
	weak_wrmsr_fence();
	__x2apic_send_IPI_dest(dest, vector, APIC_DEST_PHYSICAL);
}

static void
__x2apic_send_IPI_mask(const struct cpumask *mask, int vector, int apic_dest)
{
	unsigned long query_cpu;
	unsigned long this_cpu;
	unsigned long flags;

	/* x2apic MSRs are special and need a special fence: */
	weak_wrmsr_fence();

	local_irq_save(flags);

	this_cpu = smp_processor_id();
	for_each_cpu(query_cpu, mask) {
		if (apic_dest == APIC_DEST_ALLBUT && this_cpu == query_cpu)
			continue;
		__x2apic_send_IPI_dest(per_cpu(x86_cpu_to_apicid, query_cpu),
				       vector, APIC_DEST_PHYSICAL);
	}
	local_irq_restore(flags);
}
```

`weak_wrmsr_fence()` 值得一提：x2APIC 的 MSR 写入**不是序列化指令**，可能在之前的内存写入对其他 CPU 可见之前就把 IPI 发出去。如果发送方先写了一个数据结构再发 IPI，接收方在 IPI 处理函数中可能读到旧值。这个 fence（`mfence; lfence`）保证此前的内存写入先于 IPI 生效。xAPIC 的 MMIO 写入属于非缓存访问，天然有序，不需要它。

物理模式下多播 IPI 只能逐个发送；逻辑 cluster 模式（`x2apic_cluster.c`）可以用一次写入同时发给同一 cluster 中的最多 16 个 CPU。另有"简写"（shorthand）方式，`APIC_DEST_ALLBUT` 一次发给除自己外的所有 CPU，但要求所有 CPU 都已上线，否则可能把 IPI 发给正在启动或已离线的 CPU，由 `apic_use_ipi_shorthand` 静态键控制。IPI 的上层用法见 5.11 节。

## I/O APIC

### 重定向表

I/O APIC 有若干输入引脚（通常 24 个），每个引脚对应**重定向表**（Redirection Table）中的一项，内核用 `struct IO_APIC_route_entry` 描述：

[`arch/x86/include/asm/io_apic.h:58-88`](../../linux/arch/x86/include/asm/io_apic.h#L58-L88)（v6.18.60）

```c
struct IO_APIC_route_entry {
	union {
		struct {
			u64	vector			:  8,	// [注] 投递的向量号
				delivery_mode		:  3,	// [注] Fixed / LowPrio / SMI / NMI / INIT / ExtINT
				dest_mode_logical	:  1,
				delivery_status		:  1,
				active_low		:  1,	// [注] 极性
				irr			:  1,	// [注] Remote IRR：电平中断已投递待 EOI
				is_level		:  1,	// [注] 触发方式
				masked			:  1,
				reserved_0		: 15,
				reserved_1		: 17,
				virt_destid_8_14	:  7,
				destid_0_7		:  8;	// [注] 目标 APIC ID
		};
		struct {
			u64	ir_shared_0		:  8,
				ir_zero			:  3,
				ir_index_15		:  1,
				ir_shared_1		:  5,
				ir_reserved_0		: 31,
				ir_format		:  1,
				ir_index_0_14		: 15;	// [注] 中断重映射表索引
		};
		struct {
			u64	w1			: 32,
				w2			: 32;
		};
	};
} __attribute__ ((packed));
```

同一个 64 位表项有两种解释：不启用中断重映射时，直接包含向量号与目标 APIC ID；启用重映射时（`ir_format = 1`），只包含重映射表的索引，真正的向量与目标由 IOMMU 中的表项决定。

### 电平触发中断与 Remote IRR

ISA 设备通常是边沿触发，PCI INTx 则是电平触发、低电平有效且可共享。电平触发中断的难点在于：只要设备没有撤销中断请求，引脚就一直处于有效电平，I/O APIC 必须知道"这次中断已经处理完了"才能再次投递。这由重定向表项中的 **Remote IRR** 位实现：I/O APIC 投递一个电平中断后置位 Remote IRR，在此期间不再投递；CPU 处理完后需要向 I/O APIC 发送 EOI，清除 Remote IRR。

x86 上 LAPIC 的 EOI 对电平中断会自动广播给所有 I/O APIC（"EOI 广播"），但有例外，因此 `ioapic_chip` 的 `irq_eoi` 回调做了额外检查：

[`arch/x86/kernel/apic/io_apic.c:1699-1721`](../../linux/arch/x86/kernel/apic/io_apic.c#L1699-L1721)（v6.18.60）

```c
	i = cfg->vector;
	v = apic_read(APIC_TMR + ((i & ~0x1f) >> 1));

	/*
	 * We must acknowledge the irq before we move it or the acknowledge will
	 * not propagate properly.
	 */
	apic_eoi();

	/*
	 * Tail end of clearing remote IRR bit (either by delivering the EOI
	 * message via io-apic EOI register write or simulating it using
	 * mask+edge followed by unmask+level logic) manually when the
	 * level triggered interrupt is seen as the edge triggered interrupt
	 * at the cpu.
	 */
	if (!(v & (1 << (i & 0x1f)))) {
		atomic_inc(&irq_mis_count);
		eoi_ioapic_pin(cfg->vector, irq_data->chip_data);
	}

	ioapic_finish_move(irq_data, moveit);
}
```

先读 LAPIC 的 TMR，看该向量在 LAPIC 看来是否为电平触发。若不是——某些 82093AA I/O APIC 的勘误会把电平中断误作边沿投递，或者 CPU 下线时中断经 IPI 转发而失去了电平属性——LAPIC 的 EOI 就不会广播，Remote IRR 永远不会被清除，该引脚上的中断就此"卡死"。此时必须手动向 I/O APIC 发 EOI。注释（[`arch/x86/kernel/apic/io_apic.c:1667-1698`](../../linux/arch/x86/kernel/apic/io_apic.c#L1667-L1698)）详细记录了这段历史。`irq_mis_count` 即 `/proc/interrupts` 中的 `MIS` 计数。

手动 EOI 的方式取决于 I/O APIC 版本：

[`arch/x86/kernel/apic/io_apic.c:439-457`](../../linux/arch/x86/kernel/apic/io_apic.c#L439-L457)（v6.18.60）

```c
static void __eoi_ioapic_pin(int apic, int pin, int vector)
{
	if (mpc_ioapic_ver(apic) >= 0x20) {
		io_apic_eoi(apic, vector);
	} else {
		struct IO_APIC_route_entry entry, entry1;

		entry = entry1 = __ioapic_read_entry(apic, pin);

		/* Mask the entry and change the trigger mode to edge. */
		entry1.masked = true;
		entry1.is_level = false;

		__ioapic_write_entry(apic, pin, entry1);

		/* Restore the previous level triggered entry. */
		__ioapic_write_entry(apic, pin, entry);
	}
}
```

版本 `0x20` 及以上的 I/O APIC 有专门的 EOI 寄存器；更老的版本只能把表项临时改为"屏蔽 + 边沿触发"再改回来，利用硬件在切换触发模式时清除 Remote IRR 的副作用。

### ioapic_chip

I/O APIC 在通用中断层中表现为一个 `irq_chip`：

[`arch/x86/kernel/apic/io_apic.c:1854-1880`](../../linux/arch/x86/kernel/apic/io_apic.c#L1854-L1880)（v6.18.60）

```c
static struct irq_chip ioapic_chip __read_mostly = {
	.name			= "IO-APIC",
	.irq_startup		= startup_ioapic_irq,
	.irq_mask		= mask_ioapic_irq,
	.irq_unmask		= unmask_ioapic_irq,
	.irq_ack		= irq_chip_ack_parent,
	.irq_eoi		= ioapic_ack_level,
	.irq_set_affinity	= ioapic_set_affinity,
	.irq_retrigger		= irq_chip_retrigger_hierarchy,
	.irq_get_irqchip_state	= ioapic_irq_get_chip_state,
	.flags			= IRQCHIP_SKIP_SET_WAKE | IRQCHIP_MOVE_DEFERRED |
				  IRQCHIP_AFFINITY_PRE_STARTUP,
};

static struct irq_chip ioapic_ir_chip __read_mostly = {
	.name			= "IR-IO-APIC",
	.irq_startup		= startup_ioapic_irq,
	.irq_mask		= mask_ioapic_irq,
	.irq_unmask		= unmask_ioapic_irq,
	.irq_ack		= irq_chip_ack_parent,
	.irq_eoi		= ioapic_ir_ack_level,
	.irq_set_affinity	= ioapic_set_affinity,
	.irq_retrigger		= irq_chip_retrigger_hierarchy,
	.irq_get_irqchip_state	= ioapic_irq_get_chip_state,
	.flags			= IRQCHIP_SKIP_SET_WAKE |
				  IRQCHIP_AFFINITY_PRE_STARTUP,
};
```

几个回调体现了分层设计（5.6 节）：

- `irq_ack` 是 `irq_chip_ack_parent`——I/O APIC 自己没有"应答"操作，边沿中断的应答直接交给父级（向量域的 `apic_ack_edge()`，即 LAPIC EOI）。
- `irq_set_affinity` 先调用父级分配新 CPU 上的向量，成功后再重写重定向表项：

[`arch/x86/kernel/apic/io_apic.c:1786-1809`](../../linux/arch/x86/kernel/apic/io_apic.c#L1786-L1809)（v6.18.60）

```c
static void ioapic_configure_entry(struct irq_data *irqd)
{
	struct mp_chip_data *mpd = irqd->chip_data;
	struct irq_pin_list *entry;

	ioapic_setup_msg_from_msi(irqd, &mpd->entry);

	for_each_irq_pin(entry, mpd->irq_2_pin)
		__ioapic_write_entry(entry->apic, entry->pin, mpd->entry);
}

static int ioapic_set_affinity(struct irq_data *irq_data, const struct cpumask *mask, bool force)
{
	struct irq_data *parent = irq_data->parent_data;
	int ret;

	ret = parent->chip->irq_set_affinity(parent, mask, force);

	guard(raw_spinlock_irqsave)(&ioapic_lock);
	if (ret >= 0 && ret != IRQ_SET_MASK_OK_DONE)
		ioapic_configure_entry(irq_data);

	return ret;
}
```

`ioapic_setup_msg_from_msi()` 的名字道出了 v5.10 之后的设计观点（[`arch/x86/kernel/apic/io_apic.c:1737-1752`](../../linux/arch/x86/kernel/apic/io_apic.c#L1737-L1752) 的注释）：**I/O APIC 不过是一个把传统引脚转换为 MSI 消息的设备**。它请父级域"组合一条 MSI 消息"，再把消息中的向量、目标 ID 等字段搬到重定向表项中。这样无论父级是普通向量域还是中断重映射域，I/O APIC 驱动都不必关心具体格式。

## MSI 与 MSI-X

### 消息格式

MSI 让设备用一次普通的 PCIe 内存写事务来触发中断：写入的**地址**决定目标 CPU，写入的**数据**决定向量。x86 规定地址必须落在 `0xFEE00000`～`0xFEEFFFFF` 区间内，芯片组会把对这一区间的写入识别为中断消息：

[`arch/x86/include/asm/msi.h:14-60`](../../linux/arch/x86/include/asm/msi.h#L14-L60)（v6.18.60）

```c
typedef struct x86_msi_data {
	union {
		struct {
			u32	vector			:  8,
				delivery_mode		:  3,
				dest_mode_logical	:  1,
				reserved		:  2,
				active_low		:  1,
				is_level		:  1;
		};
		u32	dmar_subhandle;
	};
} __attribute__ ((packed)) arch_msi_msg_data_t;
#define arch_msi_msg_data	x86_msi_data

typedef struct x86_msi_addr_lo {
	union {
		struct {
			u32	reserved_0		:  2,
				dest_mode_logical	:  1,
				redirect_hint		:  1,
				reserved_1		:  1,
				virt_destid_8_14	:  7,
				destid_0_7		:  8,
				base_address		: 12;
		};
		struct {
			u32	dmar_reserved_0		:  2,
				dmar_index_15		:  1,
				dmar_subhandle_valid	:  1,
				dmar_format		:  1,
				dmar_index_0_14		: 15,
				dmar_base_address	: 12;
		};
	};
} __attribute__ ((packed)) arch_msi_msg_addr_lo_t;
#define arch_msi_msg_addr_lo	x86_msi_addr_lo

#define X86_MSI_BASE_ADDRESS_LOW	(0xfee00000 >> 20)

typedef struct x86_msi_addr_hi {
	u32	reserved		:  8,
		destid_8_31		: 24;
} __attribute__ ((packed)) arch_msi_msg_addr_hi_t;
#define arch_msi_msg_addr_hi	x86_msi_addr_hi

#define X86_MSI_BASE_ADDRESS_HIGH	(0)
```

与 I/O APIC 表项一样，地址也有"直接"和"重映射"两种格式。直接格式中目标 APIC ID 只有 8 位（`destid_0_7`），最多寻址 256 个 APIC ID；超过 255 个 CPU 的系统必须启用中断重映射，或依赖部分虚拟化平台支持的"扩展目标 ID"（`virt_destid_8_14`，再扩展 7 位）。`__irq_msi_compose_msg()` 据此生成消息：

[`arch/x86/kernel/apic/apic.c:2282-2310`](../../linux/arch/x86/kernel/apic/apic.c#L2282-L2310)（v6.18.60）

```c
void __irq_msi_compose_msg(struct irq_cfg *cfg, struct msi_msg *msg,
			   bool dmar)
{
	memset(msg, 0, sizeof(*msg));

	msg->arch_addr_lo.base_address = X86_MSI_BASE_ADDRESS_LOW;
	msg->arch_addr_lo.dest_mode_logical = apic->dest_mode_logical;
	msg->arch_addr_lo.destid_0_7 = cfg->dest_apicid & 0xFF;

	msg->arch_data.delivery_mode = APIC_DELIVERY_MODE_FIXED;
	msg->arch_data.vector = cfg->vector;

	msg->address_hi = X86_MSI_BASE_ADDRESS_HIGH;
	/*
	 * Only the IOMMU itself can use the trick of putting destination
	 * APIC ID into the high bits of the address. Anything else would
	 * just be writing to memory if it tried that, and needs IR to
	 * address APICs which can't be addressed in the normal 32-bit
	 * address range at 0xFFExxxxx. That is typically just 8 bits, but
	 * some hypervisors allow the extended destination ID field in bits
	 * 5-11 to be used, giving support for 15 bits of APIC IDs in total.
	 */
	if (dmar)
		msg->arch_addr_hi.destid_8_31 = cfg->dest_apicid >> 8;
	else if (virt_ext_dest_id && cfg->dest_apicid < 0x8000)
		msg->arch_addr_lo.virt_destid_8_14 = cfg->dest_apicid >> 8;
	else
		WARN_ON_ONCE(cfg->dest_apicid > 0xFF);
}
```

`struct irq_cfg` 是向量域为每个中断保存的"当前向量 + 目标 APIC ID"（5.8 节）。

MSI 与 MSI-X 的区别在设备一侧：MSI 每个设备最多 32 个向量，且必须是连续的 2 的幂个，共用一个地址；MSI-X 最多 2048 个条目，每个条目有独立的地址、数据和屏蔽位，存放在设备 BAR 映射的 MSI-X 表中。现代 NVMe 盘、多队列网卡几乎都用 MSI-X，每个硬件队列一个中断，可以分别绑定到不同 CPU（5.8 节的 managed interrupt）。

MSI 天然是边沿语义：一次写入就是一次中断，不存在 Remote IRR 的问题，也不会与其他设备共享——这是它取代 INTx 的主要原因之一。

### 修改 MSI 亲和性的难题

迁移一个 MSI 中断到另一个 CPU，需要同时修改地址（目标 CPU）和数据（新 CPU 上分配到的向量）。问题在于，对于**不支持逐向量屏蔽**的 MSI 设备，地址和数据是分两次写入设备配置空间的，设备可能恰好在两次写入之间发出中断，使用"新地址 + 旧向量"或"旧地址 + 新向量"的组合，把中断投递到错误的 CPU 或错误的向量上。

`msi_set_affinity()` 用一个巧妙的两步法解决这个问题：

[`arch/x86/kernel/apic/msi.c:99-140`](../../linux/arch/x86/kernel/apic/msi.c#L99-L140)（v6.18.60）

```c
	lock_vector_lock();

	/*
	 * Mark the new target vector on the local CPU if it is currently
	 * unused. Reuse the VECTOR_RETRIGGERED state which is also used in
	 * the CPU hotplug path for a similar purpose. This cannot be
	 * undone here as the current CPU has interrupts disabled and
	 * cannot handle the interrupt before the whole set_affinity()
	 * section is done. In the CPU unplug case, the current CPU is
	 * about to vanish and will not handle any interrupts anymore. The
	 * vector is cleaned up when the CPU comes online again.
	 */
	if (IS_ERR_OR_NULL(this_cpu_read(vector_irq[cfg->vector])))
		this_cpu_write(vector_irq[cfg->vector], VECTOR_RETRIGGERED);

	/* Redirect it to the new vector on the local CPU temporarily */
	old_cfg.vector = cfg->vector;
	irq_msi_update_msg(irqd, &old_cfg);

	/* Now transition it to the target CPU */
	irq_msi_update_msg(irqd, cfg);

	/*
	 * All interrupts after this point are now targeted at the new
	 * vector/CPU.
	 *
	 * Drop vector lock before testing whether the temporary assignment
	 * to the local CPU was hit by an interrupt raised in the device,
	 * because the retrigger function acquires vector lock again.
	 */
	unlock_vector_lock();

	/*
	 * Check whether the transition raced with a device interrupt and
	 * is pending in the local APICs IRR. It is safe to do this outside
	 * of vector lock as the irq_desc::lock of this interrupt is still
	 * held and interrupts are disabled: The check is not accessing the
	 * underlying vector store. It's just checking the local APIC's
	 * IRR.
	 */
	if (lapic_vector_set_in_irr(cfg->vector))
		irq_data_get_irq_chip(irqd)->irq_retrigger(irqd);
```

前提是这段代码运行在中断当前的目标 CPU 上（x86 对不可屏蔽 MSI 采用"在下一次中断时迁移"的延迟策略，5.8 节）。第一步只改向量、目标仍是本 CPU；第二步只改目标 CPU、向量已是新值。每一步都只改变一个字段，任何中间状态都是合法的组合。若设备恰好在第一步与第二步之间发出中断，它会被投递到本 CPU 的新向量上——此时本 CPU 关着中断，中断停在 LAPIC 的 IRR 中。最后检查 IRR，若有挂起就通过 `irq_retrigger` 在新目标 CPU 上补发一次。

当中断能被屏蔽（大多数 MSI-X 设备）或启用了中断重映射时，可以先屏蔽再一次性修改，走函数开头的快速路径（[`arch/x86/kernel/apic/msi.c:65-72`](../../linux/arch/x86/kernel/apic/msi.c#L65-L72)）。

## 中断重映射

启用 IOMMU 的中断重映射（`CONFIG_IRQ_REMAP=y`，Intel VT-d 或 AMD-Vi）后，设备写出的 MSI 消息和 I/O APIC 产生的消息不再直接包含向量和目标，而是一个**重映射表**（Interrupt Remapping Table，IRT）的索引。IOMMU 查表得到真正的向量与目标 APIC ID 后再投递。它带来三个好处：

- **安全**：设备（尤其是直通给虚拟机的设备）不能随意构造中断消息攻击宿主机，只能触发 IRT 中允许的条目；
- **扩展**：IRT 表项中的目标 ID 是 32 位，突破了 MSI 地址 8 位的限制，是 x2APIC 在大于 255 个 CPU 系统上工作的前提；
- **原子迁移**：修改亲和性只需改 IRT 表项，无需触碰设备，上面的两步法也就不必要了。

在通用中断层中，中断重映射表现为插在设备域（MSI / I/O APIC）与向量域之间的一层 `irq_domain`，这也是 `/proc/interrupts` 中芯片名带 `IR-` 前缀的来源。

## 关键细节 / 设计考量

### 8259 PIC 与虚拟线模式

x86 的中断控制器历史可以追溯到 IBM PC 的两片级联 8259A PIC，提供 IRQ 0～15。为了兼容，系统上电时处于"PIC 模式"或"虚拟线模式"（PIC 的输出经 LAPIC 的 LINT0 以 ExtINT 方式送入 BSP）。Linux 在启动中通过 `init_ISA_irqs()`（[`arch/x86/kernel/irqinit.c:54-73`](../../linux/arch/x86/kernel/irqinit.c#L54-L73)）初始化 PIC，再在 `setup_IO_APIC()` 中切换到对称 I/O 模式，此后 PIC 被屏蔽，ISA IRQ 改由 I/O APIC 的前 16 个引脚提供。`/proc/interrupts` 中 IRQ 0～15 仍对应 ISA 中断号，向量范围是 `0x30`～`0x3f`（`ISA_IRQ_VECTOR()`，5.1 节）——不过一旦由 I/O APIC 接管，它们也可以被动态重新分配向量。

### 为什么 Linux 不使用 TPR 做优先级

LAPIC 硬件提供了基于向量号的 16 级优先级，以及可编程的 TPR。一些操作系统（如 Windows 的 IRQL）用它实现"高优先级中断可以打断低优先级中断处理"。Linux 选择了更简单的模型：所有设备中断处理函数都在关中断下运行（5.2 节），不允许嵌套；时间敏感的工作推迟到软中断或线程中完成（5.7、5.9 节）。TPR 始终固定为 `0x10`，向量号的高低只影响多个中断同时挂起时的投递顺序。这一模型避免了中断优先级反转与复杂的嵌套栈管理，配合线程化中断，在实践中被证明足够。

## 动手实验

### 查看 APIC 模式与中断控制器

```bash
# 在 dev 虚拟机中执行
sudo dmesg | grep -iE 'x2apic|APIC: Switch|IOAPIC|IO-APIC|DMAR|AMD-Vi' | head
grep -c processor /proc/cpuinfo
head -1 /proc/interrupts; grep -E 'IO-APIC|PCI-MSI' /proc/interrupts | head
```

典型输出中可以看到 `APIC: Switched APIC routing to: physical x2apic`，以及 `/proc/interrupts` 第二列之后的芯片名：`IO-APIC 2-edge timer`、`IO-APIC 9-fasteoi acpi`、`PCI-MSIX-0000:00:03.0 0-edge virtio0-config` 等。`edge`/`fasteoi` 是 5.7 节将要分析的流控处理函数。

### 观察 LAPIC 定时器的工作模式

```bash
grep -o 'tsc_deadline_timer' /proc/cpuinfo | head -1
cat /sys/devices/system/clockevents/clockevent0/current_device
cat /proc/timer_list | grep -A3 'Clock Event Device: lapic'
```

若 CPU 支持 TSC-deadline，`/proc/timer_list` 中会显示 `lapic-deadline` 设备，`set_next_event` 为 `lapic_next_deadline`。

## 小结

- 每个 CPU 的 LAPIC 用 IRR / ISR 维护挂起与服务中的向量，CPU 处理完必须写 EOI，否则同一及更低优先级类的中断会被阻塞。
- Linux 用 `struct apic` 抽象 xAPIC、x2APIC 等模式，热路径（EOI、读写、发 IPI）通过 static call 直接调用；KVM 客户机用 PV EOI 省去大多数 EOI 的 VM exit。
- LAPIC 初始化固定 TPR 为 `0x10`、开启 SPIV 并设置伪中断向量，LVT 为定时器、性能计数器、LINT0/1、错误等本地源分配向量；LAPIC 定时器优先使用 TSC-deadline 模式。
- x2APIC 的 ICR 写入不是序列化指令，发送 IPI 前需要 `weak_wrmsr_fence()`，保证接收方能看到发送方此前的内存写入。
- I/O APIC 把引脚翻译为中断消息；电平中断依赖 Remote IRR 与 EOI 广播，内核为勘误与 CPU 下线场景提供手动 EOI；在 v6.18 中 I/O APIC 被建模为"把引脚转换为 MSI 消息的设备"。
- MSI 用写 `0xFEExxxxx` 地址的方式投递中断，地址携带目标、数据携带向量；不可屏蔽 MSI 的亲和性修改用"先改向量、再改目标、最后检查 IRR"的两步法避免丢失中断；中断重映射解决了安全、目标 ID 宽度与原子迁移问题。

## 参考

- Intel® 64 and IA-32 Architectures Software Developer's Manual, Vol. 3A, Chapter 11 "Advanced Programmable Interrupt Controller (APIC)"
- Intel® 82093AA I/O Advanced Programmable Interrupt Controller (IOAPIC) Datasheet
- PCI Express Base Specification, §6.1.4 "MSI and MSI-X Operation"
- Intel® Virtualization Technology for Directed I/O Architecture Specification, Chapter 5 "Interrupt Remapping"
- `linux/Documentation/core-api/irq/irq-domain.rst`
- `linux/Documentation/PCI/msi-howto.rst`
