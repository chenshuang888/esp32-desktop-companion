# 动态 App（MicroQuickJS）——图片/资源 Assets 读取 + 目录上传 + 菜单 icon.bin + Notes 样例 工作日志

日期：2026-04-28

> 本日志由 `docs/未总结出工作日志的对话记录.md` 整理而来，目标是把“对话中已经落地的功能/改动/约束/验收点”沉淀成可复用的工程记录，便于后续迭代与回归。

## 关键结论（先给结论，避免看完还抓不住重点）

1. **assets 目录形态确认**：动态 App 资源采用真子目录布局：`/littlefs/apps/<app_id>/assets/<name>.bin`，不再走“把 assets 平铺成一个文件”的方案。
2. **Image 资源加载路径统一**：JS 侧只传相对资源名（例如 `"fish.bin"`）；设备端统一在 `do_set_image_src()` 拼装为 `A:/littlefs/apps/<current_app_id>/assets/<src_rel>`，确保沙箱与路径一致性。
3. **VDOM 增量支持 image**：VDOM 新增 `type: 'image'`，并把 `src` 从 style 体系中独立出来，走 `sys.ui.setImageSrc()` 做更新（避免与 setStyle 混杂）。
4. **上传工具升级为“目录包上传”**：PC 端按固定目录结构上传 `manifest.json -> main.js -> (可选)icon.bin -> (可选)assets/*.bin`，并在上传前做路径长度（`PATH_LEN=31`）预算校验。
5. **菜单图标 MVP 约定**：每个 App 可选自带 `icon.bin`（约定 32×32 RGB565，路径 `/littlefs/apps/<id>/icon.bin`）。菜单页优先加载该 icon，否则回退到原有 symbol 兜底。
6. **Notes 样例的存储分工**：`sys.app.saveState/loadState` 用于保存 `nextSeq` 这类“小 KV”；便签正文存入 `sys.fs.*`（落 `data/` 沙箱目录），并对齐 `196B/64KB/16项/31字符路径` 等边界。

## 背景

在动态 App（MicroQuickJS）能力逐步完善后，系统从“能跑 JS 脚本”进入“像 App 一样可持续演进”的阶段，核心诉求开始集中在：

1. **App 资源（assets）能力**：很多 UI 不能只靠 label 绘制（例如小图标、鱼图、卡牌、背景等），需要让 App 能读取自己目录下的资源文件。
2. **PC 侧上传闭环**：动态 App 一旦从“单文件 main.js”演进到“main.js + manifest + assets”，上传工具必须支持“目录包”上传，否则会频繁手工操作，效率低且容易出错。
3. **菜单呈现体验**：当 App 数量越来越多，只靠统一兜底 icon 会显得“都长一个样”，需要支持每个 App 自带菜单图标（MVP 先支持一个 `icon.bin`）。
4. **用真实业务脚本压测边界**：需要一个更贴近真实使用的 App（比如“便签/Notes”）来验证 `sys.fs.*` + `sys.app.*` 的语义、限制与 UI 行为闭环。

本轮工作围绕以上 4 点做了从协议到落地的完整闭环：**assets 读取 + 目录上传 + icon.bin 菜单图标 + Notes 样例 App**。

---

## 当前理解（对齐现状）

动态 App 的主链路（概念层面）：

- **脚本侧（JS）**：通过 `sys.ui.*` 创建 UI（VDOM/声明式 UI）并响应事件；通过 `sys.fs.*` 做小文件持久化；通过 `sys.app.*` 保存少量 KV/状态。
- **设备侧（C）**：
  - `dynamic_app/`：registry/runtime/natives/ui 等运行时与桥接。
  - `storage/littlefs/`：动态 App 文件存储（脚本、manifest、assets、用户数据区 data/）。
  - `services/manager/`：上传管理（串行 writer / 单写者模型）。
  - `app/pages/`：菜单页展示与进入动态 App。
- **PC 侧（tools/）**：`tools/dynapp_uploader/*` 提供协议/SDK，`tools/dynapp_push_gui.py` 提供 GUI 推送入口。

约束（对话中反复强调、需要在日志里固化）：

