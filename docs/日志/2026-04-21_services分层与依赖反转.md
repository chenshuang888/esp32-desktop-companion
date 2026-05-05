# services 分层与 drivers↔services 依赖反转日志

**日期：** 2026-04-21
**分支：** `more_reconstruct`
**涉及任务：** `04-21-drivers-services-dep-inversion`（已归档到 `archive/2026-04/`）
**配套未 commit 工作：** `manager/` 子目录搬迁 + `storage/` component 拆分（上一会话完成，本次会话在此基础上继续）

## 背景 / 这一轮想解决的问题

`services/` 目录在 `retire-control-service` 收尾后剩下一锅不分层的东西：

```
services/  (13 个 .c/.h 一锅)
├── *_service.c/h × 5          ← BLE GATT 接入层
├── *_manager.c/h × 5          ← BLE 线程 → UI 线程数据中转
├── ble_conn.c/h               ← drivers↔services 循环依赖的"绕路"
├── persist.c/h                ← NVS 通用封装
└── settings_store.c/h         ← 背光 + 时间的持久化门面
```

两个具体痛点：

| 痛点 | 根因 |
|---|---|
| `services/` 职责混杂，语义不清 | manager 不是 service、persist/settings_store 又不是 BLE 相关、ble_conn 只是为了绕循环依赖 |
| `drivers REQUIRES services`（反常）| `drivers/ble_driver.c` 集中 include + init 5 个 `*_service.h`；只能靠 `ble_conn` 这种"下沉共享状态"让 service 拿到 conn_handle |

这一轮解决办法分成三步（跨两次会话完成）：

1. **`services/manager/` 子目录**：manager 搬进去，视觉上和 service 分层（但仍在同一 component 内）
2. **`storage/` 独立 component**：persist + 拆 settings_store 成 3 个细粒度 store（backlight / time / notify）
3. **依赖反转**：drivers↔services 方向翻转，`ble_conn` 退役，SUBSCRIBE 改回调订阅模式

---

## Part 1：manager/ 子目录搬迁（未分 trellis task，纯结构整理）

### 起点问题

用户问"这些 xxx_manager 文件放一个 manager/ 文件夹合适吗？"进而挑战"它们真的是 manager 吗？"

### 是不是 manager？——对 manager 本质的再分析

读完 5 个 `*_manager.h/c` 发现它们共同做 4 件事：

| 职责 | 代表 API |
|---|---|
| 持有 FreeRTOS queue（BLE 线程 push → UI 线程消费） | `*_push` / `process_pending` |
| 持有内存快照（最新 payload 或环形缓冲） | `get_latest` / `get_at` |
| 暴露版本号 / epoch 给 UI 去重刷新 | `version` / `get_epoch` |
| 部分做持久化或派生计算 | `tick_flush` / `get_position_now` |

**本质是"BLE 线程→UI 线程的数据中转站 + 给 UI 读的状态仓库"**，更接近 Redux/Vuex 的 **store** 而非传统"资源管理器"语义。

### 候选命名讨论

| 候选 | 贴合度 | 否定理由 |
|---|---|---|
| `store/` | ★★★★★ | —— |
| `state/` | ★★★★ | 太泛；且和已有 `settings_store` 的 `store` 词冲突 |
| `model/` | ★★★ | 和"纯数据 model"混淆（这些带 push/flush 行为） |
| `cache/` | ★★ | 忽略持久化/派生计算 |

本该选 `store/`，但用户决定："**就叫 manager，改不改以后再说**"——工程上最小扰动优先。

### 落地方式（方案 A：子目录，不拆 component）

理由：如果 manager 独立成 component，会和 `services/persist.h` 形成循环依赖（manager 要 persist、service 要 manager）。子目录方案**源码一行不改**即可工作：

```
services/
├── *_service.c/h × 5
├── ble_conn.c/h
├── persist.c/h
├── settings_store.c/h
└── manager/                   ← 新建
    └── *_manager.c/h × 5      ← mv 10 个文件进来
```

