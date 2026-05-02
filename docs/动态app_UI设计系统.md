# 动态 App UI 设计系统

> 适用：基于本固件 dynamic_app runtime 的 JS app 视觉与交互规范
> 配套：[`动态app_JS_API速查.md`](./动态app_JS_API速查.md)（API 字典）+ [`动态app开发指南.md`](./动态app开发指南.md)（流程教程）
>
> **本文档目标**：让动态 app 看起来与原生 app 几乎一致，并且写法是固定模板而不是每次现拼。

---

## 0. 哲学：地基 vs 应用

动态 app 的能力分两类：

| 类别 | 谁负责 | 进固件的标准 |
|---|---|---|
| **地基** | 固件提供 native API | 涉及 OS 资源（LVGL 指针、NVS、BLE）；或者**多数 app 形态稳定**（如模态弹层、toast、淡入） |
| **应用** | 业务 JS 自己写 | 每个 app 都不一样的部分（数据 schema、协议、列表项布局） |

判断尺子：

- ✅ 进固件：iOS 列表行外观、模态结构、状态栏、上滑退出 —— 几乎所有 app 都长一样
- ❌ 留 JS：通知/笔记/天气的列表内容、BLE 协议字段、本地存储格式 —— 每家不同
- ❌ 留 JS（但 prelude 给默认实现）：iOS 浅色风的卡片/胶囊按钮 —— 用户想换暗色可以替换

> **设计 token + 高阶组件库** 是介于两者之间的中间层：以默认实现的形式提供，写在 prelude.js 里随 app 自动加载，业务可直接用也可绕过。

---

## 1. 视觉规范

### 1.1 屏幕分区（240×320）

```
┌────────────────────┐ y=0
│  状态栏 (24px)     │  ← 宿主层提供，电池胶囊/蓝牙/时间
├────────────────────┤ y=24
│                    │
│                    │
│  动态 app 可用区   │  ← 240×268，所有 UI 在这里
│  (240×268)         │
│                    │
│                    │
├────────────────────┤ y=292
│  上滑退出区 (28px) │  ← 宿主层兜底，dy≥30 回 launcher
└────────────────────┘ y=320
```

**铁律**：屏底 28px 不放任何点击元素。仿手机 home indicator。

### 1.2 配色（iOS 浅色）

| 角色 | token | 色值 | 用途 |
|---|---|---|---|
| 屏幕底 | `C_BG` | `#F2F2F7` | 整页背景 |
| 卡片 | `C_PANEL` | `#FFFFFF` | 列表项 / 详情卡 |
| 卡片悬浮 | `C_PANEL_HI` | `#E5E5EA` | 按下高亮 |
| 边框 | `C_BORDER` | `#C6C6C8` | 1px 分隔线 |
| 主文字 | `C_TEXT` | `#000000` | 标题 / 正文 |
| 次文字 | `C_TEXT_DIM` | `#3C3C43` | 副标题 / 时间 |
| 弱文字 | `C_TEXT_MUTED` | `#6E6E73` | 占位 / 描述 |
| 主操作 | `C_ACCENT` | `#007AFF` | iOS 蓝（按钮 / 链接 / 链接） |
| 次操作 | `C_ACCENT_2` | `#AF52DE` | iOS 紫（装饰） |
| 成功 | `C_OK` | `#34C759` | 完成 / 已连 |
| 警告 | `C_WARN` | `#FF9500` | 注意 |
| 错误 | `C_ERR` | `#FF3B30` | 失败 / 删除 |

业务代码绝不写裸 `0x007AFF`，统一 `UI.T.C_ACCENT`。

### 1.3 间距与圆角

| token | 值 | 典型用途 |
|---|---|---|
| `SP_XS` | 4 | 紧凑 padding |
| `SP_SM` | 8 | gap |
| `SP_MD` | 12 | 卡片内边 |
| `SP_LG` | 16 | 页边距 |
| `SP_XL` | 24 | 大间隔 |
| `R_SM` | 6 | 小按钮 |
| `R_MD` | 10 | 普通按钮 |
| `R_LG` | 14 | 卡片 |
| `R_PILL` | 1000 | 胶囊（自动 clamp） |

### 1.4 字体

| 字体 | 字号 | 用途 |
|---|---|---|
| `text` | 14px CJK | 默认正文 |
| `title` | 16px CJK | 卡片标题 / 列表 label |
| `huge` | 48px | 时钟 / 温度 |
| `numM` | 24px Montserrat | 中号 ASCII 数字 |
| `icon24` | 24px Material Symbols | 图标 |
| `icon36` | 36px Material Symbols | 大图标 |

> 用 `sys.icons.*` 必须配 `'icon24'/'icon36'`，否则字体表里没那些 codepoint。

### 1.5 动画时长

| token | ms | 用途 |
|---|---|---|
| `DUR_FAST` | 150 | 模态出现 / toast 淡入 |
| `DUR_NORM` | 250 | 默认 fadeIn |
| `DUR_SLOW` | 400 | 大动作（页切换） |

