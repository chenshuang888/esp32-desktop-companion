# 动态 App（MicroQuickJS）—— Diff/持久化/手势打磨/SHADOW+LONG_PRESS/多 App 共存

日期：2026-04-26

## 背景

上一篇日志（手势框架 + destroy API + 闹钟页）落地后，框架在"输入"和"销毁"两侧已经完整。但还有几个能感受到的痛点：

1. **声明式只用了一半** —— `VDOM.mount` 描述初始 UI 是声明式，但之后 `VDOM.set` 改组件全是命令式。读者看 mount 一段以为是 React，看 set 又得切回命令式心智，这种半成品反而比纯命令式还累
2. **app 退出数据全丢** —— 闹钟开关、新增、删除全是内存里的，没法持久化
3. **手势体验粗糙** —— 长按只有按在开关上才生效，滑动也是；编辑页误触；列表和标题一起滚
4. **没第二个 app** —— 框架到底通用还是只能跑闹钟？没验证

外加一个架构层面的问题：以后真要"动态下载 app"，现在脚本硬编码 `app.js` 一根筋的设计就完蛋了。

按"基础能力 → 体验打磨 → 架构升级"的顺序做了五轮：

1. **L2 diff** —— 让"声明式"完整起来，业务从 `VDOM.set` 改成 `state + view + rerender`
2. **持久化（sys.app.saveState/loadState）** —— NVS blob + JSON 同步存取
3. **SHADOW + GAP + LONG_PRESS** —— 13 行 C 三件套，视觉/交互一次到位
4. **手势打磨** —— PRESS_LOCK + SCROLLABLE + suppressClick，把四个体验问题一次修了
5. **多 app 共存** —— 拆出 `dynamic_app_registry`，加宿主页 set_pending，菜单分两项 Alarm/Calculator，新建 calc.js

## 1) L2 diff：把声明式做完整

### 1.1 动机

之前的 `VDOM.set(id, patch)` 是精确打击 —— 业务自己算"开关变化要刷哪 3 个字段"。它的 bug 不是性能差，是**心智模型分裂**。

加了 diff 之后业务可以写：

```js
function view() {
    return h('panel', { id: 'appRoot' }, [
        ...state.alarms.map(viewAlarmRow),
        h('label', { id: 'statusLine', text: statusOf(state) })
    ]);
}
function onSwitchClick(seq) {
    state.alarms[i].on = !on;
    rerender();   // 一行
}
```

业务只描述"现在状态对应啥样"，diff 自己算出该改什么。

### 1.2 算法选择

L1（同位置 props diff）/ L2（+ 同层增删）/ L3（+ keyed reorder）三档里选了 **L2**。理由：

- 闹钟页加卡片都是 append 到尾、删卡片按 id 删，**没有"中间插入/位置交换"** —— L3 的 LIS 算法在这没场景
- 240×320 屏 + 8 槽 event queue 撑不起 keyed reorder 的批量 cmd（实际上 cmd queue 是 128 槽，event queue 才是 8 槽，这两个不要搞混）
- L2 大约 140 行 JS 就能搞定，工程量可控

### 1.3 实现要点

VDOM 里加 `diffNode / diffProps / diffChildren / shallowEq / render` 五个函数：

- **diffProps**：text 变了走 setText，样式字段变了集中调 applyStyle，事件钩子直接接管 props（dispatch 永远现取 `nodes[id].props[hook]`，所以即使每次 view() 都是新闭包也没问题，永远拿最新的）
- **diffChildren**：oldKids 按 id 索引建表 → 新树里同 id 的递归 diff、新 id 的 mount、消失的 destroy。**不支持重排**：子节点位置变化会被识别成"删旧、加新"
- **shallowEq**：数组浅比较（处理 `align: ['lm', 0, 0]` 这种数组 props，不能用 `===`）

API 是 `VDOM.render(tree)` —— 首次等价 mount，之后做增量 diff。**保留旧的 `VDOM.set/mount/destroy`** 给"精确打击"场景用（手势拖动每帧要响应，不能整树 rerender）。

### 1.4 一个真 bug：手势旁路 vs rerender

