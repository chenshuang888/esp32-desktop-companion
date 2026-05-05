# 动态 App 包格式规范

> 适用：本固件 dynamic_app runtime 接受的 app 包目录布局
> 配套：[`动态app_JS_API速查.md`](./动态app_JS_API速查.md) · [`动态app_UI设计系统.md`](./动态app_UI设计系统.md)

---

## 0. 历史与现状

dynamic_app 上传链路在 2026-05 初做过一次清理：**裸 `.js` 上传方式已彻底废弃**。
现在固件、PC SDK、PC GUI 都只接受**包目录**（pack）形式上传。

理由：
- 一个 app 通常需要多个文件（main.js + manifest + icon + assets）
- 包目录是固件 LittleFS 上的真实布局（`/littlefs/apps/<id>/`），上传形式与设备布局对齐
- 单文件上传是早期临时捷径，缺 manifest/icon 后用户体验差

---

## 1. 设备端布局（LittleFS）

```
/littlefs/
└── apps/
    └── <app_id>/
        ├── main.js              ← 必需，入口脚本
        ├── manifest.json        ← 可选，元信息
        ├── icon.bin             ← 可选，32×32 RGB565 launcher 图标
        ├── assets/
        │   ├── <name>.bin
        │   └── ...
        └── data/                ← 运行时由 sys.fs.* 自动管理（用户数据沙箱）
            └── ...
```

`<app_id>` 字符集：`[a-zA-Z0-9_-]`，长度 ≤ 15 字节。

---

## 2. PC 端源码布局（推荐）

```
dynamic_app/scripts/
├── prelude.js                   ← 内嵌入固件，runtime 自动加载，所有 app 共享
├── notif_pkg/                   ← 标杆样例（iOS 浅色 + UI 库）
│   ├── main.js
│   ├── manifest.json
│   └── README.md                ← 仅源码仓库；上传时不传
├── alarm_pkg/                   ← 复杂样例（多页 + 持久化 + 编辑模式）
├── dash/
├── habit_pkg/
├── imgdemo_pkg/
└── memory_pkg/
```

包目录命名建议：业务名 + `_pkg` 后缀（如 `notif_pkg`），与 app_id 区分（app_id 一般无后缀）。

> 不强制；同名也行（如 `dash/` 同时是包目录和 app_id）。`_pkg` 只是源码仓库的可读性约定。

---

## 3. 各文件格式

### 3.1 `main.js`（**必需**）

- ES5 语法（esp-mquickjs 不支持 ES6+：箭头函数 / let / const / class / 模板字符串 / Promise / setTimeout）
- 大小上限：~64KB（DYNAPP_SCRIPT_STORE_MAX_BYTES）
- UTF-8 编码
- 顶部应调用 `makeBle("<app_id>")` 一次（如果用 BLE）
- 末尾应调用 `sys.ui.attachRootListener('<rootId>')` 一次（如果有交互）

最简框架：
```js
var ble = makeBle("myapp");
VDOM.mount(UI.screen('root', [...]), null);
sys.ui.attachRootListener('root');
```

### 3.2 `manifest.json`（可选但**强烈推荐**）

```json
{
    "id":        "myapp",
    "name":     "我的 App",
    "icon":      "ALARM",
    "iconColor": "WARN",
    "version":   "1.0.0"
}
```

| 字段 | 必填 | 说明 |
|---|---|---|
| `id` | ✓ | 必须与目录名一致（固件校验，不一致拒绝加载） |
| `name` | ✓ | launcher 显示名（中文 OK）。缺省时回退到 `id` |
| `icon` | – | launcher 菜单图标名，固件查表翻译为 36px Material Symbols 字体 codepoint。缺省时用通用 `APPS` |
| `iconColor` | – | 图标颜色 token 名（与 `sys.tokens.C_*` 同名）。缺省时用中性灰 |
| `version` | – | 自由格式，目前固件不解析 |

**`icon` 可用值**（与 `sys.icons.*` 一致 + 业务图标）：

| 类别 | 名字 |
|---|---|
| 通讯 / 系统 | `BLUETOOTH` `BT_DISABLED` `NOTIFICATIONS` `INFO` `SETTINGS` `TUNE` |
| 时间 / 日历 | `SCHEDULE` `EDIT_CALENDAR` `ALARM` `TIMER` `STOPWATCH` |
| 媒体 / 创意 | `MUSIC` `BRIGHTNESS` `WEATHER` `IMAGE` |
| 业务 app | `HABIT` `NOTE` `GAME` `CALCULATOR` `MEMORY` `DASHBOARD` `PUZZLE` `TARGET` `PETS` `AQUARIUM` `ECHO` |
| 装饰 | `APPS` `CHEVRON_LEFT` `CHEVRON_RIGHT` `DOT` `DOT_SMALL` |

**`iconColor` 可用值**：`ACCENT`(蓝) / `ACCENT_2`(紫) / `OK`(绿) / `WARN`(橙) / `ERR`(红) / `INFO`(浅蓝) / `TEXT_MUTED`(灰) / 等。

#### 推荐：用 `make_pack_manifest.py` 生成

```bash
python tools/make_pack_manifest.py dynamic_app/scripts/notif_pkg \
    --id notif_pkg --name 通知 --icon NOTIFICATIONS --color ACCENT
```

工具校验 `--icon` / `--color` 是否在合法集合内，避免运行时才发现拼错。

**缺失 manifest 时**：上传 client 自动生成默认值（id=app_id, name=app_id），launcher 用通用 `APPS` 灰图标。能跑但视觉土。

