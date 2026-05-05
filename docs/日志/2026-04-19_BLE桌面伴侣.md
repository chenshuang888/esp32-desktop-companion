# BLE 桌面伴侣开发日志

**日期：** 2026-04-19
**分支：** `feat/more_ble_services`

## 任务概述

前一阶段搞定了 NVS 持久化，本期把 ESP32 从"被动信息终端"升级为**桌面伴侣**：新增两个 GATT service 形成完整双向闭环。

**目标：**
- 打破之前"PC 推、ESP 被动收"的单向模式
- 建立 **ESP → PC** 的主动事件通道（按钮事件 GATT Notify）
- 建立 **PC → ESP** 的媒体状态通道（曲目信息 GATT Write）
- 屏上既能看又能控：Music 页显示 Windows 当前曲目 + 进度条 + 三按钮控制
- PC 端一个脚本打通全部链路

---

## 阶段一：角色反转 —— 第一个 ESP → PC 通道

### 设计决策：继续用 GATT Notify 而不是 HID 或 Broadcast

之前 time / weather / notify 三个 service 清一色 PC Write → ESP 收，ESP 从来没主动发起过任何通信。想让触摸按钮操控 PC，三条路可选：

| 方案 | 代价 | 收益 |
|---|---|---|
| **BLE HID** | 写 HID Report Map，调试方式换一套 | 系统原生识别，零 PC 脚本 |
| **Broadcast/Beacon** | 协议完全换一套 | 不需要连接、一对多 |
| **自定义 GATT Notify** | 新 characteristic + PC 端 start_notify | 复用已有连接 |

选第三条：改动最小、复用已有 NimBLE 套路、PC 端就加 3 行代码。HID 留给"成品化"阶段做。

### `control_service`（UUID `8a5c0005` / `8a5c0006`）

- Characteristic flags = `READ | NOTIFY`（READ 是给客户端发现用的占位）
- Payload 8 字节：
  ```c
  typedef struct {
      uint8_t  type;      // 0=button (预留 1=slider)
      uint8_t  id;        // 0=Lock 1=Mute 2=Prev 3=Next 4=PlayPause
      uint8_t  action;    // 0=press (预留 long_press / release)
      uint8_t  _reserved;
      int16_t  value;
      uint16_t seq;       // 单调递增，PC 端去重
  } __attribute__((packed)) control_event_t;
  ```

### 关键决策：不写 `control_manager`

前三个 service 都配了 manager 做 queue 解耦。为什么 control 不需要？

**方向反了**：

| Service | 生产者线程 | 消费者线程 | 跨线程？ | 需要 manager？ |
|---|---|---|---|---|
| weather / notify / time | NimBLE host (write cb) | UI (LVGL) | 是 | ✅ |
| **control** | **UI (触摸回调)** | **NimBLE host (radio)** | **否\*** | **否** |

\* `ble_gatts_notify_custom` 本身线程安全（NimBLE 内部有 `ble_hs_lock`），从任意任务调用都行。整个项目里只有 UI 任务这一个线程触发 control，天然 single-writer。硬加一层 manager 只是在 NimBLE 的内部队列外面套一个空队列，违反奥卡姆剃刀。

---

## 阶段二：组件循环依赖 —— `ble_conn` 中介层的由来

### 问题

`control_service` 要发 notify 必须传 `conn_handle`。这个 handle 由 NimBLE 在 `BLE_GAP_EVENT_CONNECT` 事件里给出，当前存在 `ble_driver.c` 的 static 变量里。

最直观做法：在 `ble_driver.h` 加 `ble_driver_get_conn_handle()`，让 `control_service.c` include 调用。

**撞墙**：CMake 循环依赖。

```
drivers REQUIRES services  ← ble_driver.c 要调 time_service_init 等
services REQUIRES drivers  ← 如果新加这条，就成环
```

### 解决：`services/ble_conn.h/c`

把"当前连接状态"这一小块共享状态**下沉到 services 组件**，drivers 主动上报、services 内部的任何模块都能读。

```
drivers/ble_driver.c            services/ble_conn.c             services/control_service.c
─────────────────────                                            ─────────────────────────
GAP CONNECT ──ble_conn_set()──▶ [volatile bool + uint16_t]  ──get_handle()──▶ notify_custom
GAP DISCONNECT                                                   (还有未来的 media/HID...)
```

依赖方向保持单向 drivers → services。

**类比**：`ble_driver` 是"新闻发布者"，`ble_conn` 是"公告栏"，`control_service` 是"读者"。这种"共享状态提升"模式在后面接入 media_service 时省了一次重复设计 —— 任何 service 想发 notify 都直接 `ble_conn_get_handle()`，不用每个 service 自己跟踪 GAP 事件。

