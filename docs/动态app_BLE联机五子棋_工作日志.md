# 动态 App BLE 联机五子棋 + Launcher 栈悬垂修复 工作日志

**日期**：2026-05-02
**分支**：feat/optimize_page
**作者**：ChenShuang + Claude

---

## 0. 起因

doodle_pkg 把"canvas + 大文件 fs + assets"全链路跑通了，但平台还有几条路径没人压过：

1. **BLE 真双向**：notif 只单向收，doodle 完全不用 BLE，缺一个"双方互相 send"的实测
2. **长时 setInterval 稳定性**：现有 app 全是事件驱动，没人跑过持续若干分钟的高频 timer
3. **canvas onPress 浮点 → 离散坐标映射**：doodle 是连续描线，没做整数对齐
4. **PC 端 GUI 双向交互页**：tools/companion/gui/pages 里 home/music/notify/upload/log 都是单向展示或表单，没有"鼠标点击 → 推送 → 回包刷新"的实时页

第二轮挑选的方向是 **BLE 联机五子棋**——一次压满上面四条；同时验证一下"动态 app 也能承担轻度游戏"。

中途用户提了一个关键修正：**"谁家离线状态玩对战啊"**——把原计划里"mailbox 离线接力 / 续局"全部砍掉，改成**强在线对战**：双方必须同时在五子棋页才能玩，任一方离场判对方赢。

收尾时发现一个意外的老 bug：launcher 显示动态 app 名称会乱码。定位是 use-after-free，顺手修了。

---

## 1. 关键决策

### 1.1 棋谱单一事实源 = 设备

PC 端是瘦客户端。任一方进场都由设备 push 完整棋谱，PC 收到 sync 才重建棋盘状态。

理由：
- 设备 JS 已经持有 board 数组和判胜逻辑，PC 再来一份就有"两个权威"问题
- 重连 / 断线场景下，"谁的状态对"必须有定论。让 PC 听设备的最简单
- PC 端落子是**乐观更新**：本地先画白子 + 立刻 send，不等设备回包；如果设备拒绝（轮次不对），下一次 sync 会覆盖 PC 错误状态

代价：PC 端要写一份**只读**的判胜（用于本地乐观渲染胜线），但这部分 ~25 行 Python，不算重复。

### 1.2 在线对战 only：present 心跳 + watchdog 仲裁

原始计划是借 mailbox 做"离线续局"，被用户砍掉。改造后的协议：

```
设备 ──present (1500ms)──► PC
PC   ──present (1500ms)──► 设备
```

任一方 4 秒收不到对方心跳 → **判对方离场 = 我方赢**。

附加路径：
- 主动离场（PC 切页 / 关 GUI / 拔 BLE）发 `leave`，对方立即判赢（不等 4 秒）
- 设备退 app 没有 onExit 钩子（见 §3），靠 PC 端 watchdog 兜底

为什么不做 onExit：见 §3，**watchdog 是无论如何都需要的下界**——崩溃 / 断电 / BLE 物理断不可能跑 onExit；onExit 充其量把 4 秒延迟缩成 0 秒，结构性必要的兜底逻辑还是得写。N=1 的 app 不构成扩平台 native 的依据。

### 1.3 saveState 不存棋谱、改存战绩

强在线 = 每局都是临时对战。退出 app 后再回来对手肯定不在了，loadState 续局没意义。改成存累计 `{w: 胜场, l: 负场}`，HUD 副标题"战绩 N胜 N负 · PC 在线/离线"实时显示。

副作用：saveState 频次大幅下降（只在每局结束时写一次），更贴合 NVS 写入特性。

### 1.4 13×13 + cell=16 + 棋子用 line(thickness=11) 模拟圆

canvas 不提供圆形原语。三种选择：
- A：暴露 `sys.canvas.circle` native（违反"不为单 app 加 native"）
- B：JS 用 Bresenham 自己画（200+ 行，浪费）
- **C（采用）**：复用 `sys.canvas.line(x,y,x,y, color, thickness=11)` —— 同点起终点 + 大粗细 = (2t-1)×(2t-1) 方块。视觉上是 11×11 圆角块，棋盘上看跟圆几乎没区别。1 行代码搞定。

棋盘尺寸：13×13 / cell=16 / margin=12 → 画布 216×216，加上 36px HUD 共 252px。屏幕可用区 268px（去 24 状态栏 + 28 退出区），留 16px 缓冲。

白子加 4 个黑色边像素（上下左右各 5px 偏移），避免和木色背景糊在一起。

### 1.5 紧凑编码 sync 棋谱

