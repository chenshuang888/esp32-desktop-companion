# 动态 App 标准库 prelude 抽取 + Bug 修复 + 文档更新工作日志

日期：2026-04-27

## 背景

上一篇日志（动态 app 平台化：Calc / Timer / 2048 / 渲染瞬切 / BLE 透传 / 双端 SDK / Weather / Music）落地后，已经有 7 个动态 app + 双端 SDK + 三份开发文档，PC 端的 SDK 已经做到"开发者只关心业务注册 / 发送 / 接收"。但 JS 端没做到 —— 每个 app 文件顶部还要拷 200~300 行 VDOM + makeBle 框架代码，开发者上手第一眼面对的是基础设施而不是业务。

这一轮做三件事：

1. **修两个 BLE app 的崩溃**：weather / music 进入即黑屏 + 按钮无响应
2. **抽出标准库 prelude.js**：把 VDOM / makeBle 收进框架，业务脚本零样板
3. **同步更新三份开发文档**：让叙事跟新形态对齐

总改动：
- 新增固件文件 1 个：`dynamic_app/scripts/prelude.js`（348 行）
- 改固件 4 个文件：`dynamic_app_runtime.c` / `dynamic_app_registry.{h,c}` / `CMakeLists.txt`
- 改 7 个 app 脚本（删冗余 + 重写 echo/weather/music 业务）
- 更新 3 份文档：JS API 速查 / 开发指南 / 双端协议

业务总行：3431 → 1940（减 1491 行），prelude 占 348 行，**净精简 1143 行**。

---

## 1. Bug 修复 1：catch 同名标识符 → 整脚本 parse 失败

### 1.1 现象

烧录后进 weather / music 都是黑屏 + 800ms 兜底超时强切，但 echo 正常。串口日志：

```
SyntaxError: catch variable already exists
    at weather.js:101:45
E (...) dynamic_app: eval app failed: weather
W (...) page_dynamic_app: prepare timeout (800ms) for app: weather, force commit
```

### 1.2 根因

esp-mquickjs 是基于早期 QuickJS 精简的，**catch 绑定的标识符被提升到外层函数作用域**。weather.js / music.js 的 `makeBle` onRecv 回调里有三个 `catch (e)` 共用同一个名字：

```js
sys.ble.onRecv(function (raw) {
    try { msg = JSON.parse(raw); } catch (e) { ... }   // ← 1
    ...
    try { anyHandler(msg); } catch (e) { ... }          // ← 2 → SyntaxError
    try { typeRoutes[...](msg); } catch (e) { ... }     // ← 3
});
```

V8 / SpiderMonkey 把 catch 变量当**块级**作用域，PC 上跑都没问题；esp-mquickjs 把它提升到**函数级**，第二个同名 `e` 就被判定为重复声明 → 整个文件 parse 失败 → 一行业务都没跑 → 屏幕空白。

echo.js 没踩到是因为它只有一个 catch。

### 1.3 修法

最小改动：把同函数内多个 catch 的 `e` 改成不同名字（`eParse` / `eAny` / `eType`），或者写 `catch (_)` 显式表达"不在乎"。

### 1.4 经验

这条值得记进"常见坑"。**任何"按 spec 应该没问题，但在精简引擎上炸"的写法都要假设引擎会按最严格语义解释**，宁可显式不要省略。

---

## 2. Bug 修复 2：weather / music 按钮全部不响应

修完 §1 之后 weather / music 能进去了，但屏幕显示正常、按钮按了完全没反应。echo 也是同样精简 VDOM，但 echo 没按钮所以没人发现。

### 2.1 根因

事件链路是这样：

```
LVGL 物理事件
  → C 侧 root listener 入队 {type, dx, dy, node_id}
  → script_task drain → 调 sys.__setDispatcher 注册的 JS 函数 dispatcher(node_id, type, dx, dy)
  → dispatcher 沿 vnode 的 _parent 链冒泡找 onClick
```

完整版 VDOM（alarm / calc / timer / 2048）有：
1. mount 时 `child._parent = node` 建立父子链
2. 一个 `dispatch(id, type, dx, dy)` 函数沿 _parent 冒泡
3. 末尾 `sys.__setDispatcher(VDOM.dispatch)` 把分发函数交给 C

精简版 VDOM（echo / weather / music）从 echo.js 拷的，**三件全没有**。所以即使 onClick 写在 props 里，C 侧根本没人接事件。

### 2.2 临时修法

给 weather / music 的 VDOM 各加 30 行（`_parent` 设置 + dispatch 函数 + setDispatcher 调用），按钮立刻能用。

### 2.3 这暴露了真正的问题