- BLE 上传分片上限：单 chunk 约束为 **196B**（对应协议/缓冲与实现上限）。
- 单文件写入的累计大小上限：约 **≤ 64KB**（适合中小资源与 JSON，超大资源不建议走这条链路）。
- `sys.fs.list()` 返回数量上限：**最多 16 项**（避免 UI/内存爆炸）。
- 路径长度上限：`PATH_LEN = 31`（协议字段/设备端限制，直接影响 `assets/<name>` 的命名预算）。
- `sys.fs.write` 语义：**整体覆盖写（atomic_write）**，不是 append/stream（MVP 简化）。

---

## 对话中对齐过的“硬约束/边界”（必须写进日志，后续排障就靠它）

> 这些数值在对话中多次被拿出来校准方案与实现，属于“改一处会牵动全链路”的硬边界。

- `FS_CHUNK_MAX_BYTES = 196B`：BLE 上传分片上限/写入帧上限；决定了 `sys.fs.write()` 适合小文本/小 JSON，不适合大文件流式写。
- `MAX_SCRIPT_BYTES ≈ 64KB`：单文件累计大小上限；资源 `.bin` 超过该级别需要重新评估（不要强行塞进动态 App 这条链路）。
- `sys.fs.list()` 最多 16 项：脚本侧（Notes）与设备侧都需要对齐该上限，超出要么截断要么显式提示。
- `PATH_LEN = 31`：上传协议与设备端字段上限；对 `"<app_id>/assets/<nm>"` 这种拼接路径需要做预算（尤其是 app_id 取最大 15 时）。
- `icon.bin` 约定 32×32 RGB565：菜单图标只做最小闭环，不在本轮引入“缩放/裁剪/多分辨率”复杂度。

---

## 计划（当时的推进顺序）

为了避免“实现先后顺序反复返工”，对话里把推进顺序做了明确收敛：

1. **先把 assets 资源读取链路打通**（它决定目录布局、路径拼装策略、UI 侧 API 形态）。
2. **同时改造 PC 侧上传为“目录包上传”**（保证开发效率与验收闭环）。
3. **基于 assets 读取能力，落一个 demo 包（imgdemo/imgdemo_pkg/aquarium_pkg）** 做 end-to-end 验证。
4. **在 assets 能力稳定后，再补菜单 icon.bin 支持**（避免一开始就把元信息体系做复杂）。
5. **最后用 Notes 这种更真实的 App 验证 sys.fs/sys.app 的边界、交互与持久化**。

---

## 影响范围

本轮改动会影响以下几个层面：

- **设备端（C）**：动态 App UI 创建（image 控件）、App 资源读取路径拼装、LittleFS 文件写入/临时文件清理策略、菜单页动态 App 项展示。
- **PC 端（Python tools）**：目录包上传能力、上传步骤/进度展示、对路径长度/文件名合法性/资产数量的提前校验。
- **脚本端（JS）**：新增/调整 demo 脚本（imgdemo、aquarium、notes），以及对 `sys.ui.createImage` / `sys.fs.*` 的正确用法示范。

---

## 风险（当时的主要担忧与应对）

1. **路径长度（PATH_LEN=31）导致 assets 文件名预算极小**  
   - 风险：`<app_id>/assets/<name>` 容易超长，导致上传失败或设备端拒收。
   - 应对：PC 侧在 `client.py` 对 `assets/<name>` 做预算校验（按 `31 - len(app_id) - len('/assets/')` 计算）。

2. **LittleFS 的原子改名/临时文件策略**  
   - 风险：`.tmp` 写入路径与最终路径不在同目录时，rename/commit 可能失败（或语义不可控）。
   - 应对：writer tmp 路径与 final 路径保持同目录；并在 init/清理时把 `assets/` 子目录的残留 `.tmp` 也纳入清理范围。

3. **UI 卡顿与写入阻塞**  
   - 风险：在 UI 线程同步写大文件会卡 LVGL。
   - 应对：保持“read 同步、write/remove 异步入队”的策略（沿用 single-writer/worker 模式）。

