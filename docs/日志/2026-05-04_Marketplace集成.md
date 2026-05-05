# Marketplace 集成 工作日志（demo6 v0.10）

**日期**：2026-05-04
**分支**：main（直接落 main，无大破坏）
**作者**：ChenShuang + Claude
**起点**：v0.9（demo6 PC 端插件化达成，但插件分发只能靠手动拷文件）
**终点**：companion 加「市场」侧边栏，浏览/一键安装/一键卸载远程动态 app

---

## 0. 起因

v0.9 之后 demo6 已经是一个完整的「设备 + 工具链」自包含项目：

- 设备端跑 `dynamic_app/scripts/<name>_pkg/`
- PC 端配套 `tools/plugins/<name>/`
- companion 上传页能把"目录形式"的 app 推到设备

但**分发**这一环还是空白：开发者 A 写好一个动态 app 想给开发者 B 用，得手动打包发文件、B 手动解压到对应目录、B 自己上传到设备。

刚做完的 [esp32-marketplace](../../esp32-marketplace/) 平台填了"上传/浏览/审核/下载" 4 件事，剩下"用户怎么把市场的包装到自己设备"——这是 companion 这一侧应该做的。

目标硬指标：**点一个「安装」按钮，5 秒内 .mpkg 已经在 ESP32 上跑起来，PC 插件已就位**。

---

## 1. 关键决策

### 1.1 marketplace 不是 plugin，是 companion 内置模块

最初想把"市场"做成 `tools/plugins/marketplace/` 走插件系统加载。被自己驳回——

`plugins/` 的定位（v0.9 已经写在 `tools/plugins/README.md` §0）：

> 插件存在的唯一理由：ESP32 端某个动态 app 需要 PC 帮它做 BLE 收发**之外**的事。
> 主程序自己的功能不是插件——它们是主程序的 GUI 页。

marketplace 是"会去**安装其他插件**到 plugins/ 目录"的工具，本质是 companion 主程序的内置功能。如果它自己也是插件就成了"插件管理器插件"——递归名字就奇怪。

**最终位置**：

```
tools/companion/
├── marketplace/                    ★ 新增模块（与 providers/ gui/ 平级）
│   ├── __init__.py
│   ├── client.py                   HTTP API 客户端
│   ├── installer.py                .mpkg 解析 + plugin 落盘 + bus 调度上传
│   ├── registry.py                 已装清单 JSON
│   └── config.py                   base_url 持久化
└── gui/pages/marketplace.py        ★ 新增 GUI 页
```

`marketplace.py` 是 companion 的**第二个内置 GUI 页**（紧跟首页之后）。和 `home.py / upload.py / log.py` 同等地位。

### 1.2 复用 UploaderClient + UploadProvider，零改动

关键发现：现有 `UploaderClient.upload_app_pack(app_id, pack_dir)` 已经能干我要的活，只是输入是**目录形式**的 pack。

那 marketplace 安装链路就成了：

```
.mpkg (zip)
   │ 1) parse_mpkg
   ▼
ParsedMpkg(manifest, main_js, icon, assets, plugin_files)
   │ 2) make_temp_pack_dir
   ▼
/tmp/mpkg_xxx/
   ├── manifest.json
   ├── main.js
   ├── icon.bin
   └── assets/
   │ 3) bus.emit("upload:request", kind="pack", pack_dir=tmp)
   ▼
UploadProvider._handle_request → UploaderClient.upload_app_pack
   （现有 BLE 链路 + 进度事件 upload:begin/step/progress/end）
```

**0 行改动 `dynapp_uploader/`**，**0 行改动 `providers/dynapp/upload_provider.py`**。仅复用它们暴露的 bus 协议。

代价：写临时目录有少量 IO，但 .mpkg 总共也就几百 KB，可忽略。

### 1.3 plugin 落盘策略 + 卸载安全

把 plugin/ 目录解到 `tools/plugins/<slug>/` 是直觉操作，但**卸载**要小心：

- 不能 `shutil.rmtree(plugins/<slug>/)` 直接删——用户可能在里面手加了文件（assets / 调试代码）
- 不能保留所有原文件——那留多一个文件反而是 bug

**最终方案**：registry 记录"我们装了哪些文件"清单，卸载时按清单**逐个删**：

```json
// tools/plugins/.marketplace_meta/_registry.json
{
  "installed": {
    "demo_ai": {
      "version": "0.1.0",
      "plugin_files": [
        "plugins/demo_ai/plugin.py",
        "plugins/demo_ai/gui_page.py"
      ],
      "plugin_dir_name": "demo_ai"
    }
  }
}
```