单帧 BLE 上限 200B。`{from,to,type,body}` JSON 头 ~50B，留给 body 150B。如果按 `{moves:[[6,6],[6,7]...]}` 一步 7 字节，21 步就爆。

编码：`r * 13 + c` → 0..168，1 字节够 → 2 hex 字符。140 chars / 2 = **70 步上限**，五子棋实战足够（标准对局 30~60 手）。

### 1.6 PC 端 `<Map>` / `<Unmap>` 当生命周期信号

CTk 的 page 在 app.py 启动时全部 build，靠 `grid()` / `grid_forget()` 切换可见。Tk 的 `<Map>` / `<Unmap>` 事件正好对应"页面进入显示 / 退出显示"，绑到本 frame 上：

```python
self.bind("<Map>",   self._on_page_enter)   # 进场：发 present + sync_req + 启心跳
self.bind("<Unmap>", self._on_page_leave)   # 离场：发 leave + 停心跳
```

CompanionApp `_on_close` → `withdraw()` 也会触发当前 page `<Unmap>` → 自动发 leave，不需要显式钩 quit 流程。

### 1.7 router 通配注册

bridge_provider 给 gomoku_pkg 注册了 4 个具体 type 的 handler（move/reset/resign/sync），结果第一次跑就发现 **present 包被 router 默默丢弃**——日志里看到设备 1.5s 一次 NimBLE notify 但 PC 端 GUI 始终显示"设备离线"。

修法：用 `router.register("gomoku_pkg", None, handler)` —— `type=None` 通配。所有 `from:"gomoku_pkg"` 的消息都进同一个转发函数，再 emit `gomoku:rx` 给 GUI 自己分发。

教训：和 weather/music 那种"少量固定 type"不一样，**对端是 JS 业务的 service 应该走通配**——业务每加一个新 type 都要改 provider 是反模式。

---

## 2. 落地清单

### 2.1 设备端 gomoku_pkg

**新建（2 文件）**

- `dynamic_app/scripts/gomoku_pkg/manifest.json`
  ```json
  {"id": "gomoku_pkg", "name": "五子棋", "icon": "APPS", "iconColor": "ACCENT", "version": "1.0.0"}
  ```

- `dynamic_app/scripts/gomoku_pkg/main.js`（462 行）
  - 棋谱状态（board / moves / gameOver / winLine）
  - 在线状态（pcPresent / pcLastSeenMs）
  - 36px HUD（标题 + 副标题，副标题显示战绩 + 在线状态）+ 216×216 canvas
  - 心跳（1500ms setInterval）+ watchdog（1000ms 巡检 4s 超时）
  - `applyMove(r, c, fromPC)`：落子 / 判胜 / 通知对端 / 战绩更新
  - `markPCSeen()`：收到 PC 任何业务消息或 present 即刷新 lastSeen，第一次见到时 push sync
  - `settleByLeave(reason)`：对方离场立即判我方赢（设备永远是黑）
  - 紧凑编码 `encMoves`（70 步上限）

### 2.2 PC 端

**新建（1 文件）**

- `tools/companion/gui/pages/gomoku.py`（580 行）
  - Tkinter Canvas 13×13 棋盘 cell=28（PC 端屏大可以画大）
  - hover 提示当前可落点（白子青色描边）
  - 棋谱日志（围棋记法：列 A..N 跳过 I + 行 1..13）
  - 心跳 / watchdog 用 `self.after(...)` 复用 Tk 主循环，不开线程
  - `_on_page_enter` / `_on_page_leave` 进出场协议
  - `_dec_moves` 与设备端 `encMoves` 对偶
  - 乐观更新本地 + send：`_apply_local_move(r,c)` + `_tx("move",{r,c})`

**改动（2 文件）**

- `tools/companion/providers/bridge_provider.py`
  - 注册 `("gomoku_pkg", None, _on_gomoku_fwd)` 通配 type
  - 监听 `gomoku:tx` bus 事件 → 反向 BLE send

- `tools/companion/gui/app.py`
  - 侧边栏 PAGE_DEFS 加 `("gomoku", "五子棋", GomokuPage)`

### 2.3 Launcher 栈悬垂修复

**改动（1 文件）**

- `app/apps/launcher/pages/page_launcher.c`
  - `cell_def_t` 加 `char *dyn_label` 字段
  - `cells_collect`：动态 app 分支 `c->dyn_label = strdup(entries[i].display); c->label = c->dyn_label;`
  - `cells_clear`：同时 free `dyn_label`

---

## 3. "动态 app 是否需要 enter / update / exit 钩子"的讨论

