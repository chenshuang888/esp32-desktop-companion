# control_service 退役与 Spec 二次筛选日志

**日期：** 2026-04-21
**分支：** `more_reconstruct`
**涉及任务：** `04-21-retire-control-service` + `04-21-spec-second-pass-filter`（均已归档到 `archive/2026-04/`）

## 背景 / 这一轮想收掉的东西

本项目到目前为止经历了三轮 BLE "反向通道"设计演进：

| 阶段 | 设计 | 状态 |
|---|---|---|
| v1 | `control_service` 单通道承载 `type=BUTTON` + `type=REQUEST`（time/weather/system）| 上一轮（`反向请求模式重构日志.md`）已拆 |
| v2 | REQUEST 拆回各业务 service 自管 NOTIFY，`control_service` 只剩 BUTTON（lock/mute/prev/next/pp 5 键） | 撕裂残留 |
| **v3（本次）** | **`control_service` 整体退役**。lock/mute 放弃；媒体键按同一"触发端同 service"原则迁到 `media_service` 新 NOTIFY char | **当前** |

触发这一轮的直接原因是一句用户判断："**目前这部分没什么用**"（指 lock/mute 功能）。顺着"触发端与响应端必须同在一个 service"原则，prev/next/pp 自然该去 `media_service`，整个 control 通道就成了空壳。

然后紧接着做的是 **Spec 二次筛选**：第一轮 bootstrap Fit Check 归档了 54 条"场景无关"条目，剩下 87 条里仍有大量通用 IoT 经验并非本项目硬规则。把它们分层到 `_general_library/`，主目录精简到 29 条真正每次改动都要查的"硬规则"。

---

## Part 1：control_service 退役（Task `04-21-retire-control-service`）

### 方向确定过程（用户连改 3 次方向）

这个任务 PRD 重写了 3 次：

1. **最初方案（`media-button-channel-split`）**：page_music 3 个媒体键迁 media_service，page_control 保留 lock/mute
2. **转向一**：page_control "目前没什么用，删掉 + control_service 顺便去掉"
3. **定稿（`retire-control-service`）**：保留媒体键功能（迁 media_service）+ 彻底删 control_service + page_control + control_panel_client.py

方向变化的"不可逆决策点"——"媒体键要不要保留？保留的话新 payload 怎么设计？"——在开工前用 `AskUserQuestion` 锁定，避免写一半返工。

### 6 阶段落地

按 PRD 分 6 阶段，连续跑完，最后一次验收（本次学到：之前每 Phase 停下来等用户验证效率很低，用户明确反馈"一次性跑完再喊"）：

| Phase | 内容 | 关键输出 |
|---|---|---|
| 1 | ESP 侧加 media notify char | UUID `0x8a5c000d`；`media_button_event_t` (4B)；`media_service_send_button()` |
| 2 | PC 端对齐新通道 | `MEDIA_BUTTON_CHAR_UUID` 常量 + `MediaButtonHandler` 类 + run_session 加 `start_notify` |
| 3 | `page_music.c` 切换 | include 换 `media_service.h`；调用换 `media_service_send_button`；id 常量 2/3/4 → 0/1/2 |
| 4 | 删 `page_control` | 物理删 `.c/.h` + CMakeLists + app_main + page_router 枚举 + page_menu 入口 |
| 5 | 删 `control_service` + 下线 ControlHandler | 物理删 `.c/.h` + `control_panel_client.py` + desktop_companion 里整个 ControlHandler 类 |
| 6 | Spec 归档 + README 更新 | 归档 2 条 spec；改 5 条；UUID `0x8a5c0005/6` 标 RETIRED |

### 关键技术决策

| 决策点 | 选择 | 理由 |
|---|---|---|
| `media_service` NOTIFY payload | 新定义 4B `media_button_event_t`（`<BBH`）| 不复用即将删除的 `control_event_t`（8B）；精简 |
| 按钮 id 编号 | media 从 0 开始（0=prev / 1=pp / 2=next）| 新 service 独立语义，不沿用旧 2/3/4 |
| UUID 分配 | 末尾追加 `0x8a5c000d` | 复用上轮确立的"末尾追加例外"规则 |
| 退役 UUID | 表格中保留行 + 标 RETIRED | 审计可追 + 防未来重用踩坑 |

### 数字汇总

