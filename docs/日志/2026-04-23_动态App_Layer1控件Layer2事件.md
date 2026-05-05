# 动态 App（MicroQuickJS）Layer1 控件扩展 + Layer2 onClick 事件：工作日志

日期：2026-04-23

## 背景

上一阶段（Label 自注册）让脚本可以通过 `sys.ui.createLabel` + `sys.ui.setText` 在 Dynamic App 页面里创建/更新 label。但能力面太窄：

- 只能创建 label，不能创建容器（panel）和按钮（button）
- 没有任何样式控制（颜色、字体、对齐、padding、圆角……都改不了）
- 没有事件机制，脚本完全是单向"播报"，无法响应用户输入
- 视觉上完全不能复刻菜单页（`page_menu.c`）那种"卡片 + 列表项 + 图标"的布局

本阶段目标：把 dynamic_app 升级到"能用 JS 拼出菜单页同款界面 + 能响应点击"的程度。

## 目标

1. **Layer1：widget + 样式**
   - 新增 widget：panel（通用容器）/ button（可点击按钮）
   - 改造 createLabel：支持指定父容器（不再只能挂在 root）
   - 引入通用样式 setter：一个 `sys.ui.setStyle(id, key, ...)` 覆盖 9 个常用样式
   - 把 LVGL 内置 `LV_SYMBOL_*`、字体、对齐方式等"魔数"以命名常量暴露给 JS
2. **Layer2：onClick 事件**
   - 引入"UI → Script"反向事件队列
   - JS 通过 `sys.ui.onClick(id, fn)` 注册回调，点击时在 Script Task 上下文真正调 JS 函数
   - 严格保持线程安全契约：LVGL 只在 UI 线程操作，JS 引擎只在 Script Task 操作
3. **JS 脚本完全重写**：用上述新 API 1:1 复刻菜单页（卡片 + 7 个列表项）
4. **保持模块依赖方向**：dynamic_app 是底层组件，不能反向 include 上层 app 的头文件

## 方案与实现摘要

### 1) UI 命令扩展：cmd 结构改 union

原 cmd 结构是 `{ type, id[32], text[128] }`，加新命令会让每条命令都背 128 字节冗余。改成 union：

```c
typedef struct {
    dynamic_app_ui_cmd_type_t type;
    char id[DYNAMIC_APP_UI_ID_MAX_LEN];
    union {
        char text[128];                    // SET_TEXT
        char parent_id[32];                // CREATE_*
        struct { int32_t key, a, b, c, d; } style;   // SET_STYLE
        uint32_t handler_id;               // ATTACH_CLICK
    } u;
} dynamic_app_ui_command_t;
```

新增 cmd 类型：
- `DYNAMIC_APP_UI_CMD_CREATE_PANEL`
- `DYNAMIC_APP_UI_CMD_CREATE_BUTTON`
- `DYNAMIC_APP_UI_CMD_SET_STYLE`
- `DYNAMIC_APP_UI_CMD_ATTACH_CLICK`

### 2) 样式系统：9-key 通用 setStyle

引入 `dynamic_app_style_key_t` 枚举，用一组 (key, a, b, c, d) 覆盖 9 个最常用样式：

| key | 字段含义 | 对应 LVGL API |
|---|---|---|
| BG_COLOR | a = 0xRRGGBB | `lv_obj_set_style_bg_color` + opa=COVER |
| TEXT_COLOR | a = 0xRRGGBB | `lv_obj_set_style_text_color` |
| RADIUS | a = px | `lv_obj_set_style_radius` |
| SIZE | a=w, b=h（负数取绝对值当百分比） | `lv_obj_set_size` + `lv_pct(...)` |
| ALIGN | a=align id, b=x, c=y | `lv_obj_align` |
| PAD | a/b/c/d = left/top/right/bottom | `lv_obj_set_style_pad_*` |
| BORDER_BOTTOM | a = color | border_width=1 + border_side=BOTTOM |
| FLEX | a = 0(column)/1(row) | `lv_obj_set_flex_flow` |
| FONT | a = 0(text)/1(title)/2(huge) | `lv_obj_set_style_text_font` |

C 端用 switch 分发，JS 端通过 `sys.style.*` 常量调用，避免硬编码数字。

**SIZE 编码约定**：值 ≥ 0 → 像素；值 < 0 → `lv_pct(-v)`。例如 `(-100, 50)` = 宽 100% 高 50px。

### 3) Registry 升级：加 type 字段 + 容量提升

```c
typedef struct {
    bool used;
    ui_obj_type_t type;          // LABEL / PANEL / BUTTON
    char id[32];
    lv_obj_t *obj;
    uint32_t click_handler_id;   // 0 = 未绑定 onClick
} ui_registry_entry_t;
```

容量从 16 → **64**，配合菜单页约 30 个对象的需求。

