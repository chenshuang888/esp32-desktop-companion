# BLE 蓝牙集成开发日志

## 任务概述

为 ESP32-S3 项目集成 NimBLE 蓝牙协议栈，实现基本的 BLE 广播和连接功能。

**目标：**
- 初始化 NimBLE 协议栈
- 实现 BLE 广播功能
- 支持手机连接
- 提供简洁的驱动接口

---

## 开发过程

### 阶段一：需求分析

**用户需求：**
- 了解 ESP-IDF 提供的 BLE 初始化方法
- 创建独立的 BLE 驱动模块
- 放在 drivers 文件夹作为驱动代码
- 在 main.c 中调用初始化

**技术选型：**
- ESP32-S3 只支持 BLE 4.2，不支持经典蓝牙
- 选择 NimBLE 协议栈（轻量级，内存占用小）
- 不选择 Bluedroid（功能完整但内存占用大）

---

### 阶段二：官方示例研究

**查找示例位置：**
```
C:\esp_v5.4.3\v5.4.3\esp-idf\examples\bluetooth\
├── ble_get_started\
│   ├── nimble\NimBLE_GATT_Server\
│   └── bluedroid\Bluedroid_GATT_Server\
```

**NimBLE 初始化流程：**
1. 初始化 NVS（存储蓝牙配置）
2. 初始化 NimBLE 协议栈
3. 初始化 GAP 和 GATT 服务
4. 配置回调函数
5. 启动 NimBLE 主机任务
6. 开始广播

---

### 阶段三：代码实现

**创建的文件：**
- `drivers/ble_driver.h` - 头文件，提供公共接口
- `drivers/ble_driver.c` - 实现文件

**提供的接口：**
```c
esp_err_t ble_driver_init(void);              // 初始化 BLE
esp_err_t ble_driver_start_advertising(void); // 启动广播
esp_err_t ble_driver_stop_advertising(void);  // 停止广播
bool ble_driver_is_connected(void);           // 获取连接状态
```

**设备配置：**
- 设备名称：ESP32-S3-DEMO
- 广播间隔：20-40ms
- 连接模式：可连接、可发现

---

## Bug 修复历程（一场灾难）

### Bug #1：组件依赖声明错误

**问题：**
```
ERROR: Version solving failed:
    - no versions of espressif/esp_nimble match ^1.0.0
```

**原因：**
- 错误地在 `idf_component.yml` 中添加了 `espressif/esp_nimble` 依赖
- NimBLE 是 ESP-IDF 内置组件，不需要外部依赖

**解决方案：**
- 从 `idf_component.yml` 中删除 NimBLE 依赖
- NimBLE 通过 sdkconfig 配置启用

**教训：**
- 不是所有组件都需要在 `idf_component.yml` 中声明
- ESP-IDF 内置组件通过 sdkconfig 启用

---

### Bug #2：头文件缺少 stdbool.h

**问题：**
```
error: unknown type name 'bool'
note: 'bool' is defined in header '<stdbool.h>'
```

**原因：**
- `ble_driver.h` 中使用了 `bool` 类型
- 但没有包含 `<stdbool.h>` 头文件

**解决方案：**
```c
#pragma once

#include <stdbool.h>  // 添加这一行
#include "esp_err.h"
```

**教训：**
- C 语言的 `bool` 类型需要包含 `<stdbool.h>`
- 不要假设头文件会被其他文件间接包含

---

### Bug #3：NimBLE 头文件找不到

**问题：**
```
fatal error: nimble/nimble_port.h: No such file or directory
```

**原因：**
- sdkconfig 中 `CONFIG_BT_ENABLED` 没有启用
- 虽然在 `sdkconfig.defaults` 中设置了，但旧的 `sdkconfig` 覆盖了配置
- bt 组件的头文件路径只有在 `CONFIG_BT_NIMBLE_ENABLED=y` 时才会被添加

**解决方案：**
1. 在 `sdkconfig.defaults` 中添加 BLE 配置：
   ```
   CONFIG_BT_ENABLED=y
   CONFIG_BT_NIMBLE_ENABLED=y
   CONFIG_BT_NIMBLE_50_FEATURE_SUPPORT=n
   ```

2. 删除旧的 `sdkconfig` 文件：
   ```bash
   rm sdkconfig
   ```

3. 删除 build 目录重新编译：
   ```bash
   rm -rf build
   ```

**教训：**
- ESP-IDF 项目修改 `sdkconfig.defaults` 后，需要删除旧的 `sdkconfig`
- 或者使用 `idf.py fullclean` 清理所有配置

---

### Bug #4：CMakeLists.txt 依赖配置错误

**问题：**
- 即使 BLE 配置启用了，头文件路径还是找不到

**原因：**
- `drivers/CMakeLists.txt` 中 bt 组件依赖配置不正确
- 最初使用了 `REQUIRES bt`，应该使用 `PRIV_REQUIRES bt`

**解决方案：**
```cmake
idf_component_register(
    SRCS
        "lcd_panel.c"
        "touch_ft5x06.c"
        "lvgl_port.c"
        "ble_driver.c"
    INCLUDE_DIRS
        "."
    REQUIRES
        lvgl
        esp_lcd_touch_ft5x06
        esp_timer
        driver
        esp_lcd
        nvs_flash
    PRIV_REQUIRES
        bt  # 私有依赖，不传递给其他组件
)
```

**REQUIRES vs PRIV_REQUIRES：**
- `REQUIRES`：公共依赖，会传递给依赖此组件的其他组件
- `PRIV_REQUIRES`：私有依赖，只在当前组件内部可见
- bt 组件应该是私有依赖，因为 BLE 实现细节不需要暴露给其他组件

**教训：**
- 理解 CMake 的依赖传递机制
- 合理使用 REQUIRES 和 PRIV_REQUIRES，保持良好的封装性

---

