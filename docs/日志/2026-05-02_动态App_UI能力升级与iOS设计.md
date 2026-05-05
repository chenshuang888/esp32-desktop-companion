# 动态 App UI 能力升级：补 sys.* 地基 + iOS 设计系统 + 通知 app 复刻 工作日志

**日期**：2026-05-02
**分支**：feat/optimize_page
**作者**：ChenShuang + Claude

---

## 0. 起因

五大原生 app（音乐 / 系统 / 设置 / 时钟 / 通知）UI 升级落地后，开始反观动态 app。
肉眼对比之下：动态 app 视觉明显比原生丑——配色土、布局散、按钮反馈弱、字体单调。

带着"动态 app 是不是注定不如原生"这个疑问做了能力对比，结论：**不是 LVGL 渲染输给原生，
是 sys.ui 能力面太窄**。原生 app 用了 30+ 种 LVGL 样式 setter，动态 app 只暴露 12 个
样式 key；原生用 `lv_arc / lv_slider / 自绘 line / 旋转图片`，动态只有 label/panel/button/image。
最关键的是：**没有共享的设计 token**，所以每个动态 app 都自己写裸颜色裸 padding，互相不一致。

本轮做了三件事：
1. 给 dynamic_app 补"地基级"UI 能力（C 端 + JS prelude 双侧扩展）
2. 在 prelude.js 沉淀一套 iOS 浅色设计系统（13 个高阶组件）
3. 用补全后的能力**复刻原生通知 app**，作为压力测试 + 参考实现

---

## 1. 关键决策

### 1.1 边界判定：什么进固件 / 什么留 JS

跟用户深聊后定下两条尺子：

| 判定 | 该不该进固件 |
|---|---|
| 涉及 OS 资源（LVGL 指针 / NVS / BLE / 时间） | ✅ 必须 |
| 多数 app 形态稳定（模态 / toast / 状态栏 / 上滑退出） | ✅ 进 |
| 多数 app 都需要但每家都不一样（数据 schema / 协议 / 列表样式） | ❌ 留 JS |
| 只有少数 app 需要（仪表盘控件、网格游戏） | ❌ 留 JS |

**反例修正**：之前一度想"列表组件 / 卡片样式 / 数字滚动"也都进固件，但被否决——这些每家都不一样，
固件硬塞会限制创意。最终方案：**纯 JS 默认实现**，写在 prelude.js 里随 app 加载，业务可直接用也可绕过。

### 1.2 不接入 notify_manager

用户明确反对动态 app 调原生 `notify_manager`，理由：

- `notify_manager` 是为原生通知 app + ANCS-style PC 协议设计的，schema/容量/NVS key 都写死
- 动态 app 的卖点是"用户自定义"，被锁死 schema 就退化成"通知 app 的 JS 皮"
- `sys.ble.send/onRecv` 已经是干净字节管道，每个 app 自定协议自管数据，更灵活

→ notif_pkg 完全自管 BLE 协议（`{type:"add", body:{title,body,ts,cat}}`）+ 自管存储（`sys.app.saveState`）。

### 1.3 模态 / toast / fadeIn 进固件

虽然纯 JS 也能拼出"全屏遮罩 + 居中卡 + 多按钮"的模态，但形态稳定 + 高频 + 边界条件多
（按钮按下时机、点遮罩 vs 点卡片、下滑关闭、淡入淡出），每个 app 自己写易出 bug。

**进固件，C 端用 LVGL 一份代码服务所有 app**。JS 端通过 `sys.ui.modal({...})` 触发，
按钮结果通过新增的 `EV_MODAL` 事件回传到 JS dispatcher。

### 1.4 宿主层接管"状态栏 + 上滑退出"

之前 dynapp_host 顶部是丑陋的"返回 + Dynamic App"占位，每个动态 app 显得很"非原生"。

改造：
- dynapp_host 直接挂 `app_shell_attach_statusbar(false)` 浅色状态栏
- 屏底 28px hit zone 自算 dy → `app_router_exit_to_launcher()`
- list_root 对齐到 24px 起、高 268px

→ **所有动态 app 自动获得**："顶部电池/蓝牙/时间状态栏 + 屏底上滑回 launcher"，无需 JS 处理。