30 行代码换一个干净的依赖图，真香。

---

## 阶段三：PC 端订阅 + 按钮映射

### `tools/control_panel_client.py`（独立版）

用 bleak 订阅 control characteristic 的 notify，按钮 id 经 ctypes 调 user32 执行：

| id | 动作 | API |
|---|---|---|
| 0 | Lock | `user32.LockWorkStation()` |
| 1 | Mute | `keybd_event(VK_VOLUME_MUTE)` |
| 2 | Prev | `keybd_event(VK_MEDIA_PREV_TRACK)` |
| 3 | Next | `keybd_event(VK_MEDIA_NEXT_TRACK)` |
| 4 | PlayPause | `keybd_event(VK_MEDIA_PLAY_PAUSE)` |

关键细节：
- `seq` 去重：相同 seq 被过滤，避免 bleak 偶尔重投同一包时动作被执行两次
- `--dry-run` 开关：只打印不执行，联调首轮用
- 断线 3s 自动重连

---

## 阶段四：反向通道 —— 音乐副屏

### 发现的闭环断点

control 跑起来后点 Prev/Next 确实能切歌，但**屏上完全没反馈**"切到哪首了"。必须加 PC → ESP 的曲目推送 service，形成"看 + 控"闭环。

### `media_service`（UUID `8a5c0007` / `8a5c0008`）

- Characteristic flags = `WRITE`
- Payload 92 字节：

  ```c
  typedef struct {
      uint8_t  playing;                       // 0=paused 1=playing
      uint8_t  _reserved;
      int16_t  position_sec;                  // -1 未知
      int16_t  duration_sec;                  // -1 未知 / 直播
      uint16_t _pad;
      uint32_t sample_ts;                     // PC 采样时 unix sec
      char     title[48];
      char     artist[32];
  } __attribute__((packed)) media_payload_t;
  ```

沿用 weather_manager 的 queue + latest 模式。

### 核心技术点：进度条本地插值

PC 不能每秒推一次（带宽浪费 + 触发 LCD 重绘）。方案：**PC 只在状态变化时推，ESP 本地 esp_timer 跑秒表**。

```c
int16_t media_manager_get_position_now(void) {
    if (!s_has_data || s_latest.position_sec < 0) return -1;
    if (!s_latest.playing) return s_latest.position_sec;

    int64_t elapsed = esp_timer_get_time() - s_received_at_us;
    int32_t pos = s_latest.position_sec + (int32_t)(elapsed / 1000000);
    if (s_latest.duration_sec > 0 && pos > s_latest.duration_sec)
        pos = s_latest.duration_sec;
    return (int16_t)pos;
}
```

PC 推送时机：
- 曲目切换（MediaPropertiesChanged）
- 播放/暂停（PlaybackInfoChanged）
- seek（TimelinePropertiesChanged）
- 兜底每 10 秒校准一次（防事件遗漏）

UI `update()` 每帧调 `get_position_now` → 进度条丝滑前进，BLE 流量极低。

### page_music：看 + 控一体

一个页面承担两个职能：
- **显示**：title / artist / lv_slider 进度条 / "mm:ss / mm:ss" 时间
- **控制**：三个 60×60 圆形按钮，分别绑 `control_service` 的 id=2/4/3

Play/Pause 按钮的图标随 `latest.playing` 字段在 `▶` / `⏸` 间切换，形成 **round-trip 闭环**：

```
屏按钮 → BLE notify → PC → VK_MEDIA_PLAY_PAUSE → 播放器状态切换
     → SMTC PlaybackInfoChanged → publisher 推新 payload → 屏图标切换
```

实测延迟 100-300 ms，肉眼能察觉但可接受。

### 双去重策略

`page_music` 的 `update()` 每 10ms 调一次，不能每次都重画 title/artist（性能 + 闪烁）：

```c
static void page_music_update(void) {
    uint32_t ver = media_manager_version();
    if (ver != s_ui.last_version) {
        s_ui.last_version = ver;
        refresh_static(m);   // 只在 version 变化时重绘 title/artist/图标
    }
    refresh_progress(m);     // 进度条按差值去重，内部记 last_pos_sec
}
```

---

## 阶段五：PC 端合并 —— `desktop_companion.py`

### 起初两个独立脚本

- `control_panel_client.py`: 订阅 notify 做 Windows 动作
- `media_publisher.py`: 订阅 Windows 媒体会话 + write

实测两个进程同时 connect 同一台 ESP32 在 Windows WinRT BLE 栈上**不稳定**（文档说允许共享连接，实际经常打架）。用户反馈后合并。

