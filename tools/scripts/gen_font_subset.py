#!/usr/bin/env python3
"""
生成 GB2312 全集子集的中文 TTF，供 LVGL Tiny TTF 嵌入渲染使用。

依赖:
    pip install fonttools

流程:
    1) 把一个中文 TTF 原始文件放到 app/fonts/ 下（任意文件名，只要扩展名 .ttf）。
       必须是 TrueType (.ttf)，不能是 OpenType CFF (.otf)—— Tiny TTF 底层的
       stb_truetype 只支持 TrueType glyph outlines。
    2) 运行: python tools/scripts/gen_font_subset.py
    3) 产物: app/fonts/srhs_sc_subset.ttf  (~1.2 - 1.5 MB)
       该文件由 app/CMakeLists.txt 的 EMBED_FILES 编入固件。

推荐字体来源（均为可商用 TTF）:
    - 霞鹜文楷      : https://github.com/lxgw/LxgwWenKai/releases
    - Noto Sans SC : https://fonts.google.com/noto/specimen/Noto+Sans+SC
    - 阿里巴巴普惠体: https://fonts.alibabagroup.com/
"""
import subprocess
import sys
from pathlib import Path

ROOT     = Path(__file__).resolve().parents[2]
FONT_DIR = ROOT / "app" / "fonts"
DST      = FONT_DIR / "srhs_sc_subset.ttf"
CHARS_TXT = ROOT / "tools" / "scripts" / "_subset_chars.txt"


def find_source_ttf() -> Path:
    FONT_DIR.mkdir(parents=True, exist_ok=True)
    candidates = [p for p in FONT_DIR.glob("*.ttf") if p.name != DST.name]
    if not candidates:
        print(f"[ERR] 未在 {FONT_DIR} 找到源 TTF。", file=sys.stderr)
        print("请下载一个中文 TTF（必须 TrueType，不是 OTF/CFF）放入该目录:", file=sys.stderr)
        print("  霞鹜文楷     : https://github.com/lxgw/LxgwWenKai/releases", file=sys.stderr)
        print("  Noto Sans SC : https://fonts.google.com/noto/specimen/Noto+Sans+SC", file=sys.stderr)
        print("  阿里巴巴普惠体: https://fonts.alibabagroup.com/", file=sys.stderr)
        sys.exit(1)
    # 体积最大的通常是完整字库，优先
    return max(candidates, key=lambda p: p.stat().st_size)


def gen_gb2312_chars() -> str:
    """GB2312 全集 (6763 汉字 + 682 图形符号) + ASCII + 常用中文标点。"""
    chars = set()
    # GB2312 双字节编码区: 0xA1A1 - 0xFEFE
    for hi in range(0xA1, 0xFF):
        for lo in range(0xA1, 0xFF):
            try:
                chars.add(bytes([hi, lo]).decode("gb2312"))
            except UnicodeDecodeError:
                pass
    # ASCII 可见字符
    chars.update(chr(i) for i in range(0x20, 0x7F))
    # CJK 符号和标点 (U+3000 - U+303F)、全角 ASCII (U+FF00 - U+FFEF)
    chars.update(chr(i) for i in range(0x3000, 0x3040))
    chars.update(chr(i) for i in range(0xFF00, 0xFFF0))
    return "".join(sorted(chars))


def main() -> None:
    src = find_source_ttf()
    print(f"[INFO] 源字体: {src.name} ({src.stat().st_size / 1024 / 1024:.1f} MB)")

    chars = gen_gb2312_chars()
    CHARS_TXT.parent.mkdir(parents=True, exist_ok=True)
    CHARS_TXT.write_text(chars, encoding="utf-8")
    print(f"[INFO] 收录字符数: {len(chars)}")

    try:
        subprocess.check_call(
            [
                sys.executable, "-m", "fontTools.subset",
                str(src),
                f"--text-file={CHARS_TXT}",
                f"--output-file={DST}",
                "--no-layout-closure",
                "--drop-tables+=DSIG,vhea,vmtx,FFTM,PfEd,TSI0,TSI1,TSI2,TSI3,TSI5",
                "--no-hinting",
                "--no-glyph-names",
                "--desubroutinize",
                "--legacy-cmap",
            ]
        )
    except subprocess.CalledProcessError as e:
        print(f"[ERR] pyftsubset 执行失败: {e}", file=sys.stderr)
        sys.exit(2)
    except FileNotFoundError:
        print("[ERR] 未找到 fontTools。先执行: pip install fonttools", file=sys.stderr)
        sys.exit(2)

    if not DST.exists():
        print("[ERR] 子集产物未生成", file=sys.stderr)
        sys.exit(3)

    size_kb = DST.stat().st_size / 1024
    print(f"[OK] 生成 {DST.name}  ({size_kb:.1f} KB)")
    print("接下来: rm sdkconfig && idf.py fullclean && idf.py build")


if __name__ == "__main__":
    main()