### Bug #5：ble_store_config_init 函数未声明

**问题：**
```
error: implicit declaration of function 'ble_store_config_init'
```

**原因：**
- `ble_store_config_init()` 是 NimBLE 的库函数
- 但没有对应的头文件声明
- 官方示例中是手动声明的

**解决方案：**
```c
/* 外部库函数声明 */
void ble_store_config_init(void);
```

**教训：**
- 有些 ESP-IDF 的库函数没有公开头文件
- 需要参考官方示例，手动声明函数原型

---

## 配置文件修改总结

### 1. sdkconfig.defaults

```ini
# Bluetooth Configuration
CONFIG_BT_ENABLED=y
CONFIG_BT_NIMBLE_ENABLED=y
CONFIG_BT_NIMBLE_50_FEATURE_SUPPORT=n
```

### 2. drivers/CMakeLists.txt

```cmake
idf_component_register(
    SRCS
        "lcd_panel.c"
        "touch_ft5x06.c"
        "lvgl_port.c"
        "ble_driver.c"  # 新增
    INCLUDE_DIRS
        "."
    REQUIRES
        lvgl
        esp_lcd_touch_ft5x06
        esp_timer
        driver
        esp_lcd
        nvs_flash
    PRIV_REQUIRES
        bt  # 新增
)
```

### 3. main/main.c

```c
#include "ble_driver.h"

void app_main(void)
{
    ESP_LOGI(TAG, "Starting application");

    // 先初始化系统时间
    init_default_time();

    // 初始化 BLE
    ESP_ERROR_CHECK(ble_driver_init());

    // 再初始化应用
    ESP_ERROR_CHECK(app_main_init());

    ESP_LOGI(TAG, "Application started");
}
```

---

## 验证结果

### 串口日志输出

```
I (402) ble_driver: Initializing BLE driver
I (420) BLE_INIT: BT controller compile version [2edb0b0]
I (420) BLE_INIT: Using main XTAL as clock source
I (427) BLE_INIT: Bluetooth MAC: dc:b4:d9:1b:cd:a2
I (472) ble_driver: BLE host task started
I (477) ble_driver: BLE stack synced
I (477) ble_driver: Device address: 00:00:00:00:00:00
I (478) ble_driver: Starting BLE advertising
I (501) ble_driver: BLE advertising started
I (504) ble_driver: BLE driver initialized successfully
...
I (423307) ble_driver: Connection established; status=0
```

### 手机测试

**使用工具：** nRF Connect for Mobile (Android)

**测试步骤：**
1. 打开 nRF Connect APP
2. 点击 SCAN 扫描设备
3. 找到 "ESP32-S3-DEMO" 设备
4. 点击 CONNECT 连接
5. 连接成功，串口输出连接日志

**测试结果：** ✅ 成功

---

## 关键经验总结

### 1. ESP-IDF 项目配置的复杂性

ESP-IDF 项目不只是写代码，还涉及多个配置文件的修改：

- **CMakeLists.txt**（多个层级）
  - 项目根目录
  - 各组件目录
  - 添加源文件、依赖关系

- **idf_component.yml**
  - 声明外部组件依赖
  - 内置组件不需要声明

- **sdkconfig.defaults**
  - 默认配置选项
  - 启用/禁用功能模块

- **sdkconfig**
  - 实际生效的配置
  - 修改 defaults 后需要删除重新生成

### 2. 配置文件的优先级

```
sdkconfig > sdkconfig.defaults
```

- 修改 `sdkconfig.defaults` 后，必须删除 `sdkconfig` 才能生效
- 或者使用 `idf.py fullclean` 清理

### 3. 组件依赖的封装性

- 使用 `PRIV_REQUIRES` 隐藏实现细节
- 只暴露必要的接口给其他组件
- 类似面向对象的封装原则

### 4. 参考官方示例的重要性

- 官方示例包含完整的配置
- 不只是代码，还有 CMakeLists.txt、sdkconfig.defaults
- 遇到问题时，对比官方示例的所有文件

### 5. AI 的局限性

**AI 在这次任务中的表现：**
- ❌ 没有一次性考虑所有配置文件
- ❌ 总是遗漏某些配置，需要用户提醒
- ❌ 对 ESP-IDF 的配置机制理解不够深入
- ❌ 修了 6 次才成功，效率极低

**用户不得不做的事：**
- 提醒检查 idf_component.yml
- 提醒检查 sdkconfig.defaults
- 提醒删除 sdkconfig 重新生成
- 提醒检查 CMakeLists.txt 依赖配置
- 每次都要给 AI 兜底

**教训：**
- ESP-IDF 项目集成新功能时，必须主动、全面地考虑所有配置
- 不能等用户提醒才想起来
- 应该先列出检查清单，一次性完成所有配置

---

## 后续改进方向

### 1. 添加 GATT 服务

当前只实现了基本的广播和连接，可以添加 GATT 服务实现数据传输：

- 时间同步服务：手机 APP 设置 ESP32 时间
- 参数配置服务：远程修改设备参数
- 数据上报服务：ESP32 上报传感器数据

### 2. 优化连接参数

- 调整广播间隔
- 设置连接超时
- 实现自动重连

### 3. 添加安全认证

- 启用配对功能
- 使用加密连接
- 防止未授权访问

### 4. 低功耗优化

- 动态调整广播功率
- 连接后降低广播频率
- 实现睡眠唤醒机制

---

## 项目当前状态

**已完成功能：**
- ✅ LVGL UI 界面（现代化卡片设计）
- ✅ 多页面路由系统
- ✅ 触摸交互
- ✅ 时间调节功能
- ✅ BLE 蓝牙通信（广播、连接）

