# 动态 App 平台化 —— Calc 优化 / Timer / 2048 / 渲染瞬切 / BLE 透传 / 双端 SDK / Weather / Music

日期：2026-04-26

## 背景

上一篇日志（Diff/持久化/手势打磨/多 app 共存）落地后，框架已经有 alarm + calc 两个 app、能 NVS 持久化、手势体验顺畅。但还有几个关键能力缺位：

1. **calc 体验差** —— 布局散、按下无反馈、display 看不到上下文
2. **app 太少** —— 框架到底通用还是只能跑那两种？
3. **打开有"组装感"** —— 进 calc 能看到组件一个个冒出来
4. **完全没有 BLE 能力** —— 闹钟/计算器是纯本地的，框架到底能不能做需要后端的 app？
5. **没法让别人开发** —— 双端协议没规范、PC 没 SDK、文档欠缺

按"打磨 → 加 app → 体验质变 → 通信能力 → 平台化"的顺序做了七轮：

1. **calc 重写** —— 双行 display + 按下高亮 + 参数化布局
2. **Timer / 2048** —— 验证 setInterval 高频 + 手势方向 + NVS 跨 app 隔离
3. **渲染瞬切** —— LVGL off-screen build + commit_prepared 路径，进 app 零中间态
4. **BLE 透传管道** —— 1 个通用 GATT service，C 完全透明，JS 拿到字符串
5. **JSON 路由协议** —— `to`/`from`/`type`/`body` 四字段约定，echo app 验证
6. **PC 端 dynapp_sdk** —— 装饰器风格 handler 注册、自动重连、统一日志
7. **Weather / Music 双端** —— 真正复杂的 app，覆盖 Pull / Push / 命令上行三种范式

外加一个隐性问题在中途暴露：每加几个 native 就要在两个文件里手动同步索引上限，已经踩了一次堆崩溃，统一收成一个宏。

总改动：
- 新增 5 个 dynamic app（calc 重写 + timer + 2048 + echo + weather + music）
- 新增 1 个固件 service（dynapp_bridge_service，~200 行 C）
- 新增 1 个 PC SDK + 2 个 provider（~600 行 Python）
- 新增 3 份开发文档（~700 行 markdown）
- 框架核心：page_router 加 commit_prepared 路径、dynamic_app_ui 加 ready_cb、内嵌 ble_recv_cb GCRef

---

## 1) calc 重写：从"能用"到"好用"

### 1.1 老版本的问题

烧录看了一会发现四个槽点：

1. **display 只有一行** —— 算到一半切运算符，看不到上下文（"12 × 3" 算到 36 时，前面的 12 × 没了）
2. **按下没反馈** —— 数字键按下死气沉沉，只有运算符的"已选中态"会亮
3. **布局参数硬编码** —— rowY0/Y1/Y2... 一行行手算 Y 坐标，5 行按钮 + display 算总高 314px，留白不对称
4. **配色沉闷** —— 数字 / 功能 / 运算符全是深紫色系无层次

### 1.2 改造

**双行 display**：上方小灰字显示 expr（"12 × 3 ="），下方大字当前 display。算到一半切运算符 / 按 = 都能看到完整算式。

**按下高亮（旁路）**：onPress 直接 `sys.ui.setStyle(BG_COLOR, hi)` 不走 rerender；onClick 触发 rerender 时 view() 里的 base 色被 diff 自动覆盖回去 == 自动复位；onRelease 兜底"按下后划走没触发 click"的复位。完全符合 alarm.js 沉淀的"旁路修改的字段必须能从 state 反算"规则。

**布局参数化**：DISP_H / BTN_H / GAP / SIDE_PAD / N_COLS 顶头列出来，每行 Y 用累加，BTN_W 由 SIDE_PAD + GAP 反算。校验过总高 70+5+5*49=320px 严丝合缝。

**配色重排**：数字键浅一档紫（COL_NUM 0x4A4368），功能键深一档（COL_FN 0x3A3454），运算符橙（0xF59E0B），等号绿（0x10B981）。视觉层次清晰。

### 1.3 一个布局 bug

