# 动态 App JS API 速查

> 适用：基于本固件 dynamic_app runtime 的 JS app 开发
> 引擎：esp-mquickjs（**仅 ES5**，无箭头函数 / let / const / class / Promise）
>
> **重要**：runtime 在每次启动 app 之前会自动 eval 一段标准库 `prelude.js`，
> 暴露 `VDOM` / `h` / `makeBle` 三个全局符号。业务脚本无需 import / 拷模板，
> 直接用即可。本速查列出 prelude 提供的"上层 API"和固件提供的"下层 sys.* API"。

---

## 0. 全局对象一览

```
// ── prelude 自动注入（推荐使用） ──
VDOM                         // 声明式 UI 框架
  ├─ h(type, props, children)  // 造 vnode
  ├─ mount(node, parentId)     // 挂载（parentId=null 表 root container）
  ├─ render(node, parentId)    // diff + 增量更新（首次等同 mount）
  ├─ set(id, patch)            // 局部 patch 单节点
  ├─ find(id)                  // 取 vnode
  ├─ destroy(id)               // 销毁子树
  ├─ showModal(opts)           // 内部用，业务请走 UI.modal
  └─ dispatch(...)             // 内部用，不调
h                            // VDOM.h 的短名
makeBle(appName)             // → ble 对象（见 §5）
UI                           // iOS 浅色风格高阶组件库（见 §11）
  ├─ T / I                     // 设计 token / 图标常量短名
  ├─ screen / title / card     // 容器类
  ├─ kvRow / listRow           // 列表行
  ├─ iconBtn / pillBtn / badge // 按钮 / 徽章
  ├─ statusBar / divider       // 装饰
  ├─ hitZone                   // 页内手势（屏底退出由宿主层兜底，业务一般不用）
  ├─ modal({...})              // 弹模态卡片（封装 sys.ui.modal）
  ├─ toast(text, durMs)        // 屏底浮一条
  ├─ fadeIn(id, delayMs)       // 淡入动画
  └─ swipeExit(children, fn)   // 给 children 末尾加一个 hitZone
Router                       // 多级页面栈（见 §12，可选）
  ├─ define(name, builder)     // 注册页构造器
  ├─ start / push / replace
  ├─ pop / popTo / current / depth
  └─ onEnter(name, fn) / onLeave(name, fn)

// ── 固件原生（每个 app 都有） ──
sys
├─ log(msg)
├─ ui.*                      // 命令式绘图原语（一般不直接用，VDOM 已包好）
│   ├─ createLabel / createPanel / createButton / createImage
│   ├─ setText / setStyle / setImageSrc / destroy / attachRootListener
│   ├─ modal({...})           // 模态弹层（见 §11.2）
│   ├─ toast(text, durMs)     // 屏底 toast
│   └─ fadeIn(id, delayMs)    // 淡入动画
├─ time.*
│   ├─ uptimeMs() / uptimeStr()
│   ├─ now()                  // 当前 unix 秒（OS 时间）
│   ├─ parts(ts)              // {y,mo,d,h,mi,s,wday,yday}
│   └─ format(ts, fmt)        // strftime 包装
├─ app.saveState() / loadState() / eraseState()
├─ ble.send / onRecv / isConnected      ⚠️ 不直接用，请走 makeBle
├─ fs.read / write / exists / remove / list   // app 沙箱文件 IO
├─ symbols.*                 // LVGL 内置图标字面量（PLAY/BLUETOOTH/...）
├─ icons.*                   // Material Symbols 矢量图标 codepoint（见 §11.3）
├─ tokens.*                  // 设计 token 常量（颜色/间距/圆角，见 §11.1）
├─ style.*                   // setStyle key 枚举（VDOM 内部用，21 个 key）
├─ align.*                   // 对齐方式枚举
└─ font.*                    // TEXT/TITLE/HUGE/ICON_24/ICON_36/NUM_M

setInterval(fn, ms)          // 周期回调，返 id
clearInterval(id)            // 取消
```

