# gomoku_pkg

BLE 联机五子棋动态 app。**双向通信 + 心跳超时 + 重连补推**的完整范例。

## 能力

- 13 × 13 棋盘，双方各执黑白
- 每 1500 ms 心跳，超过 4 s 无应答判负
- 主动 leave / 被动断连区分处理
- 重连时 sync_req 补推本端棋谱
- **依赖 PC 端配套插件**：[`tools/plugins/gomoku/`](../../../tools/plugins/gomoku/)（含 GUI 页面）

## 适合谁看

想做 BLE 联机游戏、需要双端协同 + 容错的开发者。这是最复杂的内置示例（461 行 JS + 完整 PC plugin）。

## 上传到设备

```bash
# 同时上传设备 JS 和安装 PC 插件（推荐通过桌面端「市场」一键装）
python tools/dynapp_uploader/cli.py upload dynamic_app/scripts/gomoku_pkg
```

## 相关文档

- [动态app开发者指南](../../../docs/参考/动态app开发者指南.md)
- [双端通信协议](../../../docs/参考/动态app双端通信协议.md)
- [BLE 联机五子棋开发日志](../../../docs/日志/2026-05-02_动态App_BLE联机五子棋.md) — 心跳/超时/重连设计原文
