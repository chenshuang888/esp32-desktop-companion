# PC 端插件化重构 工作日志

**日期**：2026-05-02
**分支**：feat/optimize_page
**作者**：ChenShuang + Claude

---

## 0. 起因

固件端动态 app 接口已经稳定到"加新 app 不动 C 代码"。但 PC 端不行——每加一个动态 app（比如五子棋），都要：
1. 改 `bridge_provider.py` 注册新的 router handler
2. 改 `gui/app.py` 加侧边栏入口
3. 新建 `gui/pages/<app>.py` 写 GUI 页
4. 有时还要改 `shared/` 加平台 helper

也就是说**设备端动了一行 C，PC 端要改 4-5 个文件**。这违反了"动态 app = 不改主程序"的本意。

更深层的问题：以后这个 PC 脚本要打包成上位机软件给用户。**用户加新 app 时不可能让他重新编译 PC 程序**——必须做成插件化，加目录就生效。

本轮目标：**让 PC 端也变成"动态 app 配套服务 = 一个独立插件目录"**，主程序里再也找不到 "gomoku" / "weather" 这种字符串。

---

## 1. 关键决策

### 1.1 三层架构观

把 PC 端按"会不会改"重新分层：

```
第一层：sys.* 风格的"系统调用"     ← 不改
第二层：companion 主程序 + plugin SDK  ← 偶尔扩
第三层：plugins/<app>/             ← 业务全在这里
```

PC 端的"第一层"对应：
- `dynapp_sdk` / `dynapp_uploader`（独立 SDK 库）
- `companion.plugin_sdk`（Plugin 基类）

PC 端的"第三层"就是 `tools/plugins/`。新增动态 app 只动这里。

### 1.2 原生 app 配套服务**不**插件化

考虑过把 5 个原生 provider（time/weather/notify/system/media）也插件化。最终决定**不**：

- 它们是固件原生 BLE service 的对偶端，配套关系强绑定
- 5 个 provider 已经稳定多个版本，没动力翻新
- 插件化后接口要包装一层，反而增加复杂度

但 bridge_provider 里的 weather handler / SMTC music handler 还是抽出来插件化了——因为这些是"动态 app 通用服务"，不绑特定原生 BLE service。

### 1.3 plugin_id ≠ bind_app

- `plugin_id`：PC 端插件的唯一标识（如 `"gomoku"`）
- `bind_app`：设备端动态 app 的 manifest.id（如 `"gomoku_pkg"`）

它们经常不一样，所以**必须分开两个字段**。考虑过自动 `bind_app = plugin_id + "_pkg"` 推断，pass —— 命名不规则的就会出 bug，明确比省字段重要。

`bind_app=None` 表示**通用服务**，接收所有 from 的消息（如 weather / music_proxy）。

### 1.4 bridge_provider 砍成"哑总线"

原 227 行。改造后：
- ❌ 不再 `register("weather", "req", ...)` / `register("music", ...)` / `register("gomoku_pkg", ...)`
- ❌ 不再 import `dynapp_sdk.router.Router`
- ❌ 不再 import `geoip_weather` / `smtc` / `win_notifications`
- ✅ 收 a3a30003 → `pm.dispatch_message(msg)`
- ✅ 监听 `bus("bridge:tx")` → BLE write

97 行。所有业务转移到 plugins/。

### 1.5 插件路径双源

```
1. tools/plugins/                         仓库内置（开发用）
2. %APPDATA%/esp32_companion/plugins/     用户级（打包后用户加）
   或 ~/.esp32_companion/plugins/         非 Windows
```

PyInstaller 打包时用 `sys._MEIPASS` 找内置路径。

### 1.6 热加载只支持加，不支持改

- ✅ "刷新插件"按钮：扫盘发现新插件 → 加载 → 重建侧边栏
- ❌ 修改老插件 → 重载（importlib.reload 在复杂场景容易踩 GC / 单例 / 闭包等坑）

修改老插件 = 重启脚本。这是**有意取舍**。

### 1.7 providers/ 顺手分子目录

```
providers/
├── base.py
├── native/    ← 5 个原生配套
└── dynapp/     ← bridge + upload 基础设施
```

平铺 8 个文件难以一眼分清"哪些是原生 / 哪些是动态"。分子目录让阅读时立刻有上下文。

### 1.8 GUI 页生命周期 = 插件生命周期

`make_gui_page(master, app)` 返回 None 表示插件不要 GUI 页。`title=""` 也不出现。两者是 AND 关系。

`Plugin._cancel_all_tasks()` 在 unload 时清掉所有 `self.create_task()` 创建的 task —— 防止插件留下幽灵任务。

---

## 2. 落地清单

### 2.1 新增（4 文件）

| 文件 | 行数 | 职责 |
|---|---|---|
| `companion/plugin_sdk.py` | 110 | Plugin 基类 + 平台注入字段 |
| `companion/plugin_manager.py` | 220 | discover / instantiate / dispatch |
| `tools/plugins/README.md` | 240 | 插件作者指南 |
| `docs/动态app_PC端插件化重构_工作日志.md` | 本文件 | — |

