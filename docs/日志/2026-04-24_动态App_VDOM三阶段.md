# 动态 App（MicroQuickJS）VDOM 三阶段：声明式 UI + 事件冒泡 + 根委托

日期：2026-04-24

## 背景

模块重构（前一篇日志）把 dynamic_app 拆成 6 个 .c 文件后，框架本身的"地基"已经稳。但 `app.js` 的写法还是命令式的：

```js
button("addBtn", "hdr");
st("addBtn", Style.SIZE, 36, 32);
st("addBtn", Style.BG_COLOR, COLOR_ACCENT);
st("addBtn", Style.RADIUS, 8);
st("addBtn", Style.ALIGN, Align.RIGHT_MID, -8, 0);
sys.ui.onClick("addBtn", onAdd);
```

每个按钮要重复 5~10 行近乎一样的指令；`paintSlot()` 这种"状态 → UI"的同步逻辑必须手写；6 行 slot 还能维护，60 行就会失控。这是典型的**命令式 UI 痛点**：状态变化时程序员必须自己算"哪个属性要改"。

为了验证框架的真实可扩展性，做了个**Timer（定时器）页面**作为压测目标 —— 列表 + 新建对话框 + 到期 modal，要操作约 55 个控件、8 个 onClick、状态机三态切换。第一版用现有命令式风格写 ~290 行，**能跑但难读** —— 设置面板和模态框的 ALIGN/SIZE 散落在几十处，想加第 7 个 slot 都得复制几十行。

诊断：**不是 11 个原语不够，是上层缺一个抽象层**。决定在不改 C 的前提下，给 JS 侧加一层"声明式 + DOM-like"的封装，分三个阶段推进。

## 目标

- **Phase 1**：把命令式调用包装成 `h(type, props, children)` 声明式语法，建立 VDOM 节点树（**0 行 C 改动**）
- **Phase 2**：基于 VDOM 父链实现事件冒泡 + `stopPropagation`（**0 行 C 改动**）
- **Phase 3**：C 侧加 root listener delegation，让整页只挂一次 LVGL cb（**~80 行 C 改动**）
- 业务代码（Timer 页）每阶段 0~少量改动，行为始终一致

## 方案与实现

### 1) 三阶段为什么这样切

把 React 的核心（VDOM + 事件冒泡 + 委托）拆开做，每阶段独立可用、可中途停下。原因：

- **Phase 1 是"语法糖"**：只重新组织代码，不改运行时行为。错了立刻能看出来
- **Phase 2 是"纯 JS 模拟"**：VDOM 已有 `_parent`，冒泡只是顺着链上爬，不动 native
- **Phase 3 必须改 C**：因为浏览器经典"事件委托"要 LVGL 在 root 上挂 cb，不改 native 做不到

如果一上来就做 Phase 3，会陷入"VDOM 没建好 + native 协议又改了"双战线。**先建抽象、再做能力**，每步都是局部修改。

### 2) Phase 1：VDOM 框架

最小核心 `h() / mount() / find() / set()`，约 110 行：

```js
function h(type, props, children) {
    return { type, props: props||{}, children: children||[],
             _parent: null, _mounted: false };
}
function mount(node, parentId) {
    var id = node.props.id || autoId();
    if (node.type === 'panel')   sys.ui.createPanel(id, parentId);
    if (node.type === 'button')  sys.ui.createButton(id, parentId);
    if (node.type === 'label')   sys.ui.createLabel(id, parentId);
    if (node.props.text)         sys.ui.setText(id, node.props.text);
    applyStyle(id, node.props);
    if (node.props.onClick)      sys.ui.onClick(id, node.props.onClick);
    nodes[id] = node;
    node.children.forEach(c => { c._parent = node; mount(c, id); });
}
function set(id, patch) {           // 局部更新（命令式 paint 的替代）
    if (patch.text !== undefined) sys.ui.setText(id, patch.text);
    applyStyle(id, patch);          // 只 patch 里有的字段会发 setStyle
}
```

