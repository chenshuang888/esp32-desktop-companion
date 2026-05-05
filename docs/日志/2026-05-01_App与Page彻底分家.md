# App vs Page 彻底分家重构 工作日志

**日期**：2026-05-01
**分支**：feat/optimize_page
**作者**：ChenShuang + Claude

---

## 0. 起因

上一轮（4 月 30 日）做完 UI 设计系统 + weather/menu 美化后，新建了"设置 app"，把原本散在菜单里的"时间调节 / 亮度 / 关于"三个入口聚合进去。

落地之后用户立刻发现两个体验问题：

1. **概念混乱**：设置 app 内部又有"时间调节子页 / 关于子页"，但代码里它们和 weather / music 一样都是平铺的 page。"app 里还有 page" 这件事在心智模型上很别扭——用户视角的"设置 app"和代码里的 `PAGE_SETTINGS` 是 1:1，但用户视角的"设置 app 包含三屏"，代码里却是 1:3。
2. **没有 app 边界**：所有 page 都在一个 `PAGE_*` 枚举里，互相 `page_router_switch` 跳转。约定靠注释，物理上没强制——任何 page 想跳到任何其它 page 都能跳。

> "我现在在思考一个问题就是进入 app 之后还要不要带上屏幕最上面的状态栏"
> "对于一个 app 来说里面出现了多个页面让我感觉有点奇怪啊"
> "能不能做到更加的彻底一些的那种，app 就是 app, 然后页面就是页面"

最后定下：**做一次彻底重构**，app 和 page 完全分两层。

---

## 1. 目标 / 心智模型

**App 是"用户感知到的应用"**，page 是 "app 内部的一屏 UI"。

| 维度 | App | Page |
|---|---|---|
| 由谁创建 | launcher 用户点击 / lockscreen 系统启动 | app 自己内部决定 |
| 由谁管理 | `app_router`（顶层） | app 内部 `sub_router`（多 page app 才用） |
| 退出语义 | 退到 launcher（默认） / lockscreen（特殊） | pop 回上级 page |
| 互跳 | 不能直接跳 page，只能 enter / exit | 不能跨 app |
| 状态栏 | `immersive` flag 决定要不要画 | 跟随所属 app |

最关键的一条：**app 之间不能互跳 page**，只能 enter / exit。这条把"约定靠注释"变成了"物理上不允许"。

---

## 2. 关键决策

| 议题 | 决策 | 理由 |
|---|---|---|
| 路由分几层 | **两层**（app_router + sub_router） | 中间层"功能域路由"和"屏路由"职责完全不同，混在一层就是当前的问题 |
| App ID 类型 | **字符串**（"settings" 而不是 `PAGE_SETTINGS`）| 解耦枚举膨胀；日志里直接读 app 名；新增 app 不改 framework |
| sub_router 是否多实例 | **可多实例**（每个多 page app 自己 alloc 一个）| 未来 music app 想内部分歌单页/播放页 时直接 sub_router_create |
| 历史栈深度 | **4** | settings 最多 home → sub → sub-sub，4 层足够 |
| settings 子页退出手势 | **底缘 50px 上滑 pop** | 与 weather 同款，避免误触列表行 |
| home 处栈空怎么办 | **退出 app**（app_shell_exit_to_launcher） | 语义统一：上滑就是"返回上一级"，栈空就是"返回上一级 = 退出 app" |
| 沉浸式策略 | `immersive=true` 不挂 statusbar；其它由 app 自己用 `app_shell_attach_statusbar` 挂 | 不强制（保留灵活性），但提供唯一规范路径 |
| 文件搬迁 | **git mv** 保留 blame 历史 | 重构窗口期，blame 历史比代码风格统一更重要 |
| dynapp_host 怎么处理 prepare/commit | **在 app_router 上原样保留 commit_prepared** | 它是"后台准备 + 瞬切"的唯一路径，删了会回到"组件一个个冒出来"的视觉问题 |

---

## 3. 最终落地的目录结构

### 框架层（新建 3 + 删 1 个）

```
framework/
├── app_router.h/.c     —— 顶层 app 注册/进入/退出，字符串 ID，commit_prepared
├── sub_router.h/.c     —— 可多实例的 page 路由（push/pop history stack）
├── app_shell.h/.c      —— exit_to_launcher / exit_to_lockscreen helper
└── page_router.h/.c    —— 删除（功能整体替换）
```

`app/ui/` 增加：

