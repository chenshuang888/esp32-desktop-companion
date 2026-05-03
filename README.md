# ESP32-S3 桌面伴侣 (Desktop Companion)

基于 ESP-IDF v5.4.3 + LVGL v9.5.0 的 ESP32-S3 触摸屏桌面伴侣。通过 BLE 与 PC（Python 工具）双向联动，实现时间同步、天气/通知推送、正在播放副屏、反向控制面板等一体化体验。**v0.8 起内置 MicroQuickJS，支持以 JS 脚本编写的"动态 App"**：固件不重烧即可新增小程序（闹钟、计算器、2048、计时器、回声、动态天气/音乐等），并通过统一的 BLE JSON 透传管道与 PC providers 通信。

## 功能概览

### 内建页面（C 实现）

| 页面 | 功能 | BLE 方向 |
|------|------|---------|
| 时间 | 实时时钟 + 手动调节（时/分、年/月/日） | PC → ESP (CTS 0x1805) |
| 菜单 | 页面导航、背光亮度设置、BLE 状态、动态 App 入口 | — |
| 天气 | 当前温度/最高最低/湿度/描述/城市 | PC → ESP (自定义 0x8a5c0001) |
| 通知 | 滚动列表，保存最近 10 条（类别+优先级） | PC → ESP (自定义 0x8a5c0003) |
| 音乐 | 正在播放曲目 + 艺术家 + 进度条 + 播放状态；屏上按上/下/播暂键触发 PC 媒体键 | PC → ESP (0x8a5c0007 WRITE) / ESP → PC (0x8a5c000d NOTIFY) |
| 系统 | CPU / 内存 / 磁盘 / 网速 / 温度 / 电池 | PC → ESP (自定义 0x8a5c0009) |
| 动态 App | 通用宿主页面：跑任意 JS 脚本，UI 完全由脚本声明 | dynapp_bridge (0xa3a30001) |
| 关于 | 版本信息 | — |

### 动态 App（JS 脚本，无需烧录即可扩展）

| App | 说明 | 是否需要 PC provider |
|-----|------|----------------------|
| alarm | 多闹钟设置，支持编辑/删除/启用，状态落 NVS | 否 |
| calc | 计算器（四则 + 历史 + 大小写/小数处理） | 否 |
| timer | 倒计时，含 1/3/5/10 分钟快捷预设 | 否 |
| 2048 | 滑动手势 2048 小游戏 | 否 |
| echo | BLE 透传管道回环测试 | 是（dynapp_companion） |
| weather | JS 版天气页，PC 端拉真实数据 | 是（providers/weather_provider.py） |
| music | JS 版正在播放，含媒体键回写 | 是（providers/media_provider.py） |

## 硬件配置

### 板型
- **MCU**: ESP32-S3 N16R8（Xtensa LX7 双核 @ 240MHz）
- **Flash**: 16MB, **QIO** @ 80MHz（不是 Octal！）
- **PSRAM**: 8MB, **Octal** @ 80MHz
- **LCD**: ST7789, 240×320, SPI @ 40MHz, RGB565
- **触摸**: FT5x06 电容屏, I2C @ 400kHz
- **蓝牙**: NimBLE 4.2（非 5.0）

> ⚠️ N16R8 的 Flash 是 QIO 而非 Octal，`CONFIG_ESPTOOLPY_OCT_FLASH` 必须保持 **OFF**，否则会因时序冲突进入 boot 循环。

### 引脚

| 外设 | 引脚 | 外设 | 引脚 |
|------|------|------|------|
| LCD_SCK | GPIO12 | TOUCH_SCL | GPIO18 |
| LCD_MOSI | GPIO11 | TOUCH_SDA | GPIO17 |
| LCD_CS | GPIO10 | TOUCH_RST | GPIO15 |
| LCD_DC | GPIO9 | TOUCH_INT | GPIO16 |
| LCD_RST | GPIO8 | LCD_BL | GPIO14 (LEDC PWM) |

引脚定义全部集中在 `drivers/board_config.h`。

## 软件架构

### 目录结构