关键化简点：

- **props 名压缩**：`size: [-100, 32]`、`bg: 0x...`、`align: ['lm', 12, 0]`、`font: 'huge'` —— 让常用样式一眼看清
- **align 用短串**：`'tl' 'tm' 'tr' 'lm' 'c' 'rm' 'bl' 'bm' 'br'`，比 `Align.LEFT_MID` 简洁
- **嵌套用 children**：父子关系用结构表达，不用反复传 `parent_id`
- **slot 工厂函数**：`makeSlotRow(i)` 6 行 → 1 个函数 6 次调用

业务代码改写后从 ~290 行（命令式 + 业务）降到 ~330 行（VDOM 框架 + 业务），但**业务部分本身**从 ~220 行降到 ~120 行。结构感大幅提升。

### 3) Phase 2：事件冒泡 + stopPropagation

加一个 `dispatch(startId)`，和 `mount` 里的 onClick 蹦床：

```js
function dispatch(startId) {
    var node = nodes[startId];
    var stopped = false;
    var ev = {
        target: startId,
        currentTarget: null,
        stopPropagation: function() { stopped = true; }
    };
    var cur = node;
    while (cur) {
        if (typeof cur.props.onClick === 'function') {
            ev.currentTarget = cur.props.id;
            var ret = cur.props.onClick(ev);
            if (ret === false || stopped) return;
        }
        cur = cur._parent;
    }
}

// mount 里：不再直接绑业务 fn，而是绑 dispatcher 蹦床
sys.ui.onClick(id, (function(capturedId) {
    return function() { dispatch(capturedId); };
})(id));
```

业务侧 0 改动 —— 老的 `function() { ... }` 写法照样工作（不读 e 就行），新写的可以用 `function(e) { sys.log(e.target); }`。

注意：Phase 2 仍然是**一对一 native 注册**，每个有 onClick 的节点都调一次 `sys.ui.onClick`。"父容器收子按钮 click"这件事在 JS 里能模拟，但前提是**父也得是可点击控件且自挂了 onClick**，因为没有真正的 root listener，事件靠子按钮自己的 cb 触发再走 dispatch 上爬。

这一步的价值在 Timer 页里看不到（按钮都是叶子节点，没人需要冒泡），但**它是 Phase 3 的设计准备** —— 让 dispatch 函数成型，后面只换"触发源"。

### 4) Phase 3：C 侧 Root Delegation

这一步真要改 C 了。流程：

```
JS 在 appRoot 调一次:  sys.ui.attachRootListener("appRoot")
                     │
                     ▼ C 侧 drain
LVGL: lv_obj_add_event_cb(appRoot_obj, on_lv_root_click, LV_EVENT_CLICKED, NULL)
                                                 │
                                                 │ 用户点 saveBtn
                                                 ▼ LVGL 冒泡到 root
on_lv_root_click(e):
    target = lv_event_get_target_obj(e)         // 真·被点对象
    slot = registry_find_by_obj(target)          // 反查 id
    enqueue {.node_id = "saveBtn"}
                                                 │
                                                 ▼ Script tick
drain_ui_events_once(ctx):
    if ev.node_id != "":
        JS_Call s_rt.dispatcher.val with ev.node_id
                                                 │
                                                 ▼ JS 侧
__dispatch(id) → VDOM.dispatch(id) → 沿 _parent 链找 onClick → 调用
```

#### 改动清单

