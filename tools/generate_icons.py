#!/usr/bin/env python3
"""Rasterize the Material Design Icons used by Home Assistant into LVGL fonts.

The firmware has no A8/I1 image support, so icons are glyphs of 4 bpp fonts in
the Unicode private use area. Run from the repository root to regenerate
main/bed_icons.c and main/bed_icons.h:

    python3 tools/generate_icons.py
"""

from __future__ import annotations

import math
import re
import sys
import urllib.request
from pathlib import Path

MDI_VERSION = "7.4.47"
MDI_URL = f"https://cdn.jsdelivr.net/npm/@mdi/svg@{MDI_VERSION}/svg/{{}}.svg"
SIZES = (14, 18, 22)
FIRST_CODEPOINT = 0xE000
SAMPLES = 4  # per axis: 16 samples map exactly to the 4 bpp alpha range

# (macro suffix, MDI name). Alarm icons follow HA's alarm_control_panel icons.json,
# weather icons follow the HA frontend's weatherIcons table.
ICONS = (
    ("DOOR_OPEN", "door-open"),
    ("SHIELD", "shield"),
    ("SHIELD_OFF", "shield-off"),
    ("SHIELD_HOME", "shield-home"),
    ("SHIELD_LOCK", "shield-lock"),
    ("SHIELD_MOON", "shield-moon"),
    ("SHIELD_AIRPLANE", "shield-airplane"),
    ("SECURITY", "security"),
    ("SHIELD_OUTLINE", "shield-outline"),
    ("BELL_RING", "bell-ring"),
    ("WEATHER_NIGHT", "weather-night"),
    ("WEATHER_CLOUDY", "weather-cloudy"),
    ("ALERT_CIRCLE_OUTLINE", "alert-circle-outline"),
    ("WEATHER_FOG", "weather-fog"),
    ("WEATHER_HAIL", "weather-hail"),
    ("WEATHER_LIGHTNING", "weather-lightning"),
    ("WEATHER_LIGHTNING_RAINY", "weather-lightning-rainy"),
    ("WEATHER_PARTLY_CLOUDY", "weather-partly-cloudy"),
    ("WEATHER_POURING", "weather-pouring"),
    ("WEATHER_RAINY", "weather-rainy"),
    ("WEATHER_SNOWY", "weather-snowy"),
    ("WEATHER_SNOWY_RAINY", "weather-snowy-rainy"),
    ("WEATHER_SUNNY", "weather-sunny"),
    ("WEATHER_WINDY", "weather-windy"),
    ("WEATHER_WINDY_VARIANT", "weather-windy-variant"),
    ("HELP_CIRCLE_OUTLINE", "help-circle-outline"),
)

Point = tuple[float, float]


def fetch_path(name: str) -> str:
    with urllib.request.urlopen(MDI_URL.format(name), timeout=20) as response:
        svg = response.read().decode()
    match = re.search(r'\sd="([^"]+)"', svg)
    if not match:
        raise ValueError(f"no path in {name}")
    return match.group(1)


def tokens(d: str):
    for match in re.finditer(r"[A-Za-z]|[-+]?(?:\d*\.\d+|\d+\.?)(?:[eE][-+]?\d+)?", d):
        yield match.group(0)


def arc_points(start: Point, rx: float, ry: float, angle: float, large: bool, sweep: bool,
               end: Point) -> list[Point]:
    """SVG endpoint arc to polyline (SVG 1.1 implementation notes, F.6.5)."""
    if rx == 0 or ry == 0 or start == end:
        return [end]
    rx, ry = abs(rx), abs(ry)
    phi = math.radians(angle)
    cos_phi, sin_phi = math.cos(phi), math.sin(phi)
    dx, dy = (start[0] - end[0]) / 2, (start[1] - end[1]) / 2
    x1 = cos_phi * dx + sin_phi * dy
    y1 = -sin_phi * dx + cos_phi * dy
    scale = x1 * x1 / (rx * rx) + y1 * y1 / (ry * ry)
    if scale > 1:
        rx, ry = rx * math.sqrt(scale), ry * math.sqrt(scale)
    numerator = rx * rx * ry * ry - rx * rx * y1 * y1 - ry * ry * x1 * x1
    denominator = rx * rx * y1 * y1 + ry * ry * x1 * x1
    factor = math.sqrt(max(0.0, numerator / denominator))
    if large == sweep:
        factor = -factor
    cx1, cy1 = factor * rx * y1 / ry, -factor * ry * x1 / rx
    cx = cos_phi * cx1 - sin_phi * cy1 + (start[0] + end[0]) / 2
    cy = sin_phi * cx1 + cos_phi * cy1 + (start[1] + end[1]) / 2
    theta = math.atan2((y1 - cy1) / ry, (x1 - cx1) / rx)
    delta = math.atan2((-y1 - cy1) / ry, (-x1 - cx1) / rx) - theta
    if sweep and delta < 0:
        delta += 2 * math.pi
    elif not sweep and delta > 0:
        delta -= 2 * math.pi
    steps = max(4, int(abs(delta) * 16))
    points = []
    for i in range(1, steps + 1):
        t = theta + delta * i / steps
        x, y = rx * math.cos(t), ry * math.sin(t)
        points.append((cos_phi * x - sin_phi * y + cx, sin_phi * x + cos_phi * y + cy))
    return points