---

## 1. VDOM —— 声明式 UI（推荐用法）

### 1.1 三件事

```js
// 1) 描述 UI（一棵 vnode 树）
var tree = h('panel', { id: 'root', size: [-100, -100], bg: 0x14101F }, [
    h('label', { id: 'lbl', text: 'Hello',
                 fg: 0xFFFFFF, font: 'title',
                 align: ['c', 0, -10] }),
    h('button', { id: 'btn', size: [120, 40],
                   bg: 0x06B6D4, radius: 20,
                   align: ['c', 0, 30],
                   onClick: function (ev) { sys.log('clicked'); } }, [
        h('label', { id: 'btnL', text: 'Go',
                     fg: 0x000814, align: ['c', 0, 0] })
    ])
]);

// 2) 挂上去（首次走 mount）
VDOM.mount(tree, null);
sys.ui.attachRootListener('root');   // 整页只调一次

// 3) 改字段（任选一种）
VDOM.set('lbl', { text: 'World', fg: 0xF59E0B });   // 单节点局部改
// 或者重建整树 + diff:
VDOM.render(buildTreeFromState(state), null);
```

### 1.2 节点类型

| type | 说明 |
|---|---|
| `panel`  | 容器（lv_obj） |
| `label`  | 文本 |
| `button` | 按钮（与 panel 类似，但有按下视觉反馈） |

### 1.3 props 全集

| 字段 | 类型 | 例 |
|---|---|---|
| `id` | string | `'root'`（**必填**，全局唯一） |
| `text` | string | `'Hello'`（仅 label / button 内部 label） |
| `bg` | int 0xRRGGBB | `0x14101F` |
| `fg` | int 0xRRGGBB | `0xFFFFFF` |
| `radius` | px | `8` |
| `size` | `[w, h]` | `[120, 40]` 或 `[-100, -100]`（负数=百分比） |
| `align` | `[mode, dx, dy]` | `['c', 0, 0]` 居中；模式见下 |
| `pad` | `[L, T, R, B]` | `[8, 4, 8, 4]` |
| `borderBottom` | int color | `0x333333` |
| `flex` | `'row'` / `'col'` | flex 方向 |
| `gap` | `[rowGap, colGap]` | `[6, 0]` |
| `font` | `'text'` / `'title'` / `'huge'` | |
| `shadow` | `[color, width, ofsY]` | `[0x000000, 12, 4]` |
| `scrollable` | bool | panel 是否可滚 |
| `hidden` | bool | true 隐藏（占位保留，下次 false 即恢复） |
| `onClick` / `onPress` / `onDrag` / `onRelease` / `onLongPress` | fn(ev) | 见 §1.5 |

### 1.4 align 模式

```
'tl'(top-left)  'tm'(top-mid)  'tr'(top-right)
'lm'(left-mid)  'c' (center)   'rm'(right-mid)
'bl'            'bm'           'br'
```

### 1.5 事件回调

```js
function onClick(ev) {
    ev.target;          // 实际被点的子节点 id
    ev.currentTarget;   // 当前 hook 所在节点 id
    ev.dx, ev.dy;       // onDrag 时的位移（其它事件为 0）
    ev.stopPropagation();
    return false;       // 等价 stopPropagation + 终止冒泡
}
```

事件**沿 `_parent` 链冒泡**（最深处的节点先），所以可以在父节点上挂 `onClick` 给一组子按钮统一处理（看 alarm.js 的 `onCardClick`）。

### 1.6 命令式旁路（高频更新）

每次 `render()` 走整树 diff；如果某字段每帧都变（如 30Hz 进度条、拖动位移），用 `VDOM.set` 单点改更省 CPU：

```js
// rerender 整树（按需）
function rerender() { VDOM.render(view(state), null); }

// 单点旁路（高频）
VDOM.set('progFg', { size: [w, 6] });
```

---

## 2. sys.ui —— 命令式绘图（一般不直接用）

> **VDOM 已经包好了所有这些原语**，只在做特殊优化时才直接调。

