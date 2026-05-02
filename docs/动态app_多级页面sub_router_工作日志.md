# 动态 App 多级页面 Router + hidden 原语 工作日志

**日期**：2026-05-02
**分支**：feat/optimize_page
**作者**：ChenShuang + Claude

---

## 0. 起因

五子棋日志收尾时盘点了一下"动态 app 平台是否成熟到可以批量造 app"。结论是 80% 达标，**最大缺口是多级页面**——所有 app 想做"列表 → 详情 → 设置"这种多级流，要么用 modal 绕过（doodle / gomoku），要么自己手动管理"哪一页显示"的状态（alarm_pkg）。

用户拍板这一轮就专注解决这个：**让动态 app 平台真正支持 push/pop 路由**，这一关过了之后才能说"可以放心批量造 app"。

---

## 1. 关键决策

### 1.1 纯 JS 路由 + hidden 切换 vs C 层 sub_screen 栈

三个候选：

| 方案 | 实现 | 评价 |
|---|---|---|
| A. **JS 层路由 + hidden 切换** | prelude 加 Router；多个页 panel 全 mount，切页改 hidden | 0 native；可定制；保留页内 state |
| B. JS 层路由 + lazy mount/destroy | 每次切页 destroy 老页 + mount 新页 | 内存最优；切页有重建开销 |
| C. C 层 sub_screen 栈 | 每页一个独立 LVGL screen，加 native push/pop | 切页有动画；改动巨大 |

选 **A**——一次到位解决 80% 真实场景，深栈场景（5+ 级）实际不存在。用户多次表达过"能纯 JS 解决就纯 JS"的偏好，C 方案违反这条直接 pass。

**实际落地是 A 的变种**：pop 时旧页真正 destroy（释放内存）、push 时下层页保留 hidden（保留 state）。这样既有 lazy 化的内存优势，又保留下层 state 不丢——push 入栈链上的所有页都还在，但栈外的"已经走了"的页彻底销毁。这个实际是 A 和 B 的折中。

### 1.2 hidden 必须是新原语，不是用 size=[0,0] 模拟

第一直觉是用 size=[0,0] 临时收起。问题：LVGL flex / align 会重新布局，下次 unhide 还得记原 size。而且 z-order 也会变。

`LV_OBJ_FLAG_HIDDEN` 是现成的，对 flex / align 完全透明。加一个 `STYLE_HIDDEN` key 共 ~10 行 C 代码，**而且这个 key 本身有独立价值**（业务隐藏 / 显示元素是高频需求，doodle 画廊、gomoku modal 之前手动 destroy/recreate 都是因为没 hidden）。属于"顺便补的好用 primitive"。

### 1.3 Router 不接管 setInterval / ble.on

考虑过让 Router 自动给每页一个"页内 setInterval"作用域，离开自动清。但这会让 API 复杂，业务还得想清楚"是页内 timer 还是 app 级 timer"。

**最终决定不接管**。文档里明确：业务在 `Router.onLeave(name, ...)` 里自己 `clearInterval`。简单一致，符合"一个 app 一个 JS 上下文，全局变量管全部状态"的现有心智。

### 1.4 屏底上滑：栈深 ≥ 2 自动 pop，栈深 = 1 穿透到宿主

这条是 Router 让用户"无感"的关键。

宿主层有一个 30px hit zone 在最底层（z-order 最低），dy ≥ 30 时退出 app。Router 启动时在所有 page panel 之上加一个自己的 swipe zone，逻辑：
- 栈深 ≥ 2：吞事件 + `Router.pop()`
- 栈深 = 1：**不响应**，让事件穿透到宿主层 hit zone 退出 app

LVGL 的 hit testing 默认会让事件先到 z-order 最高的 obj。如果 z-order 最高的 obj `not clickable` 或 `bg_opa = 0` 而且没有 event_cb 处理 PRESSED——事件会下沉到下一层。所以"不响应 = 不处理事件"等价于"穿透"。

实际实现是：上滑触发时检查 stack.length，≥2 才调 pop；=1 时 swipe zone 啥也不干，事件继续传给宿主层 hit zone（它有自己的 PRESSED/RELEASED 监听）。