**项目架构：**
```
demo6/
├── app/                    # 应用层
│   ├── pages/             # 页面实现
│   │   ├── page_time.c    # 时间调节页面
│   │   └── page_menu.c    # 菜单页面
│   └── app_main.c         # 应用初始化
├── framework/             # 框架层
│   └── page_router.c      # 页面路由
├── drivers/               # 驱动层
│   ├── lcd_panel.c        # LCD 驱动
│   ├── touch_ft5x06.c     # 触摸驱动
│   ├── lvgl_port.c        # LVGL 移植
│   └── ble_driver.c       # BLE 驱动 (新增)
├── main/                  # 入口
│   └── main.c
└── docs/                  # 文档
    ├── 开发日志.md
    └── BLE集成开发日志.md (本文档)
```

**代码质量：**
- 模块化设计，职责清晰
- HTML/CSS/JS 分离模式（页面代码）
- 良好的封装性（PRIV_REQUIRES）
- 完整的错误处理

---

## 总结

这次 BLE 集成任务暴露了 ESP-IDF 项目开发的复杂性：

1. **配置文件众多**：CMakeLists.txt、idf_component.yml、sdkconfig.defaults
2. **配置优先级复杂**：sdkconfig 会覆盖 defaults
3. **依赖关系复杂**：REQUIRES vs PRIV_REQUIRES
4. **文档不完整**：有些函数需要手动声明

**最大的教训：**
- 集成新功能时，必须一次性考虑所有配置文件
- 不能依赖 AI 的提醒，要主动、全面地思考
- 参考官方示例的所有文件，不只是代码

**最终结果：**
- 虽然过程曲折，但功能完整实现
- BLE 驱动封装良好，接口简洁
- 为后续功能扩展打下了基础

---

## 附录：ESP32-S3 蓝牙支持对比

| 芯片型号 | BLE 4.2 | BLE 5.0 | 经典蓝牙 |
|---------|---------|---------|---------|
| ESP32   | ✅      | ❌      | ✅      |
| ESP32-S3| ✅      | ❌      | ❌      |
| ESP32-C3| ✅      | ✅      | ❌      |
| ESP32-C6| ✅      | ✅      | ❌      |

**ESP32-S3 只支持 BLE 4.2，不支持经典蓝牙和 BLE 5.0。**

---
---

# 续篇：从连接成功到功能完备

> *上一章结束在"BLE 广播 + 连接"。这一章记录的是从"连得上"到"用起来"的历程：时间同步服务、UI 统一风格、线程安全改造，以及一个完整的"PC 代理天气推送"功能。*

**时间范围：** 2026-04-17 晚 ~ 2026-04-18
**涉及提交：**
- `d97a52c` ble部分time服务完成
- `25fdc20` 优化了页面布局
- `0737a0f` 优化了线程之间的安全
- *（未提交）* 天气推送功能

---

## 阶段五：BLE 时间同步服务（CTS）

### 5.1 需求背景

光有广播和连接没有实际用途，需要通过 GATT 传递真实业务数据。第一个落地场景是**时间同步**：让手机 / PC 能读取 ESP32 的系统时间，也能把当前时间写进去（ESP32 开机后时钟从 0 开始，需要一个来源校准）。

### 5.2 技术选型：标准 CTS vs 自定义服务

两种路径：

| 方案 | 优点 | 缺点 |
|---|---|---|
| **标准 CTS (0x1805)** | nRF Connect / 小米 Mi Band 等工具直接识别；跨厂商互通 | 数据格式固定（10 字节），不能加自定义字段 |
| **自定义 128-bit UUID** | 格式随意 | 对方工具必须预先知道格式 |

选了 **标准 CTS (0x1805 / 0x2A2B)**。时间数据本身就是通用需求，用标准协议既可节省协议设计成本，又能让任何 BLE 调试工具"开箱即用"。

### 5.3 数据结构设计

BLE Current Time 的 10 字节固定格式（规范中定义）：

```c
typedef struct {
    uint16_t year;        // 1582-9999
    uint8_t  month;       // 1-12
    uint8_t  day;         // 1-31
    uint8_t  hour;        // 0-23
    uint8_t  minute;      // 0-59
    uint8_t  second;      // 0-59
    uint8_t  day_of_week; // 1=周一, 7=周日, 0=未知
    uint8_t  fractions256;// 1/256 秒精度
    uint8_t  adjust_reason; // 位掩码（时区变更等）
} __attribute__((packed)) ble_cts_current_time_t;
```

这里学到一点：**BLE 规范里的"周几"编码和 POSIX 不一样**。POSIX `struct tm` 的 `tm_wday` 是 `0=周日..6=周六`；BLE CTS 是 `1=周一..7=周日, 0=未知`。转换：

```c
cts_time->day_of_week = (timeinfo.tm_wday == 0) ? 7 : timeinfo.tm_wday;
```

### 5.4 GATT 回调实现

一个特征值（0x2A2B）同时支持 READ + WRITE，通过 `ctxt->op` 分发：

```c
static int current_time_access_cb(uint16_t conn_handle, uint16_t attr_handle,
                                  struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    switch (ctxt->op) {
    case BLE_GATT_ACCESS_OP_READ_CHR:
        // system_time → CTS 格式 → os_mbuf_append
    case BLE_GATT_ACCESS_OP_WRITE_CHR:
        // 长度校验 → ble_hs_mbuf_to_flat → CTS 格式 → settimeofday
    }
}
```

关键 API 踩坑：

| API | 作用 | 踩坑点 |
|---|---|---|
| `os_mbuf_append(ctxt->om, data, len)` | 把要返回的数据追加到响应缓冲区 | 忘记调用就返回空数据 |
| `ble_hs_mbuf_to_flat(ctxt->om, buf, len, NULL)` | 把接收的 mbuf 展开成连续内存 | 最后一个参数是"实际读取的字节数"，传 NULL 表示不关心 |
| `OS_MBUF_PKTLEN(ctxt->om)` | 获取收到的字节数 | 必须先校验长度再解析，否则可能越界 |

### 5.5 验证流程