```
app/ui/
└── app_shell_ui.h/.c   —— app_shell_attach_statusbar（依赖 ui_statusbar，
                            所以放 app/ui 而不是 framework）
```

### app 目录（按 app 边界归类）

```
app/apps/
├── lockscreen/
│   ├── lockscreen_app.h/.c                单 page，immersive
│   └── pages/page_lockscreen.c            ← page_time.c
├── launcher/
│   ├── launcher_app.h/.c                  单 page，immersive，含 modal
│   └── pages/
│       ├── page_launcher.c                ← page_menu.c
│       └── launcher_modal.c               ← page_menu_modal.c
├── weather/
│   ├── weather_app.h/.c                   单 page，immersive
│   └── pages/page_weather.c
├── notifications/
│   ├── notifications_app.h/.c             单 page
│   └── pages/page_notifications.c
├── music/
│   ├── music_app.h/.c                     单 page
│   └── pages/page_music.c
├── system/
│   ├── system_app.h/.c                    单 page
│   └── pages/page_system.c
├── settings/                              ★ 多 page 唯一例子
│   ├── settings_app.h/.c                  含 sub_router 实例
│   └── pages/
│       ├── settings_home.c                ← page_settings.c
│       ├── settings_time.c                ← page_time_adjust.c
│       └── settings_about.c               ← page_about.c
└── dynapp_host/
    ├── dynapp_host_app.h/.c               immersive，容器，N 个 JS app 复用
    └── pages/page_dynapp_host.c           ← page_dynamic_app.c
```

`app/pages/` 整个目录被删除。

---

## 4. 关键架构设计

### 4.1 app_descriptor_t —— App 的唯一身份证

```c
typedef struct {
    const char *id;                 // "settings"，全局唯一
    const char *display_name;       // "设置"，launcher 显示
    const char *menu_icon;          // ICON_* UTF-8 字面量；NULL = 不在 launcher 出现
    uint32_t    menu_icon_color;
    bool        immersive;          // true = 不挂 statusbar
    bool        show_in_menu;       // false = lockscreen / launcher / dynapp_host

    lv_obj_t *(*on_enter)(void);
    lv_obj_t *(*on_enter_with_arg)(const char *arg);   // dynapp_host 专用
    void      (*on_exit)(void);
    void      (*on_tick)(void);
} app_descriptor_t;
```

每个 app 模块定义一个 `const app_descriptor_t XXX_APP = { ... }`，在 `app_main` 里 `app_router_register(&XXX_APP)`。

### 4.2 launcher cell 表不再写死

老的 launcher（page_menu）里维护一张 `s_static_defs[]`，加新 app 要改两处（注册 + 表）。新版 launcher 直接调：

```c
const app_descriptor_t *apps[MAX_STATIC_APPS];
uint8_t n = app_router_get_visible_apps(apps, MAX_STATIC_APPS);
```

只看 `show_in_menu=true && menu_icon != NULL` 的 app。**新增 app = 加一行 register，launcher 自动收**。

### 4.3 settings 多 page：sub_router

`settings_app` 进入时：

```c
s_router = sub_router_create(/*pages=*/4, /*history=*/4);
sub_router_register(s_router, SETTINGS_PAGE_HOME,  settings_home_get_callbacks());
sub_router_register(s_router, SETTINGS_PAGE_TIME,  settings_time_get_callbacks());
sub_router_register(s_router, SETTINGS_PAGE_ABOUT, settings_about_get_callbacks());
sub_router_push(s_router, SETTINGS_PAGE_HOME);
return lv_scr_act();
```

子页只通过 `settings_app.h` 暴露的两个函数互动：

```c
void settings_app_push(settings_page_id_t id);
void settings_app_pop_or_exit(void);   // 栈空则 exit_to_launcher
```

**子页之间不直接 include sub_router 实例**，避免硬耦合。settings 内部 router 的存在对子页是透明的。

### 4.4 dynapp_host 保留双阶段切换

旧 `page_router_commit_prepared` 在新 app_router 里改名同语义保留：

```c
esp_err_t app_router_commit_prepared(const char *app_id, lv_obj_t *prepared_screen);
```

flow 不变：

```
launcher 点击动态 cell
   → dynapp_host_prepare_and_enter("calc")
     → off-screen build subtree + start script
     → ready_cb / 800ms timeout
       → app_router_commit_prepared("dynapp_host", screen)
```

切其它 app 前 launcher 仍调 `dynapp_host_cancel_prepare_if_any()` 兜底。

### 4.5 fs_worker hook 改成读 app id

