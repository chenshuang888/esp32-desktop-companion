# 动态 App（MicroQuickJS）模块重构：分层拆分工作日志

日期：2026-04-24

## 背景

完成 Layer1 控件 + Layer2 onClick 之后，dynamic_app 模块功能上已能 1:1 复刻菜单页 + 处理点击。但代码组织上出现了明显问题：

- `dynamic_app.c` 膨胀到 **948 行**，里面同时存在生命周期、JS 引擎管理、native fn 实现、tick 主循环
- `dynamic_app_ui.c` 膨胀到 **555 行**，里面同时存在队列管理、UTF-8 helper、registry CRUD、9-key 样式分发、drain 主调度
- 打开任意文件都不能在 5 秒内回答"它分几块、每块干啥"
- 加新功能时不知道该加在哪段，文件熵会持续上升

用户原话："看起来很乱，但又说不清，就是做不到一眼望过去逻辑层次很清晰的地步"。这是典型的**认知负荷过高**症状——单文件涵盖了太多抽象层级。

## 目标

1. **按职责分层拆文件**：每个 .c 文件只承担一个"层"的职责，文件名能直接表达"它管什么"
2. **每个文件加结构化注释**：顶部目录、§1/§2/§3 章节锚点
3. **不改变任何运行时行为**：纯重排，CMake 重新编译后功能与重构前 1:1 一致
4. **降低未来扩展成本**：加新 API（如 sys.ble.xxx）时只动一个固定位置

## 方案与实现

### 1) 分层架构

把"两个大文件"拆成"两组三层文件 + 两个内部头":

```
JS 侧（3 层）：
  dynamic_app.c            控制层  生命周期 + script_task 主循环
       ↓
  dynamic_app_runtime.c    引擎层  JSContext 创建/销毁/bind/eval
       ↓
  dynamic_app_natives.c    API 层  所有 sys.* native fn + tick 服务
                                   + cfunc 注册 + 全局对象绑定

UI 侧（3 层）：
  dynamic_app_ui.c            调度层    drain 主分发 + 入队 API + 生命周期
       ↓
  dynamic_app_ui_registry.c   注册表层  UTF-8 截断 + id↔obj 映射
       ↓
  dynamic_app_ui_styles.c     样式层    apply_style 9-key 分发器

内部共享（仅模块内部 include）：
  dynamic_app_internal.h     JS 侧三方共享：runtime 结构 + 跨文件函数原型
  dynamic_app_ui_internal.h  UI 侧三方共享：registry struct + 全局变量 extern

对外公共 API（不变）：
  dynamic_app.h
  dynamic_app_ui.h
```

### 2) 拆分原则

每个 .c 文件遵循三条铁律：

- **单一职责**：一个文件只解决一个抽象层的问题
- **不跨层调用**：上层调下层，绝不反向（runtime 调 natives 的 register 是个允许的例外，因为这是依赖注入）
- **顶部强制注释**：包含"职责 / 不做的事 / 文件目录"三块，给读者锚点

### 3) 内部头的引入

新增两个不对外暴露的头文件 `*_internal.h`：

- 解决"三个 .c 需要共享同一个 s_rt 全局变量"的问题
- 解决"natives.c 提供的 register 函数需要被 runtime.c 调用"的问题
- 同时保护：外部模块（page、app 层）只 include `dynamic_app.h` / `dynamic_app_ui.h`，看不到内部细节

CMake 没有"private header"机制，靠 include 约定保证：内部头不在公共 .h 里被 include。

### 4) 内部 helper 提升为跨文件函数

原本 `dynamic_app.c` 里几个 `static` helper（now_ms / dump_exception / interval/handler reset）拆分后被多个文件需要，要去掉 `static` 并加 `dynamic_app_` 前缀变成跨文件 API：

| 原来 | 现在 |
|---|---|
| `static int64_t now_ms()` | `int64_t dynamic_app_now_ms()` |
| `static void dump_exception()` | `void dynamic_app_dump_exception()` |
| `static void intervals_reset()` | `void dynamic_app_intervals_reset()` |
| `static void click_handlers_reset()` | `void dynamic_app_click_handlers_reset()` |
| `static bool run_intervals_once()` | `bool dynamic_app_run_intervals_once()` |
| `static void drain_ui_events_once()` | `void dynamic_app_drain_ui_events_once()` |
| `static int64_t get_next_interval_deadline_ms()` | `int64_t dynamic_app_next_interval_deadline_ms()` |
| `static esp_err_t setup_stdlib_and_context()` | `esp_err_t dynamic_app_runtime_setup()` |
| `static void teardown_context()` | `void dynamic_app_runtime_teardown()` |
| `static esp_err_t bind_sys_and_timers()` | `esp_err_t dynamic_app_runtime_bind_globals()` |
| `static esp_err_t eval_embedded_app()` | `esp_err_t dynamic_app_runtime_eval_app()` |

UI 侧同理：

