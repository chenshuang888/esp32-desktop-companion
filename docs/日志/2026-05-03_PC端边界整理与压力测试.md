# PC 端边界整理 + 平台压力测试 工作日志

**日期**：2026-05-03
**分支**：feat/optimize_page
**作者**：ChenShuang + Claude

---

## 0. 起因

PC 端插件化重构刚做完，主程序总算变成了"哑总线 + plugins/ 业务"的架构。但靠回顾梳理发现还有一堆**名不副实的边界**：

- `companion/shared/` 看起来是"被多方共享的目录"，但里面 `archive_org` 只服务 music 一个页，`toast` 只服务 notify 一个 provider —— 名实不符
- `gui/pages/music.py` 548 行，业务逻辑（archive.org 搜索 / 下载 / 批量保存）和 widget 渲染混在一起
- `plugins/*` 仍然 `from companion.shared.* / from companion.gui.*` 直接进主程序内部 —— 违反"插件只看 SDK"的契约
- `tools/` 根目录散着 5 个 .py 构建脚本 + 1 个 .txt，运行时与构建期混在一起
- `tools/plugins/music_proxy/` 是个用不到的孤儿插件（设备端没做 music 动态 app，从来没人调它）

中途又顺手验证了一件更基础的事——**动态 app 平台是否真的"加新 app 不改基础设施"**。这一轮一次性把这些坑都填了。

---

## 1. 关键决策

### 1.1 `shared/` → `platform/` 是命名问题，不是搬家问题

最初想"把 archive_org 等只一处用的 helper 搬进各自插件"。被用户驳回：

> shared 是给 provider 和 plugin 提供基础服务的，并非要"被多方共享"才能放进来。providers/ 只设计 BLE，gui/ 只设计页面，shared 才是平台能力适配层。

按这个语义，目录里**每个文件都没问题**——是名字误导。`shared` 暗示"共享"，让人每次 review 都问"真有多方用吗"；`platform` 准确描述"平台能力适配"。

定下规则：**不搬家，只改名**。`git mv tools/companion/shared tools/companion/platform`，所有 `from ...shared.*` 全替换。

### 1.2 原生功能复刻 ≠ 重复

讨论里另一个澄清。设备端原生 weather/notify/music 和动态 app 端的 weather/notif/music 重复，最初我当成"边界债"。用户解释：

