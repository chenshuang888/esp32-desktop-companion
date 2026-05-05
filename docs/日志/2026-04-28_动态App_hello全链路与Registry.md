# 动态 App（MicroQuickJS）—— hello 脚本全链路测试 / Registry 单源化 / 菜单热刷新与长按删除工作日志

日期：2026-04-28

> 本日志由 `对话记录2.md` 整理而来，覆盖从“写 hello.js 验证上传链路”到“registry 单源化 + 菜单体验闭环”的全过程。

## 背景

在完成动态 App 的 LittleFS 上传链路之后，系统进入“从能用到好用”的阶段，主要痛点集中在三类：

1. **缺少端到端验证脚本**：需要一个最小 app 来验证“上传→落盘→菜单出现→点击运行→可交互”
2. **脚本来源语义不清**：内嵌脚本与 FS 脚本同时存在时，覆盖规则与展示逻辑容易让人困惑（尤其是同名遮蔽）
3. **菜单体验不闭环**：推送后要“返回再进菜单”才能看到新 app；设备侧也无法本地删除 FS app

这一轮围绕上述三点做了三次推进：hello.js 全链路测试 → registry 单源化 → 菜单热刷新+长按删除（本地管理闭环）。

---

## 0) 快速画像：项目结构梳理（用于后续定位改动点）

对话开头先把项目结构做了快速梳理，核心结论是：

- `main/main.c` 负责启动顺序（persist/NVS → LittleFS → managers → NimBLE/services → app_main_init）
- `services/` 既有 5 个业务 BLE service，也有动态 App 相关 service（bridge/upload）
- `storage/` 已按介质拆分（nvs/ 与 littlefs/），动态脚本从 LittleFS 加载
- `dynamic_app/` 提供 registry/runtime/natives/ui，菜单页与动态 app 页在 `app/pages/`
- `tools/` 提供 PC 端上传工具与 GUI

这个梳理的目的：避免后续改造“写着写着改穿层”，让每个变化都能落在合适的层里。

---

## 1) 写最小测试脚本 `hello.js`：验证“上传→菜单→运行”全链路

### 1.1 为什么要新名字（避免同名遮蔽）

在“双源（内嵌+FS）”的版本里，registry 策略是“内嵌优先”，因此 **FS 上同名脚本会被忽略**。为了让测试有意义，脚本必须用一个不与内嵌 7 个 app 冲突的名字：

- 内嵌 7 个名字：`alarm/calc/timer/2048/echo/weather/music`
- 测试脚本选择：`hello`

只有这样，hello 才必须从 `/littlefs/apps/hello.js` 读取出来才能运行，从而真正验证上传链路。

### 1.2 hello.js 覆盖的测试目标

hello.js 被设计为“最小但能覆盖关键链路”的脚本：

- 页面基础渲染正常（标题 + 文本）
- 定时刷新（例如 uptime 每秒更新一次）
- 按钮点击事件可用（点一次计数 +1，验证 dispatcher/onClick 冒泡链路）
- BLE 状态可读（例如 `sys.ble.isConnected`），验证标准库与基础 native API 可用

### 1.3 建议验收步骤（当时给出的跑法）

1. build/flash/monitor（由用户自行执行）
2. 断开其他 BLE 工具（单连接约束）
3. 用 `tools/dynapp_push_gui.py` 推送 hello.js
4. 设备回菜单重新进入（当时菜单是 create 时枚举一次），应能看到 hello
5. 点击进入运行，验证 UI/按钮/定时器/状态显示

---

## 2) 菜单图标怎么来：不是随机，是“硬编码 + 兜底”

用户问到“FS 动态 app 的图标怎么获取”，当时梳理的事实是：

- 旧逻辑里：内嵌白名单 app 各自硬编码图标
- FS 新 app（如 hello）不在白名单：统一走同一个兜底图标（每次都一样，不是随机）

并提出了 4 条演进路径（按成本从低到高）：

1. 纯硬编码 if-else（只适合少量内嵌 app）
2. 同名 `.meta` 文件（例如 `hello.meta`，一行 JSON：icon/title）
3. 脚本头注释自描述（`// @icon:` `// @title:`，C 侧轻解析）
4. 完整 manifest（协议升级，元数据与资源体系化）

当时结论是：当前目标是验证链路，不做图标/资源系统，收益太小，先留作后续演进。

---

## 3) 内嵌 vs 下载：从“双源”走向“单源（仅 FS）”

### 3.1 触发点：同名遮蔽 + 叙事不一致

在“实现了下载运行能力”之后，双源会带来典型困惑：

- FS 上传了 `calc.js`，但菜单/registry 实际跑的是内嵌 calc（同名遮蔽）
- PC 工具为了展示“看起来有东西”，会把内嵌列表与 FS 列表合并，进一步模糊真实来源

