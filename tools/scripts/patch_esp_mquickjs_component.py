"""
为 demo6 的“动态 App（MicroQuickJS）MVP”提供一个可重复执行的 managed component 补丁脚本。

**背景**
- ESP-IDF 的 Component Manager 会把依赖下载/安装到 `managed_components/`。
- 这些目录通常被 `.gitignore` 忽略，因此我们不适合直接提交对第三方组件源码的修改。
- 解决思路：在依赖下载完成后，运行一次“幂等补丁脚本”，把必要的改动应用到本地的 managed 组件上。

**本脚本做什么（均为幂等）**
1) 修复上游 `esp.c` 缺少 `<string.h>` 导致 `strlen()` 在 `-Werror` 下编译失败的问题。
2) 在 `esp.c` / `include/esp_mqjs.h` 中导出：
   - `esp_mqjs_get_stdlib_def()`
   - `esp_mqjs_get_stdlib_c_function_count()`
   用于在运行时复制 stdlib 的 C function table，并追加 `sys.*` 等 native API。
3) 让工程侧可以直接 `#include "mquickjs.h"`：
   - 给 `managed_components/makgordon__esp-mquickjs/CMakeLists.txt` 的 `INCLUDE_DIRS` 增加 `"."`。

**使用**
在 `idf.py reconfigure` 成功下载/更新依赖后运行一次：

  python tools/scripts/patch_esp_mquickjs_component.py
"""

from __future__ import annotations

from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
COMP_DIR = ROOT / "managed_components" / "makgordon__esp-mquickjs"


def _read_text(p: Path) -> str:
    """以 UTF-8 读取文件；遇到异常字节用 replacement，保证脚本可运行并能打补丁。"""
    return p.read_text(encoding="utf-8", errors="replace")


def _write_text(p: Path, s: str) -> None:
    """以 UTF-8 写回文件，并统一使用 LF 行尾（便于 diff）。"""
    p.write_text(s, encoding="utf-8", newline="\n")


def patch_cmakelists() -> None:
    """
    补丁 3：让工程侧可直接 `#include "mquickjs.h"`。

    这是通过把组件 CMakeLists.txt 的 INCLUDE_DIRS 改成包含 "." 实现的：
    - 上游通常只有 `INCLUDE_DIRS "include"`
    - 我们把它改成 `INCLUDE_DIRS "include" "."`
    """
    p = COMP_DIR / "CMakeLists.txt"
    if not p.exists():
        raise FileNotFoundError(p)

    s = _read_text(p)
    if 'INCLUDE_DIRS "include" "."' in s:
        return

    # 简单处理：先覆盖上游常见的一行写法。
    needle = 'INCLUDE_DIRS "include")'
    if needle in s:
        _write_text(p, s.replace(needle, 'INCLUDE_DIRS "include" ".")'))
        return

    # 有些 revision 会写成多行：也做一次兜底兼容。
    needle2 = 'INCLUDE_DIRS "include"'
    if needle2 in s and 'INCLUDE_DIRS "include" "."' not in s:
        _write_text(p, s.replace(needle2, 'INCLUDE_DIRS "include" "."'))
        return

    raise RuntimeError(f"Unexpected CMakeLists.txt format: {p}")


def patch_header() -> None:
    """
    补丁 2（头文件部分）：在 esp_mqjs.h 中声明两个导出函数。

    这样工程侧就能在运行时获取 stdlib 定义与 function table 的项数，
    从而“复制 + 追加”出自己的 native API（sys.*）。
    """
    p = COMP_DIR / "include" / "esp_mqjs.h"
    if not p.exists():
        raise FileNotFoundError(p)

    s = _read_text(p)
    if "esp_mqjs_get_stdlib_def" in s and "esp_mqjs_get_stdlib_c_function_count" in s:
        return

    marker = "void esp_mqjs_run_script(const char *script);\n"
    if marker not in s:
        raise RuntimeError(f"Unexpected esp_mqjs.h format: {p}")

    insert = (
        marker
        + "\n"
        + "/* 返回 esp-mquickjs 内置 stdlib 定义（只读使用）。 */\n"
        + "const JSSTDLibraryDef *esp_mqjs_get_stdlib_def(void);\n"
        + "\n"
        + "/* 返回 stdlib 的 C function table 项数，用于运行时追加 native API。 */\n"
        + "size_t esp_mqjs_get_stdlib_c_function_count(void);\n"
    )

    _write_text(p, s.replace(marker, insert))


def _patch_esp_c_add_string_h(s: str) -> str:
    """补丁 1：给 esp.c 增加缺失的 <string.h>。"""
    if "#include <string.h>" in s:
        return s

    needle = "#include <sys/time.h>\n"
    if needle not in s:
        raise RuntimeError("esp.c missing expected include <sys/time.h>")

    return s.replace(needle, needle + "#include <string.h>\n")


def _patch_esp_c_append_exports(s: str) -> str:
    """补丁 2（源文件部分）：在 esp.c 末尾追加两个导出函数的实现。"""
    if "esp_mqjs_get_stdlib_def" in s and "esp_mqjs_get_stdlib_c_function_count" in s:
        return s

    append = (
        "\n"
        "const JSSTDLibraryDef *esp_mqjs_get_stdlib_def(void)\n"
        "{\n"
        "    return &js_stdlib;\n"
        "}\n"
        "\n"
        "size_t esp_mqjs_get_stdlib_c_function_count(void)\n"
        "{\n"
        "    return sizeof(js_c_function_table) / sizeof(js_c_function_table[0]);\n"
        "}\n"
    )
    return s.rstrip() + append


def patch_esp_c() -> None:
    """对 managed 组件的 esp.c 应用补丁（1 + 2）。"""
    p = COMP_DIR / "esp.c"
    if not p.exists():
        raise FileNotFoundError(p)

    s = _read_text(p)
    s2 = _patch_esp_c_add_string_h(s)
    s3 = _patch_esp_c_append_exports(s2)
    if s3 != s:
        _write_text(p, s3)


def main() -> None:
    if not COMP_DIR.exists():
        raise SystemExit(
            f"未找到组件目录：{COMP_DIR}\n"
            "请先运行：idf.py reconfigure（确保依赖已下载到 managed_components/）"
        )

    patch_cmakelists()
    patch_header()
    patch_esp_c()
    print("OK: 已应用 esp-mquickjs 补丁（幂等）")


if __name__ == "__main__":
    main()