| 项 | 数 |
|---|---|
| 删除文件 | 5（page_control.c/h + control_service.c/h + control_panel_client.py） |
| 净减代码行 | ~700 |
| 新增代码行 | ~80（media notify char + send_button + PC handler） |
| 修改的 ESP 源文件 | 9 |
| 修改的 PC 源文件 | 1（desktop_companion.py）|
| 归档的 spec | 2 |
| 修改的 spec | 5 + 2 个 index.md + README |
| 退役 UUID | 2（`0x8a5c0005/6`）|
| 新增 UUID | 1（`0x8a5c000d`）|

### 一句话总结

**同一个"触发端与响应端同 service"原则，第二次产生推论**——既然 REQUEST 拆回各业务 service 成立，那 BUTTON 也该拆。加上 lock/mute 本身没人用，就演成 `control_service` 整体下线。这是"架构原则一致性"的自然收束。

---

## Part 2：Spec 二次筛选与清漆（Task `04-21-spec-second-pass-filter`）

### 触发原因

control_service 任务做完后用户问："**spec 部分和这个项目的适配度怎么样？**"

我实话说：
- 本项目功能迭代中**真正被引用的 spec 只有 3-5 条**（UUID DR、反向请求 playbook、UI 线程契约、N16R8 板型、sdkconfig regen）
- 其余 80+ 条里大量是通用 IoT 经验（任何 ESP-IDF 项目都适用）或已和代码漂移
- 87 条平铺导致检索成本高，AI 主要靠 `grep` 找，不靠浏览

用户顺着："**可以二次筛选与改造**"。

### 分类标准（本轮与上轮的根本差异）

| 轮次 | 标准 | 性质 |
|---|---|---|
| 第一轮（bootstrap Fit Check） | 场景是否相关（STM32 / ASR-Pro / KVM → 无关归档） | **相关性过滤** |
| **本轮** | 是否本项目"每次改动都要查"的硬规则 | **使用频率分层** |

本轮分四类：
- **✅ 保留**：引用本项目代码路径 / 描述本项目独有决策 / 每次改动都要查
- **📦 归档到 `_general_library/`**：通用 IoT 经验，换项目也用得上
- **⚠️ 漂移**：内容和当前代码不一致，修复 ≤30 行 diff 则就地修，否则归档
- **🔄 重复**：实际没发现重复对

### 执行（Phase 1 踩到 Agent panic）

Phase 1 本打算派 3 个并行 Agent 分目录盘点，结果都撞 API 500 panic（"nil pointer dereference"），转为手动 grep 扫特征信号 + 对边缘案例 Read 前 30 行判断。

**特征信号法**：
- 强绑定信号：`services/*_service.c`、`drivers/*.c`、`app/pages/`、`8a5c00`、`N16R8`、`ST7789`、`FT5x06`、`Tiny TTF` 等
- 漂移信号：`control_service`、`page_control`、`EVENT_TYPE_BUTTON`、`VK_VOLUME_MUTE`
- 按命中次数 count 粗分：>=5 保留、3-4 大概率保留、1-2 边缘读原文判、0 归档

### 停手点（触发 PRD 规则）

初筛结果保留 29 条 > PRD 写的 25 条上限，按停手规则停下来让用户确认。用户选"接受 29 条"——比我预估的 ~15 条多一倍，但每条都有保留理由：

- **硬核 19 条**（砍不掉）：protocol 6 BLE 契约 + host-tools 4 + guides 4 元规则 + firmware 5 NVS/notify/ble_conn
- **弹性 10 条**：N16R8 硬件踩坑、LVGL 配置踩坑、port 分层、font 三条等

### 执行后自查发现 2 条错杀

归档完以后用户追问："**留下的是否真的重要，去掉的里面是否有可以留下的，比如架构这种层级的东西？**"

我 Read 核对了几条被归档的"架构类"条目，承认错杀：

| 文件 | 错杀原因 |
|---|---|
| `lvgl-port-layering-decision-record.md` | 读完发现方案 B（两层）就是本项目 `drivers/lcd_panel + touch_ft5x06 + lvgl_port + board_config.h` 的决策依据。我保留了 playbook 但砍了 DR，判断失衡。|
| `freertos-task-architecture-smell-checklist.md` | 任务优先级/栈/同步的 smell 清单，本项目每次加 service/task 该查。内容硬核，不该归档。|

同时反向审视保留项有 2 条水分：
- `customtkinter-sidebar-navigation-playbook.md`（侧边栏已定型，不会常改）
- `utf8-fixed-size-truncate-pitfall.md`（偶尔遇到，不是硬规则）