精简版和完整版 VDOM 的差异**对开发者完全不透明**。任何人新写一个带按钮的 app，都可能踩这个坑。这不是"改代码就能根除"的 bug，**是架构上没把基础设施收进框架**。所以接着做 §3。

---

## 3. 抽取标准库 prelude.js

### 3.1 思路：给 JS 一个标准库

类比一下：
- Python 的 builtins（print / len 不用 import）
- Node 的全局 console / process
- 浏览器的 window / document
- Arduino 的 setup() / loop() + Serial 全局可用

我们要做的是：runtime 在 eval 业务脚本**之前**，先 eval 一份固件嵌入的 prelude.js，里面定义好 VDOM 和 makeBle。业务脚本因此能直接用 `var ble = makeBle("myapp")`，不用拷一行框架代码。

### 3.2 跟用户讨论时的关键认知矫正

用户提了个很准的比喻："这种方案我的感觉就是引出来一个类似于 Python 库的 js 文件吧，开发者写业务 js 的时候直接 include 的那种思路"。

我的回答是：本质对，**但比 include 更轻** —— 不需要写 import 语句，prelude 就是天然全局。这是给嵌入式开发者最舒服的形态：他们不喜欢仪式感的 import / require。

### 3.3 抽取边界（关键决策）

阅读所有 7 个 app 后，把模块归三类：

**进 prelude（共用刚需）**：
| 模块 | 取自 | 大小 | 理由 |
|---|---|---|---|
| 完整 VDOM | calc.js + alarm.js 的 borderBottom + find | ~280 行 | 7 个 app 都在用，逻辑稳定 |
| `var h = VDOM.h` 短名 | 通用 | 1 行 | 所有 app 都写 |
| `sys.__setDispatcher(...)` 自动调 | 通用 | 1 行 | 否则按钮失效 |
| `makeBle(appName)` | weather / music 的版本 | ~50 行 | 带 ping 自动 pong / onAny / onError |

**不进 prelude（业务自己掌控）**：
- 配色常量（每个 app 风格不同）
- 持久化模板（schema 因 app 而异）
- 时间格式化（5 行小函数，强抽反而别扭）

**不做的事（避免范围蔓延）**：
- 不在 prelude 里 `delete sys.ble`（裸 API 暂不强制隐藏）
- 不引入 ES6 import/require
- 不做 onMount / onUnmount 生命周期 hook —— app 末尾直接调即可

### 3.4 实现要点

**eval 顺序**（`dynamic_app_runtime.c::dynamic_app_runtime_eval_app`）：

```c
JS_Eval(ctx, prelude_buf, prelude_len, "prelude.js", 0);  // Step 1
JS_Eval(ctx, app_buf, app_len, "myapp.js", 0);            // Step 2
```

prelude eval 失败立即 return ESP_FAIL + dump 异常（保护机制：prelude 烂了所有 app 都跑不了）。

**registry 加 getter**（`dynamic_app_registry.c`）：

```c
extern const uint8_t prelude_js_start[] asm("_binary_prelude_js_start");
extern const uint8_t prelude_js_end[]   asm("_binary_prelude_js_end");

void dynamic_app_registry_get_prelude(const uint8_t **buf, size_t *len) {
    *buf = prelude_js_start;
    *len = (size_t)(prelude_js_end - prelude_js_start);
}
```

**CMakeLists EMBED 加一行** `"scripts/prelude.js"`。

**ESP-IDF 嵌入符号**：注意文件名不要以下划线开头（`_prelude.js`），否则生成的符号是 `_binary__prelude_js_start`（双下划线）—— 一开始这么写过，怕某些工具链版本有问题，统一改成 `prelude.js`。

### 3.5 跨 app 隔离

QuickJS 的 ctx 是 **per-app 重建的**（teardown → setup），所以 prelude 的全局变量天然不跨 app 污染。每次进新 app 都是干净的环境 + 重新 eval prelude。

### 3.6 eval 顺序的安全性

业务脚本里如果开发者写了 `var VDOM = ...`，会**覆盖**prelude 注入的 VDOM —— ES5 `var` 重复声明合法（不报错）。这是有意保留的兼容性：将来如果某 app 想用魔改版 VDOM，不需要改 prelude，自己在文件顶部覆盖即可。

---

## 4. 7 个 app 的批量精简

### 4.1 echo / weather / music：完全重写

这三个用的是精简版 VDOM（没有 dispatch / diff），重写时改成 prelude 提供的完整 VDOM API：
- `VDOM.setText(id, text)` → `VDOM.set(id, { text })`
- `VDOM.setFg(id, color)` → `VDOM.set(id, { fg: color })`
- `VDOM.setSize(id, w, h)` → `VDOM.set(id, { size: [w, h] })`