### 1.5 Max depth = 4

> Q：栈深限制定多少？
> A：4。理由是更深的 UX 本身就是反模式（用户记不住自己在哪一级）。

iOS 设置 app 最深也就 3-4 级。超 4 时 `sys.log` warn + ignore，不强行禁止但提示业务做错了。

### 1.6 同名页递归 push 自动加后缀

业务场景：列表 → 详情 → "相关条目" 列表 → 另一个详情。两次 `push('detail', ...)` 会让 vnode id 冲突。

Router 内部维护 `counters[name]`，每次 push 这个 name 时 counter +1，slot id 拼成 `name__r2` / `name__r3` 等。业务代码里始终用 logical name（`Router.push('detail', ...)`），slot 实现细节不暴露。

### 1.7 强制根 vnode 是 panel 类型

builder 必须返回 `panel`，否则 sys.log 报错且不挂载。Router 自动给根强塞 `id`、`size: [-100, -100]`、`pad: [0, 0, 0, 0]`，业务别的 props 保留。

业务需要纵向布局子节点要自己设 `flex: 'col'`——Router 不接管这层（避免和 doodle / gomoku 这种"自由布局根"冲突）。

---

## 2. 落地清单

### 2.1 Phase 1：hidden 原语（C + prelude）

**改动（4 处，共 ~15 行）**

- `dynamic_app/dynamic_app_ui.h` —— 加 `DYNAMIC_APP_STYLE_HIDDEN` enum
- `dynamic_app/dynamic_app_ui_styles.c` —— 加 case 调 `lv_obj_add/clear_flag(LV_OBJ_FLAG_HIDDEN)`
- `dynamic_app/dynamic_app_natives.c` —— `sys.style.HIDDEN` 暴露
- `dynamic_app/scripts/prelude.js` ——
  - applyStyle 处理 `props.hidden`
  - STYLE_KEYS 加 `'hidden'`（让 VDOM diff 能感知该 prop 变化）

### 2.2 Phase 2：Router（仅 prelude.js）

**改动（1 文件，+~180 行）**

`dynamic_app/scripts/prelude.js` 末尾新增 `Router` 模块：
- 内部维护 `defs / enterCbs / leaveCbs / stack / counters / mounted`
- `start / push / pop / replace / popTo / current / depth`
- `onEnter / onLeave`
- 启动时 `ensureSwipeZone()` 挂自己的上滑拦截 panel
- push/pop/replace 时调 `mountPage` / `setVisible` / `VDOM.destroy`

完全 0 native 改动。

### 2.3 Phase 3：settings_pkg 验证 app

**新建（2 文件）**

- `dynamic_app/scripts/settings_pkg/manifest.json`
  ```json
  {"id": "settings_pkg", "name": "设置", "icon": "SETTINGS",
   "iconColor": "ACCENT_2", "version": "1.0.0"}
  ```

- `dynamic_app/scripts/settings_pkg/main.js`（~280 行）
  - 6 个页：`list / display / about / network / detail_ble / danger`
  - 演示 `push / pop / replace / popTo`
  - `onLeave('display')` 落盘 NVS（避免每次 +/- 写盘）
  - `topbar(title, onBack?)` 通用返回栏组件（左 "<" + 标题）

页结构：

```
list（主列表）
├─ display     亮度+/-按钮 + 深色模式切换
├─ about       固件信息 KV 列表 + "replace 演示"按钮
├─ network     → push detail_ble (三级)
├─ detail_ble  BLE 详情 + popTo('list') 演示
└─ danger      恢复默认 + modal 二次确认
```

### 2.4 Phase 4：文档

- 更新 `docs/动态app_JS_API速查.md`
  - §0 全局对象表加 Router
  - §1.3 props 全集加 `hidden`
  - **新增 §12 Router 章节**（心智模型 / 最小例子 / 约束默认 / 何时不用）
- 本日志

---

## 3. 几个设计上的取舍记录

### 3.1 为什么 push 时下层页 hidden 而不是 destroy