| 文件 | 改动 |
|---|---|
| `dynamic_app_ui.h` | 加 cmd enum `ATTACH_ROOT_LISTENER`；event 结构加 `node_id` 字段；新增 enqueue 原型 |
| `dynamic_app_ui_internal.h` | 加 `registry_find_by_obj(const lv_obj_t *obj)` 原型 |
| `dynamic_app_ui_registry.c` | 实现按指针反查 slot |
| `dynamic_app_ui.c` | 加 `on_lv_root_click`；drain 加新 case；**create 时统一 `lv_obj_add_flag(obj, LV_OBJ_FLAG_EVENT_BUBBLE)`** |
| `dynamic_app_internal.h` | runtime 加 `dispatcher` GCRef + `func_idx_sys_set_dispatcher` |
| `dynamic_app_natives.c` | 新增 `js_sys_ui_attach_root_listener`、`js_sys_set_dispatcher`；drain 双路径分发 |
| `dynamic_app_runtime.c` | extra 11 → 13；teardown 释放 dispatcher GCRef |
| `scripts/app.js` | 加 `appRoot` 包住 3 个视图；移除 mount 时的 `sys.ui.onClick`；用 `sys.__setDispatcher(...)` 注册分发函数 |

#### 关键设计点

**双路径并存** —— 老的 `sys.ui.onClick`（handler_id 路径）依然工作，新的 root delegation（node_id 路径）共享同一个 event queue，靠两个字段互斥区分：

```c
typedef struct {
    uint32_t handler_id;   /* 老路径 */
    char     node_id[32];  /* 新路径 */
} dynamic_app_ui_event_t;
```

drain 一个 if/else 分发：

```c
if (ev.handler_id != 0) { /* 调 handlers[hid-1] */ }
else if (ev.node_id[0] != '\0') { /* 调 s_rt.dispatcher */ }
```

这样未来写新 app 时可以选用任一种风格。

**LVGL 默认事件不冒泡**（这个坑卡了挺久）：

LVGL 的 `lv_obj_add_event_cb` 默认是"自己事件自己处理"，不会传给父级。除非 `lv_obj_add_flag(obj, LV_OBJ_FLAG_EVENT_BUBBLE)`。所以 root listener 想接到子按钮的 click，**必须给所有 create 出来的对象加这个 flag**。改在 `do_create` 末尾统一加，不影响一对一 onClick 路径（hid cb 还是会触发自己）。

**不靠 globalThis** —— `sys.__setDispatcher(fn)` 让 JS 主动把分发函数交给 C：

```c
static JSValue js_sys_set_dispatcher(JSContext *ctx, ...) {
    if (s_rt.dispatcher_allocated) {
        JS_DeleteGCRef(ctx, &s_rt.dispatcher);
    }
    JSValue *p = JS_AddGCRef(ctx, &s_rt.dispatcher);
    *p = argv[0];
    s_rt.dispatcher_allocated = true;
    return JS_UNDEFINED;
}
```

这是踩了 esp-mquickjs 的坑后被迫的设计 —— 见下文"经验沉淀"。

### 5) 业务代码：appRoot 容器

3 个视图 panel 原本各自直接挂 root，改成都挂在新加的 `appRoot` 下：

```js
VDOM.mount(h('panel', { id:"appRoot", size:[-100,-100], bg:COLOR_BG }), null);
VDOM.mount(h('panel', { id:"listPanel",  ... }), "appRoot");
VDOM.mount(h('panel', { id:"setPanel",   ... }), "appRoot");
VDOM.mount(h('panel', { id:"modalPanel", ... }), "appRoot");
sys.ui.attachRootListener("appRoot");
sys.__setDispatcher(function(id) { VDOM.dispatch(id); });
```

业务回调一行不改 —— 所有 `function(e) { ... }` handler 写法不变。

## 改动文件清单