删完文件后，目录如果空了就 `rmdir`；非空（有用户加的文件）就保留目录。

副效果：**重装**会先删旧 plugin 目录再装新版，但 registry 之外的用户文件丢——这个是小坑，README 里写"plugin 目录里别手加文件"。

### 1.4 PC 插件不能 hot reload，明确告诉用户

Python 模块系统对 `importlib.reload` 的支持很差：

- 已 import 的子模块不会跟着 reload
- bus 已订阅的回调还指向旧函数
- GUI 已挂的页面还是旧组件

正确做法是 **kill + relaunch 进程**。companion 还没做"自重启"，所以装/卸 plugin 后 toast 提示用户手动重启。

### 1.5 进度复用 upload:* 事件总线

UploadProvider 已经在发 `upload:begin / step / progress / end`，UploadPage 也在监听做进度条。MarketplacePage 只需：

- 监听同样的事件，画自己的进度条
- 当多个页面同时监听是 OK 的（EventBus 是广播的）

这意味着安装时如果用户切到 Upload 页也能看到进度——免费的副作用。

### 1.6 网络层用同步 requests + threading

companion 主程序里到处是 asyncio（BLE 是 bleak，runner 跑独立 event loop）。但市场 HTTP 调用没必要混进 asyncio：

- requests 同步好写
- 在工作线程跑 + `self.after(0, ...)` 桥回 GUI 线程是 tkinter 标准模式
- 不用关心 event loop 跨线程问题

`MarketplaceClient` 的所有方法都是同步阻塞，`MarketplacePage` 启 `threading.Thread(daemon=True)` 跑。

---

## 2. 落地清单

### 2.1 新增模块 `tools/companion/marketplace/`

| 文件 | 行数 | 职责 |
|---|---|---|
| `__init__.py` | 17 | 公开 API re-export |
| `config.py` | 50 | base_url 跨平台持久化（`%APPDATA%/esp32-companion/marketplace.json`）|
| `client.py` | 130 | requests Session 封装 list / detail / download，带进度回调 |
| `installer.py` | 220 | parse_mpkg / make_temp_pack_dir / install_plugin_locally / uninstall_plugin_locally / 三个 bus.emit 包装 |
| `registry.py` | 80 | _registry.json 读写 |

### 2.2 新增 GUI 页

| 文件 | 行数 | 内容 |
|---|---|---|
| `gui/pages/marketplace.py` | ~330 | 顶部地址栏 + 状态行 + 滚动卡片列表 + 进度条 |

每张卡片状态 4 态（未装 / 已装最新 / 可更新 / 孤儿）+ 对应按钮（安装 / 重装 / 更新 / 卸载）。

### 2.3 接入主程序

| 文件 | 改动 |
|---|---|
| `gui/app.py` | 新增 `from .pages.marketplace import MarketplacePage`；PAGE_DEFS 加 `("marketplace", "市场", MarketplacePage)`，紧跟首页 |

总计 demo6 这边 **2 行 import + 1 行 PAGE_DEFS** —— 真正的 zero-touch 集成。

### 2.4 文档

| 文件 | 内容 |
|---|---|
| `docs/Marketplace集成_工作日志.md`（本文）| 设计决策 + 实现细节 |
| `README.md` 版本历史 | v0.10 条目 |

---

## 3. 端到端验证

### 3.1 离线（仅 marketplace 部分）

```bash
$ cd tools && python -c "
import sys; sys.path.insert(0, '.')
from companion.marketplace import MarketplaceClient, parse_mpkg
c = MarketplaceClient()
total, items = c.list_packages(page_size=10)
print('total =', total)
for p in items: print(' -', p.slug, p.name, 'v'+str(p.latest_version), 'plugin?', p.needs_plugin)
data = c.download_mpkg('demo_ai')
print('downloaded', len(data), 'bytes')
parsed = parse_mpkg(data)
print('manifest:', parsed.manifest)
print('plugin files:', list(parsed.plugin_files.keys()))
print('has_plugin:', parsed.has_plugin)
"

total = 1
 - demo_ai AI Demo v0.1.0 plugin? True
downloaded 953 bytes
manifest: {'id': 'demo_ai', 'name': 'AI Demo', ...}
plugin files: ['plugin/plugin.py']
has_plugin: True
```

### 3.2 完整流程（需 ESP32 + companion）

