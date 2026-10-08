**靠两项信息判断：本 CPU 的“tick 已停止”标志，以及当前时间是否跨过了尚未累计的 tick 边界。**

这两个判断分别回答不同的问题。

## 1. 周期 tick 是否被停止？看状态标志

NO_HZ 是内核主动省略不需要的调度 tick。[本地文档说明](../../linux/Documentation/timers/no_hz.rst#L12-L24)

决定停止时，`tick_nohz_stop_tick()` 会设置：

```c
tick_sched_flag_set(ts, TS_FLAG_STOPPED);
```

在高精度模式下，它同时调整或取消产生调度 tick 的 `sched_timer`（源码见 [tick_nohz_stop_tick() 的高精度分支](../../linux/kernel/time/tick-sched.c#L1057-L1070)）。

因此：

```text
TS_FLAG_STOPPED 对应位 = 1（该标志定义为 BIT(1)，即 ts->flags 中此位被置位）
    → 本 CPU 已停止正常的周期 tick 重复安排
```

源码：[停止 tick 的代码](../../linux/kernel/time/tick-sched.c#L1041-L1046)；标志定义见 [tick-sched.h](../../linux/kernel/time/tick-sched.h#L20)。

注意，这个标志表示**停止状态**，不表示已经漏过几个 tick。例如刚停止 1 μs 就被唤醒，可能尚未跨过任何 tick 边界。

## 2. 跨过几个周期、需要补多少？比较时间

中断进入时，`irq_enter_rcu()` 只在当前 CPU 是 nohz_full CPU，或中断打断了 idle 任务时调用 `tick_irq_enter()`（源码见 [irq_enter_rcu()](../../linux/kernel/softirq.c#L666-L668)）。进入之后，若发现 tick 已停止，就检查是否需要更新时间：

```c
/* 简化代码：源码中此判断前还有提前返回与 TS_FLAG_IDLE_ACTIVE 处理 */
if (tick_sched_flag_test(ts, TS_FLAG_STOPPED))
    tick_nohz_update_jiffies(now);
```

源码：[tick_nohz_irq_enter()](../../linux/kernel/time/tick-sched.c#L1519-L1537)。

随后比较：

```text
now                 当前时间
tick_next_period    下一个尚未累计的全局 tick 边界
```

计算规则是：

```c
/* 简化逻辑 */
if (now < tick_next_period)
    ticks = 0;
else
    ticks = 1 + (now - tick_next_period) / TICK_NSEC;
```

源码：[tick_do_update_jiffies64()](../../linux/kernel/time/tick-sched.c#L96-L118)（函数定义从第 57 行开始，上面的计算逻辑对应第 96–118 行）。

**这里算的是需要补进的 jiffies 数，不是统计硬件少产生了几次中断。**

## 3. 用 23 ms 的例子串起来

假设 tick 周期为 10 ms：

```text
10 ms：完成时间更新
       下一累计边界为 20 ms

10 ms 后：
       停止周期 tick
       设置 TS_FLAG_STOPPED

20 ms：没有调度 tick 中断

23 ms：普通 hrtimer 中断到来
       │
       ├─ 看状态：TS_FLAG_STOPPED 已置位
       │           → 需要检查时间是否落后
       │
       └─ 比时间：23 ms 已跨过 20 ms
                   → 补进 1 个 jiffy
                   → 下一累计边界改为 30 ms
```

这里假设其他 CPU 尚未完成这次全局更新。

如果其他 CPU 已经把全局时间更新到 20 ms，那么下一边界已经是 30 ms。本 CPU 在 23 ms 检查时，**补进数量就是 0**。

所以必须分清：

| 信息                             | 范围与含义                            |
| -------------------------------- | ------------------------------------- |
| `TS_FLAG_STOPPED`                | **每 CPU 状态**：自己的周期 tick 停了 |
| `tick_next_period`、`jiffies_64` | **全局状态**：系统时间累计到哪里了    |

## 4. 恢复后怎样处理？

恢复周期 tick 时，内核先检查并补齐时间，再清除：

```c
tick_sched_flag_clear(ts, TS_FLAG_STOPPED);
```

然后重新启动周期性的 `sched_timer`。

源码：[tick_nohz_restart_sched_tick()](../../linux/kernel/time/tick-sched.c#L1088-L1104)；重新启动 `sched_timer` 的细节见 [tick_nohz_restart()](../../linux/kernel/time/tick-sched.c#L837-L850)。

**一句话：用标志知道“我停过 tick”，用 clocksource 提供的当前时间知道“全局还欠多少个 jiffy”。**