| 文件 | Phase | 类型 | 说明 |
|---|---|---|---|
| `scripts/app.js` | 1/2/3 | **重写** | VDOM 框架 110 行 + Timer 业务 220 行 |
| `dynamic_app_ui.h` | 3 | 改 | event 结构加 node_id；新 cmd enum；新 enqueue |
| `dynamic_app_ui_internal.h` | 3 | 改 | 加 registry_find_by_obj 原型 |
| `dynamic_app_ui_registry.c` | 3 | 改 | 实现按指针反查 |
| `dynamic_app_ui.c` | 3 | 改 | 加 on_lv_root_click、新 enqueue、统一开 EVENT_BUBBLE |
| `dynamic_app_internal.h` | 3 | 改 | dispatcher GCRef + 新 func_idx |
| `dynamic_app_natives.c` | 3 | 改 | 加 attachRootListener / __setDispatcher native；drain 双路径 |
| `dynamic_app_runtime.c` | 3 | 改 | extra 11→13；teardown 清 dispatcher |

## 资源对比

| 项目 | Phase 0（命令式） | Phase 3（VDOM + delegation） |
|---|---|---|
| LVGL `add_event_cb` 调用 | 8 次（每按钮一次） | **1 次**（仅 appRoot） |
| `s_rt.handlers[]` 占用 | 8 槽 | **0 槽**（delegation 不用） |
| `sys.ui.onClick` 调用次数 | 8 | **0** |
| 控件总数 | 55 | 56（多了 appRoot 容器） |
| 业务代码可读性 | 命令式分散 | 声明式集中 |

## 验证

### 静态校验（已做）

- ✅ esp-mquickjs 调用栈约定（参数→fn→this 顺序）已对照 example.c 修正
- ✅ dispatcher GCRef 在 teardown 中释放，与 intervals/handlers 同管理
- ✅ event 结构扩字段后队列 sizeof 跟随，xQueueCreate 正常分配
- ✅ LVGL `lv_event_get_target_obj` 是 v9 API（v8 可能没有）

### 行为校验（已通过烧录）

- ✅ 菜单页 → Dynamic App，三视图 + 8 按钮全部渲染正确
- ✅ +/− / Cancel / Save / Dismiss 点击全部响应（仅 1 次 native attach）
- ✅ 退出 / 再次进入不崩溃，VDOM 跟 JSContext 一起销毁重建
- ✅ 按钮按下态视觉反馈正常（LVGL 默认 PRESSED 状态）

## 经验沉淀

### "0 行 C 改动"是个反直觉的成果

Phase 1+2 完全没动 C 代码 —— 这并不神奇，本质是：**当原语集足够通用时，上层任何"重新组织代码"的封装都不需要碰原语**。VDOM 不是新能力，只是把原本写在脚本里的 `sys.ui.*` 调用，**换了个调用入口**。

判断"是封装还是新能力"的方法：**用现有原语能不能写出一段能跑的实现？** 能 → 封装即可；不能 → 那才需要改 C。

Phase 1（VDOM 数据结构）+ Phase 2（冒泡链遍历）都通过这个测试。Phase 3 失败 —— 因为"整页只挂一次 LVGL cb"这件事用现有原语写不出，所以必须加 native。

### 嵌入 JS 引擎的"GC 持有"是核心思维

任何"未来要被异步调用"的 JS 函数，都必须有持有者，否则 GC 收掉。持有者要么是 JS 对象树上的可达引用，要么是 C 侧的 `JS_AddGCRef`。

这次踩的坑：以为 `var __dynapp_dispatch = ...` 顶层 var 自动挂 globalThis，结果 esp-mquickjs 的 eval 是严格模式上下文（顶层 `this === undefined`），var 不一定上全局，函数被 GC 掉，C 侧 `JS_GetPropertyStr` 拿到 undefined → TypeError。

解药：**让 JS 主动把函数交给 C**（`sys.__setDispatcher`），C 用 GCRef 持有 —— 这跟 `sys.ui.onClick` / `setInterval` 是同一种模式的复用。**任何接收 JS 函数的 native API 都该这样写**，不要依赖"全局变量自动可达"这种脆弱假设。

### esp-mquickjs 的栈机器调用约定

正常 QuickJS 是 `JS_Call(ctx, this_val, fn, argc, argv)` 直接传指针。esp-mquickjs 是栈机器：

