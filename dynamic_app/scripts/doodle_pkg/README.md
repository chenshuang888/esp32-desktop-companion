# doodle_pkg

涂鸦板动态 app，演示 `sys.canvas` 像素绘图能力。

## 能力

- 240 × 200 画布，手指拖动画线
- 9 种颜色切换（assets/ 下 9 个 PNG）
- 一键清空画布
- 保存 / 加载到 LittleFS（与 LVGL bin 互转）
- **不依赖 PC 端配套插件**

## 适合谁看

想用动态 app 做绘图、像素艺术、简单游戏（贪吃蛇等）的开发者。

## 上传到设备

```bash
python tools/dynapp_uploader/cli.py upload dynamic_app/scripts/doodle_pkg
```

包含 26 KB assets，传输需几秒。

## 相关文档

- [动态app开发者指南](../../../docs/参考/动态app开发者指南.md)
- [JS API 速查](../../../docs/参考/动态app_JS_API速查.md) §sys.canvas
