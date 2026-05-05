# Trellis 工作流适配日志

**日期：** 2026-04-21
**分支：** `feat/treill_init`

## 任务概述

引入 Trellis 工作流后发现两个不适配问题：

1. Trellis 的默认 bootstrap 任务假设项目是 fullstack web，要求填 `spec/backend/` 和 `spec/frontend/`——对本项目这种 ESP32 固件 + LVGL UI + Python 桌面伴侣的嵌入式场景毫无意义。
2. 从上游导入的 `spec/iot/` 知识库共 138 条，覆盖了很多不是本项目场景的条目（STM32/HAL、ASR-Pro 语音、UART DMA、KVM、动态 App、文件系统等），直接复用会把 AI 的上下文塞满无关噪音，并且违反 Trellis 自己的"Fit Check"复用安全规则。

这次迭代做的事：把通用的 `spec/iot/` 按本项目真实基线裁剪一遍，补上只适用于本项目的条目，同时把工作流本身的状态理顺。

最终目标：**下次开会话 AI 能拿到的 spec 索引只有本项目能用的内容，并且知道本项目的独有约束。**

---

## 阶段一：本项目基线盘点

Fit Check 之前要先明确"本项目是什么"。基线从 `README.md`、`sdkconfig.defaults`、`drivers/board_config.h`、几个 service 源码里提取：

| 维度 | 实际情况 |
|------|---------|
| 板型 | ESP32-S3 **N16R8**（16MB QIO Flash + 8MB OCT PSRAM）|
| SDK | ESP-IDF v5.4.3 |
| UI | LVGL 9.5 + ST7789 240×320 SPI + FT5x06 I2C 触摸（**非 FT6336U**）|
| 字体 | Tiny TTF 运行时渲染 + 中文子集 EMBED_FILES（**非 binfont，非 LittleFS**）|
| 显示缓冲 | 40 行部分刷新 + 内部 RAM + DMA（**非全屏双缓冲 PSRAM**）|
| BLE | NimBLE 4.2（**非 5.0**），5 个自定义 service（UUID `8a5c000x`）+ 标准 CTS |
| PC 端 | Python + bleak + customtkinter + winsdk |
| 分层 | `main / app / framework / drivers / services / tools` |
| 无 | STM32、ASR-Pro 语音、UART 业务、WiFi、HTTP、OTA、LittleFS、多 App、动态 App、Web、KVM |

这张表同时写进了 `.trellis/tasks/00-bootstrap-guidelines/prd.md`，作为后续每次 Fit Check 的判断依据。

---

## 阶段二：按目录做 Fit Check 分类

对 `spec/iot/` 下 6 个子目录逐条过滤，每条归到四档之一：

- `[✓]` 直接可用（不变式 + 参数都匹配）
- `[~]` 需适配（思路可用但某个关键细节不同）
- `[?]` Reference 保留（暂不用但将来可能用）
- `[✗]` 删除（与项目无关）

得到的归档数字：

| 目录 | 总数 | 直接可用 | 适配 | Reference | 删除 |
|------|------|---------|------|-----------|------|
| firmware | 39 | 22 | 2 | 4 | 9 |
| device-ui | 34 | 18 | 3 | 0 | 10 |
| protocol | 13 | 5 | 0 | 0 | 6（+2 孤儿）|
| ota | 5 | 0 | 0 | 1 | 4 |
| host-tools | 14 | 5 | 0 | 0 | 9 |
| iot/guides | 29 | 15 | 0 | 0 | 14 |
| **合计** | **134** | **65** | **5** | **5** | **54** |

另外新增 7 条本项目独有条目，详见阶段五。

---

## 阶段三：Bootstrap 任务 Pivot

Trellis 初始化时已经创建了 `00-bootstrap-guidelines` 任务，`.current-task` 指向它，`task.json` 的 `relatedFiles` 是 `spec/backend/` 和 `spec/frontend/`——但那俩目录都不存在。

改法保留 `id` 不变（避免 `.current-task` 失效），只改语义：

