# 动态App(MicroQuickJS)_App文件夹布局重构_sys.fs沙箱_FS_Worker抽离_工作日志

## 背景

之前动态 app 的脚本是平铺存储：`/littlefs/apps/<name>.js`，每个 app 只有一个 .js 文件，`sys.app.saveState/loadState` 借 NVS 保存少量状态。

随着 app 越来越多（alarm/calc/timer/2048/echo/weather/music），出现两个需求：
1. **App 自有数据**：alarm 闹钟列表 / 2048 存档需要存在 FS（不是 NVS 那种小 KV）
2. **App 自带元信息**：菜单显示中文名而不是英文文件名（"闹钟" vs "alarm"）

NVS 装不下大点的状态、文件名也不能直接做中文展示。需要把 app 升级到"文件夹形态"，并给 JS 提供文件系统 API。

## 决策

### 1. 文件夹布局

```
/littlefs/apps/<app_id>/
    main.js              入口脚本（固定文件名，不在 manifest 里声明）
    manifest.json        {"id": "...", "name": "..."}
    data/                JS sys.fs.* 沙箱写区
        state.json
        ...
```

- `app_id` = 目录名，唯一标识，限 [a-zA-Z0-9_-] ≤15 字符
- manifest 极简版：MVP 只 `id` 和 `name`，缺失就回退到 app_id
- 入口固定 `main.js`：少一层"读 manifest → 解析 entry → 防字段缺失"
- `data/` 按需创建，sys.fs.write 第一次写时 mkdir

### 2. sys.fs API（5 个）

```js
sys.fs.read(path)             // → string | null   同步
sys.fs.write(path, content)   // → bool            异步入队
sys.fs.exists(path)           // → bool            同步
sys.fs.remove(path)           // → bool            异步入队
sys.fs.list()                 // → ["a.json", ...] 同步
```

**沙箱机制（C 层强制）**：JS 写 `"save.json"`，C 内部翻译成
`/littlefs/apps/<current_app_id>/data/save.json`。拒绝 `..`、`/`、`\`、首字符 `.`。

**read 同步、write 异步**：
- read/exists/list 直接读，几十毫秒可接受
- write/remove 走后台 task 串行落盘，UI 不阻塞
- 这点和 NVS 上的 sys.app.saveState 不同，NVS 同步是因为它本身够快

### 3. NVS 与 FS 各管各场景

`sys.app.saveState/loadState` 仍走 NVS（保留），适合"高频小 KV"。
`sys.fs.*` 走 FS，适合"几 KB 的 JSON / 缓存图"。
两套并存，让业务自己选。

### 4. BLE 上传协议升级

旧：`START.name` 15B = "alarm"，落盘到 `apps/alarm.js`
新：`START.name` 31B = "alarm/main.js"，落盘到 `apps/alarm/main.js`

通用化后，PC 想传 manifest.json / 未来传 assets/icon.png 都走同一协议，无需新 op。

`upload_app(app_id, main_js_path, display_name=...)` 是 PC SDK 一次性上传完整 app（先 manifest 再 main.js）。

不做兼容迁移，erase-flash 重灌。

## 实施

### Phase 1: 存储层重写 (storage/littlefs/dynapp_script_store.{h,c})

- 路径生成全部按 `apps/<id>/<filename>` 拼装
- 新增 `dynapp_app_file_read/write/exists`：app 仓内任意文件 IO
- 新增 `dynapp_app_delete`：递归 rmtree 整个 app 目录（包括 data/）
- 新增 `dynapp_user_data_read/write/remove/exists/list`：sys.fs 沙箱后端，强校验 relpath
- 新增 `dynapp_manifest_read`：手撸 JSON 解析器，只识别 "id" 和 "name" 两个 string 字段，避免引 cJSON
- 流式 writer 接口签名改 `(app_id, filename)` 两参
- init 阶段清理孤儿 .tmp（扫每个 app 子目录）

手撸 JSON 解析器没引 cJSON：MVP 字段全是 ASCII/UTF-8 普通文本，不需要支持 `\"` 转义；20 行 helper 够用。

### Phase 2: BLE 协议改造

`services/dynapp_upload_service.c`：
- `START.name` 字段从 15B 扩为 31B（新常量 PATH_LEN）
- DELETE.name 仍 15B（仅 app_id；删整个 app 目录）

PC 端同步：
- `tools/dynapp_uploader/constants.py` 加 `PATH_LEN = 31`
- `protocol.py` 的 `pack_start` 接受 `path` 而非 `name`
- `client.py` 新增 `upload_app(app_id, main_js, display_name=...)` 高层 API
- `dynapp_push_gui.py` 加 Display 输入框，自动用 `upload_app` 同时上传 manifest+main

### Phase 3: 注册表 & 菜单

`dynamic_app/dynamic_app_registry.{h,c}`：
- `dynamic_app_entry_t` 改成 `{id, display, has_manifest}`
- `list()` 自动调 `dynapp_manifest_read`，display 用 manifest.name；缺失时回退 id

`app/pages/page_menu.c`：
- 显示用 `entries[i].display`（中文 OK），user_data 仍存 `entries[i].id`

### Phase 4: JS 层

`dynamic_app/dynamic_app_natives.c` + `dynamic_app_internal.h`：
- 新增 5 个 native：sys.fs.read/write/exists/remove/list
- write/remove 入队异步；read/exists/list 同步直读
- `DYNAMIC_APP_EXTRA_NATIVE_COUNT` 19 → 24

### Phase 5: FS Worker 抽离（中途重构）

