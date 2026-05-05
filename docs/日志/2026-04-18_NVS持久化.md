# NVS 持久化开发日志

**日期：** 2026-04-18
**分支：** `feat/more_ble_services`

## 任务概述

之前所有运行时状态掉电即丢：系统时间每次开机回到硬编码默认、背光亮度复位、通知列表清空。本次引入 **NVS 持久化**，且按用户要求做成"底层提供接口给上层使用"的分层框架，而不是把 `nvs_*` 调用散到各处。

**目标：**
- 抽离 NVS 访问到统一的 KV 层，上层业务代码完全不见 `nvs_*`
- 按数据特性选回写策略：低频立即写、高频防抖写、定时快照周期写
- Flash 寿命可控（NVS wear-leveling + 合理写入频率）
- 预留未来 WiFi/绑定/闹钟等新数据的扩展位

---

## 阶段一：选型讨论（为什么是 NVS 不是文件系统）

用户一度担心 NVS 不够用，问是否需要 SPIFFS/LittleFS。盘点本项目要存的数据：

| 数据 | 大小 | 形态 |
|---|---|---|
| 背光亮度 | 1 B | u8 |
| 最近系统时间 | 8 B | i64（time_t） |
| 通知环形缓冲 10 条 | 1.36 KB | blob |
| 未来预留 (WiFi/闹钟/绑定) | < 500 B | 零散小 KV |

**合计 < 2 KB**，NVS 默认 24 KB 分区完全容纳。文件系统是给几十上百个独立文件/MB 级流式数据准备的，这个场景里强上文件系统只会带来复杂度而无收益——**典型的"如无必要勿增实体"**。

---

## 阶段二：分层架构设计

```
┌───────────────────────────────────────────────┐
│  应用层（app/pages/*, main.c, notify_manager 使用方） │
│   settings_store_set_backlight(128);          │
│   notify_manager_push(&n);  ← 内部自动置 dirty  │
└──────────────────┬────────────────────────────┘
                   │
┌──────────────────▼────────────────────────────┐
│  语义存储层（services/settings_store.c        │
│             / notify_manager.c 增强）          │
│   按模块管理 namespace + key + 序列化格式     │
│   维护内存缓存、dirty 标志、flush 判定         │
└──────────────────┬────────────────────────────┘
                   │
┌──────────────────▼────────────────────────────┐
│  KV 抽象层（services/persist.c）              │
│   persist_init / get_u8 / set_u8 / get_blob   │
│   封装 nvs_open/commit/close，短事务          │
│   唯一直接调用 ESP-IDF NVS API 的地方          │
└──────────────────┬────────────────────────────┘
                   │
              ESP-IDF nvs_flash
```

### 底层 KV：`services/persist.{h,c}`

提供与 NVS 类型一一对应的薄包装：
```c
esp_err_t persist_init(void);  // 替代 ble_driver 原先的 nvs_flash_init
esp_err_t persist_get_u8 (const char *ns, const char *key, uint8_t  *out);
esp_err_t persist_set_u8 (const char *ns, const char *key, uint8_t   val);
esp_err_t persist_get_i64(const char *ns, const char *key, int64_t *out);
esp_err_t persist_set_i64(const char *ns, const char *key, int64_t   val);
esp_err_t persist_get_blob(const char *ns, const char *key, void *buf, size_t *len);
esp_err_t persist_set_blob(const char *ns, const char *key, const void *buf, size_t len);
esp_err_t persist_erase_namespace(const char *ns);
```

每次 set 内部都是短事务：`nvs_open(RW) → nvs_set_* → nvs_commit → nvs_close`。不做内存缓存（交给上层），不做复杂错误处理（`ESP_LOGW` 日志 + 透传 esp_err_t）。

### 语义层：`services/settings_store.{h,c}`

管理背光（立即写）+ 最近系统时间（周期写）。

```c
uint8_t   settings_store_get_backlight(void);
esp_err_t settings_store_set_backlight(uint8_t duty);    // 内部立即写
esp_err_t settings_store_load_last_time(struct timeval *out);
void      settings_store_tick_save_time(void);           // UI 线程周期调
```

**注意决策**：`set_backlight` 仅负责持久化 + 更新缓存，**不直接**调 `lcd_panel_set_backlight`。如果让 services 层调 drivers 层接口会导致 services → drivers 反向依赖（drivers 已 REQUIRES services，会形成循环）。所以硬件生效交给调用方：page_menu 里先调 `settings_store_set_backlight(duty)` 再调 `lcd_panel_set_backlight(duty)`，一次菜单切档写两行代码，但依赖方向干净。

### notify_manager 增强

**对外 API 不变**，内部加持久化。

