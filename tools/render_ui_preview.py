#!/usr/bin/env python3
"""
Render 3x-upscaled (960x720) pixel-faithful previews of SWIFT OS screens:
  1. ui_preview_1_home.png       - SWIFT OS Desktop Launcher
  2. ui_preview_2_music_elf.png  - 1:1 Original Playpal iSwift Hub Now Playing View
  3. ui_preview_3_music_tabs.png - 1:1 Original Playpal iSwift Hub 3-Tab Home View
"""

import os
import shutil
from PIL import Image, ImageDraw, ImageFont

S = 3  # 320x240 -> 960x720 crisp scale
W = 320 * S
H = 240 * S
FONT_PATH = r"C:\Windows\Fonts\msyh.ttc"

OUT_DIR = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
ARTIFACT_DIR = r"C:\Users\Mark\.gemini\antigravity\brain\ef4c6f79-3f75-4f72-a635-f5b1694c856e"


def hex_rgb(val: int) -> tuple[int, int, int]:
    return ((val >> 16) & 0xFF, (val >> 8) & 0xFF, val & 0xFF)


def font(sz: int) -> ImageFont.FreeTypeFont:
    return ImageFont.truetype(FONT_PATH, sz * S)


def rr(draw: ImageDraw.ImageDraw, x: int, y: int, w: int, h: int, r: int,
       fill: int | None = None, outline: int | None = None, width: int = 1) -> None:
    box = [x * S, y * S, (x + w) * S - 1, (y + h) * S - 1]
    draw.rounded_rectangle(
        box,
        radius=r * S,
        fill=hex_rgb(fill) if fill is not None else None,
        outline=hex_rgb(outline) if outline is not None else None,
        width=max(1, width * S) if outline is not None else 0,
    )


def text_center(draw: ImageDraw.ImageDraw, cx: int, cy: int, txt: str,
                fnt: ImageFont.FreeTypeFont, col: int) -> None:
    draw.text((cx * S, cy * S), txt, font=fnt, fill=hex_rgb(col), anchor="mm")


def text_tl(draw: ImageDraw.ImageDraw, x: int, y: int, txt: str,
            fnt: ImageFont.FreeTypeFont, col: int) -> None:
    draw.text((x * S, y * S), txt, font=fnt, fill=hex_rgb(col))


def draw_wifi_icon(draw: ImageDraw.ImageDraw, x: int, y: int, col: int) -> None:
    c = hex_rgb(col)
    draw.arc([x * S, y * S, (x + 12) * S, (y + 12) * S], 225, 315, fill=c, width=2)
    draw.arc([(x + 2) * S, (y + 3) * S, (x + 10) * S, (y + 11) * S], 225, 315, fill=c, width=2)
    draw.ellipse([(x + 5) * S, (y + 8) * S, (x + 7) * S, (y + 10) * S], fill=c)


def draw_sd_icon(draw: ImageDraw.ImageDraw, x: int, y: int, col: int) -> None:
    c = hex_rgb(col)
    pts = [
        ((x + 2) * S, y * S),
        ((x + 9) * S, y * S),
        ((x + 9) * S, (y + 11) * S),
        (x * S, (y + 11) * S),
        (x * S, (y + 3) * S),
    ]
    draw.polygon(pts, outline=c, width=2)
    draw.line([(x + 4) * S, (y + 2) * S, (x + 4) * S, (y + 5) * S], fill=c, width=2)
    draw.line([(x + 6) * S, (y + 2) * S, (x + 6) * S, (y + 5) * S], fill=c, width=2)


def draw_music_note(draw: ImageDraw.ImageDraw, cx: int, cy: int, col: int, scale: float = 1.0) -> None:
    c = hex_rgb(col)
    r = int(round(2.5 * S * scale))
    stem_h = int(round(9 * S * scale))
    nx = int(round((cx - 1) * S))
    ny = int(round((cy + 3) * S))
    draw.ellipse([nx - r, ny - r + 1, nx + r, ny + r - 1], fill=c)
    draw.line([nx + r - 1, ny, nx + r - 1, ny - stem_h], fill=c, width=max(2, int(round(1.6 * S * scale))))
    draw.line([nx + r - 1, ny - stem_h, nx + r + int(round(4 * S * scale)), ny - stem_h + int(round(2.5 * S * scale))],
              fill=c, width=max(2, int(round(1.8 * S * scale))))


