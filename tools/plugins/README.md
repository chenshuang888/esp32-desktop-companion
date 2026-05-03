# tools/plugins/ —— ESP32 Companion 插件作者指南

> 给"PC 端配套动态 app 的服务"找到一个独立、不污染主程序的家。
> 新增动态 app 配套 PC 代码 = 加一个 `plugins/<name>/` 目录 + 在 GUI 上点"刷新插件"。
> 主程序里再也不会出现某个特定 app 的字符串。

---

## 0. 先问自己：这事真的需要插件吗？

**插件存在的唯一理由**：ESP32 端某个动态 app 需要 PC 帮它做 BLE 收发之外的事。

| 场景 | 需要插件？ |
|---|---|
| 纯本地动态 app（闹钟 / 计算器 / 2048 / 涂鸦 / 计时器） | ❌ |
| 只用 bridge 透传调试（echo 类） | ❌（哑总线就够） |
| 动态 app 要 PC 拉网络数据（HTTP API） | ✅ |
| 动态 app 要 PC 抓系统状态（通知 / 媒体 / 网速） | ✅ |
| 动态 app 要联机（PC 当中继路由器） | ✅ |

反过来：**PC 端独立功能不是插件**——比如音乐文件夹同步、文件上传。它们没有 ESP32 动态 app 跟它们通信，是主程序自己的 GUI 页。

一句话：**插件 = ESP32 动态 app 在 PC 端的「代理人」**。没有动态 app 找它，它就不该存在。先在 ESP32 端写脚本、确认有 PC 配合需求，再回头加 `plugins/<name>/` —— 不要"提前做好备用"。

---

## 1. 一个最小插件长什么样

```
plugins/
└── foo/
    └── plugin.py
```

```python
# plugins/foo/plugin.py
from companion.plugin_sdk import Plugin

class FooPlugin(Plugin):
    plugin_id = "foo"          # 唯一 id（侧边栏标识 / 内部路由 key）
    title     = ""              # 空 = 没 GUI 页；非空则显示在侧边栏
    bind_app  = "foo_pkg"       # 绑死动态 app id；None = 通用服务

    async def on_message(self, msg):
        # 收到 from=foo_pkg 的所有消息（已按 bind_app 过滤）
        self.log.info("got: %s", msg)
        await self.tx("ack", body={"ok": True})
```

放好之后启动 companion，或者运行中点侧边栏底部的"🔄 刷新插件"按钮，它就被加载了。

---

## 2. SDK 接口（companion.plugin_sdk.Plugin）

### 2.1 类属性（子类必填 / 可选）

| 字段 | 必填 | 说明 |
|---|---|---|
| `plugin_id` | ✅ | 唯一 id，建议小写（如 `"gomoku"`、`"weather"`）|
| `title` | 选 | 侧边栏显示名；**空字符串 = 不在 GUI 出现** |
| `bind_app` | 选 | 设备端 app_id（动态 app manifest.id）。`None` = 通用服务（接收所有 from 的消息）|

`plugin_id` 和 `bind_app` 是两件事：前者是 PC 端插件标识，后者是设备端 app 标识。两者**通常不一样**（如 `plugin_id="gomoku"` / `bind_app="gomoku_pkg"`）。

### 2.2 生命周期方法（按需覆盖）

```python
def       on_load(self):              # 实例化后立即调（同步，BLE 还未连）
def       on_unload(self):            # 卸载时（同步资源清理）
async def on_connect(self, addr):     # 每次 BLE 连上设备
async def on_disconnect(self):        # 每次 BLE 断开
async def on_message(self, msg):       # 收到匹配 bind_app 的消息（已过滤）
```

`msg` 形如 `{"from": "...", "type": "...", "body": {...}}`。

### 2.3 工具方法

```python
self.tx(mtype, body=None)               # 发给 self.bind_app
self.tx_to(app_id, mtype, body=None)     # 发给任意动态 app
self.is_connected() -> bool
self.create_task(coro)                  # 后台 task；unload 时自动 cancel
self.bus                                # 全局 EventBus（订 / 发自定事件）
self.log                                # logging.Logger 实例
```

### 2.4 GUI 页（可选）

```python
def make_gui_page(self, master, app):
    """返回 ctk.CTkFrame；None 表示不提供 GUI 页（默认）。"""
    from .gui_page import FooPage
    return FooPage(master, app, plugin=self)
```

GUI 页的实现自由——继承 `ctk.CTkFrame` 即可。可以从 `companion.gui.theme` 导色板、`companion.gui.widgets` 导通用小组件。

