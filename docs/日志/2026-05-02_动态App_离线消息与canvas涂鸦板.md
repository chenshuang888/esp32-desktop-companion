# 动态 App 离线消息总线 + sys.canvas + 涂鸦板 工作日志

**日期**：2026-05-02
**分支**：feat/optimize_page
**作者**：ChenShuang + Claude

---

## 0. 起因

通知 App 复刻验证通过后，盘点发现两件事不够爽：

1. **动态 App 在前台才能收到 BLE 消息** —— JS 线程没起来时 `dynapp_bridge` 的 inbox 队列没人 drain，C 端直接清空丢弃。这意味着：用户不进通知 App 时，PC 推过来的微信通知**全部丢失**。原生 App 走 UI 线程是 always-on 没这问题，动态 App 不行。
2. **动态 App 没充分用到文件系统** —— `sys.fs.write` 一次只能写 196B（受 BLE 上传单帧 chunk 限制），实际等于"小 KV 存档"，没法存任何"稍大文件"。`sys.canvas` 也不存在，业务想画自由笔触只能拿 panel 当像素拼装，性能不能看。

本轮一次性解决：

- **离线消息总线 dynapp_mailbox**：JS 不在跑时由 mailbox 后台 task 接管 drain，按 `"to":"xxx"` 字段归档到 NVS；JS 启动时先回灌再跑业务，对 JS 完全透明
- **sys.fs.write 透明分段**：上限提到 256KB，超过 196B 自动走"PSRAM 拷贝 → fs_worker 串行写"路径；JS API 不变
- **sys.canvas.\* native**：暴露 LVGL canvas 像素级绘图能力，buffer 在 PSRAM；canvas obj 自挂 PRESS/PRESSING/RELEASE，事件携带画布内绝对坐标
- **doodle_pkg**：第二个完整动态 App，把 sys.canvas + sys.fs（assets + data）+ NVS 之外的所有文件路径跑了一遍

---

## 1. 关键决策

### 1.1 mailbox 是"消费者缺席兜底"，不是新总线

讨论过几个选项：
- 选项 A：让 JS task 永远活着——违反"动态 App 平台轻量"原则，每个装着的 app 常驻几十 KB JSContext × N
- 选项 B：notif 走原生 notify_manager 共享数据——破坏动态 App 独立性，其它 app 想要类似能力还得逐个加 native
- **选项 C（采用）**：在 `s_inbox` 这条公共总线下游加一个"接力消费者"。JS 在跑时 mailbox 让位、JS 不在跑时 mailbox 归档 NVS。**JS 业务零侵入。**

最终是个非常自然的接力模型：

```
[BLE host task] ──写──► s_inbox (公共，容量 8)
                            │
            ┌───────────────┴───────────────┐
            │ 谁 drain 看 s_js_active flag    │
            │   true  → script_task          │
            │   false → mailbox_task         │
            └────────────────────────────────┘
                            │ false 路径
                            ▼
                  peek "to":"xxx" → NVS dynapp_mb/<id>
                            │
                            │ JS 启动时一次性回灌
                            ▼
                       回到 inbox → script_task drain
                            │
                            ▼
                       JS ble.on(...) 触发
```

### 1.2 mailbox 不解析 JSON

bridge 的契约是"透明字节流"，mailbox 不能破坏这个契约。所以 peek `"to":"xxx"` 用 `memcmp(NEEDLE, ...)` + 读到下一个 `"`，**不真正解析 JSON**——只偷一个路由 key 出来当 NVS namespace 内的 key。后续如果 JSON 变 CBOR，把 needle 换成新 marker 就行。

这一点也是踩坑后才确认对的：第一版 NEEDLE 写成 `"to":"`，但 Python 端 `json.dumps` 默认输出 `"to": "xxx"`（冒号后有空格），结果 167B 的微信通知全部 drop。修法是 NEEDLE 改成 `"to"` 后用状态机跳过空白和冒号——做"几乎 JSON 但又不解析"的折中。

### 1.3 让位用 atomic flag，不用锁

