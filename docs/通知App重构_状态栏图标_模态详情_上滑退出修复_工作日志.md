# 通知 App UI 重构 + 状态栏图标 + 模态详情 + 上滑退出修复 工作日志

**日期**：2026-05-01
**分支**：feat/optimize_page
**作者**：ChenShuang + Claude

---

## 0. 起因

App vs Page 重构落地后做联调，开始一项一项美化原生 app 的 UI。先从通知页开始，过程中暴露出一系列旧实现和新设计系统不兼容的问题：

1. **状态栏丑** —— 旧 statusbar 用 Material 字体的电池图标，远看是几条小竖线，识别度低，没数字
2. **状态栏间距大** —— 蓝牙图标和电池图标之间隔太宽，视觉松散
3. **多余文件** —— `app_ui.c/h`（早期单文件 demo）和 `framework/app_shell.c/h`（只 12 行，转发两个退出函数）已经废了但没删
4. **每页独立的"on_exit 函数名"和 stdlib 的 `on_exit()` 撞名** —— 编译报 `conflicting types`
5. **通知页布局烂** —— 卡片高度自适应，长内容压成两行+第二行钉一块，节奏不齐
6. **通知详情没法看** —— 列表里被 `LONG_DOT` 截断后，长内容看不到全貌
7. **通知页上滑退出失效** —— 加了 list 后，全屏 `LV_EVENT_GESTURE` 收不到，和 weather 页表现不一致

这次工作把上面 7 件事一并解决。

---

## 1. 关键决策

| 议题 | 决策 | 理由 |
|---|---|---|
| `app_ui.c/h` | 删 | 全项目无引用，CMakeLists 也没编译它 |
| `framework/app_shell.c/h` | 删 + 把 8 个调用方改回直接调 `app_router_*` | 只是个 12 行转发，没承担任何独立职责（不是 Facade，是 typedef）|
| `weather_icons.c/h` | 保留 | 8 张彩色 weather 图标走 EMBED bin，字体替代不了 |
| `static void on_exit(void)` | 8 个 app 文件全部 rename `on_app_exit` | 撞 `<stdlib.h>` 的 POSIX `on_exit()` |
| 状态栏电池图标 | 自绘横向胶囊 + 内填充条 + 中央数字 | Material 字体的电池图标看不清，远不如 iPhone 风格直观 |
| 蓝牙/电池间距 | `pad_column` 8 → 4 | 视觉紧凑 |
| 通知列表卡片 | 固定 64px + 显式锁定 label 宽高 + `LV_LABEL_LONG_DOT` | 解决"高度自适应导致溢到第二行钉一块"问题 |
| 通知详情 | **模态弹层** 而非新页面 | 短消息撑不起整页，弹层尺寸自适应内容；将来 music/system 也能复用 |
| 模态层 | 新建通用 `ui_modal_card` 组件 | 不绑定具体业务，未来其它 app 详情查看复用 |
| 通知页上滑退出 | **放弃 LVGL gesture，改 PRESSED+PRESSING+RELEASED 自算 dy** | LVGL gesture 对 CLICKABLE/scroll 容器抑制，事件根本不发 |

---

## 2. 落地清单

### 删除（4 个文件）

```
app/app_ui.c                        早期单文件时间调节 demo，已被 settings_time 取代
app/app_ui.h
framework/app_shell.c               12 行转发，无独立职责
framework/app_shell.h
```

`framework/CMakeLists.txt` 同步移除 `app_shell.c` 一行。

### 重命名（8 个 app 入口）

每个 `apps/<name>/<name>_app.c` 里：
- 函数 `static void on_exit(void)` → `static void on_app_exit(void)`
- 字段 `.on_exit = on_app_exit`（字段名保留 `app_router` 接口约定）

8 个文件：`lockscreen / launcher / weather / notifications / music / system / settings / dynapp_host`

### 状态栏自绘电池胶囊

`app/ui/ui_statusbar.c` 重写：

