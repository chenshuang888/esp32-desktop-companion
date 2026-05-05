# ESP32-S3 桌面伴侣 (Desktop Companion)

基于 **ESP-IDF v5.4.3 + LVGL v9.5.0** 的 ESP32-S3 触摸屏桌面伴侣。通过 BLE 与 PC（Python 工具）双向联动，实现时间同步、天气/通知推送、正在播放副屏、反向控制面板等一体化体验。

**v0.8 起内置 MicroQuickJS，支持以 JS 脚本编写的"动态 App"**：固件不重烧即可新增小程序。**v0.9 完成动态 app 平台化** ——「**新增 app 不改任何端的基础设施代码**」，配套 PC 端插件化架构（`tools/companion/plugins/`），加目录即生效。

> **快速跳转**：[快速开始](#快速开始) · [动态 App 开发](docs/动态app开发者指南.md) · [BLE 协议](#ble-协议约定) · [项目结构](#项目结构)

---

## 功能概览

### 内建页面（C 实现，已迁移到 app/apps/ 模块化目录）

| 页面 | 功能 | BLE 方向 |
|------|------|---------|
| 锁屏 (lockscreen) | 默认入口，时钟 + 触摸唤醒 | — |
| 启动器 (launcher) | 九宫格菜单 + 动态 app 入口 | — |
| 时钟 (clock) | 闹钟 / 计时器 / 秒表 / 世界时间 | PC → ESP (CTS 0x1805) |
| 天气 (weather) | 实时温度/湿度/描述 + 反向请求 | PC ↔ ESP (0x8a5c0001) |
| 通知 (notifications) | 滚动列表，10 条环形缓冲，落 NVS | PC → ESP (0x8a5c0003) |
| 音乐 (music) | 双页（list / detail），媒体键回写 | PC ↔ ESP (0x8a5c0007 / 0x8a5c000d) |
| 系统 (system) | CPU/内存/磁盘/网速/温度/电池圆盘 | PC → ESP (0x8a5c0009) |
| 设置 (settings) | 背光、关于、出厂复位 | — |
| 动态 App 宿主 (dynapp_host) | 通用宿主：跑任意 JS 脚本 | dynapp_bridge (0xa3a30001) |

### 动态 App（JS 脚本，无需烧录即可扩展）

| App | 说明 | 是否需要 PC 插件 |
|-----|------|----------------------|
| **hello** | 最小可运行示例（点按钮 + toast） | 否 |
| **pomodoro** | 番茄钟（多级页 + 持久化） | 否 |
| **settings** | 多级页面 Router 验证 app | 否 |
| **doodle** | 涂鸦板（sys.canvas 像素绘图） | 否 |
| **notif** | 通知列表（PC → 设备单向推送） | ✅ `tools/plugins/notif/` |
| **gomoku** | BLE 联机五子棋（双向） | ✅ `tools/plugins/gomoku/`（含 GUI） |
| **tictactoe** | 人机井字棋（PC AI + 监控页） | ✅ `tools/plugins/tictactoe/`（含 GUI） |

详细的开发者指南见 **[docs/动态app开发者指南.md](docs/动态app开发者指南.md)**。

---

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

---

## 项目结构

```
esp32-desktop-companion/
├── main/                       启动入口（main.c → app_main → 各 service init）
│
├── app/                        应用层（UI）
│   ├── app_main.c              UI 任务 + 页面注册 + dynamic_app_ui_drain
│   ├── app_fonts.c             Tiny TTF 字体初始化（中文 + 图标子集）
│   ├── fonts/                  嵌入 TTF（中文 ~3.4MB + Material Symbols ~25KB）
│   ├── icons/                  内建小图标（PNG 资源）
│   ├── ui/                     UI tokens / 通用 widgets
│   └── apps/                   ★ 模块化 app 目录（每个 app 是一个独立模块）
│       ├── lockscreen/         锁屏
│       ├── launcher/           启动器（九宫格）
│       ├── clock/              时钟（含 4 个子页：alarms/timer/stopwatch/world）
│       ├── weather/            天气
│       ├── notifications/      通知列表
│       ├── music/              音乐（list + detail 双页）
│       ├── system/             系统状态圆盘
│       ├── settings/           设置（背光/关于/复位）
│       └── dynapp_host/        动态 app 宿主（prepare/commit 异步切屏）
│
├── framework/                  路由器
│   ├── app_router.c/h          顶层 app 间切换（lockscreen → launcher → clock ...）
│   └── sub_router.c/h          单 app 内多级页面（如 clock 内 alarms ↔ alarm_edit）
│
├── drivers/                    硬件抽象层
│   ├── board_config.h          引脚集中定义
│   ├── lcd_panel.c/h           ST7789 + 背光 PWM
│   ├── touch_ft5x06.c/h        FT5x06 I2C 触摸
│   ├── lvgl_port.c/h           LVGL 移植（双缓冲 40 行）
│   └── ble_driver.c/h          NimBLE 协议栈 + GAP + SUBSCRIBE 分发
│
├── services/                   业务服务层（GATT 接入 + manager 数据管理）
│   ├── time_service.c          CTS 时间同步
│   ├── weather_service.c       天气推送 + 反向请求
│   ├── notify_service.c        通知推送 + 落 NVS
│   ├── media_service.c         正在播放 + 媒体键 NOTIFY
│   ├── system_service.c        PC 系统状态推送
│   ├── dynapp_bridge_service.c 动态 app JSON 透传管道
│   ├── dynapp_upload_service.c 动态 app 包上传协议
│   └── manager/                ← UI 线程独占消费 + 落盘
│       ├── time_manager.c      battery_sim.c     dynapp_mailbox.c
│       ├── weather_manager.c   device_stats.c    dynapp_upload_manager.c
│       ├── notify_manager.c    media_manager.c   playlist_manager.c
│       └── system_manager.c
│
├── storage/                    存储抽象
│   ├── nvs/                    persist.c + 各 service 的 NVS 适配
│   └── littlefs/               动态 app 脚本仓 + fs_worker
│
├── dynamic_app/                动态 App 框架（MicroQuickJS + LVGL 桥）
│   ├── dynamic_app.c           控制层：script_task 主循环 + 生命周期
│   ├── dynamic_app_runtime.c   引擎层：JSContext 创建/销毁/eval
│   ├── dynamic_app_natives.c   API 层：sys.* native fn
│   ├── dynamic_app_registry.c  注册表：app id → JS buffer
│   ├── dynamic_app_ui*.c       UI 调度 / 注册表 / 样式
│   └── scripts/                ← JS 脚本（嵌入固件 + LittleFS 动态加载并存）
│       ├── prelude.js          标准库（VDOM / UI 组件 / Router / makeBle）
│       ├── hello_pkg/          最小示例（开发者第一站）
│       ├── pomodoro_pkg/       番茄钟
│       ├── settings_pkg/       多级页验证
│       ├── doodle_pkg/         涂鸦板
│       ├── notif_pkg/          通知（PC 推）
│       ├── gomoku_pkg/         BLE 联机五子棋
│       └── tictactoe_pkg/      人机井字棋
│
├── tools/                      ★ PC 端工具（重构后的标准结构）
│   ├── README.md               总索引（4 个子目录定位 + 启动命令）
│   ├── companion/              桌面伴侣主程序（python -m companion）
│   │   ├── __main__.py / core.py / bus.py / runner.py / tray.py
│   │   ├── plugin_sdk/         ★ 插件作者唯一稳定 API（Plugin + platform/gui 门面）
│   │   ├── plugin_manager.py   扫盘 + 加载 + 路由消息
│   │   ├── providers/
│   │   │   ├── native/         5 个原生 BLE service 配套（time/weather/notify/system/media）
│   │   │   └── dynapp/         bridge_provider（哑总线）+ upload_provider
│   │   ├── platform/           平台能力适配（geoip_weather / smtc / packers / toast / ...）
│   │   └── gui/                Tk GUI（侧边栏 + 5 个内置页 + 自动合入插件页）
│   │
│   ├── plugins/                ★ 动态 app 配套 PC 插件（加目录即生效）
│   │   ├── README.md           §0 判定规则：什么时候才需要写插件
│   │   ├── notif/              监听 Win 通知 → BLE 推送
│   │   ├── weather/            HTTP 抓天气
│   │   ├── gomoku/             联机棋盘 + GUI 页
│   │   └── tictactoe/          人机 AI + 监控 GUI 页
│   │
│   ├── dynapp_sdk/             外部脚本用 BLE 客户端库 + examples/
│   ├── dynapp_uploader/        外部脚本用 .pkg 上传客户端库
│   ├── scripts/                构建期工具（gen_font_subset / make_pack_manifest / ...）
│   └── requirements.txt
│
├── docs/                       开发文档
│   ├── 动态app开发者指南.md     ★ 单一入口，30 分钟入门 + API 速查 + 15 坑速查
│   ├── 动态app双端通信协议.md
│   ├── 当前的三层架构框架图.md
│   └── *_工作日志.md            各阶段重构记录
│
├── partitions.csv              16MB 自定义分区
└── sdkconfig.defaults
```

### 分层依赖

```
main
 └─► app ──┬──► app_router / sub_router
           │
           ├─► services ──► drivers
           │   (service 自管 GATT 注册；通过 ble_driver_get_conn_handle 发 notify)
           │
           ├─► services
           │   manager (UI 线程单写)  ←──  service (BLE host 线程，仅 push 队列)
           │   持久化由 storage 层统一封装
           │
           └─► dynamic_app ──► services (dynapp_bridge / dynapp_upload)
                                ↓
                            scripts/<app>/main.js  (JS 业务)
```

**两条铁律**：
1. **services → drivers 单向依赖**。BLE 回调从不阻塞、从不访问 LVGL、从不写 NVS。
2. **LVGL 不是线程安全的** → JS 脚本线程**永远不直接调 LVGL**，只能 enqueue 指令；UI 线程 drain 后执行。

---

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
>
> 第一次烧录或更换分区表后需要 `idf.py erase-flash`，否则 LittleFS 因 magic 不匹配挂载失败。

### PC 端伴侣

```bash
cd tools
pip install -r requirements.txt

# 启动桌面伴侣（GUI 模式，默认；含侧边栏 + 自动加载所有 plugins）
python -m companion

# 后台模式（无窗口）
python -m companion --no-gui

# 指定设备名
python -m companion --device ESP32-S3-DEMO
```

依赖：`bleak`（BLE）、`customtkinter`（GUI）、`winsdk`（Windows 媒体会话，可选）、`requests`（HTTP）、`psutil`（系统状态）。

### 动态 App 开发

打开 **[docs/动态app开发者指南.md](docs/动态app开发者指南.md)**——30 分钟跑通 hello world。

最小新 app：

```bash
# 1. 拷贝模板
cp -r dynamic_app/scripts/hello_pkg dynamic_app/scripts/my_pkg

# 2. 修改 manifest.json + main.js

# 3. 生成 manifest（也可手写）
python tools/scripts/make_pack_manifest.py dynamic_app/scripts/my_pkg \
    --id my_pkg --name 我的应用 --icon STAR --color ACCENT

# 4. 启动 PC 伴侣 → 上传页推送 → 设备菜单出现新 app
python -m companion
```

需要 PC 插件配套？再加一个目录：

```bash
mkdir tools/plugins/my && touch tools/plugins/my/plugin.py
# 写 ~30 行 Python，重启 python -m companion 加载
```

> **判定规则**：纯本地 app 不需要 PC 插件。判定标准见 `tools/plugins/README.md` §0。

### 重新生成中文字体子集

新增中文文案时：

```bash
python tools/scripts/gen_font_subset.py
idf.py build   # 重新嵌入
```

---

## BLE 协议约定

所有自定义特征值的 payload 均为 `__attribute__((packed))` 结构体，PC 端 Python 用 `struct.pack` 严格对齐：

| UUID 片段 | 方向 | 结构 | 字节 |
|-----------|------|------|------|
| 0x2A2B (标准 CTS) | PC ↔ ESP | `<HBBBBBBBB` / 1B seq | 10 / 1 |
| 8a5c0002 weather | PC → ESP | `<hhhBBI24s32s` | 68 |
| 8a5c0004 notify | PC → ESP | `<IBB2x32s96s` | 136 |
| 8a5c0008 media | PC → ESP | `<BBhhHI48s32s` | 92 |
| 8a5c000a system | PC → ESP | `<BBBBBBhIHH` | 16 |
| 8a5c000b weather-req | ESP → PC (NOTIFY) | 1B seq | 1 |
| 8a5c000c system-req | ESP → PC (NOTIFY) | 1B seq | 1 |
| 0x8a5c000d media-btn | ESP → PC (NOTIFY) | `<BBH` (id/action/seq) | 4 |
| 0xa3a30002 dynapp rx | PC → ESP (WRITE) | 透传 JSON 字符串 | ≤200 |
| 0xa3a30003 dynapp tx | ESP → PC (NOTIFY) | 透传 JSON 字符串 | ≤200 |
| 0x8a5c00ee dynapp upload | PC → ESP (WRITE) | 自定义封包（BEGIN/CHUNK/END/DELETE/...） | ≤200 |

反向请求（ESP → PC 请求补推数据）由各业务 service 自管独立 NOTIFY char：订阅上升沿或页面 enter 触发一次 1B seq notify，PC 收到后用同 service 的 WRITE char 回写数据。media-btn 承担"屏上按钮 → PC 媒体键"单向事件（id=0 Prev / 1 PlayPause / 2 Next）。详见 `.trellis/spec/iot/protocol/esp-to-pc-notify-request-pattern-playbook.md`。

设备广播名：**ESP32-S3-DEMO**。

---

## 关键技术

### 中文字体 —— Tiny TTF 运行时渲染

LVGL 自带的 CJK 字库仅 1118 字，覆盖不全。项目改为：
- 用 `tools/scripts/gen_font_subset.py` 扫描源码提取用到的汉字，生成 `srhs_sc_subset.ttf`（~3.4MB）
- 同时用 `gen_icons_subset.py` 生成 Material Symbols 图标子集（~25KB）
- 通过 `EMBED_FILES` 嵌入固件镜像
- 启动时 `lv_tiny_ttf_create_data_ex` 创建多组字体（text/title/huge + icon24/icon36）
- Fallback 链挂 `lv_font_montserrat_*`，解决 LVGL 内置图标
- 开启 `LV_USE_CLIB_MALLOC` 让大块 glyph cache 自动落到 PSRAM

### 内存布局

- `CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=16384` — 小分配走内部 RAM（性能），大块自动进 PSRAM
- `CONFIG_SPIRAM_MALLOC_RESERVE_INTERNAL=32768` — 保留 32KB 给 DMA / ISR 等不能用 PSRAM 的场景
- `CONFIG_BT_NIMBLE_MEM_ALLOC_MODE_EXTERNAL=y` — BLE 堆走 PSRAM，释放内部 RAM
- LVGL 双缓冲：240×40×2×2 ≈ 38KB

### 分区表（16MB）

```
nvs       0x9000    24KB        WiFi/BT 配对、settings、notifications 快照、dynamic app saveState
phy_init  0xf000    4KB         RF 校准
factory   0x10000   6MB         主应用（含嵌入 TTF + 图标）
storage   auto      ~9.9MB      LittleFS：动态 App 脚本仓 + fs_worker 沙箱（subtype=0x83）
```

> ⚠️ subtype `0x83` 是 LittleFS 的私有标记，IDF 不识别字面量。从 SPIFFS 切到 LittleFS 后必须 `erase-flash` 一次。

### Service / Manager 解耦模式

每个数据通道分为两半：

- **`*_service.c`** — GATT 接入层，跑在 NimBLE host 线程，**只**负责校验长度后 `push()` 入队。
- **`manager/*_manager.c`** — 数据管理层，UI 线程独占消费，更新快照、维护版本号。

BLE 回调从不阻塞、从不访问 LVGL、从不写 NVS。队列满时丢弃最旧数据（天气/通知/媒体越新越好）。需要持久化的 manager 由 UI 线程按 dirty + 防抖策略 `tick_flush` 单写落盘——避免多线程写 NVS。

---

## 动态 App 平台（v0.8 / v0.9）

### 三层职责（设备端 + PC 端对称）

```
                    设备端                                PC 端
        ┌──────────────────────────┐         ┌──────────────────────────┐
        │ scripts/<app>/main.js    │ ←JSON→  │ plugins/<app>/plugin.py  │
        │ + manifest.json          │  BLE    │ + (gui_page.py 可选)      │
        │ ★ 业务作者只动这层 ★    │         │ ★ 业务作者只动这层 ★    │
        └──────────┬───────────────┘         └─────────┬────────────────┘
                   │ 调                                 │ 调
                   ▼                                    ▼
        ┌──────────────────────────┐         ┌──────────────────────────┐
        │ scripts/prelude.js        │         │ companion.plugin_sdk     │
        │ - VDOM (h/mount/set/...)  │         │ - Plugin 基类             │
        │ - UI (card/listRow/...)   │         │ - .platform re-export     │
        │ - Router (push/pop/...)   │         │ - .gui re-export          │
        │ - makeBle / setTimeout    │         │   ★ 稳定接口契约 ★      │
        └──────────┬───────────────┘         └─────────┬────────────────┘
                   │ 调                                 │ 调
                   ▼                                    ▼
        ┌──────────────────────────┐         ┌──────────────────────────┐
        │ sys.* native              │         │ companion (主程序)        │
        │ + LVGL UI 队列            │         │ providers / gui / platform│
        │ + LittleFS / NVS          │         │ ★ 业务从不直接 import ★ │
        └──────────────────────────┘         └──────────────────────────┘
```

### 增加一个动态 App = 拷一个目录

| 场景 | 改动 |
|---|---|
| 纯本地 app（番茄钟、计算器） | 新增 `dynamic_app/scripts/<app>_pkg/` |
| 需要 PC 配合（联网/系统 API） | 上 + 新增 `tools/plugins/<plugin>/` |

**0 行 C 代码改动 / 0 行 prelude / 0 行 PC 主程序 / 0 行 SDK** —— 这是 v0.9 平台化重构后达成的硬指标。

### dynapp_bridge BLE 透传管道

- 单一 GATT service `a3a30001-0000-...`，开机注册死，无需运行时增删
- payload 完全透明（应用层用 JSON），单条 ≤200 B（超长应用层自分包）
- inbox 8 槽：满则丢最老（PC 推无 backpressure）
- App 切换时清空 inbox，避免上个 app 漏的消息发给下个 app

### 切屏体验：prepare → ready → commit

直接同步切到动态 App 页面会让用户看到组件一个个冒出来。新路径：
1. `dynapp_host_prepare_and_enter(app_name)` 立即返回，不切屏
2. 后台建 off-screen 对象树并启脚本 build UI
3. 脚本调 `sys.ui.attachRootListener` 触发 ready 回调，路由器 `commit_prepared()` 瞬间换屏
4. 800ms 兜底超时强切（防脚本死循环）

详见 `app/apps/dynapp_host/pages/page_dynapp_host.c`。

---

## 调试技巧

**查看 LVGL 内存**：
```c
lv_mem_monitor_t mon;
lv_mem_monitor(&mon);
ESP_LOGI(TAG, "LVGL mem: %u/%u  frag=%d%%",
         mon.used_size, mon.total_size, mon.frag_pct);
```

**串口看动态 app 日志**：
```bash
idf.py -p COM3 monitor
# 找 [dynapp] 标签的输出（业务 sys.log 全在这里）
```

**擦 NVS 复位 / 切分区表**：
```bash
idf.py -p COM3 erase-flash
```

**强制配对重来**：通过 `persist_erase_namespace("nimble_bond")` 或直接 `erase-flash`。

**PC 插件改了不生效**：插件**修改后**必须重启 `python -m companion`（不支持 hot reload）。新增插件目录可以点"刷新插件"按钮热加载。

---

## 已知限制 / 待优化

- 通知页面长列表滚动偶发掉帧（LVGL 9.5 已知回归）
- 音乐进度条依赖 PC 端 10s 兜底推送；短于 1s 的 seek 可能看不到
- 触摸灵敏度未做校准，依赖 FT5x06 出厂默认
- 动态 app 不支持 setTimeout 之外的高级定时（无 RAF / 无 microtask 调度）
- PC 插件不支持代码热重载（修改后必须重启主程序）

---

## 依赖

| 组件 | 版本 | 用途 |
|------|------|------|
| ESP-IDF | v5.4.3+ | SDK |
| lvgl/lvgl | ^9.0.0 | GUI |
| espressif/esp_lcd_touch_ft5x06 | ^1.0.0 | 触摸驱动 |
| esp-mquickjs | — | 动态 App 的 JS 引擎（QuickJS 精简移植） |
| joltwallet/littlefs | ^1.x | 动态 app 脚本仓 |
| NimBLE (内置) | — | BLE 协议栈 |
| bleak | ≥0.22 | PC 端 BLE |
| customtkinter | 5.x | PC 端 GUI |
| winsdk | ≥1.0.0b10 | Windows 媒体会话（可选） |
| psutil | ≥5.x | 系统状态采集 |
| requests | ≥2.x | HTTP（天气 / archive.org） |

---

## 版本历史

- **v0.10 (2026-05-04)** — Marketplace 集成：companion 加「市场」侧边栏，浏览 [esp32-marketplace](../esp32-marketplace/) 上的动态 app，**一键安装/卸载/更新**。
  - 新增 `tools/companion/marketplace/`：HTTP 客户端 + .mpkg 解析 + plugin 装载 + registry
  - 新增 GUI 页 `tools/companion/gui/pages/marketplace.py`
  - 安装链路复用现有 `UploadProvider` + `UploaderClient.upload_app_pack`，零修改
  - 已装清单落 `tools/plugins/.marketplace_meta/_registry.json`
  - 详见 [docs/Marketplace集成_工作日志.md](docs/Marketplace集成_工作日志.md)
- **v0.9 (2026-05-03)** — 动态 App 平台化达成「**新增 app 不改任何端的基础设施**」：
  - PC 端三层边界整理：`shared/` → `platform/`，`plugin_sdk` 升级为包加 `platform/gui` 门面
  - 删孤儿插件 `music_proxy`，`tools/scripts/` 收纳构建期工具
  - 设备端 `UI.card` 默认值修复（潜伏的 layout bug），新增 `sys.size.CONTENT`
  - prelude SDK 改进：`setTimeout` / `ble.on(type, fn)` 收 body / `pillBtn` 暴露 `textId` / `statusBar` 加 `compact`
  - 新增 hello / pomodoro / tictactoe 共 3 个 app（含 PC AI 插件 + 监控 GUI）
  - 写《动态 app 开发者指南》单一入口文档（809 行）
- **v0.8 (2026-04-27)** — 动态 App 框架（MicroQuickJS）：JS 脚本插件 + LVGL 桥（Script/UI 双任务）+ VDOM/Router 标准库 + dynapp_bridge BLE 透传管道
- **v0.7** — Control service 退役：媒体键迁入 media_service 自管的 NOTIFY char
- **v0.6 (2026-04-20)** — TTF 运行时渲染中文 + System service + 反向请求拆回各业务 service
- **v0.5** — Media service（正在播放副屏）+ desktop_companion 合并客户端
- **v0.4** — NVS 持久化：背光、系统时间、通知环形缓冲
- **v0.3** — Notification service + 通知页面，UI 布局/逻辑分离
- **v0.2** — Weather service + 天气页面，PC 端 CustomTkinter GUI
- **v0.1** — 驱动移植：ST7789 + FT5x06 + LVGL 9.x，时间调节页面

详细过程见 `docs/` 目录各阶段日志。

---

## 文档导航

| 文档 | 内容 |
|---|---|
| **[docs/动态app开发者指南.md](docs/动态app开发者指南.md)** | 写动态 app 必读：30 分钟入门 + API 速查 + 15 坑 |
| [docs/当前的三层架构框架图.md](docs/当前的三层架构框架图.md) | VDOM / sys.* / C drain 三层数据流 |
| [docs/动态app双端通信协议.md](docs/动态app双端通信协议.md) | bridge JSON 协议契约 |
| [docs/动态app_包格式规范.md](docs/动态app_包格式规范.md) | manifest.json 字段约束 |
| [tools/README.md](tools/README.md) | PC 端 4 个子目录定位 + 启动命令 |
| [tools/plugins/README.md](tools/plugins/README.md) | 插件作者指南（§0 判定规则） |
| `docs/*_工作日志.md` | 历次重构决策与踩坑记录 |

---

## 许可证

仅供学习参考。

---

> **核心红线**：BLE 回调 / GAP event handler 里永远只能做两件事 —— push 到 manager 队列，或调 NimBLE API；不要碰 LVGL、不要阻塞、不要写 NVS。
>
> **平台承诺**：新增动态 app（含必要的 PC 配套）= 加一个或两个目录 + 跑一次上传命令。如果发现自己在改 dynamic_app/dynamic_app_*.c / prelude.js / app/apps/dynapp_host / tools/companion/* 任何一个文件，**先停下来确认**：99% 的情况是用错 API 而不是平台缺能力。