mailbox 和 script_task 都是 inbox 消费者，一个时刻只能一个生效。原本要用 mutex 互斥，但 mutex 让 mailbox task drain 时 script_task 想抢也抢不到，反之亦然。

`atomic_bool s_js_active` 一个标志位就够：
- script_task START 时 set true 后再调 replay；STOP 时 set false
- mailbox_task 每次 loop 顶上检查，true 就 sleep 200ms 不动 inbox

非阻塞、零锁开销、契约清晰。

### 1.4 sys.fs.write 256KB 上限选择"用户透明分段"而不是流式 API

考虑过：
- A：暴露 `sys.fs.openWriter(path)/append/commit` 流式 API
- **B（采用）**：JS 还是 `sys.fs.write(path, content)` 一行调用，C 内部把 content 整块拷到 PSRAM，发一个 `submit_user_write_large` 让 fs_worker 自己写。

理由：
- JS 用法保持简单一行，业务无需关心分段
- 256KB × N 个并发 in-flight 写在嵌入式不现实，单 active 写盘任务足够
- 现有 fs_worker queue 才 16 槽，强行让 JS 自己分段会爆队列

代价是 256KB 拷贝（PSRAM memcpy ~200MB/s = ~1ms），完全可接受。

### 1.5 canvas 用"saveTo / loadFrom"快通道而不是"snapshot 字符串"

QuickJS 的 string 是 UTF-8 校验的，把任意二进制塞进去会乱码。两个选项：
- A：base64 encode/decode，多 33% 内存 + 多两遍 CPU
- **B（采用）**：JS 永远不接触 raw bytes。`sys.canvas.saveTo("d.bin")` C 侧直接 buffer→file；`sys.canvas.loadFrom("d.bin")` C 侧 file→buffer。零编码开销。

代价是 doodle 没法验证 sys.fs.write 256KB 那条独立路径——但 saveTo 内部就走的同一个 `submit_user_write_large`，效果等价。

### 1.6 canvas 事件复用 PRESS/DRAG/RELEASE 但 dx/dy 改语义

现有 root listener 的事件 dx/dy 是**两次入队之间的相对增量**（手势识别用）。涂鸦需要画布内绝对坐标。

简单做法：canvas obj **自挂** 事件 cb，用 `lv_indev_get_point` 减去 `lv_obj_get_coords().x1` 算相对画布的坐标，塞进同样的 dx/dy 字段入队。**事件类型枚举不变**，业务 onPress(ev) 收到 `ev.dx, ev.dy` 当画布坐标用，根据节点类型（canvas vs panel）自己判断含义。

附带处理：canvas obj 必须 `lv_obj_clear_flag(EVENT_BUBBLE)`，否则事件冒泡到 root listener 会**双发**——canvas 一份带画布坐标 + root 一份 dx=0/dy=0，业务侧 state.cur 被反复重置，笔触全部断点。

### 1.7 长按吞掉 click（平台级修复，不只 doodle）

doodle 画廊用长按删除，但 LVGL 默认 LONG_PRESSED 触发后松手仍会发 CLICKED → 业务侧 onClick + onLongPress 同时触发 → 长按删除时同时进入了编辑页。

修法是平台级：手势状态加 `long_press_fired` flag，LONG_PRESSED 触发时置 true，下一次 CLICKED 看到 true 就吞掉并复位。**所有动态 App 都受益**。

### 1.8 doodle 工具栏图标走 assets，不走字体

最初版本用 `sys.icons.MENU/OK/IMAGE` 那套 codepoint，结果发现这些 codepoint **不在固件字体子集里**，加进去要改字体子集 + 重烘 ttf。临时改用文字"擦/存/更多"。

最后：仿 dash 同款，写一个 `_make_pngs.py` 用 PIL 画 4 张 40×40 几何图标 → LVGLImage.py 转 RGB565 .bin。**纯本地一键，零外部依赖**。这条路通了之后 assets 路径才算真验证完。

---

## 2. 落地清单

### 2.1 阶段 A：dynapp_mailbox（离线消息总线）