### 3.3 ~~`icon.bin`~~（已废弃）

老版本通过 `icon.bin`（32×32 RGB565 图）做菜单图标。从 2026-05 起，**launcher 改用 manifest 里的 `icon` 字段走 36px 矢量字体渲染**，与原生 app launcher 完全同款路径：

- ✅ 体积小：manifest 60~110B vs icon.bin 2KB
- ✅ 永远清晰：矢量字体
- ✅ 改图标只改一行 manifest，不用重新生成图

如果你的旧包还有 `icon.bin`，可以删掉（不上传也行；固件不再读它）。

### 3.4 `assets/<name>.bin`（可选）

- 任意业务图片资源（LVGL `lv_image` 兼容的 bin 格式）
- 单个文件路径长度限制：`<app_id>/assets/<name>` 总长 ≤ 31 字节
- 名字字符集：`[a-zA-Z0-9_.-]`
- JS 引用：`sys.ui.createImage(id, parentId, "fish.bin")` 或 `sys.ui.setImageSrc(id, "fish.bin")`，
  C 端自动拼成绝对路径 `A:/littlefs/apps/<app_id>/assets/fish.bin`

### 3.5 `data/`（运行时自动）

- 由 `sys.fs.read/write/exists/remove/list` 在运行时按需创建
- **不要**在源码包里手工放 `data/`，上传 client 不会传它（即使有也会被忽略）
- 沙箱隔离：每个 app 只能访问自己的 `data/`

---

## 4. 上传流程

### 4.1 通过 PC GUI（推荐）

1. 启动 companion：`python -m companion`
2. 切到「上传」页
3. 点「选目录」选择包目录（如 `dynamic_app/scripts/notif_pkg`）
4. App ID 框会自动填入目录名（可手改）
5. 点「上 传」
6. 等待进度条跑完（manifest → main.js → icon.bin → assets/...）
7. launcher 自动出现新 app 图标，无需重启设备

### 4.2 通过 SDK（脚本批量上传）

```py
import asyncio
from dynapp_uploader import UploaderClient

async def main():
    async with UploaderClient(device_name="ESP32") as c:
        await c.upload_app_pack("notif", "dynamic_app/scripts/notif_pkg",
                                display_name="通知")
        print(await c.list_apps())

asyncio.run(main())
```

可选回调：
```py
def on_step(filename, idx, total):
    print(f"[{idx}/{total}] {filename}")

def on_progress(sent, total):
    print(f"  {sent}/{total} bytes")

await c.upload_app_pack("notif", pack_dir, on_step=on_step, on_progress=on_progress)
```

---

## 5. 上传顺序

固件 LittleFS 是按文件原子提交的（每个文件 `.tmp` → rename），上传顺序无关紧要。
但 PC 端的 `upload_app_pack` 固定顺序便于排错：

1. `manifest.json`（先到，便于固件确认 id 匹配）
2. `main.js`（主体）
3. `icon.bin`（如有）
4. `assets/*`（如有，逐个）

这个顺序也是 GUI 进度条的步进顺序。

---

## 6. 删除 / 更新 app

### 删除
GUI 「删除」按钮 → 固件递归删 `apps/<id>/`，包括 main.js / manifest / icon / assets / data 全部清掉。
launcher 自动刷新。

### 更新（版本迭代）
重新上传同 app_id 的包目录。固件**保留 data/**（用户数据），覆盖其它文件。

---

## 7. 字符集 / 长度限制速查

| 项 | 上限 | 字符集 |
|---|---|---|
| `app_id` | 15 字节 | `[a-zA-Z0-9_-]` |
| 包内 filename（main.js / manifest.json / icon.bin） | 31 字节 | `[a-zA-Z0-9_.-]` |
| 包内 assets 路径（`assets/<name>`） | 31 字节（含前缀） | 同上 |
| sys.fs 用户数据 relpath | 24 字节 | 同上，不含 `/` `\` `..`，不以 `.` 起 |
| 单文件 size（main.js / 资产） | ~64KB | 二进制 |
| `manifest.id` 字段 | 必须 == 目录名 | — |

违反任一条都会被固件 / SDK 拒绝（看错误信息定位）。

---

## 8. 已知限制（暂未实现）

- 没有"app 间依赖"机制：每个包都是孤岛
- 没有"版本检查"：固件不读 manifest.version，重传不会因版本旧而拒绝
- 没有"签名校验"：任何能连 BLE 的客户端都能上传
- assets 单 chunk 上限 196B（MVP 限制；大文件会自动分片但路径有 31 字节限制）

---

## 9. 完整最小包

```
dynamic_app/scripts/echo_pkg/
├── main.js
└── manifest.json
```

`main.js`：
```js
var ble = makeBle("echo");

VDOM.mount(UI.screen('root', [
    UI.statusBar({ title: 'Echo' }),
    UI.card({}, [
        h('label', { id: 'msg', text: '等待 BLE 消息',
                     fg: UI.T.C_TEXT, font: 'text' })
    ])
]), null);
sys.ui.attachRootListener('root');

ble.onAny(function (m) {
    VDOM.set('msg', { text: 'got: ' + (m.type || '?') });
    ble.send('reply', { echo: m.body });
});

UI.fadeIn('root', 0);
```

`manifest.json`：
```json
{"id":"echo","name":"回声示范","version":"1.0.0"}
```

通过 GUI 「选目录」→「上传」即可。