4. **资源格式不统一导致显示失败**  
   - 风险：LVGL image 如果 src 格式/色深不对，可能黑屏/花屏/显示异常。
   - 应对：明确 MVP 约定：`*.bin`（RGB565），并复用 `managed_components/lvgl__lvgl/scripts/LVGLImage.py` 生成。

---

## 验收标准（建议的验收步骤）

> 注意：按项目约束，build/flash/monitor 这类重验证通常由使用者执行；本日志固化的是“当时对齐过的验收流程”。

1. 设备侧：编译并刷入（`idf.py build flash monitor`），确保 LittleFS 与 LVGL FS driver 正常。
2. 手工 sanity：先在设备文件系统中放一个测试资源到 `.../assets/test.bin`，用最小 JS 走 `sys.ui.createImage(..., 'test.bin')` 验证显示链路。
3. PC 工具上传：用 `tools/dynapp_push_gui.py` 选择一个 pack 目录（含 `main.js + assets/*.bin`），上传后菜单能出现对应 App。
4. 回归：跑已有 App（如 alarm/calc/timer/2048/notes 等）确认无破坏。
5. 边界检查：
   - 上传 65KB asset 应被拒绝（或提前在 PC 端提示超限）。
   - asset 文件名/路径超 `PATH_LEN` 应在 PC 端或设备端被拦截。
   - `sys.fs.list()` 超 16 项行为明确（截断/提示/约束）。
   - `sys.ui.setImageSrc()` 指向不存在的资源文件时：LVGL 允许“空/不显示”，但系统不应崩溃或触发异常重启。
   - `sys.fs.write("../assets/foo.bin", "x")` 这类路径穿越：应被沙箱校验拦截（确保 App A 不能写到 App B 或 assets 区）。

---

## 方案与决策

### 1) App 目录与资源约定（MVP）

MVP 采用“App 目录 + 固定入口 + 可选资源”的约定：

```
/littlefs/apps/<app_id>/
  main.js            # 固定入口
  manifest.json      # 可选（至少包含 id/name）
  icon.bin           # 可选（菜单图标，约定 32×32 RGB565）
  assets/            # 可选（资源目录）
    <name>.bin
  data/              # sys.fs 沙箱写入区（用户数据）
    *.txt / *.json
```

### 2) 资源读取路径拼装（LVGL FS 路径）

关键点：**JS 侧写相对路径，C 侧负责拼装成 LVGL 可识别路径**。

- JS：`sys.ui.createImage("img1", parentId, "fish_red.bin")`
- C（概念）：拼装为 `A:/littlefs/apps/<current_app_id>/assets/fish_red.bin`

这样可以做到：

- App A 无法“随便引用/越权读取” App B 的资源（路径前缀由系统强制添加）。
- JS API 简洁（不暴露系统绝对路径，不引入 mount 概念）。
- 资源文件名可复用（每个 App 的 assets 命名互不干扰）。

### 3) 目录包上传（PC 侧）

上传工具支持“选择目录 pack_dir”，并按固定顺序上传：

1. `manifest.json`（如不存在则自动生成一个最小 manifest）
2. `main.js`
3. `icon.bin`（可选）
4. `assets/*.bin`（可选，按字典序稳定上传）

这样可以把“一个 App 的完整素材”一次性推送到设备端，减少操作成本，也便于未来扩展更多资源类型。

### 4) Notes 样例的存储策略

Notes 作为 `sys.fs.*` 的“真实压测样例”，对话里明确了策略：

- **索引/序号**（例如 `nextSeq`）放 `sys.app.saveState/loadState`（NVS，适合小 KV、频繁写）
- **正文内容**（便签正文）放 `sys.fs.write/read/remove/list`（LittleFS data/ 区，适合文件）

并在脚本侧做“容量对齐”：

- 单条便签正文预留余量：例如在脚本内把最大正文控制在 ~180B（留出 UTF-8 与协议余量）
- 总条数控制：最多 16（对齐 `fs.list()` 上限）

---

## 实施过程（按阶段记录）

### Phase 0：梳理与对齐（避免拍脑袋改动）

本阶段主要产出是**把 tradeoff 明确写清楚**，包括：

