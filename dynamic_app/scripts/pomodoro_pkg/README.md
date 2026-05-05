# pomodoro_pkg

番茄钟动态 app。多级页面 + NVS 持久化的样例。

## 能力

- 主页：开始 / 暂停 / 重置工作番茄
- 设置页（push）：自定义工作 / 短休 / 长休时长
- 状态用 `sys.app.saveState` 落 NVS，掉电恢复
- **不依赖 PC 端配套插件**

## 适合谁看

想了解动态 app 多级页面 router + 持久化的开发者。

## 上传到设备

```bash
python tools/dynapp_uploader/cli.py upload dynamic_app/scripts/pomodoro_pkg
```

## 相关文档

- [动态app开发者指南](../../../docs/参考/动态app开发者指南.md)
- [Router / sys.app.saveState 用法](../../../docs/参考/动态app_JS_API速查.md)
