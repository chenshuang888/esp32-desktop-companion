PC 端插件化 + 框架重构 深度规划

  先盘清楚归属边界，再给完整设计。

  ---
  1. 边界与归属（重新确认）

  留主程序（不插件化）：
  - 通用基础设施：core / bus / runner / config / constants / tray
  - 通用 provider：bridge（哑总线）、upload
  - 5 个原生 app 配套 provider：time / weather / notify / system / media
  - 内置 GUI 页：home / log / upload / notify / music
  - 原生体系内部 helper：shared/{packers, geoip_weather, smtc, toast, archive_org}

  移到插件：
  - bridge_provider 里的 weather handler → plugins/weather/（通用动态 app 服务）
  - bridge_provider 里的 music handler → plugins/music_proxy/（通用 SMTC 代理）
  - bridge_provider 里的 win_notifications + notif/add 推送 → plugins/notif/
  - bridge_provider 里的 gomoku 路由 → plugins/gomoku/
  - gui/pages/gomoku.py → plugins/gomoku/gui_page.py
  - shared/win_notifications.py → plugins/notif/win_notifications.py

  重构（位置变了，内容基本不变）：
  - providers/ 拆 native/ + dynapp/ 两个子目录

  ---
  2. 最终目录结构

  tools/
  ├── dynapp_sdk/                     不动
  ├── dynapp_uploader/                 不动
  ├── companion/
  │   ├── __init__.py
  │   ├── __main__.py                 集成 plugin_manager
  │   ├── core.py / bus.py / runner.py / config.py / constants.py / tray.py
  │   ├── plugin_sdk.py               ⭐ Plugin 基类（≈80 行）
  │   ├── plugin_manager.py           ⭐ 扫描+加载+路由（≈150 行）
  │   ├── providers/
  │   │   ├── __init__.py
  │   │   ├── base.py
  │   │   ├── native/
  │   │   │   ├── time / weather / notify / system / media _provider.py
  │   │   └── dynapp/
  │   │       ├── bridge_provider.py  砍成纯总线（≈80 行，原 227）
  │   │       └── upload_provider.py
  │   ├── shared/                     ─win_notifications
  │   └── gui/
  │       ├── app.py                  动态收集插件页
  │       └── pages/                  ─ gomoku.py
  └── plugins/                        ⭐ 新增
      ├── README.md                   插件作者指南
      ├── weather/plugin.py
      ├── music_proxy/plugin.py
      ├── notif/
      │   ├── plugin.py
      │   └── win_notifications.py
      └── gomoku/
          ├── plugin.py
          └── gui_page.py

  打包/用户级也支持：%APPDATA%/esp32_companion/plugins/（同样格式）。

  ---
  3. SDK 接口设计 — plugin_sdk.py

  class Plugin:
      # —— 子类必填 ——
      plugin_id: str = ""              # 唯一 id（如 "gomoku"）
      title:     str = ""              # 侧边栏显示名；空表示无 GUI 页

      # —— 子类可选 ——
      bind_app:  str | None = None     # 绑死动态 app id（如 "gomoku_pkg"）；
                                       # None = 通用服务（收所有消息自己过滤）

      # —— 平台注入（不要在子类里覆盖）——
      bus: EventBus
      log: logging.Logger

      # —— 生命周期 ——
      def      on_load(self):              pass   # 实例化后调用一次（无 BLE）
      def      on_unload(self):            pass   # 卸载时（清理同步资源）
      async def on_connect(self, addr):    pass   # BLE 连上
      async def on_disconnect(self):       pass   # BLE 断开
      async def on_message(self, msg):     pass   # 收到消息（按 bind_app 已过滤）

      # —— 工具方法 ——
      def tx(self, mtype, body=None):                    # 发给 bind_app
      def tx_to(self, app_id, mtype, body=None):          # 发给任意 app
      def is_connected(self) -> bool:
      def create_task(self, coro) -> asyncio.Task:        # 自动随 unload 取消

      # —— GUI（可选）——
      def make_gui_page(self, master, app):
          """返回 ctk.CTkFrame；None 表示不提供 GUI 页"""
          return None

  插件作者侧最简实例：

  # plugins/gomoku/plugin.py
  from companion.plugin_sdk import Plugin

  class GomokuPlugin(Plugin):
      plugin_id = "gomoku"
      title     = "五子棋"
      bind_app  = "gomoku_pkg"

      async def on_message(self, msg):
          # 透到 GUI bus（GUI 页订阅）
          self.bus.emit("gomoku:rx",
                        (msg.get("type"),
                         msg.get("body") if isinstance(msg.get("body"), dict) else {}))

      def make_gui_page(self, master, app):
          from .gui_page import GomokuPage
          return GomokuPage(master, app, plugin=self)

  自动发现机制（无 register 装饰器）：PluginManager 加载 plugin.py 后扫 module 里所有 Plugin 子类，自动实例化。插件作者只需 class
  定义，零样板代码。

  ---
  4. PluginManager 工作机制

  class PluginManager:
      def __init__(self, bus, on_tx, is_connected): ...

      def discover(self):
          """扫两个目录的 plugin.py，importlib 加载，收集 Plugin 子类"""

      def instantiate_all(self):
          """实例化所有发现的 Plugin 子类，注入 bus/log/tx，调 on_load"""

      def reload_discovery(self):
          """重新扫盘，加载新插件（不 reload 已加载的）"""

      def dispatch_message(self, msg: dict):
          """bridge 收到消息后路由：bind_app 匹配 → 异步调 on_message"""

      def dispatch_connect(self, addr):    # 调所有插件的 on_connect
      def dispatch_disconnect(self):       # 调所有插件的 on_disconnect

      def get_gui_pages(self) -> list[(plugin_id, title, plugin_instance)]:
          """收集所有 title 非空的插件，给 gui/app.py 用"""

  路由规则（bind_app 决定）：
  - bind_app = "gomoku_pkg" → 只收 from=gomoku_pkg 的消息
  - bind_app = None → 收所有消息（插件自己按 type 过滤，如 weather）

  ---
  5. bridge_provider 改造（核心简化）

  原 227 行 → 新 ≈80 行。所有业务移走，只剩两件事：

  class BridgeProvider(Provider):
      def __init__(self, plugin_manager): self._pm = plugin_manager

      async def on_start(self, ctx):
          # 1. 订阅 BLE notify → 解 JSON → pm.dispatch_message
          # 2. 订阅 bus "bridge:tx" (to_app, mtype, body) → BLE write
          ...

  dynapp_sdk.router.Router 不再用（plugin 自己用 bind_app + on_message 即可）。

  ---
  6. 入口集成（__main__.py）

  def _build_companion(bus, cfg_data):
      comp = Companion(bus)

      # 1. 创建 PluginManager
      pm = PluginManager(
          bus,
          on_tx=lambda to, t, b: bus.emit_threadsafe("bridge:tx", (to, t, b)),
          is_connected=lambda: comp.is_connected(),
      )
      pm.discover()
      pm.instantiate_all()

      # 2. 注册原生 provider（不变）
      comp.register(TimeProvider())
      comp.register(WeatherProvider())
      # ...

      # 3. 注册基础 provider，bridge 持有 pm 引用
      comp.register(BridgeProvider(plugin_manager=pm))
      comp.register(UploadProvider())

      # 4. bus 事件 → plugin manager 转发
      bus.on("connect",    lambda addr: pm.dispatch_connect(addr))
      bus.on("disconnect", lambda _:    pm.dispatch_disconnect())

      return comp, pm

  ---
  7. 4 个示例插件（迁移内容）

  ┌──────────────┬───────────────────────┬──────────────────────────────────────────────────────────┐
  │     插件     │         性质          │                          干什么                          │
  ├──────────────┼───────────────────────┼──────────────────────────────────────────────────────────┤
  │ weather/     │ bind_app=None         │ 收 (from=weather, type=req) → 调 get_weather() → 回 data │
  ├──────────────┼───────────────────────┼──────────────────────────────────────────────────────────┤
  │ music_proxy/ │ bind_app=None         │ 启动 SMTC 监听 + 收 music/req、music/btn → 推/控         │
  ├──────────────┼───────────────────────┼──────────────────────────────────────────────────────────┤
  │ notif/       │ bind_app="notif_pkg"  │ 启动 WinNotificationMonitor → notif_pkg/add 推送         │
  ├──────────────┼───────────────────────┼──────────────────────────────────────────────────────────┤
  │ gomoku/      │ bind_app="gomoku_pkg" │ 透 gomoku:rx 到 bus + 提供 GUI 页                        │
  └──────────────┴───────────────────────┴──────────────────────────────────────────────────────────┘

  gomoku 是唯一带 GUI 页的样板 —— 教插件作者怎么写带前端的插件。

  ---
  8. GUI 集成

  # gui/app.py
  PAGE_DEFS = [   # 内置页保留
      ("home",   "首页",   HomePage),
      ("music",  "音乐",   MusicPage),
      ("upload", "上传",   UploadPage),
      ("notify", "通知",   NotifyPage),
      ("log",    "日志",   LogPage),
  ]

  class CompanionApp:
      def __init__(self, ..., plugin_manager):
          self._pm = plugin_manager
          ...

      def _build(self):
          page_defs = list(PAGE_DEFS)
          for pid, title, plugin in self._pm.get_gui_pages():
              def make(master, app, p=plugin):
                  return p.make_gui_page(master, app)
              page_defs.append((pid, title, make))
          # 按 page_defs 渲染侧边栏 + content
          ...

      def reload_plugins(self):
          """侧边栏底部按钮触发：扫新插件 → 重建侧边栏"""
          self._pm.reload_discovery()
          self._rebuild_sidebar()

  侧边栏底部加"🔄 刷新插件"按钮（log 页之下）。

  ---
  9. 工作量预估

  ┌─────────────────────────────────────────────────────┬───────────┐
  │                        任务                         │   估时    │
  ├─────────────────────────────────────────────────────┼───────────┤
  │ 写 plugin_sdk.py                                    │ 25min     │
  ├─────────────────────────────────────────────────────┼───────────┤
  │ 写 plugin_manager.py                                │ 60min     │
  ├─────────────────────────────────────────────────────┼───────────┤
  │ 重构 bridge_provider 为哑总线                       │ 30min     │
  ├─────────────────────────────────────────────────────┼───────────┤
  │ 写 4 个迁移插件（weather/music_proxy/notif/gomoku） │ 90min     │
  ├─────────────────────────────────────────────────────┼───────────┤
  │ 迁 win_notifications + gomoku gui_page              │ 15min     │
  ├─────────────────────────────────────────────────────┼───────────┤
  │ providers/ 拆 native/ + dynapp/ 子目录 + 修 import  │ 30min     │
  ├─────────────────────────────────────────────────────┼───────────┤
  │ __main__.py 集成 plugin manager                     │ 25min     │
  ├─────────────────────────────────────────────────────┼───────────┤
  │ gui/app.py 动态收集 + 刷新按钮                      │ 40min     │
  ├─────────────────────────────────────────────────────┼───────────┤
  │ 通跑一遍 + 修小 bug                                 │ 30min     │
  ├─────────────────────────────────────────────────────┼───────────┤
  │ 文档（工作日志 + 插件作者 README）                  │ 50min     │
  ├─────────────────────────────────────────────────────┼───────────┤
  │ 合计                                                │ ~6.5 小时 │
  └─────────────────────────────────────────────────────┴───────────┘

  ---
  10. 不做的事（明确边界）

  - ❌ 远程下载 / 在线市场
  - ❌ 沙箱 / 权限隔离
  - ❌ 修改老插件代码后热 reload（重启脚本）
  - ❌ 插件依赖管理（plugin.json / requires）
  - ❌ 转场动画 / GUI 页热刷新

  ---
  11. 唯一一个需要你拍板的决策点

  plugin_id vs bind_app 关系：

  我现在的设计里 plugin_id 和 bind_app 是两个字段，分开使用：
  - gomoku 插件：plugin_id="gomoku", bind_app="gomoku_pkg"（不一样）
  - notif 插件：plugin_id="notif", bind_app="notif_pkg"（不一样）
  - weather 插件：plugin_id="weather", bind_app=None（通用）

  A. 现状（推荐）：plugin_id ≠ bind_app，明确语义"插件 id 是 PC 端的，app id 是设备端的"。两者字段都要写。

  B. 简化：默认 bind_app = plugin_id + "_pkg" 自动推断，不显式时省掉一个字段。

  我倾向 A：明确比省字段重要，省那一行没收益但增加歧义（万一某天插件名和 app 名不规则就出 bug）。

  ---
  确认这个方案 OK 我就一波到底执行。其它问题（开发期+用户级双路径 / SDK 暴露 bus+log+tx / 热加载只支持加 / GUI
  页同上）继续按上一轮推荐方案走，不再问。