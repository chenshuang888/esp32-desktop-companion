# 五大原生 App UI 升级（音乐 + 系统 + 设置 + 时钟）+ 桌面伴侣音乐管理页 工作日志

**日期**：2026-05-02
**分支**：feat/optimize_page
**作者**：ChenShuang + Claude

---

## 0. 起因

通知 App 重构落定后，按计划继续优化剩余原生 app 的 UI。本轮一口气完成了 4 个 app 的重构 + 1 个 app 的全新构建，外加桌面伴侣（companion）端的音乐管理页 + archive.org 在线下载。

涉及 app：
1. **音乐 App**：拆双页（歌单 + 详情），实现"PC 端推歌单、手表端点歌、PC 用 just_playback 本地播放"全链路
2. **系统 App**：双 tab（电脑 / 设备），首次显示 ESP32 端运行情况（PSRAM/SRAM/温度/任务数/uptime/BLE/LittleFS/复位原因/固件版本）
3. **设置 App**：从 3 项扩到 5 子页（蓝牙 / 显示与亮度 / 时间调节 / 关于），仿 iOS 设置 app 的分组列表
4. **时钟 App（全新）**：4 tab + 1 编辑子页（闹钟 / 世界时钟 / 秒表 / 计时器 / 添加闹钟），仿华为时钟 app 布局
5. **桌面伴侣 GUI**：新增"音乐"分页（拖拽 / 列表 / 同步 / 在线下载推荐 20 首）

最后一口气改完后，原生 app 数量从 6（lockscreen / launcher / weather / notifications / music / system / settings）扩到 7（+ clock），UI 风格全部统一到 iOS 浅色 + 30px 底部 hit zone + sub_router 双/多 page 架构。

---

## 1. 关键决策

### 1.1 音乐 App：BLE 协议复用 + 本地播放器选型

| 议题 | 决策 | 理由 |
|---|---|---|
| 是否新增 BLE service | **复用 `media_service`，首字节加 type 分发** | 用户要求；同 service 内"触发端与响应端同 service"原则也能成立 |
| 协议扩展 | WRITE/NOTIFY 都加 1 字节 type，4 种 WRITE / 2 种 NOTIFY | NOWPLAYING / PLAYLIST_BEGIN / ITEM / END · BUTTON / PLAY_TRACK |
| 歌单上限 | **50 首**（`MEDIA_PLAYLIST_MAX_ITEMS`） | 屏上滚动够用，RAM 占用约 50×64B=3KB |
| 歌单流式推送 | BEGIN → ITEM × N → END | 单包 MTU 247 内放不下整个歌单，按"开始/条目/结束"分包，pending → live 双缓冲，END 时整体切换 |
| 双缓冲 | pending 装载 + END 时 memcpy 到 live + version+1 | 单 writer 模式（UI 线程消费队列写两个缓冲），符合现有 single-writer 约定 |
| PC 端播放器 | **`just_playback`**（纯 pip，零外部依赖） | 用户明确说要打包给别人用，不能要求装 VLC.exe；just_playback 底层 miniaudio 静态打包进 wheel，PyInstaller 出 exe 即用 |
| Spotify / 网易云 / QQ 音乐 | 全部放弃 | SMTC 拿不到 queue；网易云 API 已归档；爬版权歌不做（明确边界） |
| 圆盘旋转 | `lv_obj_set_style_transform_rotation` + lv_anim 60ms 周期 0.6° | playing=true 时启动，paused 时停 |

### 1.2 系统 App：双 page 拆分 + 圆盘表盘

| 议题 | 决策 | 理由 |
|---|---|---|
| 单页双 view 还是双 page | **改成双 page**（用户要求） | 跟 music/settings 一致用 sub_router，体验统一 |
| ESP32 端数据采集 | 新建 `device_stats`（采集层），`page_system_device` 消费 | 跟 PC 数据 `system_manager` 同一抽象；UI 线程 1Hz tick |
| 温度 sensor | `temperature_sensor_install` 一次，常开 | 1Hz 查询足够，避免频繁触发 |
| LittleFS 查询 | 10s 一次（不是 1Hz） | 挂载层有 cache，但还是不便宜 |
| Tab 栏位置 | **顶部** | 用户明确不放底部（怕误触底部 hit zone） |
| 圆盘控件 | **`lv_arc`**（不画 canvas） | 一对一翻译 conic-gradient mockup，性能好 |
| 左右滑切 tab | 整屏 PRESSED+RELEASED 监听 + 给 children 加 EVENT_BUBBLE | LVGL 9 默认 input 事件不冒泡，child（卡/容器）会吞事件 |

