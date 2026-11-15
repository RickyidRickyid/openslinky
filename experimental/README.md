# experimental/

未参与编译的实验实现。CMake 只 glob `src/`、`include/`、`third_party/`，这个目录不会被构建。

| 文件 | 状态 |
|---|---|
| `gui_frosted.cpp` | 2026-11-13 原版。**会崩**：读回缓冲按 1/4 尺寸分配却按整屏 `glReadPixels`，1080p 会往 518 KB 的堆块里写 8.2 MB。仅作对照保留。 |
| `gui_frosted-fixed-2026-11-14.cpp` | 上一份的修复版：缓冲尺寸、`nvg_image_pattern` 返回类型、`nvg_FillColor` 覆盖 pattern 三处都修了。捕获链路已验证可用（日志 `frosted: 1920x1009 -> tex 480x252`），**绘制链路未生效**，因此没启用。排查方向写在仓库根目录 README 的 §8。 |
| `esp_overlay.cpp` | 独立置顶窗口 + `SetWindowDisplayAffinity(WDA_EXCLUDEFROMCAPTURE)` 的 ESP overlay。逻辑完整但有 4 处已知崩溃原因（GDI 句柄泄漏、坐标未钳制、窗口在渲染线程创建、`BLENDFUNCTION` 布局），启用前必须按文件头的顺序逐步验证。 |

要把其中任何一份搬回编译树，移动到 `src/engine/gui/`（或 `src/logic/visual/`）并重新配置 CMake 即可。