```c
// 必须按这个顺序 push（从栈底到栈顶）：
JS_PushArg(ctx, argN);    // 参数（最深）
...
JS_PushArg(ctx, arg1);
JS_PushArg(ctx, fn);      // 函数
JS_PushArg(ctx, this_val); // this（栈顶）
JS_Call(ctx, argc);
```

我一开始按 QuickJS 标准写法 push 成 `fn → this → arg`，结果 fn 被埋在栈底，栈顶 arg 被当成 fn 调用 → "TypeError: not a function"。**对照 `example.c::js_rectangle_call` 才对**。

教训：**移植/嵌入的 fork 不要假设 API 跟上游一致**，每个新 API 都要查 example 或 header。esp-mquickjs 没文档，example.c 就是事实文档。

### LVGL 默认事件不冒泡

`LV_OBJ_FLAG_EVENT_BUBBLE` 默认 false。如果想做事件委托（root 收所有子事件），必须给所有子对象加这个 flag。改在 `do_create` 末尾统一加，是侵入性最小的做法 —— 一对一 onClick 路径完全不受影响（自己的 cb 还是会触发），只是多了一份事件传到父级。

### 双路径并存的兼容设计

Phase 3 没有强制业务迁移到 delegation 风格 —— 老的 `sys.ui.onClick` 仍然工作，新的 `attachRootListener` 是可选项。理由：

- 项目里别的脚本（如果将来有）可能仍想用一对一注册的简单模型
- 让两条路径共存于 event queue 里（用字段互斥区分）成本只有十几行代码
- 有了"两个工具同时可用"的语义，将来新加事件类型（hover、long-press）时也容易扩展

代价：event 结构体大了 32 字节，FreeRTOS 队列 8 项 → 多 256 字节。可接受。

### 三阶段切分的真正价值不在工程量，在心智负担

如果一口气把 Phase 1+2+3 一起做，碰到 esp-mquickjs 的两个坑（栈顺序 + 顶层 this 为 undefined），根本分不清是哪一层的问题。

按阶段做的好处：
- Phase 1 跑通 → 确认 VDOM 数据结构没问题
- Phase 2 跑通 → 确认冒泡链遍历没问题
- Phase 3 出 TypeError → 必然是新加的 C 那部分，定位范围 80 行

每个阶段都用真实硬件验证，错误才能精确归因。**做大型改动时切片越小，定位越快**。

### 浏览器演进史不是巧合，是工程常识收敛

我们这次走的路线 —— 从命令式 → VDOM → 冒泡 → 委托 —— 跟浏览器从 1996 → 2000 → 2010 的演进路径完全一致。不是模仿，是这条路本身就是"所有需要在两个线程间同步 UI 的系统"的最优解。

证据：React Native（JS bridge → iOS UIView）、Flutter（Dart → Skia）、Electron（Node → Chromium）、甚至游戏引擎的 ImGui 模式 —— 都在做同一件事的不同变体。

教训：嵌入式做脚本化 UI 时，**直接抄浏览器的演进路径**比自己重新发明要稳得多。每个阶段都有亿万人验证过它在真实场景里到底解决了什么问题。

## 接下来

剩下两件事按优先级：

1. **`sys.ui.destroy(id)`** —— 现在唯一阻挡"真正动态创建/销毁组件"的原语缺口。约 60~80 行 C，diff 算法的前置依赖
2. **diff 算法** —— 起码做 props-only diff（20 行 JS），让 `VDOM.set` 能自动算变化；如果有动态列表场景再做 keyed list diff（80~150 行）

Phase 3 完成后，框架已具备**事件层完整能力**（target/bubble/delegation/stopPropagation）。距离 React 的差距只剩"声明式 + diff" 这最后一块拼图 —— 但从工程现实看，是不是值得做要看后面真有几个**带动态列表**的页面再决定。
