# 动态 App 上传链路清理 + 图标走 TTF 字体路径 工作日志

**日期**：2026-05-02
**分支**：feat/optimize_page
**作者**：ChenShuang + Claude

---

## 0. 起因

通知 app 复刻验证通过后，盘点动态 app 体系还有两处不痛快：

1. **裸 `.js` 上传链路是技术债**：dynamic_app/scripts/ 下 13 个孤儿 .js 文件 + PC 端 `upload_app()`/`kind=='file'`/GUI"选 .js"按钮，是早期临时捷径，与"包目录"路径并存导致心智负担。
2. **launcher 上动态 app 图标小、有底色块、跟原生不一致**：第一版用 PIL 在 32×32 PNG 上画"圆角彩底 + 24px 图标"，烧上去发现：
   - 32px 位图在 80px 宽 cell 里看着偏小（原生 app 是 36px 矢量字体）
   - 圆角矩形彩底跟原生 app launcher 的"无底色 + 纯彩色字符"风格不搭
   - 用户反馈"想跟原生一个风格即可，就是那种透明的"

本轮一次性解决两件事：

- **彻底清裸 .js 链路**（PC 端 4 处机械删除，固件零改动）
- **动态 app launcher 图标改走 TTF 字体路径**（与原生完全同款渲染管线）

---

## 1. 关键决策

### 1.1 裸 .js 路径是 PC 端伪重构

审计发现："裸 .js"在固件里**根本不存在**——BLE 协议、文件系统、registry、launcher 都只认包目录布局 `/littlefs/apps/<id>/main.js`。所谓"裸 .js"只是 PC 端 `upload_app(app_id, main_js_path)` 这个便利包装函数 + GUI 上"选 .js"那个按钮。

→ 删除是 4 处机械删除，0 风险，0 BLE 协议变更，0 设备迁移问题。

### 1.2 选项 C：保留 alarm 作复杂样例，删 12 个