**新建（2 文件）**
- `services/manager/dynapp_mailbox.{h,c}` —— 后台 task + NVS 编码 + replay 队列同步
  - blob 格式：`[u8 ver][u8 count][entry × N]`，entry = `[u16 len][bytes]`，每 app 上限 30 条 ~6KB
  - peek `"to"` + 跳空白 + 提取 app_id（容忍 `"to" : "xxx"` JSON 标准空白）

**改动（3 文件）**
- `services/dynapp_bridge_service.{h,c}` —— 加 `dynapp_bridge_push_inbox` 公共 API（mailbox replay 用）
- `services/CMakeLists.txt` —— 注册 `manager/dynapp_mailbox.c`
- `main/main.c` —— `nimble_start` 之后调 `dynapp_mailbox_init()`
- `dynamic_app/dynamic_app.c` —— script_task START 时 `set_js_active(true) + replay`，STOP / 失败时 `set_js_active(false)`

### 2.2 阶段 B：sys.fs.write 256KB 扩容

**改动（4 文件）**
- `storage/littlefs/dynapp_script_store.{h,c}` —— 新增 `DYNAPP_USER_DATA_MAX_BYTES = 256K`；`read_file` 增 max_bytes 参数；user_data_write 用新上限
- `storage/littlefs/dynapp_fs_worker.{h,c}` —— 加 `submit_user_write_large`：拷贝到 PSRAM、worker 串行写、写完 free。新增 `FS_OP_USER_WRITE_LARGE` 操作类型
- `dynamic_app/dynamic_app_natives.c` —— `js_sys_fs_write` 改成：≤196B 走老路径（zero-copy 入队），>196B 走 large（拷贝 PSRAM）

### 2.3 阶段 C：sys.canvas.\* native

**改动（5 文件）**
- `dynamic_app/dynamic_app_ui.h` —— 6 个新 cmd 类型（CREATE_CANVAS / FILL / PIXEL / LINE / SAVE / LOAD）+ union 字段 + 6 个 enqueue API
- `dynamic_app/dynamic_app_ui_internal.h` —— registry slot 加 `void *aux`（buffer 指针），`UI_OBJ_CANVAS` 类型
- `dynamic_app/dynamic_app_ui.c` ——
  - 6 个 enqueue 实现
  - canvas helpers：`canvas_putpx` / `canvas_fill_buf` / `canvas_line_buf`（Bresenham + 粗细方块）
  - `do_create_canvas`：alloc PSRAM buffer + `lv_canvas_set_buffer` + 自挂 PRESS/PRESSING/RELEASE + `clear_flag(EVENT_BUBBLE)` 防双发
  - `do_canvas_save` / `do_canvas_load`：走 fs_worker 大块路径 / 同步 read_file
  - drain 6 个新分支
  - `unregister_all` / `CMD_DESTROY` 新增 free aux 逻辑
  - long_press_fired 标志（吞掉 LONG_PRESS 后的 CLICKED）
- `dynamic_app/dynamic_app_natives.c` —— 6 个 native 实现 + cfunc 注册 + sys.canvas 命名空间挂载
- `dynamic_app/dynamic_app_internal.h` —— `DYNAMIC_APP_EXTRA_NATIVE_COUNT` 32→38
- `dynamic_app/scripts/prelude.js` —— VDOM.h 节点类型加 `'canvas'`

### 2.4 阶段 D：doodle_pkg（第二个完整动态 App）

**新建（5 文件）**
- `dynamic_app/scripts/doodle_pkg/manifest.json` —— `{id:doodle_pkg, name:涂鸦, icon:PETS, iconColor:ACCENT_2}`
- `dynamic_app/scripts/doodle_pkg/main.js` —— ~280 行
  - 240×216 画布 + 240×52 工具栏（6 按钮：3 色块 + 擦/存/更多）
  - onPress 记起点；onDrag 调 sys.canvas.line 连续画
  - 保存：`d_<ts>.bin` 落 data/，toast 反馈
  - 模态画廊：sys.fs.list 列 d_*.bin，点击恢复，长按删除