| 原来 | 现在 |
|---|---|
| `static int registry_find()` | `int registry_find()` |
| `static int registry_alloc()` | `int registry_alloc()` |
| `static lv_obj_t *resolve_parent()` | `lv_obj_t *resolve_parent()` |
| `static void utf8_copy_trunc()` | `void utf8_copy_trunc()` |
| `static void apply_style()` | `void apply_style()` |
| `static const lv_align_t k_align_map[]` | 仍 static（仅 styles.c 内用） |
| `static lv_coord_t resolve_size()` | 仍 static（仅 styles.c 内用） |
| `static const lv_font_t *resolve_font()` | 仍 static（仅 styles.c 内用） |

### 5) 全局共享变量从 static 提升为 extern

UI 侧原本所有共享状态都在 `dynamic_app_ui.c` 里 static：

```c
// 原来
static QueueHandle_t s_ui_queue;
static volatile lv_obj_t *s_root;
static ui_registry_entry_t s_registry[...];
static const lv_font_t *s_font_text;
```

拆分后 registry.c 和 styles.c 都需要读 `s_registry`、`s_root`、`s_font_*`，所以提升为模块级全局：

```c
// dynamic_app_ui.c 里实际定义
QueueHandle_t s_ui_queue = NULL;
volatile lv_obj_t *s_root = NULL;
ui_registry_entry_t s_registry[...];
const lv_font_t *s_font_text = NULL;

// dynamic_app_ui_internal.h 里 extern 声明
extern QueueHandle_t s_ui_queue;
extern volatile lv_obj_t *s_root;
extern ui_registry_entry_t s_registry[DYNAMIC_APP_UI_REGISTRY_MAX];
extern const lv_font_t *s_font_text;
```

这是受控的"模块内全局"，比"跨模块全局"安全得多——只有模块内部三个文件能看到。

### 6) 用宏减少重复 boilerplate

`natives.c` 注册 11 个 cfunc 时，如果每个都展开会写 5 行 × 11 = 55 行近乎相同的代码。引入 `DEF_CFN` 宏：

```c
#define DEF_CFN(idx_field, fn, argn) \
    s_rt.cfunc_table[s_rt.idx_field] = (JSCFunctionDef){ \
        .func.generic = (fn), \
        .name = JS_UNDEFINED, \
        .def_type = JS_CFUNC_generic, \
        .arg_count = (argn), \
        .magic = 0, \
    }

DEF_CFN(func_idx_sys_log,              js_sys_log,              1);
DEF_CFN(func_idx_sys_ui_set_text,      js_sys_ui_set_text,      2);
DEF_CFN(func_idx_sys_ui_create_label,  js_sys_ui_create_label,  2);
// ... 11 行齐整对齐
```

55 行收缩到 11 行，且加新 fn 时一眼能看出"参数个数对不对"。bind 函数同理用 `BIND_FN` 宏。

宏只在定义/使用它的函数内有效，函数末尾立刻 `#undef`，避免污染其它文件。

### 7) createX widget 的公共骨架抽取

原来 `js_sys_ui_create_label/createPanel/createButton` 三个 native fn 95% 代码相同：

```c
// 原来：3 份几乎一样的代码
static JSValue js_sys_ui_create_panel(...) {
    if (argc < 1) return JS_ThrowTypeError(...);
    取 id;
    取 parent;
    return JS_NewBool(dynamic_app_ui_enqueue_create_panel(...));
}
```

抽出公共骨架 `js_create_widget_common`，把"调哪个 enqueue"作为函数指针传入：

```c
typedef bool (*enqueue_create_fn_t)(const char *, size_t, const char *, size_t);

static JSValue js_create_widget_common(JSContext *ctx, int argc, JSValue *argv,
                                       const char *fn_name,
                                       enqueue_create_fn_t enq);

// 三个壳子各自只剩 4 行：
static JSValue js_sys_ui_create_label(...) {
    return js_create_widget_common(ctx, argc, argv,
        "sys.ui.createLabel(id, parent?) args missing",
        dynamic_app_ui_enqueue_create_label);
}
```

3 × 25 行 → 1 × 25 行 + 3 × 5 行壳子。加第 4 个 widget 类型只要再加 5 行壳子。

## 改动文件清单

| 文件 | 类型 | 说明 |
|---|---|---|
| `dynamic_app_internal.h` | **新增** | JS 侧三方共享：runtime struct + 11 个 cfunc 索引字段 + 跨文件函数原型 |
| `dynamic_app_ui_internal.h` | **新增** | UI 侧三方共享：registry struct + 全局变量 extern + 跨文件函数原型 |
| `dynamic_app_runtime.c` | **新增** | 引擎层：setup/teardown/bind/eval。198 行 |
| `dynamic_app_natives.c` | **新增** | API 层：11 个 native fn + cfunc 注册 + 全局对象绑定 + tick 服务。573 行 |
| `dynamic_app_ui_registry.c` | **新增** | 注册表层：UTF-8 helper + id↔obj 增删查。149 行 |
| `dynamic_app_ui_styles.c` | **新增** | 样式层：apply_style 9-key 分发 + align_map / resolve_size / resolve_font。152 行 |
| `dynamic_app.c` | **重写** | 控制层：定义 s_rt 全局 + script_task 主循环 + 公共 API。948 行 → 198 行 |
| `dynamic_app_ui.c` | **重写** | 调度层：drain 主分发 + 入队 API + 生命周期。555 行 → 390 行 |
| `dynamic_app.h` | 不变 | 对外接口 3 个函数 |
| `dynamic_app_ui.h` | 不变 | 对外接口 |
| `CMakeLists.txt` | **更新** | SRCS 加 4 个新文件，加分组注释 |