```json
{
  "id": "00-bootstrap-guidelines",
  "name": "IoT Spec Fit Check",
  "subtasks": [
    "Phase 1: delete 54 unrelated entries",
    "Phase 2: adapt 5 mismatched entries",
    "Phase 3: write 7 project-specific entries",
    "Phase 4: refresh 6 index.md files",
    "Phase 5: regression verification"
  ],
  "relatedFiles": [".trellis/spec/iot/", ...],
  "meta": {
    "pivoted_from": "fullstack-bootstrap-template",
    "pivoted_at": "2026-04-21"
  }
}
```

`prd.md` 同步重写为按目录组织的 checkbox 执行清单（92 个待办项，覆盖 Phase 1-5）。

---

## 阶段四：执行归档与清理

### Phase 1：归档 54 条无关条目

不走 `rm` 直接删，而是 `mv` 到 `.trellis/spec/_archived_unrelated/2026-04-21/<原目录>/`，便于反悔。归档分布：

```
_archived_unrelated/2026-04-21/
├── firmware/     9 条（stm32×2 + asrpro×5 + uart×2）
├── device-ui/   10 条（binfont×3 + 文件系统×2 + 架构不符×3 + 交互不符×2）
├── protocol/    8 条（asrpro-uplink + handwrite + kvm-tcp-jpeg + parser + downloader×2 + 2 孤儿）
├── ota/         4 条（全 stm32 专用）
├── host-tools/  9 条（js/web/ASR/拼音等）
└── guides/     14 条（多 App + 动态 App + 资源管理器 + KVM + 音频 + embedded-web）
```

执行完 `ls -R _archived_unrelated/2026-04-21/ | wc -l` = 54 ✓

### Phase 4a：同步清理 6 个 index.md

每个子目录的 `index.md` 删除已归档条目的链接。过程中发现 `protocol/index.md` 原本就有两条断链（`pcm-serial-stream-contract.md` 和 `udp-rgb565-frame-fragmentation-contract.md` 指向的文件实际存在，但属于不相关领域），顺手一起归档。

用 `Write` 整体重写每个 index（比用 `Edit` 一条条删更稳）。重写时还顺手加了"本项目语境"说明，例如 `host-tools/index.md` 开头加：

> 本项目 PC 端使用 `bleak`（BLE）+ `customtkinter`（GUI）+ `winsdk`（Windows 媒体会话）构建桌面伴侣 `tools/desktop_companion.py` 及 `tools/ble_time_sync.py`。

`ota/index.md` 加：

> 本项目暂未实现 OTA（`storage` 分区预留），本目录内容作为将来加 OTA 时的起点参考。

---

## 阶段五：补写 7 条本项目独有 spec

Trellis 知识库的 5 类模板（playbook/pitfall/checklist/contract/decision-record）都在 `_templates/` 下。按模板必备字段写，每条都引用本项目真实代码行号作为证据：

| 新增条目 | 类型 | 证据来源 |
|---------|------|---------|
| `firmware/esp32s3-n16r8-qio-flash-oct-psram-pitfall.md` | Pitfall | `sdkconfig.defaults:52-76` 注释 + README 硬件表 |
| `firmware/sdkconfig-defaults-regen-pitfall.md` | Pitfall | README line 153 的警告 |
| `firmware/nimble-mem-external-psram-playbook.md` | Playbook | `sdkconfig.defaults:78` `BT_NIMBLE_MEM_ALLOC_MODE_EXTERNAL=y` |
| `device-ui/tiny-ttf-plus-fontawesome-fallback-playbook.md` | Playbook | `app/app_fonts.c` 完整实现 + `sdkconfig.defaults:12-21` |
| `device-ui/lvgl-40row-partial-buffer-vs-fullscreen-decision-record.md` | Decision Record | `drivers/lvgl_port.c:79-83` + `board_config.h:34` |
| `protocol/esp-to-pc-notify-request-pattern-playbook.md` | Playbook | `services/control_service.c:122-165` |
| `protocol/ble-custom-uuid-allocation-decision-record.md` | Decision Record | 5 个 `*_service.c` 的 UUID 宏 + README BLE 表 |

每条都严格按"上下文签名 → 不变式 → 参数清单 → 设计边界 → 实施步骤 → 停手规则 → 验证顺序 → 常见问题"的结构写，不是泛泛的介绍。

### Phase 2：给 5 条"部分适配"条目加说明

不改原文内容（它们对其他场景仍然有用），只在标题后插入"本项目适配说明"引用块，告诉读者本项目的具体差异：

