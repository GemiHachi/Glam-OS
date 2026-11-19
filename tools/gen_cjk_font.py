#!/usr/bin/env python3
"""
Generate LVGL 8.3 14px CJK + Symbol font (ui_font_cjk_14.c) for SWIFT OS.
Uses a multi-font fallback chain (msyh.ttc -> seguisym.ttf -> simsun.ttc -> arial.ttf)
with strict .notdef tofu detection so missing glyphs are never baked as white boxes.
Includes ASCII, Latin-1 Supplement, rich UI/music/math/arrow/geometric symbols,
CJK punctuation, Japanese Kana, fullwidth ASCII/currency, full GB2312 Level-1 & Level-2,
and common song/UI characters at 2bpp.
"""

import os
from PIL import Image, ImageDraw, ImageFont

OUT_PATH = os.path.abspath(
    os.path.join(os.path.dirname(__file__), "..", "os_firmware", "main", "ui", "ui_font_cjk_14.c")
)

FONT_CHAIN_SPECS = [
    ("msyh", r"C:\Windows\Fonts\msyh.ttc"),
    ("seguisym", r"C:\Windows\Fonts\seguisym.ttf"),
    ("simsun", r"C:\Windows\Fonts\simsun.ttc"),
    ("arial", r"C:\Windows\Fonts\arial.ttf"),
]

FONT_SIZE = 14
BPP = 2
LINE_HEIGHT = 18
BASE_LINE = 3

WHITESPACE_CPS = {0x0020, 0x00A0, 0x3000}


def collect_codepoints():
    cps = set(range(0x0020, 0x007F))

    # Latin-1 Supplement (deg, plus-minus, multiply, divide, middle dot, currencies, accents)
    cps.update(range(0x00A0, 0x0100))

    # Punctuation, dashes, quotes, currency, arrows, math, geometric, UI, music, numbers, Greek
    symbol_blocks = [
        "‐‑‒–—―‖‗‘’‚‛“”„‟†‡•‣․‥…‧‰‱′″‴‹›※‼‽‾⁄",
        "€₹₩₽℃℉№™ΩÅ",
        "←↑→↓↔↕↖↗↘↙↩↪⇐⇑⇒⇓⇔",
        "∀∂∃∅∇∈∉∏∑−∗√∝∞∠∧∨∩∪∫≈≠≡≤≥≪≫⊂⊃⊆⊇⊕⊗⊥⋅",
        "①②③④⑤⑥⑦⑧⑨⑩⑪⑫⑬⑭⑮⑯⑰⑱⑲⑳ⅠⅡⅢⅣⅤⅥⅦⅧⅨⅩⅪⅫⅰⅱⅲⅳⅴⅵⅶⅷⅸⅹ",
        "ΑΒΓΔΕΖΗΘΙΚΛΜΝΞΟΠΡΣΤΥΦΧΨΩαβγδεζηθικλμνξοπρστυφχψω",
        "■□▪▫▬▲△▶▷▼▽◀◁◆◇◈◉○◎●◐◑★☆☉☐☑☒☜☞♠♡♢♣♤♥♦♧♨♩♪♫♬♭♮♯✓✔✕✖✗✘✙✚✦✧✩✪❖☀☁☂☃☄⚡⚠⚙",
        # CJK Symbols & Punctuation
        "\u3000、。〃々〆〇〈〉《》「」『』【】〒〓〔〕〖〗〘〙〚〛〜〝〞〟・",
    ]
    for block in symbol_blocks:
        for ch in block:
            cps.add(ord(ch))

    # Japanese Hiragana & Katakana (common in online music titles/lyrics)
    cps.update(range(0x3041, 0x3097))
    cps.update(range(0x3099, 0x309F))
    cps.update(range(0x30A1, 0x30FF))

    # Fullwidth ASCII & currency symbols
    cps.update(range(0xFF01, 0xFF5F))
    cps.update(range(0xFFE0, 0xFFE6))

    # Full GB2312 Level-1 + Level-2 (rows 16..87, 6763 chars, including 邃 U+9083)
    for qu in range(16, 88):
        for wei in range(1, 95):
            if qu == 55 and wei >= 90:
                continue
            b = bytes([qu + 0xA0, wei + 0xA0])
            try:
                ch = b.decode("gb2312")
                if len(ch) == 1 and ord(ch) >= 0x80:
                    cps.add(ord(ch))
            except UnicodeDecodeError:
                pass

    # Extra modern/UI/singer/song characters
    extra = "喆陶邓紫棋薛谦粤咏琳玮珺婷妍宇轩诺汐沐梓宸曜璇璐岚韵籁弦奏摇滚爵嘻哈邃魅黛"
    for ch in extra:
        cps.add(ord(ch))

    # Strictly exclude PUA 0xE000..0xF8FF (LVGL LV_SYMBOL_* must fall back to montserrat_14)
    # and BMP-only < 0x10000
    filtered = sorted(cp for cp in cps if cp < 0x10000 and not (0xE000 <= cp <= 0xF8FF))
    return filtered