### 合并原则

同一个 `BleakClient` 同时承载 notify + write。bleak 支持这种用法：

```python
async def run_session():
    async with BleakClient(device) as client:
        # 1) 订阅控制按钮（ESP → PC）
        await client.start_notify(CONTROL_CHAR_UUID, ctrl.on_notify)
        # 2) 启动媒体推送（PC → ESP）
        pub = MediaPublisher(client, loop)
        await pub.start()
        # 3) 兜底定期全量同步
        while client.is_connected:
            await asyncio.sleep(10)
            await pub.push_now()
```

保留两个独立脚本作为调试备胎（单边联调时用），合并版为日常使用入口。

---

## 阶段六：winsdk 踩过的大坑

### 坑 1：COM 线程回调 vs asyncio loop

`winsdk.GlobalSystemMediaTransportControlsSessionManager` 的事件回调（`add_*_changed`）运行在 **COM 线程**，不是 asyncio loop。直接在里面 `await` 会 RuntimeError。

**修复**：`asyncio.run_coroutine_threadsafe(coro, loop)` 把回调桥回 bleak 的事件循环。

```python
def _on_any_changed(self, *_):
    asyncio.run_coroutine_threadsafe(self.push_now(), self._loop)
```

### 坑 2：session 切换时的 token 泄漏

用户从浏览器切到桌面客户端，当前 session 对象会变。如果不 remove 旧 session 的事件 token，旧事件仍会触发；新 session 又要重新 add。

**修复**：维护一个 `_session_tokens: list[(obj, event_name, token)]`，在 `_rebind_session` 开头 `remove_*`，结尾 `add_*`。

### 坑 3：UTF-8 截断产生 `�`

原版 `_utf8_fixed` 只处理末尾 continuation byte：

```python
while b and (b[-1] & 0xC0) == 0x80:   # 只剥 10xxxxxx
    b = b[:-1]
```

漏了这种情况：最后一字节是 leading byte（11xxxxxx），后面的 continuation bytes 已被切掉。此时 decode 会得到 `�`。

**修复**：`try/decode` 一字节一字节回退：

```python
while b:
    try:
        b.decode("utf-8")
        break
    except UnicodeDecodeError:
        b = b[:-1]
```

### 坑 4：同一次切歌触发 3 个事件 × N 回合

winsdk 一次曲目切换会触发 `MediaPropertiesChanged` + `PlaybackInfoChanged` + `TimelinePropertiesChanged`，每个事件还可能冒几次。结果单次切歌能把同一 payload 推送 7 次。

**修复**：`MediaPublisher` 缓存 `_last_payload: bytes`，bytes-wise 相等则跳过 write。兜底 10s 同步也会被静默跳过（相同内容无需重推），重连时新实例 `_last_payload = None` 必推首次。

---

## 阶段七：QQ 音乐网页版适配问题（最终锁定为**非我方问题**）

### 现象

用户播放 QQ 音乐网页版，屏上显示：
- title 一直是"QQ音乐-千万正版音乐海量无损曲库..."（截断末尾有 `�`）
- artist 永远空字符串
- 进度条能走、按钮控制**能**生效（Next/PlayPause 都响应）

### 根因

Windows SMTC 是"**播放器主动上报**"协议，不是进程偷窥。页面需要：

```javascript
navigator.mediaSession.metadata = new MediaMetadata({
    title: '红色高跟鞋',
    artist: '蔡健雅',
});
```

QQ 音乐网页版**没调**这个 API，浏览器（Edge/Chrome）只好拿 `document.title` 作 fallback。

### 验证方法

按 **Win + A** 打开 Windows 操作中心，顶部的媒体控制卡片显示的就是 SMTC 数据源。QQ 音乐网页版在那里也是"QQ音乐-千万正版..."+空艺术家 —— 证明数据从源头就不对，不是我们的脚本问题。

### 解决

换桌面客户端。用户装 QQ 音乐桌面版后：**title / artist / 进度条 / 媒体键响应全部正确**。Spotify / 网易云桌面版 SMTC 实现也都完整。

---

## 架构决策：`control_service` 与 `media_service` 不合并

用户提议合并。讨论结论：**保持分离**。

| 维度 | control_service | media_service |
|---|---|---|
| 方向 | ESP → PC (Notify) | PC → ESP (Write) |
| 职责 | 通用按钮事件通道 | 曲目状态显示通道 |
| 未来扩展 | Brightness / Shortcut / Beacon / HID | 专辑封面 / 歌词 |