第一次改完总高 332px > 320px，下面 5 行被裁掉一半。改 `BTN_H 44 → 44, GAP 6→5, DISP_H 76→70` 后正好 320。教训：**写参数化布局之前先列等式校验**，不要写完才想起来。

---

## 2) Timer / 2048：验证三个新维度

加这两个 app 是想测：

| App | 验证维度 |
|---|---|
| Timer | 高频 setInterval（30ms 跑秒表） + 双模式状态机 + NVS 持久化 |
| 2048 | onDrag 方向识别 + 4×4 网格布局 + NVS 持久化最高分 |

### 2.1 Timer 关键设计

**双 ticker**：秒表 30ms 刷新，倒计时 100ms 足够（1 秒精度）。两个 setInterval 分别开，模式切换时不真停 ticker（只判 mode 直接 return），避免反复 alloc/free。

**避免高频整树 rerender**：秒表 30ms 一次只 `sys.ui.setText("swTime", fmtSwTime(swCurrent()))`，不调 rerender 整树。否则 26 个按钮全跑 diffProps，CPU 浪费。按钮文案/选中态变化才 rerender。

**跨重启避坑**：loadState 时强制把 `running = true` 退化为 false。否则启动时旧的 startMs/endMs 是过时 uptimeMs，算出来的 elapsedMs 会暴涨成几小时。

**到点闪烁**：倒计时归零后另开一个 500ms ticker 闪 6 次（3 秒）后自动停，用 `sys.time.uptimeMs() - doneAt > 3000` 判断停时机。完全 JS 自管理。

### 2.2 2048 关键设计

**onDrag 用 dx/dy 大小判方向**：

```js
if (Math.abs(dx) > Math.abs(dy)) dir = dx > 0 ? 'R' : 'L';
else                              dir = dy > 0 ? 'D' : 'U';
```

C 侧 `DRAG_THRESHOLD_PX` 累计触发回调，JS 拿到的 dx/dy 是阈值跨过那一刻的绝对值，方向语义清晰。

**合并算法**：单行向左 compress（去 0 + 相邻相同合并），转向通过 reverseRow / 转置实现，4 方向共用一个 compressRow。

**经典配色**：完全照搬 2048 官方配色（2 浅米色到 2048 金色），8 行 dict 搞定。

### 2.3 NVS 隔离验证

三个新 app 各自 saveState 到 `dynapp/<app名>`：
- timer 存 `{mode, sw:{...}, cd:{...}}`
- 2048 存 `{board, score, best, ...}`
- alarm 已存 `{alarms, nextSeq}`

切换 10+ 次互不影响，每个 app 看到的 state 都是自己上次的。隔离机制（`current_app_key()`）按设计工作。

---

## 3) 渲染瞬切：从"看到组装"到"瞬间切换"

### 3.1 痛点的根因

体感上"打开动态 app 慢"。计时下来其实只 80-200ms，但**用户能看到组件一个个冒出来**：

```
T+0      router 切屏 → active_screen = 新空白屏      ← 用户看到空白
T+5      script 任务 eval calc.js
T+50     脚本 rerender 灌 60+ 条 cmd 进队列
T+60-90  UI 任务 10ms tick 一批批 drain，LVGL 边渲染边出现
                                                      ↑ 用户看到组件陆续出现
T+90     完成
```

**根本问题不是慢**，是**分多帧渲染让用户看到中间态**。原生页 build 是单线程同步的，build 完才下一次 LVGL 渲染，所以看不到中间。

### 3.2 解决思路

**LVGL 的关键事实**：active_screen 之外的 screen 不参与渲染，只占内存。所以可以：

```
T+0      用户点 calc → menu 不动
         后台建 off-screen 对象树 + 启脚本 build
T+0~80   脚本灌 cmd → drain 在 off-screen 上构建对象，LVGL 还在画 menu，用户毫无感觉
T+80     脚本调 attachRootListener → 触发 ready_cb → page_router_commit_prepared
T+91     LVGL 下一帧：active_screen 突然指向已建好的树 → 完整 calc 出现
```

