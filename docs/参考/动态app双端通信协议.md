# 动态 App 双端通信协议规范

> 版本 1.0 — 2026-04-26
> 适用范围：基于本固件的 Dynamic App 与配套 PC 服务之间的所有通信

---

## 0. 一句话概览

```
PC ←─ BLE GATT ─→ ESP32 ←─ JSON 字节流 ─→ JS 动态 app
```

PC 端通过一个**通用 GATT 桥接 service** 跟 ESP 收发**变长 UTF-8 字节**，字节内容是一个 **JSON 对象**。ESP 上的 dynamic app runtime 把这条 JSON 按 `to`/`from` 字段路由给当前正在跑的 app。

固件端透明转发，**不解析业务字段**。协议是 PC 端开发者和 JS app 开发者之间的契约。

---

## 1. 物理层（GATT 表）

```
Service:  a3a30001-0000-4aef-b87e-4fa1e0c7e0f6
  ├─ char rx (PC → ESP):  a3a30002-0000-4aef-b87e-4fa1e0c7e0f6   WRITE
  └─ char tx (ESP → PC):  a3a30003-0000-4aef-b87e-4fa1e0c7e0f6   READ + NOTIFY
```

- **rx**：PC 用 `WRITE without response`（更快、不需 ack）写入 1 ~ 200 字节
- **tx**：PC 必须先 `start_notify` 订阅，之后每条 ESP 发来的消息以 NOTIFY 形式到达
- 订阅前发送的消息 **会被丢弃**（NimBLE notify 直接失败）

---

## 2. 应用层包格式

每一条消息是 **一个 JSON 对象**，UTF-8 编码，**总字节数 ≤ 200**。

### 2.1 必备字段

| 字段 | 类型 | 方向 | 必填 | 含义 |
|---|---|---|---|---|
| `to` | string | PC → ESP | ✅ | 目标 app 名（与 menu 注册名一致），或 `"*"` 广播 |
| `from` | string | ESP → PC | ✅ | 发送方 app 名 |
| `type` | string | 双向 | ✅ | 消息类型，业务自定 |
| `body` | any (常用 object) | 双向 | 可选 | 业务数据；不需要时可省略 |

### 2.2 路由规则

PC → ESP：
- `to == 当前正在跑的 app 名` → 派给该 app
- `to == "*"` → 任意 app 都接
- 其它 → JS helper 静默丢弃（ESP 端不报错）

ESP → PC：
- 由 PC 端 SDK 按 `(from, type)` 双键路由给注册的 handler

### 2.3 大小限制

- 单条最大 **200 字节**（utf-8 编码后）
- 超出 PC 端 SDK 抛 `ValueError`，ESP 端 `sys.ble.send` 返 `false`
- **超长消息由应用层自己分包**（建议 body 加 `chunk` / `total` 字段，自定）

---

## 3. 保留 type（开发者不要占用）

| type | 方向 | 语义 |
|---|---|---|
| `ping` | PC → ESP | 连通性测试。JS helper **自动**回 `pong`（带回原 body），不打扰业务 |
| `pong` | ESP → PC | ping 的回包 |
| `error` | 双向 | 业务级错误，body 建议 `{code: int, msg: string}` |

未来可能新增 `hello` / `bye` 用于 app 生命周期，**当前未实现**，但 `type` 名做保留。

---

## 4. 完整示例：echo

```json
// PC → ESP
{ "to": "echo", "type": "msg", "body": "hello" }

// ESP → PC
{ "from": "echo", "type": "msg", "body": "hello (echo)" }
```

```json
// PC → ESP，单独发 ping（任意 app 都会自动回）
{ "to": "*", "type": "ping", "body": { "ts": 1714214400 } }

// ESP → PC（自动回，业务侧 ble.on 不会触发）
{ "from": "echo", "type": "pong", "body": { "ts": 1714214400 } }
```

---

## 5. 一对一约束

- 同一时刻：**一台 ESP 只接一个 PC**（NimBLE 配置如此）
- 同一时刻：**ESP 上只跑一个 dynamic app**（runtime 串行执行）
- 因此协议**不需要**会话 ID、消息 ID（业务自己做 RPC 配对的话可以加 `body.id`）

---

## 6. 错误处理建议

- PC 端 SDK 已经覆盖：
  - 连接失败 → 自动重连
  - JSON 解析失败 → DEBUG 日志，丢弃
  - handler 抛异常 → ERROR 日志 + 不影响后续
- 业务级错误，约定回 `type: "error"`，body 含 `code`/`msg`，例如：

```json
{ "from": "weather", "type": "error", "body": { "code": "fetch_fail", "msg": "timeout" } }
```

---

## 7. 推荐字段命名约定

JSON 字段名一律用**蛇形小写**：`temp_c` / `is_playing` / `weather_code`。

不要用 `tempC` / `isPlaying`（驼峰），不要用 `Temperature_C`（首字母大写）。

时间戳一律 unix 秒（int），字段名 `ts`。

---

## 8. 开发流程速记

1. 写 JS 端：脚本顶部一行 `var ble = makeBle("myapp")`（`makeBle` 由 prelude 自动注入，无需拷代码），然后 `ble.on("type", handler)` / `ble.send("type", body)`
2. 写 Python 端：在 `tools/providers/` 新建 `myapp_provider.py`，实现 `register_myapp(client)`
3. 在 `tools/dynapp_companion.py` 加一行 `register_myapp(client)`
4. 在固件 `dynamic_app/scripts/` 加 js 文件、`CMakeLists.txt` EMBED、`dynamic_app_registry.c` 注册
5. 在 `app/pages/page_menu.c` 加菜单入口

---

## 9. 协议变更策略

本协议视为**稳定契约**。修改原则：

- **可加新字段**（向后兼容）
- **可加新 type**（新 handler 不影响旧的）
- **不可改字段名**（破坏旧 app）
- **不可改 GATT UUID**（破坏所有现有 PC 端代码）

未来真要破坏式升级，**新建 service UUID** 而不是改老的。