**ESP32 端日志：**
```
I (47284) time_service: Current Time read request
I (47285) time_service: Current Time sent: 2026-04-17 21:45:32
I (52103) time_service: Current Time write request
I (52105) time_service: System time updated: 2026-04-18 10:00:00
```

**PC 端工具：** 新写了一个 `tools/ble_time_sync.py`（Tkinter GUI），集成 bleak：

- 扫描 → 连接 → 读/写时间
- 本地时间一键同步到 ESP32
- GUI 三个按钮：连接 / 读取 / 同步

测试通过。手机上用 nRF Connect 也能直接读写（证明标准协议的选择是对的）。

### 5.6 阶段教训

- **BLE 规范里的字段语义一定要看原文**，不能假设和 POSIX 一致。day_of_week 差一就是典型例子
- **mbuf 是 NimBLE 的内部数据结构**，不要直接用 `memcpy`，要用它提供的 API
- **写特征值时务必校验长度**，客户端可能发错误长度的数据

---

## 阶段六：UI 风格统一

### 6.1 背景

之前 time 页面用的是蓝色卡片（`#2C3E50` 深蓝 + `#3498DB` 亮蓝），有点 Bootstrap 2015 风。Menu 页只是一个空壳。About 页根本不存在。三个页面风格各异，而且视觉重心偏重。

目标：**一套统一的现代设计语言覆盖所有页面**。

### 6.2 新配色方案（深紫 + 青绿）

参考 Tailwind CSS 色板，敲定：

```c
#define COLOR_BG         0x1E1B2E   /* 深紫背景 */
#define COLOR_CARD       0x2D2640   /* 卡片背景 */
#define COLOR_CARD_ALT   0x3A3354   /* 分割线 / 次级容器 */
#define COLOR_ACCENT     0x06B6D4   /* 青绿主色 - 按钮、图标、重点文字 */
#define COLOR_TEXT       0xF1ECFF   /* 主要文字 */
#define COLOR_MUTED      0x9B94B5   /* 辅助文字 */
```

**为什么是这个搭配：**
- 深紫背景比纯黑柔和，低光环境对眼睛友好
- 青绿 `#06B6D4`（Tailwind `cyan-500`）在深色背景上对比度高，可读性强
- 整体氛围偏科技感，适配 BLE / 智能设备主题

### 6.3 三个页面重构

**`page_time`**：
- 圆形按钮替代原本的方形大按钮（`LV_RADIUS_CIRCLE`）
- 按压态背景色切换到青绿（`style_btn_pressed`）
- 日期栏加入英文星期缩写 `Mon / Tue / ...`
- 顶部角标放"菜单按钮"（`LV_SYMBOL_LIST`），点击跳转到 Menu

**`page_menu`** — 从"空壳返回按钮"升级成**真实的设置菜单**：

```
┌────────────────────────┐
│  ← Menu                │
├────────────────────────┤
│ 🔵 Bluetooth    Off   │  ← 状态灯显示连接状态
│ 👁 Backlight   50%    │  ← 点击切换 4 档
│ 🖼 Weather      >     │  ← 跳转（当时还没实现）
│ 📋 About        >     │
│ 🏠 Back to Clock      │
└────────────────────────┘
```

- **Bluetooth 状态行**：定时调用 `ble_driver_is_connected()` 刷新（这就是后面线程安全改造要处理的跨线程读取点）
- **Backlight 行**：`BACKLIGHT_STEPS[] = {64, 128, 192, 255}`，点一下循环下一档

**`page_about`** — 全新页面：
- 上方一个圆形蓝牙图标 badge（青绿描边）
- 中间大字标题 + 副标题
- 下方信息表格（Version / Device / Framework / GUI）

### 6.4 复用的 UI 惯例

重构时沉淀出一套"组件级"的函数命名约定，三个页面都遵循：

```c
static void init_styles(void);         /* 样式定义 */
static void create_xxx_card(void);     /* 布局创建 */
static void on_xxx_clicked(lv_event_t*); /* 事件回调 */
static void bind_events(void);         /* 统一挂载事件 */
static void update_display(void);      /* 周期性刷新 */
```

这正是上一章记录的 **"HTML/CSS/JS 分离"** 思路的延续，而且跨页面统一。写第三个页面时完全不用重新思考结构。

### 6.5 LVGL 小坑复盘

- **`lv_obj_remove_style_all()`** 在创建按钮时很关键，否则 LVGL 会带上默认样式（灰色背景、外框等），自定义 style 不一定能覆盖
- **`lv_pct(100)`** 百分比宽度要配合 flex 容器才稳定
- **图标用 `LV_SYMBOL_*` 宏**：本质是 FontAwesome 字体中的 Unicode 字符。LVGL 9.5 默认 Montserrat 字体内嵌了常用符号，但不是全部 — 比如没有温度计或云朵图标，所以天气页的图标后来只能用文字+颜色替代

---

## 阶段七：线程安全改造（time_manager）

### 7.1 问题发现：隐藏的竞态

做完时间服务后，盯着代码发现一个细思极恐的点：

```
UI 线程 (ui_task):
    page_time 的 +/- 按钮 → adjust_time() → settimeofday()

BLE 线程 (NimBLE host task):
    CTS write 回调 → cts_to_system_time() → settimeofday()
```

**两个不同的线程都会写 `settimeofday`，都会读 `gettimeofday`。**

第一反应："`settimeofday` 内核有锁吧？应该安全。"

但仔细推一下，UI 按钮改时间的实际步骤：

```c
time(&now);                // 1. 读
localtime_r(&now, &tm);
tm.tm_hour += 1;           // 2. 算
settimeofday(&tv, NULL);   // 3. 写
```

**内核只保护步骤 3 的原子性，步骤 1 和 3 中间是"裂缝"。**