类比：客人在客厅等，工人在隔壁房间装修，装好打开门请进。

### 3.3 实现要点

**page_router 加新路径** `commit_prepared(id, screen)`：销毁旧页 + lv_scr_load 已建好的 screen + 更新 current_page。**不调 callbacks->create**。原 switch 路径完全不动，所有原生页零改动。

**page_dynamic_app 拆 prepare/commit/cancel**：
- `prepare_and_switch(name)`：建 off-screen + set_root + 注册 ready_cb + 起 800ms timeout timer + start script，立即返回不切屏
- ready_cb：删 timer + commit_prepared
- timeout cb：脚本卡住兜底，到 800ms 强切（宁可 build 一半也别卡 menu）
- `cancel_prepare_if_any`：用户连点切别的 app 时撤销

**dynamic_app_ui 加 ready_cb 一次性回调**：drain 处理完 ATTACH_ROOT_LISTENER 后触发并清空。利用了"脚本统一在末尾调 attachRootListener"这个事实约定。

**状态机**：IDLE / PREPARING / COMMITTED 三态。menu 上其它 click 入口前都加 `cancel_prepare_if_any()` 兜底，避免 prepare 中切走留下野脚本。

### 3.4 关键安全点

- **off-screen 建好不 lv_scr_load**：LVGL 完全不 render，性能零影响
- **commit_prepared 失败回滚**：停脚本 + 清 registry + 删 off-screen，状态归 IDLE
- **ready_cb 一次性**：取出后立即清空，避免重复 commit 一棵已 active 的 screen
- **inbox 在 app 切换时清**：alarm 没消费完的 PC 消息不会漏给 calc

烧录测：进任意动态 app 都是 menu → 一帧 → 完整 app，零中间态。**体感从"卡顿加载"变"瞬间切换"**。

### 3.5 一个有意思的认知矫正

跟用户讨论时本来想加"hide 元素 + 黑屏 200ms"那种 iOS 启动图风格，用户反问"为啥不直接两棵树并存？" —— 这一下点醒了。**LVGL 本来就支持，只是平时大家用 lv_scr_load 都是立刻切，没意识到这个能力**。最终方案比我提的简洁很多。

---

## 4) BLE 透传管道：dynapp_bridge_service

### 4.1 设计取舍

讨论了三种方案：

| 方案 | 复杂度 | 缺点 |
|---|---|---|
| A：JS 自定义 UUID 映射到通用 GATT 容器 | 极高 | NimBLE 不支持运行时增删 service，PC 端要协商 char 占用 |
| B：固定 service + char + channel 概念 | 中 | C 多解析一个 channel 字段，PC 端协议有一层多余 |
| **C：纯透传管道，C 完全不解析** | 低 | C 只搬字节，所有路由让 JS 自己用 JSON 做 |

选 C。最薄、最自由、未来零成本扩展。

### 4.2 GATT 表

```
Service:  a3a30001-0000-4aef-b87e-4fa1e0c7e0f6
  ├─ rx char: a3a30002-...   WRITE         (PC → ESP)
  └─ tx char: a3a30003-...   READ + NOTIFY (ESP → PC)
```

UUID 用 `a3a3` 段跟原有 `8a5c` 段（5 个原生 service）做视觉区分。开机注册一次永远不变。

### 4.3 跨线程数据流

```
PC → ESP:
  NimBLE host task (高优先级)
    → access_cb 同步拷贝 mbuf 到 dynapp_bridge_msg_t（200B 上限）
    → xQueueSend(inbox, 0 timeout)        // 不能阻塞 host
    → 满则丢最老（PC 主动写无 backpressure，老消息没价值）

  Script task (低优先级，每 tick)
    → dynapp_bridge_pop_inbox 出队
    → 调 sys.ble.onRecv 注册的 JS 回调

ESP → PC:
  Script task 调 sys.ble.send
    → ble_gatts_notify_custom（NimBLE 自带锁，跨线程安全）
    → 直接异步发出，无需 outbox 队列
```

**inbox 满策略**：丢最老不丢新。BLE write-without-response 不反压，PC 一直灌，丢老消息至少保证 app 收到的是近期的。

