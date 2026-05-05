# 动态 App（MicroQuickJS）—— README 更新 / WiFi 取舍 / LittleFS 脚本落盘 / BLE 上传服务 / PC 推送工具工作日志

日期：2026-04-28

> 本日志由 `对话记录1.md` 整理而来，目标是把“做了什么、为什么这么做、改动影响面、如何验收”落成可追溯的工作记录。

## 背景

动态 App（MicroQuickJS）框架在 v0.8 已经初步跑通：JS 脚本通过 runtime 执行，UI 通过 LVGL 桥接，PC 端通过 providers 提供联网能力。但当时仍有三个明显缺口：

1. **文档叙事滞后**：README 还停留在 v0.7（control service 退役版本），没有把动态 App 系统讲清楚
2. **“动态”不彻底**：脚本多数仍是“编进固件”，改脚本需要重新烧录；开发循环不够短
3. **闭环缺失**：缺少一条“PC 一键推脚本 → 设备落盘 → 菜单出现 → 点击运行”的全链路能力

因此这一轮的主目标是：**把动态 App 从“能跑”推进到“可开发、可迭代、可交付”的形态**。

---

## 0) README 更新：把 v0.8 动态 App 系统讲清楚

### 0.1 更新范围

README 主要补齐以下内容：

- 顶部一句话定位更新（强调动态 App 卖点）
- 功能概览拆成两张表：内建页面（C）与动态 App（JS 脚本），列出内置 JS app 以及是否依赖 PC provider
- 目录树补齐动态 App 相关目录：`dynamic_app/`、`services/dynapp_bridge_service/`、PC 端 tools/providers、SDK/companion 等
- BLE 协议表补齐 dynapp_bridge 透传通道条目
- 新增章节“动态 App 框架（MicroQuickJS）”：三层架构图、prelude 标准库速查、新增 app 的四步流程、bridge 容量约束、prepare→ready→commit 切屏流程
- 依赖表补齐 `esp-mquickjs`
- 版本历史新增 v0.8（2026-04-27）并修正 v0.6/v0.7 顺序错乱

### 0.2 不改的边界

与动态 App 无关的硬件配置/引脚/内存布局/分区表/字体子集流程/调试技巧等不在本轮调整范围，避免 README 变成“无重点的大杂烩”。

---

## 1) WiFi 是否要做：先问“为什么”，再决定“怎么做”

用户提出“接下来实现 WiFi 功能”，这一轮先把需求拆解成目标导向的三类，避免“为了有 WiFi 而做 WiFi”：

1. **脱离 PC 独立联网**（直接拉天气 API、NTP 对时）
2. **OTA 升级**（分区表需要从 factory 模式改成 ota_0/ota_1，涉及整体规划）
3. **动态 App 远程脚本下载**（需要 HTTP 下载 + 落地到 FS/NVS）

结论是：在当前“PC 作为网关”的架构里，动态 App 已经能通过 BLE→PC providers 获得联网能力；除非明确要“脱离 PC 独立运行”或“OTA”，否则 WiFi 的边际收益不高，且会引入 WiFi/BLE 共存吞吐下降、内存占用上升、配网复杂度等成本。

这一段讨论的价值在于：把后续路线从“功能堆叠”扳回“按目标分期交付”。

---

## 2) 动态 App 真正动态化：LittleFS + 菜单动态列举 + BLE 上传

### 2.1 路线拆解（按依赖顺序）

为了让“改脚本不用烧录”，链路必须按依赖顺序推进：

1. **存储介质就位**：`storage` 分区从 SPIFFS 切 LittleFS，并提供 mount 与基础文件操作
2. **脚本注册与读取**：registry 支持从 FS 读取脚本，并能列举 FS 上存在的 app 名单
3. **菜单数据驱动化**：菜单页不写死 14 个 item，而是能“静态项 + 动态项（来自 registry list）”循环渲染
4. **上传服务**：PC 通过 BLE 把脚本写进 `/littlefs/apps/`

其中第 4 步是“体验质变点”：做完就形成“保存脚本→一键推送→菜单出现→点击运行”的闭环。

---

## 3) 存储分层：通用 LittleFS 挂载 vs 动态 App 脚本存储

围绕“文件系统该不该放 drivers”的讨论，最终采用两层拆分：