def _draw_char_bitmap(font, ch):
    cw, ch_h = 40, 40
    ox, oy = 8, 26
    img = Image.new("L", (cw, ch_h), 0)
    draw = ImageDraw.Draw(img)
    draw.text((ox, oy), ch, font=font, fill=255, anchor="ls")
    bbox = img.getbbox()
    return img, bbox, ox, oy


def _glyph_sig(font, ch):
    img, bbox, _, _ = _draw_char_bitmap(font, ch)
    if bbox is None:
        return None
    return (bbox, img.crop(bbox).tobytes())


def load_font_chain():
    chain = []
    for name, path in FONT_CHAIN_SPECS:
        font = ImageFont.truetype(path, FONT_SIZE)
        tofu_sigs = set()
        for sentinel in ("\ufffd", "\ue000"):
            sig = _glyph_sig(font, sentinel)
            if sig is not None:
                tofu_sigs.add(sig)
        chain.append((name, font, tofu_sigs))
    return chain


def render_glyph_with_chain(chain, cp):
    ch = chr(cp)
    primary_font = chain[0][1]

    if cp in WHITESPACE_CPS:
        if cp == 0x3000:
            adv_px = FONT_SIZE
        else:
            adv_px = max(1, int(round(primary_font.getlength(ch))))
        return b"", adv_px * 16, 0, 0, 0, 0, "whitespace"

    for name, font, tofu_sigs in chain:
        img, bbox, ox, oy = _draw_char_bitmap(font, ch)
        if bbox is None:
            continue
        sig = (bbox, img.crop(bbox).tobytes())
        if sig in tofu_sigs:
            continue

        x1, y1, x2, y2 = bbox
        box_w = x2 - x1
        box_h = y2 - y1
        ofs_x = x1 - ox
        ofs_y = oy - y2
        adv_px = max(box_w, int(round(font.getlength(ch))), 1)

        # Pack 2bpp MSB-first continuous bitstream across box_w * box_h
        out = bytearray()
        cur_byte = 0
        bit_pos = 6
        for py in range(y1, y2):
            for px in range(x1, x2):
                v = img.getpixel((px, py))
                q = (v + 42) // 85
                if q > 3:
                    q = 3
                cur_byte |= (q << bit_pos)
                if bit_pos == 0:
                    out.append(cur_byte)
                    cur_byte = 0
                    bit_pos = 6
                else:
                    bit_pos -= 2
        if bit_pos != 6:
            out.append(cur_byte)

        return bytes(out), adv_px * 16, box_w, box_h, ofs_x, ofs_y, name

    return None