如果这个瞬间 BLE 收到一次时间推送：
- UI 已经读到了 `12:00:00`（在寄存器里）
- BLE 跳进来把时间写成 `15:30:00`
- UI 继续执行：基于陈旧的 `12:00:00` 加 1 小时 = `13:00:00`，写回去
- 最终：系统时间是 `13:00:00`，BLE 推送的 `15:30:00` **被覆盖丢失**

这是经典的 **丢失更新（Lost Update）** 模式。

### 7.2 方案对比

和设计讨论后罗列了三个候选：

| 方案 | 是什么 | 优点 | 缺点 |
|---|---|---|---|
| **A. Mutex** | 在 `gettimeofday + settimeofday` 外包一个互斥锁 | 改动最小 | BLE 回调里 take lock 有风险 — 如果锁被占住，协议栈会卡住 |
| **B. 原子化封装** | 写一个 `adjust_time_atomic()` 函数让它不被打断 | 看似简单 | 要禁用调度器或关中断，属于破坏系统的脏技巧 |
| **C. 消息队列 + 单 Writer** | 所有时间修改请求投到队列，UI 线程串行处理 | BLE 回调立即返回；写入点收拢到一个线程；扩展性好 | 多一层抽象 |

选 **C**。关键理由不是"最快"，而是：

> BLE 协议栈的回调线程**不能阻塞**（否则协议超时）。Mutex 在多写场景下总有一方要等，等在 BLE 线程里是灾难。而消息队列的 `xQueueSend(..., 0)` 可以做到 **"立即返回，永不阻塞"**。

### 7.3 实现：`services/time_manager.c`

三个函数的接口：

```c
esp_err_t time_manager_init(void);                          /* 启动时一次 */
esp_err_t time_manager_request_set_time(const struct timeval*); /* 任意线程 */
void      time_manager_process_pending(void);                /* 仅 UI 线程 */
```

内部：一个深度 4 的 FreeRTOS `QueueHandle_t`，消息体就是 `struct timeval`。

`request` 端：

```c
esp_err_t time_manager_request_set_time(const struct timeval *tv)
{
    if (xQueueSend(s_queue, tv, 0) != pdTRUE) {
        ESP_LOGW(TAG, "Queue full, set time request dropped");
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
```

`process` 端（在 UI 线程调用）：

```c
void time_manager_process_pending(void)
{
    struct timeval tv;
    while (xQueueReceive(s_queue, &tv, 0) == pdTRUE) {
        settimeofday(&tv, NULL);
    }
}
```

### 7.4 改动清单

- **新增**：`services/time_manager.c/.h`（~70 行）
- **修改**：`services/time_service.c` — `cts_to_system_time` 把 `settimeofday` 替换成 `time_manager_request_set_time`
- **修改**：`app/app_main.c` — `ui_task` 循环里加 `time_manager_process_pending()`
- **修改**：`main/main.c` — 初始化顺序 `init_default_time → time_manager_init → ble_driver_init → app_main_init`（time_manager 必须在 BLE 之前就绪）
- **保持不变**：`app/pages/page_time.c` — UI 按钮仍然直接调 `settimeofday`（本来就单线程，不需要绕弯）

### 7.5 附带修复：`s_is_connected` volatile

同一次改动里发现 Menu 页面读的 `ble_driver_is_connected()` 其实也有跨线程问题 — BLE 线程在 `gap_event_handler` 里写 `s_is_connected = true/false`，UI 线程每 10ms 读一次。

`bool` 单字节在 xtensa 上读写天然原子（`l8ui`/`s8i` 是单指令），所以不会撕裂。但**没有 `volatile` 修饰，编译器可能把它缓存在寄存器里**，导致 UI 永远读不到新值。

一行修复：

```c
static volatile bool s_is_connected = false;
```

### 7.6 阶段教训

- **"内核安全"只保护它看得见的那一瞬间**。跨越多个系统调用的一致性要自己保护
- **"丢失更新"只有在 RMW 路径里才会发生**。如果 UI 只读不写，根本不需要加锁 — 这也是后来天气功能的 UI 侧所采用的模式
- **消息队列不是"更高级的锁"**，它是"所有权转移"的抽象。选择它的依据是"数据从 A 发给 B 的一次投递"语义，而不是"比锁好"
- **单点写入（single writer）是并发设计的顶级策略**。如果能保证"只有一个线程写"，整个并发问题就瓦解了

---

## 阶段八：PC 代理天气推送（全链路新功能）

### 8.1 想法起源

用户最初的分支名 `feat/network_init` 暗示想加 WiFi + SNTP。但讨论后换了思路：

> **"让 PC 去查天气，通过 BLE 推给 ESP32。"**

**好处：**
- ESP32 不需要 WiFi，省内存省功耗
- BLE / WiFi coexistence 本来就麻烦，能规避就规避
- 和已有的 BLE 生态融合，时间服务 + 天气服务都用一条连接

**代价：**
- 必须有 PC / 手机作为代理（这个项目的定位是桌面摆件，可接受）

### 8.2 架构：8 站数据链路

一条天气数据从"天上"到屏幕上的完整旅程：

```
☁ 气象卫星
  ↓
🌐 open-meteo.com（公开 API，免 Key）
  ↓ HTTPS GET
💻 PC 端 Tkinter 工具
  ↓ struct.pack 二进制打包
  ↓ BLE GATT write
📡 ESP32 蓝牙天线
  ↓ NimBLE 协议栈
🧵 BLE 回调线程
  ↓ weather_manager_push → Queue
📬 FreeRTOS 队列
  ↓ UI 线程取出
  ↓ 更新 s_latest 快照
🎨 page_weather.update() 读快照 → LVGL
  ↓
📺 屏幕显示
```

每一站都在解决一个具体问题。

### 8.3 协议设计

**载荷结构（68 字节 packed struct）：**