闹钟卡片拖动是命令式旁路（每帧 `VDOM.set(id, {align: [..., x, ...]})`，不重 render）。但 view() 里如果写死 `align: ['lm', 0, 0]`，下次 rerender 会"无声地"把已露出的卡片瞬移回 0。

修复：view() 里读当前 swipe 状态当 align：

```js
align: ['lm', (cardSwipe[seq] && cardSwipe[seq].x) || 0, 0]
```

这条经验对任何"声明式 + 命令式旁路"混用的场景都成立 —— **旁路修改的字段必须能从 state 反算**，否则下次声明式覆盖就翻车。

### 1.5 闹钟页业务从 559 → 650 行（净 +91 但概念清晰度 ×2）

- `state` 集中在一个对象
- `view()` 一个纯函数描述整页
- 事件回调里只改 state + `rerender()`
- 时钟每秒 rerender 整树，diff 实际只下发 1 条 setText cmd

## 2) registry 满 → 256 + 孤儿对象

烧录 diff 版本后立刻发现两个 bug：

```
W (27475) UI registry full, drop create id=sw_102
W (27476) parent id 'sw_102' not found, fallback to root
```

加 5 张卡片就开始报，而且出现"删不掉的按钮"。

**根因 1**：每张卡 9 个 obj（row + del + delL + alarm + tag + tm + sub + sw + knob）+ 固定页面 ~13 obj。registry 64 槽，5 张卡就满。修：64 → 256（PSRAM 上加 9KB 无压力）。

**根因 2**：`resolve_parent` 在 parent 找不到时回落到 root。父被丢了，子还是被创建在 root 上，destroy(row_X) 找不到这个孤儿子。修：parent 找不到就**返回 NULL 让 create 整体失败**，不再产生孤儿。

两个修复一起做。烧后稳。

## 3) 持久化：sys.app.saveState/loadState

### 3.1 设计取舍

讨论了三件事：

**A. 写时机**：每改即写 vs 退出时统一写

选**每改即写**。理由：闹钟开关、加删都是低频用户操作，NVS 一次 commit 几十毫秒无感；不需要 onExit 钩子；断电不丢。

**B. 要不要 onEnter / onExit 钩子**

最后**都不要**。理由：
- onEnter == 脚本顶层代码，加钩子是冗余
- onExit 在"每改即写"模式下没工作可做；将来有"草稿暂存"再补
- 资源清理（清 setInterval / 释放 ctx）是**框架的家务事**，业务不该感知 —— 框架自己清

最小必要集就是 `loadState()` + `saveState(json)`。

**C. 用队列还是同步直调 NVS**

这条讨论花了不少时间。结论：**dynamic_app 这边同步直调没问题**，但理由不是"NVS 自带锁"，而是"没有避让的需求"。

判断标准（写在 memory 里了）：调用线程满足以下三条全否才能同步直调：
1. 调用线程对延迟敏感（如 BLE 协议栈、ISR）
2. 是高频写源（爆 flash 寿命）
3. 写失败后果重大需隔离

dynamic_app 的 saveState 是 Script Task 里被用户操作触发，三条都不构成问题，所以同步调。

NVS **实现上**确实有锁（`nvs_api.cpp` 每个 API 首行 `Lock lock;`），但**官方文档没承诺线程安全**。靠"实现细节"撑着，不要据此对外宣称是 spec 保证。

### 3.2 实现

C 侧 native 三个：

- `sys.app.saveState(jsonString)` → JS 自己 stringify、C 只搬 string 到 NVS blob
- `sys.app.loadState()` → 读 blob 返回字符串，JS 自己 parse；首次返回 null
- `sys.app.eraseState()` → 用 set_blob 写 0 长度精确清空（不影响其它 app）

NVS namespace `dynapp`，key 用 app 名（多 app 拆出来后才动这块，见后文）。一开始用固定 "state"，多 app 时改成 `current_app_key()`。

mquickjs 自带 JSON.stringify/parse —— 省了 C 侧自己写序列化。

JS 业务侧改动小：

```js
var saved = JSON.parse(sys.app.loadState() || "null");
var state = saved || defaultState();
state.editSeq = -1;             // UI 临时态强制重置

function persist() {
    sys.app.saveState(JSON.stringify({
        alarms: state.alarms, nextSeq: state.nextSeq
    }));
}

// 三处 onClick 末尾加一行 persist()
```