**outbox 没做队列**：因为 NimBLE notify 接口本身线程安全 + 内部有 mbuf 限速，script task 直接调即可。一切从简。

### 4.4 JS API 三件套

```js
sys.ble.send(payloadStr)            // bool
sys.ble.onRecv(function(s){})       // 注册唯一收包回调，覆盖式
sys.ble.isConnected()               // bool
```

onRecv 的 JSValue 用 GCRef 钉住，teardown 时释放。**一开始忘了，加 BLE 三个 native 时只在一处加了 idx，没改 cfunc_table 的 extra=16**，结果越界写堆元数据，下次 heap_caps_malloc 触发 TLSF assert。修复后顺手把这个常量收成宏 `DYNAMIC_APP_EXTRA_NATIVE_COUNT`，加在 internal.h 跟 func_idx_* 字段同文件，以后加 native 只改一处，根除这类"散落两处"的隐患。

### 4.5 echo.js 验证

最简单 echo app：收 PC 来啥就回啥 + " (echo)"。配套 `tools/dynapp_bridge_test.py` 做交互式测试工具。烧录跑通收发链路。

---

## 5) JSON 路由协议（方案 A）

### 5.1 多 app 共用一个管道的归属问题

C 透明转发但 inbox 没带"目标 app"标识 —— 一旦 PC 想给 alarm 发消息但用户切到了 calc，消息就发错对象了。

讨论了 A/B 两种解法：

- **A**：PC 和 JS 约定每条消息带 `to`/`from` 字段做应用层路由
- **B**：app 启动时主动 send 一条 hello 告诉 PC 我上线了

最终只做 A，B 是过度设计。理由：用户主动连 ESP 时本来就知道屏上是啥；hello 也信不过（网络抖动重连还得重发）；A 已经做了归属验证，发错对象的消息 JS 自动丢，没有副作用。

### 5.2 协议固化

```json
PC → ESP   { "to": "weather", "type": "req" }
ESP → PC   { "from": "weather", "type": "data", "body": {...} }
```

四字段：`to` / `from` / `type` / `body`。`to: "*"` 表广播。

**保留 type**：`ping` / `pong`（连通性）、`error`（业务错误，body `{code, msg}`）。

**字段命名约定**：蛇形小写（`temp_c` 不是 `tempC`），时间戳一律 unix 秒字段名 `ts`。

### 5.3 JS helper：拷一份就能用

`makeBle(appName)` 内部独占 `sys.ble.onRecv`，按 `to` 字段过滤、按 `type` 分发，业务侧 `ble.on("data", handler)` / `ble.send("type", body)`。

helper 内置自动应答 ping → pong（不打扰业务）。所有 handler 抛异常都 try/catch + sys.log，绝不向上抛崩 app。

每个 app 拷这 50 行到脚本顶部，**只改一行 `var APP_NAME = "..."`** 就能用。完全符合 alarm/calc/timer 沉淀的"app 自带框架副本"惯例。

---

## 6) PC 端 dynapp_sdk

### 6.1 定位转变

到这一步意识到：**项目已经从"我自用"变成"平台/SDK"**。如果想让别人开发，要交付的不是某个具体 app，而是 **协议 + JS API + PC SDK + 文档**。

### 6.2 SDK 设计

```python
async with DynappClient(device_name="ESP32") as client:
    @client.on("weather", "req")
    async def _(msg):
        await client.send("weather", "data", body={...})

    await client.run_forever()
```

四个核心 API：
- `DynappClient(...)` 构造 + async ctx manager
- `@client.on(from_app, msg_type)` 装饰器注册 handler
- `await client.send(to, type, body)` 发消息
- `await client.run_forever()` 阻塞直到 Ctrl+C

### 6.3 实现要点

- **全 async**，不引线程，跟 bleak 事件循环对齐
- **send 内部 asyncio.Lock**，多 provider 并发写不抢
- **自动重连**：watchdog task 每 3s 检查，断了 backoff 1→2→4→…→30s 重试
- **路由优先级**：`(from, type) > (from, *) > (*, type) > (*, *)`，每条消息只触发最具体的一个 handler
- **handler 抛异常 = ERROR 日志 + traceback**，绝不向上传播
- **payload > 200B → ValueError**（编程错误就该立刻崩，业务能看到）