写完 Phase 1-4 后回头看，发现 `dynapp_upload_manager.c` 里塞了三件事：
1. BLE 上传协议状态机
2. BLE 运维（DELETE/LIST）
3. JS sys.fs 异步落盘（USERDATA_WRITE/REMOVE）

第 3 件事是搭便车 —— "我也想要个后台 task 写 FS" 就硬塞进 BLE manager 里。语义错位：JS 引擎为什么要 include `dynapp_upload_manager.h`？

### 抽离方案

新建 `storage/littlefs/dynapp_fs_worker.{h,c}`：
- 独占一个 `dyn_fs` task（prio=2，stack=4KB）
- 队列 16 项，所有 LittleFS 写都串行化
- 暴露 8 个 submit 接口：user_data CRUD + writer 流式 + app_delete + list_apps
- 3 个 done callback 类型（writer_commit / app_delete / list_apps）
- `set_running_check` 钩子搬到这里

`services/manager/dynapp_upload_manager.{h,c}` 瘦身：
- 删除 task / queue / item union / dispatch_*
- 退化为纯 BLE 协议状态机（IDLE / RECEIVING）
- `submit_*` 现在带 `seq` 参数；状态字段单线程访问无锁
- `status_cb` 签名加 `seq`（不再依赖全局 s_last_seq）
- 文件从 380 行压到 ~280 行

### cb_arg 设计

upload_manager 调 fs_worker 的异步 op 时，需要把 (op, seq) 传到 done cb。最简单做法：packed 进 `void*`：

```c
#define MK_CBARG(op, seq)   ((void *)(uintptr_t)(((uint32_t)(op) << 8) | (seq)))
#define CBARG_SEQ(arg)      ((uint8_t)(((uintptr_t)(arg)) & 0xFF))
```

无 malloc，cb 内拆出来调 status_cb。

### 线程模型 / 不变量

- `s_writer`（fs_worker 内）：只在 fs_worker task 上读写，单线程串行天然无竞态
- `s_sess`（upload_manager 内）：只在 BLE host task 上读写，单线程访问
- fs_worker done cb 在 fs_worker task 上调；只读 cb_arg 解包 (op,seq)，不碰 s_sess —— 跨线程零干扰
- `s_cbs[]`（status 多消费者）：只在 init/register 阶段写、运行时只读 —— 无锁安全

## 关于"读不走 worker"的设计取舍

依赖图里 `dynamic_app_registry.c` / `app/pages/page_menu.c` 直接调 `dynapp_script_store_read/list`，**没经过 fs_worker**。这是有意为之：

1. **LittleFS 自带 VFS 锁**：所有 fopen/fread/fwrite/rename 调用都是原子的，跨任务并发安全
2. **写是原子的**：fs_worker 走 atomic_write（先写 .tmp 再 rename），读者要么看老文件、要么看新文件，不会读到半截
3. **读路径无状态**：`read_file` 每次 fopen 独立 FILE*，多任务并发 read 互不干扰
4. **唯一全局可变状态**（s_active_writer）只在 fs_worker 内访问

代价：读会被 worker 的写阻塞（LittleFS 单粒度锁，~10-50ms）。实际上 BLE 上传中按 menu 会卡顿一两秒，但不崩。

不全异步化的理由：UI/JS 的读语义会破坏（同步 read 拿到 string），prelude.js 那一行 `sys.fs.read("save.json")` 要改成 promise/callback —— 复杂度上升一档，得不偿失。

## 菜单本地删除的 Bug 修正

第一版重构后，菜单长按删除走的是 `dynapp_upload_submit_delete(name)` —— 但 manager 接口签名加了 seq 后，编译报错。

借机改对：本地删除直接调 `dynapp_fs_worker_submit_app_delete`，传一个本地 done cb 设 s_dirty。
原来"借道 BLE 协议层做本地操作"是抽象泄漏，修正后两条路径（PC 删 / 本地删）都通过 fs_worker 串行执行，running_check 同时拦截。

## 编译报错记录

1. **format-truncation**：`snprintf(tmp, PATH_BUFSZ, "%s.tmp", path)` 当 path 已满 128 时，再拼 .tmp 可能溢出。修复：tmp 缓冲扩到 `PATH_BUFSZ + 8`。
2. **too few arguments to dynapp_upload_submit_delete**：菜单页旧调用没带 seq。修复见上节。

## 最终依赖图

```
                    ┌──→ fs_worker ──→ script_store  (写)
natives.c (sys.fs.* read 同步)
                    └─────────────────→ script_store  (读)

upload_service ──→ upload_manager ──→ fs_worker ──→ script_store

registry.c ─────────────────────────→ script_store  (read / list / manifest)
page_menu.c ──→ registry           (间接读)
        └──→ fs_worker (本地 delete)
```

## 用户需操作

1. **`idf.py erase-flash`** —— 旧布局不兼容
2. **重新构建烧录**
3. 通过 GUI 重传业务 app，输入中文 display name

## 验收清单

- [x] 存储层（dynapp_script_store）按 app_id 文件夹布局
- [x] manifest.json 极简解析（id/name）
- [x] sys.fs 5 个 native（read/write/exists/remove/list）
- [x] sys.fs 沙箱（C 层强制 prefix，JS 看不到绝对路径）
- [x] FS worker 抽离（task + queue 搬到 storage 层）
- [x] BLE 协议升级（START.name 31B 通用 path）
- [x] PC SDK 升级（upload_app 一次传 manifest+main）
- [x] GUI 加 Display 字段
- [x] 菜单显示 manifest.name（中文 OK）
- [x] 菜单本地删除直接调 fs_worker
- [x] 编译通过
