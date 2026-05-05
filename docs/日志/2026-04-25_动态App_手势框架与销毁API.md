# 动态 App（MicroQuickJS）手势框架 + destroy API + 闹钟页

日期：2026-04-25

## 背景

VDOM 三阶段（声明式 UI + 事件冒泡 + 根委托）跑通后，框架在"事件输入"和"局部更新"两块已经完整。但还有两个能力缺口阻挡真实 App：

1. **没法销毁组件** —— `sys.ui.create*` 只增不减，做出来的页面只能一次构造、永远摆在屏幕上。Timer 页那种"列表+对话框"已经勉强，再做"动态增删卡片"就完全不行
2. **只能识别 click** —— LVGL 自带的 `LV_EVENT_PRESSED/PRESSING/RELEASED/GESTURE` 都没暴露到 JS。手机 App 里随处可见的"左滑出现删除按钮""长按拖动"在我们这都做不了

外加一个"美容"工作：

3. 老的一对一 onClick 路径（`sys.ui.onClick`、`handler_id`、`handlers[]` 表）已被根委托完全替代，但还活着 —— 双路径并存的代码占了 100 多行，注释里反复解释"两条路径"，新人看了会迷惑。

按"先清地基、再补功能"的顺序做了三件事：清老路径 → 补 destroy → 补手势。最后用"华为闹钟复刻"页面作为整体压测。

## 目标

- **A**：删干净老 `sys.ui.onClick` 路径，事件出口只剩 root delegation
- **B**：新增 `sys.ui.destroy(id)` + `VDOM.destroy(id)`（自底向上递归），让组件可被释放
- **C**：扩展事件出口，从单一的 CLICK 变成 4 路（CLICK / PRESS / DRAG / RELEASE），每一路在 VDOM 里都按 `_parent` 链冒泡
- **D**：用一个"闹钟列表"页面把上述能力跑一遍，做出 swipe-to-reveal 删除 + 点击进编辑页这种主流交互

## 方案与实现

### 1) 删老路径（A）

老路径是 Phase 1/2 留下的：每个 onClick 在 `s_rt.handlers[]` 里占一个 slot、绑一个 GCRef、在事件队列里靠 `handler_id` 字段区分。Phase 3 上线 root delegation 后业务 0 调用，但代码留着兼容。

诊断它该删的两个角度：

- **"将来可能用到"是 YAGNI 教科书例子**。真要回到一对一注册，git revert 60 行 C 不是难事
- **概念一致性更重要**。drain 里 if/else 区分两条路、event 结构体多 32 字节字段、注释里反复解释"两种风格"，新读者会问"哪条是主路径？"

清理范围：

| 文件 | 删的内容 |
|---|---|
| `dynamic_app_natives.c` | `js_sys_ui_on_click`（35 行）、`dynamic_app_click_handlers_reset`、drain 里 `handler_id` 分支、注册/绑定 |
| `dynamic_app_internal.h` | `MAX_CLICK_HANDLERS`、`js_click_handler_t`、`handlers[]` 字段、`func_idx_sys_ui_on_click`、reset 原型 |
| `dynamic_app_ui.h` | `DYNAMIC_APP_UI_CMD_ATTACH_CLICK` enum、`event.handler_id` 字段、`union.handler_id`、`enqueue_attach_click` 原型 |
| `dynamic_app_ui.c` | `on_lv_click`、`enqueue_attach_click` 实现、drain 里 `ATTACH_CLICK` case |
| `dynamic_app_ui_internal.h` | `ui_registry_entry_t.click_handler_id` 字段 |
| `dynamic_app_runtime.c` | teardown 调 `click_handlers_reset`、extra 13→12 |

顺手发现并删了同款"过渡期保留"的 `dynamic_app_ui_register_label`（外部 0 调用，文件里自己注释为"旧 API"）。

业务侧 0 改动 —— 它本来就只用新路径。

### 2) destroy API（B）

#### 2.1 设计要点

LVGL 的 `lv_obj_del(parent)` 会**级联删除所有子对象**。如果 JS 只调 `destroy("parent")`，C 侧用 `lv_obj_del`：所有 LVGL 子对象会被一起删，但 registry 里那些子 entry 还在 → 野指针；VDOM 的 `nodes{}` 里也还有那些 vnode → 内存泄漏。

两种策略：

- **JS 侧递归**：`VDOM.destroy(id)` 自底向上：先递归销毁 children，再 destroy 自己。C 侧只删一个 obj、释放一个 slot。简单、对称。
- **C 侧扫描清理**：destroy 后遍历 registry 用 `lv_obj_is_valid` 把失效条目清掉。防御强但 JS 侧 vnode 还得自清。