```
demo6/
├── main/                     # 启动入口（app_main）
├── app/                      # 应用层
│   ├── app_main.c           # UI 任务创建 + 页面注册 + dynamic_app_ui_drain
│   ├── app_fonts.c          # Tiny TTF 字体初始化
│   ├── fonts/               # 嵌入 TTF（中文子集 ~3.4MB）
│   └── pages/               # 8 个页面（含 page_dynamic_app 宿主）
├── framework/                # 轻量页面路由器
│   └── page_router.c/h      # create/destroy/update + prepare/commit 异步切屏
├── drivers/                  # 硬件抽象层
│   ├── lcd_panel.c/h        # ST7789 + 背光 PWM
│   ├── touch_ft5x06.c/h     # FT5x06 I2C 触摸
│   ├── lvgl_port.c/h        # LVGL 移植（双缓冲 40 行）
│   └── ble_driver.c/h       # NimBLE 协议栈 + GAP + 连接句柄 / SUBSCRIBE 分发
├── services/                 # 业务服务层
│   ├── persist.c/h          # NVS KV / blob 统一封装
│   ├── settings_store.c/h   # 背光 + 系统时间持久化
│   ├── time_service/manager       # Current Time Service
│   ├── weather_service/manager    # 天气推送
│   ├── notify_service/manager     # 通知推送（10 条环形缓冲 + 落盘）
│   ├── media_service/manager      # 正在播放（带 esp_timer 进度插值）+ 媒体键 NOTIFY
│   ├── system_service/manager     # PC 系统状态推送 + 反向请求 char
│   └── dynapp_bridge_service.c/h  # 动态 App 通用 BLE 透传管道（JSON, ≤200B）
├── dynamic_app/              # 动态 App 框架（MicroQuickJS + LVGL 桥）
│   ├── dynamic_app.c        # 控制层：script_task 主循环 + 生命周期
│   ├── dynamic_app_runtime.c       # 引擎层：JSContext 创建/销毁/eval
│   ├── dynamic_app_natives.c       # API 层：sys.* native fn
│   ├── dynamic_app_registry.c      # 注册表：app 名 → 嵌入 JS buffer
│   ├── dynamic_app_ui*.c    # UI 调度 / 注册表 / 样式（11-key apply）
│   └── scripts/             # 内嵌 JS：prelude + 7 个 app
├── tools/                    # PC 端 Python 伴侣
│   ├── desktop_companion.py    # 内建页面（音乐/通知/天气）后台服务
│   ├── ble_time_sync.py        # 时间/天气/通知 GUI（CustomTkinter）
│   ├── dynapp_companion.py     # 动态 App 后台 provider 桥（weather/music）
│   ├── dynapp_sdk/             # 动态 App PC 侧 SDK（client/router/constants）
│   ├── providers/              # 各 dynamic app 的 PC 端逻辑
│   ├── media_publisher.py      # 仅音乐推送
│   ├── gen_font_subset.py      # TTF 子集生成
│   └── requirements.txt
├── docs/                     # 各阶段开发日志
├── partitions.csv            # 自定义分区（16MB）
└── sdkconfig.defaults
```

### 分层依赖

```
main
 └─► app ──┐
           │  page_router
           ├─► framework
           │
           ├─► services ──► drivers
           │   (service 自管 GATT 注册；通过 ble_driver_get_conn_handle 发 notify)
           │
           └─► services
               persist → settings_store / notify_manager
               *_service (GATT, BLE host 线程)
                    └── *_manager (UI 线程单写)
```

关键约束：**services → drivers 单向依赖**。连接句柄由 `ble_driver_get_conn_handle()` 暴露；SUBSCRIBE 事件由各 service 自己注册回调订阅。

### Service / Manager 解耦模式

每个数据通道分为两半：

- **`*_service.c`** — GATT 接入层，跑在 NimBLE host 线程，只负责校验长度后 `push()` 入队。
- **`*_manager.c`** — 数据管理层，UI 线程独占消费（`process_pending`），更新快照、维护版本号。

BLE 回调从不阻塞、从不访问 LVGL、从不写 NVS。队列满时丢弃最旧数据（天气/通知/媒体越新越好）。对需要持久化的 manager（notify、settings），由 UI 线程按 dirty + 防抖策略 `tick_flush` 单写落盘，避免多线程写 NVS。

## 关键技术

### 中文字体 —— Tiny TTF 运行时渲染