def bezier(points: list[Point], steps: int = 16) -> list[Point]:
    out = []
    for i in range(1, steps + 1):
        t = i / steps
        work = list(points)
        while len(work) > 1:
            work = [(a[0] + (b[0] - a[0]) * t, a[1] + (b[1] - a[1]) * t)
                    for a, b in zip(work, work[1:])]
        out.append(work[0])
    return out


def flatten(d: str) -> list[list[Point]]:
    """Parse an SVG path into closed polygons in viewBox units."""
    arg_count = {"M": 2, "L": 2, "H": 1, "V": 1, "C": 6, "S": 4, "Q": 4, "T": 2, "A": 7, "Z": 0}
    items = list(tokens(d))
    polygons: list[list[Point]] = []
    current: list[Point] = []
    pos: Point = (0.0, 0.0)
    start: Point = pos
    last_control: Point | None = None
    last_command = ""
    command = ""
    index = 0
    while index < len(items):
        if items[index].isalpha():
            command = items[index]
            index += 1
        elif not command:
            raise ValueError("path must start with a command")
        upper = command.upper()
        relative = command.islower()
        values = [float(v) for v in items[index:index + arg_count[upper]]]
        index += arg_count[upper]
        ox, oy = pos if relative else (0.0, 0.0)

        def absolute(i: int) -> Point:
            return (values[i] + ox, values[i + 1] + oy)

        control = None
        if upper == "M":
            if len(current) > 1:
                polygons.append(current)
            pos = start = absolute(0)
            current = [pos]
            command = "l" if relative else "L"
        elif upper == "L":
            pos = absolute(0)
            current.append(pos)
        elif upper == "H":
            pos = (values[0] + ox, pos[1])
            current.append(pos)
        elif upper == "V":
            pos = (pos[0], values[0] + (pos[1] if relative else 0.0))
            current.append(pos)
        elif upper in "CS":
            if upper == "C":
                first, control, end = absolute(0), absolute(2), absolute(4)
            else:
                reflect = last_control if last_command in "CS" and last_control else pos
                first = (2 * pos[0] - reflect[0], 2 * pos[1] - reflect[1])
                control, end = absolute(0), absolute(2)
            current += bezier([pos, first, control, end])
            pos = end
        elif upper in "QT":
            if upper == "Q":
                control, end = absolute(0), absolute(2)
            else:
                reflect = last_control if last_command in "QT" and last_control else pos
                control = (2 * pos[0] - reflect[0], 2 * pos[1] - reflect[1])
                end = absolute(0)
            current += bezier([pos, control, end])
            pos = end
        elif upper == "A":
            end = absolute(5)
            current += arc_points(pos, values[0], values[1], values[2], bool(values[3]),
                                  bool(values[4]), end)
            pos = end
        elif upper == "Z":
            if len(current) > 1:
                polygons.append(current)
            pos = start
            current = [pos]
        last_control = control
        last_command = upper
    if len(current) > 1:
        polygons.append(current)
    return polygons


