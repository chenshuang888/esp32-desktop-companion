# 动态 App 开发者指南

> 给下一位作者的单一入口：看完这一篇，30 分钟内你能跑通第一个 hello，
> 1 小时内能写一个真正能用的 app。

**前置条件**：
- 你能 `git clone` 这个仓库并跑起来固件 + PC 端伴侣
- 会 JavaScript 的基础（变量、函数、对象、回调）；不需要框架背景

**目录**：
- §1 [30 分钟跟着做：Hello World](#1-30-分钟跟着做hello-world)
- §2 [给 app 加 BLE 通信](#2-给-app-加-ble-通信)
- §3 [给 app 配套 PC 插件](#3-给-app-配套-pc-插件)
- §4 [VDOM / h() 完整 API](#4-vdom--h-完整-api)
- §5 [UI 组件库速查](#5-ui-组件库速查)
- §6 [Router 多级页面](#6-router-多级页面)
- §7 [makeBle BLE 客户端](#7-makeble-ble-客户端)
- §8 [sys.* 系统调用速查](#8-sys-系统调用速查)
- §9 [持久化与计时器](#9-持久化与计时器)
- §10 [常见坑速查](#10-常见坑速查)
- §11 [发布与上传](#11-发布与上传)

---

## 1. 30 分钟跟着做：Hello World

### 1.1 一个动态 app 是什么

**动态 app = 一个目录 + 一个 JS 脚本**。固件不重烧，把目录推到设备就能跑。最小结构：

```
dynamic_app/scripts/<my_pkg>/
├── manifest.json    包元信息（id / 名字 / 图标 / 颜色）
└── main.js          业务代码
```

不需要 build / npm / webpack —— 设备端有自带的 JS 引擎（QuickJS）直接读你写的 .js。

### 1.2 仓库已有的 hello_pkg

打开 `dynamic_app/scripts/hello_pkg/main.js`：

```js
var T = UI.T;          // 颜色 / 间距 / 圆角等设计 token
var clicks = 0;

var page = h('panel', {
    bg: T.C_BG, flex: 'col', gap: [16, 0], pad: [24, 16, 24, 16],
    flexAlign: ['center', 'center', 'center']
}, [
    h('label', { text: 'Hello!', fg: T.C_TEXT, font: 'huge' }),
    h('label', { id: 'tip', text: '点下面按钮', fg: T.C_TEXT_MUTED, font: 'text' }),
    UI.pillBtn({
        text: '点我',
        textId: 'btnLbl',
        onClick: function () {
            clicks++;
            VDOM.set('tip',    { text: '已点 ' + clicks + ' 次' });
            VDOM.set('btnLbl', { text: '再点一次' });
            UI.toast('Hi 👋', 600);
        }
    })
]);

VDOM.mount(page, null);
sys.ui.attachRootListener(page.props.id);
```

**15 行业务代码**。这就是一个完整可跑的 app。下面解释每一行做什么。

### 1.3 逐行解释

```js
var T = UI.T;
```
`UI.T` 是设计 token（`C_BG`、`C_TEXT`、`SP_LG` 等）。**永远从 token 取颜色 / 间距，别写硬编码 `#FFFFFF`** —— 整个动态 app 共享一套 iOS 风格视觉语言，token 让所有 app 看起来一致。

```js
var page = h('panel', { bg: ..., flex: 'col', ... }, [...]);
```
`h(type, props, children)` 是 React/Vue 风格的虚拟节点构造器。返回的 `page` 不是 LVGL 对象——只是 JS 内存里的一棵描述树。

```js
flex: 'col', flexAlign: ['center', 'center', 'center']
```
column 布局，主轴 / 交叉轴 / 多行轴都居中。

```js
UI.pillBtn({ text: '点我', textId: 'btnLbl', onClick: function () {...} })
```
高阶组件。`textId` 给内部 label 一个 id —— 等下要在 onClick 里改文字，必须有 id 才能定位。

```js
VDOM.set('tip', { text: '已点 ' + clicks + ' 次' });
```
按 id 局部更新一个节点的属性。**比重新 mount 整棵树快得多**——只把变化的属性下发到 LVGL。

```js
UI.toast('Hi 👋', 600);
```
屏底浮一条文字，600ms 自动消失。

```js
VDOM.mount(page, null);
sys.ui.attachRootListener(page.props.id);
```
**两行启动代码**：把虚拟树挂到屏幕根（parentId=null），再让事件监听器知道根节点是谁——按钮点击事件能冒泡到 onClick。

### 1.4 跑起来

```bash
# 1. 给 hello_pkg 生成 manifest.json（已生成，跳过）
python tools/scripts/make_pack_manifest.py dynamic_app/scripts/hello_pkg \
    --id hello_pkg --name Hello --icon GAME --color ACCENT

# 2. 启动 PC 端伴侣（后台跑就行）
python -m companion

# 3. PC GUI 上打开「上传」页 → 选 dynamic_app/scripts/hello_pkg → 上传
#    或者用 dynapp_uploader CLI（看 §11）

# 4. 设备菜单里会自动出现 "Hello"，点进去就能用
```

### 1.5 改动 + 调试节奏

**改了 main.js 想看效果**：重新打包上传一次（设备端会重新加载脚本），不需要重启固件。

**怎么看 sys.log 输出**：固件串口（`idf.py monitor`）能看到 `[dynapp]` 标签的所有 sys.log 输出。这是你**唯一**的调试通道——没有断点、没有 Chrome DevTools。**重要的状态都 sys.log 一下**。

---

## 2. 给 app 加 BLE 通信

如果你的 app 需要 PC 帮忙做事（拉网络数据、抓系统状态、AI 服务等），就要走 BLE。

> **先停一下问自己**：你的 app 真的需要 PC 配合吗？纯本地能完成的（计算器 / 番茄钟 / 2048）**不要**走 BLE。看 `tools/plugins/README.md` §0 的判定规则。

### 2.1 BLE 客户端最小用法

```js
var APP_NAME = 'my_pkg';                 // 必须跟 manifest.id 一致
var ble = makeBle(APP_NAME);

// 监听 PC 发来的某种类型消息：
ble.on('result', function (body) {
    // body 是 PC 发来的 { ... } 对象（已 unwrap，不是 raw msg）
    sys.log('got result: ' + JSON.stringify(body));
    VDOM.set('out', { text: body.value });
});

// 主动发消息给 PC：
ble.send('query', { keyword: 'hello' });
//        ↑ type            ↑ body（任意可 JSON 化的对象）
```

**约定**：
- 设备 → PC 的消息长这样：`{ from: appName, type: 'query', body: {...} }`
- PC → 设备的消息长这样：`{ to: appName, type: 'result', body: {...} }`
- 不匹配 `to` 字段的消息会被自动丢弃（不会到你的 ble.on）
- `type === 'ping'` 会被框架自动回 `pong`，业务无需处理

### 2.2 完整 makeBle API

```js
var ble = makeBle('my_pkg');

ble.send(type, body)         // 返回 bool；body 可省
ble.on(type, fn)             // fn(body, meta) —— body 是已解开的对象
                             //    meta = { type, from } 一般用不到
ble.onAny(fn)                // fn(msg)：所有 to 给我的消息（不含 ping）
ble.onError(fn)              // fn(rawString)：JSON 解析失败时
ble.isConnected()            // -> bool
ble.appName                  // 字符串，等于 makeBle 传入的 name
```

### 2.3 协议设计建议

**简单的请求 / 响应**：
```js
ble.on('weather_resp', function (body) { ... });
ble.send('weather_req', { city: 'beijing' });
```

**长连接握手 + 持续推送**（gomoku / tictactoe 模式）：
```js
ble.on('ready', function () { /* PC 准备好了 */ });
ble.on('move',  function (body) { /* PC 推一步 */ });
ble.send('hello');                       // 启动握手
```

**没有 PC 时降级**：调 `ble.send` 不会报错，发出去也没人收。业务自己用 `ble.isConnected()` 判断 + 给个超时兜底。

---

## 3. 给 app 配套 PC 插件

走 BLE 就要有 PC 端对面。**插件 = ESP32 动态 app 在 PC 端的「代理人」**。

### 3.1 一个最小插件长什么样

```
tools/plugins/my/
└── plugin.py        ← 50 行以内能跑
```

```python
# tools/plugins/my/plugin.py
from companion.plugin_sdk import Plugin

class MyPlugin(Plugin):
    plugin_id = "my"               # 唯一 id
    title     = ""                 # 空 = 没 GUI 页
    bind_app  = "my_pkg"           # 设备端 app id；填 None 表示通用服务

    async def on_message(self, msg):
        mtype = msg.get("type")
        body  = msg.get("body") or {}

        if mtype == "weather_req":
            city = body.get("city", "beijing")
            data = await fetch_weather(city)        # 你自己实现
            await self.tx("weather_resp", body=data)
```

### 3.2 插件加载

把目录拷到 `tools/plugins/`，启动 PC 端伴侣 → 自动扫描加载。**修改老插件代码后必须重启 `python -m companion`**（不支持热重载）。

### 3.3 插件可调用的 SDK

```python
class MyPlugin(Plugin):
    # 类属性（必填 plugin_id；其他可选）
    plugin_id: str
    title:     str           # 空 = 无 GUI 页；非空 = 侧边栏出现
    bind_app:  str | None    # None = 通用服务（接所有 from 的消息）

    # 生命周期 hook（按需覆盖）
    def on_load(self):                       # 实例化后一次
    def on_unload(self):                     # 退出时清理
    async def on_connect(self, addr):        # BLE 连上设备
    async def on_disconnect(self):           # BLE 断开
    async def on_message(self, msg):         # 收到 from=bind_app 的消息

    # 工具方法
    self.tx(type, body=None)                 # 发给 self.bind_app
    self.tx_to(app_id, type, body=None)      # 发给任意 app
    self.is_connected() -> bool
    self.create_task(coro) -> asyncio.Task   # 后台任务，unload 时自动取消
    self.bus                                 # 全局事件总线（GUI 跨线程通信用）
    self.log                                 # logging.getLogger("plugin.<id>")

    # GUI（可选；title 非空时会被调用）
    def make_gui_page(self, master, app):
        from gui_page import MyPage
        return MyPage(master, app)
```

### 3.4 想给插件加 GUI 页？

参考 `tools/plugins/tictactoe/gui_page.py`（200 行的只读监控面板）或 `tools/plugins/gomoku/gui_page.py`（500 行的双向交互棋盘）。

**关键点**：
- 用 `from companion.plugin_sdk.gui import theme, widgets`，**不要**直接 `from companion.gui.*`
- 插件 emit `bus("my:state", ...)`，GUI 页订阅并 `self.after(0, ...)` 跨线程刷新
- 插件 → GUI 单向流，GUI → 插件再走 bus 回写

详见 [`tools/plugins/README.md`](../../tools/plugins/README.md)。

---

## 4. VDOM / h() 完整 API

### 4.1 节点类型

```js
h('panel',  props, children)   // 通用容器（lv_obj）
h('label',  props)             // 文字（lv_label）
h('button', props, children)   // 可点击容器（lv_btn）；天然 PRESSED 视觉态
h('image',  props)             // 图片（src 来自 sys.fs）
h('canvas', props)             // 像素画布（详见 §8）
```

### 4.2 通用 props

| 字段 | 类型 | 说明 |
|---|---|---|
| `id` | string | 节点唯一 id；省则自动生成 |
| `text` | string | 仅 label 有效 |
| `bg` | int | 背景颜色 0xRRGGBB |
| `bgOpa` | int 0-255 | 背景透明度 |
| `fg` | int | 文字颜色（label / button 子 label） |
| `radius` | int | 圆角半径（px） |
| `pad` | `[t,r,b,l]` | 内边距（顺时针） |
| `align` | `['mode', dx, dy]` | 见下方 align 表 |
| `size` | `[w, h]` | **正数=px / 负数=百分比 / UI.SIZE_CONTENT=自适应** |
| `flex` | `'row' / 'col'` | 开 flex layout |
| `flexAlign` | `[main, cross, track]` | 见下方 flexAlign 表 |
| `gap` | `[row, col]` | flex 子项间距（px） |
| `grow` | int | flex grow 系数 |
| `scrollable` | bool | panel 默认 false；列表页业务自己开 |
| `border` | `{color, width, side?, opa?}` | side: 'full'/'top'/'bottom'/'left'/'right' |
| `pressedBg` | `{color, opa?}` | button 按下时的覆盖色 |
| `font` | `'text' / 'title' / 'huge' / 'icon24' / 'icon36' / 'numM'` | 字体 |
| `textAlign` | `'left' / 'center' / 'right'` | label 内文字对齐 |
| `longMode` | `'wrap' / 'dot' / 'scroll' / 'clip'` | label 超长处理 |
| `hidden` | bool | 隐藏（不影响布局占位） |
| `opa` | int 0-255 | 整体透明度 |
| `rotate` | number 或 `[deg, pivotX, pivotY]` | 旋转 0.1° 单位 |
| `shadow` | `[color, width, offsetY]` | 阴影 |
| `onClick` / `onPress` / `onDrag` / `onRelease` / `onLongPress` | function | 事件回调 |

### 4.3 align 字符串

```
'tl' top-left   't' top-mid   'tr' top-right
'lm' left-mid   'c' center    'rm' right-mid
'bl' bot-left   'bm' bot-mid  'br' bot-right
```

### 4.4 flexAlign 字符串

`['main', 'cross', 'track']`，每项可选：
- `'start'`（默认）
- `'end'`
- `'center'`
- `'evenly'` / `'around'` / `'between'`（仅 main 轴）

### 4.5 VDOM 全局函数

```js
VDOM.h(type, props, children)        // 构造 vnode（同顶层 h()）
VDOM.mount(node, parentId)           // 挂载整棵树；parentId=null 表示挂到屏幕根
VDOM.set(id, patch)                  // 局部更新单节点 props（最常用）
VDOM.find(id)                        // 按 id 查 vnode
VDOM.destroy(id)                     // 销毁节点 + 子树
VDOM.render(newTree)                 // 全量 diff 根树（少用，大多场景 set 就够）
VDOM.dispatch(id, type, dx, dy)      // 手动派发事件（一般不用）
```

**生效时机**：`VDOM.set('x', {...})` **立刻**改 JS 内存里的 props，但实际屏幕渲染要等 UI 线程下一次 drain（几毫秒）。99% 场景感觉不到。

---

## 5. UI 组件库速查

`UI.*` 是封装好的高阶组件，比裸 `h()` 节省一半代码。所有组件都接受 `opts` 对象（`screen` 例外，向后兼容）。

### 5.1 颜色 / 间距 token（`UI.T`）

```
颜色：
  C_BG 0xF2F2F7         屏幕底色（浅灰）
  C_PANEL 0xFFFFFF      卡片白
  C_PANEL_HI 0xE5E5EA   高亮按下色
  C_BORDER 0xC6C6C8     边框
  C_TEXT 0x000000       主文字（黑）
  C_TEXT_DIM 0x3C3C43   次文字
  C_TEXT_MUTED 0x6E6E73 弱文字
  C_ACCENT 0x007AFF     iOS 蓝
  C_ACCENT_2 0xAF52DE   紫
  C_OK 0x34C759         绿
  C_WARN 0xFF9500       橙
  C_ERR 0xFF3B30        红
  C_INFO 0x5AC8FA       浅蓝

间距：SP_XS=4 SP_SM=8 SP_MD=12 SP_LG=16 SP_XL=24 SP_2XL=32

圆角：R_SM=6 R_MD=10 R_LG=14 R_PILL=1000

时长：DUR_FAST=150ms DUR_NORM=250ms
```

### 5.2 图标（`UI.I` 或 `sys.icons`）

常用：`SCHEDULE / WEATHER / NOTIFICATIONS / MUSIC / SETTINGS / BRIGHTNESS / INFO / APPS / CHEVRON_LEFT / CHEVRON_RIGHT / DOT / ALARM / TIMER / GAME / CALCULATOR / TARGET / BLUETOOTH ...`

完整列表见 `dynamic_app/dynamic_app_natives.c::1462+`。

### 5.3 SIZE_CONTENT

`UI.SIZE_CONTENT` —— size 字段填它表示「按子内容自适应」。等价于 LVGL 的 `LV_SIZE_CONTENT`。

```js
size: [-100, UI.SIZE_CONTENT]    // 撑满父宽，高自适应
size: [UI.SIZE_CONTENT, 40]      // 宽自适应，固定高 40
```

### 5.4 容器

```js
UI.screen('id', children)                       // 全屏底容器（屏幕根）
UI.title('页面大标题', { id?, fg?, font? })

UI.card({
    id?, pad?,                                  // pad 默认 [12,12,12,12]
    size?,                                      // 默认 [-100, SIZE_CONTENT]
    flex?,                                      // 默认 'col'（垂直堆叠子项）
    align?, gap?, flexAlign?
}, children)
```

`UI.card` 默认 = "撑满宽 + 垂直堆叠 + 12px 内边距"，**99% 场景不用改默认**。

### 5.5 列表 / 行

```js
UI.listRow({
    icon, label, value?, valueId?,
    iconBg?, iconColor?,
    onClick?, divider?, id?
})

UI.kvRow({
    key, value, valueId?,
    divider?, id?
})

UI.divider()                       // 1px 浅描边
```

### 5.6 按钮

```js
UI.pillBtn({
    text, textId?,                 // textId 给内部 label 一个 id，方便改文字
    w?, h?, bg?, fg?,
    onClick?, id?
})

UI.iconBtn({
    icon, iconId?,                 // iconId 同上
    color?, w?, h?,
    onClick?, id?
})
```

### 5.7 标签 / 标题栏

```js
UI.badge({ text, textId?, w?, h?, bg?, fg?, id? })

UI.statusBar({
    title, titleId?,
    right?, rightId?,              // 右侧图标
    compact?                       // 默认 false（高 44）；true = 高 28（小屏省空间）
})
```

### 5.8 反馈

```js
UI.toast('文字', durMs)                // 屏底浮一条；durMs 默认 1000

UI.modal({
    title, body,
    action0?, action1?,                // 两个按钮文字；省略 = 单按钮
    onResult: function (idx) {
        // idx: 0/1 = 按钮序号；-1 = 点遮罩 / 下滑取消
    }
})

UI.fadeIn(id, delayMs)             // 给已挂载对象做淡入
```

### 5.9 退出区

```js
UI.hitZone({ id?, onExit })        // 屏底 30px 透明上滑退出区
UI.swipeExit(rootChildren, onExit) // 一行加上：children + 自动 hitZone
```

---

## 6. Router 多级页面

如果你的 app 不止一页（列表 → 详情 → 设置），用 Router。**单页 app 不要用 Router**，直接 `VDOM.mount` 就行。

### 6.1 基本用法

```js
function buildHome(props) {
    return h('panel', { ... }, [...]);   // 必须返回 panel vnode
}

function buildDetail(props) {
    return h('panel', { ... }, [
        topbarWithBack(),                // 你自己写一个带"<"按钮调 Router.pop() 的顶栏
        // 用 props 拿到 push 时传入的参数
        ...
    ]);
}

Router.define('home',   buildHome);
Router.define('detail', buildDetail);

Router.start('home');                    // 启动；只调一次
```

```js
Router.push('detail', { itemId: 42 });  // 入栈，下层页保留（只 hidden）
Router.pop();                            // 出栈 + 销毁顶层
Router.replace('other');                 // 替换栈顶
Router.popTo('home');                    // 出栈到指定页

Router.depth();                          // 栈深
Router.current();                        // -> { name, props, depth }
```

### 6.2 生命周期

```js
Router.onEnter('detail', function (props) {
    // 每次进入该页（push / pop 回退）都触发；可读 props 重新拉数据
});

Router.onLeave('detail', function () {
    // push 走 / pop 走 / replace 替换 都触发；保存草稿、清 setInterval
});
```

### 6.3 退出 app 与上滑

Router 自动给你处理屏底上滑：
- 栈深 ≥ 2 时上滑 = pop
- 栈深 = 1 时上滑 = 穿透到宿主层 → 退出整个 app

业务**完全不用**自己加 hitZone。

### 6.4 注意

- 每个 builder 必须返回 `panel` vnode（不能是 button / label）
- Max depth = 4。更深说明 UX 设计有问题，不要试图绕过
- 不接管 setInterval / ble.on，业务在 onLeave 里自己 clearInterval

完整范例：`dynamic_app/scripts/settings_pkg/main.js`（多级页 + 持久化）。

---

## 7. makeBle BLE 客户端

详见 §2。补充：

### 7.1 自动 ping/pong

PC 端可以发 `{type:"ping"}` 探活，框架自动回 `{type:"pong"}`。业务侧 `ble.on("ping")` 不会被触发——这是平台保留 type。

### 7.2 多 type 派发的常见模式

```js
ble.on('hello',  function (body) { /* ... */ });
ble.on('update', function (body) { /* ... */ });
ble.on('reset',  function (body) { /* ... */ });

// 不在乎类型时（少用）
ble.onAny(function (msg) {
    sys.log('any: ' + JSON.stringify(msg));
});
```

### 7.3 长度限制

BLE 单条消息限 `BRIDGE_MAX_PAYLOAD`（约 200 字节，UTF-8）。**别一次发整个数组**，分批 + 业务自己拼。

---

## 8. sys.* 系统调用速查

业务应该**优先用 VDOM / UI / Router**——它们都是 sys.* 的封装。直接用 sys.* 仅在两种场景：

1. 没有现成封装（如 sys.fs / sys.canvas / sys.time）
2. 性能极致优化（绕过 VDOM 直接发 cmd）

### 8.1 sys.ui.*（一般不直接用，VDOM 已封装）

```
sys.ui.createPanel(id, parentId)
sys.ui.createLabel(id, parentId)
sys.ui.createButton(id, parentId)
sys.ui.createImage(id, parentId, src)
sys.ui.setText(id, text)
sys.ui.setStyle(id, key, a, b, c, d)
sys.ui.setImageSrc(id, src)
sys.ui.attachRootListener(id)        ← VDOM.mount 之后必须调一次
sys.ui.destroy(id)
sys.ui.modal(opts)                    ← 用 UI.modal 即可
sys.ui.toast(text, ms)                ← 用 UI.toast 即可
sys.ui.fadeIn(id, delayMs)
```

### 8.2 sys.time.*

```js
sys.time.uptimeMs()        // -> int  开机至今毫秒（性能测量用）
sys.time.uptimeStr()       // -> '0d 1h 23m'
sys.time.now()             // -> unix 秒（OS 系统时间）
sys.time.parts(unixSec)    // -> { y, mo, d, h, mi, s, wday, yday }
sys.time.format(unixSec, '%Y-%m-%d %H:%M')   // strftime
```

### 8.3 sys.app.*（持久化，详见 §9）

```js
sys.app.saveState(jsonString)  // 字符串！别传对象，要 JSON.stringify
sys.app.loadState()            // -> string | null
sys.app.eraseState()           // 清空
```

### 8.4 sys.ble.*

`makeBle()` 已封装。如果一定要裸用：

```js
sys.ble.send(jsonString)
sys.ble.onRecv(function (raw) { ... })
sys.ble.isConnected()
```

### 8.5 sys.fs.*（沙箱文件 IO）

每个 app 的沙箱目录是 `apps/<your_id>/data/`（持久化在 LittleFS）。

```js
sys.fs.read(path)          // -> string | null
sys.fs.write(path, content) // -> bool（异步入队）
sys.fs.exists(path)        // -> bool
sys.fs.remove(path)        // -> bool
sys.fs.list()              // -> [filename, ...]
```

`saveState` 内部就是 `sys.fs.write('state.json', ...)` 的封装。**简单 KV 用 saveState；多文件 / 大数据才用 sys.fs.***

### 8.6 sys.canvas.*（像素绘图）

```js
sys.canvas.create(id, parentId, w, h)
sys.canvas.fill(id, color)
sys.canvas.setPixel(id, x, y, color)
sys.canvas.line(id, x0, y0, x1, y1, color, thickness?)
sys.canvas.saveTo(id, relpath)         // 存为 .bin（自家格式，可 loadFrom 复用）
sys.canvas.loadFrom(id, relpath)
```

参考 `dynamic_app/scripts/doodle_pkg/main.js`（涂鸦板）。

### 8.7 sys.log

```js
sys.log('hello ' + foo);   // 只接受字符串；非字符串自己 String(x)
```

唯一调试通道。串口 monitor 时显示。

---

## 9. 持久化与计时器

### 9.1 saveState / loadState（推荐）

```js
var state = { count: 0, items: [] };

(function loadState() {
    var raw = sys.app.loadState();
    if (!raw) return;
    try {
        var s = JSON.parse(raw);
        if (typeof s.count === 'number') state.count = s.count | 0;
        if (Array.isArray(s.items))      state.items = s.items;
    } catch (e) { sys.log('bad state: ' + e); }
})();

function saveState() {
    sys.app.saveState(JSON.stringify(state));
}
```

**铁律**：
- 不在每次状态变都调 saveState（会卡 NVS）。在 onLeave / 定时 / 关键节点写一次
- loadState 失败时**降级到默认值**（用户可能首次启动 / 文件损坏）
- 字段类型校验（typeof）保护——状态文件可能被旧版本写过

### 9.2 setInterval / setTimeout / clearXxx

```js
// 每秒一次
var t = setInterval(function () {
    if (done) { clearInterval(t); return; }
    tick();
}, 1000);

// 一次性延后
setTimeout(function () { onTimeout(); }, 1500);

// 提前取消
clearTimeout(t);
```

**坑**：JS 没真线程。setInterval 回调在 UI 线程跑——回调里别做 100ms 以上的活（卡屏幕）。

---

## 10. 常见坑速查

新作者最容易踩的 15 个坑（按踩中概率排序）：

| # | 坑 | 后果 / 解决 |
|---|---|---|
| 1 | `size` 编码：正数=px，负数=百分比，`UI.SIZE_CONTENT`=自适应 | 写错值会布局崩 |
| 2 | flex 列布局里子项不写 size，会被压成 SIZE_CONTENT 然后挤一团 | 显式 `size: [-100, ...]` |
| 3 | 改按钮文字必须给内部 label 一个 id（用 `textId`） | UI.pillBtn / badge 都支持 |
| 4 | 改 label 文字必须 `VDOM.set(id, {text: ...})` —— 直接改 vnode props 没用 | 找 id 调 set |
| 5 | mount 完必须 `sys.ui.attachRootListener(id)`，否则按钮没反应 | Router.start 替你做了；裸 mount 要自己调 |
| 6 | `ble.on(type, fn)` 的 fn 收 **body** 不是 msg | 旧文档可能误导 |
| 7 | `saveState` 必须传字符串：`JSON.stringify(state)` | 漏 stringify 会报错 |
| 8 | `font` 用字符串 `'text' / 'title' / 'huge' / 'icon24'`，不是数字 | 看 §4.2 表 |
| 9 | flex column 默认 cross_align 是 CENTER；想左对齐显式 `flexAlign:['start','start','center']` | |
| 10 | UI.statusBar 默认占 60px（44 高 + 16 pad）；小屏内容稠密时换 `compact: true` | |
| 11 | UI.card 默认已经是 `flex:'col' + size:[-100, CONTENT]`，不用每次传 | 直接 `UI.card({}, [...])` 即可 |
| 12 | Router 业务 builder 必须返回 `panel` vnode，不能是 button / label | |
| 13 | onLeave 里没 clearInterval → 离开页面后 timer 还在跑，且操作的 vnode 已销毁 | 必须清 |
| 14 | `setInterval` 回调里的 `this` 不是你想的；用闭包变量代替 | |
| 15 | 改完 main.js 必须重新打包上传，设备会重新加载脚本（不用重启固件） | |

---

## 11. 发布与上传

### 11.1 manifest.json 字段

```json
{
    "id":        "my_pkg",
    "name":      "我的应用",
    "icon":      "STAR",
    "iconColor": "ACCENT",
    "version":   "1.0.0"
}
```

- `id` 必须等于目录名，且全局唯一（菜单上不能重复）
- `name` 可中文，会显示在菜单里
- `icon` 必须是 sys.icons 已注册的 key（看 `dynamic_app/dynamic_app_natives.c::1462+`）
- `iconColor` 可选 token：`ACCENT / ACCENT_2 / OK / WARN / ERR / INFO / TEXT / TEXT_MUTED / PANEL / BG`

不想手写就用工具：

```bash
python tools/scripts/make_pack_manifest.py dynamic_app/scripts/my_pkg \
    --id my_pkg --name 我的应用 --icon STAR --color ACCENT
```

### 11.2 上传到设备

**两种方式**：

**A. PC GUI**：启动 `python -m companion`，打开「上传」页 → 选 pack 目录 → 推送。

**B. 命令行**：

```bash
# bridge_test.py 是 dynapp_sdk 的 example，可以联调
python tools/dynapp_sdk/examples/bridge_test.py --to my_pkg
> {"type":"hello"}
```

实际上传 .pkg 走 dynapp_uploader（参考 PC GUI 上传页的实现）。

### 11.3 调试节奏

```
改 main.js → 上传 → 设备菜单点 app → idf.py monitor 看日志
                                       ↑ 唯一调试通道
```

如果 app 启动就崩：
- `idf.py monitor` 看 `[dynapp]` 标签的报错
- 90% 是 JS 语法错误或未定义变量；剩下是 sys.* 用法错

---

## 12. 看现有 app 学

平台自带 7 个 app，按复杂度从低到高：

| App | 难度 | 看它学什么 |
|---|---|---|
| `hello_pkg` | ⭐ | h() 基础 + 局部更新 |
| `pomodoro_pkg` | ⭐⭐ | setInterval + 持久化 + Router 多级页 |
| `notif_pkg` | ⭐⭐ | BLE 接收 + 列表渲染 + saveState |
| `settings_pkg` | ⭐⭐ | Router 完整流程（push/pop/popTo/replace） |
| `tictactoe_pkg` | ⭐⭐⭐ | BLE 双向 + 状态机 + PC AI 插件 |
| `gomoku_pkg` | ⭐⭐⭐⭐ | BLE 联机 + 心跳 + 完整 PC GUI 插件 |
| `doodle_pkg` | ⭐⭐⭐⭐ | sys.canvas 像素绘图 + 文件存取 |

**推荐学习顺序**：hello → pomodoro → notif → tictactoe。前 4 个吃透，造任何 app 都不慌。

---

## 13. 还有问题？

- API 不知道怎么用 → 看 `dynamic_app/scripts/prelude.js` 头部注释 + 现有 app 实现
- 平台缺能力 → 大部分情况下你**不需要新能力**；如果真要加，先做 1-2 个 app 验证现有 API 不够
- 想要 hot reload / setTimeout / WiFi —— 看仓库 `docs/` 里近期工作日志的"后续不做"章节，有取舍记录

**这个平台已经做到「9 行能跑、加新 app 不改任何基础设施」**。剩下的就靠你写真业务了。

祝玩得开心 🎮
