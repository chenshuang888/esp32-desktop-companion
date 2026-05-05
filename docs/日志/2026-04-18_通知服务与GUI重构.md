# 通知推送服务 + PC 端 GUI 重构开发日志

**日期：** 2026-04-18
**分支：** `feat/more_ble_services`

## 任务概述

在已完成的时间同步（CTS 0x1805）与天气推送（自定义 8a5c0001）基础上，新增第三个 BLE 服务——**通知推送**，并对 PC 端 Tkinter 脚本做全面重构以改善用户体验。

**目标：**
- 固件端：新增通知 GATT 服务，本地环形缓冲最近 10 条，LVGL 屏幕可视化列表
- PC 端：输入标题/内容/类别/优先级推送到 ESP32；重构为现代风格 GUI
- 架构：复用 service + manager 分层模式，BLE 回调不阻塞 UI 线程

---

## 阶段一：BLE 通知推送服务（固件）

### 架构复用

严格沿用已有的三段式模式：

| 层 | 已有参照 | 新增 |
|---|---|---|
| GATT WRITE | `services/weather_service.c` | `services/notify_service.c` |
| 线程安全队列 + 数据管理 | `services/weather_manager.c` | `services/notify_manager.c` |
| UI 页面 | `app/pages/page_weather.c` | `app/pages/page_notifications.c` |

**关键差异**：`weather_manager` 是"最新快照"语义，而 `notify_manager` 需要保留多条历史——改用**环形缓冲 + version 号**。

### 数据结构

```c
// services/notify_manager.h
#define NOTIFY_TITLE_MAX  32
#define NOTIFY_BODY_MAX   96
#define NOTIFY_STORE_MAX  10

typedef enum {
    NOTIFY_CAT_GENERIC  = 0,
    NOTIFY_CAT_MESSAGE  = 1,
    NOTIFY_CAT_EMAIL    = 2,
    NOTIFY_CAT_CALL     = 3,
    NOTIFY_CAT_CALENDAR = 4,
    NOTIFY_CAT_SOCIAL   = 5,
    NOTIFY_CAT_NEWS     = 6,
    NOTIFY_CAT_ALERT    = 7,
} notify_category_t;

typedef struct {
    uint32_t timestamp;
    uint8_t  category;
    uint8_t  priority;
    uint8_t  _reserved[2];
    char     title[NOTIFY_TITLE_MAX];
    char     body[NOTIFY_BODY_MAX];
} __attribute__((packed)) notification_payload_t;   // 136 字节
```

PC 端对应 struct 格式：`"<IBB2x32s96s"`，与固件严格对齐。

### UUID 分配

延续 `8a5c0000-xxxx` 命名空间：

- Service: `8a5c0003-0000-4aef-b87e-4fa1e0c7e0f6`
- Characteristic (WRITE): `8a5c0004-0000-4aef-b87e-4fa1e0c7e0f6`

### 环形缓冲关键实现

```c
// services/notify_manager.c
static notification_payload_t s_ring[NOTIFY_STORE_MAX];
static size_t   s_head;       // 下一个写入位
static size_t   s_count;      // 已存条数
static uint32_t s_version;    // 供 UI 去重刷新

void notify_manager_process_pending(void) {
    notification_payload_t tmp;
    bool changed = false;
    while (xQueueReceive(s_queue, &tmp, 0) == pdTRUE) {
        tmp.title[NOTIFY_TITLE_MAX-1] = '\0';
        tmp.body[NOTIFY_BODY_MAX-1]   = '\0';
        memcpy(&s_ring[s_head], &tmp, sizeof(tmp));
        s_head = (s_head + 1) % NOTIFY_STORE_MAX;
        if (s_count < NOTIFY_STORE_MAX) s_count++;
        changed = true;
    }
    if (changed) s_version++;
}

// index 0 = 最新
const notification_payload_t *notify_manager_get_at(size_t index) {
    if (index >= s_count) return NULL;
    size_t slot = (s_head + NOTIFY_STORE_MAX - 1 - index) % NOTIFY_STORE_MAX;
    return &s_ring[slot];
}
```

### 新增/修改文件

**新增（6 个）：**
- `services/notify_manager.{h,c}`
- `services/notify_service.{h,c}`
- `app/pages/page_notifications.{h,c}`

