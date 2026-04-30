# UI 设计系统 + Weather 与 Menu 美化重构 工作日志

**日期**：2026-04-30
**分支**：feat/app_fs
**作者**：ChenShuang + Claude

---

## 0. 起因

PC 端 BLE 工具链 (`tools/companion`) 重构落地后，做联调测试。过程中发现固件 UI 的几个本质问题：

1. **配色全靠各 page 自定义**（`COLOR_BG / COLOR_CARD / COLOR_ACCENT` 在每个 page_*.c 重复定义），改主题要改 9 个文件
2. **没有 widget 复用**，每个 page 自己 `lv_btn_create + remove_style_all + add_style` 一遍样式
3. **字体小字号模糊**——14px CJK 用 Tiny TTF 运行时光栅化，hinting 弱，看不清
4. **菜单页是单调列表**，没有"系统级 UI"的样子（无状态栏、无图标、无翻页）
5. **没有"先 mockup 再写代码"的工作流**，每改一处 UI 都要 build+flash ≈ 2 分钟，迭代极慢

**核心矛盾**：280 像素高的小屏 + LVGL，**任何 UI 改动都该秒级反馈**而不是分钟级。

---

## 1. 目标

按"基础设施 → 单页验证 → 推广复用"三步走：

1. **建 UI 设计系统**（token + widget + anim），让所有页用同一套语言
2. **建 HTML mockup 工作流**：每个页改造前先在浏览器调，定稿再翻译到 LVGL
3. **改造 weather + menu 两个页**作为首批验证（高频、视觉重要）

---

## 2. 关键决策

| 议题 | 决策 |
|---|---|
| 配色风格 | 从"深紫青绿"换 **iOS 浅色** (`#F2F2F7` 底 + 纯黑文字 + iOS 蓝强调) |
| 字号策略 | 不换字体（Tiny TTF 全字符覆盖不可替代），靠对比度提清晰度 |
| 图标方案 | **Material Symbols Rounded 子集** 当字体用（19 图标 = 3.9 KB）|
| 退出手势 | weather: 底部 50px 边缘上滑（避免误触业务区）|
| 菜单布局 | 3×3 九宫格 + 翻页吸附；状态栏 + 主体 + 翻页指示三段式 |
| 蓝牙状态显示 | 删除菜单"Bluetooth"项，改为状态栏图标实时显示 |
| 电池 | 无硬件，用 `battery_sim` 模拟（每 5 分钟 -1%，0→100 循环）|
| Mockup 与代码同步 | 浏览器 HTML 严格遵守"只用 token / 禁 shadow / 禁 gradient"以保证翻译可行 |

---

## 3. 最终落地的目录结构

### 新增 UI 设计系统

```
app/ui/
├── ui_tokens.h         颜色/字号/间距/圆角全项目唯一来源
├── ui_widgets.h/.c     ui_card / ui_card_accent / ui_kv_row / ui_icon_btn / ui_screen_setup
├── ui_anim.h/.c        ui_anim_fade_in / ui_anim_press_feedback / ui_anim_number_rolling
└── ui_statusbar.h/.c   24px 系统级状态栏（时间 + 蓝牙 + 电池），1Hz 自更新
```

### 新增图标 / 字体

```
app/fonts/
├── srhs_sc_subset.ttf            (现有) 中文 14/16/48
└── material_icons_subset.ttf     (新增) 19 图标 ×24/36px = 3.9 KB

app/icons/                        (新增)
└── ic_*.bin × 8                  从 dash app 复制的天气图标，weather 页用

app/weather_icons.h/.c            (新增) EMBED 解析为 lv_image_dsc_t
```

### 新增 mockup 体系

