# PC 端 BLE 工具链统一重构 工作日志

**日期**：2026-04-29
**分支**：feat/app_fs
**作者**：ChenShuang + Claude

---

## 0. 起因

`tools/` 下散落 4 个独立的 PC 端 BLE 客户端进程，每个都自己 `BleakClient` 一份：

| 文件 | 行数 | 干啥 |
|---|---:|---|
| `desktop_companion.py` | 1037 | CTS 时间 / weather / notify / system / media，纯 CLI |
| `ble_time_sync.py` | 972 | 同上一部分功能，CustomTkinter GUI |
| `dynapp_companion.py` | 65 | 走 dynapp_bridge JSON 通道的 weather/media provider |
| `dynapp_push_gui.py` | 726 | 上传动态 app（CustomTkinter） |

**核心矛盾**：BLE 物理上一个 ESP 同一时刻只能被**一个 PC 进程**连接，4 个工具**永远只能开一个**。结果：
- 想烧动态 app → 必须停 desktop_companion → 时间/天气/通知/媒体全断
- 想用 GUI 推时间 → 必须停 dynapp_companion → 动态 app 那边失联
- 天气拉取代码 3 份、SMTC 媒体监听 3 份、CTS 时间打包 2 份

ESP 端 7 个 BLE service（`time_service` / `weather_service` / `notify_service` /
`system_service` / `media_service` / `dynapp_bridge_service` / `dynapp_upload_service`）
**全部在用，全部不动**。问题完全在 PC 端架构。

---

## 1. 目标

合并成一个 `tools/companion/` 包：
- 一个 BleakClient + 7 个 provider 同时点亮
- CustomTkinter GUI（可关闭，后台继续跑）
- 删掉 4 个老进程

---

## 2. 关键决策

| 议题 | 决策 |
|---|---|
| 进程形态 | GUI + 后台 asyncio 核心分离；GUI 关闭后核心继续；提供 `--no-gui` daemon |
| 老文件 | 直接删 desktop_companion / ble_time_sync / dynapp_companion / dynapp_push_gui / media_publisher |
| 手动通知 | 保留 ble_time_sync 独有的"标题+正文+分类发送"作为新 GUI 一个 page |
| 是否动 ESP 固件 | **不动**，本轮仅 PC 端 |
| Provider 调度 | 连上 ESP 后 7 个 provider **全部同时点亮** |
| 目录布局 | `tools/companion/` 包，`python -m companion` 启动 |

---

## 3. 最终落地的目录结构

```
tools/companion/
├── __main__.py              入口；--no-gui / --device / --log-level
├── __init__.py
├── constants.py             全部 UUID + struct 格式 + 阈值
├── bus.py                   线程安全事件总线
├── runner.py                asyncio 工作线程
├── core.py                  Companion：BleakClient 单例 + 扫描/连接/重连 + provider 注册
├── config.py                ~/.dynapp/companion.json 持久化
├── tray.py                  pystray 托盘（关窗 = withdraw）
│
├── providers/
│   ├── base.py              Provider ABC + ProviderContext
│   ├── time_provider.py     CTS 0x2A2B 推 + req notify 响应
│   ├── weather_provider.py  8a5c0002 写 / 8a5c000b req
│   ├── notify_provider.py   8a5c0004 写 + 内置 ToastWatcher + GUI 手动入口
│   ├── system_provider.py   8a5c000a 1Hz 写 / 8a5c000c req
│   ├── media_provider.py    8a5c0008 写 / 8a5c000d 按钮 notify + SMTC
│   ├── bridge_provider.py   a3a30002/3 dynapp JSON 路由（复用 dynapp_sdk.Router）
│   └── upload_provider.py   a3a40002/3 上传（包装 UploaderClient external_client）
│
├── shared/                  去重层
│   ├── geoip_weather.py     ip-api + open-meteo + WMO 映射（来自 3 处）
│   ├── smtc.py              winsdk SMTC 监听 + 媒体键模拟（来自 3 处）
│   ├── packers.py           CTS 10B / weather 68B / system 16B / media 92B / notify 136B
│   └── toast.py             Windows 通知中心轮询 + 白名单
│
└── gui/
    ├── theme.py             深紫 #14101F / 卡片 #231C3A / 青绿 #06B6D4 / 紫 #9333EA
    ├── widgets.py           StatusDot / Card / KV
    ├── app.py               主窗 + 200px 侧边栏 + page 路由 + 关闭拦截
    └── pages/
        ├── home.py          连接状态卡 + 7 provider 健康灯网格
        ├── upload.py        文件/目录选择 + 进度条 + app 列表
        ├── notify.py        标题/正文/分类 + 历史
        └── log.py           日志 + provider 过滤
```