LVGL 自带的 CJK 字库仅 1118 字，覆盖不全。项目改为：
- 用 `tools/scripts/gen_font_subset.py` 扫描源码提取用到的汉字，生成 `srhs_sc_subset.ttf`（~3.4MB）
- 通过 `EMBED_FILES` 嵌入固件镜像
- 启动时 `lv_tiny_ttf_create_data_ex` 创建两个字体（14px 正文 / 16px 标题）
- Fallback 链挂 `lv_font_montserrat_14/20`，解决 FontAwesome 图标（`LV_SYMBOL_*`）不在 CJK 子集里的问题
- 开启 `LV_USE_CLIB_MALLOC` 让大块 glyph cache 自动落到 PSRAM

### 内存布局

- `CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=16384` — 小分配走内部 RAM（性能），大块自动进 PSRAM
- `CONFIG_SPIRAM_MALLOC_RESERVE_INTERNAL=32768` — 保留 32KB 给 DMA / ISR 等不能用 PSRAM 的场景
- `CONFIG_BT_NIMBLE_MEM_ALLOC_MODE_EXTERNAL=y` — BLE 堆走 PSRAM，释放内部 RAM
- LVGL 双缓冲：240×40×2×2 ≈ 38KB

### 分区表（16MB）

```
nvs       0x9000    24KB        WiFi/BT 配对、settings、notifications 快照
phy_init  0xf000    4KB         RF 校准
factory   0x10000   6MB         主应用（含嵌入 TTF）
storage   auto      ~9.9MB      SPIFFS 预留（未来字体/OTA）
```

## 快速开始

### 编译固件

```bash
# 1. 激活 ESP-IDF 环境
. $HOME/esp/esp-idf/export.sh            # Linux/macOS
%USERPROFILE%\esp\esp-idf\export.bat    # Windows

# 2. 编译
idf.py set-target esp32s3
idf.py build

# 3. 烧录（Windows）
idf.py -p COM3 flash monitor
```

> 修改 `sdkconfig.defaults` 后必须删除 `sdkconfig` 再重新编译——ESP-IDF 只在 `sdkconfig` 缺失时才从 defaults 生成。

### PC 端工具

```bash
cd tools
pip install -r requirements.txt

# 一体化（推荐）：音乐副屏 + 控制面板（内建页面后台）
python desktop_companion.py

# 动态 App 后台（weather / music providers 等）
python dynapp_companion.py

# 单独：时间同步 / 天气 / 通知 GUI
python ble_time_sync.py
```

依赖：`bleak`（BLE）、`customtkinter`（GUI）、`winsdk`（Windows 媒体会话，仅 Win）、`requests`（天气 API）。`desktop_companion` 服务于内建页面，`dynapp_companion` 服务于动态 App，二者互不干扰、可同时运行。

### 重新生成中文字体子集

当新增中文文案时：

```bash
python tools/scripts/gen_font_subset.py
idf.py build   # 重新嵌入
```

脚本会扫描源代码，提取所有出现的汉字，生成最小子集 TTF，大幅压缩固件体积。

## BLE 协议约定

所有自定义特征值的 payload 均为 `__attribute__((packed))` 结构体，PC 端 Python 用 `struct.pack` 严格对齐：

| UUID 片段 | 方向 | 结构 | 字节 |
|-----------|------|------|------|
| 0x2A2B (标准 CTS) | PC → ESP (WRITE) / ESP → PC (NOTIFY) | `<HBBBBBBBB` / 1B seq | 10 / 1 |
| 8a5c0002 weather | PC → ESP | `<hhhBBI24s32s` | 68 |
| 8a5c0004 notify | PC → ESP | `<IBB2x32s96s` | 136 |
| 8a5c0008 media | PC → ESP | `<BBhhHI48s32s` | 92 |
| 8a5c000a system | PC → ESP | `<BBBBBBhIHH` | 16 |
| 8a5c000b weather-req | ESP → PC (NOTIFY) | 1B seq | 1 |
| 8a5c000c system-req | ESP → PC (NOTIFY) | 1B seq | 1 |
| 0x8a5c000d media-btn | ESP → PC (NOTIFY) | `<BBH` (id/action/seq) | 4 |
| 0xa3a30002 dynapp rx | PC → ESP (WRITE) | 透传 JSON 字符串 | ≤200 |
| 0xa3a30003 dynapp tx | ESP → PC (NOTIFY) | 透传 JSON 字符串 | ≤200 |