def draw_bolt_icon(draw: ImageDraw.ImageDraw, cx: int, cy: int, col: int, sz: float = 1.0) -> None:
    c = hex_rgb(col)
    pts = [
        ((cx + 1.5 * sz) * S, (cy - 6 * sz) * S),
        ((cx - 3.5 * sz) * S, (cy + 0.5 * sz) * S),
        ((cx + 0.2 * sz) * S, (cy + 0.5 * sz) * S),
        ((cx - 1.5 * sz) * S, (cy + 6 * sz) * S),
        ((cx + 3.5 * sz) * S, (cy - 0.5 * sz) * S),
        ((cx - 0.2 * sz) * S, (cy - 0.5 * sz) * S),
    ]
    draw.polygon(pts, fill=c)


def draw_status_bar(draw: ImageDraw.ImageDraw, audio_playing: bool) -> None:
    draw.rectangle([0, 0, W, 24 * S], fill=hex_rgb(0x060913))
    text_tl(draw, 8, 5, "10:42", font(11), 0xF8FAFC)
    text_tl(draw, 54, 5, "SWIFT OS", font(11), 0x38BDF8)
    if audio_playing:
        rr(draw, 128, 2, 26, 20, 6, 0x162038)
        draw_music_note(draw, 141, 13, 0x4ADE80, scale=0.85)
    draw_sd_icon(draw, 300, 6, 0x94A3B8)
    draw_wifi_icon(draw, 282, 7, 0x38BDF8)


def draw_tile_glyph(draw: ImageDraw.ImageDraw, kind: str, cx: int, cy: int) -> None:
    c = hex_rgb(0xFFFFFF)
    if kind == "music":
        draw_music_note(draw, cx - 2, cy + 1, 0xFFFFFF, scale=1.4)
    elif kind == "probe":
        draw_bolt_icon(draw, cx, cy, 0xFFFFFF, sz=1.5)
    elif kind == "store":
        draw.rectangle([(cx - 7) * S, (cy - 5) * S, (cx + 7) * S, (cy + 7) * S], outline=c, width=3)
        draw.line([cx * S, (cy - 9) * S, cx * S, (cy + 2) * S], fill=c, width=3)
        draw.line([(cx - 4) * S, (cy - 1) * S, cx * S, (cy + 3) * S, (cx + 4) * S, (cy - 1) * S], fill=c, width=3)
    elif kind == "files":
        draw.rectangle([(cx - 8) * S, (cy - 6) * S, (cx - 1) * S, (cy - 3) * S], fill=c)
        draw.rounded_rectangle([(cx - 8) * S, (cy - 4) * S, (cx + 8) * S, (cy + 7) * S], radius=2 * S, fill=c)
    elif kind == "settings":
        draw.ellipse([(cx - 7) * S, (cy - 7) * S, (cx + 7) * S, (cy + 7) * S], outline=c, width=4)
        draw.ellipse([(cx - 2) * S, (cy - 2) * S, (cx + 2) * S, (cy + 2) * S], fill=c)
    elif kind == "clock":
        draw.ellipse([(cx - 8) * S, (cy - 8) * S, (cx + 8) * S, (cy + 8) * S], outline=c, width=3)
        draw.line([cx * S, cy * S, cx * S, (cy - 5) * S], fill=c, width=3)
        draw.line([cx * S, cy * S, (cx + 4) * S, cy * S], fill=c, width=3)