```c
typedef struct {
    int16_t  temp_c_x10;       // 当前温度 ×10
    int16_t  temp_min_x10;     // 今日最低 ×10
    int16_t  temp_max_x10;     // 今日最高 ×10
    uint8_t  humidity;         // 湿度 %
    uint8_t  weather_code;     // 0..7 映射表
    uint32_t updated_at;       // Unix timestamp
    char     city[24];
    char     description[32];
} __attribute__((packed)) weather_payload_t;
```

Python 对应：`struct.pack("<hhhBBI24s32s", ...)` 严格对齐 68 字节。

**几个关键设计决策：**

| 决策 | 原因 |
|---|---|
| **温度 ×10 存整数** | BLE 不想带浮点序列化；`23.5°C` 存 `235`，在 ESP32 用 `/10` 和 `%10` 拆成整数+小数显示 |
| **天气编码压成 8 种** | Open-Meteo 用 WMO 代码，有几十种；我们屏幕上只显示几种大类，固件端做不了细分 |
| **`__attribute__((packed))`** | 禁止编译器加内存对齐 padding，否则 PC 端 68 字节对不上 ESP32 端 72 字节 |
| **自定义 128-bit UUID** | 天气不是标准服务，随机生成的 `8a5c0001-...` 保证全球不撞车 |

**UUID：**
- Service: `8a5c0001-0000-4aef-b87e-4fa1e0c7e0f6`
- Characteristic: `8a5c0002-0000-4aef-b87e-4fa1e0c7e0f6` (WRITE)

NimBLE 宏用 little-endian 字节序表示：

```c
static const ble_uuid128_t s_weather_svc_uuid = BLE_UUID128_INIT(
    0xf6, 0xe0, 0xc7, 0xe0, 0xa1, 0x4f, 0x7e, 0xb8,
    0xef, 0x4a, 0x00, 0x00, 0x01, 0x00, 0x5c, 0x8a
);
```

### 8.4 模块划分

```
services/
  weather_manager.c/.h       ← 队列 + 本地快照（与 time_manager 同构）
  weather_service.c/.h       ← BLE GATT 自定义服务

app/pages/
  page_weather.c/.h          ← 天气展示页

tools/
  ble_time_sync.py           ← 合并后的 PC 端 GUI（时间 + 天气）
```

**`weather_manager` 和 `time_manager` 结构对称**，只是语义不同：

| | time_manager | weather_manager |
|---|---|---|
| 消息体 | `struct timeval` (16B) | `weather_payload_t` (68B) |
| 队列深度 | 4 | 2 |
| 处理逻辑 | `settimeofday` | `memcpy` 到快照 |
| 访问方式 | 无 getter | `get_latest()` + `has_data()` |
| 队列满策略 | 丢弃新的（时间敏感） | 丢弃旧的（新数据更有价值） |

### 8.5 page_weather 设计亮点

**布局：** 顶栏（返回 + 城市）→ 天气描述大字 → 温度大字 → 高低温卡片 → 湿度 + 更新时间卡片。

**颜色代码：** 8 个天气对应 8 个十六进制颜色，显著但不刺眼：

```c
case WEATHER_CODE_CLEAR:    return 0xFBBF24; /* 金 */
case WEATHER_CODE_RAIN:     return 0x3B82F6; /* 蓝 */
case WEATHER_CODE_THUNDER:  return 0xF97316; /* 橙 */
...
```

**`updated_at` 去重：** UI 线程每 10ms 问一次 `weather_manager_get_latest()`，但数据 10 分钟才变一次。用 `updated_at`（PC 打包时的时间戳）当作"内容指纹"，相同就跳过 LVGL 重绘：

```c
if (w->updated_at == s_ui.last_updated_at) return;
```

既省 CPU 又避免屏幕闪烁。

### 8.6 PC 端工具的演进

原本写了两个独立脚本：
- `ble_time_sync.py` — Tkinter GUI 负责时间同步
- `weather_client.py` — 命令行守护进程负责天气

测试时用户反馈："每次要启动两个 Python 麻烦"。合并成一个 GUI：

```
┌─ ESP32 BLE Companion ──────────┐
│  ● 已连接                       │
│  [连接 ESP32-S3-DEMO / 断开]    │
├── 时间同步 ───────────────────── │
│  ESP32 时间: 2026-04-18 14:32   │
│  [读取时间]  [同步电脑时间]       │
├── 天气推送 ───────────────────── │
│  位置: Shanghai (31.23, 121.47) │
│  天气: 23.5°C Partly Cloudy...  │
│  [刷新天气]  [推送到 ESP32]      │
│  ☑ 自动推送（每 10 分钟）        │
│  上次推送: 14:32:15             │
└─────────────────────────────────┘
```

**技术栈：**
- `bleak` — 跨平台 BLE 客户端库（Windows / Mac / Linux 都能跑）
- `requests` — HTTP 客户端
- `tkinter` — 内置 GUI（免装）
- `asyncio` + 后台线程 — bleak 是异步 API，Tkinter 是同步的，中间用 `run_coroutine_threadsafe` 桥接

**定位策略：** `ip-api.com` 免费无 Key 反查公网 IP 的地理位置。只在首次点"刷新天气"或开启"自动推送"时调用一次，之后缓存 `self._location` 复用。

**自动推送：** Tkinter 的 `root.after(600000, callback)` 定时器调度，10 分钟一轮。断开 BLE 时自动关闭勾选。

### 8.7 调试过程中的几个细节