---

## 3. 三种典型插件骨架

### 3.1 通用服务（不绑特定 app）—— 例：weather

```python
class WeatherPlugin(Plugin):
    plugin_id = "weather"
    title     = ""
    bind_app  = None         # 通用服务

    async def on_message(self, msg):
        if msg.get("from") != "weather" or msg.get("type") != "req":
            return
        # 业务...
        self.tx_to("weather", "data", body={...})
```

### 3.2 PC → 设备单向推送 —— 例：notif

```python
class NotifPlugin(Plugin):
    plugin_id = "notif"
    title     = ""
    bind_app  = "notif_pkg"

    async def on_connect(self, addr):
        # 启动 Windows 通知监听，每条新通知 → tx("add", body)
        ...

    async def on_disconnect(self):
        # 停掉监听
        ...
```

### 3.3 双向 + GUI 页 —— 例：gomoku

```python
class GomokuPlugin(Plugin):
    plugin_id = "gomoku"
    title     = "五子棋"
    bind_app  = "gomoku_pkg"

    def on_load(self):
        # GUI 落子时 emit("gomoku:tx", (mtype, body))，本插件转发到 BLE
        self.bus.on("gomoku:tx", lambda p: self.tx(p[0], p[1]))

    async def on_message(self, msg):
        # 设备消息透到 bus，GUI 页订阅
        self.bus.emit("gomoku:rx", (msg.get("type"), msg.get("body") or {}))

    def make_gui_page(self, master, app):
        from gui_page import GomokuPage
        return GomokuPage(master, app)
```

---

## 4. 私有 helper（npm-style 同目录）

如果你的插件需要额外的 .py 文件（比如平台相关的 helper），直接放到 plugin 目录里：

```
plugins/notif/
├── plugin.py
└── win_notifications.py     ← 私有 helper
```

```python
# plugin.py
from win_notifications import WinNotificationMonitor   # 同目录
```

**为什么不用 `from .x import y`**？因为插件不是常规 Python package（它是 importlib 动态加载的单文件），相对 import 不可用。PluginManager 会把插件目录加入 `sys.path`，所以**直接用 module 名**即可。

---

## 5. 不能做 / 不该做的事

### 5.1 不要 import companion 内部 helper

允许：
- ✅ `from companion.plugin_sdk import Plugin`（SDK 入口）
- ✅ `from companion.gui.theme import ...`（GUI 写页时用）
- ✅ `from companion.shared.X import ...`（**当前阶段允许**，但理想上 helper 应进插件目录）

避免：
- ❌ `from companion.core import Companion`（绕过 SDK 直接操作底层）
- ❌ `from companion.providers... import ...`（插件不该感知 provider 体系）

### 5.2 不要直接调 BLE

走 `self.tx()` / `self.tx_to()`。走原生 BleakClient 会绕开 quiesce / write_lock 机制。

### 5.3 不要假设运行平台

`smtc` / `winsdk` 这些 Windows-only 的依赖要在 `on_connect` 里检查 `sys.platform`，不要在 module 顶部 import。否则在 macOS/Linux 跑会直接 ImportError。

### 5.4 不要在 plugin.py 顶部做耗时操作

加载阶段不要做网络请求 / 启线程 / 读盘。把这些放到 `on_load` 或 `on_connect` 里。

---

## 6. 插件目录搜索路径

加载顺序：
1. **仓库内置**：`tools/plugins/`（开发期，跟代码走）
2. **用户级**：
   - Windows: `%APPDATA%/esp32_companion/plugins/`
   - 其它: `~/.esp32_companion/plugins/`

打包后（PyInstaller）用户可以把第三方插件放到上面用户级路径，重启 / 刷新即可加载。

---

## 7. 限制（当前实现的明确边界）

- ❌ 修改老插件代码后**不能 hot reload**——重启脚本
- ❌ 不支持插件依赖管理（`plugin.json` / `requires`）
- ❌ 不支持远程下载 / 在线市场
- ❌ 不支持沙箱隔离（插件能 import 任意 module，请只用可信插件）

这些都属于"未来按需做"的范围，不在当前阶段。

---

## 8. 3 个示例插件（见仓库）

- `tools/plugins/weather/` —— 通用服务，bind_app=None
- `tools/plugins/notif/` —— PC → 设备单向，绑 notif_pkg
- `tools/plugins/gomoku/` —— 双向 + GUI 页，绑 gomoku_pkg

写新插件可以直接参考这三个的代码风格。