写五子棋时我提到"PC 退页发 leave，设备退 app 怎么办"，引发了对动态 app 平台生命周期的讨论。结论沉淀：

### 3.1 现状盘点

| 阶段 | 平台做什么 | JS 现在怎么参与 |
|---|---|---|
| **enter** | `runtime_setup()` + `bind_globals` + `eval_app()` 跑 main.js 顶层 | 顶层语句就是入场逻辑（mount UI / loadState / 启 setInterval）|
| **update** | tick 循环：drain UI 事件 → drain BLE → 跑 intervals → sleep 1~50ms | 不存在"每帧钩子"；只能注册事件源（setInterval / ble.on / onClick）|
| **exit** | `runtime_teardown()` 直接 free JSContext | ❌ 完全没机会跑 cleanup |

也就是说 **enter 已经隐式存在**（顶层代码本身），**update 不应该有**（嵌入式不该有"每帧 tick"思路），**exit 是真正的缺口**。

### 3.2 为什么不需要 update

游戏引擎 / 浏览器有 update 是因为要驱动渲染。这里 LVGL 自己在另一个线程刷屏，业务不需要每帧醒来。一个 update 钩子只会让人写出"每 16ms 整树 rerender" 的烂代码。事件驱动 + setInterval 已经覆盖所有真实需求。doodle / notif / gomoku 没人写过"每帧"代码，反证 update 是负面价值。

### 3.3 为什么 exit 现在不做

唯一真正受益的场景是"通知对端我离场"。其它要么平台已经自动做（free 句柄），要么改不了用户体验（异步 modal 来不及）。

而且 watchdog **是必须存在的下界**：崩溃 / 断电 / BLE 物理断都不可能跑 onExit 回调，对端唯一可靠的判定方式还是"超时无心跳"。onExit 充其量把 4 秒延迟缩成 0 秒。

加 onExit 还有踩坑成本：teardown 路径上 LVGL cmd 队列、BLE inbox、interval 表都正在准备 free，让 JS 跑用户回调 + 让用户调 `ble.send(...)` 入队、依赖下一个 tick 才发——但下一个 tick 已经不会来了。要做就得"先跑用户回调 → 强 flush 一次 cmd 队列 + BLE TX → 再 teardown"，工作量不小。

**结论**：等到 N≥2 个动态 app 都有"离场要通知对端"的需求时再立项。现在 N=1，watchdog 兜底足够。

---

## 4. 几次"踩坑 + 修复"

### 4.1 router 漏注册 present 导致心跳被静默丢

```
NimBLE: GATT procedure initiated: notify; att_handle=43   ← 设备 1.5s 一次发 present
NimBLE: GATT procedure initiated: notify; att_handle=43
... 但 PC GUI 一直显示 "设备 离线"
```

bridge_provider 只注册了 `move / reset / resign / sync` 4 个具体 type，`present / leave` 落进 router 的"无 handler" 路径被默默 drop。

修法：改用通配 —— `register("gomoku_pkg", None, handler)`。

教训：业务 app 的 service 应该走 type 通配。和 weather / music 那种"少量固定 type 由 PC 主导"的 service 不一样，业务 app 的 type 集合是动态扩展的——业务每加一个新 type 都改 provider 是反模式。

### 4.2 设备端 onPress 不校验对方在场 → 自己玩自己

第一版没加 `if (!pcPresent) return`，结果 PC 没在场时设备 onPress 也能正常落子 + 调 ble.send，设备自己往一个无人监听的 channel 发包。

修法：onPress 第一件事就是校验 `pcPresent`，没在场弹 toast "PC 未在五子棋页"。同样的校验在 reset / resign 操作里也要做。

### 4.3 单帧 200B 装不下满盘 sync

第一版 sync body 是 `{moves: [[6,6],[6,7]...], turn, over, win}`，每步 7 字节 JSON，21 步就爆。设备 send 静默失败（dynapp_bridge 检查 len > 200 直接 return false），PC 永远看不到完整棋谱。

修法：紧凑 hex 编码（§1.5）。70 步上限，五子棋实战足够。

教训：**和 BLE 最大单帧拉锯是双端协议设计的高频考点**——任何"列表型"字段都要先估算最坏情况长度。

### 4.4 launcher 显示动态 app 名称乱码 ⭐ 老 bug

```
进 gomoku 玩一会 → 上滑退出 → launcher 上的 "doodle" 名字变成乱码
再进 doodle → 退出 → "habit" 也变乱码
```

第一反应是字体子集 bug 或者 UTF-8 截断。但加调试日志前先看了一下数据流：