## 4) SHADOW + GAP + LONG_PRESS（13 行 C）

视觉/交互三件套：

| 能力 | C 改动 | JS 用法 |
|---|---|---|
| `SHADOW` style | apply_style 加 shadow_color/width/ofs_y/opa | `shadow: [color, w, ofsY]` |
| `GAP` style | apply_style 加 pad_row/pad_column | `gap: [rowPad, colPad]` |
| `LONG_PRESS` event | 加 LV_EVENT_LONG_PRESSED 分支 + root listener 多挂一条 | `onLongPress` |

LONG_PRESS 跟 PRESSED 共享 `s_gesture.active_target`，业务里 `onLongPress(e)` 拿到的 currentTarget 就是按下时的对象 —— 跟手势库的 active_target 锁定语义一致。

闹钟里：list 加 `gap: [6, 0]` 卡片不再贴脸；卡片加 `shadow: [0x000000, 10, 3]` 立体感；新增 `onCardLongPress`：长按直接吸附到 open 露出删除按钮（提供滑动之外的另一条路径）。

## 5) 手势打磨：四个体验问题一次修

实测发现：

1. **必须按在"开关按钮"上才能拖** —— 按在卡片其它位置一动就回弹
2. **滑动后误触进编辑页** —— 拖动小幅度后 LVGL 仍发 CLICKED
3. **整页跟着滚** —— 标题/时钟/列表一起滑，体验很差
4. **编辑页底下露出列表** —— 编辑页明明 100%×100% 还能看到背景列表

四个问题对应四个根因，全在 LVGL 默认行为上：

### 5.1 PRESS_LOCK 缺失

LVGL 默认只 button 自带 `LV_OBJ_FLAG_PRESS_LOCK`（手指按下后即使移出 obj 范围 PRESSING/RELEASED 仍持续发该 obj）。label/panel 没有，按在卡片内的 label 上稍微一动 LVGL 立刻视为 release，业务收到 release 把卡片回弹。

修：所有 create 出来的 obj 默认加 `LV_OBJ_FLAG_PRESS_LOCK`。一行。

### 5.2 拖完误触 click

LVGL 的 click-vs-gesture 阈值大于我们的 SWIPE_THRESHOLD=30。拖 30 又回弹时 LVGL 仍认为是 click，触发 CLICKED。业务里没记"刚才拖过"。

修：onCardRelease 检测到位移就置 `s.suppressClick=true`，下次 onCardClick 进来直接吞掉并复位。

### 5.3 双重滚动

appRoot 默认是 panel，LVGL panel 默认 SCROLLABLE。children 总高度超 320 时 appRoot 自己也滚 —— 标题/时钟跟着推上去。

修：UI_OBJ_PANEL 创建时默认 `clear_flag(LV_OBJ_FLAG_SCROLLABLE)`；新增 `sys.style.SCROLLABLE` 让业务显式开（list 需要内部滚）。

### 5.4 编辑页没盖住

跟 5.3 同根 —— appRoot 在滚，editView 跟着滚出位置。修了 5.3 自然就好。再保险加 `align: ['tl', 0, 0]` + `scrollable: false`。

四个修复一组烧上去，体验立刻顺畅 —— 按卡片任意位置都能拖、不再误触进编辑页、整页只有 list 内部滚、编辑页完全覆盖。

## 6) 多 App 共存：宿主页 + registry

### 6.1 关键认识

讨论了 A/B 两种方案：
- **A**：每个 app 一个独立 page（page_dynamic_alarm.c / page_dynamic_calc.c）
- **B**：单宿主页 + app 注册表

一开始倾向 A（独立到底，不耦合），但你提了一句**"以后动态下载 app 怎么搞？"** —— 这一下点醒了 A 方案的本质问题：每加一个 app 都要改 C 代码、烧固件，**那就不叫动态 app**。

最终 B：单宿主页 `page_dynamic_app`，**app 名当参数**透传到 runtime，runtime 通过 `dynamic_app_registry` 按名查脚本 buffer。**未来加新 app 只需在 registry 表加一行**；将来"从 NVS 读脚本"只需重写 registry.c 一个文件，外部零改动。