- 入口到底是固定 `main.js`，还是 manifest 声明 entry（MVP 选固定入口，减少失败面）
- sys.fs 沙箱策略（路径前缀由系统强制注入，禁止 `..` 与绝对路径）
- write 是否异步（MVP 保持异步入队，避免 UI 卡顿）
- 是否做兼容迁移（部分场景选择 erase-flash 重灌，降低迁移复杂度）

### Phase 1：打通 assets 资源读取（设备端 + demo 脚本）

围绕“让动态 App 能读取自己 assets 下的 `.bin` 并显示成 image”，主要工作点包括：

- 设备端补齐 image 控件创建时的 `src` 解析与路径拼装（统一成 `A:/littlefs/apps/<id>/assets/<src>`）
- 设备端存储层允许写入/落盘 `assets/<name>.bin`（即允许 App 文件仓内存在 assets 目录）
- demo：`imgdemo` / `imgdemo_pkg` 用来验证 `.png -> .bin` 的生成与显示链路

对话中专门提到使用 `managed_components/lvgl__lvgl/scripts/LVGLImage.py` 将 PNG 转为 LVGL 可加载的 BIN（RGB565），并且强调要做 sanity check（避免“编译后才发现资源格式不对”）。

#### 1.1 JS API 与 VDOM 侧约定（image 的“最小可用”接口）

对话中对齐的 API 形态是（MVP 先把接口收敛到最少）：

- `sys.ui.createImage(id, parent?, src?)`
  - `id`：string（必传）
  - `parent`：string|null（可选）
  - `src`：string|null（可选），表示相对 assets 目录的资源名，例如 `"fish.bin"`
- `sys.ui.setImageSrc(id, src)`：动态切换已有 image 的资源（用于动画/逐帧切换）
- VDOM：新增 `type: 'image'`
  - mount 分支：`type === 'image'` 时调用 `sys.ui.createImage(...)`
  - 更新分支：当 `src` 发生变化时，调用 `sys.ui.setImageSrc(...)`（**不混进 style 的 setStyle**）

对话里还明确了一个关键细节：**image 的创建与 src 设置分离**。

- create 阶段只创建对象；
- drain/执行阶段再调用统一的 `do_set_image_src()` 完成“路径拼装 + `lv_image_set_src()`”。

这样做的好处：

- 设备端只维护一套路径拼装逻辑（避免 do_create / create_image 两处重复拼路径，后续改路径规则也只改一处）。
- 更容易保证“parent_id 解析/union 字段写入/队列 drain”这类易错点的正确性。

#### 1.2 资源路径拼装规则（确保沙箱与可预测性）

对话中把路径约定写得非常具体：

- JS 侧传的是相对资源名：`src_rel = "fish.bin"`（只允许 base 文件名，不允许路径穿越）
- 设备端拼装为 LVGL FS 绝对路径：

  `A:/littlefs/apps/<current_app_id>/assets/<src_rel>`

其中 `<current_app_id>` 来自“当前运行的 app id”（对话里提到通过 `dynamic_app_registry_current()` 获取）。

注意点（对排障非常关键）：

- **菜单页不是动态 App sandbox**：菜单页是 C 代码，不走 `dynamic_app_registry_current()`；因此菜单加载 icon 的路径需要用 entry.id 显式拼装（后续 Phase 4 做 icon.bin 依赖这一点）。
- assets 的真实存储位置必须和拼装规则一致：如果 PC 端没有把文件传到 `/littlefs/apps/<id>/assets/`，那么路径拼得再对也会找不到文件。

#### 1.3 资源生成工具链（PNG → BIN RGB565）

对话中建议直接复用 LVGL 官方脚本：

`managed_components/lvgl__lvgl/scripts/LVGLImage.py`

并强调生成参数要稳定（避免“看起来能用，实际颜色/格式不对”）：

- 输出格式：`--ofmt BIN`
- 色彩格式：`--cf RGB565`
- 尺寸：demo 先用 32×32 做闭环（与 icon.bin 的尺寸约定一致）

对话中出现过的典型报错：脚本参数不符合预期时会报 “unrecognized arguments …”，需要按脚本要求传参，不要把多个 png 参数当成位置参数乱传。