选了 **JS 侧递归**。约定"业务必须从 VDOM 入口调 destroy"。万一被绕过（比如有人直接 `sys.ui.destroy("appRoot")`），registry 里残留 entry 会被后续操作的 `lv_obj_is_valid` 检查清掉，最坏 page 退出时 `unregister_all` 一次清完。

#### 2.2 C 侧实现

新增一种 cmd `DYNAMIC_APP_UI_CMD_DESTROY`，drain 里：

```c
case DYNAMIC_APP_UI_CMD_DESTROY: {
    int slot = registry_find(cmd.id);
    if (slot < 0) break;   // 幂等
    lv_obj_t *obj = s_registry[slot].obj;
    if (obj && lv_obj_is_valid(obj)) {
        lv_obj_del(obj);
    }
    s_registry[slot].used = false;
    s_registry[slot].obj = NULL;
    s_registry[slot].id[0] = '\0';
    break;
}
```

JS native `js_sys_ui_destroy` 三段式：取 id → enqueue → 返回 bool。约 15 行。

#### 2.3 JS 侧 `VDOM.destroy`

```js
function destroy(id) {
    var node = nodes[id];
    if (!node) return;
    var kids = node.children.slice();   // 拷贝避免边遍历边改
    for (var i = 0; i < kids.length; i++) {
        if (kids[i].props && kids[i].props.id) destroy(kids[i].props.id);
    }
    var parent = node._parent;
    if (parent && parent.children) {
        var idx = parent.children.indexOf(node);
        if (idx >= 0) parent.children.splice(idx, 1);
    }
    sys.ui.destroy(id);
    delete nodes[id];
}
```

三件事按顺序：
1. 递归 children（用 slice 副本，递归里会修改原 children 数组）
2. 从父节点 children 摘掉自己（不留悬挂引用）
3. 通知 C 删 LVGL obj + 释放 slot，同时 `delete nodes[id]` 让 id 可复用

#### 2.4 验证

加了个最小测试场景：list 头部加 "-1"/"+1" 按钮，alternately destroy/重建最后一个 slot。验证点：
- 销毁带 children 的子树（button + 3 个 label 一起没了）
- destroy 后该位置点击不再响应（事件路径正常断开）
- 同 id 立即重新 mount 不冲突
- 反复 toggle 10+ 次无内存泄漏

跑过。

### 3) 手势框架（C）

destroy 跑通后，只剩"事件单一"这个限制。设计哲学还是延续 root delegation —— 整页只挂 1 次 native cb，不为每个想响应手势的对象单独挂。

#### 3.1 选哪几种事件

LVGL 触摸事件有十几种。最小够用的 4 种：

| JS 钩子 | LVGL 事件 | 触发时机 | 携带 |
|---|---|---|---|
| `onPress` | `LV_EVENT_PRESSED` | 手指按下 | id |
| `onDrag` | `LV_EVENT_PRESSING` | 按住移动（节流后） | id, dx, dy |
| `onRelease` | `LV_EVENT_RELEASED` | 手指松开 | id |
| `onClick` | `LV_EVENT_CLICKED` | 按下+松开（无明显拖动） | id |

LVGL 自己保证 `CLICKED` 和 `RELEASED` 不会在同一手势里同时触发拖动后的版本（拖过的话 LVGL 不再发 CLICKED）。这意味着业务里 `onClick` 和 `onDrag/onRelease` 可以放心写，不用自己去重 —— 是个很重要的"框架默认契约"。

不引入 `LV_EVENT_GESTURE`：原始 dx/dy 已经够用，业务自己判断方向是 1 行代码，再加一层抽象反而复杂。

#### 3.2 节流策略

`PRESSING` 在 LVGL 里**每帧都触发**（30Hz+）。原样入队会瞬间灌满 8 项的 event queue。方案：

- C 侧维护当前手势会话的累计位移 `acc_dx, acc_dy`（PRESSED 重置，PRESSING 累加）
- 累计 `|dx| + |dy| >= 2px` 才入队，入队后清零
- queue 满则丢（拖动中漏一帧不影响业务，最坏跟手感稍差）

实测 240×320 屏 30Hz 触摸，2px 阈值约 60Hz 入队。卡片跟手很流畅。

#### 3.3 active_target 设计

PRESSING 的 LVGL target 不靠谱 —— 手指按下按钮 A、拖出按钮范围、再回到 A，中间 LVGL 报的 target 会乱跳。**业务想要的是"按下时是谁"**，不是"现在指针在哪"。