**总计**：25 个 .py 文件，比原 4 个进程合计精简 ~30%。

---

## 4. 关键架构设计

### 4.1 单 BleakClient

`Companion.client` 持有；provider 通过 `ctx.client` 用，禁止自己 connect/disconnect。
所有 `write_gatt_char` 都经过 `Companion._write_lock` 串行化，避免并发写挤兑 connection
interval。

### 4.2 事件总线

连接成功后，`Companion` 遍历每个 provider 的 `subscriptions()` 一次性
`start_notify(uuid, lambda d,b: bus.emit_threadsafe("notify:"+uuid, b))`，然后
dispatcher 按 uuid 路由到 provider 的 `on_notify`。

```
ESP NOTIFY → BleakClient cb (任意线程)
          → bus.emit_threadsafe("notify:<uuid>", bytes)
          → asyncio loop → provider 的监听器 → 业务处理
```

### 4.3 线程模型（沿用 dynapp_push_gui 已验证模式）

- 主线程：tkinter mainloop
- 工作线程：`runner.AsyncRunner` 跑独立 `asyncio.new_event_loop()`
- GUI → 后台：`runner.submit(coro)` → `asyncio.run_coroutine_threadsafe`
- 后台 → GUI：bus 监听器在 GUI 端用 `root.after(0, cb)` 入队
- **provider 严禁直接 touch tkinter widget**——必走 bus 事件

### 4.4 GUI 关闭核心继续

`tray.py` 用 pystray，`WM_DELETE_WINDOW` 拦截 → `root.withdraw()`；
托盘菜单 [Show / Quit]，Quit 才真退（停 runner + 断 BLE）。
`--no-gui` 模式跳过 GUI，纯 console + Ctrl+C 退出。

### 4.5 上传期间软互斥

`UploadProvider` START 前 `bus.emit("upload:begin")`，结束 `"upload:end"`。
`SystemProvider` / `WeatherProvider` 在 tick 里检查 `ctx.quiesce_during_upload()`
跳过该轮。BLE 协议层不互斥，仅减少带宽挤兑。

### 4.6 UploaderClient 改造

原 `tools/dynapp_uploader/UploaderClient` 自己 connect。新增 `external_client`
注入路径：

```python
UploaderClient(external_client=ctx.client)
# connect() 仅 start_notify(STATUS_UUID)，不 connect/disconnect 物理链路
# disconnect() 仅 stop_notify
```

`upload_provider` 完全复用现有 protocol/state 机器，零重写。

---

## 5. 去重情况

| 重复 | 旧位置（已删） | 新归宿 |
|---|---|---|
| ip-api + open-meteo + WMO 映射 | desktop_companion.WeatherPublisher / ble_time_sync 弱版 / providers/weather_provider | `shared/geoip_weather.py` |
| SMTC 媒体监听 | desktop_companion.MediaPublisher / media_publisher / providers/media_provider | `shared/smtc.py` |
| Windows Toast 轮询 | desktop_companion.ToastPublisher | `shared/toast.py` |
| struct.pack 各 service | 散落各 publisher | `shared/packers.py` |
| 媒体键模拟 (keybd_event) | desktop_companion / providers/media_provider | `shared/smtc.send_media_key()` |

---

## 6. Provider 列表（运行时）

| name | subscriptions | 行为 |
|---|---|---|
| time | `0x2A2B` | 连上立刻推一次；ESP 订阅时 NOTIFY 触发再推 |
| weather | `8a5c000b` | 连上 2s 后推一次；req notify 触发拉新（10min 缓存） |
| notify | — | 监听 bus `notify:manual` (GUI) + ToastWatcher 白名单 |
| system | `8a5c000c` | 1Hz psutil 采样；req notify 立即推一帧；upload 期间跳过 |
| media | `8a5c000d` | SMTC 事件 / 10s 兜底全量同步；按钮事件 → 媒体键 |
| bridge | `a3a30003` | a3a3 JSON 路由 + 内置 weather/music handler；SMTC 主动推 music/state |
| upload | — | 监听 bus `upload:request`，包装 UploaderClient |

---

## 7. 启动方式

```bash
# GUI 模式（默认）
python -m companion

# 后台 daemon（可与系统服务搭配）
python -m companion --no-gui

# 自定义设备名
python -m companion --device "ESP32-S3-DEMO"

# DEBUG 日志
python -m companion --log-level DEBUG
```