- `dynamic_app/scripts/doodle_pkg/_make_pngs.py` —— PIL 画 4 张几何图标（橡皮/软盘/三点/垃圾桶）→ LVGLImage.py 转 RGB565 .bin
- `dynamic_app/scripts/doodle_pkg/assets/ic_{erase,save,more,trash}.bin` —— 4 × 3.2KB

---

## 3. 几次"踩坑 + 修复"

### 3.1 mailbox NEEDLE 不容忍 JSON 空格

```
W (151077) dynapp_mb: drop msg without parsable to:xxx (167B)
```

NEEDLE 写成 `"to":"`，Python `json.dumps` 默认输出 `"to": "xxx"`，整条消息全部 drop。

修法：NEEDLE 改成 `"to"`，跳空白 + 跳冒号 + 跳空白 + 取引号内 ID 字符。

**教训**：和外部数据交互的 parser **必须按外部数据真实格式** 测，不能按"我以为它会输出什么"测。这条本来 90 秒就能改完，但日志里只看到 "drop 167B" 看不出原因，加了一行 dump 前 60B 才定位到——**任何"丢弃"分支都该把 head bytes 打出来**。

### 3.2 mailbox 归档/回灌的 NVS key 不一致

```
archived to notif (count=3, blob=437B)        ← peek 出 "to":"notif"
starting dynamic app: notif_pkg
replay notif_pkg: pushed 0 msgs back to inbox  ← 找 NVS key "notif_pkg" 永远空
```

JS 端 `APP_NAME = "notif"`，PC 推 `to:"notif"`；mailbox 用 "notif" 当 NVS key。但 dynamic_app_registry_current 返回的是**目录名 "notif_pkg"**，replay 用它去找 NVS 永远找不到。

修法：统一用目录名做 BLE 路由 key。JS 改 `APP_NAME = "notif_pkg"`，PC bridge_provider 推 `to:"notif_pkg"`。

**教训**：跨进程的命名空间要有**单一事实源**。这里的事实源应该是"app 目录名"——manifest.id、JS APP_NAME、PC `to` 字段、NVS key、registry_current 全部统一到它。早期允许 JS 自己起 APP_NAME 是个口子，关上才 sound。

### 3.3 LittleFS 上的 main.js 跟仓库脱节

修了 JS 后忘了重传 LittleFS，设备上跑的还是上一版 APP_NAME = "notif"，PC 改了 `to:"notif_pkg"` 后 makeBle 第一关就丢——`if (msg.to !== appName) return`。

**教训**：动态 App 体系下"设备上跑的代码"和"仓库里的代码"是两份。debug 第一句话该问"这版上传过没？"——后面要不要在 prelude 里加版本/build hash log 帮定位，先记着。

### 3.4 UserNotificationListener 事件订阅在非 UWP 进程不能用

```
WARNING ... subscribe notification_changed failed: [WinError -2147023728] 找不到元素
```

`UserNotificationListener.add_notification_changed` 需要 UWP 后台任务注册。普通 Python 进程拿不到事件回调（HRESULT 0x80070490）。

修法：改成 2 秒一次轮询 `get_notifications_async` + diff `_seen_ids`。和 `toast.py` 同款做法。

**教训**：winsdk 文档没明说这点。不是所有 winrt API 在 win32 进程都能用，碰到事件订阅返回"找不到元素"先怀疑 UWP-only 限制。

### 3.5 canvas 事件双发，笔触全部断点

第一次画线时所有线条都"画一段就回原点画一段"。原因：canvas 自己挂的 PRESS 事件和 root listener 的 PRESS 事件**都派给 canvas vnode**——canvas 一份带画布坐标 + root 一份 dx=0/dy=0，业务里 onPress 把 state.cur 反复重置成 (0, 0)。

修法：canvas obj 上 `lv_obj_clear_flag(LV_OBJ_FLAG_EVENT_BUBBLE)`，事件不冒泡到 root listener。

**教训**：LVGL 事件冒泡是默认的；自挂事件的 obj 必须显式关掉，否则会和 attachRootListener 重复触发。