- `storage/fs_littlefs.*`：通用层，职责是 mount/unmount、根路径常量（如 `/littlefs`）、容量信息、错误码归一等；不出现“app”语义
- `services/dynapp_storage.*`：业务层，把 app 名映射到 `/littlefs/apps/<name>.js`，并提供 read/write/list/delete/exists 等 API

关键点：

- 业务规则（路径约定、命名校验、覆盖策略）必须在业务层收口，避免 drivers 层污染
- 写入语义采用 `.tmp + rename`：保证读者永远看到“旧版或新版”，不会看到半截文件；同时对断电更稳
- init 时清理孤儿 `.tmp`：避免上次中断导致 FS 里残留临时文件

---

## 4) BLE 上传服务：为什么必须“入队 + worker task”，不能在 access_cb 写盘

### 4.1 项目先例：BLE 回调永远不做持久化

对话里专门盘点了 NVS 的读写线程与调用链，结论是：

- NVS 写主要发生在 UI 线程（防抖落盘），Script 线程也有 `sys.app.saveState` 这种高自由度入口
- **没有任何持久化写发生在 BLE 的 access_cb 回调中**（access_cb 的职责是解析/入队，实际写入由消费线程完成）

这条先例在引入 LittleFS 后变成硬约束：LittleFS 写入最坏延迟比 NVS 大一个数量级，不可能放在 BLE host task 做。

### 4.2 线程模型与边界

采用“service + worker”的结构：

- `dynapp_upload_service`：GATT 表 + 拆帧 + 入队 + status notify
- `dynapp_upload_worker`：独立 task + 队列 + 状态机，负责真正的流式写盘、CRC32 校验、END 时 commit（rename）

边界验证点：

- worker 不 include NimBLE 相关头文件（关注点分离）
- access_cb 做到微秒级返回（只 memcpy + xQueueSend），保证 BLE 稳定性
- worker 最坏 100~500ms 的写盘延迟也不会拖慢 UI/Script 线程

---

## 5) 上传协议：START/CHUNK/END 二进制帧（“三段式”不是为了复杂）

对话中明确了为什么不做“一波传完”的单帧上传：

- BLE 单帧净荷有限（受 MTU 与协议头影响）
- 需要 END 触发一次性校验与 commit，配合 `.tmp + rename` 保证原子性
- PC 端采用“请求-响应”：每帧 `_send_and_wait`，固件回得慢 → PC 自然慢下来，节流无需额外机制

协议还顺带支持 list/delete/format 等运维命令（是否拆 control char 取决于 GATT 表复杂度与维护成本）。

---

## 6) PC 端推送工具：SDK 与 GUI 分离

PC 端实现采用“SDK + GUI”分层：

- `tools/dynapp_uploader/`：协议 pack/parse、CRC、切片、UploaderClient（请求-响应模型）
- `tools/dynapp_push_gui.py`：GUI 入口，负责连接、上传、列表、删除、日志展示

关键实现决策：

- asyncio loop 放后台线程跑，UI 主线程通过 `run_coroutine_threadsafe` 提交任务，避免 UI 卡死
- 进度更新切回 UI 线程（例如 `widget.after(0, ...)`）
- chunk 大小按“frame 上限 - header - offset”等硬约束核算，确保在固件容忍范围内

---

## 7) 验收标准（建议按这个顺序验）

1. 固件侧：build/flash 后启动日志能看到 upload service 注册成功
2. PC 侧：GUI 能 connect、能 list、能 upload 一个小脚本
3. 设备侧：回到菜单能看到新 app 入口，点击能进入运行（prepare/commit 流程正常）
4. 回归：上传过程中 UI 不掉帧，BLE 不掉连接；异常帧/队列满/CRC mismatch 能在 PC 日志里看见并能重试

---

## 8) 经验沉淀

1. **先补齐“文档叙事”再加功能**：README 不同步会直接造成沟通成本爆炸，后续每个新成员都要重新解释一遍系统
2. **“动态”不是口号，是开发循环**：能跑的 JS 引擎不等于动态系统，必须有落盘、列举、菜单入口、上传工具这一整条链路
3. **只要进入 BLE host task，就要假设自己在“硬实时边缘”**：access_cb 的工作量必须被严格限制，持久化这种慢操作必然要异步化