1. 启 marketplace 后端 + 前端（之前已部署在本地 docker）
2. 启 companion，左侧栏看到「市场」
3. Home 等设备连上
4. 市场页看到 demo_ai 卡片，点 **安装** → 二次确认 → 进度走完
5. 设备菜单出现 demo_ai；`tools/plugins/demo_ai/plugin.py` 出现
6. registry.json 记录元数据
7. 点 **卸载** → 设备删 + plugin 目录清

---

## 4. 几次踩坑修复

### 4.1 `app.connected` 属性不存在

最初 `_on_install` 里写 `if not self._app.connected:`，运行时 AttributeError。

`CompanionApp` 没暴露 `connected` 属性，连接状态靠 bus 事件传递。

修复：MarketplacePage 自己监听 `connect/disconnect` 事件维护 `self._connected` 标志：

```python
app.bus.on("connect",    lambda _: self.after(0, self._on_conn, True))
app.bus.on("disconnect", lambda _: self.after(0, self._on_conn, False))
```

教训：跨页面共享状态时，bus 是 single source of truth；不要假设 `app` 对象上有缓存。

### 4.2 plugin 目录路径回溯计算

`registry.py` 要找到 `tools/plugins/.marketplace_meta/`，从 `companion/marketplace/registry.py` 回溯：

```
companion/marketplace/registry.py
   .parent → companion/marketplace/
   .parent → companion/
   .parent → tools/             ← 要的就是这里
```

写错过一次（只 `.parent` 两次），meta 目录创建到 `companion/plugins/` 去了。debug 加 `print(REGISTRY_FILE)` 一眼看到。

教训：`Path(__file__).parent.parent.parent` 这种链式调用没有类型提示，容易数错次数。改成命名变量 `tools_dir = ...`，可读性强。

### 4.3 zip-slip 防御要做两次

`parse_mpkg` 拒绝路径含 `..` 是基本盘：

```python
if ".." in n.split("/"):
    raise InstallerError(...)
```

但 `install_plugin_locally` 写文件时仍然要二次校验：

```python
target = plugin_root / rel
target.resolve().relative_to(plugin_root.resolve())
```

理由：万一前面的字符串检查漏了什么诡异编码（比如 unicode normalization），二次 resolve 是最后防线。

### 4.4 同 slug 重装会不会冲突

设备端 `upload_app_pack` 是覆盖语义（同 path 写两次后写赢），所以重装会覆盖旧 main.js / manifest.json，没问题。

PC 端 `install_plugin_locally` 进入时如果 `plugins/<slug>/` 已存在就 `shutil.rmtree(plugin_root)`，再写新文件——这一步会把用户在 plugin 目录里手加的文件也删掉。

权衡：

- 不删旧的 → 残留旧版本垃圾文件
- 删旧的 → 用户改的文件丢

选了"删旧的"。文档里明示：plugin 目录是 marketplace 管理的，**不要手加文件**。如果想加的话改用其他 plugins/<my_custom>/ 目录。

---

## 5. 与 marketplace 后端的契约

集成成立的前提是 marketplace 后端**保证以下不变量**：

| 契约 | 后端做了什么 | 我们依赖什么 |
|---|---|---|
| `.mpkg` 是 zip | 上传时 yauzl 解析校验 | 我们 zipfile 直接读 |
| 根目录有 manifest.json | scanner 强校验 | 我们 `z.read("manifest.json")` |
| manifest.id 全局唯一 = slug | DB 唯一约束 | 我们用 manifest.id 当设备端 app_id |
| plugin 在 plugin/ 子目录 | 上传时按这个约定扫 | 我们按 `plugin/` 前缀过滤 |
| 危险包不会被 published | scanner + AI verdict=danger 直接拒 | 我们假设下载的包都过审了 |
| download 接口返 .mpkg 字节流 | redirect 到 MinIO 预签名 URL | requests follow redirect |

如果 marketplace 后端改了任何一条都会 break 集成，所以**两边的 .mpkg 解析逻辑应该尽快抽出共享 spec 文档**——避免某一侧改了另一侧不知道。

---

## 6. 设计层面的反思

### 6.1 「装/卸需要重启」不是缺陷，是 Python 现实

最初觉得"用户体验差"，想做 hot reload。研究下来发现：

- `importlib.reload` 不动子模块
- bus 已订阅的 lambda 还指向旧对象
- providers 已 start 的协程还引用旧 client