所以加了 `s_gesture.active_target / active_id` 在 PRESSED 时记录，后续 PRESSING/RELEASED 都用这个，不读 LVGL 实时 target。CLICKED 是另外一个 LVGL 事件，跟 active_target 无关，正常用 LVGL target。

```c
static struct {
    lv_obj_t *active_target;
    char      active_id[DYNAMIC_APP_UI_ID_MAX_LEN];
    int16_t   acc_dx, acc_dy;
} s_gesture;
```

加了 `unregister_all` 清 gesture 状态，避免 page 退出再进时残留。

#### 3.4 event 结构扩展

```c
typedef enum {
    DYNAMIC_APP_UI_EV_CLICK   = 1,
    DYNAMIC_APP_UI_EV_PRESS   = 2,
    DYNAMIC_APP_UI_EV_DRAG    = 3,
    DYNAMIC_APP_UI_EV_RELEASE = 4,
} dynamic_app_ui_event_type_t;

typedef struct {
    uint8_t type;          // 4 种类型
    int16_t dx, dy;        // 仅 DRAG 用
    char    node_id[32];
} dynamic_app_ui_event_t;
```

数值常量与 JS 侧 `HOOK_NAME` 表一一对应，不能改。

#### 3.5 dispatcher 升级

drain 调 dispatcher 时多 push 三个参数：

```c
JS_PushArg(ctx, arg_dy);                // arg4 最深
JS_PushArg(ctx, arg_dx);                // arg3
JS_PushArg(ctx, arg_type);              // arg2
JS_PushArg(ctx, arg_id);                // arg1
JS_PushArg(ctx, s_rt.dispatcher.val);   // fn
JS_PushArg(ctx, JS_NULL);               // this 栈顶
JS_Call(ctx, 4);
```

JS 侧 dispatch 改签名，按 type 查表选 hook 名：

```js
var HOOK_NAME = { 1: 'onClick', 2: 'onPress', 3: 'onDrag', 4: 'onRelease' };

function dispatch(startId, type, dx, dy) {
    var hook = HOOK_NAME[type]; if (!hook) return;
    var ev = { target: startId, currentTarget: null,
               dx: dx | 0, dy: dy | 0,
               stopPropagation: function () { stopped = true; } };
    var cur = nodes[startId];
    while (cur) {
        if (typeof cur.props[hook] === 'function') {
            ev.currentTarget = cur.props.id;
            var ret = cur.props[hook](ev);
            if (ret === false || stopped) return;
        }
        cur = cur._parent;
    }
}
```

冒泡逻辑、`stopPropagation` 语义都和原来 onClick 一样，只是 hook 名不再是写死的。

### 4) 闹钟页验证（D）

把整个 app.js 业务部分换成华为风格闹钟复刻，作为压测目标。

#### 4.1 布局（240×320）

```
header        高 36   "闹钟" + ☰
clockArea     高 80   时段 + HH:MM:SS（huge 字）
statusBar     高 20   "已开启 N 个闹钟" / "所有闹钟已关闭"
list          高 140  flex_col 装卡片，超出可滚
fab           底部 44 蓝色圆 +
```

#### 4.2 卡片三层结构

为了 swipe-to-reveal，每张卡片做了三层：

```
row_<seq>     panel    flex 项，做"轨道"，固定 [-100, 56]
  ├─ del_<seq>   button   红色"删除"按钮，绝对定位贴 row 右边 80×56（在底层）
  └─ alarm_<seq> button   卡片本体，align ['lm', 0, 0]（在上层覆盖删除按钮）
```

卡片本体平移到 `align: ['lm', -80, 0]` 时，下面的红色删除按钮就露出来了。LVGL 里子对象按 mount 顺序叠加，后挂的盖前挂的，所以删除按钮要先 mount、卡片本体后 mount。

#### 4.3 手势状态机

```js
var cardSwipe = {};   // seq -> { phase: 'rest'|'dragging'|'open', x: 当前位移 }

onPress:   进 dragging 态
onDrag:    nx = s.x + e.dx，限位 [-80, 0]，setCardX(seq, nx)
onRelease: |x| ≥ 30 → 吸附到 -80 (open)；否则回到 0 (rest)
onClick:   open 态时收起；rest 态时 openEditView
```

跨卡片协作：每次 release 到 open 时调 `closeAllExcept(seq)`，把其他露出的卡片都收起 —— 跟 iOS 行为一致，同时只能有一张卡片处于 open。

#### 4.4 编辑页跳转

`openEditView` 直接 mount 一个 `editView` panel 覆盖整个 appRoot（z-order 后挂的盖前挂的）。返回时 destroy 自己，listView 自动重见。