### 1.5 dynamic_app 不依赖 app 组件（避免循环依赖）

想直接调 `ui_modal_card_*` 复用原生组件，但 `app` 已依赖 `dynamic_app`，反向依赖会形成环。

最终方案：**在 dynamic_app_ui.c 里自包含 LVGL 实现** modal/toast/fadein（约 210 行）。
颜色 token 硬编码与 ui_tokens.h 一致（0xF2F2F7 / 0xFFFFFF / 0x007AFF...），重复但安全。

---

## 2. 落地清单

### 2.1 C 端：dynamic_app_ui.h/c —— Style key & 命令扩展

**新增 9 个 style key**（数值含义见 `dynamic_app_style_key_t` enum 注释）：
| key | 用途 |
|---|---|
| `OPA / BG_OPA` | 透明度（整体 / 仅背景） |
| `FLEX_GROW` | flex 弹性伸展 |
| `TEXT_ALIGN` | left/center/right |
| `LONG_MODE` | label 截断模式（wrap/dot/scroll/clip） |
| `ROTATION` | 通用旋转（不限 image） |
| `FLEX_ALIGN` | flex 对齐细控（main/cross/track） |
| `BORDER` | 完整边框（color/width/side/opa）|
| `PRESSED_BG` | 按下态背景色（按钮反馈）|

**新增 4 个 cmd**：
- `SHOW_MODAL` —— 模态卡（title + body + 0~2 按钮 + modal_id）
- `TOAST` —— 屏底浮一条
- `FADE_IN` —— 给已存在对象做透明 0→cover 动画

**新增 1 个事件类型** `EV_MODAL`：modal 按钮按下时通过事件队列回 JS，dx=按钮索引 / -1=取消。

**字体扩展**：`set_fonts` 从 3 字体（text/title/huge）扩到 6（+ icon24/icon36/num_m）。

### 2.2 C 端：dynamic_app_natives.c —— 6 个新 native + 常量大爆炸

新 native：
- `sys.time.now()` —— `time(NULL)` 当前 unix 秒
- `sys.time.parts(ts)` —— `localtime_r` 拆 `{y,mo,d,h,mi,s,wday,yday}`
- `sys.time.format(ts, fmt)` —— `strftime` 包装
- `sys.ui.modal(opts)` —— 入队 SHOW_MODAL
- `sys.ui.toast(text, durMs)` —— 入队 TOAST
- `sys.ui.fadeIn(id, delayMs)` —— 入队 FADE_IN

新常量挂载：
- `sys.icons.*` —— 16 个 Material Symbols codepoint（与 `app/app_fonts.h::ICON_*` 同步）
- `sys.tokens.*` —— 13 颜色 + 6 间距 + 4 圆角 + 3 时长（与 `ui_tokens.h` 同步）
- `sys.style.*` —— 9 个新 key
- `sys.font.*` —— `ICON_24=3 / ICON_36=4 / NUM_M=5`

### 2.3 C 端：page_dynapp_host.c —— 宿主层视觉契约

**砍**：返回按钮（90×30）+ "Dynamic App" 标题 + 自定义 style 配色

**加**：
- `app_shell_attach_statusbar(false)` 浅色状态栏（24px）
- 屏底 28px 透明 hit zone + 自算 dy（PRESSED/PRESSING/RELEASED） + dy≥30 退出
- list_root 对齐 (0,24)，size = 240×(320-24-28) = 240×268

**结果**：动态 app 与原生 app 在状态栏、退出手势上行为完全一致。

### 2.4 JS 端：prelude.js —— UI 设计系统（核心增量）

文件从 354 → 716 行。

**核心模块** `UI`：
```
UI.T              设计 token（颜色/间距/圆角/动画时长，sys.tokens 短名）
UI.I              图标常量（sys.icons 短名）
UI.screen         全屏底容器
UI.title          页面大标题
UI.card           白底卡片+1px+圆角14
UI.kvRow          两端对齐键值行
UI.listRow        iOS 列表行（图标块+label+chevron+按下高亮）
UI.iconBtn        透明图标按钮+按下蒙层
UI.pillBtn        胶囊按钮（accent 蓝底）
UI.badge          圆角胶囊小标签
UI.statusBar      业务自绘标题区（44px）
UI.divider        水平 1px 分隔线
UI.hitZone        屏底退出区（一般用不到，宿主已兜底）
UI.modal          C 端模态的 JS 包装（自动分配 modal_id 注册回调）
UI.toast          薄包装
UI.fadeIn         薄包装
UI.swipeExit      给 children 末尾加 hitZone
```

