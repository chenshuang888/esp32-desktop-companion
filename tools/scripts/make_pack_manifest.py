"""make_pack_manifest.py —— 为动态 app 包目录写 manifest.json

用法：
    python tools/scripts/make_pack_manifest.py <pack_dir> --id <id> --name <显示名>
                                                  [--icon NAME] [--color TOKEN]
                                                  [--version 1.0.0]

示例：
    python tools/scripts/make_pack_manifest.py dynamic_app/scripts/notif_pkg \
        --id notif_pkg --name 通知 --icon NOTIFICATIONS --color ACCENT

    python tools/scripts/make_pack_manifest.py dynamic_app/scripts/alarm_pkg \
        --id alarm_pkg --name 闹钟 --icon ALARM --color WARN

输出：
    <pack_dir>/manifest.json   包元信息（含 launcher 图标 codepoint 名 + 颜色名）

固件 launcher 解析 manifest.icon / iconColor 后：
    - 字段名 -> Material Symbols Rounded 字体里的 codepoint（见 ICONS 列表）
    - 颜色名 -> UI_C_* 设计 token（见 COLORS 列表）
    - 用 36px 矢量字体渲染（与原生 app launcher 完全同款）
    - 不再需要 icon.bin / icon.png；包体积更小，永远清晰
"""
from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path


# ----------------------------------------------------------------------------
# 与固件 dynamic_app/dynamic_app_registry.c::k_icon_table 对齐。
# 新增 icon 时三处都要改：
#   1. material_icons_subset.ttf 子集需包含该 codepoint（pyftsubset）
#   2. dynamic_app_registry.c::k_icon_table 加一行
#   3. 这里加一行（让脚本能校验 manifest 字段值）
# ----------------------------------------------------------------------------
ICONS = {
    "BLUETOOTH", "BT_DISABLED", "SCHEDULE", "WEATHER", "NOTIFICATIONS",
    "MUSIC", "TUNE", "SETTINGS", "BRIGHTNESS", "INFO", "EDIT_CALENDAR",
    "APPS", "CHEVRON_LEFT", "CHEVRON_RIGHT", "DOT", "DOT_SMALL",
    "ALARM", "TIMER", "STOPWATCH", "HABIT", "NOTE", "GAME", "CALCULATOR",
    "IMAGE", "MEMORY", "DASHBOARD", "PUZZLE", "TARGET", "PETS", "AQUARIUM",
    "ECHO",
}

# 与 dynamic_app_registry.c::k_color_table / ui_tokens.h 对齐
COLORS = {
    "BG", "PANEL", "PANEL_HI", "BORDER",
    "TEXT", "TEXT_DIM", "TEXT_MUTED",
    "ACCENT", "ACCENT_2", "OK", "WARN", "ERR", "INFO",
}


def main():
    ap = argparse.ArgumentParser(
        description="Write manifest.json for a dynamic app pack.")
    ap.add_argument("pack_dir", help="Path to the pack directory")
    ap.add_argument("--id",   required=True, help="App id (must match dir name)")
    ap.add_argument("--name", required=True, help="Display name (UTF-8, can be CJK)")
    ap.add_argument("--icon",  default="APPS",
                    help="Icon name (see ICONS list). Default: APPS")
    ap.add_argument("--color", default="TEXT_MUTED",
                    help="Icon color token (see COLORS list). Default: TEXT_MUTED")
    ap.add_argument("--version", default="1.0.0", help="Free-form version string")
    args = ap.parse_args()

    pack_dir = Path(args.pack_dir).resolve()
    if not pack_dir.is_dir():
        sys.exit("not a directory: {}".format(pack_dir))

    if args.icon.upper() not in ICONS:
        sys.exit("unknown --icon {!r}\navailable: {}".format(
            args.icon, ", ".join(sorted(ICONS))))
    if args.color.upper() not in COLORS:
        sys.exit("unknown --color {!r}\navailable: {}".format(
            args.color, ", ".join(sorted(COLORS))))

    manifest = {
        "id":        args.id,
        "name":      args.name,
        "icon":      args.icon.upper(),
        "iconColor": args.color.upper(),
        "version":   args.version,
    }

    out = pack_dir / "manifest.json"
    with open(out, "w", encoding="utf-8") as f:
        json.dump(manifest, f, ensure_ascii=False)
        f.write("\n")
    print("wrote {} ({}B)".format(out, out.stat().st_size))


if __name__ == "__main__":
    main()