```js
sys.ui.createPanel(id, parentId)
sys.ui.createLabel(id, parentId)
sys.ui.createButton(id, parentId)
sys.ui.setText(id, "...")
sys.ui.setStyle(id, sys.style.<KEY>, a, b, c, d)
sys.ui.destroy(id)
sys.ui.attachRootListener(id)         // 在最外层 panel 上调一次！
```

> **必须**在你的 root panel mount 完之后调一次 `sys.ui.attachRootListener(rootId)`，否则按钮不响应（VDOM 不会自动调，因为它不知道哪个是 root）。

---

## 3. sys.time —— 时间

```js
// 开机相关
var ms = sys.time.uptimeMs();        // 开机至今毫秒
var s  = sys.time.uptimeStr();       // "00:01:23"

// 真·墙钟（OS 系统时间，由 PC/NTP 同步获得）
var ts = sys.time.now();             // 当前 unix 秒（int）

// 拆解时间分量（按本地时区）
var t = sys.time.parts(ts);
// t = { y:2026, mo:5, d:2, h:14, mi:32, s:7, wday:5, yday:121 }
//   wday：0=周日 .. 6=周六（与 struct tm 一致）

// 格式化字符串（strftime 包装）
sys.time.format(ts, "%H:%M");                // "14:32"
sys.time.format(ts, "%Y-%m-%d %H:%M");       // "2026-05-02 14:32"
sys.time.format(ts, "%A");                   // "Friday"（受系统 locale）
```

约束：
- `now()` 返回 32-bit unix 秒，**2038 年溢出**，不影响近 12 年
- 系统时间需要 PC 端 BLE 时间服务推送过来才有效；未同步前 `now()` 可能返回 0 或编译时刻
- `parts/format` 都是同步调用，性能足够 1Hz 刷新

---

## 4. sys.app —— 每个 app 独享的持久化

```js
sys.app.saveState(jsonString);       // → bool
var raw = sys.app.loadState();       // → string | null（首次返 null）
sys.app.eraseState();                // → bool
```

- 存到 NVS namespace `dynapp`，key 为当前 app 名
- 单条上限 4KB
- 跨 app 互相不可见
- JS 自己 `JSON.stringify` / `JSON.parse`，C 不解析

---

## 5. makeBle —— BLE 路由（推荐入口）

```js
var ble = makeBle("myapp");          // 在脚本顶部调一次

ble.on("data", function (msg) {       // 注册 type 回调
    sys.log(JSON.stringify(msg.body));
});
ble.onAny(function (msg) {            // 收到任何"给我的"消息（不含 ping）
    sys.log("any: " + msg.type);
});
ble.onError(function (raw) {          // JSON 解析失败回调
    sys.log("bad json: " + raw);
});

ble.send("req", { force: true });     // 发 { from:"myapp", type:"req", body:{...} }
ble.isConnected();                    // → bool
ble.appName;                          // 'myapp'
```

行为：
- 自动按 `to` 字段过滤（不是给我的丢掉）
- 自动应答 `ping` → `pong`，不打扰业务
- handler 抛异常会被 catch 住 + sys.log，不会崩 app

> ⚠️ 每个 app 调一次 `makeBle` 即可。再调会**覆盖**底层 onRecv，旧 helper 失效。

详见 [`docs/动态app双端通信协议.md`](./动态app双端通信协议.md)。

---

## 6. setInterval / clearInterval

```js
var id = setInterval(function () { ... }, 100);    // 每 100ms 一次
clearInterval(id);
```

约束：
- 最多 8 个并发 interval
- 没有 `setTimeout`，单次延时自己用 setInterval + 第一次后 clear
- 高频回调（< 50ms）注意不要每帧整树 rerender，用 `VDOM.set` 旁路

---

## 7. sys.symbols —— LVGL 内置图标

```js
var p = sys.symbols.PLAY;
sys.ui.setText(id, sys.symbols.PLAY + " Play");
// 或在 VDOM 里：
h('label', { id: 'x', text: sys.symbols.BLUETOOTH + ' BT' })
```