### 6.4 文件结构

```
tools/
  dynapp_sdk/
    __init__.py
    client.py
    router.py
    constants.py
  providers/
    __init__.py
    weather_provider.py
    media_provider.py
  dynapp_companion.py        # 总入口
  dynapp_bridge_test.py      # 调试工具，无 SDK 依赖
```

不做 pip 安装、不上 PyPI。开发者把 `tools/` 整个拷走就能跑。

---

## 7) Weather 双端

### 7.1 协议

```
ESP → PC   { to: weather, type: req,   body?: {force} }     启动 / 用户刷新
ESP ← PC   { to: weather, type: data,  body: {...} }        完整快照
ESP ← PC   { to: weather, type: error, body: {msg} }        拉取失败
```

`data.body`：temp_c / temp_min / temp_max / humidity / code / city / desc / ts。`code` 用人类可读字符串（`"clear"` / `"rain"` 等），不再用原 binary 的 enum 数字。

### 7.2 PC 端

`weather_provider.py` 用 OpenMeteo（免费、无 key、纯 HTTP GET）。10 分钟缓存防 ESP 反复进 weather app 把 API 打爆。WMO weather code 表完整搬过来转成字符串 + 描述。

`requests.get` 是阻塞的，用 `asyncio.to_thread` 包装避开事件循环阻塞。

### 7.3 JS 端

UI：城市 + 状态条 + 大温度 + min/max + 描述 + 湿度 + 时间戳 + 刷新按钮。

**短按 = 走缓存 / 长按 = 强制刷新**：onClick / onLongPress 两个 hook。

**8 秒超时**：req 发出后用 setInterval 每 500ms 检查 `uptimeMs - startMs > 8000`，超时则状态变 error。

**1.5 秒防连点**：req 之间最小间隔 1.5s，避免 BLE 拥塞。

UI 一次性 mount 后只 setText 改字段，不 rerender 整树。

---

## 8) Music 双端：最复杂的范式验证

### 8.1 三种范式

Music 是验证框架能不能撑得起"实时双向高频通信"的最佳样本：

| 范式 | 例子 | 实现 |
|---|---|---|
| Push | PC 主动推 state | SMTC 事件触发 → 立刻 send |
| Command-Ack | ESP 按按钮 → PC 执行 | onClick → send btn → PC 模拟媒体键 → 略等再推 state |
| 状态插值 | 进度条丝滑前进 | PC 推 base+ts，JS 30Hz 用 uptimeMs 插值 |

### 8.2 PC 端 SMTC

`media_provider.py` 把原 desktop_companion.py 里 SMTC 的代码搬过来（不动原文件），改 binary 推送为 JSON 推送：

- 监听 `media_properties_changed` / `playback_info_changed` / `timeline_properties_changed` 三个 SMTC 事件
- 事件来了 → 拼 JSON state → 去重（4 字段相同 + position 差 < 1s 就不发）→ send
- 收到 ESP 的 `btn` → 模拟 Windows 媒体键 (VK_MEDIA_PREV_TRACK / PLAY_PAUSE / NEXT_TRACK)
- 按键后等 300ms 再 force push 一次 state（让 SMTC 状态更新）

**线程问题**：winsdk 事件回调在 COM 线程，必须 `asyncio.run_coroutine_threadsafe` 桥接回 asyncio loop。

### 8.3 JS 端进度条插值

```js
function currentPos() {
    if (!state.playing) return state.posBase;
    var dt = (sys.time.uptimeMs() - state.lastUpdateMs) / 1000;
    var p = state.posBase + dt;
    if (state.durSec > 0 && p > state.durSec) p = state.durSec;
    return p;
}
```

收到 state 时记 `posBase` + `lastUpdateMs`，30Hz 心跳用 `uptimeMs` 估算当前位置。播放/暂停切换时重置基准。**PC 不用每秒推**，只在状态变化时推一次。