**修改（7 个）：**
- `services/CMakeLists.txt` — 注册新 .c
- `drivers/ble_driver.c` — 调用 `notify_service_init()`
- `main/main.c` — 调用 `notify_manager_init()`
- `framework/page_router.h` — 新增 `PAGE_NOTIFICATIONS` 枚举
- `app/CMakeLists.txt` — 注册新 .c
- `app/app_main.c` — 注册页面 + UI 循环加 `notify_manager_process_pending()`
- `app/pages/page_menu.c` — 去掉冗余的 "Back to Clock"，替换为 "Notifications" 入口

---

## 阶段二：UI 通知页（LVGL）

### 布局（240×320）

- 顶部 Back 按钮（回菜单）
- 右上 "x/10" 计数器
- 列表容器：`lv_obj_set_flex_flow(LV_FLEX_FLOW_COLUMN)`，默认可滚动
- 每条通知卡片（76px 高）：
  - 左上类别图标（按 category 着色）
  - 标题（`LV_LABEL_LONG_DOT` 单行省略）
  - 正文（`LV_LABEL_LONG_WRAP` 两行 wrap）
  - 右上时间 `HH:MM`
- 空状态：居中 "No notifications"

### category 图标/颜色映射

| category | symbol | color |
|---|---|---|
| GENERIC | `LV_SYMBOL_BELL` | MUTED |
| MESSAGE | `LV_SYMBOL_ENVELOPE` | ACCENT |
| EMAIL | `LV_SYMBOL_ENVELOPE` | 0xFBBF24 |
| CALL | `LV_SYMBOL_CALL` | 0x10B981 |
| CALENDAR | `LV_SYMBOL_LIST` | 0xA78BFA |
| SOCIAL | `LV_SYMBOL_EYE_OPEN` | 0x06B6D4 |
| NEWS | `LV_SYMBOL_FILE` | MUTED |
| ALERT | `LV_SYMBOL_WARNING` | 0xF97316 |

### version 去重刷新

`page_update()` 在 UI 线程每 10ms 调一次，若每次都全量 `lv_obj_clean + 重建`，会掉帧。**方案**：页面记住 `last_version`，`notify_manager_version()` 不变则跳过；变化时才全量重建（10 条可接受）。

---

## 阶段三：PC 端 GUI 初版（纯 ttk）

在已有 `tools/ble_time_sync.py` 的连接/时间/天气布局之下，追加通知推送区：
- 类别 `ttk.Combobox`（8 项枚举）
- 优先级 `ttk.Combobox`（Low/Normal/High）
- 标题 `ttk.Entry`
- 内容 `tk.Text`（3 行）
- 推送按钮 `ttk.Button`

**UTF-8 安全截断**（避免中文切到半个字符）：
```python
def _utf8_fixed(s: str, size: int) -> bytes:
    b = s.encode("utf-8")
    if len(b) > size - 1:
        b = b[:size - 1]
        while b and (b[-1] & 0xC0) == 0x80:
            b = b[:-1]
    return b.ljust(size, b"\0")
```

### 遇到的问题

**问题 1**：底部"推送通知"按钮在 Windows 显示时被挤出窗口。
- 初次猜测：窗口高度不够，700 → 760 → 900 逐级加高仍然复现
- 根因：**Tkinter 默认不声明 DPI awareness**。Windows 若处于 125%/150% 缩放，系统会把逻辑像素再放大一次，700 逻辑像素实际占 1050，超出笔记本屏幕。常见商用软件（QQ/VSCode）都启用了 DPI awareness，所以不会出现这种现象。

**修复**：
```python
def _enable_windows_dpi_awareness() -> None:
    if sys.platform != "win32":
        return
    import ctypes
    # 优先 Per-monitor v2（Win10 1703+）
    for fn in (
        lambda: ctypes.windll.shcore.SetProcessDpiAwareness(2),
        lambda: ctypes.windll.shcore.SetProcessDpiAwareness(1),
        lambda: ctypes.windll.user32.SetProcessDPIAware(),
    ):
        try:
            fn(); return
        except Exception:
            continue
```
在 `__main__` 最前面调用，Tk 按真实像素绘制，720 就是 720。

**问题 2**：即使 DPI 修正，纵向堆叠 4 块功能依然笨重，用户明确反馈"不好看"。

---

## 阶段四：PC 端 GUI 重构（CustomTkinter）

### 库选型

| 库 | 视觉 | 迁移成本 | 结论 |
|---|---|---|---|
| CustomTkinter | 深色圆角现代 | 低（继承 tk 框架） | **选用** |
| PySide6 | 工业级 | 高（全部重写） | 过重 |
| ttkbootstrap | Bootstrap 风 | 极低 | 风格不契合 |
| Flet | Material / Flutter | 中 | 带运行时 |

### 布局

