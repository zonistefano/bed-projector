#!/usr/bin/env python3
"""Rasterize Montserrat into the LVGL fonts used by the projector pages.

LVGL's built-in Montserrat fonts are Medium weight and ASCII only. The pages
need heavier strokes (the image is projected and loses contrast), a bold face
for temperatures and Latin-1 letters for Italian entity names and forecasts.
Text fonts fall back to the inline icon fonts, so a label can mix words and
Material Design Icons. Run from the repository root after
`pip install fonttools` to regenerate main/bed_fonts.c and main/bed_fonts.h:

    python3 tools/generate_fonts.py
"""

from __future__ import annotations

import math
import sys
import urllib.request
from pathlib import Path

from fontTools.pens.basePen import BasePen
from fontTools.ttLib import TTFont

sys.path.insert(0, str(Path(__file__).resolve().parent))
from generate_icons import INLINE, bezier, coverage, pack  # noqa: E402

MONTSERRAT_URL = "https://cdn.jsdelivr.net/gh/JulietaUla/Montserrat@v7.222/fonts/ttf/Montserrat-{}.ttf"
TEXT = [*range(0x20, 0x7F), *range(0xA0, 0x100), 0x2013, 0x2014, 0x2019, 0x2022, 0x2026]
DIGITS = [ord(c) for c in " +-.,/:%0123456789°"]
CLOCK = [ord(c) for c in " -:0123456789"]

# (name, weight, pixel size, characters, inline icon fallback size)
FONTS = (
    ("text_10", "SemiBold", 10, TEXT, None),
    ("text_12", "SemiBold", 12, TEXT, 12),
    ("text_14", "SemiBold", 14, TEXT, 14),
    ("bold_14", "Bold", 14, DIGITS, None),
    ("bold_16", "Bold", 16, DIGITS, None),
    ("bold_20", "Bold", 20, DIGITS, None),
    ("bold_26", "Bold", 26, DIGITS, None),
    ("clock_28", "SemiBold", 28, CLOCK, None),
    ("clock_34", "SemiBold", 34, CLOCK, None),
    ("clock_40", "SemiBold", 40, CLOCK, None),
    ("clock_46", "SemiBold", 46, CLOCK, None),
)


class PolygonPen(BasePen):
    """Flatten a glyph outline into closed polygons in font units."""

    def __init__(self, glyph_set):
        super().__init__(glyph_set)
        self.polygons: list[list[tuple[float, float]]] = []
        self.current: list[tuple[float, float]] = []

    def _moveTo(self, point):
        self._closePath()
        self.current = [point]

    def _lineTo(self, point):
        self.current.append(point)

    def _curveToOne(self, first, second, end):
        self.current += bezier([self._getCurrentPoint(), first, second, end], 8)

    def _qCurveToOne(self, control, end):
        self.current += bezier([self._getCurrentPoint(), control, end], 8)

    def _closePath(self):
        if len(self.current) > 2:
            self.polygons.append(self.current)
        self.current = []

    _endPath = _closePath


def load(weight: str) -> TTFont:
    cache = Path(__file__).resolve().parent / ".cache" / f"Montserrat-{weight}.ttf"
    if not cache.exists():
        cache.parent.mkdir(exist_ok=True)
        with urllib.request.urlopen(MONTSERRAT_URL.format(weight), timeout=30) as response:
            cache.write_bytes(response.read())
    return TTFont(cache)


def ranges(codepoints: list[int]) -> list[tuple[int, int]]:
    runs: list[tuple[int, int]] = []
    for code in sorted(set(codepoints)):
        if runs and runs[-1][0] + runs[-1][1] == code:
            runs[-1] = (runs[-1][0], runs[-1][1] + 1)
        else:
            runs.append((code, 1))
    return runs