### 8.4 乐观更新

按 ▶/❚❚ 时本地立刻翻转 playing 状态 + 重设基准，**不等 PC 回 state**。然后再 send btn。这样按键反馈是即时的，PC 处理 + push 回来之前用户已经看到状态变了。

### 8.5 UI 难点

240×320 屏要塞下：状态条 / title / artist / 进度条 / 时间 / 三按钮。

进度条用两层 panel：bg 是 `[200, 6]` 的圆角灰条，fg 是同位置但宽度按比例的青色条。每帧 setSize 改 fg 宽度。

三按钮：左右两个小（60×50）按钮 + 中间大（70×56）播放按钮，对应桌面/手机一致的 UI 习惯。

---

## 9) 三份开发文档

为了让别人能开发，写了三份文档：

| 文档 | 篇幅 | 内容 |
|---|---|---|
| `动态app双端通信协议.md` | ~200 行 | GATT / JSON / 路由 / 大小限制 / 保留 type / 协议演进 |
| `动态app_JS_API速查.md` | ~250 行 | 所有 sys.* 签名 + 11 个常见坑 + 最小模板 |
| `动态app开发指南.md` | ~250 行 | 30 分钟手把手 + 调试技巧 + 设计规范 + 进阶范式 + FAQ |

**写文档原则**：不写 BLE/NimBLE/LVGL 内部原理，开发者不需要知道。只讲他们直接用得到的 API + 怎么调试 + 怎么避坑。

---

## 改动文件清单

| 文件 | 阶段 | 类型 | 说明 |
|---|---|---|---|
| `dynamic_app/scripts/calc.js` | 1 | 重写 | 双行 display + 按下高亮 + 参数化布局 + 新配色 |
| `dynamic_app/scripts/timer.js` | 2 | 新增 | 双模式 Timer，30ms 秒表 + 100ms 倒计时 + 闪烁 + 持久化 |
| `dynamic_app/scripts/game2048.js` | 2 | 新增 | 4×4 网格 + onDrag 方向 + 经典配色 + best 持久化 |
| `dynamic_app/scripts/echo.js` | 4/5 | 新增 | 验证 BLE 收发 + 之后升级为带 to/from 路由的 helper 模板 |
| `dynamic_app/scripts/weather.js` | 7 | 新增 | Pull 范式 + 8s 超时 + 长按强刷 + OpenMeteo 数据 |
| `dynamic_app/scripts/music.js` | 8 | 新增 | Push + Command-Ack + 30Hz 进度条插值 + 乐观更新 |
| `framework/page_router.h/c` | 3 | 改 | 新增 `commit_prepared` 路径 + 销毁旧页抽 helper |
| `app/pages/page_dynamic_app.h/c` | 3 | 改 | 状态机 IDLE/PREPARING/COMMITTED + off-screen build + 800ms 兜底 |
| `dynamic_app/dynamic_app_ui.h/c` | 3/4 | 改 | 新增一次性 ready_cb；ATTACH_ROOT_LISTENER 后触发 |
| `app/pages/page_menu.c` | 3/2/7/8 | 改 | 4 个动态 app 入口改 prepare_and_switch；其它入口加 cancel 兜底；6 个新菜单项 |
| `services/dynapp_bridge_service.h/c` | 4 | 新增 | 通用 GATT service + inbox 队列 + send 直调 |
| `services/CMakeLists.txt` | 4 | 改 | 加 dynapp_bridge_service.c |
| `main/main.c` | 4 | 改 | nimble_start 之前调 dynapp_bridge_service_init |
| `dynamic_app/dynamic_app_internal.h` | 4 | 改 | 加 ble_recv_cb GCRef + 3 个 func_idx + DYNAMIC_APP_EXTRA_NATIVE_COUNT 宏 |
| `dynamic_app/dynamic_app_natives.c` | 4 | 改 | sys.ble.{send, onRecv, isConnected} 实现 + drain inbox |
| `dynamic_app/dynamic_app_runtime.c` | 4 | 改 | teardown 时调 dynamic_app_ble_reset；用宏取代硬编码 extra |
| `dynamic_app/dynamic_app.c` | 4 | 改 | 主循环加 drain_ble_inbox_once；start 时清 inbox |
| `dynamic_app/CMakeLists.txt` | 4/2/7/8 | 改 | EMBED 加 5 个新脚本；REQUIRES 加 services |
| `dynamic_app/dynamic_app_registry.c` | 2/4/7/8 | 改 | 注册 timer / 2048 / echo / weather / music |
| `tools/dynapp_sdk/__init__.py` | 6 | 新增 | 包入口 |
| `tools/dynapp_sdk/constants.py` | 6 | 新增 | UUID / MAX_PAYLOAD / 默认设备名 |
| `tools/dynapp_sdk/router.py` | 6 | 新增 | (from, type) 双键路由 + 异常包裹 |
| `tools/dynapp_sdk/client.py` | 6 | 新增 | 扫描/连接/订阅/重连/send/装饰器，~280 行 |
| `tools/providers/__init__.py` | 6 | 新增 | provider 包文档 |
| `tools/providers/weather_provider.py` | 7 | 新增 | OpenMeteo + 10 分钟缓存 + WMO 转字符串 |
| `tools/providers/media_provider.py` | 8 | 新增 | SMTC 监听 + 状态变化 push + btn → 媒体键模拟 |
| `tools/dynapp_companion.py` | 6/7/8 | 新增 | 总入口 + 命令行参数 + 注册两个 provider |
| `tools/dynapp_bridge_test.py` | 4/5 | 新增/改 | 交互式调试工具，支持 --to / --raw / --type / --once |
| `docs/动态app双端通信协议.md` | 9 | 新增 | 协议规范 |
| `docs/动态app_JS_API速查.md` | 9 | 新增 | API 速查 |
| `docs/动态app开发指南.md` | 9 | 新增 | 开发指南 |