```
ui_mockups/
├── README.md                     工作流说明 + LVGL 兼容规则
├── _shared/
│   ├── tokens.css                复刻 ui_tokens.h 的 CSS 变量
│   ├── frame.css                 240×320 屏幕模拟器 + 2× 缩放
│   └── weather_icons/*.png       为 mockup 准备的图标拷贝
├── weather/
│   ├── v1.html                   当前 LVGL 实现的浏览器镜像（baseline）
│   ├── v2.html                   极简无卡片
│   ├── v3.html                   横排 hero + 大卡
│   ├── v4.html                   左大图标 + 右温度（3 种天气状态对比）
│   └── v5.html                   v4 hero + v1 双卡布局 ★ 最终采纳
└── menu/
    ├── v1.html                   当前列表 baseline
    └── v2.html                   状态栏 + 3×3 九宫格 + 翻页 ★ 最终采纳
```

### 新增工具

```
tools/gen_icons_subset.py         Material Symbols 子集生成（pyftsubset 包装）
                                  ICONS dict 加 codepoint → 重跑即可
```

### 新增服务

```
services/manager/battery_sim.h/.c 电池模拟（线性下降 / OK/LOW/CRIT 三档）
```

### 大改

- `app/pages/page_weather.c` —— 全页按 v5 mockup 重写
- `app/pages/page_menu.c` —— 全页按 v2 mockup 重写（列表 → 九宫格）
- `app/app_fonts.c/.h` —— 加 2 个图标字体 + 18 个 ICON_* codepoint 宏

---

## 4. 关键架构设计

### 4.1 UI 设计 Token 系统

`ui_tokens.h` 是全项目唯一颜色/字号/间距来源：

```c
// 浅色 iOS 主题
#define UI_C_BG          lv_color_hex(0xF2F2F7)
#define UI_C_PANEL       lv_color_hex(0xFFFFFF)
#define UI_C_TEXT        lv_color_hex(0x000000)   // 纯黑：最大对比度 = 最清晰
#define UI_C_ACCENT      lv_color_hex(0x007AFF)   // iOS 蓝

// 字号语义别名
#define UI_F_BODY        APP_FONT_TEXT
#define UI_F_TITLE       APP_FONT_TITLE
#define UI_F_HUGE        APP_FONT_HUGE

// 8 倍数间距
#define UI_SP_XS    4
#define UI_SP_SM    8
#define UI_SP_MD    12
#define UI_SP_LG    16
```

**铁律**：任何 page/widget 不再写裸 `lv_color_hex(0xXXXX)`，改色一处生效。

### 4.2 Widget 三件套

| Widget | 用途 | 内部样式 |
|---|---|---|
| `ui_card` | 圆角面板 | 白底 + 14px 圆角 + 1px 浅描边 + 12px padding |
| `ui_card_accent` | 强调卡片 | 同上 + accent 色描边 |
| `ui_kv_row` | KV 行 | flex 两端对齐 + 自带 1px 底部分隔线 |
| `ui_icon_btn` | 图标按钮 | 透明 + 按下浅 accent 底 |
| `ui_screen_setup` | 屏幕底色刷 | UI_C_BG + 去 padding/scroll |

### 4.3 字体 fallback 链

```
g_app_font_text  (CJK 14)
    └── fallback → g_app_font_icons_24 (Material 图标)
        └── fallback → lv_font_montserrat_14 (LV_SYMBOL 老图标兜底)

g_app_font_huge  (CJK 48)
    └── fallback → g_app_font_icons_36
        └── fallback → lv_font_montserrat_24
```

写汉字 / 写 Material 图标 / 写 LV_SYMBOL_LEFT 都会按链路自动找对字体。

### 4.4 Material Symbols 子集化

源 ttf 14.3 MB → pyftsubset → 3.9 KB（**3732× 压缩**）。

`tools/gen_icons_subset.py` 里维护 ICONS dict（19 个图标 codepoint），加图标只需改 dict 重跑，产物 .ttf 自动更新。

`app_fonts.h` 暴露 18 个 ICON_* 宏，UTF-8 字面量直接 `lv_label_set_text(lbl, ICON_BLUETOOTH)` 即可。

### 4.5 mockup-first 工作流

发现的根本痛点：**LVGL 改一次要 build+flash ≈ 2 分钟，一晚上调 30 个版本**。