### 1.3 设置 App：4 项分两组

| 议题 | 决策 | 理由 |
|---|---|---|
| 功能数量 | 4 项（蓝牙 / 显示与亮度 / 时间调节 / 关于） | 用户参考手机设置，砍掉所有花哨选项 |
| 分组方式 | **2 组**：蓝牙独立 / 其余 3 项合并 | 仿 iOS 设置 app，首功能 highlight |
| 屏息时长 | **纯 UI 占位**（不接逻辑） | 用户决策——"实现细节先放着" |
| 蓝牙断开按钮 | **纯 UI 占位**（不接逻辑） | 同上；ble_driver 也未暴露 disconnect API |
| 行图标 | **28×28 彩色圆角方块** + 白色 Material 字体图标 | 仿 iOS 风格，比单字符彩色图标更"高级" |
| 亮度 slider | 拖拽实时改背光，松手时落 NVS | 避免拖拽过程频繁写 NVS |

### 1.4 时钟 App（全新）

| 议题 | 决策 | 理由 |
|---|---|---|
| 整体布局 | **顶部 4 tab + 30px 底部 hit zone**，仿华为时钟 | 用户参考 |
| 4 个子页 | 闹钟 / 世界 / 秒表 / 计时器 + 添加闹钟（push 进来）| 时钟 app 标准 |
| Tab 切换方式 | 点击 + 整屏左右滑（复用 system app 模式） | 体验统一 |
| 状态保留策略 | **退出 page 即清零**（用户指定） | 简化，避免后台跑 |
| 秒表精度 | **0.01 秒（centisecond）** | 用户指定 |
| 闹钟到时触发 | **不实现，UI 占位** | 用户指定"先 UI" |
| 计时器到 0 | 仅显示"结束"，不响铃 | 同上 |
| 添加闹钟页 | **push 子页**（不在 tab 序列里） | 区分"主功能 tab"和"二级页面" |

### 1.5 桌面伴侣 GUI

| 议题 | 决策 | 理由 |
|---|---|---|
| 是否做"音乐"分页 | 做 | 让用户不用关心文件路径 |
| 拖拽 | 不做（要装 tkinterdnd2）| 用户改用"添加文件"按钮即可 |
| "在线下载" 数据源 | **archive.org**（CC0 / 公共领域） | 完全合法可商用；网易云/QQ 等版权源全部放弃 |
| 一键推荐 20 首 | DEFAULT_PICKS 5 类 × 4 首（钢琴/爵士/吉他/环境音/古典） | 仅英文器乐，不爬中文流行歌 |
| 中文流行歌 | **不爬**（边界明确） | 让用户自己用方案 1（添加文件）从已有 mp3 库拖入 |

---

## 2. 落地清单

### 2.1 BLE 协议扩展

**`services/media_service.h/c`** —— v2 协议（type 分发）：

```c
/* WRITE: type tag */
#define MEDIA_MSG_NOWPLAYING       0x01     // 旧 media_payload_t (92B)
#define MEDIA_MSG_PLAYLIST_BEGIN   0x02     // total + version (4B)
#define MEDIA_MSG_PLAYLIST_ITEM    0x03     // index + title[40] + artist[24] (66B)
#define MEDIA_MSG_PLAYLIST_END     0x04     // 无 body

/* NOTIFY: type tag */
#define MEDIA_NOTIFY_BUTTON        0x01     // 旧 button (4B)
#define MEDIA_NOTIFY_PLAY_TRACK    0x02     // track_index + seq (4B)
```

`media_access_cb` 改成"读首字节 → switch 分发"，新增 `media_service_send_play_track(uint16_t)`。

**`services/manager/playlist_manager.h/c`** —— 新增双缓冲管理器：
- 队列深 16，BLE 线程入队（PLEV_BEGIN / PLEV_ITEM / PLEV_END）
- UI 线程 `process_pending` 消费队列，写 pending；END 时 memcpy 到 live + version+1
- `get_count() / get_track_at() / version() / has_data()` 给 UI 读

### 2.2 音乐 App（双页）