```c
static bool is_app_running(const char *name) {
    const char *cur = app_router_current_id();
    if (!cur || strcmp(cur, "dynapp_host") != 0) return false;
    const char *running = dynamic_app_registry_current();
    return running && strcmp(running, name) == 0;
}
```

字符串比较取代了 `PAGE_DYNAMIC_APP` 枚举判断；行为完全一致。

### 4.6 主循环：page_router_update → app_router_tick

```c
// 旧
page_router_update();
// 新
app_router_tick();   // 转发到 current app on_tick
                     // 多 page app（settings）on_tick 内部再转发到 sub_router_tick
```

---

## 5. 顺手修的隐藏 leak

### 5.1 launcher 的 upload_manager 观察者

旧实现里 `dynapp_upload_manager_register_status_cb(on_upload_status)` 在 page on_enter 注册，但**永远不注销**。manager 容量 4，重复注册靠 idempotent 兜底，但每次重建 launcher 都要走一遍这个调用（注销失败也不会有人提醒）。

新实现把它移到 `launcher_app_module_init()`，在 `app_router_register` 之后、第一次 enter 之前调一次。重建 launcher screen 不再触发注册。

### 5.2 menu_modal 在 layer_top 残留

旧实现：modal 挂 `lv_layer_top()`，跨 screen 存在。如果用户长按出 modal 不操作，直接切其它 app（比如某个 BLE 推送驱动的自动跳转），modal 会在新 app 上残留。

新实现：launcher `on_exit` 里调 `launcher_modal_dismiss()` 兜底销毁。

---

## 6. 文件改动清单

### 新建（17 个）
- `framework/app_router.h/.c`
- `framework/sub_router.h/.c`
- `framework/app_shell.h/.c`
- `app/ui/app_shell_ui.h/.c`
- `app/apps/lockscreen/lockscreen_app.h/.c`
- `app/apps/launcher/launcher_app.h/.c`
- `app/apps/weather/weather_app.h/.c`
- `app/apps/notifications/notifications_app.h/.c`
- `app/apps/music/music_app.h/.c`
- `app/apps/system/system_app.h/.c`
- `app/apps/settings/settings_app.h/.c`
- `app/apps/dynapp_host/dynapp_host_app.h/.c`

### 重命名 + 改内容（git mv 保留 blame）
- `app/pages/page_time.{c,h}` → `app/apps/lockscreen/pages/page_lockscreen.{c,h}`
- `app/pages/page_menu.{c,h}` → `app/apps/launcher/pages/page_launcher.{c,h}`
- `app/pages/page_menu_modal.{c,h}` → `app/apps/launcher/pages/launcher_modal.{c,h}`
- `app/pages/page_weather.{c,h}` → `app/apps/weather/pages/page_weather.{c,h}`
- `app/pages/page_notifications.{c,h}` → `app/apps/notifications/pages/page_notifications.{c,h}`
- `app/pages/page_music.{c,h}` → `app/apps/music/pages/page_music.{c,h}`
- `app/pages/page_system.{c,h}` → `app/apps/system/pages/page_system.{c,h}`
- `app/pages/page_settings.{c,h}` → `app/apps/settings/pages/settings_home.{c,h}`
- `app/pages/page_time_adjust.{c,h}` → `app/apps/settings/pages/settings_time.{c,h}`
- `app/pages/page_about.{c,h}` → `app/apps/settings/pages/settings_about.{c,h}`
- `app/pages/page_dynamic_app.{c,h}` → `app/apps/dynapp_host/pages/page_dynapp_host.{c,h}`

### 修改
- `framework/CMakeLists.txt` —— SRC 加 3 删 1
- `app/CMakeLists.txt` —— SRC 完全重写、INCLUDE_DIRS 全 app 路径
- `app/app_main.c` —— `page_router_*` 全替换为 `app_router_*`，10 register → 8 register

### 删除
- `framework/page_router.h/.c`
- `app/pages/`（空目录）

---

## 7. App 归类与 immersive 设置

| App ID | display | icon | color | immersive | show_in_menu | 多 page |
|---|---|---|---|---|---|---|
| `lockscreen` | — | — | — | true | false | 否 |
| `launcher` | — | — | — | true | false | 否（含 modal） |
| `weather` | 天气 | ICON_WEATHER | 0xF59E0B | **true** | true | 否 |
| `notifications` | 通知 | ICON_NOTIFICATIONS | 0xFF3B30 | false | true | 否 |
| `music` | 音乐 | ICON_MUSIC | 0xAF52DE | false | true | 否 |
| `system` | 系统 | ICON_TUNE | 0x3C3C43 | false | true | 否 |
| `settings` | 设置 | ICON_SETTINGS | 0x6E6E73 | false | true | **是** |
| `dynapp_host` | — | — | — | true | false | 否 |