- **`LV_SYMBOL_IMAGE` 的选择**：LVGL 自带符号里没有"云"、"太阳"等天气图标。最初想引入 FontAwesome 子集，但用户说"先跑通"，改用文字 + 颜色区分（`"Rainy"` + 蓝色）。简单且不需要动字体资源
- **`°` 字符的显示**：U+00B0 在 Montserrat 字体的 Latin-1 补充段里，默认应该能显示。烧录前担心过显示成方块，实测没问题
- **MTU 协商**：payload 68 字节 + ATT 头 3 字节 = 71，bleak 在 Windows / Linux 默认能协商到 100+，无需手动配置
- **字符串截断**：Python 端 `.encode("utf-8")[:23]` 按字节截断有个隐患 — 中文 UTF-8 多字节字符可能被截成半个字符，导致 C 端显示乱码。短期内由 PC 端保证传入的城市名 / 描述都是纯 ASCII 英文，规避这个问题

### 8.8 阶段教训

- **协议设计最怕"好心加字段"**。如果 `weather_payload_t` 里加一个 `uint8_t` 而忘记在 Python 端同步，偏移全乱 — 所以在固件头文件里特意加了注释"与 Python `struct.pack` 严格对齐"
- **"模式复用"比"代码复用"更有价值**。time_manager 和 weather_manager 没有共享一行代码，但设计模式完全一致。维护者看第二个就秒懂
- **"PC 代理"是低功耗设备的常见范式**。当 IoT 设备没有直接上网能力时，让手机 / PC / 网关帮忙是最优雅的方案。小米手环、Fitbit 都是这个思路

---

## 踩过的坑（精华汇总）

### 坑 1：跨线程时间修改的隐藏竞态

已在阶段七详细记录。核心：`settimeofday` 原子不等于"调整时间"原子。**RMW 三步合一需要自己保护。**

### 坑 2：volatile 不是万能的

volatile 只解决"编译器可能把变量缓存到寄存器"这一种可见性问题。它**不**提供：
- 原子性（对 `int64`、struct、指针赋值仍可能撕裂）
- 内存屏障（多核可见性）
- RMW 完整性（`volatile int x; x++;` 仍可能丢失更新）

适用场景非常窄：**单写 + 多读 + 小标量（bool / uint8 / int32）**。
- ✅ `s_is_connected`：bool，BLE 单写 UI 读
- ❌ 时间戳读写：要上锁或消息队列

### 坑 3：packed struct 的跨语言对齐

任何两个不同语言 / 编译器要用二进制通信，**三个东西必须对齐**：

1. **字节序**（endianness）— Python `<` 指定 little-endian，对上 xtensa 天然 little-endian
2. **字段大小**（type width）— `int16_t` ↔ `h`，`uint32_t` ↔ `I`
3. **内存布局**（padding）— C 加 `__attribute__((packed))`，Python 的 struct 自带紧凑布局

三个里任一错位，整个结构解析全乱。**宁可把 `sizeof` 静态断言写在代码里**：

```python
PAYLOAD_SIZE = struct.calcsize(PAYLOAD_STRUCT)
assert PAYLOAD_SIZE == 68, f"payload size mismatch: {PAYLOAD_SIZE}"
```

### 坑 4：BLE 回调不能阻塞

NimBLE 的 GATT 访问回调 `access_cb` 跑在 host 线程里。在里面做重活（长时 I/O、take mutex）会导致：
- 协议栈心跳丢失 → 连接断开
- 后续 GATT 请求排队延迟 → 主机端超时

正确姿势：**回调里只做校验 + 投递到队列**，真正的业务逻辑交给别的线程。

### 坑 5：LVGL 不是线程安全的

整个 LVGL 对象树、样式、定时器**只能在同一个线程**操作。如果 BLE 回调里直接 `lv_label_set_text`，大概率在某次并发访问时崩在 `lv_array_push_back` 或 `lv_draw_sw_*` 里（回忆上一章的看门狗 Bug）。

消息队列 + UI 线程单 writer 模式**顺便解决了这个问题** — BLE 线程永远不碰 LVGL，所有 UI 更新都在 UI 线程里做。

---

## 关键经验总结

### 1. 数据流的"单向性"设计

本阶段两个功能的设计模式高度一致：

```
外部数据源 → 投递（non-blocking）→ 队列 → UI 线程消费 → 本地快照 → LVGL
```

这条链路的关键性质：

- **单向**：数据只往 UI 线程流，不会反向
- **单写**：UI 线程是唯一写快照的；写快照的不用锁
- **去耦**：数据源（BLE）和消费者（LVGL）在不同线程，一方慢不影响另一方

如果后续要加**传感器数据上报**（温湿度、光感），或者**PC 推送亮度配置**，都能照抄这个模式。

### 2. "单线程所有权"大幅简化并发

UI 线程拥有 LVGL 对象、拥有 `s_latest` 快照、拥有所有 UI 状态。BLE 线程只做两件事：
1. 收数据
2. 投递

写 `weather_payload_t` 的只有 UI 线程（从队列复制到快照），读快照的也只有 UI 线程。**完全没有共享可变状态，也就不需要任何锁。**

### 3. 标准协议 vs 自定义协议的选择

- **标准服务（时间 CTS、电池、DIS）**：对方工具开箱即用，设备间互通
- **自定义服务（天气、配置、命令）**：格式自由，但需要客户端代码配合

两者都有位置。**能用标准就不自定义**，这是省事之道。但当字段语义真的超出标准范围，果断用 128-bit UUID 自定义。

### 4. 协议兼容性靠"契约文档"

C 端的 `weather_payload_t` 和 Python 端的 `PAYLOAD_STRUCT` 是**同一个契约的两种表达**。改动一方必须同时改另一方。

实践做法：
- 在 C 头文件里写明"与 Python `struct.pack` 严格对齐"
- 在 Python 脚本里 assert 大小，链接到 C 端的 struct 定义
- 字段注释都用相同顺序

这些不是为了好看，是**减少未来维护时"只改了一边"的事故概率**。

### 5. Tkinter + asyncio 的桥接模式

`bleak` 是纯异步 API，`tkinter` 是纯同步 GUI。把它们黏起来的范式：

