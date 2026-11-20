# OpenSlinky

把 **Slinky**（Minecraft 1.8.9 客户端）逆向重写成可读 C++ 的工程。
产物是一个原生 DLL，经 **JVMTI** 注入 JVM，在游戏进程内挂接模块（战斗 / 移动 / 视觉 / 工具），
外加一个把 DLL 内嵌在自身的单文件注入器。

> **给接手的人**：这份文档是入口，覆盖构建、注入、验证与各模块定位。
> 读完这一份就能构建、验证、定位任何模块。

| | |
|---|---|
| **产物** | `Slinky.exe`（注入器，内嵌 DLL）+ `SlinkyNative.dll`（客户端本体） |
| **工具链** | MSVC 2022 BuildTools · LLVM clang-cl · CMake + Ninja |
| **目标** | JVM（Minecraft 1.8.9 / Forge），x86-64 |
| **接口** | JVMTI agent 注入 · JNI 调用游戏 · NanoVG 绘制界面 · OpenGL 直绘 |
| **基线构建** | DLL `40657B428BCA7A82` · EXE `4B16B0C86AFF2FFF`（2026-11-14，含材质层与毛玻璃） |

---

## 1. 目录结构

```
CMakeLists.txt              构建入口：clang-cl + Ninja，MSVC 头与库
build.ps1 / build.bat       一键构建（已含环境变量设置）
cmake/                      toolchain-windows-clang-cl.cmake、导入库 .def
include/slinky/             engine.h（Ctx / Hook / FN 宏）、jids.h、gui_state.h、gl_slots.h
src/
  engine/                   引擎层
    base/                   java_natives_*.cpp —— 每个 JNI native 的实现与事件派发
    jni/                    模块管理、hook_tables.inc（钩子表）、JNI 访问器、window_click
    gui/                    NanoVG 界面：布局、卡片、状态机、GL 槽绑定
    math/ protected/        向量、受保护钩子
  logic/                    模块逻辑，一个模块一个 .cpp + .h
    combat/ move/ visual/ utility/
  data/                     原镜像的 .rdata / .data / 字体 / .s 汇编
  platform/                 GL 槽加载、NanoVG 名称绑定、crt 与数学导出
injector/                   注入器：main / tui / inject_windows
third_party/                nanovg、GL 头、minicrt
```

**分层约定**：`engine/` 只做「把游戏的能力暴露出来」，`logic/` 只做「模块行为」。
`logic/` 里的每个 `FN(...)` 都是对 `engine/` 或 `platform/` 中某个已存在函数的重新声明，
**返回类型必须与真实定义逐字一致**（这条踩过一次崩端，见 §7）。

---

## 2. 构建

```powershell
$msvc = 'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Tools\MSVC\14.44.35207'
$sdk  = 'C:\Program Files (x86)\Windows Kits\10'; $ver = '10.0.26100.0'
$env:INCLUDE = "$msvc\include;$sdk\Include\$ver\ucrt;$sdk\Include\$ver\um;$sdk\Include\$ver\shared"
$env:LIB     = "$msvc\lib\x64;$sdk\Lib\$ver\ucrt\x64;$sdk\Lib\$ver\um\x64"
$env:PATH    = "C:\Program Files\LLVM\bin;" + $env:PATH

# 首次配置
cmake -S . -B _build_final -G Ninja -DCMAKE_BUILD_TYPE=Release `
  "-DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-windows-clang-cl.cmake"