从"单列纵向堆叠"改为"**侧边栏导航 + 右侧内容区**"：

```
┌──────────────────┬──────────────────────────────────────┐
│  ESP32 Logo      │                                      │
│  ─────────────   │                                      │
│  🏠  Home        │                                      │
│  ⏱   Time        │          当前选中页内容                │
│  🌤  Weather     │                                      │
│  🔔  Notify      │                                      │
│                  │                                      │
│  ● 已连接         │                                      │
│  AA:BB:CC:...    │                                      │
└──────────────────┴──────────────────────────────────────┘
   180px              540px
                      720×480 固定
```

**4 个页面**：
- Home — 设备状态卡片 + 大号连接/断开按钮
- Time — 大字号 ESP32 时间 + 读取/同步按钮
- Weather — 位置 + 大温度 + Low/High/湿度三小卡 + 刷新/推送 + 自动开关
- Notify — 类别/优先级下拉 + 标题输入 + 多行内容 + 推送按钮

页面切换用 `grid` + `grid_remove()` 管理，不重复创建实例。

### 配色

完全对齐 ESP32 屏幕：

```python
COLOR_BG       = "#1E1B2E"   # 主背景
COLOR_SIDEBAR  = "#2D2640"   # 侧边栏/卡片
COLOR_CARD_ALT = "#3A3354"   # 次级卡片/分割
COLOR_ACCENT   = "#06B6D4"   # 主色
COLOR_TEXT     = "#F1ECFF"
COLOR_MUTED    = "#9B94B5"
COLOR_SUCCESS  = "#10B981"
COLOR_DANGER   = "#EF4444"
```

### 状态同步

原 `_refresh_buttons(busy)` 重构为统一的 `_refresh_state()`：
- 根据 `self._busy + self._ble.connected` 派生所有按钮 state
- 侧边栏底部状态灯（●）颜色：绿=已连接 / 橙=操作中 / 红=未连接
- Home 页卡片与侧边栏状态双份显示，视觉信息密度更高

### 依赖变更

`tools/requirements.txt`：

```
bleak>=0.22
requests>=2.31
customtkinter>=5.2.0,<6
```

---

## 验证结果

| 项 | 结果 |
|---|---|
| ESP32 固件编译 | ✓ 无 warning 通过 |
| 烧录 + 启动 | ✓ BLE 广播正常 |
| PC 连接 + 推一条通知 | ✓ 屏幕 Notifications 页显示 |
| 连续推 11 条通知（环形丢旧） | ✓ 第 11 条入 → 最旧一条被丢弃，UI 列表正确刷新 |
| PC GUI 布局 | ✓ 用户确认"布局非常的号" |
| Windows DPI awareness | ✓ 加入后 720×480 稳定可见 |

---

## 经验教训

1. **Tkinter 在 Windows 默认不声明 DPI awareness**，这是高缩放下控件"莫名溢出"的根因。所有 Windows 桌面 Python 小工具都应在入口声明 `SetProcessDpiAwareness`，而不是去调大窗口来绕过。

2. **纯纵向堆叠不适合功能模块多的工具**。即使窗口够大，视觉上也像配置面板而非应用。侧边栏 + 内容区是最低成本的解决方案，且与用户日常软件（QQ/VSCode/微信）心智一致。

3. **环形缓冲比"丢最旧"策略更贴合通知语义**。weather_manager 是"最新即全部"，覆盖没损失；notify_manager 是"最近 N 条历史"，需要显式环形+索引倒序。

4. **version 号是 UI 去重的最小成本方案**。10ms 周期跑的 `page_update` 若不去重会疯狂重建 LVGL 对象；单独一个 `uint32_t` 对比即可省掉无谓刷新。

5. **UTF-8 字符串跨语言对齐**不能用简单 `[:size]`，必须按字节回退到合法边界，否则会引入非法字符序列。

6. **多个 BLE 服务的组织方式**：每个服务一对 `*_service.c`（GATT 接入）+ `*_manager.c`（线程安全数据），init 在 `ble_driver_init` 集中调用，main 里只管业务 manager 的初始化。复制-粘贴-改 UUID 的代价远低于抽象一个通用框架。

---

## 后续可做

- PC GUI 窗口最小宽/高自适应（当前固定 720×480，后续如需高分辨率扩展可改为 `grid_columnconfigure(weight=1)`）
- 固件侧通知页点击某条弹出详情（当前只在列表中 wrap 显示前两行）
- 通知"全部清空"按钮
- 持久化到 NVS，掉电保留最近 N 条