---

## 验证

烧录 + `python tools/dynapp_companion.py` 后逐项测：

### Calc / Timer / 2048
- ✅ Calc：双行 display 显示运算上下文，按下任意键高亮，新配色清晰
- ✅ Timer：秒表 30ms 流畅；倒计时 1m/3m/5m 预设、±30s 调整、归零闪烁；模式切换不漏数据；重启后回到暂停态
- ✅ 2048：四方向滑动响应正确；合并算分；best 跨重启保留；game over 提示

### 渲染瞬切
- ✅ 进任意动态 app（alarm / calc / timer / 2048 / echo / weather / music）都是 menu → 一帧 → 完整 app，零中间态
- ✅ 用户连点不同 app（alarm 切 calc 切 2048）正常，无残留
- ✅ prepare 中切别的菜单项（System / Music 等）安全切走，脚本停干净
- ✅ 退出动态 app 走老路径，与原行为一致

### BLE Echo
- ✅ PC 端 `python dynapp_bridge_test.py --to echo` 输入文本，ESP 屏显示 + 回复 " (echo)"
- ✅ `--to alarm` 发消息，echo 不响应（路由验证）
- ✅ `--type ping --once "x"` 收到 pong（helper 自动回）
- ✅ `--raw` 发非 JSON 字节，ESP 状态变 "bad json"

### Weather
- ✅ 启动 1s 内显示当前天气
- ✅ 短按 Refresh 走缓存（10 分钟内）
- ✅ 长按 Refresh 强制重拉
- ✅ 拔网线后超时 8s 显示 "err: timeout"
- ✅ 不连 PC 时显示 "err: no PC"

### Music
- ✅ 在 Windows 上播放任意媒体（网易云 / Spotify / 浏览器视频），进 Music app 立刻显示曲目
- ✅ 进度条 30Hz 平滑前进，无跳跃
- ✅ ▶/❚❚ 按钮控制本机播放器，乐观更新先变状态再被 PC 确认
- ✅ 切歌按钮触发 Windows 媒体键，0.3s 后看到新曲目
- ✅ 没媒体会话时显示 "no music app"

---

## 经验沉淀

### "组装感"的根因不是慢，是分多帧渲染让用户看到中间态