# 增量构建
cmake --build _build_final
```

环境变量不是可选项：不设 `INCLUDE` / `LIB`，链接阶段找不到 ucrt。

**加载自检**（不启动游戏就能确认 DLL 可加载）：

```powershell
loadtest.exe _build_final\SlinkyNative.dll      # 期望输出 OK
```

`loadtest.exe` 只做 `LoadLibrary` + `DllMain`；随后打印的 `Initialization failed: 1000`
是它在没有 JVM 的环境下走初始化路径的正常结果，不代表损坏。

---

## 3. 注入器

`Slinky.exe` 把 DLL 以 **RCDATA 200** 内嵌在自己身上，运行时解压到 `%TEMP%\slinky-<pid>\`
再用 JVMTI attach。**所以改完 DLL 必须重新链接 exe**，否则游戏里跑的还是旧的那份。

```powershell
Slinky.exe          英文界面
Slinky.exe -cn      中文界面（苹方字体）
Slinky.exe -dev     开发者模式
```

重链三步（全部验证过）：

```powershell
# 1) 资源：路径指向刚构建出的 DLL
llvm-rc /fo injector.res /I "<sdk>\Include\<ver>\um" /I "<sdk>\Include\<ver>\shared" `
        /I "<sdk>\Include\<ver>\ucrt" /I "<msvc>\include" injector.rc

# 2) 链接
clang-cl /std:c++17 /EHsc /O2 /MT /nologo /utf-8 /D_WIN32_WINNT=0x0601 `
  main.cpp tui.cpp inject_windows.cpp injector.res /Fe:Slinky.exe `
  /link /SUBSYSTEM:CONSOLE `
  /LIBPATH:"<msvc>\lib\x64" /LIBPATH:"<sdk>\Lib\<ver>\ucrt\x64" /LIBPATH:"<sdk>\Lib\<ver>\um\x64" `
  psapi.lib user32.lib advapi32.lib

# 3) 校验内嵌的是什么（不要相信文件名和目录）
```

**三种「改了没生效」**，按踩到的顺序排列 —— 这三种都真实发生过，且症状完全一样：

1. 只重编 DLL、没重链 exe：游戏里是旧的 DLL。
2. 重链了 exe，但跑的是**另一个目录里的旧 exe**（发布包同时存在 `.zip` 和同名解压目录时最容易）。
3. 游戏进程没重启，旧的 DLL 还映射在进程里，新的一次注入并不替换它。

统一的三步排查：

```powershell
# a) 进程什么时候起的（没变过就是没重启）
Get-Process javaw | Select-Object Id, StartTime

# b) 这次注入落在哪份文件上
Get-ChildItem $env:TEMP -Filter 'slinky-*' -Directory |
  Sort-Object LastWriteTime -Descending | Select-Object -First 3

# c) 它的 SHA 前 16 位 vs. 构建产物
Get-FileHash _build_final\SlinkyNative.dll -Algorithm SHA256
```

运行期日志：`%TEMP%\slinky.log`（追加写，不轮转 —— 排查前先删掉，否则旧记录会混进来）。
配置：`%USERPROFILE%\.slinky\profiles\<名字>`，扁平 JSON，`{"<模块名>": {"__on": bool, "__bind": int, ...}}`。

---

## 4. 模块系统

### 4.1 描述符（0x210 字节，引擎按偏移直接走）

```c
struct Descriptor {
    const char *name;                  // +0x000
    const char *description;           // +0x008
    unsigned category;                 // +0x010   0 Combat  2 Visual  3 Block  4 Utility
    unsigned char enabled;             // +0x014
    unsigned char pad_15[3];
    unsigned flags;                    // +0x018
    unsigned bind;                     // +0x01c
    unsigned char hold;                // +0x020
    unsigned char pad_21[0x27];
    struct { void *handler; void *context; } handlers[27];   // +0x048
    void (*settings_gui)(void *, void *);                    // +0x1f8
    void (*settings_register)(void *);                       // +0x200
    void *suffix;                                            // +0x208
};
```

### 4.2 事件槽位（`handlers[事件类型]`，实测）

`dispatch_event()` 直接拿事件结构的**第一个 dword 当 handler 列表下标**：

```c
extern "C" void dispatch_event(Mgr *manager, unsigned *e) {
    unsigned type = *e;
    if (type >= 27) return;
    Handler *h = manager->lists[type];
    for (Handler f = *h; f; f = *++h) f(e);
}
```

| 槽 | 事件 | 载荷 |
|---|---|---|
| `0` | tick | `Hook*` → `+0x08` 是 `Ctx`（jvm / world / player / in_game…） |
| `5` | toggle | `{pad[8]; Ctx*; Module*; unsigned char on}` —— **只手动开关才触发**，profile 自动启用不发 |
| `6` | world render | `+0x08` 是摄像机 `Vec3` |
| `10` | gui render | |
| `13` | rotation override | 写 `h[8]=1; h[9]=1` 表示本模块接管转向 |
| `17` | late tick | |
| **`20`** | **世界变更 `0x14`** | 见 §5 |

### 4.3 队列容量