- `device-ui/st7789-ft6336u-lvgl9-bringup-playbook.md` → 触摸 IC 是 FT5x06 不是 FT6336U
- `device-ui/lvgl-fullscreen-double-buffer-psram-render-mode-full-playbook.md` → 本项目走 40 行部分刷新，见新的 DR
- `device-ui/font-manager-contract.md` → 本项目没独立 font_manager，字体在 `app/app_fonts.c` 直接初始化
- `firmware/lvgl-allocator-and-binfont-oom-hardening-playbook.md` → 不用 binfont，但 allocator 思路（`LV_USE_CLIB_MALLOC=y`）一致
- `firmware/http-streaming-utf8-and-lvgl-thread-bridge-playbook.md` → 不用 HTTP，但"后台任务→UI 线程桥接"对 BLE 同样适用

### Phase 4b：新增条目挂进 index.md

在对应目录 `index.md` 末尾加一个 `### 本项目专属（demo6）` 小节，把新增 7 条以项目身份挂进去：

- `firmware/index.md` 加 3 条（N16R8 / sdkconfig regen / NimBLE PSRAM）
- `device-ui/index.md` 加 2 条（Tiny TTF fallback / 40 行 DR）
- `protocol/index.md` 加 2 条（ESP→PC NOTIFY / BLE UUID DR）

---

## 阶段六：回归验收

写了个一次性 Python 脚本做自动校验：

```python
# 扫所有 index.md，正则抓 `./xxx.md` 链接，逐个验证目标文件是否存在
# 同时扫所有 md 文件，找出"磁盘有但 index 未引用"的孤儿
```

结果：

```
[断链] 链接数 88, 断链 0
[孤儿文件] 0 个

[统计]
目录           有效  归档
device-ui       26    10
firmware        33     9
guides          15    14
host-tools       5     9
ota              1     4
protocol         7     8
合计            87    54
```

87 + 7 个 index.md = 94 个文件，和预期匹配。

数字写进 `task.json` 的 `meta.verification`，以后可查。

---

## 阶段七：工作流状态理顺

### Task finish + archive

`00-bootstrap-guidelines` 做完后，`.current-task` 还指向它，每次 session-start 都会显示：

```
Status: NOT READY
Missing: Context not configured (no jsonl files)
```

——因为 Trellis 的 multi-agent pipeline 期待 "ready" 状态的任务有 `implement.jsonl` 等 context 文件，但我们没用那个功能。解决办法是把任务归档掉：

```bash
python ./.trellis/scripts/task.py finish
python ./.trellis/scripts/task.py archive 00-bootstrap-guidelines
```

注意到 `archive` 触发了 Trellis 的 **auto-commit** 行为（commit `877400c: chore(task): archive 00-bootstrap-guidelines`）——这是 Trellis 脚本自己调 git，不是 AI 越权操作。commit 内容只含 task 目录的移动，语义合理。

归档后 session-start 显示 `Status: NO ACTIVE TASK`，干净。

### `_extracted/` 工具产物迁移

`spec/iot/_extracted/` 和 `_extracted_v1_20260420/` 是上一轮会话的蒸馏 backlog（Python 脚本 + 12000+ 行对话切块），属于工具产物不是 spec 正文。虽然不会被 `session-start.py` 注入（它们没 `index.md`），但会被 `git add .trellis/` 一起提交。

迁移到 `.trellis/_backlog/`：

```bash
mkdir -p .trellis/_backlog
mv .trellis/spec/iot/_extracted .trellis/_backlog/
mv .trellis/spec/iot/_extracted_v1_20260420 .trellis/_backlog/
```

从此 `spec/iot/` 干净。

---

## 适配前后对比

### spec 目录

```
适配前：                          适配后：
.trellis/spec/                    .trellis/spec/
├── guides/          3 条          ├── guides/          3 条（不变）
└── iot/           138 条          ├── iot/            87 条 + 6 个 index
    ├── _extracted/ 30 文件        │   ├── firmware/    33
    ├── _extracted_v1_...          │   ├── device-ui/   26
    ├── _templates/                │   ├── guides/      15
    ├── firmware/   39             │   ├── protocol/     7
    ├── device-ui/  34             │   ├── host-tools/   5
    ├── protocol/   13             │   ├── ota/          1
    ├── ota/         5             │   ├── shared/       0
    ├── host-tools/ 14             │   └── _templates/  4
    └── guides/     29             └── _archived_unrelated/2026-04-21/
                                       └── ...          54 条（留档）
```