**应用样式 props 扩展**：
- `applyStyle` 新增识别 9 个 prop：`opa / bgOpa / grow / textAlign / longMode / rotate / flexAlign / border / pressedBg`
- `STYLE_KEYS` diff 列表同步扩展
- `dispatch` 加 EV_MODAL（type=6）特殊路由：node_id 是 modal_id，dx 是按钮索引，从 modalCbs 表查回调

### 2.5 测试包：notif_pkg/main.js —— 通知 app 完整复刻

约 250 行 JS，覆盖：
- iOS 浅色 64px 卡片列表 + 类别色块（msg=蓝 mail=橙 call=绿 cal=紫 alert=红...）
- 标题区右上徽章 + 长按弹"清空所有"模态
- 单击列表项 → 详情模态（含完整时间戳 + 删除/关闭按钮）
- 5 条假数据演示 + BLE 协议 `{type:"add"|"clear"}` + sys.app 持久化
- 1Hz 时间显示刷新
- 完全不依赖 notify_manager

视觉对比原生通知 app：**几乎一致**，连按下高亮、卡片节奏、空状态铃铛图标都一样。

---

## 3. 几次"踩坑 + 修复"

### 3.1 esp-mquickjs API 与标准 QuickJS 不同

写 `sys.time.parts/format` 时用了 `JS_ToInt64`、`JS_IsObject`，编译报"未声明"。

esp-mquickjs 是裁剪版：
- 没有 `JS_ToInt64`（只有 `JS_ToInt32`，传入 `int*` 不是 `int32_t*`）
- 没有 `JS_IsObject`（用 `JS_IsPtr` + `!JS_IsNull && !JS_IsUndefined` 组合判断）
- 没有 `JS_NewInt64`（`JS_NewInt32` 够用，unix 秒到 2038 之前都够）

**解法**：unix 秒用 32-bit。文档里标注 2038 限制，未来再考虑双精度路径。

### 3.2 dynamic_app_registry_list 中转 buffer 写死 8

用户上传 `notif_pkg` 后 launcher 看不到，PC GUI 列表也查不到。
加调试日志后发现：`dynapp_script: list:   'notif_pkg' OK but out full (n=8 max=8)`。

排查发现 `dynamic_app_registry.c:49`：

```c
char fs_names[8][DYNAPP_SCRIPT_STORE_MAX_NAME + 1];  // ← 中转 buffer 写死 8
int  fs_count = dynapp_script_store_list(fs_names, ...);
```

launcher 传入 `MAX_DYN_APPS=16` 也没用，前面这一道先把列表砍到 8。
PC 端 `FS_LIST_MAX=8` 也是同样问题。

**解法**：两个都提到 24，留点裕量。

### 3.3 组件循环依赖

想 `dynamic_app_ui.c` 直接 `#include "ui_modal_card.h"` 复用原生 `ui_modal_card_*`，
但 `app` 组件已 `REQUIRES dynamic_app`（page_dynapp_host 调 dynamic_app_*）。
反向依赖会形成环，CMake 会报错。

**解法**：放弃复用，在 dynamic_app_ui.c 内部自包含实现 modal/toast/fadein（用 LVGL 原生 API）。
颜色 token 硬编码（与 ui_tokens.h 数值一致）。代价：~210 行重复，但解耦干净。

### 3.4 LVGL 9 没有 `JS_IsObject`

详见 3.1。补充：判断对象的最稳办法是 `!JS_IsNull(v) && !JS_IsUndefined(v) && JS_IsPtr(v)`。
String/Function 也是 ptr 但取属性时取到 undefined，下游 `JS_ToInt32` 会自然报错，安全。

### 3.5 JS 端 hitZone 与宿主 hit zone 重叠

最初 notif_pkg/main.js 自己用 `UI.hitZone({onExit})` 在屏底放退出区，但宿主层后来也加了
屏底 28px hit zone。两者 z-order 冲突，事件被宿主吞，JS 端 hitZone 永远不触发。