反向请求（ESP → PC 请求补推数据）由各业务 service 自管独立 NOTIFY char：订阅上升沿或页面 enter 触发一次 1B seq notify，PC 收到后用同 service 的 WRITE char 回写数据。media-btn 承担"屏上按钮 → PC 媒体键"单向事件（id=0 Prev / 1 PlayPause / 2 Next）。详见 `.trellis/spec/iot/protocol/esp-to-pc-notify-request-pattern-playbook.md`。

> 历史：原 `0x8a5c0005/0006` control service（含 Lock/Mute/Prev/Next/PlayPause 5 个按钮）于 2026-04-21 退役，短码 RETIRED 不再复用。media-btn 按"触发端与响应端同 service"原则拆回 media_service。

设备广播名：**ESP32-S3-DEMO**。

## 动态 App 框架（MicroQuickJS）

> 设计目标：**JS 写小程序，固件不重烧**。脚本调 `sys.ui.*` 自建 LVGL UI，调 `sys.ble.send/onMessage` 与 PC provider 通信，调 `sys.app.saveState/loadState` 落 NVS。

### 三层架构（`dynamic_app/`）

```
                    ┌─────────────────────────────────────┐
                    │  scripts/<app>.js + prelude.js      │ ← 业务脚本（嵌入固件）
                    └────────────────┬────────────────────┘
                                     │ eval
┌───────────────────────────────────┐│┌──────────────────────────────────┐
│ Script Task (Core 0)              │││ UI Task (Core 1, LVGL 唯一线程) │
│  ┌────────────┐  ┌────────────┐   │││ ┌────────────────────────────┐  │
│  │ runtime.c  │→ │ natives.c  │   │││ │ dynamic_app_ui.c/_drain    │  │
│  │ (JSContext)│  │ (sys.* 实现)│  │││ │ (执行 enqueue 来的指令)    │  │
│  └────────────┘  └─────┬──────┘   │││ └─────┬──────────────────────┘  │
│                        │ enqueue  │││       │ apply_style / set_text  │
│                        ▼          │││       ▼                         │
│              ┌──────────────────┐ │││ ┌──────────────────┐            │
│              │  UI 指令队列     │─┼┼┼→│ id↔lv_obj_t 映射 │            │
│              └──────────────────┘ │││ └──────────────────┘            │
└───────────────────────────────────┘│└──────────────────────────────────┘
                                     ▼
                          ┌──────────────────────┐
                          │ dynapp_bridge BLE    │ ←→ PC dynapp_sdk
                          │ (JSON inbox/outbox)  │
                          └──────────────────────┘
```

**红线（与内建页面同源）**：
- LVGL 不是线程安全的 → JS 脚本线程**永远不直接调 LVGL**，只能 enqueue 指令；UI 线程 drain 后执行。
- BLE 回调线程不阻塞、不写 NVS、不碰 LVGL → 走 dynapp_bridge inbox 队列再交给 script task。

### JS 标准库 `prelude.js`（自动注入）

每次 app 启动前 runtime 自动 eval 一次，业务脚本可直接用：

| 全局 | 说明 |
|------|------|
| `VDOM` | 声明式 UI：`h('panel', {...}, [...])` + `mount/find/set/destroy/dispatch/render`（带 diff） |
| `h` | `VDOM.h` 别名 |
| `makeBle('myapp')` | 返回 `{ send, on, onAny, onError, isConnected, appName }`，按 app 名路由 BLE 消息 |

业务脚本最小骨架见 `dynamic_app/scripts/echo.js`。

### 增加一个动态 App 的步骤

1. **写脚本**：在 `dynamic_app/scripts/` 下加 `myapp.js`（用 `VDOM` + `sys.*` API，仅 ES5）
2. **登记**：
   - `dynamic_app/CMakeLists.txt` 的 `EMBED_TXTFILES` 加一行
   - `dynamic_app_registry.c` 的 `g_apps[]` 加一行 `{ "myapp", myapp_js_start, myapp_js_end }`