因此提出“对齐手机系统/应用商店两层模型”的方向：**动态 app 只来源于 FS**；固件内只保留 runtime 标准库（prelude.js）等基础设施。

### 3.2 改造内容（集中在 5 个文件，便于回滚）

单源化改造集中在以下 5 个文件：

- `dynamic_app/CMakeLists.txt`：EMBED_TXTFILES 只剩 `prelude.js`
- `dynamic_app/dynamic_app_registry.c`：重写为纯 FS（移除内嵌表与双源逻辑）
- `dynamic_app/dynamic_app_registry.h`：注释/结构体对齐单源语义（移除 builtin 字段）
- `app/pages/page_menu.c`：display_name/icon 获取逻辑化简为“空壳 hook”（为未来 manifest 预留扩展点）
- `app/pages/page_dynamic_app.c`：pending 默认值从 `"alarm"` 改为 `""`（去掉“默认内嵌 app”的假设）

同时明确了一条工程实践：

- `dynamic_app/scripts/` 下的 7 个 `.js` **物理保留在仓库**（作为示例/PC 推送样板），但不再编进固件

### 3.3 行为变化（必须记录清楚）

单源化后：

- 出厂 FS 为空时，菜单动态区为空（没有“自带脚本 app”）
- 推送 hello.js 后菜单才出现入口
- PC 工具的 Apps 列表应只反映固件 LIST 返回的 FS 名单

这也是后续“菜单热刷新”必须做的原因：单源化后，推送是常态，“推完还要手动返回再进菜单”会很别扭。

---

## 4) 菜单体验闭环：上传后热刷新 + 长按删除

### 4.1 热刷新：推送完成后不需要“返回再进菜单”

做法是“跨线程只传状态，不直接碰 UI”：

- 上传完成事件发生在 manager/consumer task：只 set `volatile` dirty flag
- UI 线程的 `page_menu_update()` poll 并清 dirty：重建动态 item 列表

这样即使用户不在菜单页，dirty 也会保留；切回菜单后 update 触发一次刷新，符合预期。

### 4.2 长按删除：设备侧闭环管理

核心目标：设备上能直接删掉 FS app，不必回 PC。

关键实现点：

- 新增 `app/pages/page_menu_modal.*`：半透明遮罩 + 居中卡片 + Cancel/Delete 按钮
- 菜单项长按触发删除确认；Delete 走统一删除路径
- 删除路径统一调用 `dynapp_upload_submit_delete()`：与 PC 端远程删除复用同一个队列、同一 BUSY 拦截、同一事件广播
- 运行中拦截：upload manager 暴露 `set_running_check` hook，由 `app_main.c` 注入 `is_app_running`，避免循环依赖（保持项目既有“register callback 注入”的解耦风格）

### 4.3 改动文件清单（热刷新 + 删除）

对话中记录的改动清单如下：

- `services/manager/dynapp_upload_manager.h`：新增 register_status_cb + set_running_check API
- `services/manager/dynapp_upload_manager.c`：单 cb→数组；delete 调 running_check 拒绝删除运行中的 app
- `app/app_main.c`：注入 `is_app_running` 作为 running_check
- `app/pages/page_menu_modal.h`（新增）：删除确认对话框 API
- `app/pages/page_menu_modal.c`（新增）：遮罩 + 卡片 + Cancel/Delete
- `app/CMakeLists.txt`：加入 `page_menu_modal.c`
- `app/pages/page_menu.c`：`s_dirty`/`on_upload_status`/重建动态项/长按事件/删除确认回调

---

## 5) PC GUI 同步：单源化后不再展示“内嵌 app”

单源化后，PC GUI 的 Apps 视图应只展示 FS 上真实存在的 app：

- 删除 `BUILTIN_APP_NAMES` 常量与 export
- Apps 列表只渲染 LIST op 返回的 `fs_names`
- 所有列表项均可选可删（不再存在 builtin 的“不可删除”逻辑）

---

## 6) 验收标准（建议按这个顺序验）

1. 单源化后首次进入菜单：动态区为空（符合预期）
2. 推送 hello.js：菜单无需离开即可刷新出现 hello
3. 点击 hello：能进入运行并正常交互
4. 长按 hello：弹确认框；Cancel 不删；Delete 删除并自动刷新
5. 运行态保护：hello 运行中发 delete，返回 BUSY，hello 不被破坏
6. 回归：多次进出菜单、反复推送/删除，回调注册不重复（幂等）

---

## 7) 经验沉淀

1. **“同名遮蔽”不是小毛病，是来源语义没定清**：双源在规模上来后会引发大量“我明明推了，为什么没生效”的沟通成本
2. **改大架构要集中改动点**：单源化收敛到 5 个文件，diff 清晰、可回滚，风险可控
3. **跨线程 UI 更新只传 flag**：manager/worker/task 里不碰 LVGL，把 UI 更新收口到 UI 线程是最稳的纪律