新增三个内部 helper：
- `registry_find(id)` —— 按 id 查 slot
- `registry_alloc(id, type, obj)` —— 占空槽
- `resolve_parent(parent_id)` —— 父 id 查 obj，找不到回落到 s_root（带 WARN 日志）

### 4) 反向事件链路：UI → Script

这是模块第一次有"反方向"通信。设计上严格分离两条链路：

```
[Script Task]                                  [UI Task]
JS sys.ui.* ──→ s_ui_queue ──→ dynamic_app_ui_drain ──→ LVGL
                                                          │
                drain_ui_events_once ←── s_event_queue ←── on_lv_click
JS callback 执行                                          (LVGL 点击回调，
                                                           只入队 handler_id，
                                                           绝不阻塞 LVGL)
```

**关键点**：
- `on_lv_click` 在 LVGL UI 线程上下文，**绝不能直接调 JS_Call**（QuickJS 上下文非线程安全）
- 它只做一件事：从 `lv_event_get_user_data(e)` 取出 handler_id，`xQueueSend` 到反向队列
- 反向队列满则丢弃（绝不阻塞 LVGL 调度循环）
- Script Task 主循环每 tick 调 `drain_ui_events_once`，最多消 8 个事件，防 JS 抖死

### 5) JS 回调函数 GC 钉住

在 `dynamic_app.c` 新增：

```c
#define MAX_CLICK_HANDLERS 16
typedef struct {
    bool allocated;
    JSGCRef func;
} js_click_handler_t;
```

`sys.ui.onClick(id, fn)` 实现：
1. 找空 slot（slot ∈ [0, 16)）
2. `JS_AddGCRef(ctx, &handlers[slot].func)` 钉住 fn，阻止被 GC 回收
3. `handler_id = slot + 1`（0 保留为"无效"，便于 LVGL user_data 区分）
4. enqueue ATTACH_CLICK 命令携带 handler_id
5. 入队失败时回滚（DeleteGCRef + allocated=false）

**首版限制**：每个 widget 只能 onClick 一次，重复调用会在 drain 时 WARN 跳过。原因：detach 旧 callback 的 GCRef 释放需要"UI→Script"反向通知，复杂度上升一档；MVP 阶段不值。

### 6) JS 全局对象扩充：sys.* 加挂

`bind_sys_and_timers` 新增挂载：

- `sys.ui.createPanel/createButton/setStyle/onClick`（4 个 native fn）
- `sys.symbols.{BLUETOOTH, EYE_OPEN, SETTINGS, IMAGE, BELL, AUDIO, BARS, PLAY, LIST, LEFT, RIGHT}`（11 个 LVGL UTF-8 图标常量）
- `sys.style.{BG_COLOR, TEXT_COLOR, RADIUS, SIZE, ALIGN, PAD, BORDER_BOTTOM, FLEX, FONT}`（9 个 enum 常量）
- `sys.align.{TOP_LEFT, TOP_MID, ..., BOTTOM_RIGHT}`（9 个对齐方式）
- `sys.font.{TEXT, TITLE, HUGE}`（3 个字体档位）

JS 一律走命名常量，不再出现魔数。

### 7) 模块依赖方向修正：字体注入

dynamic_app 是底层组件（被 app 依赖），但样式系统要支持 FONT key 就需要拿到 `g_app_font_*`。如果 dynamic_app 反向 include `app/app_fonts.h`，CMake 直接报找不到头。

解决：在 `dynamic_app_ui.h` 暴露 setter：

```c
void dynamic_app_ui_set_fonts(const lv_font_t *text,
                              const lv_font_t *title,
                              const lv_font_t *huge);
```

由 `page_dynamic_app.c` 在 `page_init()` 里调一次：

```c
dynamic_app_ui_set_fonts(APP_FONT_TEXT, APP_FONT_TITLE, APP_FONT_HUGE);
dynamic_app_ui_set_root(s_ui.list_root);
dynamic_app_start();
```

桥接层只持指针不持有所有权，字体生命周期由 `app_fonts.c` 管。

### 8) JS 脚本重写：1:1 复刻菜单页

`scripts/app.js` 完全重写。约 80 行 JS 实现卡片 + 7 列表项 + 点击日志。核心结构：

```js
function makeItem(parent, id, icon, text) {
    var item = button(id, parent);
    st(item, Style.SIZE, -100, 50);             // 100% × 50px
    st(item, Style.BORDER_BOTTOM, COLOR_CARD_ALT);

    var ic = label(id + "_ic", item, icon);     // 图标
    st(ic, Style.TEXT_COLOR, COLOR_ACCENT);
    st(ic, Style.FONT, Font.TITLE);
    st(ic, Style.ALIGN, Align.LEFT_MID, 14, 0);

    var tx = label(id + "_t", item, text);      // 主文字
    st(tx, Style.TEXT_COLOR, COLOR_TEXT);
    st(tx, Style.FONT, Font.TEXT);
    st(tx, Style.ALIGN, Align.LEFT_MID, 48, 0);

    sys.ui.onClick(item, (function (cid) {
        return function () { sys.log("click " + cid); };
    })(id));   // IIFE 闭包捕获 id（ES5 没有块作用域）
    return item;
}
```

