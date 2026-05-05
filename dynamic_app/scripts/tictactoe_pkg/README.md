# tictactoe_pkg

人机井字棋动态 app。**PC 端做 AI、设备端做 UI** 的协同样例。

## 能力

- 设备端：3 × 3 棋盘 + 落子
- PC 端：minimax AI + 监控 GUI 显示对局状态
- 设备发请求 → PC 计算下一步 → PC 推回坐标
- **依赖 PC 端配套插件**：[`tools/plugins/tictactoe/`](../../../tools/plugins/tictactoe/)（含 GUI）

## 适合谁看

想做"设备端做轻量 UI、PC 端跑算力"模式的开发者（语音识别、图像识别、AI 都适用此模式）。

## 上传到设备

```bash
python tools/dynapp_uploader/cli.py upload dynamic_app/scripts/tictactoe_pkg
```

## 相关文档

- [动态app开发者指南](../../../docs/参考/动态app开发者指南.md)
- [双端通信协议](../../../docs/参考/动态app双端通信协议.md)