### 2.2 移动 / 重命名

| 原 | 新 |
|---|---|
| `providers/time_provider.py` | `providers/native/time_provider.py` |
| `providers/weather_provider.py` | `providers/native/weather_provider.py` |
| `providers/notify_provider.py` | `providers/native/notify_provider.py` |
| `providers/system_provider.py` | `providers/native/system_provider.py` |
| `providers/media_provider.py` | `providers/native/media_provider.py` |
| `providers/bridge_provider.py` | `providers/dynapp/bridge_provider.py` |
| `providers/upload_provider.py` | `providers/dynapp/upload_provider.py` |
| `companion/shared/win_notifications.py` | `tools/plugins/notif/win_notifications.py` |
| `companion/gui/pages/gomoku.py` | `tools/plugins/gomoku/gui_page.py` |

`git mv` 保留改名历史。

### 2.3 新增插件（4 个）

| 插件 | bind_app | 类型 | 行数 |
|---|---|---|---|
| `tools/plugins/weather/plugin.py` | None | 通用服务 | 50 |
| `tools/plugins/music_proxy/plugin.py` | None | 通用服务 + Windows SMTC | 100 |
| `tools/plugins/notif/plugin.py` | `notif_pkg` | PC → 设备单向 | 60 |
| `tools/plugins/gomoku/plugin.py` | `gomoku_pkg` | 双向 + GUI 页 | 50 |

### 2.4 修改（5 文件）

- `providers/dynapp/bridge_provider.py` —— 砍成哑总线（227 → 97 行）
- `providers/dynapp/upload_provider.py` —— 路径深度修正（多一级 dirname）
- `companion/__main__.py` —— 注入 PluginManager + 退出时 unload_all
- `companion/gui/app.py` —— `_collect_page_defs` + `_populate_pages` + 刷新插件按钮
- `tools/plugins/gomoku/gui_page.py` —— theme/widgets import 改为绝对路径

### 2.5 修复 import 路径

`providers/native/*` 和 `providers/dynapp/*` 的相对 import 全部 +1 级（`..constants` → `...constants`），`from .base` → `from ..base`。批量脚本处理。

---

## 3. 端到端验证

跑了 4 个 sanity test：

```python
# 1. 所有模块 import 不报错 ✅
import companion.plugin_sdk, companion.plugin_manager
import companion.providers.{native,dynapp}.*

# 2. 4 个内置插件成功 discover + load ✅
loaded 4 plugins
plugins: ['gomoku', 'music_proxy', 'notif', 'weather']
gui pages: [('gomoku', '五子棋')]

# 3. 设备 → 插件路由（按 bind_app 过滤）✅
pm.dispatch_message({'from':'gomoku_pkg','type':'move','body':{'r':6,'c':6}})
# → bus.emit('gomoku:rx', ('move', {'r':6,'c':6}))

# 4. GUI → 插件 → BLE（反向通道）✅
bus.emit('gomoku:tx', ('move', {'r':5,'c':5}))
# → tx_func('gomoku_pkg', 'move', {'r':5,'c':5})
```

`_build_companion(...)` 跑通：
- 7 个 provider 注册成功
- 4 个插件加载成功

---

## 4. 几次踩坑修复

### 4.1 sed 脚本批量改 import 时把 `.base` 误连改两次

第一遍 `from \.base ` → `from ..base `，第二遍按 `^from \.\.` 加深一级，结果 `..base` 又变成 `...base`。修复：再跑一遍把 `from ...base` 还原成 `from ..base`。

教训：批量改 import 用 regex 不安全，应该一次处理完。最好用 `libcst` 或 `rope` 这种 AST 工具。但这次量小，手 grep 验证就够。

### 4.2 upload_provider 移动后 sys.path 计算错了一级

```python
_TOOLS_DIR = dirname(dirname(dirname(__file__)))   # 原：3 级
```

文件从 `providers/upload_provider.py` 移到 `providers/dynapp/upload_provider.py`，深度多一级，要改 4 次 dirname。补了。

### 4.3 插件需要 import companion.X，必须保证 tools/ 在 sys.path

`python -m companion` 启动时 `tools/` 自动在 sys.path（cwd），但插件目录加进 sys.path 是为了**插件内部相对 import**。两件事不一样。

修复：plugin_manager.discover_and_load() 第一步就调 `_ensure_tools_in_syspath()` 保险一遍。

### 4.4 gomoku gui_page 移动后相对 import 失效

原 `from ..theme import ...`（相对 `gui/pages/`），现在文件在 `plugins/gomoku/`，相对路径不对。改为绝对路径 `from companion.gui.theme import ...`。

教训：插件需要导通用 GUI 资源，必须用绝对路径——它不是 companion 内部模块，相对路径无法表达。

---

## 5. 设计要点回顾

### 5.1 "插件可以 import companion 什么"

明确允许：
- `companion.plugin_sdk` —— SDK 入口
- `companion.gui.theme` / `companion.gui.widgets` —— GUI 资源（写 GUI 页时）
- `companion.shared.*` —— 通用 helper（**当前阶段允许**，理想是 helper 进插件目录）