整页 mount 一次，编辑页按需 create / destroy。这就是 "destroy API" 的真实使用场景。

#### 4.5 子事件返回 false 阻止冒泡

`onSwitchClick` 和 `onDelClick` 都 `return false`：

- 滑块在卡片本体内 → 没这个会触发卡片的 onClick → 进编辑页（错的）
- 删除按钮在 row 容器内 → 没这个理论上不会冒泡到 row（row 没 onClick），但还是显式写 false 表达意图

## 改动文件清单

| 文件 | 阶段 | 类型 | 说明 |
|---|---|---|---|
| `dynamic_app_natives.c` | A/B/C | 改 | 删 `js_sys_ui_on_click` 等 60 行；加 `js_sys_ui_destroy`、drain 调 dispatcher 扩参 |
| `dynamic_app_internal.h` | A/B | 改 | 删 handlers/MAX_CLICK_HANDLERS/func_idx_sys_ui_on_click；加 func_idx_sys_ui_destroy |
| `dynamic_app_ui.h` | A/B/C | 改 | 删 ATTACH_CLICK enum / handler_id 字段；加 DESTROY enum、event_type_t、event 结构扩 type/dx/dy |
| `dynamic_app_ui.c` | A/B/C | 改 | 删 `on_lv_click`/ATTACH_CLICK case；加 DESTROY case、s_gesture 状态机、`on_lv_root_event` 4 路分发 |
| `dynamic_app_ui_internal.h` | A | 改 | 删 `click_handler_id` 字段 |
| `dynamic_app_ui_registry.c` | A | 改 | `registry_alloc` 不再初始化 `click_handler_id` |
| `dynamic_app_runtime.c` | A/B | 改 | 删 click_handlers_reset 调用；extra 13→12→13 |
| `dynamic_app.c` | A | 改 | 删 `memset(s_rt.handlers, ...)` |
| `dynamic_app.h` | A | 改 | 删 `dynamic_app_ui_register_label` 原型 + 注释里相关示例 |
| `scripts/app.js` | B/C/D | **重写** | VDOM 加 destroy + dispatch 改 4 路；业务整页换成闹钟 |

## 验证

烧录测试通过：

### 手势

- ✅ 左滑卡片跟手平移；超 30px 松手吸附露出删除按钮；不到阈值松手回弹原位
- ✅ 已露出再左滑能从当前位置继续拖（不跳回 0）
- ✅ 露出 A 后点 B → A 自动收起
- ✅ 点露出的"删除"→ 整行消失，状态行刷新；alarms 数组正确缩短
- ✅ 没拖过的卡片点本体 → 编辑页打开；编辑页 ←/完成 → 列表恢复
- ✅ 点滑块 → 切开关，颜色 + 位置变化；卡片不滑动（return false 生效）

### destroy

- ✅ 编辑页打开-关闭 N 次：editView mount/destroy 配对，列表始终在
- ✅ 点 FAB 加 N 张卡片，再删 N 张：alarms[] 与 cardSwipe[] 不留垃圾
- ✅ 反复加删 20+ 次：LVGL/JS 都不报错

### 老路径清理

- ✅ `grep -rn "handlers\[\|MAX_CLICK_HANDLERS\|on_lv_click\|ATTACH_CLICK\|sys\.ui\.onClick\|register_label" dynamic_app/ app/` → 0 残留
- ✅ 业务 0 改动（本来就没用老 API）

## 经验沉淀

### "active_target 在 PRESSED 时锁定"是手势库的核心契约

第一版直接用 LVGL 实时 target 在 PRESSING 里查 registry，结果手指拖出按钮范围后 target 会变成下层 panel，业务接到 dx 但 currentTarget 漂移了 —— 完全不可用。

改成"PRESSED 时锁定 target，整个手势会话内不变" → 业务代码立刻干净：`onDrag(e)` 里的 `e.currentTarget` 就是当初按下的那张卡片，永远。

这条规则对应浏览器 PointerEvents 的 `pointercapture` —— 同一个问题，同一个解。任何手势库都得有这个机制，不是 nice to have。

### 节流的"加权累计 + 阈值"模式

最早想的是"每 N ms 入队一次"。问题：用户慢慢拖（dx 累计 0.5px/帧）会看不到响应；快速拖（dx 累计 30px/帧）又入队太少跟不上。

改成"累计绝对值超过阈值才入队" → 自动适配速度。慢拖时帧数多但每次 dx 小，累够 2px 才发；快拖时一帧就超阈值立刻发。**度量"该不该处理"应该和它要表达的物理量一致**，时间不是这里的物理量、位移是。

