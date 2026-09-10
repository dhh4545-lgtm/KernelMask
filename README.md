# KernelMask

KernelMask 是基于 [KernelSU](https://github.com/tiann/KernelSU) 的二改分支，一个基于内核的 Android root 解决方案。

> 本项目是 KernelSU 的下游分支，在原项目基础上新增了路径隐藏功能，并在 LKM 模式下提供了三个 KMI 版本的预编译内核模块及自动适配。

## 主要特性

### 来自 KernelSU 的核心功能

- **基于内核的 root**：通过加载内核模块（LKM）实现 root，不修改系统分区
- **模块机制框架**：提供模块安装、启动脚本执行、SELinux 策略补丁等基础设施，模块格式与 Magisk 兼容
- **元模块架构**：自 v3.0 起，/system 等分区的具体挂载逻辑由元模块（metamodule）负责，核心本身不做挂载；用户需自行安装 meta-overlayfs、hybrid_mount 等元模块才能实现系统less 修改
- **SELinux 策略补丁**：在内核层面修补 SELinux 策略，支持模块的 sepolicy.rule
- **SU 兼容性**：提供与 Magisk 兼容的 su 接口，支持大多数 root 应用

### KernelMask 新增功能

- **LKM 多版本预编译**：在 LKM 模式下预编译了 android14-6.1、android15-6.6、android16-6.12 三个 KMI 版本的内核模块，ksud 运行时自动检测设备 KMI 并选择对应预编译模块（无需用户手动选择）
- **路径隐藏（PathHide）**：内核级路径隐藏功能，可隐藏指定路径，防止应用检测 root 相关文件
  - 部署后模块文件名为 `KernelMask.ko`
  - 配置文件位于 `/data/adb/ksu/pathhide.txt`，每行一个绝对路径
  - 通过 `ksud path-hide` 命令管理（deploy/load/unload/reload/status/get-config/set-config）
- **UI 精简**：移除主页"支持开发"和"了解 KernelSU"卡片，界面更简洁
- **包名与应用名**：包名 `com.kernelmask`，应用名 `KernelMask`

## 支持的内核版本

| KMI | 内核版本 | Android 版本 |
|-----|----------|--------------|
| android14-6.1 | 6.1 | Android 14 |
| android15-6.6 | 6.6 | Android 15 |
| android16-6.12 | 6.12 | Android 16 |

## 构建

### 环境要求

- Android NDK r29+
- Rust 工具链（带 aarch64-linux-android target）
- JDK 21
- Git LFS

### 编译内核模块

```bash
cd kernel
# 设置 KDIR 为对应内核版本的 DDK kdir 路径
KDIR=/path/to/ddk/kdir/android15-6.6 \
PATH=/path/to/clang/bin:$PATH \
ARCH=arm64 LLVM=1 LLVM_IAS=1 \
make CONFIG_KSU=m CC=clang \
KSU_MANAGER_PACKAGE=com.kernelmask
```

### 编译 ksud

```bash
cd userspace/ksud
cargo build --release --target aarch64-linux-android
# 编译产物复制到 manager/app/src/main/jniLibs/arm64-v8a/libksud.so
```

### 编译 APK

```bash
cd manager
JAVA_HOME=/path/to/jdk21 ./gradlew assembleRelease
# 产物位于 manager/app/build/outputs/apk/release/
```

## 安装

1. 解锁 Bootloader
2. 提取 boot.img（或 init_boot.img）
3. 使用 KernelMask 应用修补 boot 镜像
4. 刷入修补后的镜像
5. 重启设备

## 致谢

- [KernelSU](https://github.com/tiann/KernelSU) — 本项目基于 KernelSU v3.3.0 二改，感谢原项目作者及所有贡献者
- [ylarod/ddk](https://github.com/ylarod/ddk) — Android 内核驱动开发工具包，用于多 KMI 版本编译

## 许可证

本项目基于 GNU General Public License v3.0 开源，详见 [LICENSE](LICENSE)。

本项目作为 KernelSU 的下游分支，保留原项目的 GPL-3.0 许可证。所有修改部分同样以 GPL-3.0 许可证发布。

## 开发说明

本仓库代码在开发过程中使用了AI编程助手辅助生成。

## 免责声明

本项目仅供学习和研究使用。使用本项目可能导致设备失去保修、无法正常启动或数据丢失。请在充分了解风险的前提下使用，作者不对使用本项目造成的任何损失负责。