`services/CMakeLists.txt`：`SRCS` 里 5 个 manager.c 加 `manager/` 前缀；`INCLUDE_DIRS` 追加 `"manager"`。头文件平铺可见，所有 `#include "xxx_manager.h"` 依然生效。

---

## Part 2：storage/ 独立 component 拆分

### 起点问题

`persist.c/h` 和 `settings_store.c/h` 留在 `services/` 根，语义错位：它们和 BLE 无关，但住在业务 service 旁边。用户："**这两个文件放哪里合适？**"

### 方向选择（3 选 1）

| 方向 | 代价 | 评价 |
|---|---|---|
| 留原地 | 0 | 语义混但能跑 |
| 挪到 `services/manager/` 做邻居 | 低 | ❌ 违背 manager 的"跨线程中转"语义 |
| 新建 `storage/` 顶级 component | 中 | ✅ 彻底分层；无循环依赖风险（storage 不依赖任何其他 component） |

用户选顶级 component："感觉这两个文件一个是底层 nvs init，一个是给上层提供写 nvs 的函数，这样做没问题"。

### NVS 调用面盘点（开工前）

grep 整个项目 `persist_*` 调用：

| 调用方 | 位置 | 用途 |
|---|---|---|
| `persist_init()` | `main/main.c` | 启动初始化 NVS |
| `persist_get/set_u8` | `services/settings_store.c` | 背光 |
| `persist_get/set_i64` | `services/settings_store.c` | 最近系统时间 |
| `persist_get/set_blob` | `services/manager/notify_manager.c` | 通知环形缓冲 |

发现一个锐化机会：`notify_manager.c` 是业务层，直接跨层调 `persist_*` 违背分层。用户："**让 settings_store 再封装一层吧，我不希望上层调 persist 底层**"。

### 拆分决策链（用户主导方向）

1. **扩 `settings_store` 加 `save_notifications`**？❌ 用户："settings 管通知历史语义不对"
2. **新建 `notify_store`**？✅ 用户："那就按业务拆三个：time_storage / backlight_storage / notify_storage"
3. **命名**：`_storage` 后缀（不用 `_store`，保持项目惯例）
4. **NVS namespace**：拆成三个独立 namespace（`backlight` / `time` / `notify`），旧 `settings` namespace 作废（开发阶段 erase-flash 无损失）
5. **`notify_storage` 不需要 init 函数**：它是无状态透传层；背光/时间需要 init 是因为有 `s_backlight` 缓存 / `s_last_save_us` 防抖节奏

### 最终目录结构

```
storage/                       ← 新建顶级 component
├── persist.c/h                (底层 NVS 封装)
├── backlight_storage.c/h      (namespace: backlight)
├── time_storage.c/h           (namespace: time)
├── notify_storage.c/h         (namespace: notify)
└── CMakeLists.txt             (REQUIRES nvs_flash, esp_timer)
```

**业务层 `persist_*` 调用 = 0**（main.c 的 `persist_init()` 是全局初始化保留，不算业务调用）。

### CMakeLists 链式改动

| 文件 | 改动 |
|---|---|
| 顶层 `CMakeLists.txt` | `EXTRA_COMPONENT_DIRS` 追加 `storage` |
| `storage/CMakeLists.txt` | 新建（SRCS 4 文件；REQUIRES `nvs_flash`, `esp_timer`）|
| `services/CMakeLists.txt` | 移除 `persist.c/settings_store.c`；REQUIRES `nvs_flash` → `storage` |
| `app/CMakeLists.txt` | REQUIRES 追加 `storage`（page_menu 直接用 backlight_storage） |
| `main/CMakeLists.txt` | REQUIRES 追加 `storage` |

依赖链：`main / app → services → storage → nvs_flash`，单向无循环。

---

## Part 3：drivers↔services 依赖反转（Task `04-21-drivers-services-dep-inversion`）

### 起点问题

`services/` 清理到最后剩 `ble_conn.c/h`（34 行共享状态中转层）。用户："这个文件放哪里？"

### 诚实评估：它是"架构妥协"的症状

承认了先前的浅回答不够。`ble_conn` 难处理的**根本原因**是反常依赖：