### 6.2 三层抽象

```
page_dynamic_app          唯一宿主页（不绑定具体 app）
  └─ dynamic_app_start("xxx")
       └─ dynamic_app_runtime
            └─ registry_get("xxx") → 拿到对应 buffer
                 └─ JS_Eval(buffer)
```

`registry` 是核心 —— 它把"脚本来源"这个会变化的细节关进一个文件里：

```c
extern const uint8_t alarm_js_start[] asm("_binary_alarm_js_start");
extern const uint8_t calc_js_start[]  asm("_binary_calc_js_start");

static const app_entry_t g_apps[] = {
    { "alarm", alarm_js_start, alarm_js_end },
    { "calc",  calc_js_start,  calc_js_end  },
};
```

将来 OTA 下载就改成"先查 NVS 有没有这个 app 的脚本，没有再查内嵌的"。`page_dynamic_app` / `runtime` / `natives` 都不动。这是 Linux `dlopen()` 的同款思路。

### 6.3 app 名怎么传？

`xTaskNotify` 只能传 32 位整数，传不了字符串。两个全局变量当通信渠道：

- **`page_dynamic_app::s_pending_app`**：UI 线程菜单点击 set，UI 线程 page create 读 —— 同一线程，无竞态
- **`registry::s_current_name`**：UI 线程 set，Script 线程 read —— 跨线程但**严格因果**：set → notify → wait 唤醒 → read，FreeRTOS notify 自带内存屏障保证可见

这种"启动信号 + 启动参数"是 FreeRTOS 标准模式，绝对安全。

### 6.4 切 app 干净到什么程度

`PAGE_DYNAMIC_APP` 切回自己（用户从 alarm 切到 calc），page_router 触发：

```
destroy 阶段：
  ├─ ui_set_root(NULL)              关闭 root，禁止再创建 LVGL
  ├─ dynamic_app_stop()             通知 Script 退出 tick
  │   └─ JS_FreeContext + 释放 1MB JS 堆
  ├─ ui_unregister_all()            清 256 槽 registry
  └─ lv_obj_del(screen)             删 LVGL 屏幕

create 阶段：
  ├─ 建新 screen / 新容器
  ├─ ui_set_root(新容器)
  └─ dynamic_app_start("calc")      读 s_pending_app
       └─ Script 重新醒来 → 新 ctx → eval calc.js
```

calc 拿到的环境跟"刚开机第一次跑 calc"完全等价。**两个 app 不存在"共存于内存"** —— 它们是**串行**的，时间上分时复用同一套资源。

唯一**真共存**的是 NVS：alarm 数据存 `dynapp/alarm`，calc 数据存 `dynapp/calc`（calc 没持久化），互不影响。

### 6.5 calc.js 实现

复用了 alarm.js 里的 VDOM 框架（拷过来一份，**不抽公共模块** —— 保持每个 app 独立、可单独换实现）。业务部分约 200 行：

- 状态机：`{display, prev, op, justEval, newEntry}`
- 5 行 × 4 列按钮，'0' 占两格
- 链式运算：按运算符自动结算上一段（`5 × 3 - 2 = 13` 而不是 `5 × (3-2) = 5`）
- 选中态：按下运算符后该按钮变亮色（橙 → 黄）
- 不持久化（计算器即用即丢符合直觉）

布局难点是 240×320 塞下 5 行按钮 + display：BTN=46、间距 4，5 行 + display 60 = 314，剩 6 px ≈ 极限。

## 改动文件清单

