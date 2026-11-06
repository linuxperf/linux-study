# 0.2 实验环境与只读构建

> **本节源码**：[`Makefile`](../../linux/Makefile)、[`init/Kconfig`](../../linux/init/Kconfig)、[`fs/proc/version.c`](../../linux/fs/proc/version.c)、[`scripts/kconfig/Makefile`](../../linux/scripts/kconfig/Makefile)
> **关键函数**：`version_proc_show()`、`proc_version_init()`
> **前置知识**：[0.1 阅读方法与分析边界](01-overview.md)、Shell 基础、虚拟机基本操作

## 概述

能够编译内核、能够启动内核和正在运行目标内核，是三个不同状态。为了保护源码分析的基线，本书不在仓库 `linux/` 中执行构建，而是在虚拟机本地磁盘准备源码副本与独立构建目录（Out-of-tree Build）。构建结果和配置差异保存在仓库之外，源码引用仍指向仓库内的原始文件。本节先打通可重复的配置与编译流程，再说明运行身份检查和安装前的边界。

## 环境对象与配置基线

| 对象 | 路径 | 用途 |
|---|---|---|
| 仓库根目录 | 虚拟机中为 `/root/linux` | 包含书稿、环境文件与只读内核树 |
| 内核分析基线 | `/root/linux/linux` | 所有源码结论的依据，不写入任何文件 |
| 完整基线配置 | `/root/linux/linux/.config` | 拷贝后作为构建配置的起点 |
| 工作源码副本 | 本节约定为 `/var/tmp/linux-study-work/src` | 可清理、可构建；初始内容来自分析基线 |
| 构建输出 | `/var/tmp/linux-study-work/build` | 保存 `.config`、生成头文件、目标文件和索引数据库 |
| 实验记录 | `/var/tmp/linux-study-work/records` | 保存工具版本、配置差异、构建日志与运行身份 |

注意两个容易混淆的配置文件：