**新建** `app/apps/music/pages/`：
- `page_music_list.{h,c}` —— 歌单页：状态栏 + 标题 + 列表（44px/项）+ 50px 常驻 mini-player + 30px 底部 hit zone
- `page_music_detail.{h,c}` —— 详情页：160×160 旋转黑胶圆盘（紫底+灰内圈+中心钉孔）+ 标题/作者 + 进度条 + 三按钮 + 30px hit zone

**改造** `music_app.c` 改 sub_router 双 page，公开 `music_app_push/pop_or_exit`。

**关键修复（迭代过程中暴露的）**：
1. **列表滚动回弹** —— companion 1Hz 推 NOWPLAYING 即便内容空也 version++，原代码每秒 rebuild_list → 滚动位置丢。改成只有"当前播放歌的 title 变化"才 rebuild。
2. **歌单截断匹配** —— 协议 title 字段 40B，NOWPLAYING title 48B。长曲名第 13/14 首高亮失败。改 `find_playing_index` 用"歌单截断版作为前缀去匹配 nowplaying"，长名也能命中。
3. **mini-player 上滑误触** —— 把 mini-player 上滑当退出 hit 源 → 在按钮上滑也会退出。最终方案：**屏幕最底 30px 留空白纯 hit zone，按钮和 mini-player 全部上移**（参考通知 app + iOS home indicator）。
4. **详情页退出失败** —— 24×80 hit zone 太窄按不到。同样改成屏底 30px 纯 hit zone + 按钮上移。

### 2.3 系统 App（双 page + 圆盘表盘）

**新建** `app/apps/system/pages/`：
- `system_ui_common.{c,h}` —— 公共组件：`sys_make_gauge`（lv_arc 圆盘）、`sys_make_info_card` + `sys_make_kv_row`、`sys_make_tabbar`（顶部 28px 双 tab + 自动 underline）、`sys_attach_hit_and_swipe`（30px 底部 hit zone + 整屏左右滑切 tab + 给 children 加 EVENT_BUBBLE）
- `page_system_pc.c` —— PC tab：3 圆盘 (CPU/MEM/DISK) + 卡 1 (电池/温度) + 卡 2 (网速/运行)
- `page_system_device.c` —— Device tab：3 圆盘 (PSRAM/SRAM/温度) + 卡 1 (运行/任务/BLE) + 卡 2 (存储/复位/固件)

**新建** `services/manager/device_stats.{h,c}` —— ESP32 端运行状态采集：
- `device_stats_init()`：装芯片温度 sensor（`temperature_sensor_install`），记录 PSRAM/SRAM 总量、复位原因、固件版本（`esp_app_get_description`）
- `device_stats_tick()`：1Hz 采集 free_heap / 任务数 / uptime / BLE / 温度（1Hz）/ littlefs（10s 一次）
- `app_main.c` 在 ui_task 里 `if ((loop_cnt++ % 100) == 0) device_stats_tick()`

**事件冒泡修复** —— 整屏左右滑切 tab 失效：LVGL 9 默认 input 事件不冒泡，给 screen 所有 child + grandchild 加 `LV_OBJ_FLAG_EVENT_BUBBLE`。

### 2.4 设置 App（5 子页）

**新建** `app/apps/settings/pages/`：
- `settings_bluetooth.{h,c}` —— 56px 圆形大图标 hero（连接=蓝/未连接=灰）+ 信息卡（设备名/MAC，从 `esp_read_mac(ESP_MAC_BT)`）+ 红色"断开连接"按钮（不接逻辑）
- `settings_display.{h,c}` —— 亮度 slider（拖拽实时改背光，松手落 NVS）+ 屏息时长 5 段 segmented control（仅 UI 占位）

**重写**：
- `settings_home.c` —— 4 行 2 组（蓝牙独立 / 显示+时间+关于）。彩色圆角图标方块，蓝牙行右侧实时显示连接状态。30px hit zone 退出
- `settings_time.c` —— 切浅色（去深紫），加秒位 stepper，新增"X 年 X 月 X 日 · 周X"显示，去 LVGL gesture
- `settings_about.c` —— 紫色 logo hero + 编译日期 + 真实芯片信息卡（`esp_chip_info` / `IDF_VER` / `esp_flash_get_size` / `heap_caps_get_total_size(MALLOC_CAP_SPIRAM)`）

### 2.5 时钟 App（全新）