可用图标见固件 `dynamic_app_natives.c::dynamic_app_natives_bind` 的 `symbols` 段。

---

## 8. sys.font —— 字体

| 常量 | 用途 | VDOM 字符串 |
|---|---|---|
| `sys.font.TEXT` (0)    | 默认正文 14px CJK            | `'text'` |
| `sys.font.TITLE` (1)   | 标题 16px CJK               | `'title'` |
| `sys.font.HUGE` (2)    | 超大数字 48px               | `'huge'` |
| `sys.font.ICON_24` (3) | Material Symbols 24px 图标 | `'icon24'` |
| `sys.font.ICON_36` (4) | Material Symbols 36px 图标 | `'icon36'` |
| `sys.font.NUM_M` (5)   | 中号 ASCII 数字 24px (Montserrat) | `'numM'` |

**重要**：要显示 `sys.icons.*` 的字符，label 的 `font` 必须是 `'icon24'` 或 `'icon36'`，否则字体表里没有那些 codepoint，渲染成□。

---

## 9. 常见坑

| 问题 | 原因 | 解决 |
|---|---|---|
| 屏上空白 | 忘了 `sys.ui.attachRootListener` | mount 完根 panel 后调一次 |
| 按钮不响应 | 同上 | 同上 |
| 卡 menu 不切 | 脚本 build 超 800ms | 简化或拆成 lazy build |
| 高频 setText 闪烁 | 每帧整树 rerender | 高频字段改用 `VDOM.set` 单点 |
| BLE send 一直返 false | PC 没 start_notify 订阅 tx | 确认 PC 端 SDK 跑起来 |
| ble.on 不回调 | 自己又调了一次 `makeBle("xxx")` 覆盖 | 每个 app 只调一次 makeBle |
| `SyntaxError: catch variable already exists` | 同函数内多个 `catch (e)` 同名 | 用 `eParse` / `eAny` 等不同名字 |
| TLSF assert 重启 | 加 native 但没改 `DYNAMIC_APP_EXTRA_NATIVE_COUNT` | 同步改宏 |
| `sys.icons.*` 显示为方块 | label 的 `font` 不是 `'icon24'/'icon36'` | 用图标必须配图标字体 |
| modal 弹出后按钮不响应 | 没在 root panel 上调 `attachRootListener` 之前先 mount | 顺序：mount root → attachRootListener → 之后才能 modal |
| 屏底自定义按钮按不到 | 宿主层占了屏底 28px 做上滑退出区 | 屏底 28px 不放点击元素（仿手机 home indicator） |

---

## 10. 完整最小 app 模板

```js
// hello.js —— 一个按钮，按一下给 PC 发消息

var ble = makeBle("hello");

VDOM.mount(h('panel', { id: 'root', size: [-100, -100], bg: 0x14101F }, [
    h('label', { id: 'hi', text: 'Hello, dynamic world',
                 fg: 0xFFFFFF, font: 'title',
                 align: ['c', 0, -20] }),
    h('button', { id: 'btn', size: [120, 40],
                   bg: 0x06B6D4, radius: 20,
                   align: ['c', 0, 30],
                   onClick: function () {
                       ble.send('greet', { ts: sys.time.uptimeMs() });
                       VDOM.set('hi', { text: 'Sent!' });
                   }}, [
        h('label', { id: 'btnL', text: 'Greet PC',
                     fg: 0x000814, align: ['c', 0, 0] })
    ])
]), null);
sys.ui.attachRootListener('root');

ble.on('reply', function (msg) {
    sys.log('PC said: ' + JSON.stringify(msg.body));
    VDOM.set('hi', { text: 'Got reply' });
});

sys.log('hello: ready');
```

完整业务，**24 行**。框架那 348 行 VDOM + makeBle 全部由 prelude 自动注入。

---

## 11. UI 设计系统（iOS 浅色 + 高阶组件）