```
┌─────────────────────────────────┐
│ 14:32        [BT] [ ████ 87 ]>  │   高 24px
└─────────────────────────────────┘
                  └ 32×14 胶囊
                    1px 边框 + 内 28×10 填充条
                    中央居中 14px 蒙特塞拉特数字
                    右侧 2×6 正极凸起
```

- 填充宽度 = `(BATT_FILL_W * pct + 50) / 100`，最少 1px（让 0% 也有微亮线）
- 颜色按 `battery_state_t`：OK 绿 / LOW 橙 / CRITICAL 红
- 胶囊外壳描边、凸起、数字都用主题色（深色页白系，浅色页黑系）
- 蓝牙图标和电池组的 `pad_column` 改 `UI_SP_XS`(4)，紧凑 50%

### 通知页 UI 重构

`app/apps/notifications/pages/page_notifications.c` 全文重写。

**风格切到 iOS 浅色**（与 weather/launcher 对齐）：
- 整页 `ui_screen_setup`（UI_C_BG 白底）
- statusbar 改 `dark=false`
- 列表卡用 `ui_card`（白底 + 1px 浅描边 + 14px 圆角）
- 图标块改 36×36 圆角彩底 + 白色 Material Symbols 矢量图标
- category → iOS 系统色映射（消息蓝/邮件橙/电话绿/日历紫/警告红/...）

**头部**：
- 左 "通知" 16px 标题
- 右 36×20 圆角胶囊徽章（accent 蓝底白字数字，0 时灰底，>99 显示 "99+"）
- 长按徽章 → 弹"清空所有"确认模态

**列表卡片（核心优化）**：
```
固定 64px 高
┌──────────────────────────────────┐
│ ┌──┐ Title (LONG_DOT)      14:32 │  y=2  20px
│ │🔔│                              │
│ └──┘ body single line ellipsis...│  y=28 18px
└──────────────────────────────────┘
```

关键：**用 `lv_obj_set_size(w, h)` 显式锁定宽高**，不用 `lv_obj_set_width` + 自适应高度。
否则 CJK 字体光栅化偶尔会被解析成两行把后续内容挤压。
- 标题 116×20
- 正文 160×18（占用第二行整段，包括时间下方区域）
- 时间 40×20 右对齐，固定在 TOP_RIGHT

**空状态**：
- 居中铃铛图标（36px Material）+ "暂无通知" 16px CJK
- 不再是冷冰冰的小灰字

**入场动画**：标题/徽章/列表错开 0/60/120ms fade-in。

### 通用模态组件 `ui_modal_card`

新文件：
- `app/ui/ui_modal_card.h`
- `app/ui/ui_modal_card.c`

API：

```c
ui_modal_card_t *m = ui_modal_card_create();
lv_obj_t *cnt = ui_modal_card_content(m);
// 往 cnt 里加任意 LVGL 对象（自动纵向 flex 堆叠）
ui_modal_card_add_action(m, "删除", on_del, NULL);
ui_modal_card_add_action(m, "关闭", NULL, NULL);  // NULL cb = 仅关闭
ui_modal_card_show(m);
```

**特性**：
- 全屏半透明黑遮罩，点击空白关闭
- 居中卡片宽 224，高 SIZE_CONTENT，最大 270，超出内部纵向滚动
- 内容容器纵向 flex，所有 label 都可 `LV_LABEL_LONG_WRAP + LV_PCT(100)` 多行 wrap
- 卡片下滑（LV_DIR_BOTTOM）关闭
- 底部最多 2 个 action 按钮（左灰右蓝，flex_grow 平分宽度）
- action 按钮触发后自动关闭模态再回调（避免回调里操作即将销毁的对象）
- 淡入动画 UI_DUR_FAST

`app/CMakeLists.txt` 加入 `ui/ui_modal_card.c`。

### 通知详情模态

通知列表卡 `LV_EVENT_CLICKED` → `show_detail_modal(index)`：

