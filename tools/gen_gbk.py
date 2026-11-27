#!/usr/bin/env python3
"""生成 Unicode -> GBK 对照表 main/bambu/gbk_table.c（打印机 FTP 上的中文文件名是 GBK 编码）。"""
import pathlib

OUT = pathlib.Path(__file__).resolve().parent.parent / "main" / "bambu" / "gbk_table.c"
pairs = []
for cp in range(0x80, 0x10000):
    if 0xD800 <= cp <= 0xDFFF:
        continue
    try:
        b = chr(cp).encode("gbk")
    except UnicodeEncodeError:
        continue
    if len(b) == 2:
        pairs.append((cp, b[0] << 8 | b[1]))
lines = ["// 由 tools/gen_gbk.py 生成，不要手改", "#include <stdint.h>", "",
         f"const uint16_t gbk_table_count = {len(pairs)};", "const uint16_t gbk_table[][2] = {"]
for i in range(0, len(pairs), 8):
    lines.append("    " + " ".join(f"{{0x{u:04X},0x{g:04X}}}," for u, g in pairs[i:i + 8]))
lines.append("};")
OUT.write_text("\n".join(lines) + "\n", encoding="utf-8")
print(len(pairs), "entries")