### 3.6 LONG_PRESS 后还触发 CLICKED

doodle 画廊长按删除时：弹"删除？"模态 + 同时点击事件触发"恢复编辑页"。

修法（平台级）：手势状态加 `long_press_fired` flag，LONG_PRESS 触发时置 true，下一次 CLICK 看到 true 就吞掉并复位。**所有动态 App 都受益**。

**教训**：LVGL 的 LONG_PRESSED 和 CLICKED 是独立事件序列，业务期望"长按 = 不再 click"是合理预期，平台层应该兜底而不是让每个业务 app 自己处理。

### 3.7 do_create switch 漏 UI_OBJ_CANVAS 导致编译失败

加了 `UI_OBJ_CANVAS` enum 但 `do_create()` 的 switch 没处理，`-Werror=switch` 直接挂。

修法：加 `case UI_OBJ_CANVAS: return -3;`（canvas 走 do_create_canvas，不应进 do_create）。

**教训**：加 enum 值时 grep 整个仓库 switch 用法，确认每个 switch 要么处理新值要么有 default。`-Werror=switch` 是好朋友。

### 3.8 doodle 工具栏图标找不到字体 codepoint

第一次 main.js 用了 `sys.icons.MENU / OK`，但这些 codepoint 不在固件字体子集 `material_icons_subset.ttf` 里，渲染显示空白方块。

修法：直接用 PIL 画几何 → LVGLImage.py 转 RGB565 .bin → assets/ic_*.bin。从此 doodle 走"PNG 资源 → LVGL FS driver"路径，**不再受字体子集限制**。

**教训**：图标 codepoint 是字体子集的一部分；动态 App 想用某个 codepoint 必须确认它在子集里，否则就走 assets。后者更灵活：每个 app 自由选图标，不用扩字体子集。

---

## 4. 接力总线模型沉淀

mailbox 跑通后，JS 业务零改动就能"App 不在跑期间也收到所有消息"，这是个**消息总线雏形**。和教科书消息总线对照：

| 总线特性 | 我们有 | 怎么做的 |
|---|---|---|
| 生产者/消费者解耦 | ✅ | BLE host 只往 s_inbox 写，不知道谁消费 |
| 统一传输介质 | ✅ | s_inbox 是 single source of truth |
| topic 路由 | ✅ | `"to":"xxx"` 字段 |
| 消费者上下线动态接管 | ✅ | js_active flag 控制谁 drain |
| 持久化（消费者下线时不丢） | ✅ | mailbox 归档 NVS |
| 重放（消费者上线消费历史） | ✅ | dynapp_mailbox_replay |
| 多消费者扇出 | ❌ | 一个 topic 只有一个 app |
| 跨进程 | ❌ | 限设备内 |

定位最像：**Android BroadcastReceiver + JobScheduler 组合**——BroadcastReceiver = `ble.on("add")` 只在 app 活的时候有效；JobScheduler = mailbox，"消息到了但 app 没活，存起来等 app 醒来"。

价值：通用消息总线（MQTT/Kafka）不知道业务，靠配置约定 topic。我们这套**为生命周期设计**——它知道消费者会"睡觉"、知道醒来要"补课"、知道资源有限。这种气质在嵌入式/移动端比通用总线更常见。

---

## 5. 平台能力当前覆盖盘点

doodle_pkg + notif_pkg 加起来把动态 App 平台**所有持久化 + 通信路径**跑了一遍：

| 路径 | 谁验证 |
|---|---|
| LittleFS `apps/<id>/main.js` 上传 + 加载 | 全部 app |
| LittleFS `apps/<id>/manifest.json` 解析 | 全部 app |
| LittleFS `apps/<id>/assets/*.bin` 静态资源 | doodle、imgdemo、dash、memory |
| LittleFS `apps/<id>/data/<rel>` 沙箱写（≤196B） | dash、habit |
| LittleFS `apps/<id>/data/<rel>` 沙箱写（**256KB**）| **doodle**（saveTo） |
| LittleFS `apps/<id>/data/<rel>` 沙箱读 | doodle（loadFrom）、dash |
| LittleFS sys.fs.list/remove | doodle（画廊管理） |
| NVS `dynapp/<id>/state` 沙箱（≤4KB） | notif、dash |
| NVS `dynapp_mb/<id>` 离线消息归档 | **mailbox 自动**（notif 收益） |
| BLE 实时消息（前台） | notif 实时收新通知 |
| BLE 离线消息（**后台兜底**）| **notif 关 app 时仍收**（mailbox 接力）|
| sys.canvas 像素绘图 + PSRAM buffer | **doodle** |