### Phase 2：改造 PC 上传工具支持“目录包上传”

这一阶段的目标是把“上传一个 App”从“单文件推送”升级为“目录包推送”，核心点：

- GUI 侧允许选择目录，并显示 pack 内容概览（main.js、assets 数量、总大小等）
- client SDK 侧提供 `upload_app_pack(pack_dir)` 一类高层 API，内部按既定顺序上传
- 对路径长度、文件名合法性、assets 数量等做提前校验（尽量在 PC 侧报错，减少设备侧灰度问题）

#### 2.1 pack 目录结构与上传顺序（严格固化）

对话中固化的 pack 结构是：

```
<pack_dir>/
  main.js          （必需）
  manifest.json    （可选；缺省时由 PC 侧自动生成最小 manifest）
  icon.bin         （可选；菜单图标，约定 32×32 RGB565）
  assets/          （可选）
    <name>.bin
    ...
```

上传顺序固定为：

1. `manifest.json`
2. `main.js`
3. `icon.bin`（如果存在）
4. `assets/*.bin`（如果存在，按字典序稳定遍历）

为什么顺序要固定：

- 便于端到端验收时定位“卡在哪一步”（例如 GUI 显示 `uploading 3/8 assets/fish.bin` 这种进度提示就很直观）。
- 减少设备端状态机复杂度（writer/commit 清晰、出错面更小）。

#### 2.2 PATH_LEN=31：assets 文件名预算必须前置校验

对话里把 `PATH_LEN=31` 当成一个“小坑”专门拎出来强调：真正写入 FS 的 path 形如：

`"<app_id>/assets/<nm>"`

因此 `<nm>` 的预算必须按 app_id 长度扣减：

`budget = PATH_LEN - len(app_id) - len("/assets/")`

PC 端应在上传前就拦截超预算的 assets 文件名，避免“传到一半才失败”（尤其是 app_id 取最大 15 时预算会很紧）。

#### 2.3 GUI 侧的 pack 概览（减少误判与返工）

对话中强调 GUI 不要只写 “main.js + N assets”，还要把 `icon.bin` 是否存在反映出来，避免用户以为“我上传了 icon 但没生效”，或误把“没有 icon”当成“菜单渲染有 bug”。

### Phase 3：Notes 样例 App（验证 sys.fs/sys.app 的边界与交互闭环）

`dynamic_app/scripts/notes.js` 的定位不是“产品级便签”，而是一个**边界明确、可回归、可压测**的样例：

- 列表页：通过 `sys.fs.list()` 枚举便签文件（例如 `n1.txt/n2.txt`），并用 `sys.fs.read()` 提取首行做预览
- 详情页：编辑/更新便签内容，通过 `sys.fs.write()` 覆盖写回
- 删除流程：长按触发二次确认，再调用 `sys.fs.remove()`
- 持久化：使用 `sys.app.saveState/loadState` 保存 `nextSeq`，避免重启后文件命名冲突
- 限制对齐：脚本内限制最大条数与单条内容字节数，避免触发 196B/16 项等上限

### Phase 4：菜单 icon.bin 支持（让 App “长得不一样”）

在 assets 基础能力稳定后，补上菜单层面的 MVP 图标：

- 约定：`/littlefs/apps/<id>/icon.bin` 作为该 App 的菜单图标（32×32，RGB565）
- 菜单页渲染策略：
  - 如果 icon.bin 存在：用 `lv_image` 显示（路径形如 `A:/littlefs/apps/<id>/icon.bin`）
  - 否则：回退到原先的内置 `LV_SYMBOL_LIST`（或已有 icon_for_app 兜底）

这样做的好处是：

- 不引入复杂 icon 元数据体系（manifest 里暂不放 icon 字段也能工作）
- 不影响老 App（没有 icon.bin 就继续走原逻辑）

#### 4.1 为什么 icon.bin 放在 apps/<id>/icon.bin（而不是 assets/ 里）

对话里明确提到一个关键上下文：**菜单页不是动态 app sandbox**，也就没有 `dynamic_app_registry_current()`。

