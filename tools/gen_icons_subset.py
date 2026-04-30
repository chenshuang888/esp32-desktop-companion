#!/usr/bin/env python3
"""
生成 Material Symbols Rounded 的图标子集，供 LVGL Tiny TTF 渲染使用。

依赖:
    pip install fonttools

流程:
    1) 把 MaterialSymbolsRounded.ttf 放到 app/fonts/
    2) 运行: python tools/gen_icons_subset.py
    3) 产物: app/fonts/material_icons_subset.ttf  (~15-25 KB)
       由 app/CMakeLists.txt 的 EMBED_FILES 编入固件

挑选的图标对应 demo6 当前菜单 / 状态栏 需求；新增图标在 ICONS 字典里加 codepoint，
重新跑此脚本即可。
"""
import subprocess
import sys
from pathlib import Path

ROOT     = Path(__file__).parent.parent.resolve()
FONT_DIR = ROOT / "app" / "fonts"
SRC      = FONT_DIR / "MaterialSymbolsRounded.ttf"
DST      = FONT_DIR / "material_icons_subset.ttf"

# ---- 图标清单 ----
# 命名跟 Material Symbols 官网一致: https://fonts.google.com/icons
# 找新图标方法：在网站搜，右下"Code point"栏的值就是这里的数字
ICONS = {
    # 状态栏 ----------------------------------------------------------------
    "bluetooth":            0xE1A7,   # 蓝牙（已连接，亮色显示）
    "bluetooth_disabled":   0xE1A8,   # 蓝牙断开（灰色）
    "battery_full":         0xE1A4,   # 满电（>80%）
    "battery_5_bar":        0xEBDD,   # 5 格（60-80%）
    "battery_3_bar":        0xEBE0,   # 3 格（30-60%）
    "battery_1_bar":        0xEBDC,   # 1 格（<30%）

    # 九宫格 app 图标 ------------------------------------------------------
    "schedule":             0xE8B5,   # 时钟 → 时间页
    "partly_cloudy_day":    0xF172,   # 多云 → 天气页（图标按 weather_code 动态切的另说）
    "notifications":        0xE7F4,   # 铃铛 → 通知
    "music_note":           0xE405,   # 音符 → 音乐
    "tune":                 0xE429,   # 滑块 → 系统设置
    "settings":             0xE8B8,   # 齿轮 → 设置 app
    "brightness_5":         0xE1A9,   # 太阳 → 背光亮度
    "info":                 0xE88E,   # ⓘ → 关于
    "edit_calendar":        0xE556,   # 日历笔 → 时间调整
    "apps":                 0xE5C3,   # ⊟ → 动态 app 通用 fallback

    # 翻页 / 通用 ----------------------------------------------------------
    "chevron_left":         0xE5CB,
    "chevron_right":        0xE5CC,
    "circle":               0xEF4A,   # 实心圆点（分页指示 active）
    "fiber_manual_record":  0xE061,   # 实心小圆点（分页指示备选）
}


def main() -> None:
    if not SRC.exists():
        print(f"[ERR] 缺少源字体: {SRC}", file=sys.stderr)
        print("请把 MaterialSymbolsRounded.ttf 放到 app/fonts/", file=sys.stderr)
        sys.exit(1)

    src_size = SRC.stat().st_size
    print(f"[INFO] 源: {SRC.name} ({src_size / 1024 / 1024:.1f} MB)")
    print(f"[INFO] 子集图标数: {len(ICONS)}")

    # 拼出 fontTools 需要的 unicodes 参数: U+E1A7,U+E1A8,...
    unicodes = sorted(set(ICONS.values()))
    arg_unicodes = ",".join(f"U+{u:04X}" for u in unicodes)

    cmd = [
        sys.executable, "-m", "fontTools.subset",
        str(SRC),
        f"--unicodes={arg_unicodes}",
        f"--output-file={DST}",
        "--no-layout-closure",
        # Material Symbols 是 variable font，剥掉 Variations 表大幅减体积
        "--drop-tables+=DSIG,vhea,vmtx,FFTM,STAT,fvar,gvar,HVAR,MVAR,VVAR,avar",
    ]
    print("[INFO] 子集化中...")
    subprocess.check_call(cmd)

    dst_size = DST.stat().st_size
    print(f"[OK]  产物: {DST.name} ({dst_size / 1024:.1f} KB, "
          f"压缩比 {src_size / dst_size:.0f}×)")
    print()
    print("==== 在 C 代码中如何引用 ====")
    print("  app_fonts_init() 里加 lv_tiny_ttf_create_data_ex(...) 创建 lv_font_t*")
    print("  作为 fallback 挂到现有字体链")
    print("  使用: lv_label_set_text(lbl, \"\\xEE\\x86\\x85\")  // U+E185 等")
    print()
    print("==== 图标 codepoint 速查 ====")
    for name, cp in ICONS.items():
        utf8 = chr(cp).encode("utf-8")
        utf8_str = "".join(f"\\x{b:02X}" for b in utf8)
        print(f"  {name:24s} U+{cp:04X}  \"{utf8_str}\"")


if __name__ == "__main__":
    main()