| 文件 | 阶段 | 类型 | 说明 |
|---|---|---|---|
| `dynamic_app_ui.h` | 2/4/5 | 改 | REGISTRY_MAX 64→256；style enum 加 SHADOW/GAP/SCROLLABLE；event enum 加 LONG_PRESS=5 |
| `dynamic_app_ui_styles.c` | 4/5 | 改 | apply_style 加 SHADOW/GAP/SCROLLABLE 三个 case |
| `dynamic_app_ui.c` | 5 | 改 | LONG_PRESSED 分支 + root listener 多挂；create 时加 PRESS_LOCK；panel 默认 clear SCROLLABLE |
| `dynamic_app_ui_registry.c` | 2 | 改 | resolve_parent 找不到时返回 NULL（不 fallback root） |
| `dynamic_app_natives.c` | 3/6 | 改 | sys.app.{save/load/erase}State 三个 native；style 常量加 SHADOW/GAP/SCROLLABLE；NVS key 用 current app name |
| `dynamic_app_internal.h` | 3 | 改 | 加三个 func_idx 字段 |
| `dynamic_app_runtime.c` | 3/6 | 改 | extra 13→16；eval 改成走 registry 按当前 app 名取 buffer |
| `dynamic_app_registry.h/c` | 6 | 新增 | app 名 → 脚本 buffer 查找 + 当前 app 名状态 |
| `dynamic_app.h/c` | 6 | 改 | dynamic_app_start 加 app_name 参数；通过 registry_set_current 透传 |
| `dynamic_app/CMakeLists.txt` | 3/6 | 改 | REQUIRES 加 storage；新增 registry.c；EMBED 两个 js |
| `app/pages/page_dynamic_app.h/c` | 6 | 改 | 加 set_pending(name)；create 用 pending app 名启动 |
| `app/pages/page_menu.c` | 6 | 改 | 把 "Dynamic App" 拆成 "Alarm" + "Calculator" 两项，各自 set_pending |
| `dynamic_app/scripts/alarm.js` | 1/3/4/5 | 改名+大改 | 原 app.js；加 VDOM.render diff；业务改成 view+rerender；接入 sys.app；用 shadow/gap/long_press；适配 PRESS_LOCK / SCROLLABLE / suppressClick |
| `dynamic_app/scripts/calc.js` | 6 | 新增 | 计算器，VDOM 框架拷自 alarm + 计算器 view + state |

## 验证

烧录测试通过：

### 闹钟（alarm）
- ✅ 切开关、加卡、删卡 → 状态正确，自动持久化
- ✅ 退出再进，所有状态保留（包括 nextSeq）
- ✅ 按卡片任意位置都能拖（PRESS_LOCK 修复）
- ✅ 拖完不会误进编辑页（suppressClick 修复）
- ✅ 长按任意位置都能拉出删除按钮
- ✅ 整页只有 list 内部滚，标题/时钟/+号钉死
- ✅ 编辑页完全覆盖，不再露底
- ✅ 时钟每秒 rerender 整树，无明显卡顿

### 计算器（calc）
- ✅ 5 行按钮 + display 都在屏内
- ✅ 基础四则、链式运算、=、±、%、C、. 全部正常
- ✅ 0 ÷ 0 显示 Err
- ✅ 按运算符后该键变亮色

### 多 app 切换
- ✅ Menu → Alarm → 返回 → Calculator → 返回 → Alarm，反复切换 10+ 次无残留
- ✅ alarm 和 calc 互不影响（calc 切换不影响 alarm 的 NVS 数据）
- ✅ 每次切换 1MB JS 堆释放再重建，PSRAM 占用平稳

## 经验沉淀

### "声明式只用一半"是最坏的状态

之前 mount 用声明式、set 用命令式 —— 业务读起来需要在两套心智模型之间切换，比纯命令式还难受。**要么不上声明式，上了就要把更新路径也做掉**。这条对任何 React-like 框架都成立。

### 旁路修改的字段必须能从 state 反算

闹钟手势拖动是命令式旁路（每帧改 align），如果 view() 里写死 `align: 0` 下次 rerender 就翻车。修法：view() 读当前 swipe 状态当 align 值。

更普遍的规则：**任何"声明式 + 命令式旁路"混用的场景，旁路修改的字段必须能从 state 反算**。否则等于声明式和命令式抢夺同一个属性的所有权。

### 方案 B（每改即写）+ 不加 onExit > 方案 A（退出时写）+ 加 onExit

讨论持久化时，最初考虑过 `onExit` 钩子让业务在退出前 save。最终发现：在"每改即写"模式下 onExit 没工作可做；想做"清 setInterval/释放资源"那是**框架的家务事**，业务不该感知。