> prelude 暴露 `UI` 模块，提供与原生 app 一致的 iOS 浅色风格组件。
> 不强制使用 —— 想做暗色 / 自定义风格仍可直接用 VDOM 原语。
> 详细规范见 [`动态app_UI设计系统.md`](./动态app_UI设计系统.md)。

### 11.1 设计 token（`sys.tokens` / `UI.T`）

```js
// 颜色（iOS 系统色）
UI.T.C_BG          // 0xF2F2F7  屏幕底
UI.T.C_PANEL       // 0xFFFFFF  卡片
UI.T.C_PANEL_HI    // 0xE5E5EA  按下高亮
UI.T.C_BORDER      // 0xC6C6C8  分隔线
UI.T.C_TEXT        // 0x000000  主文字
UI.T.C_TEXT_DIM    // 0x3C3C43  次要
UI.T.C_TEXT_MUTED  // 0x6E6E73  弱
UI.T.C_ACCENT      // 0x007AFF  iOS 蓝（主操作）
UI.T.C_ACCENT_2    // 0xAF52DE  iOS 紫
UI.T.C_OK / C_WARN / C_ERR / C_INFO  // 状态色

// 间距（8 倍数体系）
UI.T.SP_XS=4 / SP_SM=8 / SP_MD=12 / SP_LG=16 / SP_XL=24 / SP_2XL=32

// 圆角
UI.T.R_SM=6 / R_MD=10 / R_LG=14 / R_PILL=1000

// 动画时长（ms）
UI.T.DUR_FAST=150 / DUR_NORM=250 / DUR_SLOW=400
```

**惯例**：业务代码不写裸的 `0xFFFFFF` 或 `12`，统一用 token，未来换肤只改 prelude。

### 11.2 sys.ui.modal / toast / fadeIn —— 通用 UI 能力

```js
// 模态弹层（C 端实现，统一动画 + 手势 + 按钮逻辑）
UI.modal({
    title:   '清空所有通知？',
    body:    '此操作无法撤销。',
    action0: '取消',           // 缺省/空表示无该按钮
    action1: '清空',
    onResult: function (idx) {
        // idx: 0=action0 / 1=action1 / -1=点遮罩或下滑取消
        if (idx === 1) doClearAll();
    }
});

// 屏底 toast，自动消失
UI.toast('已删除', 800);              // 默认 1500ms

// 给已 mount 的对象做淡入（透明 0 → cover）
UI.fadeIn('list', 100);              // 100ms 后开始
```

**模态行为**：
- 全屏半透明黑遮罩 → 点击关闭（dx=-1）
- 居中卡片宽 224 高自适应（最大 270，超过滚动）
- 卡片下滑 ≥30px → 关闭（dx=-1）
- 按钮按下后**先关再回调**，避免回调里操作即将销毁的对象
- 同时只有一个 modal，再调一次先关旧的再开新的

### 11.3 sys.icons —— Material Symbols 图标

```js
// 必须配 'icon24' 或 'icon36' 字体才显示
h('label', { text: sys.icons.BLUETOOTH, font: 'icon24', fg: UI.T.C_ACCENT })

// 可用图标（持续追加）
sys.icons.BLUETOOTH / BT_DISABLED
sys.icons.SCHEDULE / WEATHER / NOTIFICATIONS / MUSIC / TUNE
sys.icons.SETTINGS / BRIGHTNESS / INFO / EDIT_CALENDAR
sys.icons.APPS / CHEVRON_LEFT / CHEVRON_RIGHT / DOT / DOT_SMALL
```

需要更多 codepoint 见 `app/app_fonts.h`，加进 `dynamic_app_natives.c::sys.icons.*` 即可暴露。

### 11.4 UI 高阶组件

仿原生 `app/ui/ui_widgets.c`，覆盖动态 app 90% 的"标准布局"需求。

#### `UI.screen(id, children)`
全屏底容器（白底 `C_BG`，撑满，pad 0）。整个 app 的最外层 panel。

