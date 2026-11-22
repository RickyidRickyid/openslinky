# Driplite UI 相关代码（不是面板）

## 先说清楚这里面没有什么

**没有 ClickGUI / 面板代码。**

我扫过整个反编译目录，`clickgui`、`panel`、`menu` 零命中。native 载荷里只有三类 UI：

- HUD（`hud_final.c`）—— 屏幕上的信息条
- 通知（`notifications_final.c`）—— 右下角弹出提示
- 雷达（`radar_final.c`）—— 小地图

面板大概率在 **Java 层**，不在这次逆向的范围里。

## 这里面有什么

| 文件 | 内容 |
| --- | --- |
| `hud_final.c` | HUD 主逻辑，679 行，5 个虚函数 |
| `notifications_final.c` | 通知组件，532 行，6 个虚函数 |
| `radar_final.c` | 雷达，1278 行，5 个虚函数 |
| `visual_helpers.c` | 绘制原语库，15730 行（含 drawSprite、视锥体裁剪、顶点池） |
| `config-schema.json` | 所有模块的配置字段定义（offset / type / min / max） |

## 这三类 UI 怎么画

跟 ESP 一样走 `reach_send_*` → 顶点池 → `glDrawArrays`。差别在坐标空间：

- ESP 走世界空间投影（`reach_send_a`，带 3D 变换）
- HUD / 通知 / 雷达走屏幕空间（`reach_send_b`，坐标直接是像素）

文字不在这里画。`class_map.csv` 里有 `net/minecraft/client/gui/FontRenderer` 的映射，说明文字是通过 JNI 调 MC 自己的字体渲染器，native 只负责把文字的位置算出来。

## 能参考的视觉参数

`config-schema.json` 里 `player-esp` 的字段：

```
Box color (on / R / G / B)
Box rounding          ← 框有圆角
Glow size             ← 有发光
Drop shadow           ← 有投影
Text size
```

也就是说 **ESP 框本身有圆角、发光、投影**，但这些是世界空间几何，不是 GUI 面板的卡片。

## 结论

如果你想参考的是「面板卡片长什么样」，这批材料给不了答案——没有截图，没有面板代码。

如果你要的是「2D 矩形 + 文字在固定管线下怎么组织」，`notifications_final.c` 最接近，它就是个带进场动画的小卡片。