明确不允许：
- `companion.core` / `companion.providers.*` —— 主程序内部，绕过 SDK

将来可能加 `lint` 检查这条规则；现在靠文档约定。

### 5.2 "插件运行时能拿到什么"

构造时由 PluginManager 注入：
- `self.bus`：全局 EventBus
- `self.log`：`logging.getLogger("plugin.<plugin_id>")`
- `self._tx_to`：底层 BLE 发送函数（业务用 `self.tx()` / `self.tx_to()`）
- `self._is_connected_fn`：BLE 连接状态查询

**不暴露**：BleakClient、Companion、ProviderContext、Core 实例。这是有意的"插件作者只能看到 SDK，看不到主程序内部"。

### 5.3 GUI 页和插件实例的关系

GUI 页是 plugin 的"输出"——通过 `make_gui_page()` 一次性返回 ctk.CTkFrame 实例。

插件**持有 GUI 页的引用是可选的**：如果插件需要在 on_message 里调 GUI 页方法，可以在 make_gui_page 时存到 self._gui。但更常见的做法是**插件 emit bus 事件，GUI 页订阅**——解耦更彻底。gomoku 用的就是后者：

```
插件 → bus("gomoku:rx", ...) → GUI 页 .on(callback)
GUI 页 → bus("gomoku:tx", ...) → 插件 .on(callback) → tx
```

---

## 6. 文件改动数（量化）

- 新建：6 个 Python 文件 + 2 个 markdown
- 移动 / 重命名：9 个文件（git mv）
- 修改 import 路径：8 个文件
- 删除：0（保留全部历史）
- 净增代码：~700 行（其中 plugin_sdk + plugin_manager 占 330）
- 净减代码：~130 行（bridge_provider 从 227 → 97）

`bridge_provider.py` 由"业务 + 基础设施混合的 227 行"砍成"纯基础设施 97 行"，是这次最有价值的简化。

---

## 7. 后续 / 不在本轮范围

- ❌ 修改老插件后 hot reload
- ❌ 远程下载 / 在线市场
- ❌ 沙箱权限隔离
- ❌ `plugin.json` 元信息（version / requires / minSdkVersion）
- ❌ shared/ 进一步分平台 / 服务子目录（之前 P2 标记的清理，等再加文件时做）
- ❌ 把 weather/smtc 这种 helper 从 shared/ 真正搬进各自插件（当前是 plugin import shared，理论上违反"插件不依赖主程序内部"的纯洁性，但实际工作就行）

---

## 8. 关键经验沉淀

### 8.1 "插件化 = 业务和基础设施分家"
重构最大的收益不是"加新 app 不改主程序"——是 **bridge_provider 终于变成它本来该是的样子**：一个传输层。原来塞进去的 weather / SMTC / WinNotif / gomoku 全是历史包袱。

### 8.2 SDK 的"暴露面"是产品决策，不是技术决策
`Plugin` 基类暴露什么字段、方法，决定了未来插件作者的代码长什么样。少暴露 → 插件能力受限；多暴露 → 主程序内部接口被锁死。我选了 4 个能力（bus / log / tx / is_connected）+ 5 个生命周期 hook，刚好覆盖现有 4 个迁移插件的所有需求，没有冗余。

### 8.3 "插件 vs 内置 provider"的边界 = "动态 vs 原生"的边界
不要把所有 provider 都插件化（强行重构 native/* 的 5 个原生 provider 没收益）。**边界对应业务模型**：原生 BLE service 强绑业务，插件机制对它没附加价值。

### 8.4 git mv 比 cp + rm 重要
所有文件移动用 `git mv`。重构后 `git log --follow` 仍能看到完整历史，未来追溯 bug / blame 都不丢上下文。

### 8.5 端到端验证 > 单元测试
我没写 pytest。但用一段 30 行的脚本把"加载 → 路由 → 反向通道"一次性跑通——比单元测试更有说服力，因为测的是"集成在一起的真实行为"。一旦这条链路通了，单点细节出问题的概率就很低。

### 8.6 "刷新插件"按钮放在底部状态栏附近
最初想放顶部菜单，最终放底部"连接状态"上方。理由：**它是低频操作 + 非破坏性**，应该和"日志查看"这种工具型动作放一起，而不是和"切页"这种高频导航并列。这是 UI 节奏感。

---

## 9. 一句话总结

**PC 端从"硬编码业务逻辑的单体脚本"改造成"通用 BLE 总线 + 插件化业务"**——主程序里再无 "gomoku" / "weather" 字样，所有动态 app 配套 PC 代码都在 `tools/plugins/<name>/` 独立目录里维护，加目录即生效，"刷新插件"按钮支持热加载。

打包成上位机软件后，**用户加新插件 = 把目录拷到 `%APPDATA%/esp32_companion/plugins/` + 在 GUI 上点一下**——这就是这次重构最终交付给未来用户的体验。

至此，**ESP32 demo6 的"动态 app 平台"在 PC 端和设备端都达到了"加新 app 不改基础设施"的目标**。下一阶段可以专心造 app 了。