`MODULE_QUEUE_CAP 128`。原版三处硬编码 `0x45`（69），模块数超过 69 后**第 70 个之后的开关点了没反应，不报错**。
Kill Aura / Speed / Stasis / Timer Advance 早期「没效果」全部是这个原因。

### 4.4 注册表

`src/engine/jni/module_manager.cpp`，`total = 0x48`：

```c
mods[0x39] = sw2_module_descriptor;          // SafeWalk 键盘版，顶掉原版
mods[0x43] = bed_esp_module_descriptor;
mods[0x44] = timer_advance_module_descriptor;
mods[0x45] = stasis_module_descriptor;
mods[0x46] = kill_aura_module_descriptor;
mods[0x47] = speed_module_descriptor;
```

三个循环（计数、分类、装表）**都必须跑 `total`**，只改一个会得到「数到了但进不了派发表」。

---

## 5. 世界变更事件 `0x14`（本次会话的关键发现）

`src/engine/base/java_natives_world.cpp` 里，四个游戏钩子都会派发 `0x14`：

```c
kind 0   chunk unloaded      v[0], v[1] = 区块坐标
kind 1   chunk dirty         v[0], v[1] = 区块坐标     ← S21/S26 区块数据到达时触发
kind 2   single block set    v[0..2] = x,y,z，v[3] = 新方块 id
```

模块从 `handlers[20]` 拿到它们。这是 1.8.9 上最接近 Myau 那种
`MixinBlockRendererDispatcher` 的东西：**方块被放/被拆，事件当场就到**，不需要轮询、不需要改字节码、
也不需要重新生成 `hook_tables.inc`。

「没有 `renderBlock` 钩子所以做不到秒响应」这个结论是错的 —— 只是没有人用过 20 号槽，全仓库此前零使用。

---

## 6. 当前模块状态

| 模块 | 状态 | 备注 |
|---|---|---|
| **Bed ESP** | ✅ 事件驱动 | 见 §7；走动跨区仍会重建一次索引 |
| Kill Aura | ✅ | 目标筛选 / 静默转头 / APS / switch delay；缺 AutoBlock（无发包接口） |
| Speed | ✅ | VANILLA / FLOAT 双模式 |
| Stasis | ✅ | 脉冲冻结 |
| Timer Advance | ✅ | 三档预设，默认 bind Z |
| SafeWalk | ✅ | 键盘版，顶掉原版槽位 0x39，按真实潜行键 |
| Inv Manager | ✅ | 护甲评分、每类留一件、windowClick 加速 |
| Chest Stealer | ✅ | Myau / Expo 两种节奏 |

**静默转头只有一套正确写法**（踩了四遍才确认）：

```c
if (!update_rotation_priority_peak(1000)) return;      // 不申请就完全没反应
Rot e = get_entity_rotation(jvm, player);
Rot o = get_silent_rotation_offset();
apply_silent_rotation(Rot{ wrap_angle(want.yaw - wrap_angle(e.yaw + o.yaw)),
                           want.pitch - (e.pitch + o.pitch) });
// 并在槽 13 里写 h[8] = 1; h[9] = 1;
```

三个坑：漏 `update_rotation_priority_peak` 完全无反应；增量按裸朝向算会每 tick 累加跑飞；
在 frame 回调里调 `apply_player_rotation` 会抽搐。

---

## 7. Bed ESP：从「永远扫不完」到事件驱动

改造前每轮把 `(2r+1)×(2h+1)×(2r+1)` 全扫一遍（r=32, h=16 → 139,425 格，每格一次完整 JNI 往返），
且**每完成一轮就在下一 tick 重新开始**，玩家一走出扫描框还会中途重启。日志实证：一次会话里
`pass done` 出现 1,607 次、累计约 2.2 亿次方块查询，而 `drawing N` 只变了 24 次 —— 99% 的扫描是纯浪费。

现在（`src/logic/visual/bed_esp.cpp`）：

| 机制 | 说明 |
|---|---|
| 一趟建索引 | 进世界 / 走开半个 range 才跑一趟，扫完即停 |
| 近层优先 | 层序按 `0, -1, +1, -2, +2 …`，脚下那层第一个扫 |
| 每层即时发布 | 不等整趟结束 |
| `handlers[20]` | 放下 `+ bed`、拆掉 `- bed`、区块卸载清该区块 —— 零帧延迟 |
| `handlers[5]` | 开启时清空索引重扫（关闭期间错过的事件不会残留） |
| 稳态 prune | 每 40 tick 复核 8 个位置，兜事件漏报 |
| `BED_SCAN_BUDGET 16384` | 每 tick 上限；全部 JNI 局部引用用完即 `DeleteLocalRef` |

