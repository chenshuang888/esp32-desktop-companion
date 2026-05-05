# hello_pkg

最小可运行示例 app，作为开发者第一个动态 app 模板。

## 能力

- 一个 panel + 一个按钮 + 一段文字
- 点按钮触发 `sys.ui.toast`
- **不依赖 PC 端配套插件**

## 适合谁看

刚拿到固件想写第一个动态 app 的开发者。看完 27 行 main.js 就能模仿写出自己的 app。

## 上传到设备

```bash
python tools/dynapp_uploader/cli.py upload dynamic_app/scripts/hello_pkg
```

或通过桌面端「市场」页面安装。

## 相关文档

- [动态app开发者指南](../../../docs/参考/动态app开发者指南.md) — 30 分钟入门
- [JS API 速查](../../../docs/参考/动态app_JS_API速查.md)