- `init()`：尝试从 NVS 加载 blob；失败当空处理
- `process_pending()`：消费队列后，有变化则 `s_dirty = true`，记录 `s_dirty_since_us`
- 新增 `tick_flush()`：若 dirty 且距 dirty_since 超过 2 秒，落盘
- `clear()`：立即写空态

存储格式（blob 带版本号）：
```c
typedef struct {
    uint8_t  version;    // = NOTIFY_PERSIST_VERSION
    uint8_t  head;
    uint8_t  count;
    uint8_t  _pad;
    notification_payload_t ring[NOTIFY_STORE_MAX];
} __attribute__((packed)) notify_persist_v1_t;  // 1364 B
```

加载时若 `version != 1` 视为不兼容直接丢弃，避免未来格式演进时写一坨 migration 代码。

---

## 阶段三：数据布局与回写策略

| 数据 | namespace | key | 策略 | 每日最坏写次数 |
|---|---|---|---|---|
| 背光 | `settings` | `bl` | 用户点击时立即写 | < 10 |
| 系统时间 | `settings` | `last_ts` | UI 线程每 5 分钟 | 288 |
| 通知快照 | `notify` | `ring` | dirty + 2 秒防抖 | < 100 |

**Flash 寿命估算**：每日 ~400 次，NVS 24 KB = 6 sectors × 10 万次擦写寿命，除去 wear-leveling 放大因子约 30 万次有效擦写，约 **2 年磨损下限**。这种桌面玩具项目够用；若要拉长，时间写入间隔拉到 30 分钟即可 × 6 倍。

---

## 阶段四：初始化顺序调整

### 关键改动：把 NVS 初始化从 BLE 提前到 main 最开始

原本 `nvs_flash_init()` 埋在 `ble_driver_init()` 里（BLE 自己要用 NVS 存绑定信息）。现在 `settings_store` / `notify_manager` 都要在 BLE 之前就读取 NVS，所以必须前置。

**main.c**：
```c
void app_main(void) {
    ESP_ERROR_CHECK(persist_init());              // ← 最先
    ESP_ERROR_CHECK(settings_store_init());       // 加载背光缓存

    /* 恢复上次系统时间，失败回退默认 */
    struct timeval tv;
    if (settings_store_load_last_time(&tv) == ESP_OK) {
        settimeofday(&tv, NULL);
    } else {
        init_default_time();
    }

    time_manager_init();
    weather_manager_init();
    notify_manager_init();       // 内部自动 load 上次快照
    ble_driver_init();           // 不再调 nvs_flash_init
    app_main_init();
}
```

**ble_driver.c**：删除 `nvs_flash_init()` 及其错误处理的 10 行，`persist_init` 已负责。

**app_main.c**：
- `lvgl_port_init()` 后一行：`lcd_panel_set_backlight(settings_store_get_backlight())` — 应用上次背光
- `ui_task` 循环末尾加：
  ```c
  notify_manager_tick_flush();
  settings_store_tick_save_time();
  ```
  两个 tick 函数内部都先判断"是否到时间/是否 dirty"，不到条件立即返回，绝大多数 10ms 周期里啥也不干。

**page_menu.c**：背光切档逻辑改为先持久化再生效：
```c
settings_store_set_backlight(duty);
lcd_panel_set_backlight(duty);
```

---

## 阶段五：编译遇到的坑

### 坑 1：`ESP_ERR_NVS_NOT_FOUND` 未声明

`settings_store.c` 和 `notify_manager.c` 编译失败：
```
error: 'ESP_ERR_NVS_NOT_FOUND' undeclared
did you mean 'ESP_ERR_NOT_FOUND'?
```

**根因**：`ESP_ERR_NVS_NOT_FOUND` 定义在 `nvs.h`，不是 `esp_err.h`。我在 `persist.h` 漏了这个 include，而 `persist.h` 又在 public API 里返回这些错误码。

**修复**：`persist.h` 顶部加 `#include "nvs.h"`。这意味着 persist 层没有完全屏蔽 NVS 细节——但调用方本来就要判 `NOT_FOUND` 区分"没存过"和"出错"两种语义，硬转译会丢信息。该露就露。

### 坑 2：`fatal error: nvs.h: No such file or directory`（main 组件）

坑 1 修完后 main.c 编译失败，因为 main 组件通过 `persist.h` 间接 include `nvs.h`，但 services 的 CMakeLists 把 `nvs_flash` 放在 `PRIV_REQUIRES` 里。

**ESP-IDF 依赖类型的区别**：
- **PRIV_REQUIRES**：私有依赖，只有本组件的 `.c` 能 include，**不传递**给使用者
- **REQUIRES**：公共依赖，凡是 REQUIRES 本组件的外部组件自动看到这个依赖的头文件路径