如果 push 时把下层页 destroy，pop 回来时要重新 mount + 重建所有子节点 + 重新 saveState。代价：
- 列表页有 100 行时，每次 pop 回来都重新 build → 卡顿
- 列表的滚动位置丢失
- 列表里有 canvas（缩略图）的话，canvas 内容全部清零

而 hidden 切换：LVGL 内部状态全部保留，pop 回来 hidden=false 一句话恢复，**零开销**。代价是栈深越深内存越多，但 max depth = 4 + iOS 设计美学下每页通常很轻 → 不是问题。

### 3.2 为什么 pop 时旧页 destroy

反过来：pop 走的页是"用户已经离开了"的页，重新进入时**应该是新的状态**（重新读 props、重新 build）。如果 pop 时 hidden 不 destroy，下次 push 同名页要么 id 冲突要么用旧 vnode（业务期望全新）。

也为了内存——栈深 4 时如果没人 destroy，每次 push/pop 都长内存，N 次后炸。

**所以 push = 下层 hidden、上层 mount；pop = 当前 destroy、下层 unhide**。这是一对不对称的操作但符合直觉。

### 3.3 onEnter 触发时机

**push 进入** → 触发 `onEnter('newPage')`
**pop 回退到本页** → 触发 `onEnter('thisPage')` ← 这条容易漏
**replace** → 触发 `onLeave('old')` + `onEnter('new')`
**start** → 触发 `onEnter('first')`

意思是 onEnter **只在这页"成为栈顶"时触发**，不论从哪儿来。这样业务在 onEnter 里做"刷新数据 / 同步 UI"不需要关心来源。

### 3.4 attachRootListener 每页都要调

第一版漏了——以为 attach 一次就够。但看 `on_lv_root_event` 实际是按"哪个 obj 收到事件 → 查 registry"工作的，**每个挂了 cb 的 obj 才会触发分发**。子页内的按钮点击，子页 root panel 没挂 cb 就直接断在 LVGL，根本到不了 JS。

修法：push 时新页 mount 完成后调 `sys.ui.attachRootListener(slot)`。pop 时不需要 detach（页 destroy 时 LVGL 自己清 cb）。

### 3.5 不做转场动画

LVGL 转场动画（左滑/右滑）成本不便宜，而且会出"动画期间用户又点 push 怎么办"的并发问题。**先不做**——栈切换是即时的，体验上不如 iOS 但不至于难用。用户觉得需要时再加。

---

## 4. 验证设计（settings_pkg 怎么覆盖完整流程）

| 操作路径 | 覆盖 Router API |
|---|---|
| 启动 app | `Router.start('list')` |
| list → display → 调亮度 → pop | `push` + `pop` + `onLeave` 落盘 |
| list → display → 调亮度 → 上滑退出 | swipe zone 拦截 + pop 自动触发 |
| list → about → 点 "replace 演示" → pop | `replace`（替换栈顶为 danger） |
| list → network → BLE 详情 → 点 "回首页" | `popTo('list')` 跨级回退 |
| list → danger → 二级确认 → 取消 | `UI.modal` 不影响 Router 栈 |
| 在 detail_ble 上滑两次 | 第 1 次 pop 到 network，第 2 次 pop 到 list |
| 在 list 上滑 | swipe zone 不响应 → 穿透到宿主层 → 退出 app |

最深栈：`list → network → detail_ble`（3 级），余 1 级给意外深度。

---

## 5. 文件改动清单

### 新建（3）
- `dynamic_app/scripts/settings_pkg/manifest.json`
- `dynamic_app/scripts/settings_pkg/main.js`
- `docs/动态app_多级页面sub_router_工作日志.md`（本文件）

### 修改（5）
- `dynamic_app/dynamic_app_ui.h` —— `DYNAMIC_APP_STYLE_HIDDEN` enum
- `dynamic_app/dynamic_app_ui_styles.c` —— hidden case
- `dynamic_app/dynamic_app_natives.c` —— `sys.style.HIDDEN` 暴露
- `dynamic_app/scripts/prelude.js` ——
  - applyStyle 加 hidden 分支
  - STYLE_KEYS 加 'hidden'
  - **新增 Router 模块**（~180 行）
- `docs/动态app_JS_API速查.md` —— §0 全局表 / §1.3 props / 新增 §12 Router 章节