def render_home(path: str) -> None:
    img = Image.new("RGB", (W, H), hex_rgb(0x0B1020))
    draw = ImageDraw.Draw(img)
    draw_status_bar(draw, audio_playing=True)

    tiles = [
        ("iSwift 音乐", "TF", "music", 0xE60012),
        ("PSRAM 探针", "TF", "probe", 0x0EA5E9),
        ("应用中心", "OS", "store", 0x0284C7),
        ("文件管理", "OS", "files", 0xD97706),
        ("系统设置", "OS", "settings", 0x475569),
        ("番茄时钟", "OS", "clock", 0x16A34A),
    ]
    for idx, (title, badge, kind, accent) in enumerate(tiles):
        r = idx // 3
        c = idx % 3
        x = 10 + c * (94 + 8)
        y = 24 + 8 + r * (86 + 8)
        rr(draw, x, y, 94, 86, 12, 0x151D32, outline=0x253250, width=1)
        rr(draw, x + 26, y + 8, 42, 42, 12, accent)
        draw_tile_glyph(draw, kind, x + 47, y + 29)
        text_tl(draw, x + 74, y + 6, badge, font(10), 0x38BDF8)
        text_center(draw, x + 47, y + 68, title, font(12), 0xE2E8F0)

    rr(draw, 315, 32, 3, 140, 2, 0x334155)
    img.save(path)