因此 menu 侧最容易拿到的就是 `entries[i].id`，用它拼装：

`/littlefs/apps/<id>/icon.bin` → `A:/littlefs/apps/<id>/icon.bin`

把 icon 放在 App 根目录一层，属于“最少层级、最少绕弯”的 MVP 方案：

- 不需要解析 manifest 里的 icon 字段（避免引入 JSON 解析复杂度与边界条件）
- 不需要把菜单当作“动态 App 的一部分”处理（权限/沙箱语义清晰）
- 不影响 assets 的资源体系（assets 仍然保留给 App 运行时使用）

#### 4.2 设备端检测策略（存在即用，不存在回退）

对话里对检测策略的原则是“稳”：

- 直接对 `/littlefs/apps/<id>/icon.bin` 做文件存在性判断（例如 `stat()` 并判断 regular file）
- 存在 → `lv_image_set_src(icon_img, "A:/.../icon.bin")`
- 不存在 → 走原来的 symbol icon 逻辑（不影响旧行为）

### Phase 5：icon 生成脚本沉淀（降低素材准备成本）

为降低“手工准备 32×32 RGB565 icon.bin”的成本，补了一个小脚本生成链路：

- `dynamic_app/scripts/imgdemo_pkg/_make_icon.py`：用 PIL 生成 32×32 的 `icon.png`，再调用 `LVGLImage.py` 转成 `icon.bin`

这类脚本的价值在于：

- 降低素材制作门槛（先用程序生成占位 icon，后续再替换成真实设计稿）
- 统一生成参数（RGB565、输出格式、尺寸约束），避免素材不一致

---

## 关键排障点（对话中踩过的坑，写出来避免二次踩坑）

1. **“路径拼得对但就是不显示图片”**  
   典型原因不是路径拼装，而是 assets 根本没传上去（例如上传中途被协议/校验拦截，或 pack_dir 结构不满足导致 assets 遍历不到）。排障建议：
   - 先看 GUI 侧进度是否真的上传到 `assets/<name>.bin`
   - 再看设备侧 writer 日志是否出现类似 `writer opened: /littlefs/apps/<id>/assets/<name>.bin.tmp`
   - 最后确认 `do_set_image_src()` 拼出来的 `A:/littlefs/apps/<id>/assets/<name>.bin` 与上传路径一致

2. **`PATH_LEN=31` 导致 assets 名字预算过紧**  
   表现：PC 端直接报 “path too long / budget=xx”，或设备端拒收。解决：
   - 缩短 `<nm>`（优先）
   - 或缩短 `app_id`（不推荐频繁改，但在极限预算下可救急）

3. **`LVGLImage.py` 参数/用法不匹配**  
   表现：`LVGLImage.py: error: unrecognized arguments: ...`  
   解决：按脚本要求传参；建议固定成“单文件转换”的调用方式，避免一次性塞多个参数。

4. **`.tmp` 残留导致目录脏**  
   表现：断电/中断上传后留 `.tmp`，下次启动可能影响空间与行为。对话里明确需要清理：
   - `apps/<id>/` 目录下的孤儿 `.tmp`
   - `apps/<id>/assets/` 子目录下的孤儿 `.tmp`

5. **assets 文件名被“路径解析规则”误判为非法**  
   对话里提到过一个典型场景：上传路径里出现多层斜杠（例如 `imgdemo_pkg/assets/a.bin`），如果协议解析器/校验规则把它当作“非法路径”，就会导致 assets 一张都传不上去，进而出现“设备端拼路径正确但文件不存在”的假象。排障时优先确认“协议是否允许目录层级/是否按既定 pack 结构传输”。

---

## 按文件的变更清单（便于 code review 与回归）

> 目标：后续只看这一段，也能快速定位“改动落在什么文件、为什么要改、改了什么点”。

- `dynamic_app/dynamic_app_ui.c`、`dynamic_app/dynamic_app_ui.h`、`dynamic_app/dynamic_app_ui_internal.h`
  - 增量支持 image 类型 UI 节点（创建 + 更新）
  - image 的 `src` 设置收敛到 `do_set_image_src()`：统一做路径拼装与 `lv_image_set_src`