颜色与 `page_menu.c` 完全一致：
- BG `0x1E1B2E` / CARD `0x2D2640` / CARD_ALT `0x3A3354`
- ACCENT `0x06B6D4` / TEXT `0xF1ECFF`

### 9) 销毁链路加固

新增 `dynamic_app_ui_clear_event_queue()`，并嵌进 `dynamic_app_ui_unregister_all()`。
page destroy 顺序保持不变：

```
set_root(NULL)        // 关闭 enqueue 门禁
→ stop()              // 异步通知 Script Task 退出
→ unregister_all()    // 清 registry + 清反向事件队列
→ lv_obj_del(screen)  // 级联删除 LVGL 对象
```

teardown_context 里同时调 `click_handlers_reset(ctx)` 释放所有 GCRef，避免下次 start 时旧引用残留。

## 实施过程踩到的坑

### 坑 1：esp-mquickjs 不支持 ES6

第一版 app.js 用了 `const Style = sys.style;`，烧录后串口刷：

```
SyntaxError: unexpected character in expression
    at app.js:7:1
```

esp-mquickjs 是精简 fork，**只支持 ES5**：不能用 `const` / `let` / 箭头函数。
所有变量改 `var`，箭头函数 `() => {...}` 改回 `function () {...}`。

### 坑 2：JS_IsString 在这个 fork 里签名变了

```c
// 标准 QuickJS:
JS_BOOL JS_IsString(JSValue val);
// esp-mquickjs:
JS_BOOL JS_IsString(JSContext *ctx, JSValue val);
```

直接报 `passing argument 1 of 'JS_IsString' makes pointer from integer`。补 ctx 参数即可。

### 坑 3：JS_ToInt32 是 `int*` 不是 `int32_t*`

xtensa 上 `int32_t` 被 typedef 为 `long int`，与 `int` 不兼容（虽然位宽相同）。GCC 直接报 `incompatible pointer type`。
解决：用 `int` 局部变量接收，调用 enqueue 时强转 `(int32_t)`。

### 坑 4：模块循环依赖

最初 dynamic_app_ui.c 直接 `#include "app_fonts.h"`，编译报：

```
fatal error: app_fonts.h: No such file or directory
```

dynamic_app 的 CMakeLists 里 REQUIRES 没有 app（也不应该有，是反向依赖）。
解决：注入式依赖（见上文 §7）。

### 坑 5：Edit 工具误把双引号变成中文 smart quotes

某次 Edit 操作把 `"sys.ui.setText..."` 字面量里的 `"` 替换成了 `“ ”`，导致编译报一堆 `unexpected character`。批量找 `“ / ”` 字符替换回 ASCII 双引号。

### 坑 6：JS 一次性灌爆 Queue

烧录后只显示第一个列表项，后面全部丢失。原因：

- JS_Eval 是同步跑完整个 app.js 的
- 一次 build 调 60+ 次 enqueue
- 队列原容量 16，UI 每帧只 drain 4 条
- xQueueSend 用了 `timeout=0`（满则丢）

三件套修复：
1. **队列容量** 16 → 128
2. **每帧 drain 数量** 4 → 32
3. **enqueue timeout** 0 → 100ms（满则 Script Task 阻塞 100ms 等 UI 消化）

第三条是关键 —— 让 Script Task 自动节流，绝不丢命令。

### 坑 7：按钮按下没视觉反馈

视觉链路修好后，所有 7 项渲染正常，但点击没反馈。原因：`remove_style_all` + 默认 `bg_opa=TRANSP` 把按下态也清光了。
解决：在 `do_create` 的 BUTTON 分支补两行：

```c
lv_obj_set_style_bg_color(obj, lv_color_hex(0x06B6D4), LV_STATE_PRESSED);
lv_obj_set_style_bg_opa(obj, LV_OPA_30, LV_STATE_PRESSED);
```

按下时青色蒙层 30% 不透明度，与菜单页观感一致。

## 关键文件改动清单

