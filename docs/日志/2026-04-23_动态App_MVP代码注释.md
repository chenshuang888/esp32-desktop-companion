# 动态 App（MicroQuickJS）MVP：代码注释补全工作日志

**日期：** 2026-04-23  
**目的：** 把近期新增但尚未完全理解的 Dynamic App（MicroQuickJS MVP）相关代码补上“通俗易懂、能串起整条链路”的注释，方便后续学习与维护。  

> 本次工作只补充/整理注释（含模块说明、线程模型、调用链路、关键取舍），不修改业务逻辑与功能行为。

---

## 1. 背景与问题

近期加入了“动态脚本 App（MicroQuickJS）MVP”的能力，涉及：

- 新增一个脚本运行时（Script Task）用于执行 JS（含 `setInterval` 事件循环）
- 通过“Script → UI 队列桥”把 UI 更新请求交给 UI 线程执行（避免跨线程直接操作 LVGL）
- 新增 `PAGE_DYNAMIC_APP` 演示页面与菜单入口
- 增加 `makgordon/esp-mquickjs` 依赖，并对 managed component 做幂等补丁（用于导出 stdlib 信息、修复 `-Werror` 编译问题等）

由于模块较多、线程与资源边界较“硬”，单靠代码本身不容易快速理解。因此需要把**“为什么这么写、谁在什么时候调用、线程约束是什么”**写成注释，形成可读的学习材料。

---

## 2. 本次注释补全的重点（学习视角）

### 2.1 最关键的红线：LVGL 线程模型

- **脚本线程（Script Task）禁止直接调用 LVGL**  
  LVGL 并非线程安全；脚本侧只能发“UI 指令”，由 UI 线程统一执行。

- **UI 线程通过 `dynamic_app_ui_drain()` 消费队列并执行 LVGL API**  
  这样可以把“跨线程 UI 更新”变成一条可控、可审计的路径。

### 2.2 为什么要“先 drain 再 update/render”

UI loop 里把 `dynamic_app_ui_drain()` 放在 `page_router_update()` / `lvgl_port_task_handler()` 之前，核心目标是：

- 尽量让“这一帧收到的脚本更新”在同一帧就渲染出来  
  避免肉眼可见的“延迟一帧”。

### 2.3 为什么要复制并扩展 stdlib C function table

为了在 JS 里提供 `sys.*` 以及 `setInterval/clearInterval` 等 native API，需要把上游 stdlib 的 C function table：

- **复制一份可写的表**
- **在末尾追加我们自己的函数定义**

否则如果直接改上游表（或无法获取表信息），就无法在运行时安全扩展 native API。

---

## 3. 涉及文件与“读代码顺序建议”

建议按以下顺序阅读（由外到内，最容易串起来）：

1) UI 主循环与 drain 位置：`app/app_main.c`  
2) 演示页面如何注册 label + 启动脚本：`app/pages/page_dynamic_app.c`  
3) Dynamic App 对外 API 与线程约束：`dynamic_app/dynamic_app.h`  
4) Script Task / sys.* / setInterval 实现：`dynamic_app/dynamic_app.c`  
5) 队列/registry/UTF-8 截断与 drain 逻辑：`dynamic_app/dynamic_app_ui.c`  
6) 演示脚本 app.js：`dynamic_app/scripts/app.js`

补充阅读（构建与依赖相关）：

- dynamic_app 组件构建与脚本嵌入：`dynamic_app/CMakeLists.txt`
- app 组件引入 dynamic_app 与页面源文件：`app/CMakeLists.txt`
- managed component 幂等补丁脚本：`tools/patch_esp_mquickjs_component.py`
- 依赖声明：`main/idf_component.yml`

---

## 4. 结果

### 4.1 注释内容类型

- **模块级注释**：每个模块的职责、边界、线程模型、调用链路
- **函数级注释**：参数含义、谁调用/何时调用、线程要求
- **关键点旁注**：解释“为什么这样设计”（而不仅是“做了什么”）

### 4.2 对照学习时的“核心链路”

从“页面显示时间每秒变化”这个效果出发，完整链路为：

`page_dynamic_app` 创建 label（id="time")  
→ 注册到 registry  
→ `dynamic_app_start()` 通知脚本任务启动  
→ JS `setInterval()` 定时触发  
→ JS 调用 `sys.ui.setText("time", text)`  
→ C 侧 enqueue 一条 UI 指令到队列  
→ UI 线程 `dynamic_app_ui_drain()` 出队并执行 `lv_label_set_text()`

---

## 5. 验证说明

本次仅补注释，默认不主动执行重型验证/全量构建。

如需你手动验证（建议）：

- `idf.py reconfigure`（依赖变动后确保 managed_components 正常）
- `idf.py build`（确认编译通过）
- 上电运行后，进入菜单的 `Dynamic App` 页面，观察：
  - 屏幕 label 是否每秒更新
  - 串口日志是否持续输出 `tick ...`

---

## 6. 后续可改进（不属于本次注释补全范围）

- 为 `sys.ui.setText()` 增加“返回结果/失败原因”（目前 MVP 阶段队列满会丢弃，属于 fire-and-forget）
- 扩展 UI 指令类型：例如 setColor/setVisible/setValue 等，并明确每类控件的支持范围
- 若未来支持从 SD/SPIFFS 加载脚本，需要补充：
  - 热更新/回滚策略
  - 权限模型（哪些 sys.* API 可以被脚本调用）