---

## 2. 组件库（`UI.*`）

按"使用频率"排序：

### 2.1 容器

```js
UI.screen('root', [...])      // 全屏底，撑满，pad 0
UI.card(opts, [...])          // 白底+1px+圆角14
UI.statusBar({ title, right })  // 业务自绘标题区（44px）
```

### 2.2 列表行

```js
UI.listRow({
    icon: sys.icons.BLUETOOTH,
    label: '蓝牙',
    value: '已连接',           // 可选
    iconBg: UI.T.C_ACCENT,    // 默认灰
    iconColor: UI.T.C_PANEL,  // 默认白
    onClick: function () {},
    divider: true,            // 默认带底部 1px
    id: 'row_bt',             // 可选
    valueId: 'val_bt'         // 用于动态更新 value
})

UI.kvRow({
    key: 'MAC',
    value: 'AA:BB:...',
    valueId: 'mac_val',
    divider: true
})
```

### 2.3 按钮 / 徽章

```js
UI.iconBtn({ icon: sys.icons.SETTINGS, w:36, h:30, onClick: fn })
UI.pillBtn({ text: '保存', w:140, h:44, onClick: fn })   // 默认 accent 蓝底白字
UI.badge({ text: '99+', w:36, h:20 })                   // accent 圆角胶囊
```

### 2.4 反馈

```js
// 模态弹层（标题 + 正文 + 0~2 按钮 + 自动手势）
UI.modal({
    title: '清空所有通知？',
    body:  '此操作无法撤销。',
    action0: '取消',          // 缺省 / 空字符串 = 无该按钮
    action1: '清空',
    onResult: function (idx) {
        // 0 / 1 = 按钮 ; -1 = 点遮罩或下滑取消
    }
});

// 屏底 toast
UI.toast('已删除', 800);

// 淡入已 mount 的对象
UI.fadeIn('list', 100);
```

### 2.5 装饰

```js
UI.divider()                  // 1px 水平浅灰
UI.hitZone({ onExit })        // 屏底退出区（一般不用，宿主已兜底）
UI.swipeExit(children, fn)    // 给 children 末尾加 hitZone
```

---

## 3. 标准模板

### 3.1 单页 app（如通知 / 设置首页）

```js
var ble = makeBle("myapp");
var T = UI.T, I = UI.I;

function buildUI() {
    VDOM.mount(UI.screen('root', [
        // 顶部业务标题区（不要和宿主状态栏重叠，所以 y=0 就是状态栏下）
        UI.statusBar({ title: '我的 App' }),

        // 内容（卡片 / 列表）
        UI.card({ pad: [0,0,0,0] }, [
            UI.listRow({ icon: I.SETTINGS, label: '设置', iconBg: T.C_TEXT_MUTED }),
            UI.listRow({ icon: I.INFO,     label: '关于', iconBg: T.C_INFO,
                         divider: false })
        ])
    ]), null);
    sys.ui.attachRootListener('root');
    UI.fadeIn('root', 0);
}

buildUI();
sys.log('myapp ready');
```

### 3.2 列表 + 详情模态（仿通知 app）

```js
function showDetail(idx) {
    var n = items[idx];
    UI.modal({
        title: n.title,
        body:  sys.time.format(n.ts, '%Y-%m-%d %H:%M') + '\n\n' + n.body,
        action0: '删除',
        action1: '关闭',
        onResult: function (r) {
            if (r === 0) { items.splice(idx, 1); rebuildList(); UI.toast('已删除'); }
        }
    });
}
```

### 3.3 1Hz 时间显示

```js
setInterval(function () {
    VDOM.set('clock', { text: sys.time.format(sys.time.now(), '%H:%M:%S') });
}, 1000);
```

---

## 4. 宿主层契约

dynapp_host 在动态 app 启动前已经做好：

| 元素 | 位置 | 由 | 业务能否覆盖 |
|---|---|---|---|
| 状态栏 | y=0~24 | `app_shell_attach_statusbar(false)` | ❌（强制浅色） |
| 可用区 | y=24~292 | `list_root` panel | ✅（业务 root mount 在这里） |
| 上滑退出区 | y=292~320 | hitZone + 自算 dy + `app_router_exit_to_launcher` | ❌（强制） |

业务**只需要**关心 240×268 的可用区。

不需要：
- 自己画状态栏
- 自己处理"上滑回 launcher"
- 自己处理"返回按钮"

可以：
- 在可用区内加自己的标题区（`UI.statusBar` 是业务自绘 44px，不是状态栏）
- 在内容区做任意手势（页内左右滑切 tab、长按、双击等）

---

## 5. 字体覆盖范围

### 5.1 CJK（text/title/huge）
内嵌 Tiny TTF 子集，覆盖项目用到的常用汉字 + ASCII。**不能**显示子集外字符（会渲染成方块或缺失）。

如果你的 app 需要新汉字，要在 `app/fonts/` 加进字体子集源。