`VDOM.set` 是局部 patch 接口，比独立的 setText/setFg/setBg/setSize 更通用 —— 多个属性可一次传。

### 4.2 alarm / calc / timer / game2048：批量删

这四个用的是完整 VDOM（跟 prelude 同源），业务部分一行不动，只需删除文件顶部的 §1 VDOM 段（22-318 行那一坨）+ 末尾的 `sys.__setDispatcher(...)` + `var h = VDOM.h;`。

用 Python 一次性处理：找到 `sys.__setDispatcher(...)` 这一行作为 marker，删除从文件开头到 marker 结束的所有内容，替换成两行简短头部注释。

### 4.3 行数对比

| 文件 | 改前 | 改后 | 净减 |
|---|---|---|---|
| alarm.js | 721 | 403 | -318 |
| calc.js | 539 | 269 | -270 |
| timer.js | 674 | 463 | -211 |
| game2048.js | 511 | 307 | -204 |
| echo.js | 241 | 104 | -137 |
| weather.js | 362 | 187 | -175 |
| music.js | 383 | 207 | -176 |
| **小计** | **3431** | **1940** | **-1491** |
| prelude.js（新增） | - | 348 | +348 |
| **净精简** | | | **-1143** |

### 4.4 验证

烧录后逐个测：7 个 app 全部正常 —— prelude 注入链路通了，所有按钮事件冒泡正常，BLE 收发不变。第一个用户测试反馈 "测试没有问题"。

---

## 5. 新形态下的开发者体验

### 5.1 写新 app 的最小模板（24 行）

```js
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
    VDOM.set('hi', { text: 'Got reply' });
});
```

跟改造前的 200+ 行模板相比，开发者真的只剩业务。

### 5.2 唯一保留的"仪式"

`var ble = makeBle("myapp")` 这一行没有自动注入。原因：prelude 不知道 appName。这是**有意保留**的显式仪式 —— 让开发者明确知道自己在跟哪个 namespace 通信。

`sys.ui.attachRootListener('root')` 这一行也没自动调。原因：VDOM 不知道哪个是"根 panel"，业务可能想 mount 多棵树。这是 LVGL root delegation 机制的暴露面，暂时保留。

---

## 6. 文档同步

三份文档全面更新，对齐新叙事：

### 6.1 `动态app_JS_API速查.md`（重写约 60%）

- 开头说明 prelude 自动注入
- 新增 §1 VDOM 完整 API（h / mount / render / set / find / destroy + 事件系统）
- §5 改 `makeBle` 为推荐入口，`sys.ble.*` 标注"不直接用"
- 常见坑加上"catch 同名标识符"
- 最小模板从 30 行缩到 24 行

### 6.2 `动态app开发指南.md`（重写约 50%）

- 新增 §1 "关键认知：你只写业务"，强调 prelude 自动注入
- 架构图加上 prelude 块
- §4 Step 1 改为直接给完整业务（不再有"拷模板"步骤）
- 新增 §6.6（VDOM.set vs render 的选择）
- 新增 §7.5（乐观更新范式）
- 性能参考表改成 VDOM 系列时延
- FAQ 加"能改 prelude 吗"一条

### 6.3 `动态app双端通信协议.md`（小改）

只在 §8 开发流程速记里加一句"`makeBle` 由 prelude 自动注入，无需拷代码"。协议本身完全不变。

---

## 7. 一个跟用户的对话沉淀

用户中途问了一个特别准的问题：

> 像 `dynamic_app_drain_ui_events_once` / `dynamic_app_drain_ble_inbox_once` 这种函数内部都是怎么实现的啊，反正这部分都是单线程跑在 js 线程里面的，不会就是去指定的队列里面取消息然后解析，然后对着 js 应用层注册的回调函数的那个表进行分发吗？

回答的核心：**完全对，但分发表不在 C 侧**。C 侧每个 drain 只调一个固定 JS 入口函数（`dispatcher` 或 `onRecv`），分发完全在 JS 那 50 行 helper 里。

```
PC 推消息字节
  → C 把字节塞进 inbox 队列
  → script_task drain 拿出来
  → 调 ble_recv_cb（唯一入口，prelude 的 makeBle 注册的那个 function）
  → helper 内 JSON.parse + 看 to/type
  → 查 typeRoutes 表
  → 调业务的 ble.on("data", fn) 注册的 fn
```

UI 路径多了一步"`lv_obj_t*` 反查 id 字符串"（用 dynamic_app_ui_registry 哈希表），其它跟 BLE 完全对称。

**这是嵌入式跟解释器搭桥最经典的模式**：高优 ISR/任务里只入队 + 立刻 return；低优解释器任务在 tick 里 drain 队列调一个固定的 JS 入口；分发表在解释器那边自己组织。跟 micropython / lua 的事件桥接套路一模一样。

