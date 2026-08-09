#!/usr/bin/env python3
"""Require all source Chinese/UI punctuation in every fixed UI font size."""

from pathlib import Path
import re


project = Path(__file__).parents[1]
source_dir = project / "firmware" / "PokePodAmoled"
header_path = source_dir / "FixedChineseFont.h"
punctuation = set("，。！？：；、“”‘’（）【】《》—…·￥")

required = set(range(0x20, 0x7F)) | {ord(char) for char in punctuation}
sources = sorted(
    path for path in source_dir.iterdir()
    if path.suffix in {".cpp", ".h", ".ino"}
    and path.name != header_path.name
)
for path in sources:
    source = path.read_text(encoding="utf-8")
    required.update(
        ord(char) for char in source
        if 0x3400 <= ord(char) <= 0x4DBF
        or 0x4E00 <= ord(char) <= 0x9FFF
        or 0xF900 <= ord(char) <= 0xFAFF
        or char in punctuation
    )

header = header_path.read_text(encoding="utf-8")
for size in (16, 20, 28):
    body_match = re.search(
        rf"kFixedGlyphs{size}\[\]\s*=\s*\{{(.*?)\n\}};",
        header,
        re.DOTALL,
    )
    assert body_match is not None, f"missing kFixedGlyphs{size}"
    available = {
        int(point, 16)
        for point in re.findall(r"\{0x([0-9a-fA-F]+),", body_match.group(1))
    }
    missing = required - available
    assert not missing, (
        f"fixed {size}px missing {len(missing)} UI glyphs: "
        + "".join(chr(point) for point in sorted(missing))
    )

print(
    f"PASS test_fixed_font_coverage "
    f"({len(sources)} sources, {len(required)} glyphs x 16/20/28)"
)