**还没用到的平台能力**：sub_router push/pop（多级页面）、lv_arc/lv_chart 控件原语、sys.canvas 多个并存。

---

## 6. 文件改动清单

### 新建（11）
- `services/manager/dynapp_mailbox.h`
- `services/manager/dynapp_mailbox.c`
- `tools/companion/shared/win_notifications.py`（PC 端 Windows 通知监听，给 notif app 提供数据源）
- `dynamic_app/scripts/doodle_pkg/manifest.json`
- `dynamic_app/scripts/doodle_pkg/main.js`
- `dynamic_app/scripts/doodle_pkg/_make_pngs.py`
- `dynamic_app/scripts/doodle_pkg/assets/ic_erase.{png,bin}`
- `dynamic_app/scripts/doodle_pkg/assets/ic_save.{png,bin}`
- `dynamic_app/scripts/doodle_pkg/assets/ic_more.{png,bin}`
- `dynamic_app/scripts/doodle_pkg/assets/ic_trash.{png,bin}`
- `docs/动态app_离线消息总线_canvas_涂鸦板_工作日志.md`（本文件）

### 修改（重要）
- `services/dynapp_bridge_service.{h,c}` —— 加 `push_inbox` API
- `services/CMakeLists.txt` —— 注册 dynapp_mailbox
- `main/main.c` —— 加 `dynapp_mailbox_init()`
- `dynamic_app/dynamic_app.c` —— script_task 接入 mailbox set_js_active + replay
- `dynamic_app/dynamic_app_ui.h` —— 6 canvas cmd + enqueue API + USER_PATH_MAX
- `dynamic_app/dynamic_app_ui_internal.h` —— slot 加 aux + UI_OBJ_CANVAS
- `dynamic_app/dynamic_app_ui.c` —— 6 enqueue + canvas helpers + do_create_canvas + save/load + canvas 事件 cb + drain 6 分支 + aux free + long_press_fired
- `dynamic_app/dynamic_app_internal.h` —— EXTRA_NATIVE_COUNT 32→38 + 6 个 idx 字段
- `dynamic_app/dynamic_app_natives.c` —— js_sys_fs_write 分段 + 6 sys.canvas native + cfunc 注册 + sys.canvas 命名空间
- `dynamic_app/scripts/prelude.js` —— VDOM.h 接受 'canvas'
- `storage/littlefs/dynapp_script_store.{h,c}` —— USER_DATA_MAX_BYTES 256K + read_file 加 max_bytes
- `storage/littlefs/dynapp_fs_worker.{h,c}` —— submit_user_write_large + USER_WRITE_LARGE op
- `tools/companion/providers/bridge_provider.py` —— 接 win_notifications，notif/add 推送
- `dynamic_app/scripts/notif_pkg/main.js` —— APP_NAME 改 `notif_pkg`（与目录名对齐）

---

## 7. 验证

### 7.1 mailbox 离线消息
- [x] 关闭 notif app 时发微信，mailbox 归档：`archived to notif_pkg (count=N, blob=XB)`
- [x] 打开 notif app，replay：`replay notif_pkg: pushed N msgs back to inbox`
- [x] JS 端 `ble.add fired` × N 次，items 累加
- [x] 退出 app 后 mailbox 重新接管
- [x] 设备重启不丢（NVS 持久化）

### 7.2 sys.fs.write 256KB
- [x] doodle saveTo 一次写入 ~100KB（240×216×2）成功
- [x] fs_worker 无 alloc / queue 报错
- [x] loadFrom 同步读出 + 画布恢复正确