#### `UI.title(text, props?)`
页面大标题（左对齐 16px CJK，主文字色，自带左右 16px / 上下 ?）

#### `UI.card(opts, children)`
白底卡片 + 1px 浅描边 + 圆角 14。
```js
UI.card({ pad: [0,0,0,0] }, [   // pad=[0,0,0,0] 让里面 listRow 自管 padding
    UI.listRow({...}),
    UI.listRow({...})
])
```

#### `UI.kvRow({ key, value, valueId?, divider? })`
两端对齐的键值行（左灰 key + 右黑 value），底部 1px 分隔线。

#### `UI.listRow({ icon, label, value?, iconBg?, iconColor?, onClick?, divider?, valueId?, id? })`
iOS 列表行：左 28×28 圆角彩底图标块 + label（自动 dot 截断）+ 可选 value + chevron 右指。整行作为 button，按下自动变浅灰高亮。
```js
UI.listRow({
    icon: sys.icons.BLUETOOTH,
    label: '蓝牙',
    value: '已连接',
    iconBg: UI.T.C_ACCENT,
    onClick: function () { gotoBluetoothPage(); }
})
```

#### `UI.iconBtn({ icon, w?, h?, color?, onClick?, id? })`
透明图标按钮 + 按下 accent 蒙层。

#### `UI.pillBtn({ text, w?, h?, bg?, fg?, onClick?, id? })`
胶囊按钮（默认 accent 蓝底白字）。

#### `UI.badge({ text, bg?, fg?, w?, h?, id? })`
小圆角胶囊（数字徽章 / tag）。

#### `UI.statusBar({ title, right? })`
**业务自绘的标题区**，44px 高（不是固件状态栏；那个是宿主层挂的 24px 电池/蓝牙状态栏）。
```js
UI.statusBar({ title: '通知', right: sys.icons.SETTINGS })
```

#### `UI.divider()` / `UI.hitZone({ onExit })` / `UI.swipeExit(children, fn)`
辅助：分隔线 / 屏底退出区（一般用不到，宿主层兜底）。

### 11.5 宿主层契约（你不需要做的事）

dynapp_host 在动态 app 启动前已挂好：
- 顶部 24px 状态栏（电池胶囊 / 蓝牙 / 时间），与原生 app 一致
- 屏底 28px 透明上滑退出区（dy ≥ 30 触发回 launcher）

所以：
- **可用区是 240×268**（屏 320 减 24 状态栏减 28 退出区）
- 业务 root panel 用 `UI.screen('id', [...])`，会自动撑满 list_root（已对齐到 24px 起）
- **屏底 28px 不放点击元素**（会被宿主 hit zone 吞）

### 11.6 用 UI 库重写 §10 的最小模板

```js
// hello.js —— 用 UI 库的版本（视觉自动对齐原生）
var ble = makeBle("hello");

VDOM.mount(UI.screen('root', [
    UI.statusBar({ title: 'Hello' }),
    UI.card({}, [
        h('label', { id: 'msg', text: 'Hello, dynamic world',
                     fg: UI.T.C_TEXT, font: 'text' })
    ]),
    UI.pillBtn({
        id: 'btn', text: 'Greet PC',
        onClick: function () {
            ble.send('greet', { ts: sys.time.now() });
            UI.toast('已发送', 800);
            VDOM.set('msg', { text: '已发送，等待回复…' });
        }
    })
]), null);
sys.ui.attachRootListener('root');

ble.on('reply', function (msg) {
    UI.modal({
        title: 'PC 回复',
        body:  JSON.stringify(msg.body),
        action1: '好的'
    });
});

UI.fadeIn('root', 0);
sys.log('hello: ready');
```

**视觉与原生 app 几乎一致**，业务逻辑仍然短小。


---

## 12. Router —— 多级页面栈（push/pop 路由）