```
┌──────────────────────────┐
│ [🔔]      2026-05-01 14:32│  顶部 36px：图标 + 完整时间戳
│ ─────────────────────── │
│ 标题（多行 wrap 自适应）   │
│ ─────────────────────── │
│ 正文（多行 wrap 自适应）   │
│ ...                      │
│ ─────────────────────── │
│ [删除]            [关闭] │
└──────────────────────────┘
```

- title/body 都是 `LV_LABEL_LONG_WRAP + LV_PCT(100)`，行数随内容自适应
- 总高超过 270px 时，整个 content 容器内部滚动
- 删除按钮调 `notify_manager_remove_at(s_modal_index)` 后 version 自增触发列表重建

### 单条删除 API

`services/manager/notify_manager.{h,c}` 新增：

```c
esp_err_t notify_manager_remove_at(size_t index);
```

环形缓冲删 index 后，把所有"更旧"的元素朝 head 方向逐个平移一格，head 回退一格，count--，version++，dirty=true（2 秒后自动落 NVS）。

```c
for (size_t i = index; i + 1 < s_count; i++) {
    size_t dst = (s_head + N - 1 - i)     % N;
    size_t src = (s_head + N - 1 - (i+1)) % N;
    memcpy(&s_ring[dst], &s_ring[src], sizeof(s_ring[0]));
}
s_head = (s_head + N - 1) % N;
memset(&s_ring[s_head], 0, sizeof(s_ring[0]));
s_count--;
```

---

## 3. 上滑退出修复（最难的坑）

通知页加了 list 之后，**绑在 screen 上的 `LV_EVENT_GESTURE` 永远收不到**。
weather 页同样代码却能正常退出。

调试过程：

### 第一轮：怀疑事件被 list 吞

list 是 LVGL scroll 容器，纵向移动会被识别为 scroll。
方案：`LV_OBJ_FLAG_EVENT_BUBBLE`，让事件冒到 screen。

**结果失败**。LVGL scroll 容器一旦进入滚动状态就不发 GESTURE，BUBBLE 也救不了。

### 第二轮：底部独立 hit zone

在屏幕底部放一个 50px 的不可滚动 CLICKABLE 容器，自己处理 PRESSED + GESTURE。
理论上事件不会被 list 干扰。

**结果还是失败**。日志（这次加了详细 ESP_LOGI）显示：

```
PRESS@hit  y=319    ← 按下事件正常
PRESS@hit  y=312    ← 多次按下 release
PRESS@hit  y=308    ← 一直在 PRESS
（没有 GEST 任何记录）
```

只有 `PRESS` 没有 `GEST`！LVGL 完全没把这次操作识别成 gesture。

### 真正的根因

LVGL `lv_indev` 内部处理 CLICKABLE 容器时进入 **"短按 click 模式"**：会调用 `lv_indev_wait_release`，从而**抑制 gesture 计算**。
垂直滑动也不会触发 `LV_EVENT_GESTURE`。

为什么 weather 页能用？因为 weather 页的 screen **不是 CLICKABLE**，也没有 scroll 子容器，纯展示。
indev 的纵向移动直接进 gesture 路径。

### 第三轮：放弃 LVGL gesture，自算 dy

LVGL 一定会发的事件：
- `LV_EVENT_PRESSED` —— 按下瞬间
- `LV_EVENT_PRESSING` —— 按下后每帧
- `LV_EVENT_RELEASED` —— 松开

PRESSED 记起手 y0，PRESSING 每帧更新 y_last，RELEASED 算 `dy = y0 - y_last`，dy ≥ 30 就退出。

```c
static void on_pressed(lv_event_t *e) { /* y0 = y_last = current */ }
static void on_pressing(lv_event_t *e){ /* y_last = current */ }
static void on_released(lv_event_t *e){
    int dy = s_ui.press_y0 - s_ui.press_y_last;
    if (dy >= 30) app_router_exit_to_launcher();
}
```

