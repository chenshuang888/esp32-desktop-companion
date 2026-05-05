先说结论性判断：你这个想法能做但不是简单事

  直接说三个核心难点，再讲怎么破：

  难点 1：GATT 表必须在 BLE 协议栈启动之前注册死

  NimBLE 的核心约束：

  ble_driver_nimble_init()      ← 协议栈基础设施
    ↓
  所有 service_init()           ← ble_gatts_add_svcs(svc_def[])
    ↓
  ble_driver_nimble_start()     ← 之后 GATT 表锁定，不能动
    ↓
  广播开始，PC 连接，正常工作

  NimBLE 不支持运行时增删 GATT service。一旦 nimble_start 跑过，那张 GATT 表就凝固了。

  这意味着：你想要"用户进 dynamic app 时再注册 service" —— 物理上做不到。要么在开机就预占好"为 dynamic app 准备的 service
  槽位"，要么改方案。

  难点 2：跨线程数据传递的字符串/数据生命周期

  BLE host 任务收到 PC 写来的数据：

  NimBLE host task (优先级高)
    ↓ access_cb 在 host 任务上下文里同步执行
    ↓ ctxt->om 是 mbuf，cb 返回后立刻释放
    ↓ 必须立刻拷贝出来再丢队列
    ↓
  脚本任务 (优先级低)
    ↓ 取队列 → 调 JS 回调 → JS 处理

  中间的"拷贝出来"涉及堆分配。20 byte 的小消息无所谓，但如果 PC 推一个 1KB JSON 进来，每秒推 10 条，就要小心了。

  难点 3：JS 回调挂在 BLE event 上的 GC 引用管理

  JS 里写 sys.ble.on('write', function(data){...})，这个 function 是个 JSValue，必须 JS_AddGCRef 持有，否则下一次 GC 就被回收了。app
  退出时（脚本 teardown）必须把这些 ref 全释放，否则下个 app 启动还会触发上个 app 的回调（野指针式调用）。

  这点参考 setInterval 那块的 dynamic_app_intervals_reset(ctx) 思路，但比它复杂。

  ---
  三种方案，按"ROI 排序"

  方案 A：通用 GATT 表 + JS 自定义 UUID 映射（你说的方向，最强但最难）

  JS 里声明：
  sys.ble.defineService("a1b2...", [
      { uuid: "c3d4...", flags: ["write", "notify"], onWrite: handleData }
  ]);

  要做：
  - 开机时预先注册一个通用容器 service（比如 1 个 service，10 个 char），UUID 用 dynapp-XXXX-XXXX-... 的占位符
  - JS 调 defineService 时不真的添加 service（做不到），而是把"我现在认领第 N 个 char"记下来
  - 收到这个 char 的 write/read，转发到对应 JS 回调

  问题：UUID 是 PC 端识别 service 的唯一身份。占位 UUID 不能动 → PC 端必须知道"动态 app 用的 char 0 实际语义是
  X"，需要协商协议。复杂度爆表。

  或者：想真正动态化，需要 NimBLE 支持运行时 svc swap（重启协议栈再注册），代价是连接断开重连。

  方案 B：固定一个"DynApp Generic" service（推荐）⭐

  开机就注册死一个 service，专门给所有动态 app 共用：

  Service:  dynapp-0001-0000-...
    ├─ char_0  WRITE       (PC → ESP)：给 dynamic app 发数据
    ├─ char_1  READ/NOTIFY (ESP → PC)：dynamic app 给 PC 推数据
    └─ char_2  WRITE       (PC → ESP)："channel" 控制：PC 告诉 ESP 我要跟哪个 app 通信

  JS 侧：
  sys.ble.onMessage(function(channel, data) { ... });   // PC → app
  sys.ble.send(channel, data);                          // app → PC

  channel 是个字符串，用来区分"这条消息是给 alarm 的 / 给 chat-app 的 / 给 control-pad 的"。PC 端写消息时第一个字段是 channel 名，ESP
   解析后只把 channel 匹配的转给当前正在跑的 app（或忽略）。

  优点：
  - 一个 service 永远在 GATT 表里，零动态注册问题
  - JS API 极简：on / send 两个函数
  - PC 端只要适配一份协议就能跟所有动态 app 聊
  - 同一时刻只有一个动态 app 在跑（你框架本来就是串行的），channel 主要起"app 名验证"作用

  缺点：
  - 不是"每个 app 自己有 service UUID"那种纯净感
  - PC 端不能用通用 BLE 工具按 service UUID 区分 app（但反正 PC 端是你自己写的桌面伴侣）

  方案 C：消息总线模式（最简洁）

  更进一步：根本不暴露 char/uuid 概念，就一对 inbox/outbox：

  sys.ble.recv(function(payloadString) { ... });
  sys.ble.send(payloadString);

  ESP 完全不管 payload 内容，就是个透明管道。JS 自己用 JSON 封装 {type: "msg", from: "alarm", body: "..."}，PC 端按约定解析。

  等于把方案 B 的 channel 概念也丢给 JS 处理。C 侧只剩"管道 + 队列"，最薄。

  ---
  我推荐：方案 C

  理由：
  1. 你脚本里已经在用 JSON.stringify/parse（saveState 就是这么用的），JSON 当包封天然合适
  2. ESP 侧改动最小：1 个 service + 2 个 char，~150 行 C
  3. 协议演进零成本：PC 跟 alarm 加个新消息类型，只要改 alarm.js 和 PC 桌面伴侣就行，固件不动
  4. 奥卡姆剃刀：你只需要"传字节"这一件事，加 channel/UUID 都是为不存在的需求买单

  ---
  完整方案 C 的工程规划

  架构

                  BLE 总线（NimBLE host task）
                          │
                    ┌─────┴─────┐
                    │ dynapp_   │  ← 新增 service
                    │ bridge_   │     UUID: a3a3-0001-...
                    │ service.c │
                    └─────┬─────┘
                          │
        ┌─────────────────┴─────────────────┐
        │                                   │
     inbox queue                       outbox queue
     (PC → app)                        (app → PC)
        │                                   │
        │ 容量 8，每条 ≤ 200B               │ 容量 8
        │ host_task 写, script_task 读      │ script_task 写, host_task 读 (notify)
        │                                   │
        ↓                                   │
    Script Task tick:                       │
     ├ 调 dynamic_app_drain_ble_inbox()    │
     │  → 循环 pop → 调 JS 回调            │
     │                                     │
     └ JS 调 sys.ble.send(payload) ────────┘
                ↓
         enqueue outbox
                ↓
      host_task 通过 ble_gatts_notify_custom 发出去

  新增文件

  ┌────────────────────────────────────┬───────────────────────────────────────────────────┬──────┐
  │                文件                │                       职责                        │ 行数 │
  ├────────────────────────────────────┼───────────────────────────────────────────────────┼──────┤
  │ services/dynapp_bridge_service.h/c │ GATT service 定义 + inbox/outbox 队列 + access_cb │ ~180 │
  ├────────────────────────────────────┼───────────────────────────────────────────────────┼──────┤
  │ services/dynapp_bridge_api.h       │ 给 dynamic_app_natives 用的入队/出队 API          │ ~30  │
  └────────────────────────────────────┴───────────────────────────────────────────────────┴──────┘

  修改文件

  ┌────────────────────────────────────┬────────────────────────────────────────────────────────────────────────────────┐
  │                文件                │                                      改动                                      │
  ├────────────────────────────────────┼────────────────────────────────────────────────────────────────────────────────┤
  │ services/CMakeLists.txt            │ 加 dynapp_bridge_service.c                                                     │
  ├────────────────────────────────────┼────────────────────────────────────────────────────────────────────────────────┤
  │ app/app_main.c                     │ 在 nimble_start 之前调 dynapp_bridge_service_init()                            │
  ├────────────────────────────────────┼────────────────────────────────────────────────────────────────────────────────┤
  │ dynamic_app/dynamic_app_internal.h │ 加 func_idx_sys_ble_send / func_idx_sys_ble_set_recv_cb + js_ble_recv_cb GCRef │
  ├────────────────────────────────────┼────────────────────────────────────────────────────────────────────────────────┤
  │ dynamic_app/dynamic_app_natives.c  │ 实现 js_sys_ble_send / js_sys_ble_set_recv_cb + 在 tick 里 drain inbox         │
  ├────────────────────────────────────┼────────────────────────────────────────────────────────────────────────────────┤
  │ dynamic_app/dynamic_app.c          │ teardown 时清 ble cb 引用                                                      │
  └────────────────────────────────────┴────────────────────────────────────────────────────────────────────────────────┘

  JS API 设计

  // 注册接收 cb：PC 推数据来时回调
  sys.ble.onRecv(function(payloadStr) {
      var msg = JSON.parse(payloadStr);
      // ... 业务处理
  });

  // 发送数据：仅在已连接时实际下发，未连接静默丢弃
  sys.ble.send(JSON.stringify({type: "ack", id: 42}));

  // 查询连接状态
  var connected = sys.ble.isConnected();

  只有 3 个 API。比 setInterval 那套还简洁。

  关键设计取舍

  1. payload 大小上限 200 字节：BLE MTU 默认 23（ATT layer），减去 3 byte header 实际只有 20 byte 单包；如果协商 MTU 247 实际能传 244
   byte。我们接 200 是个稳妥值。超长消息让 JS 自己拆包，C 不管。
  2. inbox 满了丢弃最老的：BLE 写命令是 PC 主动行为，ESP 无法 backpressure（NimBLE WRITE without response 直接成功返回）。满了 drop
  oldest 比 drop newest 体验好。
  3. outbox 满了 send() 返 false：JS 调 send 失败时自己决定重试还是放弃。
  4. app 切换时清两个队列：alarm 没消费完的 PC 消息不能漏给 calc。
  5. isConnected 不够，加一个 onConnect / onDisconnect 回调：app 想知道"PC 来了/走了"事件。后续可加，第一版先不做。

  潜在的坑（提前说，免得到时候重写）

  1. NimBLE notify 会被 BLE 协议栈限速：如果 JS 一秒 send 100 次，NimBLE 内部 mbuf 可能耗尽。outbox 队列 + 100ms 阻塞 send
  可以缓冲一下，但极端高频还是会丢。建议 JS 侧 setInterval 至少 50ms。
  2. 断线重连后回调要保留：连接断了再连，JS 注册的 onRecv 不应该消失（脚本没 stop）。GCRef 的生命周期跟脚本走，跟 BLE 连接无关。
  3. JS 异常不能让 BLE 任务挂掉：drain inbox 在 script task 里跑，JS 抛异常要 catch 住继续，参考现有 dynamic_app_run_intervals_once
  的处理。

  ---
  要不要按方案 C 落地？如果要，我会创建 task 跟踪四件事：

  1. dynapp_bridge_service.c（GATT + 双队列 + access_cb）
  2. dynamic_app_natives 三个 API + tick 里 drain inbox
  3. teardown / app 切换时清队列和 GCRef
  4. 写一个示例 JS app（比如最简单的 echo：PC 发啥就回啥）+ Python 端对应脚本

  或者你想走方案 B（带 channel 概念）也可以，区别就是 JS API 多个 channel 参数、C 侧多解析一个字段。你拍板。