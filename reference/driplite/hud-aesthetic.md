# Driplite 的 HUD 美学（从配置和渲染代码还原）

## 一、它有哪些元素

配置结构里 21 个字段，去掉通用的（enabled/keybind），剩下 15 个全是显示开关：

```
0x630  Logo              左上角标志
0x631  Watermark         客户端名
0x632  Show version      版本号
0x633  Module list       已启用模块的列表
0x634  Show module details   模块附带信息（设置项摘要）
0x635  Show binds        按键绑定
0x636  Notifications     通知流
0x637  FPS counter       帧数
0x638  Lower case        ← 模块名小写
0x639  Background        背景板
0x63a  Drop shadow       投影
0x63b  Hide combat       按分类隐藏
0x63c  Hide visuals
0x63d  Hide movement
0x63e  Hide utility
0x640  Scale             整体缩放
0x644  Module scale      模块列表单独缩放
0x648  Primary color (R) ← 主色，只有一组
0x64c  Primary color (G)
0x650  Primary color (B)
```

## 二、三个决定审美的细节

**一，只有一组主色。**

不是「每个模块一个颜色」，也不是「主题包」。整个 HUD 就一个 RGB，所有元素共用。这是克制的做法——配色统一，视觉噪音低。

**二，`Lower case` 是个独立开关。**

模块名默认大写显示（`Player ESP`），勾上变成小写（`player esp`）。把大小写做成选项而不是写死，说明作者在意这个细节。小写在 cheat client 里是「极简派」的标志，跟 `PLAYER ESP` 那种粗体大写是两种审美。

**三，模块列表按四个分类可以分别隐藏。**

`Hide combat / visuals / movement / utility` 对应模块的四大类。也就是说模块列表不是一坨，是**分组**的——需要的时候可以只显示视觉类，把战斗类藏起来。

## 三、渲染逻辑：右对齐、垂直堆叠、时间驱动

`hud_vfn7` 的主体是一个链表遍历：

```c
lVar15 = *(lVar19 + 0xa0);              // 列表尾
if (lVar15 != *(lVar19 + 0x98)) {       // 非空
    fVar20 = (float)uVar25 + ...;        // 起始 x（屏幕宽）
    fVar31 = fVar30 + ...;               // 起始 y
    do {
        lVar13 = *(*(lVar15 - 0x10) + 0x28);   // 创建时间
        lVar12 = *(*(lVar15 - 0x10) + 0x30);   // 存活时长
        if (now/1000 < lVar13 + lVar12 + 500) {
            fVar21 = 文字宽度;
            fVar22 = (local_164 - fVar21) + fVar30;   // 右对齐
            fVar23 = 淡出系数;
            fVar22 = local_164 - (fVar21 + ...) * fVar30;  // 滑入
        }
    } while (...);
}
```

四个特征：

**右对齐**：`local_164 - fVar21`，`local_164` 是屏幕宽度。元素从右边缘往左排。

**垂直堆叠**：`fVar31` 每轮累加，从下往上或从上往下叠。

**时间驱动淡出**：`(now/1000) < 创建时间 + 存活时长 + 500`，超过 500ms 开始消失。消失过程用 `fVar23` 做 alpha 插值。

**滑入滑出**：`fVar22` 的位置乘 `fVar30`（0→1 的进度），元素从边缘滑进来。

这是**非侵入式**的设计：贴边、不挡视野、自动消失。

## 四、代码里的动画曲线

```c
fVar23 = (float)uVar14 / _DAT_279180ab01c;    // 归一化进度
fVar23 = fVar23 * _DAT_279180ab038 * fVar23 * fVar23;   // 三次方
```

`t * k * t * t` 是**三次缓动**（ease-in），起步慢、收尾快。用在淡出上，视觉上比线性自然。

还有一段：

```c
dVar26 = hud_sub_42d170((double)(DAT_279180ab024 - (fVar23 + fVar23)), (int)DAT_279180ab028);
```

调了个数学函数做曲线映射，参数是 `1 - 2t` 和某个整数。像是 sigmoid 或者 smoothstep。

## 五、跟 ESP 的关系

HUD 和 ESP 是两套东西，但共享同一套绘制原语：

| | ESP | HUD |
| --- | --- | --- |
| 坐标空间 | 世界 → 屏幕投影 | 屏幕像素 |
| 提交函数 | `reach_send_a`（3 参，带 z） | `reach_send_b`（4 参，w=1） |
| 内容 | 框、线、文字 | 文字为主 |
| 动画 | 无 | 时间驱动淡入淡出 |

HUD 没有圆角、没有渐变、没有描边——因为它走的是 `reach_send_b` 的纯色提交，颜色靠 `glColor4f` 全局设一次。

## 六、能抄的和不能抄的

**能抄**：

- 单主色的克制配色
- 右对齐 + 贴边的布局（不挡视野）
- 时间驱动的淡入淡出 + 三次缓动
- 模块列表按分类分组，可分别隐藏
- 小写模块名这个选项

**抄不了**：

- 具体间距和字号——那些常量在 `.data` 段运行时才填，静态读出来是 0
- 具体颜色值——配置里是用户自己设的，出厂默认值没找到
- 面板（ClickGUI）——不在 native 层

## 七、一句话

Driplite 的 HUD 美学不是靠特效堆出来的，是靠**克制**：一个颜色、一种字体、贴边排列、该消失就消失。它的视觉分量全压在 ESP 上，HUD 只做信息补充。