13 个孤儿 .js 里只有 `alarm.js`（403 行，多页 + 持久化 + 编辑模式）有学习价值，**转成 alarm_pkg/**；其余 12 个（calc/weather/music/notes/hello/timer/game2048/mole/reaction/aquarium/echo/imgdemo）质量参差，notif_pkg 已经覆盖了"标杆样例"角色，**直接删**。

### 1.3 图标方案：从 PNG 转向 TTF 字体

第一版"PNG 圆角彩底"被否后，重新评估：

| 方案 | 优 | 劣 |
|---|---|---|
| PNG 子集 ttf 嵌包 | 灵活到极致，每 app 自带字体 | 单 app ~10-20KB RAM；launcher 9 格同时活 = ~150KB 峰值；工具链需 fonttools |
| **manifest 指定固件已嵌字体的 codepoint** ✓ | 0 RAM 增量；视觉与原生 100% 一致；改图标改一行 manifest | 限制在固件子集已包含的 codepoint（扩字体子集是常规操作） |

选后者。理由：动态 app 的"灵活性"应该体现在业务逻辑 + UI 组合，不是每 app 自带一份字体。

### 1.4 manifest 字段桥接策略

manifest 用**字符串名**（`"icon": "ALARM"` / `"iconColor": "WARN"`），在固件 `dynamic_app_registry.c` 里查表翻译为 UTF-8 codepoint + 0xRRGGBB。

为什么不直接在 manifest 里写 codepoint：
- 字符串名直观可读（`"ALARM"` vs `""`）
- 跟 PC 工具的 `sys.tokens.* / sys.icons.*` 同名词汇，规则统一
- 翻译表集中在固件一处，新增图标三处对齐（字体子集 + 查表 + sys.icons 暴露）

### 1.5 launcher 静态/动态 app 走同一渲染路径

之前 launcher 为动态 app 走 `lv_image_create` + `icon.bin` 路径，与静态 app 的 `lv_label + 36px 字体`走两套代码。改造后**两条路径合并**：动态 app cell 也是 label + 36px Material Symbols，只是 icon codepoint / 颜色来源从 `app_descriptor_t::menu_icon` 换成 `dynamic_app_entry_t::icon`。

→ launcher 删了 image 渲染分支、`dyn_has_image` 字段、`<sys/stat.h>` include 和 `DYNAPP_COLOR` 硬编码绿色（动态 app 终于可以每个 app 自己的色，不再都是绿）。

### 1.6 dynamic_app 不依赖 app（保持组件解耦）

想 `dynamic_app_registry.c` 直接 `#include "app_fonts.h"` 复用 `ICON_*` 宏，但 `app` 已经依赖 `dynamic_app`，反向依赖会形成 CMake 环。

最终方案：在 `dynamic_app_registry.c` 直接硬编码 UTF-8 字面量字节序列（与 app_fonts.h 同步，但**两份独立维护**），跟之前 `dynamic_app_ui.c` 里硬编码 token 颜色同款做法。代价：新增图标要改两处。

---

## 2. 落地清单

### 2.1 阶段 A：清理裸 .js 上传链路

**删除（13 个文件）**
- `dynamic_app/scripts/{calc,weather,music,notes,hello,timer,game2048,mole,reaction,aquarium,echo,imgdemo,alarm}.js`

**新建**
- `dynamic_app/scripts/alarm_pkg/{main.js, manifest.json}`（alarm.js 转包格式）

**PC 端清理（4 处机械删除）**
- `tools/dynapp_uploader/__init__.py`：docstring 改成 `upload_app_pack` 示例
- `tools/dynapp_uploader/client.py`：删 `upload_app()`（~18 行）
- `tools/companion/providers/upload_provider.py`：删 `kind == "file"` 分支
- `tools/companion/gui/pages/upload.py`：删"选 .js"按钮 + `_pick_file` + `_file_path` + `_do_upload` 中 elif 分支（~25 行）

**新建文档**
- `docs/动态app_包格式规范.md`（~190 行，覆盖目录布局 / 字段格式 / 上传步骤 / 字符集与长度限制）

### 2.2 阶段 B：图标走 TTF 字体路径

**字体子集扩展**
- `app/fonts/material_icons_subset.ttf` 用 pyftsubset 重新生成：20 → 33 codepoint（4.4KB → 6.8KB）
  - 关键 flags：`--drop-tables+=DSIG,GPOS,GSUB,kern,LTSH,VDMT,VORG,STAT,HVAR,MVAR,gvar,fvar,avar`
  - 不删 fvar/avar 会触发 fontTools 内部 KeyError，删了文件正常

**固件改动（6 个文件）**
- `app/app_fonts.h`：新增 13 个 `ICON_*` 宏（ALARM/TIMER/HABIT/NOTE/GAME/CALCULATOR/IMAGE_PIC/MEMORY/DASHBOARD/PUZZLE/TARGET/PETS/CAMPAIGN）
- `storage/littlefs/dynapp_script_store.h/.c`：`dynapp_manifest_t` 加 `icon[24]` + `icon_color[16]`；解析层读取这两字段（可选，缺失不报错）
- `dynamic_app/dynamic_app_registry.h/.c`：
  - `dynamic_app_entry_t` 加 `icon[8]` UTF-8 + `icon_color` uint32_t
  - 新增 31 项 `k_icon_table` + 13 项 `k_color_table` 查表
  - `registry_list` 翻译并透传
- `dynamic_app/dynamic_app_natives.c`：`sys.icons.*` 暴露新 13 个 codepoint，让动态 app 内部 label 也能用这些图标
- `app/apps/launcher/pages/page_launcher.c`：
  - `cell_def_t` 删 `dyn_has_image` + `dyn_icon_path[80]`，加 `dyn_icon[8]`
  - 删 `<sys/stat.h>` + `DYNAPP_COLOR` 硬编码
  - `cells_collect` 动态 app 分支：从 manifest 读 icon/color，回退到 `ICON_APPS` + 灰
  - `create_cell_obj` 静态/动态走同一 `lv_label + 36px 字体` 路径

**PC 端工具替换**
- 删 `tools/make_pack_icon.py`（PNG 路径，已废弃）
- 新建 `tools/make_pack_manifest.py`（生成 manifest.json，含字段校验）

**6 个包全部刷新**
- 重写 `manifest.json`（含 icon/iconColor 字段）
- 删除 12 个旧 `icon.bin` / `icon.png` 文件

| 包 | icon | iconColor |
|---|---|---|
| `notif_pkg` | NOTIFICATIONS | ACCENT |
| `alarm_pkg` | ALARM | WARN |
| `dash` | DASHBOARD | ACCENT_2 |
| `habit_pkg` | HABIT | OK |
| `imgdemo_pkg` | IMAGE | INFO |
| `memory_pkg` | PUZZLE | ACCENT_2 |

**文档同步**
- `docs/动态app_包格式规范.md`：manifest 章节重写（icon/iconColor 字段表 + 可用值）；icon.bin 章节标记废弃
- `docs/动态app_UI设计系统.md`：字体覆盖范围更新到 33 codepoint + 加图标 6 步流程

---

## 3. 几次"踩坑 + 修复"

### 3.1 launcher 看不到 notif_pkg（关键 bug）

第一次 PC 端上传 notif_pkg 后，launcher 上没出现，PC GUI 列表也查不到。加了详细 list 调试日志后真相浮现：

```
W dynapp_script: list:   'notif_pkg' OK but out full (n=8 max=8)
```

`dynamic_app_registry_list` 函数里中转 buffer **写死 8 行**：

```c
char fs_names[8][...];  // ← launcher 传 max=16 也救不回来
```

11 个 app 按字典序，notif_pkg 排在第 9 位被砍。`dynapp_fs_worker.c` 的 `FS_LIST_MAX=8` 也是同样问题。

修复：两处都提到 24（PC GUI list 和 launcher 都受益）。

### 3.2 第一版图标小、不像原生

PNG 32×32 路径生成的图标：
- 在 80×88 cell 里看着偏小（原生用 36px 字体填满 cell 主区）
- 圆角矩形彩底+居中字符 ≠ 原生"无底色 + 纯彩字符"风格

教训：**做 UI 别想当然以为"PIL 拼装就够了"**。原生 launcher 的视觉本质是矢量字体直接画，位图怎么处理都差一档。第二版直接转向 TTF 字体路径，视觉立刻和原生一致。

### 3.3 fontTools subset 删 fvar 触发 avar 解析 KeyError

第一次跑 pyftsubset 加 `--drop-tables+=DSIG,GPOS,GSUB,kern,...,fvar,gvar` 时报错：

```
KeyError: 'fvar'  in _a_v_a_r.py decompile
```

avar 表内部要读 fvar，提前删 fvar 就崩。

修复：把 `avar` 也加到 drop-tables 里，让两者一起删（这是可变字体相关的表，子集化场景下都不需要）。

### 3.4 名字显示为 "notif_pkg" 而不是中文

第一次上传时没有手写 manifest，PC `upload_app_pack` 自动生成 fallback `{"id":"notif_pkg","name":"notif_pkg"}`。launcher 显示 `notif_pkg` 9 个 ASCII 字符在 80px cell 里被截断，看起来像"没名字"。

修复：每个包都手写 manifest.json 用中文 name。

### 3.5 dynamic_app 想 include app/app_fonts.h 触发循环依赖

`app` 组件已 `REQUIRES dynamic_app`（page_dynapp_host 调 dynamic_app_*），如果 dynamic_app 反向依赖 app/ 的头会形成 CMake 环。

修复：与 `dynamic_app_ui.c` 同款做法，在 `dynamic_app_registry.c` 直接硬编码 UTF-8 字面量字节序列（与 `app_fonts.h::ICON_*` 同步但独立维护）。

### 3.6 Edit 工具吞 PUA codepoint 字符

写 `make_pack_icon.py` 时直接在 ICONS 字典里塞 `"ALARM": ""` 这种 PUA 字符，Edit 工具读写时把它们当空字符串处理（终端也显示成空格）。

修复：Python 脚本侧用 `chr(92) + 'u' + format(cp, '04X')` 拼出 `\uXXXX` 转义序列，再用 Python heredoc Write 整段。

---

## 4. 文件改动清单

### 新建（4）
- `dynamic_app/scripts/alarm_pkg/main.js`（从 alarm.js 移）
- `dynamic_app/scripts/alarm_pkg/manifest.json`
- `tools/make_pack_manifest.py`
- `docs/动态app_包格式规范.md`
- `docs/动态app_上传链路清理_TTF图标_工作日志.md`（本文件）

### 删除（合计 25+ 项）
- 13 个孤儿裸 .js（alarm/calc/weather/music/notes/hello/timer/game2048/mole/reaction/aquarium/echo/imgdemo）
- 6 个包的 `icon.bin` + `icon.png`（共 12 个）
- `tools/make_pack_icon.py`（PNG 路径）
- `tools/dynapp_uploader/client.py::upload_app()`（~18 行）

### 修改（重要）
- `app/fonts/material_icons_subset.ttf` —— pyftsubset 重生成 33 codepoint（6.8KB）
- `app/app_fonts.h` —— 加 13 个业务 ICON_* 宏
- `storage/littlefs/dynapp_script_store.h/.c` —— manifest_t 加 icon/icon_color 字段，解析层兼容
- `dynamic_app/dynamic_app_registry.h/.c` —— entry_t 加 icon/icon_color；新增图标/颜色名字查表（31+13）
- `dynamic_app/dynamic_app_natives.c` —— sys.icons.* 暴露新 13 个 codepoint
- `app/apps/launcher/pages/page_launcher.c` —— 删 image 渲染路径，统一字体渲染
- `dynamic_app/scripts/{notif,alarm,dash,habit,imgdemo,memory}_pkg/manifest.json` —— 6 个全部含 icon/iconColor 字段
- `tools/dynapp_uploader/__init__.py` —— docstring 切到 upload_app_pack
- `tools/companion/providers/upload_provider.py` —— 删 kind==file 分支
- `tools/companion/gui/pages/upload.py` —— 删选 .js 按钮 + _pick_file + _file_path 字段
- `docs/动态app_包格式规范.md` —— manifest 章节重写
- `docs/动态app_UI设计系统.md` —— §5.2 字体覆盖范围更新到 33

---

## 5. 验证

### 5.1 上传链路清理
- [x] 仓库 `dynamic_app/scripts/` 只剩 6 个 _pkg + prelude.js（之前 6 个 _pkg + prelude.js + 13 个孤儿 .js）
- [x] PC GUI 上传页只剩"选目录"按钮（"选 .js"已删）
- [x] `grep -rn "upload_app\b\|kind == \"file\"\|_pick_file\|_file_path" tools/` 无残留
- [x] 现有动态 app（dash 等）功能不受影响——固件零改动，运行时行为完全一致

### 5.2 图标 TTF 路径
- [x] 6 个包全部上传后 launcher 显示 36px 矢量图标 + 各自配色
- [x] 通知 app 蓝铃铛、闹钟橙铃铛、仪表盘紫圆盘、习惯绿勾、图片浅蓝、记忆紫拼图，**视觉与原生完全一致**
- [x] 中文名显示正确（"通知" / "闹钟" / "仪表盘" / "习惯" / "图片演示" / "记忆游戏"）
- [x] 包体积比之前小：每个 pkg 少 2KB icon.bin
- [x] 字体子集从 4.4KB → 6.8KB（多了 13 个 codepoint，~50% 字符增长合理）

### 5.3 launcher 静态/动态一致性
- [x] 静态 app（lockscreen / weather / 通知 / 音乐 / 系统 / 设置 / 时钟）和动态 app 走同一字体渲染路径
- [x] launcher 删了 `<sys/stat.h>` / `DYNAPP_COLOR` / `dyn_has_image` 全部残留
- [x] cell 大小、字号、圆角、按下高亮所有视觉参数完全统一

### 5.4 不破坏旧动态 app
- [x] 老脚本（dash / habit_pkg / imgdemo_pkg / memory_pkg）功能完整跑
- [x] 老脚本未配置 manifest.icon 时 launcher 自动回退 ICON_APPS + 灰
- [x] sys.app.saveState 数据保留（NVS 没动）

---

## 6. 不在本次范围（下一轮）

- ❌ `sys.app.exit()` JS 主动回 launcher（屏底上滑已能用）
- ❌ `lv_arc / lv_slider` 控件原语（写仪表盘类 app 时再补）
- ❌ sub_router push/pop（多级页面 app 时再补）
- ❌ 删 notif_pkg1 测试副本
- ❌ 复刻其它原生 app（设置 / 时钟 / 系统）的动态版

---

## 7. 关键经验沉淀

### 7.1 "审计先行"避免误判工作量
最初判断"清裸 .js 链路"可能要改 BLE 协议、迁移设备数据、动固件。审计后发现固件根本不知道有"裸 .js"概念，PC 端 4 处机械删除就完事，**实际工作量 25 分钟，而不是预估的 2 小时**。
**教训**：跨层的"重构"在动手前必须做完整审计——很多看起来大的改动只是表层。

### 7.2 用户反馈优于纸面方案
我推 PNG 路线时还在自我感觉良好（"32×32 圆角彩底很 iOS"），用户烧上去一看立刻指出"图标小+不要背景+和原生不一致"。第二天改 TTF 路径才真正达标。
**教训**：UI 类的设计决策**必须先烧上设备肉眼对比**，PC 端浏览器/PIL 预览不可靠。

### 7.3 List 函数中转 buffer 是隐藏雷点
`dynamic_app_registry_list` 内部用 `char[8][...]` 中转，调用方传 `max=16` 也被砍到 8。这种"漏斗形"截断在代码里很常见——上层传更大的容量给下层，下层却有自己的常量。
**教训**：写涉及"列表收集"的函数时，**接受方容量**和**内部中转容量**应该作为参数透传，或者文档明确说明上限。

### 7.4 字体子集化的常见陷阱
- **不删 fvar/avar 会让文件留 90KB**（默认很多变体相关表）
- **同时删 fvar 不删 avar 会触发 KeyError**（解析依赖关系）
- **正确组合**：`drop-tables+=DSIG,GPOS,GSUB,kern,LTSH,VDMT,VORG,STAT,HVAR,MVAR,gvar,fvar,avar` 一次到位

### 7.5 LVGL 静态/动态渲染合一是个胜利
之前 launcher 为动态 app 维护单独的 image 渲染路径，逻辑上很别扭——两条路径意味着两份 bug 风险。改成 manifest 提供 codepoint + 颜色后，static_app 和 dynamic_app cell 走完全相同的代码路径，**条件分支彻底消失**。
**教训**：当两套实现服务同一抽象（"在 launcher 上画一个 cell"）时，找出共同的最小数据结构（这里是 `{icon_codepoint, color}`），让两边汇流到同一渲染路径。

### 7.6 manifest 字段做"字符串桥接"比直接传值更可读
`"icon": "ALARM"` 比 `"icon": ""` 直观得多，工具脚本写起来也舒服（直接写名字而不是查 codepoint 表）。代价是固件里要有一张翻译表，但表只在一个地方维护，很集中。
**教训**：对人类可见的配置字段，优先用语义名字而不是数值——查表的小代价换可读性。

### 7.7 工具链复杂度也是地基的一部分
最初 PNG 路径要装 PIL 还要调 LVGLImage.py，TTF 路径只要 fontTools 一次性子集化就行（用户后续不用再跑工具）。**工具链每多一步用户体验就降一档**——动态 app 写 manifest 改 icon 字段是 0 工具操作，比"重新跑 PIL 出 PNG"友好得多。

---

## 8. 一句话总结

把"PC 端 4 处便利包装"和"动态 app launcher 图标 PNG 路径"两条历史包袱一起卸了：上传链路只留包目录一条干道，launcher 图标改用 manifest 字段查表 + 36px 矢量字体渲染，与原生完全同款，仓库再瘦一圈，视觉再统一一档。