```c
// page_launcher.c::cells_collect
dynamic_app_entry_t entries[MAX_DYN_APPS];          // ← 栈上局部变量
int n = dynamic_app_registry_list(entries, ...);
for (int i = 0; i < n; i++) {
    cell_def_t *c = &s_ui.cells[s_ui.cell_count++];
    c->label = entries[i].display;                  // ← 持有栈数组成员的指针
    c->dyn_name = strdup(entries[i].id);            // dyn_name 拷了
    if (entries[i].icon[0]) {
        strncpy(c->dyn_icon, entries[i].icon, ...); // dyn_icon 拷了
        c->icon = c->dyn_icon;
    }
}
```

`dyn_name` 和 `dyn_icon` 都做了拷贝，**唯独 label 直接指着栈数组**。函数退出后栈帧被后续调用覆盖，cell_def 里的 label 指针就 dangling 了。

为什么以前没暴露？因为 cells_collect 之后立刻就开始用 cells 创建按钮，而 LVGL 的 lv_label_set_text 内部会自己拷贝字符串——只要那一瞬间栈内存还没被覆盖就侥幸是对的。但 LVGL 真的拷贝吗？看了一下：lv_label 的 text 默认是 `LV_LABEL_LONG_WRAP` + `lv_obj_set_style_text_align`，确实有自己的 copy 逻辑——所以**第一次显示是对的**。

那乱码是怎么来的？反复进 / 退动态 app 时，launcher 的 cells_clear 不会被调（cells 一直保留），但 lv_label 在 launcher 重建时（screen 切换）会**重新读 c->label**——这时 label 指向的栈早就被覆盖成完全无关的字节序列。

修法：cell_def_t 加 `char *dyn_label` 字段，cells_collect 时 `strdup`，cells_clear 时 free。和现有的 `dyn_name` / `dyn_icon` 同模式。

教训：
- **栈上结构不能让外部持有指针，要么调用方拷贝、要么改成静态/堆**。这条规则在 dyn_name / dyn_icon 已经实践过，但 label 漏了——同一个函数里有"拷贝派"和"裸指针派"两种风格混着写，是这种 bug 的高发模式。**要么全拷，要么全引；规则要一致。**
- **乱码这种症状不要先怀疑字符串处理（编码 / 截断 / 字体）**，先看数据生命周期。dyn_label 拷贝后图标也就跟着回来了——大概率之前 label 内存乱波及到周边渲染，让 icon obj 也表现异常。

### 4.5 设备端 gameOver 后多次 modal

`applyMove` 判胜后调 `announceWin()`，第一次没问题。但如果 mailbox（已被砍掉）或者 PC 重发了同一步会怎样？

实际不会——`applyMove` 头一行 `if (gameOver) return false`，第二次胜利前 gameOver 已被 set。✅

但**认输 / 离场两条路径都得手动 set gameOver + bumpRecord**，不要走 applyMove。这条容易写漏，写完我专门 grep 了一遍 `gameOver = '` 确认每条赋值后都跟着 `bumpRecord`。

---

## 5. 验证

### 5.1 五子棋基本功能
- [x] 设备端 launcher 能看到"五子棋"图标（紫色 APPS）
- [x] 进 app 显示棋盘 + HUD + 战绩
- [x] PC 端切到五子棋页 → HUD 翻"● 设备 在场"，副标题显示当前手数
- [x] 设备落子 → PC 棋盘自动出现黑子 + 棋谱日志加一行
- [x] PC 鼠标点 → 设备棋盘自动出现白子
- [x] 五子连珠 → 双方都画红色胜线 + 弹胜负 modal
- [x] 战绩自动累加（设备端 saveState 持久化）

### 5.2 在线判负
- [x] 设备退 app → 4 秒后 PC 弹"设备已离线" + PC 判赢 + 战绩计 PC 1 胜
- [x] PC 切到别的 page → 设备 4 秒后判赢
- [x] PC 关 GUI → 触发 `<Unmap>` 发 leave → 设备立刻判赢
- [x] BLE 拔了 → PC `disconnect` 事件触发立即判 PC 赢

### 5.3 不破坏旧能力
- [x] doodle / notif 仍可正常使用
- [x] launcher 上其它动态 app 名称不再乱码（核心收益）
- [x] 切换 app N 次后 launcher 仍稳定

---

## 6. 文件改动清单

### 新建（4）
- `dynamic_app/scripts/gomoku_pkg/manifest.json`
- `dynamic_app/scripts/gomoku_pkg/main.js`
- `tools/companion/gui/pages/gomoku.py`
- `docs/动态app_BLE联机五子棋_工作日志.md`（本文件）