**修复**：`services/CMakeLists.txt` 把 `nvs_flash` 从 PRIV_REQUIRES 提到 REQUIRES：

```cmake
PRIV_REQUIRES
    bt                       # 仅 time_service/weather_service/notify_service 内部用
REQUIRES
    esp_timer
    nvs_flash                # persist.h 暴露 NVS 错误码 → 必须 public
```

**经验**：只要某个依赖的符号出现在组件公共 header 里，就必须 public REQUIRES；只有 `.c` 内部用的放 PRIV_REQUIRES 就好。

### 坑 3：main 组件没有显式 REQUIRES services

为了让 `main.c` 能直接 `#include "persist.h"`，给 `main/CMakeLists.txt` 加了 `REQUIRES app services drivers`。虽然通过 `main → app → drivers → services` 的传递链也能间接解析，但显式声明更清晰，调试和理解依赖关系都更容易。

---

## 验证结果

用户本地编译通过，烧录后测试：

- ✓ 背光：调整亮度 → 重启 → 背光保持
- ✓ 通知：推送几条 → 等防抖落盘 → 重启 → 列表保留
- ✓ 系统时间：可从 NVS 恢复（而非硬编码默认）
- ✓ 初次烧录 / erase-flash：无崩溃，所有模块回退默认

---

## 经验教训

1. **ESP-IDF 组件依赖区分 public 和 private 非常关键**：public header 里用到的符号，对应依赖必须 public REQUIRES，否则上层使用者编译不过。"能编译的私有依赖" 不一定是"正确的私有依赖"。

2. **NVS 错误码不在 esp_err.h**：`ESP_ERR_NVS_*` 系列定义在 `nvs.h`。写 KV 抽象层时要考虑错误码在 public 接口的可见性——完全隐藏 NVS 符号需要自定义错误码做转译，代价大于收益，**暴露 `nvs.h` 是务实的选择**。

3. **services 不调 drivers 是条值得坚持的边界**：初版 plan 想让 `settings_store_set_backlight` 内部直接调 `lcd_panel_set_backlight`，但 drivers 已 REQUIRES services，反过来会循环。调用方多写一行 `lcd_panel_set_backlight(duty)` 成本远低于"循环依赖带来的奇怪编译错误"。

4. **dirty + 防抖是小嵌入式持久化的甜点区**：既避免每次修改都写 flash 磨损，又把"多条变化合并一次写"做成隐式行为，上层无感。`esp_timer_get_time()` 比 FreeRTOS tick 更合适，精度 1μs 且不受调度影响。

5. **blob 带 version 字段，一旦不匹配就丢**：migration 代码看似稳妥实则复杂度指数增长。版本号从 1 起步，将来改结构时升版本 + 旧数据丢弃；用户失去一次快照不致命。

6. **NVS 的空间预算要显式算**：别看"才 24KB"就觉得够用，6 个 sector 写满就爆。每年 ~15 万次写 ≈ 每 3 秒一次（极端情况）。用 dirty 防抖能把"连续推 15 条"的写次数从 15 压到 1。

---

## 关键文件清单

**新建（4 个）：**
- `services/persist.h`
- `services/persist.c`
- `services/settings_store.h`
- `services/settings_store.c`

**修改（7 个）：**
- `services/CMakeLists.txt` — 注册新文件 + public REQUIRES nvs_flash/esp_timer
- `services/notify_manager.h` — 新增 `notify_manager_tick_flush()` 声明
- `services/notify_manager.c` — init 加 load、process_pending 后 dirty、tick_flush 防抖落盘、clear 立即写
- `drivers/ble_driver.c` — 删除 nvs_flash_init 调用（persist_init 已做）
- `drivers/CMakeLists.txt` — 从 REQUIRES 移除 nvs_flash（已由 services public 传递）
- `main/main.c` — 加 persist_init/settings_store_init，恢复 last_time
- `main/CMakeLists.txt` — 显式 REQUIRES app services drivers
- `app/app_main.c` — 应用上次背光 + UI 循环接入两个 tick_flush
- `app/pages/page_menu.c` — 背光切档走 settings_store

---

## 后续可做

- **NVS erase 菜单项**：About 页加一个"恢复出厂"按钮，调 `persist_erase_namespace("settings")` + `"notify"`
- **时间写入频率自适应**：启动后第一分钟拉长到 60s（刚开机系统时间还可能在 BLE 同步过程中波动），稳定后改回 5 分钟
- **添加 WiFi / 闹钟等时直接复用 persist 层**：分配新 namespace 即可，不需动底层
- **`notify_manager_clear()` 暴露到 UI**：目前是内部 API，通知页可以加个"清空全部"按钮调用