新工作流：
```
设计 → HTML/CSS 在浏览器调 (~0s 反馈) → 改到满意 → 翻译 LVGL (~10 分钟)
```

`ui_mockups/_shared/tokens.css` 把 `ui_tokens.h` 的颜色/字号/间距复刻成 CSS 变量，HTML 只能引用变量不能硬编码 → **保证 mockup 可翻译到 LVGL**。

`ui_mockups/_shared/frame.css` 提供 240×320 容器 + 2× 缩放 + 8px 调试网格 + 尺寸标注切换。

### 4.6 weather 页 v5 布局

```
┌─────────────────────────┐
│      Los Angeles        │  顶部弱化城市名
│                         │
│  ┌────┐                 │  hero 横排
│  │ ☀  │   14.3°         │  左 80×80 图标 + 右温度 + 状态色
│  │    │   晴            │
│  └────┘                 │
│                         │
│  ┌─最低──分隔──最高─┐   │  minmax 卡 220×50
│  │  8°   │   22°      │   │
│  └────────────────────┘  │
│  ┌────────────────────┐  │  info 卡 220×80
│  │ 湿度        45%    │  │  KV row 列表
│  │ 更新     14:32     │  │
│  └────────────────────┘  │
│                         │  底部 20px 边缘 = 退出手势区
└─────────────────────────┘
```

**关键技术**：
- 8 张 weather 图标作为 binary 直接 EMBED 到固件，运行时拼 `lv_image_dsc_t`
- `lv_image_set_scale(512)` 把 40×40 原图显示为 80×80
- 数据更新时温度走 `ui_anim_number_rolling` 滚动动画
- 入场 staggered fade-in（4 个元素错开 80ms）
- 底缘上滑退出：`LV_EVENT_PRESSED` 记起手 y，`LV_EVENT_GESTURE` 检查 y ≥ screen_h - 50 + 上滑方向

### 4.7 menu 页 v2 布局

```
┌─────────────────────────┐
│ 14:32         ⌬ ▮ 100% │  状态栏 24px (ui_statusbar)
├─────────────────────────┤
│  [日历][云][铃]         │  
│   时间 天气 通知         │  
│                         │  
│  [音][滑][阳]           │  3×3 九宫格 80×88
│   音乐 系统 亮度         │  
│                         │  
│  [ⓘ][▣][...]            │  
│   关于 动态app...        │  
│                         │
│        ● ○              │  分页指示 16px
└─────────────────────────┘
```

**翻页机制**：
- 横向 scroll 容器 + `LV_SCROLL_SNAP_CENTER` 自动吸附
- 每页一个独立 `lv_obj`，3×3 grid 布局
- `LV_EVENT_SCROLL` 监听 scroll_x，按 (x + 120) / 240 算当前页 → 同步分页指示

**cell 结构**：
- 数据层：`cell_def_t` 数组（kind + page_id + icon + label + color + dyn_name）
- 视图层：80×88 透明 button + 36px 图标 label + 14px 文字 label
- 静态 cell（7 个）：iOS 风一图一色配色
- 动态 cell：能用 `icon.bin` 就用 image，否则字体 fallback

**保留逻辑**：长按删除（menu_modal）、上传完成自动重建（s_dirty + on_upload_status）

### 4.8 状态栏 widget

`ui_statusbar.h/.c` 是**第一个跨页共享组件**，未来其他页（time / system / etc）都能直接用：

```c
lv_obj_t *bar = ui_statusbar_create(parent);  // 自动 240×24 占顶部
// 内部 1Hz timer 自更新
// 销毁时 LV_EVENT_DELETE 回调 free ctx + 停 timer
```

数据源：`time(NULL)` / `ble_driver_is_connected()` / `battery_sim_get_*`。

### 4.9 电池模拟

`services/manager/battery_sim.c`：
```c
uint8_t pct = 100 - (esp_timer_get_time() / (5 * 60 * 1000 * 1000) % 101);
```