- 仓库根目录 [`.config`](../../.config) 只有两条证书相关记录，不是完整配置，不能替代 `linux/.config`。
- 完整配置中的 `CONFIG_SYSTEM_TRUSTED_KEYS` 和 `CONFIG_SYSTEM_REVOCATION_KEYS` 都为空字符串，见 [`linux/.config:12458`](../../linux/.config#L12458) 与 [`linux/.config:12466`](../../linux/.config#L12466)。这两个符号是字符串，不能把根目录中类似布尔开关的文字直接当作配置语义。

当前基线还有 `CONFIG_CC_IS_CLANG=y`、`CONFIG_LD_IS_LLD=y`、`CONFIG_DEBUG_INFO_DWARF5=y`、`CONFIG_GDB_SCRIPTS=y`，并记录了 Clang 21.1.8。它们描述配置生成时的工具与选项，不保证读者机器上的无版本后缀 `clang` 就是同一版本。每次重新配置后都应保存最终 `.config`。

## 主流程分析

```mermaid
flowchart LR
    A[仓库只读源码与完整配置] -->|复制| B[仓库外源码副本]
    A -->|复制配置| C[独立输出目录]
    B --> D[只清理副本]
    D --> E[olddefconfig]
    C --> E
    E --> F[检查配置差异]
    F --> G[编译与生成数据库]
    G --> H[记录构建产物]
    H --> I[可选：安装并启动]
    I --> J[重新核验运行身份]
```

*图 0-2：只读分析基线与可写实验工作区的分离*

### 为什么仅添加 `O=` 还不够

内核构建系统（Kernel Build System，Kbuild）通过 `O=` 指定构建输出目录，处理入口见 [`Makefile:170-181`](../../linux/Makefile#L170-L181)。但独立输出构建会先检查源码树是否干净：源码目录中若存在 `.config`、`include/config` 或架构生成头文件目录，就会拒绝继续，见 [`Makefile:695-705`](../../linux/Makefile#L695-L705)。本仓库已经有 `linux/.config`，因此不能直接对原树加 `O=` 后期待构建成功。

错误提示通常建议执行 `make mrproper`。**不要在仓库 `linux/` 中照做**：它会删除配置和生成文件，违反只读约定。本节只在新建副本上执行清理，而且先把基线配置保存到输出目录。

### `LLVM=1` 的含义

`LLVM=1` 不只是选择 C 编译器，还选择 `ld.lld`、`llvm-ar`、`llvm-nm`、`llvm-objcopy` 等配套工具，见 [`Makefile:520-528`](../../linux/Makefile#L520-L528)。配置、编译和数据库生成阶段应保持同样的工具选择。使用带版本后缀的发行版工具时，可以统一使用 `LLVM=-21`，但必须先确认相应命令都已安装。

`olddefconfig` 根据现有配置和依赖关系补齐配置，新符号使用默认值，帮助文本见 [`scripts/kconfig/Makefile:153-154`](../../linux/scripts/kconfig/Makefile#L153-L154)。它不是保证配置字节不变的操作；换了编译器或依赖工具后，应使用 `scripts/diffconfig` 检查差异。

## 关键细节 / 设计考量

### 虚拟机声明不等于环境已经可用

[`dev/Vagrantfile:24-25`](../../dev/Vagrantfile#L24-L25) 固定了 Ubuntu 26.04 box 及版本，使用 VirtualBox，声明 8 GB 内存、4 个 vCPU 和 100 GB 磁盘；仓库共享到 `/root/linux`。这些是环境意图，不是已验证的运行状态。box 下载可用性、宿主架构、VirtualBox 版本和 Guest Additions 兼容性都要实际检查；尤其不能假定 ARM 宿主能直接运行这套 x86_64 环境。

配置还启用了固定 root 密码和 root SSH 密码登录，见 [`dev/Vagrantfile:83-89`](../../dev/Vagrantfile#L83-L89)。它只适合隔离的学习环境，不应暴露到不可信网络。优先使用 `vagrant ssh`，启动后修改默认密码；共享目录意味着虚拟机内 root 也可能改动宿主仓库，不能把虚拟机快照当成共享文件的备份。

### 不以缩减配置代替基线复现

本书基线启用了大量功能与调试信息，完整编译可能消耗较多内存、磁盘和时间。4 个 vCPU 不意味着必须使用高并行度；本节从 `-j2` 起步，出现内存不足时先减少并行度并检查日志。

如果为工具练习另建精简配置，应使用不同输出目录并保存差异。不要静默运行 `localmodconfig` 后继续声称复现了本书配置；它可能去掉后续章节所需但本机尚未加载的驱动或机制。

## 可观测性

环境准备阶段最重要的观测是“构建了什么”和“运行了什么”，而不是性能指标。

| 检查入口 | 看什么 | 对应实现与边界 |
|---|---|---|
| 基线 `Makefile` / 构建目标 `kernelversion` | 源码基础版本 | [`Makefile:2-5`](../../linux/Makefile#L2-L5)、[`Makefile:2171-2172`](../../linux/Makefile#L2171-L2172)；不证明运行版本 |
| 输出目录 `.config`、`include/config/kernel.release` | 最终配置与构建 release | 构建 release 可包含本地后缀，应与构建日志一起保存 |
| `uname -r`、`/proc/version` | 当前运行的 release、版本与编译信息 | `/proc/version` 由 `version_proc_show()` 生成并由 `proc_version_init()` 注册，见 [`fs/proc/version.c:10-27`](../../linux/fs/proc/version.c#L10-L27)；依赖 `CONFIG_PROC_FS=y` |
| `/proc/config.gz` | 可选的运行内核内嵌配置 | 依赖 `CONFIG_IKCONFIG` 和 `CONFIG_IKCONFIG_PROC`，定义见 [`init/Kconfig:753-770`](../../linux/init/Kconfig#L753-L770)；本书基线 `CONFIG_IKCONFIG=n`，不能默认存在 |
| `/boot/config-$(uname -r)` | 发行版或安装流程保存的配置副本 | 不是内核自动导出的接口；可能不存在，需核对它与实际启动镜像的关系 |

`/proc/version` 的格式还包含构建用户、主机与编译器，见 [`init/version.c:35-38`](../../linux/init/version.c#L35-L38)。相同的 release 字符串不保证二进制或配置相同；复现实验还应保留源码来源、最终配置与产物校验值。

## 动手实验

以下命令是操作步骤，不是内核源码摘录，尚未在 dev 虚拟机实测。除宿主机启动步骤外，都在虚拟机内同一个 Bash 会话执行。操作前先备份宿主仓库；后续安装新内核前再创建虚拟机快照。

### 1. 启动环境并确认架构

在宿主机仓库根目录执行；宿主机需要可用的 Vagrant 和 VirtualBox：

```bash
vagrant plugin install vagrant-vbguest
cd dev
vagrant up --provider=virtualbox
vagrant ssh
```

进入虚拟机后执行；这里核验的仍可能只是发行版内核：

```bash
sudo -i
passwd
uname -m
uname -r
cat /proc/version
findmnt /root/linux
df -h /var/tmp
free -h
```

只有 `uname -m` 为 `x86_64` 才继续本节的本机构建流程。若共享目录不存在，先处理挂载和插件问题，不要在同名空目录里继续操作。

### 2. 补充用户态依赖

Vagrant provisioner 已安装部分依赖，本节显式补齐常用工具；包可用性以实际发行版仓库为准：

```bash
apt-get update
apt-get install -y build-essential clang clangd llvm lld \
    flex bison bc libncurses-dev libelf-dev libssl-dev libdw-dev \
    dwarves python3 rsync git ripgrep pkg-config openssl kmod cpio zstd xz-utils
clang --version
ld.lld --version
llvm-ar --version
clangd --version
```

安装 `dwarves` 不代表自动启用了所有类型信息选项，仍以重新生成的配置为准。依赖版本要求见 [`Documentation/process/changes.rst`](../../linux/Documentation/process/changes.rst)。

### 3. 建立一次性工作区

以下脚本要求工作目录尚不存在，以免清理已有副本或覆盖历史记录；不要把 `SRC` 指向仓库原树：

```bash
set -euo pipefail
REPO=/root/linux
WORK=/var/tmp/linux-study-work
SRC=$WORK/src
OUT=$WORK/build
REC=$WORK/records
test -f "$REPO/linux/Makefile"
test -f "$REPO/linux/.config"
test ! -e "$WORK"
mkdir -p "$SRC" "$OUT" "$REC"
rsync -a --exclude='.git' "$REPO/linux/" "$SRC/"
cp "$REPO/linux/.config" "$OUT/.config"
cp "$OUT/.config" "$REC/baseline.config"
make -C "$SRC" LLVM=1 mrproper
make -C "$SRC" O="$OUT" LLVM=1 olddefconfig
python3 "$SRC/scripts/diffconfig" \
    "$REC/baseline.config" "$OUT/.config" | tee "$REC/config.diff"
```

应确认 `mrproper` 的工作目录是 `$SRC`，而不是 `$REPO/linux`。如差异涉及本书使用的功能，先解释差异再编译。证书路径报错时先检查配置来源，不要通过删除仓库文件绕过。

还有一个安装前必须处理的配置：基线为 `CONFIG_MODULE_SIG=y`、`CONFIG_MODULE_SIG_ALL=y`，但 `CONFIG_MODULE_SIG_KEY` 是空字符串。模块安装阶段会调用签名工具，见 [`scripts/Makefile.modinst:98-116`](../../linux/scripts/Makefile.modinst#L98-L116)；空的签名密钥不能直接视为“自动生成密钥”。自动生成只对默认路径 `certs/signing_key.pem` 生效，见 [`certs/Kconfig:6-17`](../../linux/certs/Kconfig#L6-L17) 和 [`certs/Makefile:43-64`](../../linux/certs/Makefile#L43-L64)。

若仅构建和阅读，可以保留基线；若准备安装本次构建的模块，可在编译前显式选择默认模块签名密钥路径，并记录这一实验配置变化：

```bash
"$SRC/scripts/config" --file "$OUT/.config" \
    --set-str MODULE_SIG_KEY 'certs/signing_key.pem'
make -C "$SRC" O="$OUT" LLVM=1 olddefconfig
python3 "$SRC/scripts/diffconfig" \
    "$REC/baseline.config" "$OUT/.config" | tee "$REC/config.diff"
```

生成的私钥留在构建输出目录，不能提交到仓库或写入书稿。这是模块签名准备，不会自动让固件信任自编译的启动镜像；Secure Boot 的启动信任仍需另外处理。

### 4. 构建并生成编译数据库

编译数据库（Compilation Database）记录源文件对应的真实编译命令，供后续语义导航使用。保持上一步变量不变；`pipefail` 用于让构建失败不被 `tee` 的成功状态掩盖：

```bash
{
    uname -a
    clang --version
    ld.lld --version
    llvm-ar --version
    clangd --version
} > "$REC/environment.txt"
make -C "$SRC" O="$OUT" LLVM=1 -j2 2>&1 | tee "$REC/build.log"
make -C "$SRC" O="$OUT" LLVM=1 -j2 compile_commands.json \
    2>&1 | tee "$REC/compile-commands.log"
cp "$OUT/.config" "$REC/final.config"
cp "$OUT/include/config/kernel.release" "$REC/kernel.release"
sha256sum "$OUT/vmlinux" "$OUT/arch/x86/boot/bzImage" \
    > "$REC/images.sha256"
```

x86 默认构建目标为 `bzImage`，并依赖 `vmlinux`，见 [`arch/x86/Makefile:305-313`](../../linux/arch/x86/Makefile#L305-L313)。前者用于启动，后者用于符号、反汇编和调试，不能混用。`compile_commands.json` 的作用见 [0.3](03-source-navigation.md)；生成数据库并不是启动目标内核。

### 5. 安装前检查与运行后核验

本节不提供自动安装、修改引导菜单或重启的一键脚本。只有在下列条件全部满足后，才按照虚拟机发行版的安装流程处理构建产物：

1. 已创建虚拟机快照，并能从控制台选择旧内核启动。
2. 已确认启动所需的存储、根文件系统和虚拟设备驱动可用；若它们是模块，已准备匹配的模块和初始镜像。
3. 已确认 Secure Boot、签名策略和内核 lockdown 对启动及调试的影响。
4. 构建 release 不会覆盖需要保留的已有内核与模块目录；需要本地后缀时，应在副本的输出配置中设置并重新构建、记录差异。
5. 明确发行版安装工具如何安装镜像、模块，生成初始镜像并更新引导菜单，不能假定单个 `make install` 完成所有步骤。

实际启动后重新记录 `uname -r` 和 `/proc/version`，并核对构建记录。若无法确认运行身份，后续实验只能作为工具练习。安装注意事项参考 [`Documentation/admin-guide/quickly-build-trimmed-linux.rst`](../../linux/Documentation/admin-guide/quickly-build-trimmed-linux.rst)，不要把其中直接修改源码树的示例照搬到本仓库原树。

## 小结

- 完整基线来自 `linux/.config`，不是仓库根目录 `.config`。
- 源码副本与构建输出都放在仓库之外，清理只作用于副本。
- `O=` 分离产物，但不能绕过源码树的干净状态检查。
- 配置、编译与索引统一选择 LLVM 工具，保存实际版本与配置差异。
- 编译成功不等于目标内核已经运行；安装与重启前必须准备回退路径。

## 参考

- [`Documentation/kbuild/llvm.rst`](../../linux/Documentation/kbuild/llvm.rst)：LLVM 工具链与版本选择。
- [`Documentation/kbuild/kbuild.rst`](../../linux/Documentation/kbuild/kbuild.rst)：构建输出和安装变量。
- [`Documentation/kbuild/kconfig.rst`](../../linux/Documentation/kbuild/kconfig.rst)：配置目标与差异检查。
- [`dev/Vagrantfile`](../../dev/Vagrantfile)：本仓库实验环境声明。