总行数从 1503（原 2 个 .c）→ 1660（6 个 .c），增加约 10%，全部是结构化注释 + 章节横幅。

## 重构前后对比

### 文件视图

```
重构前：
  dynamic_app.c     948 行   ← 啥都有
  dynamic_app_ui.c  555 行   ← 啥都有

重构后：
  dynamic_app.c              198 行
  dynamic_app_runtime.c      198 行
  dynamic_app_natives.c      573 行
  dynamic_app_ui.c           390 行
  dynamic_app_ui_registry.c  149 行
  dynamic_app_ui_styles.c    152 行
  dynamic_app_internal.h     143 行
  dynamic_app_ui_internal.h   95 行
```

### 阅读体验

打开 `dynamic_app.c`：

- 重构前：滚到 540 行才看到 script_task；看到一半发现里面调的 `setup_stdlib_and_context` 在 280 行，又得跳回去
- 重构后：198 行全是控制流；看到 `dynamic_app_runtime_setup` 一行调用就知道"这是引擎层的事"，要读细节直接打开 `runtime.c` 不会被打断

### 加新功能时的改动点

加一个新 native fn `sys.ui.setProgress(id, percent)`：

- 重构前：要在 `dynamic_app.c` **同一个文件**的 5 个不同位置加代码（typedef、setup_stdlib、bind、struct 字段、native fn 本体），新手很容易漏一个
- 重构后：
  - `dynamic_app_internal.h` 加一行 `func_idx_sys_ui_set_progress`
  - `dynamic_app_natives.c` §3 加 native fn 实现
  - `dynamic_app_natives.c` §7 加一行 `DEF_CFN(...)`
  - `dynamic_app_natives.c` §8 加一行 `BIND_FN(ui, "setProgress", ...)`
  - `dynamic_app_ui.h` 加 cmd enum
  - `dynamic_app_ui.c` §6 drain 加 case
  - `dynamic_app_ui_styles.c` 不动（只动样式时才改）

每个改动点都有明确的章节锚点，不会"找错地方"。

## 验证

### 静态校验（已做）

- ✅ `s_rt` 在 `dynamic_app.c` 唯一定义，`dynamic_app_internal.h` extern 声明
- ✅ UI 侧 7 个共享变量全部 1:1 对应（定义 ↔ extern）
- ✅ 所有跨文件函数无重复定义（grep 校验通过）
- ✅ 6 个 .c 的 include 都正确包含 internal header 或公共 header
- ✅ CMakeLists 的 SRCS 列出全部 6 个 .c

### 行为校验（待用户）

- [ ] `idf.py build` 编译通过
- [ ] 烧录后菜单页 → Dynamic App，列表 7 项正常显示
- [ ] 点击任一项串口出 `script: click <id>` 日志
- [ ] 返回菜单再进入，不出现内存泄漏 / registry 残留

## 经验沉淀

### "看起来乱"≠"逻辑乱"，是分层混淆

最初代码并不"差"，每个函数都对，但所有抽象层（操作系统级 / 引擎级 / 业务级）混在一个文件里。
**人脑读代码时是按"层"切换上下文的**，文件无层就需要不断切换，认知负荷成倍上升。

诊断信号：
- 一个文件里同时出现 `xTaskCreate` + `JS_NewContext` + `lv_obj_create` —— 三个不同抽象层
- 函数命名前缀不一致：`setup_xxx` / `js_sys_xxx` / `now_ms` —— 没有统一的命名空间
- 打开后必须用编辑器搜索功能才能跳转，不能靠"我大概记得在哪一节"

### 拆分时机

**功能稳定 + 加新功能开始痛**就是该拆的信号。本次重构选在 Layer2 onClick 完成、确认功能能用之后立刻做，而不是等到第三次扩展时——那时改动会更难，因为已经形成的习惯路径都还围绕"大文件"走。

### 内部头是模块化的关键

C 没有 module 概念，模块化全靠"哪些 .h 给外人看，哪些只给自己人看"的约定。
本次引入两个 `*_internal.h`，让外部 page 层只能看到对外 API，看不到 s_rt / registry struct 这些实现细节——和 Java 的 `package-private` 等价。

### 用宏要克制

`DEF_CFN` / `BIND_FN` 都只在单个函数内使用 + 立刻 `#undef`。这是 C 里宏的安全用法——它解决的是"重复 boilerplate"，不是"通用工具"。
通用工具用 inline 函数；boilerplate 才用宏。

### 重构 ≠ 改行为

整个重构没动一行运行时逻辑。要确保这点，唯一办法是**先备份能跑起来的版本**，重构后先跑一次回归。本次重构前的状态在最后一次成功烧录的固件里——如果烧录新版后行为不对，能立刻回退对比。