**新建** `app/apps/clock/`：
- `clock_app.{h,c}` —— sub_router 5 page（4 tab + 1 编辑子页）。tab 切换用 replace（不入栈），编辑子页用 push（可 pop 回）
- `pages/clock_ui_common.{c,h}` —— 顶部 4-tab 栏（4 等分 60px / 个）+ 30px 底部 hit zone + 整屏左右滑切 tab（复用 system app 模式）
- `pages/page_alarms.{c,h}` —— 闹钟列表（3 假数据）+ 卡片右侧自绘 36×20 胶囊开关 + 右上 28px 圆形 FAB 进添加
- `pages/page_world.{c,h}` —— 世界时钟（北京/东京/伦敦/纽约，基于 `localtime_r` + 时差计算，每分钟刷新）
- `pages/page_stopwatch.{c,h}` —— 秒表：180×180 圆框 + 大数字 mm:ss + 小数字 .cc + IDLE/RUNNING/PAUSED 状态机（`esp_timer_get_time` 微秒）
- `pages/page_timer.{c,h}` —— 计时器：lv_arc 进度环（剩余比例）+ 5 预设 chip + IDLE/RUN/PAUSE/DONE 状态机
- `pages/page_alarm_edit.{c,h}` —— 添加闹钟：顶部 < 返回 + 大数字 hh:mm + 4 个 stepper 按钮 + 取消/保存

### 2.6 桌面伴侣（companion）

**新建** `tools/companion/shared/archive_org.py` —— archive.org 客户端：
- `search(keyword, limit)` —— 调 advancedsearch.php，过滤 collection=audio_music/opensource_audio/netlabels
- `resolve_mp3(hit)` —— 调 metadata 接口拿文件清单，挑体积合适（>100KB）的 mp3
- `download(hit, dest, progress_cb)` —— 流式下载，64KB chunk，`.part` 临时文件 + rename 原子提交
- `collect_recommendations()` —— 按 `DEFAULT_PICKS`（5 类 × 4 首）一键收集 ~20 首

**新建** `tools/companion/gui/pages/music.py` —— 音乐管理页：
- 文件夹路径栏（选目录 / 打开）
- 添加音乐（多选 mp3/flac/wav/ogg/m4a，自动复制到 Watch 目录）
- 在线下载（弹窗：搜索 + 一键推荐 20 首，进度按钮）
- 列表展示 + 删除（二次确认）
- 推送同步按钮 → emit("media:rescan", {future})

**改造** `tools/companion/providers/media_provider.py`：
- 启动时扫 `~/Music/Watch/` → 加载到 just_playback `Playback`
- 自维护 `current_index` + 用 `is_track_ended()` 检测自动连播下一首（循环）
- 1Hz 轮询播放状态 → 推 NOWPLAYING（替代 SMTC 当主源；本地无歌单时 SMTC 兜底）
- 监听 NOTIFY 8a5c000d，按首字节 type 分流：0x01 → vlc prev/playpause/next；0x02 → `play_index(idx)`
- 监听 bus 事件 `media:rescan` / `media:set_folder` 让 GUI 触发刷新

**核心切换**：`python-vlc` → `just_playback`。just_playback 底层 miniaudio 静态打包进 wheel，PyInstaller 出 exe 即用，零外部软件依赖。

---

## 3. 几次"踩坑 + 修复"

### 3.1 LVGL 9 事件不冒泡
**症状**：system / clock app 整屏左右滑切 tab 失败，screen 收不到 PRESSED/RELEASED。

**原因**：LVGL 9 默认 input 事件**不**自动冒泡到父对象，child（卡片、容器、arc）会"吞掉"事件。

**修法**：`sys_attach_hit_and_swipe` / `clk_attach_hit_and_swipe` 末尾遍历 screen 的 children + grandchildren 加 `LV_OBJ_FLAG_EVENT_BUBBLE`。

### 3.2 -Werror=format-truncation 卡编译
**症状**：`char buf[8]; snprintf(buf, sizeof(buf), "%02d:%02d", ...)` 编译失败。

**原因**：编译器看到 `%d` 上界算到 3 位数，认为 buffer 可能不够（即便实际 5 字符够用）。

**修法**：把所有这种 buffer 统一加大到 16（含中文字符串场景到 64）。已修：`settings_display.c` / `settings_time.c` / `settings_about.c` / `clock pages/* 4 个`。

### 3.3 mini-player 上滑误触按钮
**症状**：在按钮上上滑能退出 app；从屏幕底部上滑会误触播放暂停。

**根因**：把 mini-player 自身（含按钮）当 hit 源 → CLICKED 和 RELEASED 在按钮区不可避免互相干扰。