要做对得**全套重新挂一遍**：unsubscribe 所有 bus / cancel 所有 task / 重新 import / 重新 register。复杂度极高，收益是省一次重启。

放弃了，明确写文档：装完重启即可。

未来如果真有强需求，路径是把 PluginManager 做成"reload 全部 plugins"——和 v0.9 时已经做过的`add 新插件目录热加载`同源。

### 6.2 marketplace 集成的成本被设计成「极低」

整个集成在 demo6 这一侧只动了：

- 加一个目录 `companion/marketplace/`（新增）
- 加一个 GUI 页 `gui/pages/marketplace.py`（新增）
- 改 3 行 `gui/app.py`（侧边栏注册）

**0 行修改 `dynapp_uploader`，0 行修改 `UploadProvider`，0 行修改任何 plugin**。

设计目标达成：把 marketplace 当成 v0.9 平台之上的**新消费者**，不动平台本身。这种"加东西不改老东西"的能力是 v0.9 平台化重构的回报。

### 6.3 单 .mpkg 让两侧逻辑对称

设备端解 .mpkg 的等价 Python 实现就是 `installer.parse_mpkg`。如果当初是 `.pkg + plugin.zip` 双文件方案，companion 这边要写两次解压两次校验，逻辑会膨胀两倍。

更深的：**如果能让两侧用同一份 schema**（manifest.json zod 定义在 marketplace，Python 这边用 jsonschema 引入），未来加字段就只改一个地方。当前为了简单两边 hardcode 字段名，先欠这个技术债，等真改 manifest schema 再说。

### 6.4 GUI 页面的状态机

MarketplacePage 看起来朴素，但隐含一个 4 维状态：

```
busy_slug × connected × installed × outdated
```

按钮显示规则：

- `busy_slug != None` → 所有按钮 disable（其实是点击时 messagebox 拦）
- `not connected` → 安装/卸载点击时 messagebox 拦
- `not installed` → 显示 [安装]
- `installed and not outdated` → [重装] [卸载]
- `installed and outdated` → [**更新**] [卸载]
- `installed and slug not in market` → [卸载]（孤儿）

这种"按状态渲染按钮"如果是真用户量，应该用 Vue/React 那种声明式 UI。tkinter + customtkinter 是命令式，每次刷新都重建 widget，每次状态变都全量 `_render_items()`——简单粗暴但够用。

### 6.5 进度反馈复用是隐性收益

最初没想用 `upload:*` 事件，准备自己发 `marketplace:install:progress` 之类。改用现有 upload 事件后才发现：

- UploadPage 同时也能看到进度（用户切到上传页一致体验）
- 复用 progress bar 组件代码 0 行
- bus 调试时只需要一套事件名

教训：bus 事件名是"接口"，应该按**语义**命名（"上传进行中"），不是按**触发方**命名（"市场页正在装"）。前者多消费者复用，后者一对一。

---

## 7. 量化

```
新增代码：
  marketplace/             ~500 行 Python
  gui/pages/marketplace.py ~330 行
修改代码：
  gui/app.py               +3 行
文档：
  Marketplace集成_工作日志.md   本文
  README.md                +6 行 v0.10 条目
```

总改动量 ~830 行，4 小时内完成（含端到端测试通过）。

---

## 8. 后续 / 不在本轮范围

- ❌ companion 启动时静默检查更新 → 已装包有新版本则 toast
- ❌ registry 加 sha256，每次启动校验"plugin 是否被外部改过"
- ❌ 包评论展示在卡片上（需要新增 reviews API 调用）
- ❌ 真正的 plugin hot reload
- ❌ marketplace 登录态嵌入 companion（私有包场景）
- ❌ 把 marketplace 的 manifest schema 抽成两端共享
- ❌ 把"市场地址"做到主程序总配置里（现在仅市场页可改）
- ❌ Companion CLI：`companion publish my.mpkg` 一行命令上传（这个未来可能性最大）

---

## 9. 一句话总结

**v0.10 把 demo6 的开发者生态从「线下分发」推进到「线上 marketplace 一键装机」**——marketplace 不是 plugin 而是 companion 内置模块，安装链路全程零修改 `dynapp_uploader / UploadProvider`，仅做一层 .mpkg 解析适配；卸载按 registry 文件清单精确删除避免误伤；进度反馈复用现有 `upload:*` bus 事件做到上传页和市场页一致体验。整个 demo6 + esp32-marketplace 项目从固件 → PC 工具 → 网页平台 → 客户端集成的完整生态闭环到此打通。