def rasterize(polygons: list[list[Point]], size: int) -> list[int]:
    """Nonzero-winding coverage per pixel, 0..15, of the 24x24 viewBox scaled to size."""
    scale = size / 24
    edges = []
    for polygon in polygons:
        for a, b in zip(polygon, polygon[1:] + polygon[:1]):
            if a[1] != b[1]:
                edges.append((a[0] * scale, a[1] * scale, b[0] * scale, b[1] * scale))
    grid = SAMPLES * size
    coverage = [0] * (size * size)
    for sy in range(grid):
        y = (sy + 0.5) / SAMPLES
        crossings = []
        for x0, y0, x1, y1 in edges:
            if (y0 <= y < y1) or (y1 <= y < y0):
                crossings.append((x0 + (y - y0) * (x1 - x0) / (y1 - y0), 1 if y1 > y0 else -1))
        crossings.sort()
        winding = 0
        for (x, direction), following in zip(crossings, crossings[1:] + [(None, 0)]):
            winding += direction
            if winding == 0 or following[0] is None:
                continue
            first = max(0, math.ceil(x * SAMPLES - 0.5))
            last = min(grid - 1, math.ceil(following[0] * SAMPLES - 0.5) - 1)
            for sx in range(first, last + 1):
                coverage[(sy // SAMPLES) * size + sx // SAMPLES] += 1
    return [min(15, value) for value in coverage]


def pack(pixels: list[int]) -> list[int]:
    if len(pixels) % 2:
        pixels = pixels + [0]
    return [(pixels[i] << 4) | pixels[i + 1] for i in range(0, len(pixels), 2)]


def font_source(size: int, glyphs: list[list[int]]) -> str:
    bitmap, descriptors, offset = [], [], 0
    for macro, pixels in zip((m for m, _ in ICONS), glyphs):
        data = pack(pixels)
        bitmap.append(f"    /* {macro} */")
        for i in range(0, len(data), 16):
            bitmap.append("    " + ", ".join(f"0x{b:02x}" for b in data[i:i + 16]) + ",")
        descriptors.append(f"    {{.bitmap_index = {offset}, .adv_w = {size * 16}, .box_w = {size}, "
                           f".box_h = {size}, .ofs_x = 0, .ofs_y = 0}},")
        offset += len(data)
    return f"""
static LV_ATTRIBUTE_LARGE_CONST const uint8_t glyph_bitmap_{size}[] = {{
{chr(10).join(bitmap)}
}};

static const lv_font_fmt_txt_glyph_dsc_t glyph_dsc_{size}[] = {{
    {{.bitmap_index = 0, .adv_w = 0, .box_w = 0, .box_h = 0, .ofs_x = 0, .ofs_y = 0}},
{chr(10).join(descriptors)}
}};

static const lv_font_fmt_txt_cmap_t cmaps_{size}[] = {{
    {{.range_start = 0x{FIRST_CODEPOINT:X}, .range_length = {len(ICONS)}, .glyph_id_start = 1,
     .unicode_list = NULL, .glyph_id_ofs_list = NULL, .list_length = 0,
     .type = LV_FONT_FMT_TXT_CMAP_FORMAT0_TINY}},
}};

static const lv_font_fmt_txt_dsc_t font_dsc_{size} = {{
    .glyph_bitmap = glyph_bitmap_{size}, .glyph_dsc = glyph_dsc_{size}, .cmaps = cmaps_{size},
    .kern_dsc = NULL, .kern_scale = 0, .cmap_num = 1, .bpp = 4, .kern_classes = 0,
    .bitmap_format = 0,
}};

const lv_font_t bed_icons_{size} = {{
    .get_glyph_dsc = lv_font_get_glyph_dsc_fmt_txt,
    .get_glyph_bitmap = lv_font_get_bitmap_fmt_txt,
    .line_height = {size}, .base_line = 0, .subpx = LV_FONT_SUBPX_NONE,
    .underline_position = 0, .underline_thickness = 0, .dsc = &font_dsc_{size},
}};
"""


def utf8_literal(codepoint: int) -> str:
    return "".join(f"\\x{b:02X}" for b in chr(codepoint).encode())


def main() -> int:
    root = Path(__file__).resolve().parents[1]
    polygons = [flatten(fetch_path(name)) for _, name in ICONS]
    header = ["/* Generated by tools/generate_icons.py from Material Design Icons "
              f"{MDI_VERSION} (Apache 2.0). */", "#pragma once", "", '#include "lvgl.h"', ""]
    header += [f"extern const lv_font_t bed_icons_{size};" for size in SIZES] + [""]
    for i, (macro, name) in enumerate(ICONS):
        header.append(f'#define BED_ICON_{macro} "{utf8_literal(FIRST_CODEPOINT + i)}" /* mdi:{name} */')
    source = [header[0], '#include "bed_icons.h"']
    for size in SIZES:
        source.append(font_source(size, [rasterize(p, size) for p in polygons]))
    (root / "main/bed_icons.h").write_text("\n".join(header) + "\n")
    (root / "main/bed_icons.c").write_text("\n".join(source))
    return 0


if __name__ == "__main__":
    sys.exit(main())