def render_music_player_1to1(path: str) -> None:
    """1:1 replica of playpal_iswift_hub Now Playing View (ui_manager.c lines 1904-2349)."""
    img = Image.new("RGB", (W, H), hex_rgb(0x000000))
    draw = ImageDraw.Draw(img)

    # Left column (0..120)
    # Status dot (10, 13, 6x6) + label (20, 9) + [< 选歌] chip (60, 6, 50x20)
    draw.ellipse([10 * S, 13 * S, 16 * S, 19 * S], fill=hex_rgb(0x0A84FF))
    text_tl(draw, 20, 8, "ONLINE", font(9), 0xB3B3B3)
    rr(draw, 60, 6, 50, 20, 4, 0x181818, outline=0x2A2A2A, width=1)
    text_center(draw, 85, 16, "< 选歌", font(10), 0xFFFFFF)

    # Album cover card (10, 30, 100x96, r=12) with Lover-style pastel gradient art
    card = Image.new("RGB", (100 * S, 96 * S))
    cdraw = ImageDraw.Draw(card)
    for py in range(96 * S):
        t = py / (96 * S - 1)
        r_col = int(242 * (1 - t) + 178 * t)
        g_col = int(176 * (1 - t) + 218 * t)
        b_col = int(218 * (1 - t) + 252 * t)
        cdraw.line([0, py, 100 * S - 1, py], fill=(r_col, g_col, b_col))
    cdraw.text((50 * S, 40 * S), "Lover", font=font(18), fill=hex_rgb(0xD80066), anchor="mm")
    cdraw.text((50 * S, 58 * S), "Taylor Swift", font=font(9), fill=hex_rgb(0x6D4A72), anchor="mm")

    mask = Image.new("L", (100 * S, 96 * S), 0)
    ImageDraw.Draw(mask).rounded_rectangle([0, 0, 100 * S - 1, 96 * S - 1], radius=12 * S, fill=255)
    img.paste(card, (10 * S, 30 * S), mask)
    rr(draw, 10, 30, 100, 96, 12, fill=None, outline=0x2A2A2A, width=1)

    # Title (10, 134), Artist (10, 156), Bottom HUD chip (10, 205)
    text_tl(draw, 10, 134, "Cruel Summer", font(13), 0xFFFFFF)
    text_tl(draw, 10, 156, "Taylor Swift", font(11), 0x7A7A7A)
    draw.rectangle([10 * S, 205 * S, 13 * S, 219 * S], fill=hex_rgb(0xE60012))
    text_tl(draw, 17, 204, "iSwift AUDIO", font(10), 0x7A7A7A)

    # Vertical separator (120, 8..232)
    draw.line([120 * S, 8 * S, 120 * S, 232 * S], fill=hex_rgb(0x161616), width=S)

    # Right column (x=121..320)
    # Scrollable Lyrics Box (x=131, y=4, w=181, h=142)
    rx = 135
    text_tl(draw, rx, 10, "Fever dream high in the quiet", font(11), 0x7A7A7A)
    text_tl(draw, rx, 26, "of the night, you know that I", font(11), 0x7A7A7A)
    text_tl(draw, rx, 56, "And it's blue, the feeling I got", font(11), 0xFFFFFF)
    text_tl(draw, rx, 74, "Ooh whoa-oh, it's a cruel summer", font(11), 0xFFFFFF)
    text_tl(draw, rx, 104, "Killing me slow, out the window", font(11), 0x7A7A7A)

    # Bottom Controls (y=150..240)
    bx = 121
    by = 150
    text_tl(draw, bx + 12, by + 6, "01:45", font(10), 0x7A7A7A)
    text_tl(draw, bx + 157, by + 6, "03:50", font(10), 0x7A7A7A)
    # Progress bar (50, 12, 99x3)
    rr(draw, bx + 50, by + 12, 99, 3, 2, 0x2A2A2A)
    rr(draw, bx + 50, by + 12, 46, 3, 2, 0xE60012)

    # Prev button (12, 42, 34x34)
    pcx, pcy = bx + 29, by + 59
    draw.line([(pcx - 6) * S, (pcy - 5) * S, (pcx - 6) * S, (pcy + 5) * S], fill=hex_rgb(0xB3B3B3), width=3)
    draw.polygon([((pcx - 4) * S, pcy * S), ((pcx + 4) * S, (pcy - 5) * S), ((pcx + 4) * S, (pcy + 5) * S)], fill=hex_rgb(0xB3B3B3))
    draw.polygon([((pcx + 2) * S, pcy * S), ((pcx + 9) * S, (pcy - 5) * S), ((pcx + 9) * S, (pcy + 5) * S)], fill=hex_rgb(0xB3B3B3))

    # Play/Pause circle button (57, 39, 40x40)
    rr(draw, bx + 57, by + 39, 40, 40, 20, 0x121212, outline=0x2A2A2A, width=1)
    mcx, mcy = bx + 77, by + 59
    draw.rectangle([(mcx - 5) * S, (mcy - 6) * S, (mcx - 2) * S, (mcy + 6) * S], fill=hex_rgb(0xFFFFFF))
    draw.rectangle([(mcx + 2) * S, (mcy - 6) * S, (mcx + 5) * S, (mcy + 6) * S], fill=hex_rgb(0xFFFFFF))

    # Next button (108, 42, 34x34)
    ncx, ncy = bx + 125, by + 59
    draw.polygon([((ncx - 8) * S, (ncy - 5) * S), ((ncx - 8) * S, (ncy + 5) * S), ((ncx - 1) * S, ncy * S)], fill=hex_rgb(0xB3B3B3))
    draw.polygon([((ncx - 2) * S, (ncy - 5) * S), ((ncx - 2) * S, (ncy + 5) * S), ((ncx + 5) * S, ncy * S)], fill=hex_rgb(0xB3B3B3))
    draw.line([(ncx + 6) * S, (ncy - 5) * S, (ncx + 6) * S, (ncy + 5) * S], fill=hex_rgb(0xB3B3B3), width=3)

    # Favorite button (153, 42, 34x34)
    text_center(draw, bx + 170, by + 59, "已藏", font(11), 0xE60012)

    img.save(path)