### "onClick 与 onDrag 互斥触发"是 LVGL 送的礼

如果框架不显式说明，业务一定会自己写：

```js
onPress:   function() { dragged = false; },
onDrag:    function() { dragged = true; },
onRelease: function() {},
onClick:   function() { if (!dragged) doClick(); },
```

每个业务都重复一遍。然后某天 LVGL 升级行为变了，全线挂掉。

LVGL 内部已经做了这个判断（`gesture_limit` 触发后不发 CLICKED）。我们要做的就是**把这个语义记到 spec 里**，告诉业务"放心写两个 hook，不会同时调"。一行文档抵 100 行业务防御代码。

### 三层卡片结构是标准 swipe 模式

我一开始想用"卡片本体 + clip 露出"实现，结果发现 LVGL 没有简单的 `overflow: hidden` —— 子对象超出父容器会被裁剪是默认行为，但兄弟对象的 z-order 才是真正关键。

正确模式是"轨道（panel）+ 删除按钮（底层）+ 卡片本体（上层）"。卡片本体平移走时，自然露出底下的删除按钮。这是 iOS UITableView 滑动删除的实现原理，移植过来零摩擦。

教训和 VDOM 那次一样：嵌入式做"复刻原生 App"时，**直接抄已经验证过亿万次的 iOS/浏览器模式**比自己想要稳得多。

### "看似简单的功能，为什么 4 个文件都要改"

destroy 这种功能，C 里看起来就是 "lv_obj_del + 释放 slot" —— 10 行。但实际改了：

- ui.h（cmd enum + 原型）
- ui.c（enqueue + drain case）
- natives.c（JS native）
- internal.h（func_idx）
- runtime.c（extra 计数）
- app.js（VDOM.destroy 递归）

**这是 Script <-> UI 双线程 + JS <-> C 双语言架构的固定开销**。每加一个原语都要"过六个文件"。设计层面这是正确的（每层职责清晰），但日常开发时心理负担不小。可以考虑写个 codegen 脚本 / 宏，从一份 spec 自动生成这六处骨架代码 —— 不过现在原语数量（13 个）还没多到需要这种工具。

### 删老路径比加新功能心理收益更大

清完一对一 onClick 那 130 行后，整个 dynamic_app 模块的"心智地图"明显简化：

- drain 不再 if/else 区分两条路 → 单一职责
- event 结构里再没有"两个互斥字段"
- 文档不再需要解释"为什么有两套"

加新功能是"代码长 +N 行"；删老代码是"代码长 -N 行 + 概念清晰度 ×2"。后者的复利更大。规则：**任何一次"灯下 0 调用"的代码（business 0 引用 + grep 0 命中）都该删，不要等"以后可能用"**。

### 闹钟页这种密度的 UI 不是 demo

400 多行 JS 实现一张完整闹钟页，包括手势、动态增删、视图切换、状态联动 —— 这已经是产品级的页面了，不是 toy。证明现在的 13 个原语 + 4 个事件钩子表面够覆盖大多数手机 App 范式。

具体能力 checklist：
- ✅ 列表 + 滚动
- ✅ 动态增/删元素
- ✅ 跨视图切换（mount/destroy 顶层 panel）
- ✅ 手势识别（4 种事件 + dx/dy）
- ✅ 状态联动（点开关 → 改时间字色 + 改状态行文字）
- ✅ 嵌套点击 + 阻止冒泡（滑块在卡片里）
- ✅ 自定义控件（手搓 switch、FAB）
- ❌ 真·阴影（要补 SHADOW style key）
- ❌ 过渡动画（要暴露 lv_anim API，工程量大）
- ❌ 长按事件（要加 LV_EVENT_LONG_PRESSED，跟 PRESSING 类似 5 行）
- ❌ 真时间（要从 dynamic_app sandbox 暴露 RTC 接口）

后三条都是"可加可不加"，看具体页面要不要。

## 接下来

按"做了立刻能感觉到"的强度排，候选：

1. **`SHADOW` style key**（5 行 C） —— 卡片不再扁平
2. **`LONG_PRESS` 事件**（5 行 C） —— 多点交互模式
3. **`GAP` flex 行间距**（3 行 C） —— 列表卡片不贴脸
4. **接 RTC 真时间**（30 行 C，暴露 `sys.time.now()`） —— 闹钟有真实意义
5. **diff 算法**（150 行 JS）—— React 最后那块拼图，但目前没有动态列表场景非要它

前 3 条加起来 13 行 C，可以一次性补完；4 单做；5 等真正需要"keyed list 重排"的页面再说。