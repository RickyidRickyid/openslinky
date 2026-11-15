# OpenSlinky

> Minecraft 1.8.9 客户端的原生重写实现

将 [Slinky](https://slinky.gg)（Minecraft 1.8.9 客户端）逆向重写为可读、可维护的 C++ 工程。
产物是一个原生 DLL，通过 JVMTI 注入 JVM，在游戏进程内运行模块；另附一个把 DLL 内嵌在自身的单文件注入器。

| | |
|---|---|
| **产物** | `Slinky.exe`（注入器，内嵌 DLL）、`SlinkyNative.dll`（客户端本体） |
| **工具链** | MSVC 2022 BuildTools · LLVM clang-cl · CMake + Ninja |
| **目标** | JVM（Minecraft 1.8.9 / Forge），x86-64 |
| **语言** | C++17，界面渲染使用 NanoVG |

---

## 目录

- [特性](#特性)
- [快速开始](#快速开始)
- [快捷键](#快捷键)
- [配置](#配置)
- [从源码构建](#从源码构建)
- [项目结构](#项目结构)
- [已知限制](#已知限制)
- [文档](#文档)

---

## 特性

74 个模块，按五个分类组织：

| 分类 | 内容 |
|---|---|
| **Combat** | Kill Aura、Hit Select、Backtrack、Reach、Velocity、Criticals、Auto Clicker、Aim Assist、TPAura 等 |
| **Move** | Speed、Fly、Sprint、Safe Walk、Null Move、Fast Accel、Instant Stop 等 |
| **Visual** | ESP 系列（Bed / Player / Chest / Item / Block）、Chams、Name Tags、Indicators、Trajectories 等 |
| **Block** | Scaffold、Fast Place、Fast Mine、Ghost Hand、Clutch、Bridge Assist 等 |
| **Utility** | Chest Stealer、Inventory Manager、Timer、Blink、Ping Fix、Anti Bot 等 |

### 毛玻璃界面

ClickGUI 采用实时背景模糊的毛玻璃面板，参考 Apple HIG 与 Windows Fluent 的材质规范实现：

- **捕获**：每 3 帧从默认帧缓冲读回一次，1/4 降采样后做双遍 box 模糊（三角核，接近高斯）
- **色彩**：模糊后提升饱和度，避免画面趋于灰白
- **材质**：卡片暗玻璃配色 + 135° 渐变边缘光 + 三层递减环境投影
- **五档可调**：透明度、面板磨砂、色调、暗化强度、背景模糊半径
- **可完全关闭**：关闭时不仅不绘制，连背景捕获一并停止，零运行时开销

### 其他

- 模块设置持久化到 `%USERPROFILE%\.slinky\profiles\`，支持多套配置快速切换
- 中文 / 英文界面，中文界面内嵌字体子集
- Bed ESP 采用事件驱动的区块更新（`0x14` 世界变更事件），替代轮询扫描

---

## 快速开始

1. 解压发布包
2. 启动 Minecraft 1.8.9（Forge）
3. 双击 `Slinky.exe`

`SlinkyNative.dll` 已经内嵌在 `Slinky.exe` 中，无需单独放置。注入器会自动查找 `javaw.exe` 进程。

```
Slinky.exe              中文界面（默认）
Slinky.exe -en          英文界面
Slinky.exe --list       列出当前 Java 进程
Slinky.exe -n javaw     按进程名注入
Slinky.exe --help       查看全部参数
```

> 更新版本时必须使用新的 `Slinky.exe`。DLL 内嵌在 exe 里，只替换 DLL 不会生效。

---

## 快捷键

| 按键 | 功能 |
|---|---|
| `R Shift` | 打开 / 关闭 ClickGUI |

模块与配置的绑定键在界面内单独设置。

---

## 配置

配置文件位于 `%USERPROFILE%\.slinky\`：

```
.slinky/
├── settings                    全局设置（语言、GUI 外观）
├── profiles/                   配置存档，每个文件一套完整模块状态
│   ├── Default
│   └── <自定义名称>
└── bin/                        原版客户端二进制（可选，本工程不依赖）
```

切换配置：GUI → **Profiles** → 点击对应条目加载。

---

## 从源码构建

### 依赖

- Visual Studio 2022 BuildTools（提供 MSVC 头文件与库）
- LLVM / clang-cl
- CMake 3.20+、Ninja
- LLVM `llvm-rc`（编译注入器资源）

### 构建

```powershell
cmake -S . -B build -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-windows-clang-cl.cmake
cmake --build build
```

产物位于 `build/SlinkyNative.dll`。

### 注入器

注入器把 DLL 作为 RCDATA 资源 `200` 内嵌，因此每次重新构建 DLL 后都必须重新链接注入器：

```powershell
llvm-rc /fo injector.res injector/injector.rc
clang-cl /std:c++17 /O2 /MT injector/*.cpp injector.res /Fe:Slinky.exe
```

验证内嵌内容是否与构建产物一致的方式，以及构建、注入、调试过程中的常见问题，见 [DEVELOPMENT.md](DEVELOPMENT.md)。

---

## 项目结构

```
src/
├── data/             数据段（字符串表、字体、导入表）
├── engine/
│   ├── base/         引擎入口、帧钩子
│   ├── gui/          ClickGUI 渲染、布局、控件、毛玻璃
│   ├── jni/          JNI 原语与 ID 表、事件分发、模块管理
│   ├── math/         数学与设置存储
│   ├── modlib/       引擎层辅助（方块表、背包、好友等）
│   └── protected/    受保护区域（钩子表）
├── logic/
│   ├── combat/       战斗模块
│   ├── move/         移动模块
│   ├── visual/       视觉模块
│   ├── block/        建筑模块
│   ├── utility/      工具模块
│   └── movement/     移动模块（B4 移植）
├── platform/         平台层（NanoVG 绑定、OpenGL 槽位）
└── injector/         注入器
include/slinky/       公共头文件（JNI ID 表、模块 API）
third_party/          NanoVG、精简 CRT、GL 头
```

---

## 已知限制

| 项 | 说明 |
|---|---|
| **TPAura** | 纯客户端修改坐标，服务端会在下一帧拉回。这是模块原理，不是缺陷 |
| **Hit Select** | `cancel_rate_air = 1` 会 100% 取消对着空气的点击，`cancel_rate = 1` 对实体也按时机取消。两项都是设计行为，觉得干扰就调低 |
| **毛玻璃开销** | 实时捕获存在固定成本（约 4.6 ms / 次，每 3 帧一次）。低配机器建议关闭或降低捕获频率 |
| **原版设置面板** | 由原版引擎绘制的模块详情面板不受毛玻璃影响，其绘制流程在原版二进制中 |
| **平台** | 仅支持 Windows x64 + Minecraft 1.8.9 / Forge |

---

## 文档

| 文件 | 内容 |
|---|---|
| [DEVELOPMENT.md](DEVELOPMENT.md) | 构建细节、注入调试、事件表、踩坑记录、代码地图 |
| [毛玻璃参考-2026-11-14.md](毛玻璃参考-2026-11-14.md) | 毛玻璃材质规范的调研与参数对照 |

---

## 声明

本项目仅用于逆向工程学习与软件架构研究。使用前请确认符合你所处环境的规则与条款。作者不对使用后果承担责任。