### 5.2 图标（icon24/icon36）
Material Symbols Rounded 字体子集，目前覆盖 33 个 codepoint：

**通用 / 系统**：BLUETOOTH / BT_DISABLED / SCHEDULE / WEATHER / NOTIFICATIONS / MUSIC / TUNE / SETTINGS / BRIGHTNESS / INFO / EDIT_CALENDAR / APPS / CHEVRON_LEFT / CHEVRON_RIGHT / DOT / DOT_SMALL

**业务 app**：ALARM / TIMER（=STOPWATCH）/ HABIT / NOTE / GAME / CALCULATOR / IMAGE / MEMORY / DASHBOARD / PUZZLE / TARGET / PETS（=AQUARIUM）/ ECHO

加新图标流程：
1. `app/fonts/material_icons_subset.ttf` 用 pyftsubset 重新生成（加新 codepoint）
2. `app/app_fonts.h` 加 `ICON_XXX` 宏
3. `dynamic_app/dynamic_app_registry.c::k_icon_table` 加一行（launcher manifest 用）
4. `dynamic_app_natives.c::sys.icons.*` 加一行（动态 app 内部 label 用）
5. `tools/make_pack_manifest.py::ICONS` 加一行（manifest 工具校验用）
6. 重编固件

不需要改前端代码，业务直接 `sys.icons.NEW_ICON` 即可。

### 5.3 数字（numM）
Montserrat 24px，仅 ASCII 数字 + 标点。用于"中号数字带特殊字形"场景（计时器秒数、温度数字等）。

---

## 6. 性能与限制

| 资源 | 上限 |
|---|---|
| JS 堆 | 1MB（PSRAM） |
| Registry slot | 256 个 LVGL 对象 |
| 命令队列 | 128 条（满则脚本阻塞 100ms） |
| setInterval | 8 个并发 |
| `sys.app.saveState` | 4KB JSON / app |
| `sys.fs.write` 单 chunk | 196B（MVP 限制） |
| 同时活跃 modal | 1 个（再弹会先关旧的） |

实际测下来通知 app 复刻 ~250 个 LVGL 对象、几百条命令、~30KB JS 堆占用，离上限远得很。

---

## 7. 写一个新 app 的清单

1. `dynamic_app/scripts/<app_id>_pkg/` 建目录
2. `main.js` 顶部声明：
   ```js
   var ble = makeBle("<app_id>");
   var T = UI.T, I = UI.I;
   ```
3. 写 `buildUI()`：用 `UI.screen + statusBar + card + listRow` 拼骨架
4. mount 后 `sys.ui.attachRootListener('root')`（**必须**）
5. 持久化用 `sys.app.saveState/loadState`
6. BLE 用 `ble.on(type, cb)` 接收 / `ble.send(type, body)` 发送
7. 时间用 `sys.time.now() + sys.time.format(ts, fmt)`
8. 反馈用 `UI.modal / UI.toast`
9. **不要**：自己画状态栏 / 自己做上滑退出 / 屏底放点击元素 / 用裸颜色值

可选：
- `manifest.json`：`{"id":"<app_id>", "name":"显示名"}`
- `icon.bin`：32×32 RGB565 图标（参考已有 `_pkg/_make_icon.py`）
- `assets/<name>.bin`：业务图片（`sys.ui.createImage` / `setImageSrc` 引用）

通过 PC GUI 上传整个目录 → launcher 自动出现 → 点击启动。

---

## 8. 参考实现

完整工作示例：[`dynamic_app/scripts/notif_pkg/main.js`](../dynamic_app/scripts/notif_pkg/main.js)

复刻原生通知 app，覆盖：
- iOS 列表行 + 类别色块
- 详情模态 + 删除/关闭按钮
- 长按徽章 → 清空所有模态
- BLE 接收新通知 + 1Hz 时间刷新
- `sys.app.saveState` 持久化
- 完全不依赖 `notify_manager`（脚本自管协议与数据）

约 250 行 JS，是写新 app 时的首选 copy 对象。

---

## 9. 不该用 UI 库的场景

`UI.*` 是默认实现，**不是必须**。下列情况直接用 VDOM 原语：

- 暗色风游戏 / 仪表盘（用 `h('panel', { bg: 0x14101F, ... })` 自管）
- 全屏画布类（如 2048 / mole 那种网格游戏）
- 极简实验性 demo（不在乎视觉一致性）

UI 库的设计是"加法"——加了它你拿到 iOS 风；不加你仍能用 VDOM 自由发挥。

---

## 10. 后续扩展（暂未实现）

| 候选 | 用例 | 工作量 |
|---|---|---|
| `lv_arc` 控件 | 仪表盘圆环、进度环 | ~1h |
| `lv_slider` 控件 | 亮度/音量 slider | ~1h |
| sub_router push/pop | 多级页面（设置二级菜单） | ~3h |
| `sys.app.exit()` | JS 主动回 launcher | ~10min |
| 暗色 token 集 | 暗色风格 app | ~30min |

按需补，不为假设需求提前加 API。