**解法**：JS 端 hitZone 留作"页内手势"用（比如左右滑切 tab），全局退出**只**走宿主。
notif main.js 删掉自己的 hitZone，list 高度调到充满 232px。

### 3.6 媒体心跳日志刷屏

companion 1Hz 推 NOWPLAYING 心跳（即便没歌），`media_manager.c:86` 每次都打 INFO，
日志被 `media: [PAUSE] "" - "", pos=-1/-1` 刷满。

**解法**：检测"实质性变化"（标题/作者/播放状态变了），只在真变化时打 INFO，
心跳 tick 降到 DEBUG（默认看不到）。

---

## 4. 文件改动清单

### 新建（4）
- `dynamic_app/scripts/notif_pkg/main.js` —— 通知 app 复刻参考实现
- `dynamic_app/scripts/notif_pkg/README.md` —— 协议 + 验收点
- `docs/动态app_UI设计系统.md` —— iOS 浅色设计系统规范文档
- `docs/原生App统一改造_动态App_UI能力升级_工作日志.md` —— 本文件

### 修改（重要）
- `dynamic_app/dynamic_app_ui.h` —— enum 加 9 个 style key + 4 个 cmd + EV_MODAL；命令 union 扩；3 个新 enqueue API
- `dynamic_app/dynamic_app_ui.c` —— set_fonts 扩 6 字体；顶部加 s_modal/s_modal_press_y0 状态；末尾加 modal/toast/fadein 自包含 LVGL 实现（~210 行）；drain 加 4 个 case
- `dynamic_app/dynamic_app_ui_internal.h` —— extern 字体声明扩 6 个
- `dynamic_app/dynamic_app_ui_styles.c` —— resolve_font 扩 6 槽；apply_style 加 9 个 case；新增 resolve_flex_align
- `dynamic_app/dynamic_app_natives.c` —— 6 个新 native 实现；sys.style 加 9 常量；sys.font 加 3；sys.icons 16 个；sys.tokens 26 个；time 头文件 include
- `dynamic_app/dynamic_app_internal.h` —— 6 个新 func_idx 字段；EXTRA_NATIVE_COUNT 26→32
- `dynamic_app/dynamic_app_registry.c` —— 中转 buffer 8→24（**修复 launcher 看不到 notif_pkg 的根因**）
- `dynamic_app/scripts/prelude.js` —— 354→716 行，新增 UI 模块（13 组件 + token + 4 helper）+ applyStyle 9 prop 扩展 + dispatch EV_MODAL 路由
- `app/apps/dynapp_host/pages/page_dynapp_host.c` —— 砍返回按钮 + 标题；挂状态栏；屏底 28px hit zone 自算 dy 退出
- `services/manager/media_manager.c` —— "媒体心跳"日志降级（DEBUG），仅真变化打 INFO
- `storage/littlefs/dynapp_script_store.c` —— list 函数加详细调试日志（每个目录的 OK/SKIP/原因）
- `storage/littlefs/dynapp_fs_worker.c` —— FS_LIST_MAX 8→24
- `docs/动态app_JS_API速查.md` —— §0 全局对象一览补 UI 模块；§3 sys.time 重写；§8 sys.font 扩；§9 常见坑加 3 条；新增 §11 UI 设计系统（含完整模板）

---

## 5. 验证

### 5.1 通知 app 复刻视觉验收
- [x] iOS 浅色 + 64px 卡片节奏整齐
- [x] 类别图标颜色（msg蓝 / mail橙 / call绿 / cal紫 / alert红）正确
- [x] HH:MM 时间显示（依赖系统时间已同步）
- [x] 长标题/正文 LONG_DOT 单行省略
- [x] 卡片按下变浅灰高亮（pressedBg）
- [x] 列表项点击 → 模态详情（含完整时间戳 + 删除/关闭）
- [x] 模态点"删除" → 列表更新 + toast
- [x] 模态点"关闭"或点遮罩 → 关闭
- [x] 长按右上徽章 → 弹"清空所有"模态
- [x] BLE 推 add → 新通知插入顶部 + toast
- [x] 退出后 sys.app.saveState 持久化生效