```python
# 后台 thread 运行一个 asyncio event loop
self._loop = asyncio.new_event_loop()
threading.Thread(target=lambda: self._loop.run_forever(), daemon=True).start()

# Tkinter 按钮点击时，把协程提交到后台 loop
future = asyncio.run_coroutine_threadsafe(self._ble.connect(), self._loop)

# 协程完成后用 root.after(0, ...) 回到 Tkinter 主线程执行 UI 更新
future.add_done_callback(lambda f: self.root.after(0, lambda: update_ui(f.result())))
```

这套模板可以直接复制到任何"异步 I/O + Tkinter GUI"项目。

---

## 项目当前状态

### 功能清单

| 模块 | 功能 | 状态 |
|---|---|---|
| **UI 框架** | 三页面路由（time / menu / about / weather） | ✅ |
| **UI 风格** | 深紫 + 青绿统一配色 | ✅ |
| **BLE 连接** | 广播、连接、断开、自动重连 | ✅ |
| **BLE 时间服务** | 标准 CTS，读写系统时间 | ✅ |
| **BLE 天气服务** | 自定义 GATT，接收天气数据 | ✅ |
| **线程安全** | time_manager / weather_manager 消息队列 | ✅ |
| **背光控制** | 4 档切换 | ✅ |
| **PC 端工具** | Tkinter GUI，整合时间 + 天气 | ✅ |

### 项目目录结构

```
demo6/
├── app/                         # 应用层
│   ├── app_main.c              # 应用初始化 + UI 任务循环
│   ├── app_main.h
│   └── pages/                  # 页面实现
│       ├── page_time.c/h       # 时间调节页
│       ├── page_menu.c/h       # 菜单页
│       ├── page_about.c/h      # 关于页
│       └── page_weather.c/h    # 天气页 (新增)
│
├── framework/                   # 框架层
│   └── page_router.c/h         # 页面路由
│
├── drivers/                     # 驱动层
│   ├── lcd_panel.c/h
│   ├── touch_ft5x06.c/h
│   ├── lvgl_port.c/h
│   └── ble_driver.c/h          # 增加 volatile 修饰
│
├── services/                    # 服务层
│   ├── time_service.c/h        # BLE CTS (阶段五)
│   ├── time_manager.c/h        # 时间消息队列 (阶段七)
│   ├── weather_service.c/h     # BLE 天气服务 (阶段八)
│   └── weather_manager.c/h     # 天气消息队列 (阶段八)
│
├── main/
│   └── main.c                  # 入口，初始化顺序严格受控
│
├── tools/
│   ├── ble_time_sync.py        # PC 端 GUI，整合了时间 + 天气
│   └── requirements.txt        # bleak, requests
│
└── docs/
    ├── 开发日志.md              # 上一章（UI + 路由）
    └── BLE集成开发日志.md        # 本日志
```

### 启动流程

```
app_main()
  ├─ init_default_time()      # 兜底系统时间
  ├─ time_manager_init()      # 队列先就绪
  ├─ weather_manager_init()   # 队列先就绪
  ├─ ble_driver_init()        # BLE 启动（内部调 time_service_init + weather_service_init）
  └─ app_main_init()          # LVGL + 路由 + UI 任务
       └─ ui_task
            循环 {
              time_manager_process_pending()    ← 消费时间请求
              weather_manager_process_pending() ← 消费天气请求
              page_router_update()              ← 当前页面的 update 回调
              lvgl_port_task_handler()          ← LVGL 渲染
              vTaskDelay(10ms)
            }
```

---

## 后续改进方向

### 短期（下一轮迭代）

- **中文字体支持**：目前 city / description 只能用英文，若要显示"上海 / 多云"需要接入 LVGL 中文字体子集（~100-200KB flash）
- **天气图标**：用 FontAwesome 子集 或 Canvas 手绘图标替代文字。需要评估字体 flash 占用
- **屏幕超时息屏**：30 秒无触摸关背光、触摸唤醒。`lcd_panel_set_backlight(0)` 已经有
- **NVS 持久化**：背光设置、最近一次天气数据重启后保留

### 中期

- **BLE OTA**：通过 BLE 推送新固件（`esp_https_ota` + 自定义传输通道）
- **温湿度传感器**：接个 I2C 传感器（AHT20 / BME280），建立"感知 → 上报 → 展示"完整闭环
- **BLE 电池服务 (0x180F) + 设备信息服务 (0x180A)**：让 About 页信息更完整

### 长期

- **WiFi + MQTT**（可选）：如果真要离线运行，加 WiFi 直连云端（而不是 PC 代理）
- **事件总线**：消息队列模式已经铺开，可以抽象一层通用的 `event_bus`，让所有跨线程消息走统一通道
- **主题切换**：浅色 / 深色两套配色运行时切换

---

## 写在最后

这一章记录的是"从连得上到用起来"的过程。四个阶段每一步都在**把设备变得更有用，同时把代码变得更结构化**。

最满意的设计：

> **time_manager 和 weather_manager 这两个模块。**

不是因为代码漂亮，而是因为它们**解决了两种看似不同、本质完全相同的问题**（时间同步 / 天气推送），用的是**同一套机制**（队列 + 单 writer）。当第三个类似需求出现时，照抄结构就行 — 这是"设计模式"真正的价值，不是为了学术味的工整，是为了把大脑从重复问题里解放出来。

回头看上一章结尾的话："这次 BLE 集成任务暴露了 ESP-IDF 项目开发的复杂性"。一天过去，这句话依然成立：配置文件、依赖声明、内存模型、线程模型，每一项都需要谨慎对待。但这些复杂性**是可驾驭的** — 靠的是清晰的分层、谨慎的契约，以及每完成一步就立刻写下"我为什么这样做"。

> 这份日志就是那个"立刻写下"。

---

*续篇生成时间：2026-04-18*
*项目版本：v0.4*
*分支：feat/network_init*（虽然最后没走网络，而是走了 BLE 代理，分支名没改）