无任务、无回调，纯查询时计算。8 小时 20 分钟从 100 降到 0，重置回 100 循环。

后续接真电池硬件只需替换内部实现，对外 `battery_sim_get_percent/state` 接口不变。

---

## 5. 视觉对比

### weather 页

| 项 | Before | After |
|---|---|---|
| 主题 | 深紫青绿 | iOS 浅色 |
| 顶部返回键 | 36×30 按钮（占视觉权重）| 删除，改边缘上滑退出 |
| 城市名 | 右上角 | 顶部居中弱化 |
| 状态文字 | 32px Title 单独行 | 跟温度横排在 hero |
| 天气图标 | 无 | 80×80 大图标，按 weather_code 切换 |
| 数据卡片 | 暗紫底无描边 | 白底 + 1px 浅描边 |
| 数据更新 | 直接闪变 | 温度数字滚动动画 |
| 入场 | 无 | 6 元素错开 60ms fade-in |

### menu 页

| 项 | Before | After |
|---|---|---|
| 布局 | 列表（向下滚动）| 3×3 九宫格 + 横向翻页 |
| 顶部 | "‹ Menu" 按钮 | 24px 状态栏（时间 + 蓝牙 + 电池）|
| 蓝牙 | 列表项点击 no-op | 状态栏图标实时显示 |
| 图标 | 单色 LV_SYMBOL | iOS 风一图一色，Material Symbols 矢量 |
| 翻页 | 无 | snap_x 吸附 + ●○ 指示器 |
| 退出 | 顶部返回键 | 下滑回锁屏（与 page_time 上滑进菜单互为往返）|

---

## 6. 文件改动清单

### 新建（13 个）
- `app/ui/ui_tokens.h`
- `app/ui/ui_widgets.h` / `.c`
- `app/ui/ui_anim.h` / `.c`
- `app/ui/ui_statusbar.h` / `.c`
- `app/weather_icons.h` / `.c`
- `app/icons/ic_*.bin × 8`
- `app/fonts/material_icons_subset.ttf`
- `services/manager/battery_sim.h` / `.c`
- `tools/gen_icons_subset.py`
- `ui_mockups/` 全部内容（README + _shared + weather/v1-v5 + menu/v1-v2）

### 修改（5 个）
- `app/CMakeLists.txt` —— UI/icons/font 新源 + EMBED
- `app/app_main.c` —— `weather_icons_init()` 调用
- `app/app_fonts.h` / `.c` —— 加 Material 图标字体 + ICON_* 宏 + fallback 链
- `app/pages/page_weather.c` —— 全页按 v5 重写
- `app/pages/page_menu.c` —— 全页按 v2 重写
- `services/CMakeLists.txt` —— `battery_sim.c`

---

## 7. 联调发现的 Bug 与修复

### 7.1 PC 端 GUI 跨线程崩溃
**现象**：companion GUI 点 Notify 发送 → `RuntimeError: no running event loop`
**根因**：`bus.emit()` 假定 asyncio 工作线程内调用，但 GUI 是 tkinter 主线程
**修复**：`tools/companion/gui/pages/{notify,upload}.py` 改用 `bus.emit_threadsafe()`

### 7.2 Asset 路径太长
**现象**：上传 dashboard_pkg → `asset path too long: '<app_id>/assets/ic_clear.bin' exceeds 31 chars`
**根因**：固件 PATH_LEN=31 硬限制，`dashboard_pkg` (13 字符) + `/assets/` (8 字符) + 文件名 → 超限
**修复**：把 `dashboard_pkg` 重命名为 `dash`（4 字符），budget 从 10 涨到 19

### 7.3 dash app saveState TypeError
**现象**：`sys.app.saveState(jsonString): need string`，导致 `ble.on(data)` 回调 throw，UI 不更新（看起来像"天气拉取失败"）
**根因**：`dash/main.js` 直接传 object，但 native API 要 string
**修复**：`saveState(JSON.stringify(...))` + `loadState` 后 `JSON.parse`

---