**最终方案**：**屏底 30px 不放任何点击元素**，纯空白 hit zone（参考手机 home indicator）。mini-player 和按钮全部上移让出。**全 app 统一遵循此规则**。

### 3.4 长歌名匹配失败（前 12 首高亮 / 后 2 首点击会回弹）
**症状**：歌单 14 首，前 12 首正常，后 2 首点击不高亮，且列表跳回第一行。

**根因**：协议 `title[40]` vs nowplaying `title[48]`，长曲名（41/42 字符）在歌单端被截断，`strncmp(40)` 永远不等。

**修法**：`find_playing_index` 改用"歌单截断版作为前缀"匹配 nowplaying（`strncmp(it->title, m->title, strnlen(it->title, 40))`），长名也能命中。

### 3.5 列表滚动每秒回弹
**症状**：滚到第 5 首，1 秒后又跳回第 1 首。

**根因**：companion 每秒推 NOWPLAYING（即便内容空），ESP 端 media_manager 都 version++，UI 看 version 变化就 rebuild_list → 滚动位置丢。

**修法**：UI 只在"当前播放的歌 title 变化"时才 rebuild_list；其他场景（暂停/继续/播放进度变）只更新 mini-player 文本。

---

## 4. 文件改动清单

### 新建（21）
- `services/manager/playlist_manager.{h,c}`
- `services/manager/device_stats.{h,c}`
- `app/apps/music/pages/page_music_list.{h,c}`、`page_music_detail.{h,c}`
- `app/apps/system/pages/system_ui_common.{h,c}`、`page_system_pc.{h,c}`、`page_system_device.{h,c}`
- `app/apps/settings/pages/settings_bluetooth.{h,c}`、`settings_display.{h,c}`
- `app/apps/clock/clock_app.{h,c}`、`pages/clock_ui_common.{h,c}`、`page_alarms.{h,c}`、`page_world.{h,c}`、`page_stopwatch.{h,c}`、`page_timer.{h,c}`、`page_alarm_edit.{h,c}`
- `tools/companion/shared/archive_org.py`
- `tools/companion/gui/pages/music.py`
- `ui_mockups/music/v1.html`、`system/v1.html`、`settings/v2.html`、`clock/v1.html`

### 删除（2）
- `app/apps/music/pages/page_music.{c,h}`（被 list/detail 双页替代）

### 重写（重要）
- `services/media_service.{h,c}` —— v2 协议 type 分发 + send_play_track
- `app/apps/music/music_app.c` —— sub_router 双 page
- `app/apps/system/system_app.c` —— sub_router 双 page + 公开 switch_to/exit
- `app/apps/settings/settings_app.c` —— 注册新增 bluetooth / display 子页
- `app/apps/settings/pages/settings_home.c` —— 4 行 2 组分组卡
- `app/apps/settings/pages/settings_time.c` —— 切浅色 + 加秒位 + 周显示
- `app/apps/settings/pages/settings_about.c` —— logo hero + 真实芯片信息
- `services/manager/media_manager.h` —— include media_service.h 去重定义
- `tools/companion/providers/media_provider.py` —— vlc 替换 just_playback + 接 GUI 事件

### 接线
- `services/CMakeLists.txt` 加 playlist_manager.c / device_stats.c + 依赖
- `app/CMakeLists.txt` 加全部新增源 + include dirs + ESP 依赖（app_update / spi_flash / esp_hw_support）
- `main/main.c` 加 `playlist_manager_init()` / `device_stats_init()`
- `app/app_main.c` ui_task 加 `playlist_manager_process_pending()` + `device_stats_tick()`（1Hz 节流）+ 注册 `CLOCK_APP`
- `tools/companion/__main__.py` `MediaProvider(music_folder=...)` 注入
- `tools/companion/config.py` 加 `music_folder` 默认值
- `tools/companion/gui/app.py` PAGE_DEFS 加"音乐"分页
- `tools/requirements.txt` 加 `just_playback>=0.1.8`

---

## 5. 验证

### 5.1 音乐 App
- [x] 列表显示 PC 推送的歌单
- [x] 点列表项 → companion 日志 `play_track idx=X` → vlc 开播
- [x] mini-player 显示当前播放，常驻底部
- [x] 三按钮（prev/play_pause/next）正常工作
- [x] 点 mini-player 中部 → 进详情页
- [x] 详情页旋转圆盘（playing 时转）
- [x] 进度条按时间推进
- [x] 列表滚动不回弹
- [x] 列表/详情底部上滑退出