### 修改（3）
- `tools/companion/providers/bridge_provider.py` —— 通配 router + gomoku:tx 反向桥
- `tools/companion/gui/app.py` —— 侧边栏入口
- `app/apps/launcher/pages/page_launcher.c` —— **dyn_label 拷贝修复 use-after-free**

---

## 7. 平台能力覆盖盘点（更新）

doodle_pkg + notif_pkg + gomoku_pkg 加起来：

| 路径 | 谁验证 |
|---|---|
| LittleFS `apps/<id>/main.js` 上传 + 加载 | 全部 |
| LittleFS `apps/<id>/manifest.json` 解析 | 全部（gomoku 触发 launcher label bug，反向加固）|
| LittleFS `apps/<id>/assets/*.bin` 静态资源 | doodle |
| sys.fs.write ≤196B / 256KB | doodle |
| sys.fs.list / remove | doodle |
| NVS `dynapp/<id>/state` 沙箱 | notif（消息历史）/ gomoku（战绩）|
| **NVS `dynapp_mb/<id>` 离线消息归档** | notif |
| BLE 实时**单向接收**（PC → 设备）| notif |
| **BLE 真双向**（互发 send + on） | **gomoku**（首次）|
| **长时高频 setInterval**（10 分钟级 1Hz+） | **gomoku**（首次）|
| sys.canvas 像素绘图 + PSRAM buffer | doodle |
| **canvas onPress 浮点 → 离散坐标映射** | **gomoku**（首次）|
| 平台 long_press 吞 click | doodle |
| UI.modal / toast / fadeIn | 全部 |
| Material Symbols 图标字体 | 全部 |

**仍未覆盖**：sub_router push/pop（多级页面）、多 canvas 并存、sys.canvas + sys.fs 大文件并发写、动态 app native 模块加载（如有动态 require 设计）、onExit 钩子（已决定不做）。

---

## 8. 一句话总结

**强在线五子棋把 BLE 双向 + 长时 setInterval + canvas 离散坐标三条没人压过的路径一次性跑通**，**顺手修了 launcher 老 use-after-free** —— 后者价值可能比五子棋本身还大，因为它一直在那儿等着第 N 个动态 app 把它逼出来。

---

## 9. 不在本次范围（下一轮）

- ❌ 平台 onExit 钩子（讨论结论：N=1 不立项）
- ❌ sub_router push/pop（gomoku 用单页 + modal 够用）
- ❌ 多 canvas 并存（仍未验证）
- ❌ 五子棋 PC 端权威判胜的"防作弊"（业务无聊不优化）
- ❌ 五子棋禁手 / 让子 / 计时（标准规则之外的可玩性扩展）
- ❌ 自动 build/version 标识（doodle 日志里就提过，仍未做）

---

## 10. 关键经验沉淀

### 10.1 业务 app 的 BLE service 注册要走 type 通配
和 weather/music 那种"PC 主导、type 固定"不一样，业务 app 的 type 集合会动态扩展。每加一个 type 都改 provider 是反模式。统一 `register(from_app, None, fwd_to_bus)` 后，新增 type 零侵入。

### 10.2 栈上结构外部持有指针 = 定时炸弹
launcher 这条 bug 在那儿等了至少一个版本。规则简单清晰：**调用方传栈结构出来时，使用方要么字段全拷、要么全用引用——不要一个字段拷一个字段引**。grep 确认风格一致比单点修复更重要。

### 10.3 乱码不要先怀疑字符串处理
encoding / 截断 / 字体子集都是猜测。先看数据生命周期：是不是 use-after-free / 跨任务读到半新半旧 / DMA buffer 没等 cb 就 free。

### 10.4 双端协议设计先估单帧最大长度
任何"列表型"body 都要按最坏情况估算。BLE 200B 单帧是硬限制，超了要么失败要么分包。紧凑编码（hex / 位打包 / CBOR）通常比"分包重组"简单。

### 10.5 在线对战的"判活"必须有不依赖回调的兜底
onExit / leave 包是优化项；watchdog 心跳超时是**结构必需**。崩溃、断电、物理拔线都不会触发回调，但都会让心跳停。设计阶段就要把"对面没了"这件事的兜底先立住，再考虑加快路径。

### 10.6 强在线对战是简化设计的好工具
最初想用 mailbox 做"离线续局"，方案变得很复杂（NVS namespace 共存 / replay 顺序 / 状态合并）。改成"双方必须同时在场"后，整个状态机简化一个数量级——而且这才是"对战"的真实语义。**当一个能力（mailbox）让设计变复杂，要先质疑它是不是这个场景的对的工具**，而不是想着怎么把它塞进去。