---

## 6. 平台能力覆盖盘点（更新）

| 路径 | 谁验证 |
|---|---|
| 单页布局 + 命令式绘图 | 全部 app |
| sys.canvas 像素绘图 | doodle |
| BLE 真双向 + present 心跳 | gomoku |
| mailbox 离线归档 | notif |
| **多级页面栈（push/pop/replace/popTo）** | **settings**（首次） |
| **hidden 切换原语** | **settings + Router 内部** |
| **跨页生命周期（onEnter/onLeave）** | **settings**（首次） |
| **同名页递归 push（自动 id 后缀）** | settings 没用，但已实现 |

仍未覆盖：
- ❌ 多 canvas 并存（理论支持没人压过）
- ❌ Router 嵌套（page 内嵌另一个 Router）—— 单层够用，不做
- ❌ 转场动画 —— 不做
- ❌ 平台 onExit 钩子（gomoku 日志已讨论结论：不做）

---

## 7. 一句话总结

**hidden 原语补 + Router 模块到位 + settings_pkg 验证一遍**，动态 app 平台从此可以**无障碍承载多级流业务**——任何"列表→详情→设置"形态的 app 现在都能用 ~80 行 JS 写完。这是动态 app 平台从"能玩"到"能造"的临门一脚。

---

## 8. 接下来"批量造 app" 的口袋清单

平台能力到位后，下一波可以按需做的 app（用户场景驱动，不强求都做）：

| app | 用 Router 吗 | 用什么核心能力 |
|---|---|---|
| 笔记本（短笔记） | ✅ list/detail | NVS state + Router |
| 习惯打卡 | ❌ 单页 | 已有 habit_pkg 雏形 |
| 单局 / 双局小游戏 | ❌ 单页 | canvas |
| 食谱 / 待办 | ✅ list/detail | NVS + Router |
| 健康指标日记 | ✅ list/detail/chart | 缺 lv_chart 控件 |
| 多页设置类 | ✅ | settings_pkg 已做 |
| 番茄钟 / 计时器 | ❌ 单页 | setInterval |
| 倒数日 | ❌ 单页 | sys.time |
| 日历 / 日程 | ✅ month/day | 缺 lv_grid 复杂控件 |

> 红线：**做新 app 时如果发现"必须加 native"，先质疑业务是不是该砍**。
> Router / hidden 这一轮做完，未来 6 个月内大概率没什么必须加 native 的需求。

---

## 9. 关键经验沉淀

### 9.1 加平台能力前先确认"路径之缺"，不是"东西之缺"
之前讨论 onExit 时容易陷入"这个 hook 听起来很合理"的陷阱。实际问题是"对端怎么知道我离场"——而这个问题用 watchdog 已经能解。**先盯路径（路径走得通吗）再考虑形式（要不要加 hook）**，避免设计冲动。

### 9.2 push 是非对称操作（hidden 走 + mount 来）；pop 是另一种非对称（destroy 走 + unhide 来）
不要追求"push 和 pop 完全镜像"——它们在内存模型上要求就不一样：栈底要保留 state，离开栈的页要释放。把这两条规则写到注释里能省后人半小时。

### 9.3 attachRootListener 每个 root 都要调
`on_lv_root_event` 是 per-obj 的 lv_obj_add_event_cb 注册，不是全局事件总线。任何"独立的 root panel"都要 attach 一次。这条规则需要写进 §12 文档（已加）。

### 9.4 hidden 是个独立有价值的 primitive
我最初是为了 Router 加 hidden，但写完发现 doodle 画廊、gomoku modal 二次确认、计算器多面板等场景都受益——以后业务想"暂时收起一个 panel"再也不用 destroy/recreate 了。**好 primitive 的标志是它解锁多个场景**，不只是为某一个特定需求服务。

### 9.5 Router 不接管 setInterval / ble.on 是对的
"全包"风格的框架（接管所有副作用）会让初学者写不清楚边界，复杂业务里又约束太多。**只接管 mount/unmount 生命周期，副作用让业务自己用 onLeave 清**——这个分割线和 React 类似，业务代码长得也像 React，迁移过来零成本。