3. **加菜单入口**：`app/pages/page_menu.c` 调 `page_dynamic_app_prepare_and_switch("myapp")`
4. **（可选）PC 端 provider**：在 `tools/providers/` 下用 `dynapp_sdk.DynappClient` 写后台逻辑，并在 `tools/dynapp_companion.py` 的 `main()` 里 `register_xxx(client)`

### dynapp_bridge BLE 透传管道

- 单一 GATT service `a3a30001-0000-...`，开机注册死，无需运行时增删
- payload 完全透明（应用层用 JSON），单条 ≤200 B（超长应用层自分包）
- inbox 8 槽：满则丢最老（PC 推无 backpressure）；outbox：`sys.ble.send` 失败由 JS 决定重试
- App 切换时清空 inbox，避免上个 app 漏的消息发给下个 app

### 切屏体验：prepare → ready → commit

直接同步切到动态 App 页面会让用户看到组件一个个冒出来。新路径：
1. `page_dynamic_app_prepare_and_switch(name)` 立即返回，不切屏
2. 后台建 off-screen 对象树并启脚本 build UI
3. 脚本调 `sys.ui.attachRootListener` 触发 ready 回调，路由器 `commit_prepared()` 瞬间换屏
4. 800ms 兜底超时强切（防脚本死循环）

## 调试技巧

**查看 LVGL 内存**：
```c
lv_mem_monitor_t mon;
lv_mem_monitor(&mon);
ESP_LOGI(TAG, "LVGL mem: %u/%u  frag=%d%%",
         mon.used_size, mon.total_size, mon.frag_pct);
```

**擦 NVS 复位**：
```bash
idf.py -p COM3 erase-flash
```

**强制配对重来**：通过 `persist_erase_namespace("nimble_bond")` 或直接 `erase-flash`。

## 已知限制 / 待优化

- 通知页面长列表滚动偶发掉帧（LVGL 9.5 已知回归）
- 音乐进度条依赖 PC 端 10s 兜底推送；短于 1s 的 seek 可能看不到
- 触摸灵敏度未做校准，依赖 FT5x06 出厂默认

## 依赖

| 组件 | 版本 | 用途 |
|------|------|------|
| ESP-IDF | v5.4.3+ | SDK |
| lvgl/lvgl | ^9.0.0 | GUI |
| espressif/esp_lcd_touch_ft5x06 | ^1.0.0 | 触摸驱动 |
| esp-mquickjs | — | 动态 App 的 JS 引擎（QuickJS 精简移植） |
| NimBLE (内置) | — | BLE 协议栈 |
| bleak | ≥0.22 | PC 端 BLE |
| customtkinter | 5.x | PC 端 GUI |
| winsdk | ≥1.0.0b10 | Windows 媒体会话（可选） |

## 版本历史

- **v0.8 (2026-04-27)** — 动态 App 框架（MicroQuickJS）：JS 脚本插件 + LVGL 桥（Script/UI 双任务）+ VDOM 标准库 + dynapp_bridge BLE 透传管道 + PC dynapp_sdk/providers；内置 alarm/calc/timer/2048/echo/weather/music 7 个 app；prepare/commit 异步切屏消除中间态
- **v0.7** — Control service 退役：lock/mute 放弃，媒体键（prev/pp/next）迁入 media_service 自管的 NOTIFY char `0x8a5c000d`
- **v0.6 (2026-04-20)** — TTF 运行时渲染中文，取代 LVGL CJK 内置字库；System service（CPU/MEM/DISK/BAT/NET/TEMP）+ 反向请求拆回各业务 service
- **v0.5** — Media service（正在播放副屏）、Control service（反向按钮事件）、desktop_companion 合并客户端
- **v0.4** — NVS 持久化：背光、系统时间、通知环形缓冲
- **v0.3** — Notification service + 通知页面，UI 布局/逻辑分离
- **v0.2** — Weather service + 天气页面，PC 端 CustomTkinter GUI
- **v0.1** — 驱动移植：ST7789 + FT5x06 + LVGL 9.x，时间调节页面

详细过程见 `docs/` 目录各阶段日志。

## 许可证

仅供学习参考。

只需要牢记一条红线：BLE 回调 / GAP event handler 里永远只能做两件事 —— push 到 manager
队列，或调 NimBLE API；不要碰 LVGL、不要阻塞、不要写 NVS。