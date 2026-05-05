# 动态 App（MicroQuickJS）MVP：构建/依赖说明

本说明用于避免出现 “`FAILED: build.ninja` + `cmake --regenerate-during-build` 退出” 这种**只有总括信息、缺少根因**的构建失败。

## 1) 必须声明 managed component 依赖

本 MVP 依赖 `makgordon/esp-mquickjs`（MicroQuickJS on ESP-IDF）。

请确保已在 `main/idf_component.yml` 的 `dependencies` 中声明：

```yml
dependencies:
  makgordon/esp-mquickjs:
    version: "^0.2.1"
```

否则当 `dynamic_app` 组件在 `CMakeLists.txt` 中 `REQUIRES esp-mquickjs` 时，CMake 在 reconfigure 阶段可能找不到该组件，最终表现为 `build.ninja` regenerate 失败。

## 2) 重新生成 build 配置

在修改 `main/idf_component.yml` 后，请运行一次：

```bash
idf.py reconfigure
```

它会刷新 `dependencies.lock` 并确保 `managed_components/` 中的依赖处于可被构建系统识别的状态。

## 3) 组件补丁（当前 MVP 需要）

本 MVP 为了追加 `sys.*` 的 native API，会对 `managed_components/makgordon__esp-mquickjs` 做轻量补丁（导出 stdlib table 等）。

另外，`makgordon/esp-mquickjs@0.2.1` 在当前编译选项（`-Werror`）下会因为 `esp.c` 缺少 `#include <string.h>` 而报 `strlen()` 隐式声明错误，本补丁也会顺带修复这一点。

如果你执行 `idf.py reconfigure` 后发现 managed component 被重新拉取/覆盖，请再运行一次：

```bash
python tools/patch_esp_mquickjs_component.py
```

## 4) 常见链接错误：embedded 脚本符号名不匹配

如果出现类似：

- `undefined reference to _binary_scripts_app_js_start/_end`

请注意：ESP-IDF 的 `EMBED_TXTFILES` 生成的符号名通常基于“文件名”而不是“相对路径”。

当前 `dynamic_app/CMakeLists.txt` 里是 `EMBED_TXTFILES "scripts/app.js"`，实际符号是：

- `_binary_app_js_start`
- `_binary_app_js_end`

对应修复点在：`dynamic_app/dynamic_app.c` 的 `extern ... asm("_binary_...")` 声明。

## 5) MicroQuickJS 限制：Date 构造器不可用

在当前 `esp-mquickjs` 的 stdlib 中，`Date` 仅支持 `Date.now()`，使用 `new Date()` 会触发类似错误：

- `TypeError: only Date.now() is supported`

为保证示例 App 可运行，本 MVP 提供了 `sys.time.uptimeStr()` / `sys.time.uptimeMs()`，建议优先用它们做“时钟/定时显示”。
