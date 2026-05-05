# 动态 App（MicroQuickJS）Label 自注册能力：工作日志

日期：2026-04-23

## 背景

为了增强动态 App（MicroQuickJS）脚本侧的 UI 能力，希望脚本可以“自己注册/创建 label”，而不是只能操作固件预先注册好的控件；同时需要保持线程安全与权限隔离：

- 脚本线程禁止直接调用 LVGL
- **纵向列表（简单）**的 label 父容器只允许在 `PAGE_DYNAMIC_APP` 内创建（避免脚本跨页面/跨权限创建 UI）

## 目标

1. 新增 JS API：脚本可请求创建 label，并通过 id 操作文本
2. 强制权限边界：仅 `PAGE_DYNAMIC_APP` 生命周期内允许创建（root 未设置时返回失败）
3. Dynamic App 页面使用“纵向列表 root”作为脚本创建控件的挂载点
4. 同步更新 MVP 设计文档，避免实现与文档脱节

## 方案与实现摘要

### 1) UI 命令扩展：新增 CREATE_LABEL

- 在 `dynamic_app/dynamic_app_ui.h` 扩展 UI 指令类型：
  - `DYNAMIC_APP_UI_CMD_CREATE_LABEL`
- 新增 enqueue API：
  - `dynamic_app_ui_enqueue_create_label(id, id_len)`
  - 仅当 Dynamic App 页面设置过 root（`dynamic_app_ui_set_root()`）时允许入队
- 在 `dynamic_app/dynamic_app_ui.c` 的 `dynamic_app_ui_drain()` 中处理 CREATE_LABEL：
  - 校验 root 有效且属于 `PAGE_DYNAMIC_APP` 生命周期
  - 幂等：同 id 已存在且对象有效时不重复创建
  - 创建 `lv_label` 并注册到 `id -> lv_obj_t*` 的 registry

备注：registry 上限从 8 提升为 16（MVP 级别防止过快耗尽）。

### 2) JS 绑定：新增 `sys.ui.createLabel(id) -> boolean`

- 在 `dynamic_app/dynamic_app.c` 增加绑定函数 `js_sys_ui_create_label()`：
  - 入参：`id`
  - 行为：调用 `dynamic_app_ui_enqueue_create_label()` 投递创建请求
  - 返回：创建请求是否成功入队（仅表示“请求入队成功”，不是“已渲染完成”）
- 兼容 MicroQuickJS 的 API 差异：`JS_NewBool` 签名为 `JS_NewBool(int val)`，修复为 `JS_NewBool(ok ? 1 : 0)`

### 3) 页面改造：`PAGE_DYNAMIC_APP` 提供纵向列表 root 并绑定权限

- `app/pages/page_dynamic_app.c`：
  - 新增 `list_root` 容器（flex column）作为脚本 label 的父容器
  - 页面 init 时调用 `dynamic_app_ui_set_root(list_root)`，然后 `dynamic_app_start()`
  - 页面 destroy 时先 `dynamic_app_ui_set_root(NULL)` 关闭门禁，再 stop 脚本、清 registry

### 4) 示例脚本更新

- `dynamic_app/scripts/app.js`：
  - 启动时先 `sys.ui.createLabel("time")`
  - 再 `sys.ui.setText("time", sys.time.uptimeStr())` 并定时刷新

## 文档同步

更新 `docs/动态App(MicroQuickJS)_MVP设计.md`，补齐并修正以下内容：

- 页面 create 流程从“固件预注册 time label”改为“设置 root + 脚本 createLabel”
- 新增 JS API `sys.ui.createLabel(id) -> boolean` 的说明
- 强调仅 `PAGE_DYNAMIC_APP` 生效（root 清空后会返回 false）
- 说明布局为纵向列表 root（用于挂载脚本动态创建的 label）

## 构建与验证

- 用户执行 `idf.py build` 时遇到编译错误：`JS_NewBool` 参数数量不匹配
- 已修复：将 `JS_NewBool(ctx, ok)` 改为 `JS_NewBool(ok ? 1 : 0)`
- 功能性验证：创建与 setText 行为可用（具体跑屏/串口验证由用户环境执行）

## 已知限制（MVP）

1. 当前仅支持：创建 label + setText；样式（颜色/对齐/字体）与顺序/布局仍未开放给脚本
2. `sys.ui.createLabel()` 为 fire-and-forget：返回值仅表示“是否成功入队”，不保证立刻显示
3. registry 有上限（当前 16）；满时会丢弃创建请求（并打印 warning）

## 后续建议

- 若需要“列表中顺序/位置”能力：可补充 `moveTo(id, index)`（基于 `lv_obj_move_to_index`）
- 若需要“样式”能力：优先做受控的高层接口（如 `setTextColor/setAlign/setFontToken`），避免无边界暴露 LVGL
- 若动态 App 未来要扩展 BLE/WiFi 等能力：建议按“服务化 RPC + 事件回传”架构逐步引入权限与配额