## 8. 设计原则沉淀

工作过程中确立了几条后续都该遵守的原则：

1. **黑底白字 vs 白底黑字**：白底黑字对比度 21:1，是清晰度天花板。**永远从黑白开始设计，最后再加颜色**
2. **设计 token 化**：颜色/字号/间距/圆角集中管理，改一处全局变
3. **Mockup 先行**：任何非 trivial UI 改动先在 HTML 调，定稿再翻译。**别用编译烧录调样式**
4. **图标走字体**：单色规整图标用字体（缩放清晰、改色简单、API 一致），彩色复杂图用 PNG/binary
5. **手势限定起手区**：全屏手势避免冲突业务的方法 = 限定起手位置（如底部 50px 边缘）
6. **共享组件抽出**：能跨页共用的（status bar / card / kv_row / 动画函数）一律提到 ui/，不在 page 里复制粘贴

---

## 9. 验证

### 9.1 weather 页
- [x] 8 种 weather_code 切换 → 图标和状态色都跟着变
- [x] 数据更新时温度滚动动画
- [x] 入场 staggered fade-in 流畅
- [x] 底缘上滑退出，业务区滑动不响应
- [x] 浅色主题对比度足够，14px 中文清晰可读

### 9.2 menu 页
- [x] 状态栏：时间 1Hz 跳动、蓝牙连/断图标切换、电池图标和颜色按 % 切档
- [x] 3×3 九宫格布局正常
- [x] 横向滑动翻页吸附流畅
- [x] 分页指示器同步当前页
- [x] 静态 cell 点击进入对应页
- [x] 亮度 cell 点击循环切档
- [x] 动态 app 长按删除 modal 工作正常
- [x] 上传完成后菜单自动刷新（s_dirty 链路）

### 9.3 设计系统
- [x] 4 个 weather mockup（v1-v5）+ 2 个 menu mockup（v1-v2）浏览器渲染正常
- [x] mockup 与 LVGL 实际效果差异在可接受范围（仅字体光栅化质量差异）
- [x] 改 ui_tokens.h 一处颜色 → page_weather + page_menu 都跟着变（未来 page_*.c 改造完后全设备生效）

---

## 10. 不在本次范围（下一轮工作）

- ❌ 其它页（time / system / notifications / music / about / time_adjust）改造 —— 沿用 v5 工作流，每页 30-60 分钟
- ❌ 24h 折线图 / 多日预报 —— 需要扩 weather PC 推送 payload（open-meteo API 已支持）
- ❌ 真电池硬件接入 —— battery_sim 接口已稳，加 IO 驱动即可替换
- ❌ 状态栏 BT/电池图标可点击跳详情页
- ❌ 翻页时整屏滑动动画（目前是即时切换，无水平转场）
- ❌ 动态 app 自定义图标策略：是否所有 dynapp 都强制带 icon.bin

---

## 11. Commit 顺序建议

每步可单独验证、单独回退：

1. `feat(ui): design tokens + widgets + anim` —— ui/ 目录基础设施
2. `feat(ui): mockup framework + weather v1-v5` —— ui_mockups/ 工作流
3. `feat(weather): icons + v5 layout + edge swipe exit` —— weather 页改造
4. `feat(font): material icons subset + 18 ICON_* macros` —— 图标字体
5. `feat(ui): statusbar widget + battery_sim` —— 共享组件
6. `feat(menu): 3x3 grid + paging + statusbar` —— menu 页改造
7. `fix(companion): use emit_threadsafe in GUI pages` —— PC 端 bug 修复
8. `chore: rename dashboard_pkg → dash; fix saveState type` —— dash app 修复

---

## 12. 一句话总结

把"每页自己写颜色样式 + 列表式菜单 + 模糊小字"重构为"全项目 token + widget + anim 三件套 + iOS 浅色主题 + Material 矢量图标 + mockup-first 工作流 + 状态栏共享组件 + 3×3 九宫格菜单"，从"功能堪用"跨进"系统级 UI"门槛。