def main():
    chain = load_font_chain()
    raw_cps = collect_codepoints()

    rendered = {}
    font_stats = {}
    skipped = []

    for cp in raw_cps:
        res = render_glyph_with_chain(chain, cp)
        if res is None:
            skipped.append((cp, chr(cp)))
            continue
        bmp, adv_w, bw, bh, ox, oy, src_name = res
        rendered[cp] = (bmp, adv_w, bw, bh, ox, oy)
        font_stats[src_name] = font_stats.get(src_name, 0) + 1

    emitted_cps = sorted(rendered.keys())
    ascii_cps = [cp for cp in emitted_cps if 0x20 <= cp <= 0x7E]
    assert len(ascii_cps) == 95, f"Expected 95 ASCII glyphs, got {len(ascii_cps)}"

    non_ascii = [cp for cp in emitted_cps if cp >= 0x80]

    # Split non-ASCII into SPARSE_TINY cmaps with span <= 60000 and never crossing PUA 0xE000..0xF8FF
    groups = []
    cur_grp = []
    for cp in non_ascii:
        if not cur_grp:
            cur_grp.append(cp)
        else:
            crosses_pua = (cur_grp[0] < 0xE000 and cp > 0xF8FF)
            if (cp - cur_grp[0] <= 60000) and not crosses_pua:
                cur_grp.append(cp)
            else:
                groups.append(cur_grp)
                cur_grp = [cp]
    if cur_grp:
        groups.append(cur_grp)

    # Validate assertions before writing
    emitted_set = set(emitted_cps)
    required_chars = "深邃初雪·—…♪♫×÷°℃←→♥✓\u3000￥"
    for ch in required_chars:
        assert ord(ch) in emitted_set, f"Required character {ch!r} (U+{ord(ch):04X}) missing!"

    for grp in groups:
        r_start, r_end = grp[0], grp[-1]
        assert not (r_start <= 0xF8FF and r_end >= 0xE000), (
            f"CMAP range 0x{r_start:04X}..0x{r_end:04X} overlaps PUA 0xE000..0xF8FF!"
        )

    glyph_bitmap = bytearray()
    glyph_dsc = [(0, 0, 0, 0, 0, 0)]  # gid=0 reserved

    all_ordered = ascii_cps + [cp for g in groups for cp in g]
    for cp in all_ordered:
        bmp, adv_w, bw, bh, ox, oy = rendered[cp]
        b_idx = len(glyph_bitmap)
        glyph_bitmap.extend(bmp)
        glyph_dsc.append((b_idx, adv_w, bw, bh, ox, oy))

    os.makedirs(os.path.dirname(OUT_PATH), exist_ok=True)
    with open(OUT_PATH, "w", encoding="utf-8") as f:
        f.write("/* Auto-generated by SWIFT OS/tools/gen_cjk_font.py - 14px 2bpp CJK + Symbols */\n")
        f.write('#include "lvgl.h"\n\n')
        f.write("static LV_ATTRIBUTE_LARGE_CONST const uint8_t glyph_bitmap[] = {\n")
        for i in range(0, len(glyph_bitmap), 16):
            chunk = glyph_bitmap[i:i + 16]
            f.write("    " + ", ".join(f"0x{b:02x}" for b in chunk) + ",\n")
        f.write("};\n\n")

        f.write("static const lv_font_fmt_txt_glyph_dsc_t glyph_dsc[] = {\n")
        for b_idx, adv_w, bw, bh, ox, oy in glyph_dsc:
            f.write(
                f"    {{.bitmap_index = {b_idx}, .adv_w = {adv_w}, "
                f".box_w = {bw}, .box_h = {bh}, .ofs_x = {ox}, .ofs_y = {oy}}},\n"
            )
        f.write("};\n\n")

        for gi, grp in enumerate(groups):
            r_start = grp[0]
            f.write(f"static const uint16_t unicode_list_{gi}[] = {{\n")
            for i in range(0, len(grp), 12):
                chunk = grp[i:i + 12]
                f.write("    " + ", ".join(f"0x{cp - r_start:04x}" for cp in chunk) + ",\n")
            f.write("};\n\n")

        f.write("static const lv_font_fmt_txt_cmap_t cmaps[] = {\n")
        f.write(
            f"    {{\n"
            f"        .range_start = 32, .range_length = {len(ascii_cps) - 1}, .glyph_id_start = 1,\n"
            f"        .unicode_list = NULL, .glyph_id_ofs_list = NULL, .list_length = 0,\n"
            f"        .type = LV_FONT_FMT_TXT_CMAP_FORMAT0_TINY\n"
            f"    }},\n"
        )
        gid_cursor = 1 + len(ascii_cps)
        for gi, grp in enumerate(groups):
            r_start = grp[0]
            r_len = grp[-1] - r_start
            f.write(
                f"    {{\n"
                f"        .range_start = 0x{r_start:04x}, .range_length = {r_len}, .glyph_id_start = {gid_cursor},\n"
                f"        .unicode_list = unicode_list_{gi}, .glyph_id_ofs_list = NULL, .list_length = {len(grp)},\n"
                f"        .type = LV_FONT_FMT_TXT_CMAP_SPARSE_TINY\n"
                f"    }},\n"
            )
            gid_cursor += len(grp)
        f.write("};\n\n")

        f.write("static lv_font_fmt_txt_glyph_cache_t cache;\n")
        f.write("static const lv_font_fmt_txt_dsc_t font_dsc = {\n")
        f.write("    .glyph_bitmap = glyph_bitmap,\n")
        f.write("    .glyph_dsc = glyph_dsc,\n")
        f.write("    .cmaps = cmaps,\n")
        f.write("    .kern_dsc = NULL,\n")
        f.write("    .kern_scale = 0,\n")
        f.write(f"    .cmap_num = {1 + len(groups)},\n")
        f.write(f"    .bpp = {BPP},\n")
        f.write("    .kern_classes = 0,\n")
        f.write("    .bitmap_format = 0,\n")
        f.write("    .cache = &cache\n")
        f.write("};\n\n")

        f.write("const lv_font_t ui_font_cjk_14 = {\n")
        f.write("    .get_glyph_dsc = lv_font_get_glyph_dsc_fmt_txt,\n")
        f.write("    .get_glyph_bitmap = lv_font_get_bitmap_fmt_txt,\n")
        f.write(f"    .line_height = {LINE_HEIGHT},\n")
        f.write(f"    .base_line = {BASE_LINE},\n")
        f.write("    .subpx = LV_FONT_SUBPX_NONE,\n")
        f.write("    .underline_position = -2,\n")
        f.write("    .underline_thickness = 1,\n")
        f.write("    .dsc = &font_dsc,\n")
        f.write("    .fallback = &lv_font_montserrat_14\n")
        f.write("};\n")

    print(
        f"Generated {OUT_PATH}: glyphs={len(all_ordered)}, "
        f"bitmap_bytes={len(glyph_bitmap)}, cmaps={1 + len(groups)}, "
        f"sources={font_stats}, skipped_tofu={len(skipped)}"
    )
    if skipped:
        print("Skipped (all-tofu):", [(hex(cp), ch) for cp, ch in skipped[:20]])


if __name__ == "__main__":
    main()