> prelude 暴露 `Router` 命名空间，**纯 JS 层**实现的页栈。每页独立 mount/destroy，
> 切页时旧页 hidden，回退时直接显示，不会丢滚动位置 / canvas 内容 / setInterval。
>
> 不强制使用 —— 单页 app 完全可以照常用 `VDOM.mount`。**只有"列表→详情"这种
> 多级流才用 Router**，避免把单页 app 也用上 Router 反而绕。

### 12.1 心智模型

```
Router.define(name, builder)        builder(props) → vnode (panel 类型)
Router.start(name, props?)          调一次。第一次 mount + attachRootListener
Router.push(name, props?)            入栈（栈深 +1，最大 4）
Router.replace(name, props?)         替换栈顶（栈深不变）
Router.pop()                         出栈
Router.popTo(name)                   出栈到指定 name 为栈顶
Router.current()                     → { name, props, depth }
Router.depth()                       → 栈深整数
Router.onEnter(name, fn(props))       生命周期：每次进入触发（push / pop 回退都触发）
Router.onLeave(name, fn())            离开时触发（push 走 / pop 走 / replace 替换）
```

### 12.2 最小例子

```js
function buildList() {
    return h("panel", { bg: UI.T.C_BG, flex: "col" }, [
        UI.statusBar({ title: "列表" }),
        UI.card({ pad: [0,0,0,0] }, [
            UI.listRow({ icon: sys.icons.INFO, label: "条目 A",
                onClick: function () { Router.push("detail", { id: "A" }); } }),
            UI.listRow({ icon: sys.icons.INFO, label: "条目 B",
                onClick: function () { Router.push("detail", { id: "B" }); } })
        ])
    ]);
}

function buildDetail(props) {
    return h("panel", { bg: UI.T.C_BG, flex: "col" }, [
        h("panel", { size: [-100, 36], bg: UI.T.C_PANEL, flex: "row",
                     pad: [4,2,12,2], gap: [0,8] }, [
            h("button", { size: [40,28], bg: UI.T.C_PANEL, scrollable: false,
                          pressedBg: { color: UI.T.C_PANEL_HI, opa: 255 },
                          onClick: function () { Router.pop(); } }, [
                h("label", { text: sys.icons.CHEVRON_LEFT, font: "icon24",
                             fg: UI.T.C_ACCENT, align: ["c",0,0] })
            ]),
            h("label", { text: "详情：" + (props.id || "?"),
                         fg: UI.T.C_TEXT, font: "title" })
        ])
    ]);
}

Router.define("list",   buildList);
Router.define("detail", buildDetail);
Router.start("list");
```

### 12.3 约束 & 默认行为

- **每页根 vnode 必须是 `panel`**；Router 强制塞 `id`、`size: [-100,-100]`、`pad: [0,0,0,0]`
- 业务想纵向布局子节点要自己设 `flex: "col"`（Router 不接管 flex）
- **栈深 ≥ 2 时屏底上滑 = pop**；栈深 = 1 时上滑穿透到宿主层 = 退出 app
- **栈深上限 4**：超过 push 时 `sys.log` warn + 忽略
- **同名页递归 push 自动加 id 后缀**（`detail` 第二次 push 时变 `detail__r2`）
- `pop` 时旧页**真正销毁**（VDOM.destroy）；保留下层栈所有页（含其滚动位置 / canvas）
- Router **不接管 setInterval / ble.on**：业务在 `onLeave` 里自己 `clearInterval`

### 12.4 何时不用 Router

- 单页 app（gomoku / doodle / notif）—— 直接 VDOM.mount 简单
- 用 modal 已经能满足"二级页"语义（doodle 画廊 / gomoku 认输确认）
- 双页平铺切换（音乐/系统圆盘那种 tab 滑动）—— 用 hidden 自己切，不需要栈语义

### 12.5 hidden vs Router.pop 怎么选

- 想保留页内**所有状态**且**可能多次切换**（侧边抽屉 / tab 内切换）→ 自己用 `VDOM.set(id, { hidden: true })`
- 想要**返回栈语义**（pop 回上一级 / popTo 回首页）→ 用 Router