### 5.2 系统 App
- [x] PC tab：3 圆盘 + 两张卡，PC 数据准确
- [x] Device tab：PSRAM/SRAM/温度圆盘 + ESP 端真实数据
- [x] 点击 tab + 整屏左右滑切换都好用
- [x] 底部上滑退出

### 5.3 设置 App
- [x] 4 行 2 组列表展示
- [x] 蓝牙行实时显示连接状态
- [x] 蓝牙详情页 hero + 信息卡
- [x] 显示页 slider 拖拽即时改背光，松手 NVS 持久化
- [x] 屏息 segmented 切换（仅 UI）
- [x] 时间页 stepper 调整时分秒年月日，同步系统时间
- [x] 关于页 logo + 真实芯片/Flash/PSRAM/IDF 版本

### 5.4 时钟 App
- [x] 4 tab 切换（点击 + 左右滑）
- [x] 闹钟列表 3 项 + 开关切换 + FAB 进添加页
- [x] 添加闹钟：< 返回 + 大数字预览 + stepper + 取消/保存
- [x] 世界时钟 4 城市基于本地时差自动计算
- [x] 秒表：开始/停止/重置 + 0.01 秒精度 + 状态机正确
- [x] 计时器：5 预设 chip + 进度环 + 暂停/继续/取消
- [x] 退出 page 状态自动清零

### 5.5 桌面伴侣
- [x] 音乐分页：选目录 / 打开 / 添加文件 / 删除 / 推送同步
- [x] 在线下载弹窗：搜索 + 一键推荐 20 首
- [x] archive.org 搜索 + 解析 mp3 直链 + 流式下载
- [x] just_playback 替换 vlc 后所有播放控制功能正常

---

## 6. 不在本次范围（下一轮）

- ❌ 闹钟实际触发逻辑（响铃 / 震动 / 弹窗）
- ❌ 闹钟数据 NVS 持久化
- ❌ 屏息时长功能（采集 idle 时间 → 自动熄屏）
- ❌ 蓝牙断开按钮的实际逻辑
- ❌ 计时器到 0 的提示音 / 弹窗
- ❌ 秒表"计次"功能（当前只 log）
- ❌ 世界时钟"添加城市"功能（FAB 当前只 log）
- ❌ Spotify Web API / 其它在线音乐源
- ❌ 中文流行歌爬虫（边界明确放弃）

---

## 7. 关键经验沉淀

### 7.1 LVGL 9 input 事件不冒泡
凡是想"在 screen 上监听全屏 PRESSED/RELEASED"的页面，**必须** 给所有 child（递归一层够用）加 `LV_OBJ_FLAG_EVENT_BUBBLE`，否则被 child 吞。

### 7.2 屏底 30px 必须留空白
**全 app 统一规则**：屏幕最下面 30px 是纯透明 hit zone，**不放任何点击元素**。仿手机 home indicator。这一条之前在音乐页迭代时反复踩坑（mini-player 6px → 50px → 最终拆成 mini-player + 30px hit zone 上下分开）。

### 7.3 -Werror=format-truncation 一律加大 buffer
含数字格式的 buffer 至少 16 字节，含中文 + 数字至少 64。不要省那几个字节。

### 7.4 sub_router_replace vs push
- **同级 tab 切换** → `replace`（不入栈，避免历史膨胀）
- **下钻子页面** → `push`（可 pop 回）
- 一个 app 同时用两种很正常（system / clock 都是）

### 7.5 BLE 协议升级用 type 分发
不要随便加新 service。同 service 内加 type 字节做协议升级，节省 GATT 表 + 给客户端加适配也容易（首字节 switch）。

### 7.6 桌面端音频选型
打包给别人用 = **不能要求装外部软件**。Python 库选 just_playback / pygame / playsound 这种自带解码的，避免 python-vlc + VLC.exe 这种"两件套"。

---

## 8. 一句话总结

一口气把 4 个原生 app 改造 + 1 个全新 app + 桌面伴侣音乐管理页全跑通，**全 app 统一到"iOS 浅色 + 顶部 tab（如有）+ 内容区 + 屏底 30px 纯 hit zone + sub_router 多 page"** 这一套架构，从此原生 app 总数定格在 7 个（lockscreen / launcher / weather / notifications / music / system / settings / clock），后续 UI 改动都按这个模板。