实测日志（成功）：
```
PRESS@hit  y=319
RELEASE@hit  y0=319 y1=234 dy=85
EXIT triggered (dy=85)
```

完美。

### 经验沉淀（重要！）

> **LVGL gesture 在 CLICKABLE 容器或有 scroll 子容器的页面上不可靠**。
> 凡是页面里有列表、卡片可点击、按钮多的场景，统一用 PRESSED+PRESSING+RELEASED 自算 dy 实现自定义手势，不要用 `LV_EVENT_GESTURE` + `lv_indev_get_gesture_dir`。
> 只有像 weather 这种纯展示页才能放心用 LVGL 内置 gesture。

底部 hit zone 高度从 50 → 28px（视觉留白合理），起手只看 dy 阈值不再看 y0 区间。

---

## 4. 文件改动清单

### 新建（2）
- `app/ui/ui_modal_card.h`
- `app/ui/ui_modal_card.c`

### 删除（4）
- `app/app_ui.c` / `app/app_ui.h`
- `framework/app_shell.c` / `framework/app_shell.h`

### 修改（重要）
- `app/apps/notifications/pages/page_notifications.c` —— 全文重写（list/卡/模态/手势）
- `app/ui/ui_statusbar.c` —— 自绘电池胶囊 + 紧凑布局
- `app/ui/ui_statusbar.h` —— 接口加 bool dark
- `app/ui/app_shell_ui.h/.c` —— 接口加 bool dark 透传
- `services/manager/notify_manager.{h,c}` —— 新增 `remove_at`
- `app/CMakeLists.txt` —— 加 ui_modal_card.c
- `framework/CMakeLists.txt` —— 移除 app_shell.c
- 8 个 `apps/*/<name>_app.c` —— `on_exit` → `on_app_exit`
- 8 个 page_*.c 调用 `app_shell_*` 的全部改回 `app_router_*` + include

### 调用方更新（statusbar dark 参数）
- 浅色：`launcher_pages.c` / `settings_home.c` / `settings_about.c` / `weather_pages.c` / **新 notifications**
- 深色：`music_pages.c` / `system_pages.c` / `settings_time.c`

---

## 5. 验证

### 状态栏
- [x] 电池胶囊正确显示百分比数字（87, 50, 12...）
- [x] 内填充条按 % 缩放，颜色随 OK/LOW/CRIT 切换
- [x] 蓝牙和电池视觉紧凑
- [x] 浅色页（白底）和深色页（深紫底）文字色都清晰

### 通知页列表
- [x] 长标题/正文 LONG_DOT 单行省略
- [x] 短消息和长消息卡高都是 64px，节奏整齐
- [x] 时间固定右上不被挤压

### 通知详情模态
- [x] 点列表项弹出
- [x] 多行 sender / 多行 body 都正常 wrap
- [x] 内容超长时整张卡内部滚动
- [x] 点空白关闭
- [x] 卡片下滑关闭
- [x] "删除"按钮删除该条 + 列表自动重建
- [x] 长按头部徽章弹"清空所有"确认

### 上滑退出
- [x] 在 hit zone 区域上滑 dy ≥ 30 → 退出 launcher
- [x] 列表区域上滑 → 列表正常滚动，不误触退出
- [x] 实测 dy=85 案例触发退出

---

## 6. 不在本次范围（下一轮）

- ❌ music / system / settings_time 页的列表/卡片美化（深色主题统一切到浅色）
- ❌ 模态层"删除"按钮的二次确认（如果用户怕误删）
- ❌ 通知详情里支持"标记已读"（需要 manager 加 read 状态字段）
- ❌ 通知按 category 分组显示

---

## 7. 一句话总结

把"卡片自适应高度被 CJK 光栅化压成两行钉一块 + LVGL gesture 在 CLICKABLE/scroll 场景失效"这两个隐性 LVGL 陷阱用"显式锁宽高 + 自算 dy 替代 gesture"两个手段彻底绕过，顺手把通知详情设计成可复用的模态层组件。