### 7.3 sys.canvas
- [x] 画布默认白底
- [x] 拖动出连续笔触（无双发断点）
- [x] 切换颜色 / 橡皮 / 粗细
- [x] fill 清屏

### 7.4 doodle_pkg 完整流程
- [x] launcher 显示紫色 PETS 图标
- [x] 进入：白画布 + 工具栏（3 色块 + 橙橡皮/绿软盘/灰三点）
- [x] 画几条 → 存 → toast 已保存
- [x] 更多 → 画廊 → 列出 d_*.bin
- [x] 点条目恢复编辑（loadFrom 全链路）
- [x] **长按条目弹删除模态、不再误进编辑页**（平台级长按吞 click 修复）
- [x] 删除生效 + 列表自动刷新
- [x] 跨 session 持久化

### 7.5 不破坏旧能力
- [x] notif 实时通知功能完整
- [x] notif saveState（NVS）功能完整
- [x] dash / habit / imgdemo 等老 app 不受影响

---

## 8. 一句话总结

**消息总线兜住了"App 不在跑也别丢消息"**，**sys.fs.write 拿掉 196B 紧箍咒**，**sys.canvas 把像素级绘图能力交给 JS**，**doodle 把这三件事 + assets 资源链路一次跑通**。动态 App 平台的运行时核心能力到此基本闭环——下一个 app 怎么写都行，平台不会再因为"少了哪条路径"卡壳。

---

## 9. 不在本次范围（下一轮）

- ❌ sub_router push/pop（doodle 画廊用模态绕过了，但提醒事项 / 多级设置类 app 早晚要做）
- ❌ lv_arc / lv_chart / lv_slider 控件原语（仪表盘类 app 需要）
- ❌ sys.canvas 多个并存（doodle 单 canvas 够用）
- ❌ 字体子集加 BRUSH / OK / MENU 等业务 codepoint
- ❌ 自动化 build/version 标识（避免"上传没"的 debug 时间）
- ❌ mailbox 白名单 / GC（恶意 app_id 会 NVS 累积，目前靠 namespace 容量保护）
- ❌ saveTo 失败回调（fire-and-forget，业务侧不知道是否真落盘）

---

## 10. 关键经验沉淀

### 10.1 "JS 不在跑就丢消息" 是动态 App 体系的固有问题，要在 C 层兜底
原生 App 走 always-on UI 线程，没有"消费者缺席"问题。动态 App 如果想"看起来像原生"，平台必须在消费者缺席时帮忙存数据。mailbox 就是这层兜底。

### 10.2 双向数据流的边界要 C 层决定
JS → C 走 cmd queue（异步）；C → JS 走 event queue（异步）；C ↔ NVS 走 fs_worker queue（异步）；BLE host → JS 走 inbox + drain（异步）。**所有跨线程数据流都通过 queue**，永远不让 JS context 在另一个线程被读写。这条规则跑到底，没有任何同步原语相关 bug。

### 10.3 平台扩展和业务验证要分开做
sys.fs 扩容（阶段 B）和 sys.canvas（阶段 C）独立编译验证：写测试脚本验完再做 doodle。doodle 是验收，不是开发载体。

### 10.4 PIL 画几何图标 → LVGLImage.py 转 .bin 是动态 App 资源标准做法
不依赖外部素材、纯本地一键、跟 dash/memory 同款。**任何动态 app 想要图标都该照这个范式做**，不要为了某个图标去改字体子集。

### 10.5 "同名不同义"是跨层 bug 的高发区
notif vs notif_pkg、`"to"` vs `"to":"`、`dx/dy` 在 root vs canvas 路径下含义不同——这些都坑过。统一命名空间、统一 NEEDLE 容差、给 dx/dy 字段在事件 ev 里加注释，能省半小时。

### 10.6 "drop / dropped"日志必须带 head bytes
任何 "因为 X 丢了 N 字节" 的 log 都该把前 60B 打出来，否则 debug 没线索。这条加进去后 mailbox 那个 NEEDLE bug 几秒钟就定位到了。
