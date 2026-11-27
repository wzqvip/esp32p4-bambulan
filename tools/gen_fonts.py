#!/usr/bin/env python3
"""生成 LVGL 字体（需要 node 版 lv_font_conv：npm i -g lv_font_conv）。

字体源文件（均为 SIL OFL 许可）：
  NotoSansSC-Medium.ttf   由 google/fonts 的 NotoSansSC[wght].ttf 用
                          `fonttools varLib.instancer ... wght=500` 生成
  Montserrat-SemiBold.ttf 由 google/fonts 的 Montserrat[wght].ttf 用 wght=600 生成

用法：python3 tools/gen_fonts.py <字体目录>
会扫描 main/ 下所有 .c 源码里出现的非 ASCII 字符，自动加进字体，所以改了界面文字后重跑一次即可。
20 和 28 号字都包含 GB2312 一级汉字（3755 个常用字），用于显示中文任务名和打印机名。
"""
import pathlib
import re
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
OUT = ROOT / "main" / "ui" / "fonts"
font_dir = pathlib.Path(sys.argv[1] if len(sys.argv) > 1 else ".")
CJK = font_dir / "NotoSansSC-Medium.ttf"
NUM = font_dir / "Montserrat-SemiBold.ttf"


def source_chars():
    chars = set()
    for f in (ROOT / "main").rglob("*.c"):
        if "fonts" in f.parts:
            continue
        for lit in re.findall(r'"((?:[^"\\\n]|\\.)*)"', f.read_text(encoding="utf-8")):
            chars.update(c for c in lit if ord(c) > 0x7E and not 0xE000 <= ord(c) <= 0xF8FF)
    return chars


def gb2312_level1():
    out = set()
    for hi in range(0xB0, 0xD8):
        for lo in range(0xA1, 0xFF):
            try:
                out.add(bytes([hi, lo]).decode("gb2312"))
            except UnicodeDecodeError:
                pass
    return out


def conv(name, font, size, symbols, ranges=None, fallback=None):
    cmd = ["lv_font_conv", "--no-compress", "--format", "lvgl", "--bpp", "4", "--size", str(size),
           "--font", str(font)]
    if ranges:
        cmd += ["-r", ranges]
    if symbols:
        cmd += ["--symbols", "".join(sorted(symbols))]
    cmd += ["--lv-include", "lvgl.h", "-o", str(OUT / f"{name}.c")]
    if fallback:
        cmd += ["--lv-fallback", fallback]
    print("generating", name, f"({len(symbols)} extra chars)")
    subprocess.run(cmd, check=True)


def main():
    OUT.mkdir(parents=True, exist_ok=True)
    ui = source_chars()
    extra = set("°·…：，。「」？！（）　")
    conv("ui_font_cn_28", CJK, 28, ui | extra | gb2312_level1(), "0x20-0x7E", "lv_font_montserrat_28")
    conv("ui_font_cn_20", CJK, 20, ui | extra | gb2312_level1(), "0x20-0x7E", "lv_font_montserrat_20")
    conv("ui_font_num_56", NUM, 56, set(), "0x25,0x2D-0x39", None)


if __name__ == "__main__":
    main()