最小必要集是 `loadState/saveState`，没有任何生命周期钩子。读者反而立刻能看出"哪段是初始化、哪段是事件、哪段是启动"。**结构清晰 > 框架钩子**。

### 判断"要不要队列写 NVS"的三条标准

不能凭"NVS 是不是线程安全"判断。要看调用线程：

1. 对延迟敏感？
2. 是高频写源？
3. 写失败后果重不重？

三条全否才能同步直调；任一条是"是/不能/重"就要走队列丢到专门的写者线程并加 debounce。BLE 那边三条都中（协议栈 + 通知风暴 + 不能让 NVS 拖累 BLE），所以走队列；dynamic_app 这边都不中，所以同步调。

NVS 实现上有锁，但**官方文档没承诺**线程安全 —— 靠实现细节撑着不要据此对外宣称 spec 保证。这条已经追加到 memory 的 `feedback_single_writer_pattern.md`。

### LVGL 的"默认行为"是体验杀手

四个手势问题（PRESS_LOCK 缺失、误触 click、双重滚动、编辑页露底）全是 LVGL 默认行为造成的。LVGL 设计是"按桌面级控件思路给默认值"（panel 默认可滚、label 没 PRESS_LOCK），但小屏嵌入式上反着来更合理。

我们的修法：在 `do_create` 里**统一把默认改成嵌入式友好的**（PRESS_LOCK + 关 SCROLLABLE），需要打开的让业务显式开（`scrollable: true`）。**默认值反转 + 提供显式开关** > **保留怪默认 + 让业务每次写防御代码**。

### "动态 app"的 90% 工程在切换的干净度上

calc 拿到的环境必须跟"冷启动"完全等价，否则就是隐性耦合。我们的 destroy 五件套（关 root → stop script → 释 ctx + 1MB 堆 → 清 registry → 删 screen）每一步都不能少。任何一步漏了将来都是诡异的"上个 app 残留"bug。

### 半成品抽象比直接复用更糟

calc.js 没抽 VDOM 公共模块，直接把 alarm.js 的 VDOM 框架拷了一份。短期看是重复，长期看是**对的**：每个 app 自带 VDOM 副本意味着 app 之间永远不会因为框架升级互相牵连。等真有 5+ 个 app 都需要同一份 VDOM、且修改同步成本高时再抽，**不要为不存在的需求预先付费**。

### 8 槽 / 128 槽 cmd queue 的混淆

中间一度以为 diff 会爆 event queue（8 槽），后来才发现 diff 产生的 cmd 走的是 **cmd queue（128 槽）**，event queue 是反向通道（UI → Script 点击事件）。两个队列方向不同、容量不同，命名上要区分清楚。**遇到队列容量讨论先确认是哪个方向的队列**。

### registry 这层抽象的真正价值不在当下

当前 registry 就是个 "name → buffer" 表，看起来增加了一层。但**关键是它把"脚本来源"这个会变化的细节关进了一个文件里**。将来 OTA 下载 / NVS 加载 / SD 读脚本，外部代码完全不用动 —— 这是 Linux `dlopen` 的同款思路。

设计层面这条规则：**任何"现在固定、将来可能变"的细节都应该被一层抽象关起来**。哪怕这层抽象现在只有一种实现。

## 接下来

候选（按"做了立刻能感觉到"排）：

1. **图片/图标控件**（30 行 C） —— `lv_image_create` 暴露给 JS，闹钟/计算器都能放图标
2. **半透明 opacity style key**（5 行 C） —— 半透明蒙层、模态弹窗都受益
3. **再做 1-2 个 app**（便签 / 待办 / 简易音乐控制） —— 验证 registry 设计是否真通用
4. **从 NVS 加载脚本**（30 行 C，重写 registry.c） —— 真"动态"下载 app 的第一步
5. **编辑闹钟功能**（200 行 JS） —— 闹钟现在编辑页是占位，做完才算完整
6. **L3 keyed reorder diff**（150 行 JS） —— 还是没场景，先放着

短期我倾向 **1+2 一组**（35 行 C，35 分钟），之后选 **3 或 4** 看你想验证通用性还是推进真"动态"。