def font_source(name: str, font: TTFont, size: int, codepoints: list[int],
                fallback: int | None) -> str:
    units = font["head"].unitsPerEm
    scale = size / units
    ascent = math.ceil(font["hhea"].ascent * scale)
    descent = math.ceil(-font["hhea"].descent * scale)
    cmap = font.getBestCmap()
    glyph_set = font.getGlyphSet()
    bitmap, descriptors, offset = [], [], 0
    runs = ranges([code for code in codepoints if code in cmap])
    for start, length in runs:
        for code in range(start, start + length):
            glyph = cmap[code]
            pen = PolygonPen(glyph_set)
            glyph_set[glyph].draw(pen)
            advance = round(font["hmtx"][glyph][0] * scale * 16)
            points = [p for polygon in pen.polygons for p in polygon]
            if not points:
                descriptors.append(f"    {{.bitmap_index = {offset}, .adv_w = {advance}, .box_w = 0, "
                                   ".box_h = 0, .ofs_x = 0, .ofs_y = 0},")
                continue
            left = math.floor(min(x for x, _ in points) * scale)
            right = math.ceil(max(x for x, _ in points) * scale)
            bottom = math.floor(min(y for _, y in points) * scale)
            top = math.ceil(max(y for _, y in points) * scale)
            width, height = right - left, top - bottom
            polygons = [[(x * scale - left, top - y * scale) for x, y in polygon]
                        for polygon in pen.polygons]
            data = pack(coverage(polygons, width, height))
            bitmap.append(f"    /* U+{code:04X} */ " + ", ".join(f"0x{b:02x}" for b in data) + ",")
            descriptors.append(f"    {{.bitmap_index = {offset}, .adv_w = {advance}, .box_w = {width}, "
                               f".box_h = {height}, .ofs_x = {left}, .ofs_y = {bottom}}},")
            offset += len(data)
    cmaps, glyph_id = [], 1
    for start, length in runs:
        cmaps.append(f"    {{.range_start = 0x{start:X}, .range_length = {length}, "
                     f".glyph_id_start = {glyph_id}, .unicode_list = NULL, .glyph_id_ofs_list = NULL, "
                     ".list_length = 0, .type = LV_FONT_FMT_TXT_CMAP_FORMAT0_TINY},")
        glyph_id += length
    fallback_ref = f"&bed_icons_inline_{fallback}" if fallback else "NULL"
    return f"""
static LV_ATTRIBUTE_LARGE_CONST const uint8_t glyph_bitmap_{name}[] = {{
{chr(10).join(bitmap)}
}};

static const lv_font_fmt_txt_glyph_dsc_t glyph_dsc_{name}[] = {{
    {{.bitmap_index = 0, .adv_w = 0, .box_w = 0, .box_h = 0, .ofs_x = 0, .ofs_y = 0}},
{chr(10).join(descriptors)}
}};

static const lv_font_fmt_txt_cmap_t cmaps_{name}[] = {{
{chr(10).join(cmaps)}
}};

static const lv_font_fmt_txt_dsc_t font_dsc_{name} = {{
    .glyph_bitmap = glyph_bitmap_{name}, .glyph_dsc = glyph_dsc_{name}, .cmaps = cmaps_{name},
    .kern_dsc = NULL, .kern_scale = 0, .cmap_num = {len(runs)}, .bpp = 4, .kern_classes = 0,
    .bitmap_format = 0,
}};

const lv_font_t bed_font_{name} = {{
    .get_glyph_dsc = lv_font_get_glyph_dsc_fmt_txt,
    .get_glyph_bitmap = lv_font_get_bitmap_fmt_txt,
    .line_height = {ascent + descent}, .base_line = {descent}, .subpx = LV_FONT_SUBPX_NONE,
    .underline_position = 0, .underline_thickness = 0, .dsc = &font_dsc_{name},
    .fallback = {fallback_ref},
}};
"""


def main() -> int:
    root = Path(__file__).resolve().parents[1]
    for _, _, _, _, fallback in FONTS:
        if fallback and fallback not in INLINE:
            raise ValueError(f"no inline icon font of size {fallback}")
    faces = {weight: load(weight) for weight in {weight for _, weight, _, _, _ in FONTS}}
    note = ("/* Generated by tools/generate_fonts.py from Montserrat (SIL Open Font License 1.1). */")
    header = [note, "#pragma once", "", '#include "lvgl.h"', ""]
    header += [f"extern const lv_font_t bed_font_{name};" for name, *_ in FONTS]
    source = [note, '#include "bed_fonts.h"', '#include "bed_icons.h"']
    for name, weight, size, codepoints, fallback in FONTS:
        source.append(font_source(name, faces[weight], size, codepoints, fallback))
    (root / "main/bed_fonts.h").write_text("\n".join(header) + "\n")
    (root / "main/bed_fonts.c").write_text("\n".join(source))
    return 0


if __name__ == "__main__":
    sys.exit(main())