> 动态 app 本身是去复刻原生，验证平台能力。原生和动态在功能上是双轨，不是耦合，本意是 PC 那边 plugins/* 和动态脚本之间高度解耦。

按这个澄清，`platform/geoip_weather.py` 被 native + plugin 同时用是**合理的**——两条独立链路共享一个平台能力。问题不在"要不要共享"，而在"插件**怎么访问**"。

→ 引出下一节。

### 1.3 plugin_sdk 升级为包，加 platform/gui 门面

让 `plugin_sdk` 从单文件升级为包：

```
plugin_sdk/
├── __init__.py    （原 plugin_sdk.py，Plugin 基类不动）
├── platform.py    re-export geoip_weather
└── gui.py         re-export theme / widgets
```

插件作者从此**只 import `companion.plugin_sdk[.platform/.gui]`**，再不需要知道 `companion.platform.*` 或 `companion.gui.*` 的存在。SDK 暴露的是**白名单**：`platform.py` 只挂当前真有插件用的能力（geoip_weather），smtc/toast/packers 等"原生 provider 私有依赖"**故意不挂**——等真有插件需要再加。

这一步落地后，`grep "from companion\." tools/plugins/` 只剩 7 行，全是 `from companion.plugin_sdk[.x] import ...` —— **0 处直接进主程序内部**。

### 1.4 GUI 业务下沉

`gui/pages/music.py` 548 行被砍：把 `_folder_path / _scan / _add_files / _delete_file / parse_track_meta` 等纯业务函数提到 `platform/music_library.py`（77 行），把 archive.org 搜索的"search + resolve_mp3 过滤"流程提到 `platform/archive_org.py::search_with_mp3()`。

`gui/pages/music.py` 缩到 494 行，全是 widget 渲染 + 事件桥接，**没有一行业务逻辑**。

规则定型：**`gui/` 只做渲染，`providers/` 只做 BLE，`plugins/` 只做动态 app 业务，`platform/` 做对接外部能力**。

### 1.5 删 music_proxy 孤儿

设备端没做 music 动态 app，PC 端 `plugins/music_proxy/` 从来没人调。

用户决策：

> 我也不打算做这个。

`git rm -rf tools/plugins/music_proxy/`，同步把 `plugin_sdk.platform` re-export 里的 `smtc` 也撤掉（当前只有原生 media_provider 用 smtc，插件不需要 SDK 暴露）。

引出更深的洞察：

> 目前只有 ESP32 端动态 app 需要 BLE 能力的情况下，电脑端的脚本才需要新增插件。

把这句话沉淀进 `tools/plugins/README.md` §0：

> **插件存在的唯一理由**：ESP32 端某个动态 app 需要 PC 帮它做 BLE 收发之外的事。
> 反之，PC 端独立功能（如 music 文件夹同步、文件上传）**不是插件**——它们是主程序的 GUI 页。
> **插件 = ESP32 动态 app 在 PC 端的「代理人」**。没动态 app 找它，它就不该存在。

这条规则从源头防止以后再出现 music_proxy 那种"提前做好备用"的孤儿。

### 1.6 根目录散文件归位

5 个构建期 .py + `_subset_chars.txt` 散在 `tools/` 根，看不出"哪些是运行时哪些是构建期"。`git mv` 进 `tools/scripts/`，4 处 `parents[1]` → `parents[2]` 路径修复，docstring 命令示例同步更新。

`dynapp_bridge_test.py` 同理移到 `dynapp_sdk/examples/bridge_test.py`——它本质是 SDK 的 CLI 用例，应该跟 SDK 一起。

最终 `tools/` 根目录只剩：

```
tools/
├── README.md            ← 新写的总索引（这次重构产出）
├── requirements.txt
├── companion/           运行时主程序
├── plugins/             动态 app 配套插件
├── dynapp_sdk/          独立 SDK + examples/
├── dynapp_uploader/     独立 SDK
└── scripts/             构建期工具
```

### 1.7 「插件 = 微型主程序」的目录形态

讨论中达成共识。理想插件目录结构：

```
plugins/<name>/
├── plugin.py            ← 入口（必有）
├── gui_page.py          ← 业务自带 UI（可选）
├── platform/            ← 业务自带平台能力（可选，仅本插件用的小东西）
└── assets/              ← 业务自带资源（可选）
```

`platform/` vs `companion/platform/` 的判定标准：**会有第二个业务调它吗？**
- 会 → 升格到主程序 `platform/`（如 `geoip_weather`）
- 不会 → 留在插件目录（如 `plugins/notif/win_notifications.py`）

当前 4 个插件中：
- `notif/` ✅ 已是终态（plugin.py + win_notifications.py）
- `gomoku/` ✅ 接近终态（plugin.py + gui_page.py）
- `weather/` 单文件，复用 `companion/platform/geoip_weather`
- 删除的 `music_proxy/` 不再讨论

### 1.8 平台压力测试：造 2 个 app 不改任何基础设施

落地最后两步前，决定**先验证整体平台是否成熟**——造 2 个新 app，要求**0 行 C / 0 行 prelude / 0 行主程序 / 0 行 SDK 改动**。如果通过了，整个动态 app 项目算正式毕业。

选 2 个互补维度：

| App | 维度 | 验证目标 |
|---|---|---|
| `pomodoro_pkg` | 纯本地，无 PC | 设备端零改动，纯 JS 平台能否独立做出有用 app |
| `tictactoe_pkg` | 双向 BLE + AI 插件 | 加新 PC 插件时只动 plugins/ 目录 |

---

## 2. 落地清单

### 2.1 边界整理（4 个 PR 量级的活，一次提交）

| 操作 | 文件 |
|---|---|
| `git mv` | `tools/companion/shared/` → `tools/companion/platform/` (5 个文件) |
| `git mv` | `tools/companion/plugin_sdk.py` → `tools/companion/plugin_sdk/__init__.py`（升级为包） |
| 新增 | `tools/companion/plugin_sdk/platform.py`（re-export geoip_weather） |
| 新增 | `tools/companion/plugin_sdk/gui.py`（re-export theme / widgets） |
| 新增 | `tools/companion/platform/music_library.py` （77 行，下沉自 music.py） |
| 修改 | `platform/archive_org.py` 加 `search_with_mp3()` 高层函数 |
| 重写 | `gui/pages/music.py` 548 → 494 行，纯渲染 |
| 修改 | 5 个 native provider 的 `from ...shared.*` → `from ...platform.*` |
| 修改 | 4 个 plugin 的 import 全部走 SDK 门面 |
| 修改 | `gui/pages/notify.py` 文件头加定位注释（避免与 plugins/notif 混淆） |

### 2.2 散文件归位

| 操作 | 文件 |
|---|---|
| `git mv` | 5 个 .py + 1 个 .txt → `tools/scripts/` |
| `git mv` | `tools/dynapp_bridge_test.py` → `tools/dynapp_sdk/examples/bridge_test.py` |
| 修改 | 4 处 `Path(__file__).parent.parent` → `parents[2]` |
| 修改 | 5 处 docstring 命令示例（`tools/x.py` → `tools/scripts/x.py`） |
| 修改 | 根 `README.md` 2 处提及 |

### 2.3 文档新增

| 文件 | 内容 |
|---|---|
| `tools/README.md` | 新增。4 子目录定位 / 启动命令 / 依赖关系图 / "想加新功能去哪" 速查表 |
| `tools/plugins/README.md` §0 | 新增。「什么时候需要写插件」判定规则 |

### 2.4 删除

| 文件 |
|---|
| `tools/plugins/music_proxy/`（整目录） |
| `plugin_sdk/platform.py` 移除 smtc re-export |

### 2.5 平台修复（造 settings_pkg 时暴露的真 bug）

用户上传后才发现 settings_pkg layout 错乱（"页面挤中间一小块"），追查发现 `UI.card` 缺两个默认值：

| 缺失 | 现象 | 修复 |
|---|---|---|
| 默认 size | 多个 card 被 column flex 父容器拉成 0 宽 | `size: [-100, SIZE_CONTENT]` |
| 默认 flex | 单 card 内多个 listRow 全部叠在 (0,0) | `flex: 'col'` |

为支持 `SIZE_CONTENT`，新增 sentinel：

| 文件 | 改动 |
|---|---|
| `dynamic_app/dynamic_app_ui_styles.c` | `resolve_size()` 加 `-32768 → LV_SIZE_CONTENT` |
| `dynamic_app/dynamic_app_natives.c` | 暴露 `sys.size.CONTENT = -32768` |
| `dynamic_app/scripts/prelude.js` | 顶部声明 `SIZE_CONTENT`，通过 `UI.SIZE_CONTENT` 暴露；`UI.card` 用上 |

这是本轮**唯一**的设备端改动。意外的"边界整理副作用"，但价值很高——之后所有用 `UI.card` 的页面都不会再踩这个坑。

### 2.6 平台压力测试：2 个新 app

#### App #1：番茄钟（pomodoro_pkg）

```
dynamic_app/scripts/pomodoro_pkg/
├── main.js          （310 行）
└── manifest.json
```

特性：
- 状态机 `idle → focus → short_break / long_break → focus → ...`
- `setInterval(1000)` 1Hz 倒计时，阶段结束自动 pause + toast 提示
- Router 多级页：home（大字倒计时 + 4 按钮）/ settings（专注/短休/长休/长休频率四个 +/- 调节器）
- `sys.app.saveState` 持久化 settings + 已完成番茄数
- `Router.onLeave('settings')` 回首页时落盘
- `UI.modal` 二级确认重置

**0 基础设施改动**。pomodoro_pkg 整个目录新增就完事。

#### App #2：井字棋（tictactoe_pkg）+ AI 插件

```
dynamic_app/scripts/tictactoe_pkg/
├── main.js          （260 行）
└── manifest.json

tools/plugins/tictactoe/
├── plugin.py        （200 行 AI 引擎 + 状态广播）
└── gui_page.py      （175 行 只读监控面板）
```

协议设计：

```
ESP → PC  { type:"hello" }                   握手（启动 + 重连）
ESP → PC  { type:"move",  body:{ r, c } }    玩家落子（X）
ESP → PC  { type:"reset" }                   请求新局
PC → ESP  { type:"ready" }                   AI 已就绪
PC → ESP  { type:"move",  body:{ r, c } }    AI 落子（O）
PC → ESP  { type:"reset_ack" }               AI 同意重开
```

AI 实现：启发式 1-step lookahead（自己能赢→走 / 对手能赢→堵 / 中心 → 角 → 边）。**不做 minimax 全局最优**，留点空间让人偶尔能赢。

GUI 解耦：plugin 每次状态变化 emit `bus("tictactoe:state", snapshot)`，gui_page **完全被动订阅**——这意味着 AI 可以无 GUI 跑（gui_page.py 删了不影响业务）。

**0 基础设施改动**。新插件目录 `plugins/tictactoe/` 是唯一 PC 端新增。

### 2.7 平台压力测试中暴露的小坑（不算 bug）

| 坑 | 我犯的错 | 解决 |
|---|---|---|
| `setTimeout` 不存在 | 写 tictactoe 时假设有 | 改用 `setInterval` 自管周期 |
| `ble.on(type, fn)` 回调收 msg 不是 body | 写错签名 | 改成 `function (msg) { var body = msg.body; ... }` |
| `pillBtn` 没 label id | 改不到主按钮文字 | 自造 `h('button', { id: 'btnRun' }, [h('label', { id: 'btnRunLbl' })])` |
| `UI.statusBar` 默认 60 px 高（44 + 16 内 pad） | tictactoe 内容稠密时挤不下 | tictactoe 干脆删 statusBar |
| `flexAlign[0]: 'start'` 让内容挤在顶部 | 没用上垂直居中 | 改 `'center'` |

这些都是**我以为有但实际没有 / 文档不一致**的小问题。每个都是平台未来可改进的方向，但本轮不动 prelude（保持"零基础设施"承诺）。

---

## 3. 端到端验证

### 3.1 边界整理后

```
$ python -m companion       # GUI 启动正常
loaded 4 plugins
plugins: ['gomoku', 'notif', 'tictactoe', 'weather']
gui pages: [('gomoku', '五子棋'), ('tictactoe', '井字棋')]
```

`grep "from companion\." tools/plugins/` 只剩 7 行，全是 `companion.plugin_sdk*`，**0 处穿透到内部**。

### 3.2 tictactoe AI 端到端测试

```python
# 模拟玩家走 (0,0) → AI 中心 → 玩家走 (1,0) 形成两子威胁 → AI 必须堵 (2,0)
hello → ready
move (0,0) → move (1,1)    ← AI 走中心（启发式）
move (1,0) → move (2,0)    ← AI 正确堵截 ✓
reset    → reset_ack
```

### 3.3 平台压力测试的最终成绩

```bash
$ git status --short
?? dynamic_app/scripts/pomodoro_pkg/      # 新增
?? dynamic_app/scripts/tictactoe_pkg/     # 新增
?? tools/plugins/tictactoe/               # 新增
```

**只有 3 个 untracked 目录，0 个已追踪文件被修改**。平台目标完美达成：**新增动态 app 不改任何基础设施代码**。

---

## 4. 几次踩坑修复

### 4.1 settings_pkg layout 错乱（最大一坑）

**现象**：进入 settings_pkg 后所有内容堆中央一小块。

**根因**：LVGL 默认值 + `UI.card` 缺默认值的组合：
1. `card` 没默认 `size` → `LV_SIZE_CONTENT`，按子内容收缩
2. 子是 listRow `size: [-100, 48]`，希望按父 100% 撑宽
3. 形成循环依赖：listRow 找父宽 → 父等子算尺寸 → fallback 到最小
4. 父 `flex: 'col'` 默认 cross_align CENTER，所有 card 横向居中那一窄条

**修复 1（第一轮）**：给 `UI.card` 默认 `size: [-100, SIZE_CONTENT]`，断开循环。修完发现还有问题——

**现象 2**：列表只显示 1 行，后面 3 行不见了。

**根因 2**：card 自己默认没 layout，4 个子 listRow 全部堆叠在 (0,0)，z-order 高的盖住下面的。

**修复 2**：给 `UI.card` 默认 `flex: 'col'`。

**教训**：iOS 卡片的语义本来就是"撑满宽 + 垂直堆叠"。LVGL 默认值几乎全错（按内容收缩 + 无 layout），SDK 的 `UI.card` 必须把这两个默认值写对，不能依赖业务每次手动传。

### 4.2 tictactoe 设备端布局超出屏幕

**现象**：tictactoe 主页底部按钮被切掉。

**第一次诊断**：以为是 cell/gap 太大，逐步缩到 cell=50、row=52、gap=4。还是超。

**真正根因**（用户揪出来的）：`UI.statusBar` 自己 44 高，内部还加 SP_LG/SP_SM 的 pad，**实际占 ~60 px**。"井字棋"标题对这个 app 是冗余信息（侧边栏菜单已经告诉用户在哪个 app），**整个 statusBar 应该删掉**。

**最终修复**：删 statusBar，statusLbl 用 'title' 字体顶到屏幕顶部，flexAlign 改 `'center'` 让内容垂直居中。

**教训**：UI 组件的"标准头部"在 iOS-like 大屏 OK，在 268 px 高的小屏是**奢侈**。SDK 默认值不能假设屏幕大小。

### 4.3 plugin_sdk 升级为包后 `from .bus` 失效

**现象**：`plugin_sdk/__init__.py` 里原本有 `from .bus import EventBus`（在单文件时代是 `companion.bus`），升级为包后 `.bus` 变成 `plugin_sdk.bus`，找不到。

**修复**：改 `from ..bus import EventBus`。

**教训**：单文件 → 包升级时，相对 import 要 +1 层。这个本来就是已知陷阱，但被我忘了，一启动就崩。

### 4.4 dispatch 没有事件循环时 async 插件崩

**现象**：写脚本测 tictactoe 时直接 `pm.dispatch_message(...)`，async on_message 报 `no running event loop`。

**修复**：测试代码包进 `asyncio.run(main())`。生产代码本来就在 asyncio.runner 里跑，不受影响。

**教训**：async plugin 的测试必须在事件循环里。

---

## 5. 文件改动总数（量化）

### 边界整理 + 散文件归位

- 新建：4 个文件（plugin_sdk/platform.py / plugin_sdk/gui.py / platform/music_library.py / tools/README.md）
- `git mv`：12 个文件（保留改名历史）
- 修改：13 个 .py（5 native provider + 4 plugin + GUI 2 个 + main.py + 1 个 README）
- 删除：1 个目录（music_proxy）

### 平台修复

- 修改：3 个文件（dynamic_app_ui_styles.c / dynamic_app_natives.c / prelude.js）

### 平台压力测试

- 新建：5 个文件（2 manifest + 2 main.js + 1 plugin.py + 1 gui_page.py，共 ~950 行业务）
- 修改：0 个基础设施文件

---

## 6. 设计要点回顾

### 6.1 命名比搬家更值钱

`shared/` 改名 `platform/` 这一步只动 1 行 git，却让"伪 shared" / "孤儿"等评价瞬间消失。**`shared` 的语义假设是"被多方共享"，`platform` 的语义是"对接外部能力"**——后者更准确，且不依赖"调用方数量"这个动态指标。

广义教训：**目录命名是个产品决策**，不是技术决策。错的名字会让 review 者每次都问"这真是 shared 吗"。

### 6.2 SDK 是「接口承诺」，不是「目录组织」

讨论中用户问"companion/ 里还有什么可以抽 SDK"，我列了 5 个候选 + 全部驳回的理由。核心规则：

> SDK 触发条件 = **是否有至少 2 个不同消费者想 import 同一份代码**。仅仅"看起来通用"不构成抽 SDK 的理由——必须有第二个作者或第二个项目真的在用它。

当前 3 个 SDK（dynapp_sdk / dynapp_uploader / plugin_sdk）的 2 个消费者都在仓库内（companion 和 plugins，或 companion 和外部脚本）——这才合格。

### 6.3 「插件 = ESP32 动态 app 的 PC 代理人」

这条是用户提出的概念性洞察，把"什么时候需要插件"讲透了。本来含混的"为什么有 plugins/" 立刻有了硬规则：

- 没动态 app 找它 → 不该存在（删 music_proxy）
- 主程序自己的功能（music 文件夹同步、文件上传）→ 不是插件，是 GUI 页

这个规则写进 `plugins/README.md` §0 后，未来加新功能时**第一步会自然问对的问题**——避免再出现孤儿。

### 6.4 平台压力测试 > 单元测试

"造 2 个 app 不改基础设施"是这一轮最有说服力的验收方式。比起跑 100 个 unit test，**让一个新 app 走完"manifest → 上传 → 加载 → 跑通"全流程**更能暴露真实缺口：

- `setTimeout` 不存在
- `ble.on` 签名文档不一致
- `pillBtn` 没 label id
- `UI.statusBar` 在小屏挤
- `UI.card` 默认值缺失（最大一坑）

这些坑都是**业务作者第一次写 app 时必踩**的，靠我们自己做几个 app 主动踩，比等真用户踩好。

### 6.5 Bug 在被发现的瞬间最便宜

`UI.card` 那个 size + flex 双默认值 bug，是在用户**第一次打开 settings_pkg 看到错乱布局**的瞬间被发现的。如果当时不修，后续每个用 `UI.card` 的页面都会重蹈覆辙——而每个新人作者都会以为"是我用错了"，自己绕开（手动传 size + flex），bug 永远潜伏在 SDK 里。

修复成本：5 行 C + 5 行 JS。**收益**：今后所有 `UI.card` 用户开箱即用。

---

## 7. 后续 / 不在本轮范围

- ❌ `UI.statusBar` 加 `compact` 选项（小屏占空间过多）—— 等真有第 2 个 app 抱怨再做
- ❌ `setTimeout` 在 prelude 实现（基于 setInterval 包一层）—— 一行的事，但本轮不动 prelude
- ❌ `ble.on(type, fn)` 改名 `onType` 或让 fn 收 body —— 文档/命名问题，等达成共识再改
- ❌ 插件 hot reload（importlib.reload + bus 订阅清理 + GUI 重挂）—— 单独大任务
- ❌ 写一份完整的「动态 app 开发者指南」—— 已积累足够素材，下一轮做
- ❌ 把"三层职责"规则写进 lint 检查 —— 当前靠文档约定即可

---

## 8. 关键经验沉淀

### 8.1 「重构没动力时」就靠造业务驱动

PC 端插件化重构刚做完时，本来想直接造新 app。但 review tools/ 时发现一堆边界债——是 sit on 不动还是顺手清掉？选择清掉。结果：清边界又暴露 settings_pkg 的 `UI.card` bug，修完顺势造 2 个新 app 验证整个平台——**4 件事在一次重构里链式产出**。

教训：**重构和业务不是对立的，互为驱动**。重构会暴露 bug、bug 修完平台变稳、平台稳了造业务才不踩坑、造业务又暴露 SDK 缺口反哺重构。

### 8.2 「文档应该写约束，而不是写流程」

`plugins/README.md` 原来全是"怎么写一个插件"的流程。本轮加的 §0「什么时候需要写插件」是**约束**——它不教你怎么做，它教你**什么时候不该做**。

约束比流程更值钱，因为：
- 流程每个项目模板里都有，复制就能用
- 约束是项目特有的"为什么"，必须本项目作者亲自总结

这条规则我应该回头看其他文档，把"流程"段落里能升华成"约束"的部分都拎出来。

### 8.3 「LVGL 默认值是陷阱，不是合理预期」

修 `UI.card` 时发现 LVGL 默认 `lv_obj_t` 行为：
- size：`LV_SIZE_CONTENT`（按子收缩）
- layout：无（子叠加）
- align：`LV_ALIGN_CENTER`（居中）

**三个默认值在容器场景全是错的**——卡片应该撑满宽 + 垂直堆叠 + 顶对齐。SDK 必须把对的默认值写进 `UI.card`，不能信"LVGL 默认 = 合理默认"。

教训泛化：**封装框架时，默认值是产品决策，不是"继承底层"**。每个默认值都应该问"业务 90% 的场景是什么"，对那个 90% 写默认。

### 8.4 git mv 比改路径重要

整轮重构 12 个 `git mv`（shared → platform / plugin_sdk.py → 包 / 5 个散文件 → scripts/ / bridge_test 进 examples）。`git log --follow` 仍能追溯所有文件的完整历史。

如果未来某天因为 `platform/smtc.py` 出 bug 要 blame，**仍能精确找到 2026-04 写它时的提交**——文件移过 1 次但历史不丢。

### 8.5 「作者的隐含假设」就是未来的 bug

`UI.card` 默认 flex/size 缺失，settings_pkg 作者**不知道自己依赖了"card 自动垂直堆叠"**——但他用法里全是这个假设。一旦 SDK 默认行为不匹配，立刻崩。

修复方式不是"教作者每次显式传 flex"，而是**让 SDK 默认行为匹配作者的隐含假设**。这条规则适用于所有 UI 组件库：**默认值要承载约定俗成的语义**。

---

## 9. 一句话总结

**这一轮把 PC 端边界从「主程序里再无业务字符串」推进到「插件作者只看 plugin_sdk」**——SDK 门面（platform / gui re-export）锁住了所有反向依赖，三层职责（providers / gui / plugins / platform）名实相符；顺手通过造 2 个新 app（番茄钟纯本地 + 井字棋人机对战）证明了**动态 app 平台真正达到「加新 app 不改任何基础设施代码」**的目标，并在过程中修复了一个潜伏的 `UI.card` bug。

下一阶段可以专心写新 app 了。
