# settings_pkg

设置页样例 app，多级 Router 的验证用例。

## 能力

- 顶层菜单 → 多级子页面 push / pop
- 演示路由栈深度 ≤ 4 的限制
- **不依赖 PC 端配套插件**

## 适合谁看

想理解动态 app `sys.router` API 行为（push 不 destroy 旧页 / 上滑 pop / 栈深 = 1 时退出 app）的开发者。

## 上传到设备

```bash
python tools/dynapp_uploader/cli.py upload dynamic_app/scripts/settings_pkg
```

## 相关文档

- [动态app开发者指南](../../../docs/参考/动态app开发者指南.md) §Router 章节
- [JS API 速查](../../../docs/参考/动态app_JS_API速查.md)