| 文件 | 改动 |
|---|---|
| `dynamic_app/dynamic_app_ui.h` | cmd 结构改 union；新增 5 个 cmd enum + 9 个 style key + event_t；7 个新公共 API；registry 容量宏 16→64；event 队列容量宏 8 |
| `dynamic_app/dynamic_app_ui.c` | registry 加 type/click_handler_id 字段；新增 registry_find/alloc/resolve_parent；on_lv_click 反向回调；apply_style 9-key 分发；do_create 三类 widget；drain 6 路 switch；event 队列 init/pop/clear；Queue 容量 128，enqueue 阻塞 100ms |
| `dynamic_app/dynamic_app.c` | 4 个新 native fn（createPanel/createButton/setStyle/onClick）+ extract_parent_id helper；handler 表 + GCRef pin/release；drain_ui_events_once 在主循环 tick 前调；bind_sys_and_timers 注入 sys.{ui,symbols,style,align,font} 共 30+ 常量；teardown 释放 handlers |
| `dynamic_app/scripts/app.js` | 完全重写：80 行 ES5 复刻菜单页 7 项 + onClick 日志 |
| `app/pages/page_dynamic_app.c` | create_body 简化：去掉 hint label，list_root 220×250；page_init 新增 `dynamic_app_ui_set_fonts(...)` 注入字体 |
| `app/app_main.c` | `dynamic_app_ui_drain(4)` → `drain(32)` 提升 UI 帧消化能力 |

## 完整双向链路（最终架构）

```
┌─────────────────────────┐         ┌─────────────────────────┐
│  Script Task (Core 0)   │         │   UI Task (Core 1)      │
│                         │         │                         │
│   QuickJS 引擎          │         │   LVGL                  │
│   你的 app.js           │         │   屏幕渲染              │
│                         │         │                         │
│   sys.ui.createX  ──────┼──cmd──→ │   dynamic_app_ui_drain  │
│   sys.ui.setStyle       │  queue  │      ↓                  │
│   sys.ui.setText        │  (128)  │   lv_obj_create         │
│   sys.ui.onClick        │         │   lv_obj_set_style_*    │
│                         │         │   lv_obj_add_event_cb   │
│                         │         │                         │
│   你写的 onClick fn ←───┼──event──┤   on_lv_click           │
│   (在这里被 JS_Call)    │  queue  │   (只入队 handler_id)   │
│                         │   (8)   │                         │
│   setInterval 心跳      │         │                         │
└─────────────────────────┘         └─────────────────────────┘
       Core 0                               Core 1
       JS 单线程世界                        LVGL 单线程世界
              ↑                                    ↑
              └────── 通过两条 Queue 通信 ────────┘
                       绝不共享 LVGL 指针
                       JS 永远只认字符串 id
```

## 验收结果

- [x] `idf.py build` 通过
- [x] 烧录后从菜单页进入 Dynamic App，看到与菜单页类似的列表（卡片 + 7 个图标项）
- [x] 列表可上下滚动
- [x] 点击任一项有青色蒙层视觉反馈
- [x] 串口日志看到 `script: click <id>`
- [x] 返回菜单再进入，列表仍正常显示（destroy/recreate 路径无泄漏）
- [x] NVS 不受影响（本改动零持久化触点）

## 不在本次范围（留给下一阶段）

- 复刻菜单页的真实业务（蓝牙状态/背光档位实时显示）—— 本次只做视觉与点击日志
- onClick 二次绑定的优雅 detach（首版抛 WARN）
- 第 3 层声明式 build：`sys.ui.build({type: "panel", children: [...]})`
- 更多 widget：滑块、开关、进度条、图片
- 事件参数化：onClick(fn) 现在无参，未来可传 widget id / 触摸坐标
- 反向事件 NOTIFY_UI_EVENT bit 唤醒（当前最大延迟 50ms，多数交互场景够用）

## 经验沉淀

### 通用模式（可外推到任何嵌入式脚本运行时）

1. **跨线程不共享对象，只传消息** —— Actor 模型雏形
2. **用字符串 id 代替指针做"句柄"** —— 防野指针、JS crash 重启不留垃圾
3. **回调函数必须 GCRef 钉住** —— 否则 GC 回收后触发即崩
4. **关键资源要有"门禁"** —— `s_root = NULL` 这个开关让销毁顺序变简单
5. **销毁顺序：先关入口，再停生产者，最后清理资源**
6. **跨任务命令通道用 Queue，跨任务信号用 Task Notification** —— 各司其职，别混用

### 模块依赖方向铁律

底层组件**永远不能**反向 include 上层组件的头。要拿上层资源，三种方式：
- 注入式（setter）—— 本次用的
- 回调注册（function pointer）
- 全局符号 + weak alias

CMake 的 REQUIRES 关系决定 include path，违反方向直接编不过，是好事。

### Queue 三件套节流

只要存在"突发生产者 + 受限消费者"的场景，必须同时调三处：
1. 队列容量 ≥ 单次突发量
2. 消费速率 ≥ 平均生产速率
3. 生产端 send 用阻塞超时（让生产者自动节流）

少任何一个都会丢消息。