---

## 8. 跳转图（重构后）

```
                          lockscreen
                              ↑↓ 上滑/下滑
                          launcher
        ┌─────────┬───────┬───┴───┬─────┬─────┬───────────┐
        ↓         ↓       ↓       ↓     ↓     ↓           ↓
     weather  notifications music system settings dynapp_host(N 个 JS app)
                                          │
                                  sub_router
                                  ├── home (列表)
                                  ├── time (调节)
                                  └── about (信息)
                                  ↑
                                home 上滑→home pop_or_exit→launcher
                                time/about 上滑→pop 回 home
```

**约束**：横向虚线（app 之间）只能走 `app_router_enter`；纵向虚线（settings 内部）只能走 `sub_router_push/pop`。

---

## 9. 验证

测试结果用户已确认通过：

- [x] launcher → weather / notifications / music / system / settings 都能进
- [x] 各 app 上滑 / back → 回 launcher
- [x] launcher 下滑 → lockscreen；lockscreen 上滑 → launcher
- [x] settings: home → time / about 进入；time / about 底缘上滑 → 回 home；home 底缘上滑 → 回 launcher
- [x] launcher → dash / calc 等 JS app 启动正常，无白屏
- [x] 长按动态 app cell → modal 删除工作；删除当前正在跑的 app 被 fs_worker hook 拒绝
- [x] 长按出 modal → 不操作直接切其它 app → 回 launcher 时 modal 不残留

---

## 10. 设计原则沉淀

1. **概念分层 vs 实现分层**：用户感知"app"和"page"是两层，代码就该是两层。混在一起会出现"代码里都是 page，但用户想的是 app"这种鸿沟，要么用户别扭，要么开发者添加新功能时把握不住边界
2. **多实例 router 优于单实例**：原 page_router 是 `static s_router`，所有 page 一锅炖。改成 `sub_router_create()` 让 app 自己持有，相当于把"app 内的命名空间"还给 app
3. **物理边界 vs 注释边界**：`app_router_enter("xxx")` 强制 app 间只能整体切换；老 `page_router_switch` 任何 page 想跳哪都行，全靠注释约束。物理边界比注释更可靠
4. **重构窗口期要趁早**：当前 9 个静态 page + 几个动态 app，搬完只用一晚上。如果再加 5 个 app，搬迁工作量翻倍且易出错
5. **新增 app 流程要简单**：现在加新 app = 4 步（写 page 实现、写 app 壳、加 CMake、register）。launcher 自动收。低摩擦的扩展才会真的扩展

---

## 11. 不在本次范围（下一轮工作）

- ❌ 后台 app（多个 app 同时存在；当前一时一 app）
- ❌ App 间通信（如 notifications app 推送到 lockscreen）
- ❌ 动画式切换（淡入淡出 / 滑动）
- ❌ history 深度 > 4
- ❌ 把还在用旧深紫主题的 page 顺带迁移到新 token（time / notifications / music / system / time_adjust 内部 UI 仍是旧主题）
- ❌ 给 notifications / music / system 加统一 statusbar（这些页布局按"无 statusbar + 顶部 30px back 按钮"设计，强行挂会冲突）

---

## 12. Commit 顺序建议

每步独立可 build、独立可回退：

1. `feat(framework): add app_router + sub_router + app_shell skeleton` —— Phase 1，旧 page_router 仍保留
2. `chore: git mv pages -> apps/<id>/pages (no content change)` —— 纯文件移动，git 识别为 rename
3. `refactor(apps): wrap each page as app_descriptor + switch routing api` —— Phase 2 内容修改
4. `chore: remove framework/page_router; update CMake + app_main` —— 删旧
5. `refactor(launcher): one-shot upload observer + modal dismiss on exit` —— Phase 4 修 leak
6. `feat(ui): app_shell_attach_statusbar helper; migrate launcher/settings_home/about` —— Phase 3 状态栏

---

## 13. 一句话总结

把"所有 page 在一个 PAGE_* 枚举里平铺、互相 page_router_switch"重构为"app 是字符串 ID 的功能域、app 之间只能 enter/exit、多 page app 自持 sub_router 管子页"，让"用户感知的 app"和"代码里的 page"成为正交两层，并顺手修了两个跨 app 的隐藏 leak。
