# KernelMask

KernelMask 是 [KernelSU](https://github.com/tiann/KernelSU) 的下游分支，在原项目基础上进行了定制化修改。

## ⚠️ 测试版声明

本项目为**测试版本**，仅供学习研究使用。

- 不保证稳定性，可能导致设备无法启动
- 使用前请务必备份重要数据
- 因使用本项目造成的任何损失，作者不承担任何责任

## 功能特性

- 基于 KernelSU v3.3.0 二改
- LKM 多版本预编译支持：android14-6.1、android15-6.6、android16-6.12
- 路径隐藏（PathHide）：内核级隐藏指定路径
- 安全模式下自动禁用路径隐藏模块
- 包名：com.kernelmask

## 许可证

- 内核部分（`kernel/` 目录）：GPL-2.0
- 用户空间部分：GPL-3.0

本仓库代码在开发过程中使用了AI编程助手辅助生成。