依赖（`tools/requirements.txt`）：

```
bleak>=0.22
requests>=2.31
customtkinter>=5.2.0,<6
psutil>=5.9
winsdk>=1.0.0b10; sys_platform == "win32"
pystray>=0.19      ← 新增
Pillow>=10.0       ← 新增（pystray 依赖）
```

---

## 8. 文件改动清单

### 新建（25 个）

`tools/companion/` 全部内容（见 §3）。

### 修改

- `tools/dynapp_uploader/client.py`：增加 `external_client` 参数；`connect()` /
  `disconnect()` 在外部模式下只做 notify 订阅
- `tools/dynapp_uploader/__init__.py`：注释把 GUI 入口指向 `python -m companion`
- `tools/requirements.txt`：新增 `pystray` + `Pillow`

### 删除（7 个 + 1 目录）

- `tools/desktop_companion.py`
- `tools/ble_time_sync.py`
- `tools/dynapp_companion.py`
- `tools/dynapp_push_gui.py`
- `tools/media_publisher.py`
- `tools/providers/weather_provider.py`
- `tools/providers/media_provider.py`
- `tools/providers/`（空目录一并删）

### 保留

- `tools/dynapp_sdk/` —— 仍被 bridge_provider 复用（Router 类）
- `tools/dynapp_uploader/` —— 协议核心
- `tools/dynapp_bridge_test.py` —— CLI 联调备用
- `tools/gen_font_subset.py` / `patch_esp_mquickjs_component.py` —— 构建脚本

---

## 9. 验证

### 9.1 静态校验（已做）

- 25 个 .py 文件 `ast.parse` 全部通过
- 所有模块 `importlib.import_module` 全部通过（包括 GUI / win 模块）
- `python -m companion --help` 正常输出

### 9.2 联调清单（待用户烧录）

1. `python -m companion --no-gui` → 看 log 里 7 个 provider 全 `started`
2. ESP 进入 time / weather / notification / system / music 各页 → 数据正常
3. 烧 dynamic app（如 habit_pkg）：开 Upload 页选目录 → Upload。验证：
   - 进度条流畅
   - upload 期间 system 推送暂停
   - END 后恢复 1Hz
4. GUI 模式 → 关窗 → 托盘出现 → Show 恢复主窗 → 后台日志连续未中断
5. GUI Notify 页发一条手动通知 → ESP 通知页确认收到
6. 动态 app dashboard / weather → 走 a3a3 bridge channel，与老的 8a5c
   weather_service 并存而不冲突

---

## 10. 风险与已知坑

- **7 个 notify 同挂**：NimBLE 默认 MAX_SUBSCRIPTIONS=15 余量足；同 connection
  interval 内多 provider 并发写靠 `_write_lock` 串行化
- **GUI 跨线程**：严格 GUI→runner.submit / 后台→bus + root.after。
  dynapp_push_gui 已踩过的坑全部沿用其模式
- **托盘退出语义**：明确"关窗 = 隐藏；托盘 Quit = 真退"。无 pystray 时降级为
  关窗直退
- **bridge_provider 与老 weather_service 并存**：动态 app 走 a3a3 channel，老的
  `page_weather.c` 走 8a5c000b。两条链路同时活，互不影响

---

## 11. 不在本次范围

- ESP 固件改动（time_service 已存在足够用，未来加 sys.time native API 是下一轮）
- habit / dashboard 用真实时间（依赖下一轮 sys.time native；本轮先把工具链整顺）
- 老 8a5c service 删除（保留并发挥作用，等动态 app 完全取代后再清理）

---

## 12. Commit 顺序建议

每步可单独验证、单独回退：

1. `feat(companion): scaffold package` —— 空骨架 + base.Provider + bus + runner
2. `feat(companion): shared geoip/smtc/packers/toast` —— 公共层
3. `feat(companion): time/weather/notify/system/media providers` —— 5 个原生
4. `feat(companion): bridge provider` —— dynapp_sdk 接入
5. `refactor(uploader): split external_client mode` —— UploaderClient 改造
6. `feat(companion): upload provider` —— 集成上传
7. `feat(companion): GUI shell + pages + tray` —— CustomTkinter
8. `chore: remove legacy tools` —— 删 5 个老文件
9. `chore: update requirements (pystray)` —— 依赖

---

## 13. 一句话总结

把"4 个互斥的 PC 进程 + 各种重复实现"压成"一个 BleakClient + 7 个 provider +
一个 GUI"，从此 ESP 的 7 个 BLE service 在 PC 端永远同时在线，再也不需要在工具
之间切换。