做了 4 次 mv：挪回 2 + 挪出 2。最终数字仍是 29/56，但含金量更高。

### 新目录结构

```
.trellis/spec/
├── iot/                             29 条硬规则 + 7 个 index
│   ├── firmware/          10 条     N16R8 硬件、NVS、本项目模块、freertos smell
│   ├── device-ui/          7 条     40 行 DR、port 分层 DR+playbook、Tiny TTF、page_router
│   ├── protocol/           6 条     5 个 BLE service 契约 + UUID DR + 反向请求
│   ├── host-tools/         2 条     desktop_companion + SMTC
│   ├── guides/             4 条     Fit Check + 防复发 + UI/NimBLE 线程契约
│   ├── ota/                0 条     本项目未实现
│   └── shared/             0 条     空壳
├── _general_library/                56 条通用经验 + README
│   ├── firmware/          24 条
│   ├── device-ui/         20 条
│   ├── guides/            10 条
│   ├── host-tools/         1 条
│   └── ota/                1 条
└── _archived_unrelated/2026-04-21/  第一轮 54 条 + 本轮新归档 2 条
```

### 数字汇总

| 项 | 数 |
|---|---|
| 原始实质条目 | 85 |
| 保留在 `iot/` | 29 |
| 归档到 `_general_library/` | 56 |
| 新建 `_general_library/README.md` | 1 |
| 重写的 index.md | 7（6 子目录 + iot 顶层）|
| 漂移修复 | 1 处（desktop-companion-bleak-multiplex-playbook.md "避免 Lock/Mute 等动作" 改为"避免媒体键误触"） |
| 断链 / 孤儿 | 0 / 0 |

### 反思

1. **Spec 知识库不是"越多越好"**。87 条平铺 AI 根本不浏览，只 grep 高频关键词。结构化索引（"改什么→看哪条"）比堆条目有效。
2. **三级分类比二级好**：`iot/`（硬规则，每次查）/ `_general_library/`（通用经验，换项目捡回）/ `_archived_unrelated/`（场景无关，理论永远不用）语义分离，不互相污染。
3. **sanity check 必做**：激进归档后**一定要再读几条"架构类"归档的原文**核对，否则容易把"标题通用但内容本项目强绑定"的条目误杀。本轮就挪回了 2 条。
4. **内容漂移是隐性债**：本轮只修了 1 处明显漂移，但保留的 29 条里可能还有细微漂移（例如某行提到的代码路径已经改了）。**建议半年一次 spec 审计**。

---

## 协作规则沉淀（本轮新增 memory）

这一轮在用户持续反馈下新增两条全局 feedback memory（写入 `~/.claude/projects/.../memory/`）：

| 规则 | 触发点 |
|---|---|
| `feedback_no_build_by_ai` | Phase 1 结束我习惯性跑 `idf.py build` 被用户打断："编译你不要做，你这边做不了" |
| `feedback_batch_phases_not_per_step` | 整个 6 阶段做完后用户说："以后这样的任务可以一次性做完 5 个阶段再喊我编译，有问题再解决" |

从这两条规则推出的新协作默认值：
1. 代码改动连续做完再一次性汇报，**不要每个 Phase 停顿**
2. **编译验证交给用户**，我只保证写到"理论可编译"的状态
3. 停下来询问的时机只剩：**不可逆决策点 / PRD 未预料的歧义**

---

## 两任务合并视角

这一天连做两件看似无关的事（代码重构 + 文档清理），但内在逻辑一致：

> **"每次改动都应该把无用的东西一并清掉，避免 debt 持续累积。"**

control_service 是代码层面的 debt（功能没人用还占 2 个 UUID）；87 条 spec 是文档层面的 debt（每条都"可能有用"但实际不查）。都靠"是否每次都会用到"这个标准筛掉。

---

## 数字合计

| 项 | 数 |
|---|---|
| 删除代码文件 | 5 |
| 删除/归档 spec | 58（本次 2 + Part 2 的 56）|
| 退役 UUID | 2 |
| 新增 UUID | 1 |
| 代码净减 | ~620 行 |
| spec `iot/` 精简 | 87 → 29（67% 减） |
| 新增全局 memory | 2 条 |
| auto-commit 任务归档 | 2 次 |

**执行耗时**（含讨论决策）：约 2.5 小时 AI 会话交互。