### 5.2 宿主层契约
- [x] 顶部 24px 状态栏（电池/蓝牙/时间）正确显示
- [x] 屏底 28px 上滑（dy≥30） → 回 launcher
- [x] list_root 240×268 可用区充满
- [x] 状态栏配色（浅色文字偏黑）与动态 app 浅底协调

### 5.3 上传链路
- [x] PC GUI 上传 notif_pkg → 设备 littlefs/apps/notif_pkg/ 出现
- [x] launcher 自动出现 notif_pkg 图标（通用 app 兜底）
- [x] PC GUI list_apps 返回 notif_pkg
- [x] 修了 registry 8→24 后，11 个 app 全部能在 launcher 显示

### 5.4 不破坏旧动态 app
- [x] hello / weather / dash / 2048 等老脚本仍正常运行
- [x] 老脚本没用新 prop 时，applyStyle 走旧分支不影响
- [x] sys.style 编号兼容（旧 12 个 key 编号不变）

---

## 6. 不在本次范围（下一轮）

- ❌ 清理裸 `.js` 上传链路（用户已点名要做，下一轮）
- ❌ 删除 notif_pkg1（测试遗留副本）
- ❌ `sys.app.exit()` JS 主动退出（屏底上滑已能用）
- ❌ `lv_arc / lv_slider` 控件原语（写仪表盘类 app 时再补）
- ❌ sub_router push/pop（写多级页面 app 时再补）
- ❌ 暗色 token 集（首先要有暗色风 app 需求）

---

## 7. 关键经验沉淀

### 7.1 "地基 vs 应用"边界判断
不是"JS 能做就让 JS 做"，而是"**这个东西有没有可提取的稳定形态**"。
模态、toast、状态栏、上滑退出 —— 所有 app 长一样 → 进固件。
列表项布局、协议 schema、数据结构 —— 每家不同 → 留 JS。

### 7.2 设计 token 的杠杆效应
13 个颜色 + 6 间距 + 4 圆角的 token 表，比"补 10 个新 LVGL 控件"对视觉的提升大得多。
原因：动态 app"看起来土"的根本不是控件少，是**配色随意 + 间距不一致**。
一套强制的 token 表让所有动态 app 自动对齐到 iOS 浅色，不再各写各的。

### 7.3 调试日志的价值
`registry_list 8 槽截断` 这个 bug 隐藏极深 —— 上传成功、文件落盘、单独 stat 都能看到，
但中转 buffer 写死 8 把字典序靠后的 app 全砍了。靠加详细日志（每条 SKIP 都打原因）
才迅速定位。教训：**enum 数据流的每个截断点都该有可观测性**。

### 7.4 动态 app 不接 manager
即使技术上能让动态 app 调原生 manager（暴露 native 即可），也应该拒绝。
manager 是为单一 app 设计的，schema 写死。动态 app 要拥抱"用户自定义"这个根本卖点，
就必须**自管协议 + 自管数据**，固件只提供字节管道（BLE）和存储沙箱（fs/app）。

### 7.5 组件循环依赖的代价
想复用 `ui_modal_card.c` 但被 `app` 已经依赖 `dynamic_app` 阻挡，最终在 dynamic_app_ui.c
内部用 LVGL 原生 API 重写一份。**写两遍代码 < 拆出一个公共组件**，因为后者要重新设计依赖关系，
而前者只是 ~210 行重复实现。在嵌入式项目里 KISS 优于 DRY。

### 7.6 esp-mquickjs ≠ QuickJS
esp-mquickjs 是裁剪版：仅 ES5，没有 `JS_ToInt64 / JS_IsObject / JS_NewInt64 / setTimeout`，
`JS_ToInt32` 用 `int*` 不是 `int32_t*`。写 native 时**先在 mquickjs.h 里搜 API 是否存在**，
不要凭 QuickJS 文档假定。

---

## 8. 一句话总结

把动态 app 的"地基"补到能跟原生平视的程度（modal/toast/状态栏/上滑/时间/图标/token），
配上一套 250 行的 prelude UI 库，用 250 行 JS 复刻原生通知 app 视觉几乎一致 ——
**动态 app 的天花板瓶颈从此不在固件，而在用户脚本本身**。