---

## 8. 改动文件清单

| 文件 | 类型 | 说明 |
|---|---|---|
| `dynamic_app/scripts/prelude.js` | 新增 | 标准库：完整 VDOM + makeBle + 自动 setDispatcher |
| `dynamic_app/CMakeLists.txt` | 改 | EMBED_TXTFILES 加 prelude.js |
| `dynamic_app/dynamic_app_registry.h` | 改 | 加 `dynamic_app_registry_get_prelude` 声明 |
| `dynamic_app/dynamic_app_registry.c` | 改 | extern prelude 符号 + 实现 getter |
| `dynamic_app/dynamic_app_runtime.c` | 改 | eval app 前先 eval prelude，失败立即 return |
| `dynamic_app/scripts/echo.js` | 重写 | 241 → 104 行 |
| `dynamic_app/scripts/weather.js` | 重写 | 362 → 187 行；先修 catch；再补 dispatch；最后整体重写 |
| `dynamic_app/scripts/music.js` | 重写 | 383 → 207 行；同上 |
| `dynamic_app/scripts/alarm.js` | 删 | 721 → 403 行（删 §1 VDOM 段） |
| `dynamic_app/scripts/calc.js` | 删 | 539 → 269 行 |
| `dynamic_app/scripts/timer.js` | 删 | 674 → 463 行 |
| `dynamic_app/scripts/game2048.js` | 删 | 511 → 307 行 |
| `docs/动态app_JS_API速查.md` | 改 | 大改写 ~60% |
| `docs/动态app开发指南.md` | 改 | 大改写 ~50% |
| `docs/动态app双端通信协议.md` | 改 | 小改：开发流程速记加一句 |

---

## 9. 经验沉淀

### 9.1 "只在严苛引擎上炸"的写法不要省略

`catch (e)` ... `catch (e)` 在所有现代 JS 引擎上都没问题，但 esp-mquickjs 炸。**任何"按 spec 应该没问题但在精简引擎上炸"的写法都要假设引擎按最严格语义解释**，宁可显式不要省略。这条对 micropython / lua / 其它嵌入式解释器都成立。

### 9.2 "拷模板"是基础设施没收进框架的信号

精简版和完整版 VDOM 共存了好几个 app 才暴露 bug。**只要某段代码"每个 app 都得拷"，就是框架欠的债**。短期没事，规模一大必踩坑。这次抽 prelude 不是为了"代码漂亮"，是为了根除"开发者拷漏一段就崩"的可能。

### 9.3 给业务一个"第一眼看到的就是业务"的环境

PC 端 SDK 早就做到了开发者只写业务。JS 端这次才补齐。**这种对称很重要 —— 双端体验不一致会让开发者觉得 ESP 端"难"**。改造完之后两端都是 30 行起步，叙事终于一致。

### 9.4 prelude 不是 import，是 builtins

跟用户讨论时澄清了这点。Python 的 `import` 是有仪式感的，prelude 完全没有 —— 业务脚本就像"天生有 VDOM 这个东西"。这种"零仪式"对嵌入式开发者最友好，他们不喜欢"还要先 import 啥"的感觉。Arduino 的 `Serial.println` 就是这种感觉，很多人甚至不知道它是个对象。

### 9.5 渐进式重构 + 最小破坏

抽 prelude 时，先让业务里继续 `var VDOM = ...` 也能跑（var 重复声明合法 → 后面的覆盖前面的）。这意味着可以"先注入 prelude 不删任何 app"先验证链路通，再批量删 app 里的冗余。这种**两步走的重构对大改特别重要** —— 第一步出问题立刻能定位是 prelude 链路问题，第二步出问题立刻能定位是某个 app 的业务残留。一次到位很容易踩到难定位的混合 bug。

---

## 10. 接下来

候选（按"做了立刻能感觉到"排）：

1. **从 NVS 加载 JS 脚本**（~30 行 C，重写 registry.c） —— 真正的"动态下载 app"，开发者不用编固件就能换 app
2. **真实时间 sys.time.now()**（~30 行 C） —— 包一层 localtime，让 alarm/timer 跟真实墙钟挂钩
3. **图标 / 图片控件**（~30 行 C） —— `lv_image_create` 暴露给 JS，weather 能放天气图标
4. **app 模板生成器**（~50 行 Python） —— `python tools/new_app.py myapp` 自动建 .js + provider.py + 菜单注册的脚手架

平台体验已经基本完整了。短期建议做 1+2 一组（60 行 C，1 小时） —— 把"动态"做实 + 给真实时间这个高频需求开口子。