control_service 是 **通用输入设备通道**，Lock / Mute 本来就和音乐无关。未来加亮度滑条、快捷键面板这些也都归它。合进 media 语义就扭曲了。

各 80 行代码，维护成本近零；合并收益小于拆分收益比。

---

## 关键文件清单

**新增（9 个）：**
- `services/ble_conn.h` / `.c` — 共享连接状态中介
- `services/control_service.h` / `.c` — GATT Notify（ESP → PC 按钮事件）
- `services/media_service.h` / `.c` — GATT Write（PC → ESP 媒体推送）
- `services/media_manager.h` / `.c` — queue + latest + 进度插值
- `app/pages/page_control.h` / `.c` — 2×2 按钮面板
- `app/pages/page_music.h` / `.c` — 音乐副屏（看 + 控一体）
- `tools/control_panel_client.py` — PC 端订阅（独立版）
- `tools/media_publisher.py` — PC 端媒体推送（独立版）
- `tools/desktop_companion.py` — PC 端合并版（日常使用入口）

**修改（10 个）：**
- `drivers/ble_driver.c` — GAP CONNECT/DISCONNECT 上报 `ble_conn_set`，init 追加 media/control_service_init
- `framework/page_router.h` — enum 加 `PAGE_CONTROL` / `PAGE_MUSIC`
- `main/main.c` — 调 `media_manager_init`
- `app/app_main.c` — include + register + `ui_task` 里 `media_manager_process_pending`
- `app/pages/page_menu.c` — 新增 Controls / Music 菜单项；卡片 scrollable 恢复
- `services/CMakeLists.txt` — SRCS 加 ble_conn/control_service/media_service/media_manager
- `app/CMakeLists.txt` — SRCS 加 page_control/page_music；REQUIRES 加 services
- `tools/requirements.txt` — 加 `winsdk>=1.0.0b10`

---

## 经验教训

1. **方向反转不需要改 BLE 角色**：peripheral/central 关系不变，只是 characteristic 的 flags 从 WRITE 换成 NOTIFY，客户端加一步 `start_notify` 订阅。GATT 的双向性常被低估。

2. **`conn_handle` 下沉是解组件循环依赖的模板招式**：以后任何 "drivers 私有状态被多个 services 需要" 的场景都可以复用这种共享中介模式。中介层代码极小（30 行），但换来干净的依赖图。

3. **本地插值 + 事件驱动 push 是带宽/平滑度的最优平衡**：PC 推得越少、ESP 显示越平滑。核心是传 `sample_ts` 让接收方知道从哪个时刻开始插值。

4. **Windows SMTC 是协议不是偷窥**：播放器必须主动调 Media Session API 才会被看到。桌面客户端基本都实现，网页播放器看具体站点。锅在播放器不是脚本 —— 调试时先用 Spotify / 桌面客户端"定基线"，确认脚本无误，再 back 到目标播放器。

5. **WinRT BLE 多进程共享连接在实践中不靠谱**：一个进程能搞定就别起两个。bleak 的 `start_notify` 和 `write_gatt_char` 在同一 client 上完全共存，合并成本极低。

6. **winsdk COM 线程回调必须 `run_coroutine_threadsafe` 桥接**：这是跨运行时的唯一正确做法。直接 await 必崩。

7. **UTF-8 截断要 try/decode 回退，不能只看 continuation byte**：最后一字节可能是 leading byte（11xxxxxx），它本身不是 continuation 但后续 bytes 已被切走，仍然是坏字符。`try/decode` 一字节一字节回退最保险。

8. **single-writer 模式的判断标准是"实际调用者数量"而非"线程安全性"**：即使 API 线程安全，如果只有一个任务会调它，仍然不需要 queue。反之多消费者场景哪怕 API 线程安全也要 queue 做背压。control 符合前者。

---

## 后续可做

- **OTA 固件更新 service**：无线刷机，免 USB
- **HID 升级**：`control_service` 升级为系统级蓝牙键盘 / 媒体控制器，脱离 PC 脚本独立工作
- **专辑封面**：`media_payload` 扩展 thumbnail 字段（320×? 压缩 bitmap + 分包传输协议）
- **歌词滚动**：publisher 抓 LRC，按时间戳推送一句一句的 chunk
- **control_service 扩充**：Brightness 滑条 / StreamDeck 风格快捷键面板（启用 `control_event_t.type=1 slider` + `value` 字段）
- **多播放器优先级**：若同时开 Spotify + YouTube Music，`desktop_companion` 应选哪个（当前是 current_session 默认）
- **清理独立脚本**：确认 `desktop_companion.py` 稳定后删除 `control_panel_client.py` 和 `media_publisher.py`