### session-start hook 注入量

- 适配前：guidelines 里塞了 fullstack 模板 `## backend` + `## frontend`（空的）+ iot 总索引——AI 会被混淆
- 适配后：只有 `## guides` + `## iot`，干净且指向实际存在的内容

---

## 给将来的自己：使用指南

### 开新任务

```bash
# Windows 用 python 不是 python3
python ./.trellis/scripts/task.py create "任务标题" --slug <任务名>
python ./.trellis/scripts/task.py start <任务名>
```

`start` 之后 `.current-task` 指向它，下次开会话 SessionStart hook 自动注入。

### 任务做完归档

```bash
python ./.trellis/scripts/task.py finish
python ./.trellis/scripts/task.py archive <任务名>
```

注意 `archive` 会 auto-commit。

### 按场景找 spec

| 我要做什么 | 先去哪里找 |
|----------|----------|
| 改板型 / 分区 / Flash 配置 | `spec/iot/firmware/esp32s3-n16r8-*` + `sdkconfig-defaults-*` |
| 调 NimBLE 内存 / GATT 配置 | `spec/iot/firmware/nimble-mem-external-psram-playbook.md` + `ble-conn-shared-state-*` |
| 加 BLE 服务 / 改协议 | `spec/iot/protocol/` 下 5 个 `ble-*-contract.md` + 两个本项目 DR |
| ESP 要主动问 PC 要数据 | `spec/iot/protocol/esp-to-pc-notify-request-pattern-playbook.md` |
| 改中文字体 / 图标 | `spec/iot/device-ui/tiny-ttf-plus-fontawesome-fallback-playbook.md` |
| 新增 LVGL 页面 | `spec/iot/device-ui/page-framework-*` + `lvgl-240x320-time-menu-page-*` |
| PC 端 GUI 改造 | `spec/iot/host-tools/customtkinter-sidebar-navigation-*` + `desktop-companion-bleak-multiplex-*` |
| 改 NVS 持久化 | `spec/iot/firmware/nvs-single-writer-contract.md` + `nvs-persist-settings-store-*` |

### 复用 spec 条目前先做 Fit Check

强制流程在 `spec/iot/guides/spec-reuse-safety-playbook.md`。核心：
1. 找到最相近的 1~3 个条目（别一次抄 10 篇）
2. 写一段 Context Signature：对齐平台/SDK/外设/资源/并发模型
3. 分层：哪些是不变式（直接用）/ 参数（先填再用）/ 示例（先映射再用）
4. 不匹配就停手，先跑最小冒烟闭环

---

## 遗留事项

1. **新增 7 条 spec 未经人工 review**：措辞、停手规则、验收标准是 AI 基于本项目代码写的，建议抽 2-3 篇扫读确认（尤其 `lvgl-40row-*-decision-record.md` 和 `ble-custom-uuid-allocation-decision-record.md` 两篇决策记录）
2. **workflow.md 里 20+ 处 `python3`**：Trellis 官方模板是 Linux/Mac 写法，Windows 下 AI 会自动改 `python`，但人类照着贴会报错；不推荐改 `workflow.md`（下次 `trellis update` 会覆盖），建议在 `AGENTS.md` 或项目 `CLAUDE.md` 里加一行约定
3. **Trellis auto-commit 行为**：`task.py archive` 和 `add_session.py` 会自动 `git commit`，和"AI 不 commit"约定有边界重叠，使用时注意
4. **spec/iot/shared/ 空壳**：只有 `index.md`，没有实质条目，保留作为"跨域硬规则"入口，将来若有跨域约定再补

---

## 数字汇总

| 项 | 数 |
|---|---|
| 归档条目 | 54 |
| 适配说明插入 | 5 |
| 新增 spec | 7 |
| 更新的 index.md | 6 |
| 最终有效条目 | 87 + 7 index.md = 94 文件 |
| 链接总数 / 断链 / 孤儿 | 88 / 0 / 0 |
| prd.md checkbox 项 | 92 |
| 执行耗时 | 约 1 小时（含 AI 会话交互）|

---

**结论**：Trellis 工作流已完成本项目适配，可以正常使用。
