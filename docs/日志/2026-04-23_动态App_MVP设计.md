# 动态 App（MicroQuickJS）MVP 设计说明

> 目标：先把“JS 动态 App 能通过桥接调用 UI（LVGL）”跑通，形成可扩展的母版页面（`PAGE_DYNAMIC_APP`）。

## 1. 设计目标

- **脚本线程不直接触碰 LVGL**：所有 UI 操作必须通过队列交给 UI 线程执行。
- **可展示生命周期**：页面 `create/destroy` 触发 App `start/stop`，便于未来扩展“App 加载/卸载/权限”等能力。
- **MVP 优先可跑通**：
  - `app.js` 先通过 `EMBED_TXTFILES` 内嵌到固件（秒开、零环境干扰）。
  - 先实现 `sys.log()` 与 `sys.ui.setText()`，KV/NVS/权限系统后续再加。

## 2. 线程模型（Dual-Core）

- **UI Task（建议 Core 1）**
  - 运行 `lv_timer_handler()`（项目中通过 `lvgl_port_task_handler()` 调用）
  - 每次循环**先 drain UI 指令队列**，再执行页面 `update()` 与渲染
  - 入口：`app/app_main.c` 的 `ui_task()`

- **Script Task（建议 Core 0）**
  - 运行 MicroQuickJS VM + 简易事件循环（MVP：`setInterval/clearInterval`）
  - 通过 FreeRTOS Queue 向 UI 线程发送指令
  - 入口：`dynamic_app/dynamic_app.c` 的 `script_task()`

## 3. UI 指令桥（Script → UI）

- 指令结构体：`dynamic_app/dynamic_app_ui.h`
  - `DYNAMIC_APP_UI_CMD_SET_TEXT`：携带 `id` 与 `text`
  - payload 采用**固定长度字符串**（MVP：`id<=32`，`text<=128`）
  - 入队时做 **UTF-8 边界截断**，避免出现 `�`

- UI 线程消费：`dynamic_app_ui_drain(max_count)`
  - 限制每轮最多处理 `max_count` 条，避免 UI 卡顿
  - 查表（id → lv_obj_t*），仅对 Label 执行 `lv_label_set_text()`

## 4. App 生命周期

页面：`app/pages/page_dynamic_app.c`

- `create()`：
  - 创建页面布局（包含一个纵向列表 root 容器，用于挂载脚本动态创建的 label）
  - `dynamic_app_ui_set_root(list_root)`：设置脚本允许创建 label 的父容器（仅在 `PAGE_DYNAMIC_APP` 生命周期内）
  - 脚本侧通过 `sys.ui.createLabel(id)` 自己创建/注册需要的 label
  - `dynamic_app_start()`：通知 Script Task 创建 JSContext、执行 `app.js`

- `destroy()`：
  - `dynamic_app_stop()`：通知 Script Task 释放 JSContext
  - `dynamic_app_ui_unregister_all()`：清理 id→obj 注册表，避免悬挂指针

## 5. JS 侧 API（MVP）

通过全局对象 `sys` 暴露：

- `sys.log(msg)`
  - 映射到 `ESP_LOGI("dynapp", ...)`

- `sys.ui.createLabel(id) -> boolean`
  - **异步**：仅入队创建请求（fire-and-forget），实际创建发生在 UI Task 的 `dynamic_app_ui_drain()`
  - **仅 `PAGE_DYNAMIC_APP` 生效**：页面 destroy 后（root 清空）会返回 false
  - **幂等**：同 id 重复调用不会重复创建

- `sys.ui.setText(id, text)`
  - **异步**：仅入队 UI 指令（fire-and-forget）
  - UI 线程在下一帧执行 `lv_label_set_text`

定时器（MVP）：

- `setInterval(fn, ms)` / `clearInterval(id)`
  - Script Task 侧实现
  - 通过 `JSGCRef` 保活回调函数（避免 GC 回收）

## 6. 内存策略（MVP）

- JS VM 使用 **PSRAM 固定块堆**（当前：`1MB`）
  - 代码位置：`dynamic_app/dynamic_app.c` 中 `JS_HEAP_SIZE_BYTES`
  - 使用 `heap_caps_malloc(..., MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)`

## 7. 重要限制（MVP）

- **JS 线程禁止直接操作 LVGL 对象**：只能使用 `sys.ui.*` 发指令。
- **sys.ui.setText 是 fire-and-forget**：
  - 队列满时会丢弃（MVP：不阻塞 UI/脚本）
  - 后续如果需要“返回结果”，再引入 Promise/callback 机制
- **当前仅演示 UI 能力**：NVS/Wi-Fi/BLE 等能力建议按“权限 + 单写者/owner task”逐步扩展。

## 8. 组件补丁说明

为支持桥接与扩展，当前对 `managed_components/makgordon__esp-mquickjs` 做了两点轻量补丁：

1) `managed_components/makgordon__esp-mquickjs/CMakeLists.txt` 增加 `.` 到 `INCLUDE_DIRS`，以便项目侧可直接 `#include "mquickjs.h"`。
2) 新增 `esp_mqjs_get_stdlib_def()` / `esp_mqjs_get_stdlib_c_function_count()`，用于在运行时复制并“追加” C function table，从而创建 `sys.*` / `setInterval` 等 native API。

补丁脚本（建议在依赖下载后执行一次，确保可重复）：

```bash
python tools/patch_esp_mquickjs_component.py
```

> 后续如果要完全依赖组件管理器自动拉取版本，建议把这两点补丁 upstream 或在本仓库内做 vendor 固定版本。

## 9. 下一步建议

- `App Manifest + permissions`：在 JS 侧拦截未声明的 API（比如 BLE/Wi-Fi/NVS）。
- `sys.kv.set()`：建议先做异步 fire-and-forget，并遵循 `NVS single-writer` 契约。
- `app.js` 加载切到 SD/SPIFFS：实现“不刷固件改逻辑”，并补充热更新的回滚策略。