原生页 build 也不快（也得几十 ms），但单线程同步执行 → 在两次 LVGL 渲染缝隙里完成，用户看不到中间。动态 app 走"脚本 → 队列 → UI 任务"跨线程，单 tick 32 条 cmd 限速，60+ 条 cmd 跨 2-3 帧，正好被 LVGL 拍下来给用户看到。

**修法不是优化速度，是让 LVGL "拍不到" 中间态** —— off-screen 上慢慢搭，搭好瞬间切。LVGL 本来就支持"非 active screen 不渲染"，是个白送的能力，只是之前没意识到。

### "加 native 时 cfunc_table 越界"是迟早会踩的坑

加 BLE 三个 native 时改了 `dynamic_app_natives_register` 的索引上限到 +18，但 `dynamic_app_runtime.c` 那边 `extra = 16` 没改 → 越界 3 个 JSCFunctionDef 写到 PSRAM 末尾外，覆盖 TLSF 堆 free-list 元数据 → 下次 heap_caps_malloc 触发 assert。

**根除办法不是"以后小心"**，是把这种"散落两处的常量"收成单一来源 (`DYNAMIC_APP_EXTRA_NATIVE_COUNT` 宏)。这条对任何"两处必须同步"的设计都成立 —— 要么自动推导，要么干脆别允许两处都存在。

### 透传管道 + JSON > 二进制 struct

原生 service 都用 packed struct，PC 端要 `struct.pack`，固件要 `__attribute__((packed))`，加字段两端必须同步改、还得算偏移。

dynapp_bridge 完全透明 + JSON：
- 字段名见名知意
- 加字段不需要协调
- PC / JS 任一端改协议，C 不动
- 多 200B 字节但 BLE 完全够用

唯一代价：JS 解析 JSON 比 unpack struct 慢几十微秒。完全无感。

### 平台化的关键产出不是 app，是协议 + SDK + 文档

到 Weather/Music 这一步意识到：要让别人开发，**你交付的不是"我做了一个天气 app"，而是"看完文档 30 分钟内别人也能做一个"**。

- 协议固定 → 双端独立演化
- SDK 包装 BLE/重连/路由 → 开发者不接触底层
- 文档 30 min 上手 + 11 个常见坑 → 减少回答问题的次数

这条对任何想做"平台"而不是"应用"的项目都成立。

### Push + 插值 > 高频 Push

Music 进度条用 PC 推 base + ts，JS 30Hz 自插值。PC 完全不用每秒推 state。**用客户端的本地时间补全服务端的离散更新**，省 BLE 带宽 + 进度条更丝滑。这条对任何"远端给数据点 + 本地连续显示"的场景都成立（股价 / 心率 / 计步等）。

### 乐观更新比"等服务器回"用户体验好得多

Music 按按钮时本地先翻转状态再发 BLE 命令。从 PC 收到回包是 ~300ms，用户感知不到延迟。**只要本地状态机能 100% 预测服务器结果**，就该乐观更新。这是所有现代 UI 框架（React / SwiftUI 等）都在用的常识，但容易在嵌入式 app 里被忘掉。

---

## 接下来

候选（按"做了立刻能感觉到"排）：

1. **从 NVS 加载 JS 脚本**（~30 行 C，重写 registry.c） —— 真"动态下载 app"的第一步，让开发者不用编固件就能换 app
2. **真实时间 sys.time.now()**（~30 行 C） —— 包一层 localtime，让 alarm/timer 能跟真实墙钟挂钩
3. **图标 / 图片控件**（~30 行 C） —— `lv_image_create` 暴露给 JS，weather 就能放天气图标
4. **app 模板生成器**（~50 行 Python） —— `python tools/new_app.py myapp` 自动建 .js + provider.py + 菜单注册的脚手架
5. **再做 1-2 个 app 跑通模板**（任意） —— 笔记 / 通讯录 / 番茄钟，验证模板易用性

短期我倾向 **1 + 2 一组**（60 行 C，1 小时） —— 把"动态"做实 + 给真实时间这个高频需求开口子。**4 + 5 一组**作为"平台体验"打磨，等真有第三方开发者再做。