验收数据（1920×1009，r=64）：`ev20 kind=2` 12 条、`+ bed` 38 / `- bed` 26、整趟 `pass done` 只有 3 次、
`DIAG` 残留 0、`drawing` 稳定在 4–6。

**已知边界**：走动超过半个 range 仍会重建一次索引（r=32 约 0.45 s，r=64 约 1.7 s）。
彻底消除的做法是用 `kind 1` 喂区块队列，一个区块只扫一次（16×16×33）。

---

## 8. 毛玻璃 GUI：已接入，真机验收通过

面板背后是当前世界画面：整屏 `glReadPixels` → 1/2 → 1/4 降采样 → 横竖两趟盒式模糊 → 行翻转 →
交给 NanoVG 上传 → `nvgImagePattern` 铺满面板 → tint + 顶部 1.5px 高光。捕获在帧钩子里、
`begin_frame_framebuffer` **之前**（之后默认 framebuffer 里就没有世界了），每 3 帧一次、只在界面打开时跑。

```
include/slinky/gui_frosted.h   第 9 行  SLINKY_FROSTED_GUI   0 = 原来的平灰底   1 = 毛玻璃（当前 1）
src/engine/gui/gui_frosted.cpp           捕获 / 降采样 / 模糊 / 上传 / 绘制 / 释放
```

改完开关必须重链 exe（§3）。关掉时整个文件编成三个空函数，行为与改动前完全一致。

### 四个坑，第一个是「画了不显示」的唯一原因

| # | 问题 | 症状 |
|---|---|---|
| 1 | `nvgImagePattern` 要的是 **NanoVG image handle**，不是 GL 纹理名 | `glnvg__findTexture()` 在上下文纹理表里线性找，找不到返回 NULL，`nvg__fill()` 直接 `return 0` —— **不报错、不崩、一条顶点都不发**。用 `glGenTextures` 的名字（驱动分配，几百上千）去传，永远对不上 handle（从 1 开始编号），整个填充被静默跳过。正解：`nvgCreateImageRGBA` / `nvgUpdateImage` 让 NanoVG 自己管纹理 |
| 2 | `box_blur` 只跑了横趟 | 拖影，不是磨砂。横竖各一趟才对 |
| 3 | `CAP_MAX_W/H` 夹的是缩放后的变量、不是缓冲 | 1440p / 4K 下按 480 宽去读 900 宽的行，越界读。按 `fb/4` 走，只留总像素上限 |
| 4 | `glReadPixels` 的 alpha 通道可能回 0 | `FillPaint` 画出来整块透明。降采样时把 A 直接写 255 |

**坑 1 为什么特别容易踩**：仓库里两条能画的路径 —— `draw_framebuffer_texture_over_screen()` 传
`renderer->fb->tex`、`draw_texture()` 传 `GImg->tex` —— 那两个字段**名叫 tex、实际存的是
`NVGLUframebuffer.image`（handle）**。名字骗人，于是「传 GL 名也能画」这个印象是错的。

一眼分辨的办法是看数字形态：handle 是小整数（`image 5`），GL 名字是驱动分配的（`tex 666`）。

真机日志（验收时抓的）：

```
frosted: 1920x1009 -> 480x252, blur r=3, mean=95.7     ← mean 非 0 = 读回的是真实画面
frosted: image 5 created (480x252)
frosted: draw alpha=1.00 panel=1920x1009 image=5 (480x252)
```

`mean=0` 就是读回黑屏 —— 说明世界不在默认 framebuffer 里，捕获点要换。

调参：`CAP_BLUR_RADIUS`（3 ≈ 12px 全屏模糊）、`CAP_EVERY`（3 → 1 更跟手，卡就 → 6）、
`gui_draw_frosted` 里的 tint alpha（0.45）与高光 alpha（0.10）。

---

## 9. 踩坑表（全部真实发生过）