```
drivers ──REQUIRES──▶ services   （反常方向）
   │                      ▲
   │ 写                  │ 读
   └─▶ ble_conn ◀─────────┘
```

- **想放 drivers/**：services 读不到（services 不能 REQUIRES drivers，否则循环）❌
- **想放 services/**：语义混（它不是业务 service）⚠️
- **独立 component**：34 行单飞过度设计 ⚠️

**三个选项**：

| 选项 | 说明 | 推荐指数 |
|---|---|---|
| A. 留原地 | 承认当前架构小妥协 | ★★★★ |
| B. 独立 `ble_shared/` component | 解决命名洁癖；但**架构反常依旧** | ★★ |
| C. 依赖反转重构 | 真正根治；10+ 文件改动 | ★★★ |

用户："**我希望做吧，好处很大**"——选 C。

### 关键发现（影响工作量判断）

读代码才发现一个一直误解的事实：**GATT 表本来就是 service 自管的**。`ble_driver_init()` 内部只是按顺序调用 5 个 `*_service_init()` 作为时序触发器，不涉及"中央注册 GATT 表"的抽象。

| 想象中 | 实际 |
|---|---|
| drivers 集中管理 5 个 service 的 GATT 表 | ❌ |
| drivers 顺序调用 service_init 作为时序触发 | ✅ |

这让重构比预估简单：不需要设计"GATT 表注册 API"，只需让 `app_main` 接管调用顺序即可。

### API 设计（drivers 侧）

```c
// 拆生命周期为两段
esp_err_t ble_driver_nimble_init(void);    // nimble_port_init + gap/gatt_init + name set
esp_err_t ble_driver_nimble_start(void);   // ble_hs_cfg + store_config + host task 启动

// 吸收 ble_conn 的职责
bool ble_driver_is_connected(void);
bool ble_driver_get_conn_handle(uint16_t *out);

// SUBSCRIBE 事件订阅（容 8 个 cb）
typedef void (*ble_driver_subscribe_cb_t)(uint16_t attr_handle,
                                          uint8_t prev_notify, uint8_t cur_notify);
esp_err_t ble_driver_register_subscribe_cb(ble_driver_subscribe_cb_t cb);
```

### 启动时序（main.c）

**关键 NimBLE 约束**：`ble_gatts_add_svcs()` 必须在 `nimble_port_freertos_init()` 启动 host task 之前完成——host task 跑起来后 sync 回调会立刻 `start_advertising`，GATT 表必须已就绪。

```c
// 现在
ble_driver_init();       // 内部一把梭

// 重构后
ble_driver_nimble_init();      // 开"GATT 注册窗口"
time_service_init();            // 各 service 自己 add_svcs + register_subscribe_cb
weather_service_init();
notify_service_init();
media_service_init();
system_service_init();
ble_driver_nimble_start();     // 关窗口，启动 host task
```

### 改动清单（11 文件）

| 类别 | 位置 |
|---|---|
| 删除 | `services/ble_conn.c/h` |
| 新 API | `drivers/ble_driver.h/c` |
| include + 调用换 | 4 × `*_service.c`（time/weather/system/media）+ `app/pages/page_music.c` |
| on_subscribe 改 static | time/weather/system 3 对（.h 删声明，.c 加前向声明 + init 末尾注册）|
| 启动序列 | `main/main.c`（补 5 × service include） |
| CMakeLists | `drivers/CMakeLists.txt` 去 `services`；`services/CMakeLists.txt` 加 `drivers` |
| spec | 归档 `ble-conn-shared-state-*.md` 到 `_archived_unrelated/2026-04-21/firmware/`；改 `firmware/index.md` / `iot/index.md` / `esp-to-pc-notify-request-pattern-playbook.md` |

### 关键决策

| 决策点 | 选择 | 理由 |
|---|---|---|
| 拆 init 粒度 | `nimble_init` + `nimble_start` | 中间夹 service_init 窗口 |
| subscribe_cb 数组容量 | 8 | 当前只 3（time/weather/system），预留 5 个余量 |
| `*_service_on_subscribe` 可见性 | 改 static + header 删声明 | 不再对外暴露，由 init 内部注册 |
| `ble_conn.c/h` | 彻底删除 | 34 行中转层，依赖反转后无存在意义 |
| `s_conn_handle` | 补 `volatile` | 原先 ble_conn 里是 volatile；复用 drivers 自己的变量必须同等保证（单写多读契约） |

---

## Part 4：编译坑 —— 上一轮的遗留债被本次 build 首次暴露

### 现象

`idf.py build` 卡在 `notify_manager.c:57`：

```
error: 'ESP_ERR_NVS_NOT_FOUND' undeclared (first use in this function);
       did you mean 'ESP_ERR_NOT_FOUND'?
```

### 定位

- `persist.h` 从早期就 include 了 `nvs.h`（注释明写"暴露 ESP_ERR_NVS_* 常量"）
- 上一轮 storage 拆分时新建的 `notify_storage.h` 只 include 了 `esp_err.h`，**忘了继承这个 pattern**
- `notify_manager.c` 同一轮把 include 从 `persist.h` 换到 `notify_storage.h`，丢了 `ESP_ERR_NVS_NOT_FOUND` 符号
- 上一轮没立即编译，潜伏到本轮首次 build 才爆出来

### 修复

`notify_storage.h` 追加 `#include "nvs.h"`（带同款注释）。1 行改动，编译通过。

### 教训

- **NVS 错误码不在 `esp_err.h` 里**（`ESP_ERR_NVS_*` 系列定义在 `nvs.h`）——这个坑 `docs/NVS持久化日志.md` 的"坑 1"章节已经记过一遍，但做拆分时仍然漏了。复用 pattern 也要主动"继承 include"
- **写完代码不验证就收工是坑**：上一轮"拆 storage"对话如果当时 build 一下，这 1 行 bug 当场修；延后两天在完全无关的任务里暴露，查起来多花的时间远超 1 行修复的成本

---

## 数字汇总

| 项 | Part 1 manager | Part 2 storage | Part 3 dep-inversion | 合计 |
|---|---|---|---|---|
| 新建目录 | 1（`services/manager/`） | 1（`storage/`） | 0 | 2 |
| 新建文件 | 0（mv） | 4（3 store + CMakeLists） | 0 | 4 |
| 删除文件 | 0 | 2（settings_store.c/h） | 2（ble_conn.c/h） | 4 |
| 修改源文件 | 1（services CMakeLists） | 5（4 callsite + 1 CMake） | 11 | 17 |
| 修改 CMakeLists | 1 | 5 | 2 | 8 |
| 改动 spec | 0 | 0 | 4（1 归档 + 3 改）| 4 |

## 最终架构

```
demo6/
├── main/                      main.c 编排启动序列
├── app/                       页面 + page_router
├── framework/                 LVGL UI 脚手架
├── drivers/                   硬件 + NimBLE 生命周期（纯粹，不感知业务 service）
│   └── ble_driver.c/h        subscribe_cb 订阅机制
├── services/                  BLE 业务层
│   ├── *_service.c/h × 5     GATT 接入（每个自管 add_svcs + register_subscribe_cb）
│   └── manager/              × 5 跨线程数据中转
└── storage/                   NVS 封装（独立 component）
    ├── persist.c/h           底层 KV/blob
    ├── backlight_storage.c/h 背光
    ├── time_storage.c/h      系统时间
    └── notify_storage.c/h    通知环形缓冲
```

**依赖方向（全部单向）**：

```
main ──▶ app ──▶ services ──▶ drivers
                     │
                     └──▶ storage ──▶ nvs_flash
```

## 一句话总结

**三步走完成从"一锅 services"到"职责清晰的分层组件 + 正向依赖图"**：
1. Part 1 用**子目录**解决视觉分层（最小扰动）
2. Part 2 用**独立 component**解决 storage 职责归属（抽象 persist 细节）
3. Part 3 用**依赖反转 + 回调订阅**解决架构债（代价最大但收益最大，今后加 service 完全不用改 drivers）

本轮之后 `services/` 的每一项都能一句话解释清楚它在哪、为什么在那、依赖谁——"为什么有个 ble_conn 在这儿"这种无法解释的工件彻底消失。
