# `tools/` —— PC 端工具总目录

> ESP32-S3 桌面伴侣（esp32-desktop-companion）的 PC 配套：BLE 桌面伴侣 + 动态 app 配套服务 + 构建期脚本。

---

## 目录速览

```
tools/
├── companion/        ← BLE 桌面伴侣主程序（python -m companion）
├── plugins/          ← 动态 app 配套插件（加目录即生效）
├── dynapp_sdk/       ← 给外部脚本用的 BLE 客户端库 + bridge 协议示例
├── dynapp_uploader/  ← 给外部脚本用的 .pkg 上传客户端库
├── scripts/          ← 构建期脚本（字体子集、manifest、组件 patch）
└── requirements.txt  ← 主程序运行依赖
```

---

## 4 个子目录的定位

| 目录 | 定位 | 何时打开 |
|---|---|---|
| `companion/` | 运行时主程序：BLE 扫描连接、原生 service 配套、插件加载、GUI 侧边栏 | 改 PC 端"主程序"逻辑、原生 BLE service 配套 |
| `plugins/` | 动态 app 在 PC 端的代理（先看 `plugins/README.md` §0 判定规则） | ESP32 端动态 app 需要 PC 帮它做事时 |
| `dynapp_sdk/` | 独立 SDK 库：`Client` / `Router` / `examples/bridge_test.py` | 写**外部脚本**跟设备 a3a3 桥通信（不用主程序时） |
| `dynapp_uploader/` | 独立 SDK 库：.pkg 上传协议封装 | 外部 CI / 命令行工具批量推 pack |
| `scripts/` | 构建期工具：字体子集生成、manifest 生成、第三方组件 patch | 改字体 / 加图标 / 写新动态 app 包 |

`companion/` 是**默认打开点**——99% 改动都在这里。

---

## 启动 / 常用命令

```bash
# 启动 PC 端伴侣（GUI 模式，默认）
python -m companion

# 后台模式（无 tkinter 窗口）
python -m companion --no-gui

# 指定设备名
python -m companion --device ESP32-S3-DEMO

# 字体子集化（新增中文文案后跑一次）
python tools/scripts/gen_font_subset.py

# 图标子集化（gen_icons_subset.py 内 ICONS dict 改了之后）
python tools/scripts/gen_icons_subset.py

# 给动态 app 包生成 manifest.json
python tools/scripts/make_pack_manifest.py dynamic_app/scripts/foo_pkg \
    --id foo_pkg --name 我的应用 --icon STAR --color ACCENT

# managed_components 第三方组件幂等补丁（idf.py reconfigure 后跑一次）
python tools/scripts/patch_esp_mquickjs_component.py

# bridge 联调（不用启动主程序）
python tools/dynapp_sdk/examples/bridge_test.py --to echo
```

依赖安装：

```bash
pip install -r tools/requirements.txt
```

---

## 依赖关系（从下往上读）

```
                     [外部脚本]
                          │
                          ▼
        ┌───────────────────────────────────┐
        │  dynapp_sdk/  +  dynapp_uploader/  │   独立 SDK 库（不依赖主程序）
        └───────────────────────────────────┘
                          ▲
                          │ 主程序 + 插件可用
                          │
        ┌───────────────────────────────────┐
        │  companion/                        │
        │   __main__.py / core.py / bus.py   │
        │   ├── providers/                   │   只懂 BLE 协议
        │   ├── gui/                         │   只懂页面渲染
        │   ├── platform/                    │   平台能力（HTTP/Win API/打包）
        │   ├── plugin_sdk/                  │   插件唯一稳定 API
        │   └── plugin_manager.py            │
        └─────────────┬─────────────────────┘
                      │ 加载并路由
                      ▼
        ┌───────────────────────────────────┐
        │  plugins/<name>/  （只走 plugin_sdk）│
        └───────────────────────────────────┘
```

**铁律**：
- `plugins/` 只 `import companion.plugin_sdk[.platform/.gui]`，不直接进 `companion.gui.*` / `companion.platform.*`。
- `companion/` 内部不允许出现具体动态 app id（如 `"gomoku_pkg"`）。
- `scripts/` 是一次性工具，不被运行时 import。

---

## 想加新功能？

| 想做什么 | 看哪里 |
|---|---|
| ESP32 端某动态 app 要 PC 配合（拉数据 / 系统 API / 联机） | `plugins/README.md` |
| 加 / 改原生 BLE service 配套 | `companion/providers/native/` |
| 加 PC 端独立功能页（不绑动态 app） | `companion/gui/pages/` + 必要时 `companion/platform/` |
| 给主程序加平台能力（HTTP / 系统 API） | `companion/platform/<name>.py`，按需在 `plugin_sdk/platform.py` re-export |
| 写外部独立脚本跟设备桥接 | 参考 `dynapp_sdk/examples/bridge_test.py` |

---

## 已知非阻塞 TODO

- 历史 `docs/*.md`（工作日志快照）里仍引用旧路径 `tools/gen_*.py` —— 不修，它们是历史档案。
- 未来如有「插件签名 / 沙箱 / 在线市场」需求，扩 `companion/plugin_manager.py`。
