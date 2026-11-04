# Linux 内核源码分析（v6.18.60 · x86_64）

本书以 Linux v6.18.60 源码为准，在 x86_64 架构、LLVM/Clang 工具链和仓库自带的 `.config` 配置下，逐个子系统分析内核的设计与实现。

## 读者对象

具备 C 语言与操作系统基础、初次深入阅读内核源码的工程师。

## 阅读指南

- 目录见 [SUMMARY.md](SUMMARY.md)，术语译法见 [glossary.md](glossary.md)。
- 代码块均标注 `文件路径:起始行-结束行`，路径相对于内核源码根目录，可直接在 `linux/` 中跳转核对。
- 依赖配置项的行为会注明 `CONFIG_XXX`，默认以 `linux/.config` 的取值为准。
- 实验环境见仓库 `dev/Vagrantfile`，实验命令会注明运行环境与权限要求。