| 现象 | 根因 | 修法 |
|---|---|---|
| 改了没效果 | 游戏跑的是旧 DLL | 见 §3 的三种变体 |
| 毛玻璃「捕获成功但画不出来」，不报错也不崩 | `nvgImagePattern` 传了 GL 纹理名，它要的是 NanoVG image handle；查不到就 `return 0` 静默跳过 | 用 `nvgCreateImageRGBA` / `nvgUpdateImage` 让 NanoVG 自己管纹理（§8） |
| 新模块点了没反应 | `en_list` 容量 0x45=69 | `MODULE_QUEUE_CAP 128` |
| 一开就崩 | `FN` 声明的返回类型与真实定义不符（`Vec3` 当 `double`），栈被写坏 | 每个 `FN` 都和定义比对 |
| Bed ESP 永远 `found=0` | 每 tick 数千次 `getBlockState` 不 `DeleteLocalRef`，JNI 局部引用表耗尽 | 用完立即释放 |
| 一轮只扫 6% | 层结束时没复位扫描游标 | 每层结束无条件复位 |
| 提高预算无效 | 一个 tick 只能推进一层 | 多层层间 `goto` |
| 一开 Bed ESP 就崩 | 对已经是 `Block` 的对象又调 `getBlock()`（`get_block_state` 内部已转过） | 直接用返回值 |
| 床框被方块挡住 | 没关深度测试 | `glDisable(0xB71)` / `glEnable(0xB71)` 成对 |
| 改框高度就「坏」 | 描边模式下 0.5625 高的框成一条细带 | 填充开了之后再调 |
| 静默箱子界面没了但拿不到东西 | 直接写 `currentScreen = null`，`handleMouseClick` 需要 screen | 改用 `windowClick` |
| 设 `inGameHasFocus=0` 后模块全停 | 客户端认为窗口失焦，tick 停了 | 不要动这个字段 |
| Inv Manager 滑块拖不动 | 设置对象是 cpp 里的独立实例，原生 GUI 不写它 | 跟随 `chest_stealer_mode_setting` |
| `snprintf` 链接失败 | `/NODEFAULTLIB` 环境 | 用多条日志代替拼接 |
| `libc_cos` 不存在 | `crt.cpp` 只导出 `libc_cosf` | 用 float 版本 |
| 全局 `replace("clutch_", "scaf_")` | 一次写坏 230k 行，改错 71 个 data.s 符号 | 不做全局文本替换 |

---

## 10. 未完成

| 项 | 卡在哪 | 下一步 |
|---|---|---|
| Bed ESP 走动跨区延迟 | 仍需重建一次索引 | 区块增量（`kind 1` 喂队列） |
| Chest Stealer 静默化 | 只把 windowClick 接进了 Inv Manager | 照 `inv_click()` 复制 |
| Kill Aura AutoBlock | 无 C08 构造/发送接口 | 需 `NetworkManager` 获取路径 |
| Bed Wars 拉弓飞行 | 映射已查到（`C08PacketPlayerBlockPlacement`），缺发包对象 | 同上 |
| 毛玻璃帧率代价 | 未量过；`CAP_EVERY=3` 时估算 1–3 fps | 嫌重就 → 6，或只在打开动画期间捕获一次 |

---

## 11. 代码地图：想改什么去哪里

| 想做的事 | 文件 |
|---|---|
| 加一个模块 | `src/logic/<类>/<名字>.cpp` + 同名 `.h`，然后在 `module_manager.cpp` 占一个 `mods[]` 槽并抬 `total` |
| 看游戏能提供什么能力 | `src/engine/base/java_natives_*.cpp`（每个 JNI native 一个文件） |
| 加/查游戏钩子 | `src/engine/jni/hook_tables.inc`（**生成物，勿手改**） |
| 配置与 profile 存取 | `src/engine/jni/module_manager.cpp`、`src/data/*.s` 里的字符串 |
| 界面布局 / 卡片 / 状态 | `src/engine/gui/gui_layout.cpp` · `gui_cards.cpp` · `gui_state.cpp` |
| GL 函数槽 | `src/platform/gl_slots.cpp`（声明在 `include/slinky/gl_slots.h`） |
| NanoVG 入口 | `src/platform/nvg_names.cpp`（包装成 C 符号，`extern "C"`） |
| 混淆名对照 | 解压 `OpenExpo.zip` 得到 `OpenExpo/mappings/joined.srg`，是加新 JNI ID 的唯一可靠来源；**不要猜名字** |