- `dynamic_app/dynamic_app_natives.c`、`dynamic_app/dynamic_app_internal.h`、`dynamic_app/dynamic_app_runtime.c`
  - 暴露 `sys.ui.createImage` / `sys.ui.setImageSrc` 的 native 绑定
  - 参数校验与错误提示收敛（id 必须为 string，src 必须为 string|null 等）
- `storage/littlefs/dynapp_script_store.c`
  - 支持 `assets/` 子目录相关路径写入（允许 `assets/foo.bin` 落到 `<app_id>/assets/foo.bin`）
  - `.tmp` 清理逻辑扩展到 `assets/` 子目录，避免中断上传残留
- `services/manager/dynapp_upload_manager.c`
  - 与 writer/单写者模型对齐，确保多文件上传（manifest/main/icon/assets）仍然串行落盘
- `tools/dynapp_uploader/client.py`
  - `upload_app_pack(pack_dir)` 扩展识别根目录 `icon.bin` 并上传
  - 对 `assets/<nm>` 做 `PATH_LEN=31` 预算校验，提前拦截超长文件名
- `tools/dynapp_push_gui.py`
  - pack 概览统计加入 `icon.bin`（提示用户“本次包内包含 icon”）
  - 进度/总大小统计纳入 icon.bin，减少“上传了但不知道有没有传到”的认知成本
- `app/pages/page_menu.c`
  - 动态 App 菜单项：存在 `icon.bin` 则用 `lv_image` 渲染，否则回退 symbol
- `dynamic_app/scripts/notes.js`
  - sys.fs + sys.app 的真实样例：list/read/write/remove + nextSeq 持久化 + 边界对齐
- `dynamic_app/scripts/imgdemo_pkg/_make_icon.py`
  - 32×32 icon.png + 调用 LVGLImage.py 生成 icon.bin 的小工具脚本

---

## 涉及文件（按模块归类）

> 这里列的是对话与当前仓库状态中明确涉及的关键文件，便于后续定位与回归。

设备端 / UI / 菜单：

- `app/pages/page_menu.c`（动态 App 菜单项支持 icon.bin：存在则 lv_image，否则回退 symbol）
- `dynamic_app/dynamic_app_ui.c`、`dynamic_app/dynamic_app_ui.h`、`dynamic_app/dynamic_app_ui_internal.h`
- `dynamic_app/dynamic_app_natives.c`、`dynamic_app/dynamic_app_internal.h`、`dynamic_app/dynamic_app_runtime.c`

设备端 / 存储：

- `storage/littlefs/dynapp_script_store.c`（assets 子目录、writer/tmp 清理、资源文件路径支持）
- （对话提及）`storage/littlefs/dynapp_fs_worker.c`（如涉及 writer_open 路径对齐与异步落盘）

PC 工具：

- `tools/dynapp_push_gui.py`（目录包选择/概览；icon.bin + assets 统计展示）
- `tools/dynapp_uploader/client.py`（`upload_app_pack`：支持上传 icon.bin 与 assets，并做 PATH_LEN 预算校验）

脚本与资源：

- `dynamic_app/scripts/notes.js`（sys.fs/sys.app 的真实样例）
- `dynamic_app/scripts/aquarium.js`（对话中提及：将 label 鱼替换为 image + assets）
- `dynamic_app/scripts/imgdemo_pkg/*`（imgdemo 包：main.js + a/b/c.png + 生成脚本等）

---

## 后续 TODO（对话里提到但未强制在本轮完成）

1. **manifest 元信息扩展**：版本号、作者、描述、能力声明（permissions）等后续逐步加，但要保持向后兼容。
2. **更完善的资源体系**：目前 MVP 仅支持 `.bin(RGB565)`；后续可以考虑更多格式/压缩/分辨率与缓存策略。
3. **更严格的配额与异常提示**：例如按 App 统计空间占用、LittleFS 满时的可观测性与 UI 提示。
4. **Notes 的 PC 同步版**：对话中提到可以做“便签 + companion provider”的双端同步，但属于后续增量。