def render_music_tabs_1to1(path: str) -> None:
    """1:1 replica of playpal_iswift_hub 3-Tab Home View with [桌面] return button."""
    img = Image.new("RGB", (W, H), hex_rgb(0x000000))
    draw = ImageDraw.Draw(img)

    # Top tab bar (0, 0, 320x34)
    draw.rectangle([0, 0, W, 34 * S], fill=hex_rgb(0x121212))
    draw.line([0, 34 * S, W, 34 * S], fill=hex_rgb(0x2A2A2A), width=S)

    tabs = ["在线选歌", "本地音乐", "收藏音乐"]
    for i, name in enumerate(tabs):
        x = 4 + i * 64
        sel = (i == 0)
        rr(draw, x, 4, 61, 25, 6, 0xE60012 if sel else 0x181818,
           outline=0xE60012 if sel else 0x2A2A2A, width=1)
        text_center(draw, x + 30, 16, name, font(11), 0xFFFFFF)

    rr(draw, 198, 4, 36, 25, 6, 0x181818, outline=0x28CD41, width=1)
    text_center(draw, 216, 16, "已连", font(11), 0x28CD41)

    rr(draw, 238, 4, 36, 25, 6, 0x181818, outline=0x2A2A2A, width=1)
    text_center(draw, 256, 16, "播放", font(11), 0xFFFFFF)

    rr(draw, 278, 4, 38, 25, 6, 0x1E293B, outline=0x2A2A2A, width=1)
    text_center(draw, 297, 16, "桌面", font(11), 0xFFFFFF)

    # Page 0: 在线选歌 (y=34..240)
    py = 34
    rr(draw, 8, py + 4, 244, 28, 6, 0x121212, outline=0x2A2A2A, width=1)
    text_tl(draw, 14, py + 10, "输入拼音/英文搜歌 (如 qingtian)", font(11), 0x7A7A7A)

    rr(draw, 258, py + 4, 54, 28, 6, 0xE60012)
    text_center(draw, 285, py + 18, "搜索", font(11), 0xFFFFFF)

    # Quick chips row (y=36, h=26)
    chips = [("热歌榜", 54, True), ("周杰伦", 54, False), ("林俊杰", 54, False), ("陈奕迅", 54, False), ("邓紫棋", 54, False)]
    cx = 8
    for cname, cw, active in chips:
        rr(draw, cx, py + 36, cw, 24, 12, 0xE60012 if active else 0x181818)
        text_center(draw, cx + cw // 2, py + 48, cname, font(11), 0xFFFFFF)
        cx += cw + 6

    text_tl(draw, 8, py + 64, "网易云热歌榜 (已加载 20 首)", font(11), 0x7A7A7A)

    # Song rows (y=82..)
    songs = [
        ("1. Cruel Summer - Taylor Swift", "已藏", True, True),
        ("2. 晴天 - 周杰伦", "收藏", False, False),
        ("3. 夜曲 - 周杰伦", "已藏", True, False),
        ("4. 江南 - 林俊杰", "收藏", False, False),
    ]
    for i, (title, rtxt, r_acc, active) in enumerate(songs):
        ry = py + 82 + i * 34
        if ry + 28 > 240:
            continue
        rr(draw, 8, ry, 254, 28, 5, 0x4A050A if active else 0x121212,
           outline=0xE60012 if active else 0x2A2A2A, width=1)
        text_tl(draw, 14, ry + 6, title, font(11), 0xFFFFFF)

        rr(draw, 266, ry, 46, 28, 5, 0xE60012 if r_acc else 0x181818,
           outline=0xE60012 if r_acc else 0x2A2A2A, width=1)
        text_center(draw, 289, ry + 14, rtxt, font(11), 0xFFFFFF)

    img.save(path)


def main() -> None:
    os.makedirs(OUT_DIR, exist_ok=True)
    os.makedirs(ARTIFACT_DIR, exist_ok=True)

    outputs = [
        ("ui_preview_1_home.png", render_home),
        ("ui_preview_2_music_elf.png", render_music_player_1to1),
        ("ui_preview_3_music_tabs.png", render_music_tabs_1to1),
    ]
    for fname, fn in outputs:
        p1 = os.path.join(OUT_DIR, fname)
        p2 = os.path.join(ARTIFACT_DIR, fname)
        fn(p1)
        shutil.copy2(p1, p2)
        print(f"[OK] Rendered {p1} -> copied to {p2}")


if __name__ == "__main__":
    main()
