"""
Render clean, modern, normal-palette pure-Chinese UI preview sheets for G'lam OS.

Outputs 5 high-DPI (960x720, 3x supersampled from 320x240 logical coordinates)
PNGs directly to docs/images/ so previews match os_ui.c verbatim:
  - ui_v2_1_bento_home.png
  - ui_v2_2_music_studio.png
  - ui_v2_3_music_discovery.png
  - ui_v2_4_apps_quad_showcase.png
  - ui_v2_5_theme_palettes.png
"""

from dataclasses import dataclass
import math
import os
from typing import Tuple

from PIL import Image, ImageDraw, ImageFont


S = 3
LOGICAL_W = 320
LOGICAL_H = 240
W = LOGICAL_W * S  # 960
H = LOGICAL_H * S  # 720

OUT_DIR = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "docs", "images"))

FONT_REG_PATH = r"C:\Windows\Fonts\msyh.ttc"
FONT_BOLD_PATH = r"C:\Windows\Fonts\msyhbd.ttc"
_FONT_CACHE = {}


def get_font(pt: float, bold: bool = False) -> ImageFont.FreeTypeFont:
    px = max(8, int(round(pt * S)))
    key = (px, bold)
    if key not in _FONT_CACHE:
        path = FONT_BOLD_PATH if bold else FONT_REG_PATH
        try:
            _FONT_CACHE[key] = ImageFont.truetype(path, px)
        except OSError:
            _FONT_CACHE[key] = ImageFont.load_default()
    return _FONT_CACHE[key]


def rgba(hex_col: int, alpha: int = 255) -> Tuple[int, int, int, int]:
    return ((hex_col >> 16) & 0xFF, (hex_col >> 8) & 0xFF, hex_col & 0xFF, alpha)


def lerp_col(c1: int, c2: int, t: float) -> Tuple[int, int, int]:
    r1, g1, b1 = (c1 >> 16) & 0xFF, (c1 >> 8) & 0xFF, c1 & 0xFF
    r2, g2, b2 = (c2 >> 16) & 0xFF, (c2 >> 8) & 0xFF, c2 & 0xFF
    return (
        int(r1 + (r2 - r1) * t),
        int(g1 + (g2 - g1) * t),
        int(b1 + (b2 - b1) * t),
    )


def txt(
    draw: ImageDraw.ImageDraw,
    x: float,
    y: float,
    text: str,
    pt: float,
    col: int,
    bold: bool = False,
    anchor: str = "lt",
    alpha: int = 255,
    scale: int = S,
):
    font = get_font(pt * (scale / S), bold=bold)
    draw.text((x * scale, y * scale), text, font=font, fill=rgba(col, alpha), anchor=anchor)


def draw_rr(
    canvas: Image.Image,
    x: float,
    y: float,
    w: float,
    h: float,
    r: float,
    fill_top: int,
    fill_bot: int = None,
    outline: int = None,
    outline_alpha: int = 255,
    top_hl: int = None,
    top_hl_alpha: int = 40,
    lip_h: float = 0.0,
    lip_col: int = 0x0B0E14,
    scale: int = S,
):
    ix = int(round(x * scale))
    iy = int(round(y * scale))
    iw = max(1, int(round(w * scale)))
    ih = max(1, int(round(h * scale)))
    ir = max(1, int(round(r * scale)))

    if lip_h > 0:
        lip_px = max(1, int(round(lip_h * scale)))
        overlay_lip = Image.new("RGBA", (iw, ih + lip_px), (0, 0, 0, 0))
        d_lip = ImageDraw.Draw(overlay_lip)
        d_lip.rounded_rectangle([0, lip_px, iw - 1, ih + lip_px - 1], radius=ir, fill=rgba(lip_col, 255))
        canvas.alpha_composite(overlay_lip, (ix, iy))

    mask = Image.new("L", (iw, ih), 0)
    ImageDraw.Draw(mask).rounded_rectangle([0, 0, iw - 1, ih - 1], radius=ir, fill=255)

    rect_img = Image.new("RGBA", (iw, ih), (0, 0, 0, 0))
    if fill_bot is None or fill_bot == fill_top:
        ImageDraw.Draw(rect_img).rectangle([0, 0, iw - 1, ih - 1], fill=rgba(fill_top, 255))
    else:
        dr = ImageDraw.Draw(rect_img)
        for row in range(ih):
            c = lerp_col(fill_top, fill_bot, row / max(1, ih - 1))
            dr.line([(0, row), (iw - 1, row)], fill=(c[0], c[1], c[2], 255))

    if top_hl is not None and top_hl_alpha > 0:
        dr = ImageDraw.Draw(rect_img)
        dr.line([(ir // 2, 1), (iw - 1 - ir // 2, 1)], fill=rgba(top_hl, top_hl_alpha), width=max(1, scale // 2))

    rect_img.putalpha(mask)
    canvas.alpha_composite(rect_img, (ix, iy))

    if outline is not None:
        ol_img = Image.new("RGBA", (iw, ih), (0, 0, 0, 0))
        ImageDraw.Draw(ol_img).rounded_rectangle(
            [0, 0, iw - 1, ih - 1],
            radius=ir,
            outline=rgba(outline, outline_alpha),
            width=max(1, scale // 2),
        )
        canvas.alpha_composite(ol_img, (ix, iy))


@dataclass
class Theme:
    name: str
    sub: str
    is_light: bool
    bg_top: int
    bg_bot: int
    card_top: int
    card_bot: int
    card_border: int
    card_hi: int
    alt_top: int
    alt_bot: int
    alt_border: int
    text_pri: int
    text_sec: int
    text_mut: int
    accent: int
    accent_dark: int
    accent_tint: int
    accent_border: int
    status_ok: int
    dock_bg: int
    dock_border: int
    lcd_top: int
    lcd_bot: int
    lcd_border: int
    lcd_fg: int
    lcd_sub: int
    lip_shadow: int


THEME_CLASSIC_DARK = Theme(
    name="经典深色",
    sub="默认主题 · 深空蓝灰表面 + 标准系统蓝",
    is_light=False,
    bg_top=0x12151C,
    bg_bot=0x161A22,
    card_top=0x1E232E,
    card_bot=0x1A1E28,
    card_border=0x2C3342,
    card_hi=0x394356,
    alt_top=0x232936,
    alt_bot=0x1B202B,
    alt_border=0x333B4D,
    text_pri=0xF3F5F8,
    text_sec=0x9AA3B2,
    text_mut=0x687282,
    accent=0x2E7CF6,
    accent_dark=0x1D63D8,
    accent_tint=0x192844,
    accent_border=0x2A5699,
    status_ok=0x2DBE60,
    dock_bg=0x161A22,
    dock_border=0x29303F,
    lcd_top=0x151820,
    lcd_bot=0x12151C,
    lcd_border=0x252B38,
    lcd_fg=0xF3F5F8,
    lcd_sub=0x9AA3B2,
    lip_shadow=0x0B0E14,
)

THEME_CLEAN_LIGHT = Theme(
    name="初雪",
    sub="日间主题 · 纯净浅灰白底 + 标准系统蓝",
    is_light=True,
    bg_top=0xF2F4F8,
    bg_bot=0xEAEFF5,
    card_top=0xFFFFFF,
    card_bot=0xF8FAFC,
    card_border=0xDCE2EC,
    card_hi=0xFFFFFF,
    alt_top=0xEBF0F6,
    alt_bot=0xE2E8F0,
    alt_border=0xD5DCE8,
    text_pri=0x111827,
    text_sec=0x526071,
    text_mut=0x8492A6,
    accent=0x2563EB,
    accent_dark=0x1D4ED8,
    accent_tint=0xDBEAFE,
    accent_border=0x93C5FD,
    status_ok=0x16A34A,
    dock_bg=0xFFFFFF,
    dock_border=0xDCE2EC,
    lcd_top=0xEBF0F6,
    lcd_bot=0xE2E8F0,
    lcd_border=0xD5DCE8,
    lcd_fg=0x111827,
    lcd_sub=0x526071,
    lip_shadow=0xC2CBDA,
)

THEME_SOFT_TEAL = Theme(
    name="深邃",
    sub="OLED 主题 · 纯黑基底 + 澄澈系统蓝",
    is_light=False,
    bg_top=0x000000,
    bg_bot=0x000000,
    card_top=0x121214,
    card_bot=0x0C0C0E,
    card_border=0x26262A,
    card_hi=0x303036,
    alt_top=0x1A1A1E,
    alt_bot=0x121214,
    alt_border=0x2A2A30,
    text_pri=0xFFFFFF,
    text_sec=0xA1A1AA,
    text_mut=0x666670,
    accent=0x3B82F6,
    accent_dark=0x2563EB,
    accent_tint=0x0D1B33,
    accent_border=0x254E8C,
    status_ok=0x22C55E,
    dock_bg=0x000000,
    dock_border=0x26262A,
    lcd_top=0x08080A,
    lcd_bot=0x050507,
    lcd_border=0x202024,
    lcd_fg=0xF1F5F4,
    lcd_sub=0x95A5A2,
    lip_shadow=0x0B0E0F,
)


APP_ICON_COLORS = {
    "music": (0xE5484D, 0xCC353A, 0xF06A6E),
    "pomo": (0xEB6A3E, 0xD15326, 0xF2845E),
    "calc": (0xE59524, 0xC87D14, 0xEEAA49),
    "apps": (0x2E7CF6, 0x1D63D8, 0x5596F8),
    "files": (0x3898EC, 0x247BC7, 0x5EAEF2),
    "settings": (0x5E6C7E, 0x485464, 0x768598),
    "bg": (0x6366F1, 0x4B4EDB, 0x8083F5),
    "probe": (0x29A383, 0x1E876B, 0x46B89A),
    "racer": (0x5B5BD6, 0x4747BA, 0x7676E0),
}


def make_bg(theme: Theme, w: int = W, h: int = H) -> Image.Image:
    """Clean, natural vertical gradient background without neon glare."""
    canvas = Image.new("RGBA", (w, h), (0, 0, 0, 255))
    dr = ImageDraw.Draw(canvas)
    for row in range(h):
        c = lerp_col(theme.bg_top, theme.bg_bot, row / max(1, h - 1))
        dr.line([(0, row), (w - 1, row)], fill=(c[0], c[1], c[2], 255))
    return canvas


def draw_wifi_icon(draw: ImageDraw.ImageDraw, x: float, y: float, col: int, scale: int = S):
    cx, cy = x * scale, (y + 3.5) * scale
    for r in (2.5, 4.8, 7.0):
        rr = r * scale
        draw.arc([cx - rr, cy - rr, cx + rr, cy + rr], 225, 315, fill=rgba(col, 240), width=max(1, int(scale * 0.65)))
    dot_r = 0.9 * scale
    draw.ellipse([cx - dot_r, cy - dot_r, cx + dot_r, cy + dot_r], fill=rgba(col, 255))


def draw_sd_icon(draw: ImageDraw.ImageDraw, x: float, y: float, col: int, scale: int = S):
    pts = [
        (x * scale, (y + 2) * scale),
        ((x + 2) * scale, y * scale),
        ((x + 7.5) * scale, y * scale),
        ((x + 7.5) * scale, (y + 9.5) * scale),
        (x * scale, (y + 9.5) * scale),
    ]
    draw.polygon(pts, outline=rgba(col, 235), width=max(1, scale // 2))
    for i in range(3):
        px = (x + 2.8 + i * 1.6) * scale
        draw.line([px, (y + 1.8) * scale, px, (y + 4.0) * scale], fill=rgba(col, 210), width=max(1, scale // 2))


def draw_eq_bars(draw: ImageDraw.ImageDraw, x: float, y: float, col: int, scale: int = S):
    heights = (4.0, 7.5, 5.2, 6.4)
    for i, bh in enumerate(heights):
        bx = (x + i * 2.6) * scale
        by1 = (y + (8.0 - bh)) * scale
        by2 = (y + 8.0) * scale
        draw.rounded_rectangle([bx, by1, bx + 1.5 * scale, by2], radius=max(1, scale // 2), fill=rgba(col, 240))


def draw_white_glyph(draw: ImageDraw.ImageDraw, cx: float, cy: float, kind: str, s_unit: float = 1.0, scale: int = S):
    """Draw clean pure-white vector symbol centered inside colored icon badge."""
    w_line = max(2, int(1.35 * s_unit * scale))
    fg = (255, 255, 255, 255)
    fg_soft = (255, 255, 255, 220)
    if kind == "music":
        for nx in (cx - 4.0 * s_unit, cx + 3.0 * s_unit):
            draw.ellipse(
                [(nx - 2.5 * s_unit) * scale, (cy + 1.8 * s_unit) * scale, (nx + 2.3 * s_unit) * scale, (cy + 6.2 * s_unit) * scale],
                fill=fg,
            )
            draw.line(
                [(nx + 1.9 * s_unit) * scale, (cy + 3.2 * s_unit) * scale, (nx + 1.9 * s_unit) * scale, (cy - 5.2 * s_unit) * scale],
                fill=fg,
                width=w_line,
            )
        draw.line(
            [(cx - 2.1 * s_unit) * scale, (cy - 5.2 * s_unit) * scale, (cx + 4.9 * s_unit) * scale, (cy - 6.8 * s_unit) * scale],
            fill=fg,
            width=max(2, int(1.9 * s_unit * scale)),
        )
    elif kind == "pomo":
        r = 6.0 * s_unit * scale
        draw.ellipse([cx * scale - r, cy * scale - r, cx * scale + r, cy * scale + r], outline=fg, width=w_line)
        draw.line([cx * scale, cy * scale, cx * scale, (cy - 3.6 * s_unit) * scale], fill=fg, width=w_line)
        draw.line([cx * scale, cy * scale, (cx + 3.2 * s_unit) * scale, (cy + 1.5 * s_unit) * scale], fill=fg_soft, width=w_line)
        draw.line([(cx - 2.0 * s_unit) * scale, (cy - 7.6 * s_unit) * scale, (cx + 2.0 * s_unit) * scale, (cy - 7.6 * s_unit) * scale], fill=fg, width=w_line)
    elif kind == "calc":
        for r_idx in range(2):
            for c_idx in range(2):
                bx = cx - 5.0 * s_unit + c_idx * 5.8 * s_unit
                by = cy - 5.0 * s_unit + r_idx * 5.8 * s_unit
                col_cell = fg if (r_idx == 1 and c_idx == 1) else fg_soft
                draw.rounded_rectangle(
                    [bx * scale, by * scale, (bx + 4.2 * s_unit) * scale, (by + 4.2 * s_unit) * scale],
                    radius=max(1, int(scale * s_unit)),
                    fill=col_cell,
                )
    elif kind == "apps":
        for r_idx in range(2):
            for c_idx in range(2):
                bx = cx - 4.4 * s_unit + c_idx * 5.0 * s_unit
                by = cy - 4.4 * s_unit + r_idx * 5.0 * s_unit
                draw.rounded_rectangle(
                    [bx * scale, by * scale, (bx + 3.8 * s_unit) * scale, (by + 3.8 * s_unit) * scale],
                    radius=max(1, int(scale * s_unit)),
                    fill=fg,
                )
    elif kind == "files":
        pts = [
            ((cx - 5.4 * s_unit) * scale, (cy - 4.2 * s_unit) * scale),
            ((cx - 1.5 * s_unit) * scale, (cy - 4.2 * s_unit) * scale),
            ((cx - 0.1 * s_unit) * scale, (cy - 2.3 * s_unit) * scale),
            ((cx + 5.4 * s_unit) * scale, (cy - 2.3 * s_unit) * scale),
            ((cx + 5.4 * s_unit) * scale, (cy + 4.4 * s_unit) * scale),
            ((cx - 5.4 * s_unit) * scale, (cy + 4.4 * s_unit) * scale),
        ]
        draw.polygon(pts, fill=fg)
    elif kind == "settings":
        r_out = 4.6 * s_unit * scale
        r_in = 1.8 * s_unit * scale
        draw.ellipse([cx * scale - r_out, cy * scale - r_out, cx * scale + r_out, cy * scale + r_out], outline=fg, width=max(2, int(1.6 * s_unit * scale)))
        draw.ellipse([cx * scale - r_in, cy * scale - r_in, cx * scale + r_in, cy * scale + r_in], fill=fg)
    elif kind == "probe":
        pts = [
            ((cx - 6.2 * s_unit) * scale, (cy + 1.0 * s_unit) * scale),
            ((cx - 3.0 * s_unit) * scale, (cy + 1.0 * s_unit) * scale),
            ((cx - 1.1 * s_unit) * scale, (cy - 4.8 * s_unit) * scale),
            ((cx + 1.5 * s_unit) * scale, (cy + 4.6 * s_unit) * scale),
            ((cx + 3.6 * s_unit) * scale, (cy - 1.5 * s_unit) * scale),
            ((cx + 6.2 * s_unit) * scale, (cy - 1.5 * s_unit) * scale),
        ]
        draw.line(pts, fill=fg, width=w_line)


def draw_app_icon(canvas: Image.Image, x: float, y: float, size: float, kind: str, scale: int = S) -> ImageDraw.ImageDraw:
    """Standard modern colored rounded app icon badge + crisp white vector glyph."""
    top_col, bot_col, bdr_col = APP_ICON_COLORS.get(kind, APP_ICON_COLORS["apps"])
    draw_rr(
        canvas, x, y, size, size, size * 0.25,
        fill_top=top_col, fill_bot=bot_col,
        outline=bdr_col, top_hl=0xFFFFFF, top_hl_alpha=70,
        scale=scale,
    )
    d = ImageDraw.Draw(canvas)
    draw_white_glyph(d, x + size / 2.0, y + size / 2.0, kind, s_unit=size / 32.0, scale=scale)
    return d


def draw_custom_racer_icon(canvas: Image.Image, x: float, y: float, size: float, scale: int = S) -> ImageDraw.ImageDraw:
    """Represents an App's own custom icon.jpg (loaded verbatim from app package)."""
    draw_rr(canvas, x, y, size, size, size * 0.24, fill_top=0x334155, fill_bot=0x1E293B, outline=0x64748B, top_hl=0xFFFFFF, top_hl_alpha=55, scale=scale)
    d = ImageDraw.Draw(canvas)
    cx, cy = x + size / 2.0, y + size / 2.0
    d.arc(
        [(cx - size * 0.34) * scale, (cy - size * 0.22) * scale, (cx + size * 0.34) * scale, (cy + size * 0.48) * scale],
        190, 350, fill=(203, 213, 225, 220), width=max(2, int(size * 0.14 * scale)),
    )
    cw, ch = size * 0.44, size * 0.22
    d.rounded_rectangle([(cx - cw / 2) * scale, (cy - ch / 2 + 1) * scale, (cx + cw / 2) * scale, (cy + ch / 2 + 1) * scale], radius=max(2, int(2.2 * scale)), fill=(229, 72, 77, 255))
    d.rounded_rectangle([(cx - size * 0.11) * scale, (cy - size * 0.16) * scale, (cx + size * 0.11) * scale, (cy + 1) * scale], radius=max(1, int(1.4 * scale)), fill=(248, 250, 252, 255))
    for wx in (cx - size * 0.16, cx + size * 0.08):
        d.rectangle([wx * scale, (cy + size * 0.11) * scale, (wx + size * 0.09) * scale, (cy + size * 0.18) * scale], fill=(15, 23, 42, 255))
    return d


def draw_status_bar(canvas: Image.Image, draw: ImageDraw.ImageDraw, theme: Theme, title: str = "系统桌面"):
    bar_bg = theme.card_top if theme.is_light else 0x0F1218
    if theme.name == "深邃":
        bar_bg = 0x000000
    draw_rr(canvas, 0, 0, 320, 22, 0, fill_top=bar_bg, fill_bot=bar_bg, scale=S)
    draw.line([0, 22 * S, 320 * S, 22 * S], fill=rgba(theme.card_border, 190), width=max(1, S // 2))
    draw.ellipse([10 * S, 9 * S, 14 * S, 13 * S], fill=rgba(theme.accent, 255))
    txt(draw, 19, 11, title, 8.2, theme.text_pri, bold=True, anchor="lm", scale=S)

    draw_wifi_icon(draw, 222, 7.5, theme.text_sec, scale=S)
    txt(draw, 258, 11, "无线已连", 7.2, theme.text_sec, anchor="rm", scale=S)
    draw_sd_icon(draw, 266, 6.2, theme.status_ok, scale=S)
    txt(draw, 310, 11, "存储就绪", 7.2, theme.status_ok, bold=True, anchor="rm", scale=S)


def draw_subpage_topbar(canvas: Image.Image, draw: ImageDraw.ImageDraw, theme: Theme, title: str, right_text: str = ""):
    draw_rr(canvas, 0, 0, 320, 24, 0, fill_top=theme.card_bot, fill_bot=theme.card_bot, scale=S)
    draw.line([0, 24 * S, 320 * S, 24 * S], fill=rgba(theme.card_border, 200), width=max(1, S // 2))
    draw_rr(canvas, 8, 4, 42, 16, 8, fill_top=theme.alt_top, fill_bot=theme.alt_bot, outline=theme.alt_border, scale=S)
    txt(draw, 29, 12, "< 主页", 7.2, theme.text_pri, bold=True, anchor="mm", scale=S)
    txt(draw, 60, 12, title, 8.6, theme.text_pri, bold=True, anchor="lm", scale=S)
    if right_text:
        dot_x = 302 - len(right_text) * 7.0
        draw.ellipse([dot_x * S, 10 * S, (dot_x + 4) * S, 14 * S], fill=rgba(theme.status_ok, 255))
        txt(draw, 310, 12, right_text, 7.2, theme.text_sec, anchor="rm", scale=S)


def render_home_screen(theme: Theme) -> Image.Image:
    canvas = make_bg(theme, W, H)
    draw = ImageDraw.Draw(canvas)
    draw_status_bar(canvas, draw, theme, "系统桌面")

    # Left Column (x=10, w=146): Clock Card + Now Playing Card
    draw_rr(
        canvas, 10, 28, 146, 78, 10,
        fill_top=theme.card_top, fill_bot=theme.card_bot,
        outline=theme.card_border, top_hl=theme.card_hi, top_hl_alpha=60,
        scale=S,
    )
    txt(draw, 20, 34, "10:42", 24.5, theme.text_pri, bold=True, anchor="lt", scale=S)
    draw_rr(canvas, 104, 38, 42, 16, 8, fill_top=theme.accent_tint, fill_bot=theme.accent_tint, outline=theme.accent_border, scale=S)
    txt(draw, 125, 46, "第1轮", 6.8, theme.accent, bold=True, anchor="mm", scale=S)
    txt(draw, 20, 88, "11月18日  周三", 8.0, theme.text_sec, anchor="lm", scale=S)
    txt(draw, 146, 88, "专注待机", 7.0, theme.text_mut, anchor="rm", scale=S)

    # Media Now Playing Card (x=10, y=114, w=146, h=76)
    draw_rr(
        canvas, 10, 114, 146, 76, 10,
        fill_top=theme.card_top, fill_bot=theme.card_bot,
        outline=theme.card_border, top_hl=theme.card_hi, top_hl_alpha=60,
        scale=S,
    )
    draw_eq_bars(draw, 20, 124, theme.accent, scale=S)
    txt(draw, 34, 128, "正在播放", 7.4, theme.accent, bold=True, anchor="lm", scale=S)
    txt(draw, 20, 147, "晴天 · 周杰伦", 10.2, theme.text_pri, bold=True, anchor="lm", scale=S)
    txt(draw, 20, 164, "独立音乐  02:14 / 04:36", 7.2, theme.text_sec, anchor="lm", scale=S)
    draw_rr(canvas, 20, 177, 126, 4, 2, fill_top=theme.alt_bot, fill_bot=theme.alt_bot, scale=S)
    draw_rr(canvas, 20, 177, 64, 4, 2, fill_top=theme.accent, fill_bot=theme.accent_dark, scale=S)

    # Right Column (x=164, w=146): 2x2 Bento App Tiles with normal colored icon badges
    tiles = [
        ("音乐", "music", False, 164, 28),
        ("番茄钟", "pomo", False, 241, 28),
        ("计算器", "calc", False, 164, 114),
        ("极速赛车", "racer", True, 241, 114),
    ]
    for label, kind, is_custom, tx, ty in tiles:
        draw_rr(
            canvas, tx, ty, 69, 76, 10,
            fill_top=theme.card_top, fill_bot=theme.card_bot,
            outline=theme.card_border, top_hl=theme.card_hi, top_hl_alpha=60,
            scale=S,
        )
        cx = tx + 34.5
        if is_custom:
            draw = draw_custom_racer_icon(canvas, cx - 17, ty + 11, 34, scale=S)
        else:
            draw = draw_app_icon(canvas, cx - 17, ty + 11, 34, kind, scale=S)
        txt(draw, cx, ty + 60, label, 8.2, theme.text_pri, bold=True, anchor="mm", scale=S)

    # Bottom Dock (x=10, y=198, w=300, h=34) with colored micro icon squares
    draw_rr(
        canvas, 10, 198, 300, 34, 10,
        fill_top=theme.dock_bg, fill_bot=theme.dock_bg,
        outline=theme.dock_border, top_hl=theme.card_hi, top_hl_alpha=45,
        scale=S,
    )
    dock_items = [
        ("应用中心", "apps", 18),
        ("文件管理", "files", 114),
        ("系统设置", "settings", 210),
    ]
    for label, kind, dx in dock_items:
        draw_rr(canvas, dx, 203, 92, 24, 7, fill_top=theme.alt_top, fill_bot=theme.alt_bot, outline=theme.alt_border, scale=S)
        draw = draw_app_icon(canvas, dx + 8, 206.5, 17, kind, scale=S)
        txt(draw, dx + 31, 215, label, 7.6, theme.text_pri, bold=True, anchor="lm", scale=S)

    return canvas


def render_app_center_screen(theme: Theme) -> Image.Image:
    canvas = make_bg(theme, W, H)
    draw = ImageDraw.Draw(canvas)
    draw_subpage_topbar(canvas, draw, theme, "应用中心", "传包服务在线")

    draw_rr(canvas, 10, 30, 300, 22, 6, fill_top=theme.card_top, fill_bot=theme.card_bot, outline=theme.card_border, scale=S)
    txt(draw, 18, 41, "局域网上传地址：192.168.1.108", 7.2, theme.text_sec, anchor="lm", scale=S)
    txt(draw, 302, 41, "自带图标优先 · 缺省生成彩色图标", 6.8, theme.accent, bold=True, anchor="rm", scale=S)

    apps = [
        ("极速赛车", "自定义图片图标 · 148千字节", "racer", True, True, 58),
        ("独立音乐", "原版播放界面 · 系统自带示例", "music", False, True, 106),
        ("内存检测", "默认生成彩色图标 · 62千字节", "probe", False, False, 154),
    ]
    for name, desc, kind, is_custom, has_uninst, cy in apps:
        draw_rr(
            canvas, 10, cy, 300, 42, 8,
            fill_top=theme.card_top, fill_bot=theme.card_bot,
            outline=theme.card_border, top_hl=theme.card_hi, top_hl_alpha=50,
            scale=S,
        )
        if is_custom:
            draw = draw_custom_racer_icon(canvas, 18, cy + 6, 30, scale=S)
        else:
            draw = draw_app_icon(canvas, 18, cy + 6, 30, kind, scale=S)

        txt(draw, 56, cy + 14, name, 8.8, theme.text_pri, bold=True, anchor="lm", scale=S)
        txt(draw, 56, cy + 29, desc, 7.0, theme.text_sec, anchor="lm", scale=S)

        draw_rr(canvas, 214, cy + 10, 44, 22, 6, fill_top=theme.accent, fill_bot=theme.accent_dark, outline=theme.accent_border, scale=S)
        txt(draw, 236, cy + 21, "启动", 7.4, 0xFFFFFF, bold=True, anchor="mm", scale=S)

        if has_uninst:
            draw_rr(canvas, 262, cy + 10, 40, 22, 6, fill_top=theme.alt_top, fill_bot=theme.alt_bot, outline=theme.alt_border, scale=S)
            txt(draw, 282, cy + 21, "卸载", 7.2, theme.text_sec, anchor="mm", scale=S)

    draw_rr(canvas, 10, 202, 300, 30, 7, fill_top=theme.dock_bg, fill_bot=theme.dock_bg, outline=theme.dock_border, scale=S)
    txt(draw, 18, 217, "共扫描 3 个应用 · 安装包目录就绪", 7.2, theme.text_sec, anchor="lm", scale=S)
    draw_rr(canvas, 232, 207, 72, 20, 6, fill_top=theme.alt_top, fill_bot=theme.alt_bot, outline=theme.alt_border, scale=S)
    txt(draw, 268, 217, "扫描安装包", 7.2, theme.text_pri, bold=True, anchor="mm", scale=S)
    return canvas


def render_pomodoro_screen(theme: Theme) -> Image.Image:
    canvas = make_bg(theme, W, H)
    draw = ImageDraw.Draw(canvas)
    draw_subpage_topbar(canvas, draw, theme, "番茄工作台", "后台计时就绪")

    draw_rr(canvas, 10, 30, 146, 200, 10, fill_top=theme.card_top, fill_bot=theme.card_bot, outline=theme.card_border, top_hl=theme.card_hi, top_hl_alpha=55, scale=S)
    cx, cy, rad = 83, 118, 50
    box = [(cx - rad) * S, (cy - rad) * S, (cx + rad) * S, (cy + rad) * S]

    for deg in range(135, 406, 15):
        a = math.radians(deg)
        r_in = (rad + 6) * S
        r_out = (rad + (9 if deg % 45 == 0 else 7.2)) * S
        draw.line(
            [cx * S + r_in * math.cos(a), cy * S + r_in * math.sin(a), cx * S + r_out * math.cos(a), cy * S + r_out * math.sin(a)],
            fill=rgba(theme.text_mut if deg % 45 else theme.text_sec, 190),
            width=max(1, S // 2),
        )

    draw.arc(box, 135, 405, fill=rgba(theme.alt_border, 255), width=7 * S)
    draw.arc(box, 135, 330, fill=rgba(theme.accent, 255), width=7 * S)
    end_a = math.radians(330)
    ex, ey = cx + rad * math.cos(end_a), cy + rad * math.sin(end_a)
    draw.ellipse([(ex - 4) * S, (ey - 4) * S, (ex + 4) * S, (ey + 4) * S], fill=rgba(theme.text_pri, 255), outline=rgba(theme.accent, 255), width=S)

    draw_rr(canvas, cx - 26, cy - 31, 52, 15, 7, fill_top=theme.accent_tint, fill_bot=theme.accent_tint, outline=theme.accent_border, scale=S)
    txt(draw, cx, cy - 23.5, "专注时段", 6.8, theme.accent, bold=True, anchor="mm", scale=S)
    txt(draw, cx, cy + 2, "25:00", 21, theme.text_pri, bold=True, anchor="mm", scale=S)
    txt(draw, cx, cy + 25, "第 1 / 4 轮", 7.0, theme.text_sec, anchor="mm", scale=S)
    txt(draw, cx, 210, "单轮 25 分钟 · 后台独立计时", 6.8, theme.text_mut, anchor="mm", scale=S)

    # Right: Split-Flap Clock Card + Tactile Buttons (both HH and MM use clear primary text)
    draw_rr(canvas, 164, 30, 146, 98, 10, fill_top=theme.card_top, fill_bot=theme.card_bot, outline=theme.card_border, top_hl=theme.card_hi, top_hl_alpha=55, scale=S)
    txt(draw, 237, 40, "当前时间", 7.2, theme.text_mut, anchor="mm", scale=S)

    for fx, fval in ((176, "10"), (242, "42")):
        draw_rr(canvas, fx, 50, 56, 54, 7, fill_top=theme.alt_top, fill_bot=theme.alt_bot, outline=theme.alt_border, top_hl=theme.card_hi, top_hl_alpha=55, lip_h=1.8, lip_col=theme.lip_shadow, scale=S)
        txt(draw, fx + 28, 77, fval, 22, theme.text_pri, bold=True, anchor="mm", scale=S)
        draw.line([fx * S, 77 * S, (fx + 56) * S, 77 * S], fill=rgba(theme.bg_top, 210), width=max(2, int(1.2 * S)))
        draw.rectangle([fx * S, 74 * S, (fx + 3) * S, 80 * S], fill=rgba(theme.alt_border, 255))
        draw.rectangle([(fx + 53) * S, 74 * S, (fx + 56) * S, 80 * S], fill=rgba(theme.alt_border, 255))

    txt(draw, 237, 117, "2026年11月18日 周三", 7.0, theme.text_sec, anchor="mm", scale=S)

    draw_rr(
        canvas, 164, 138, 146, 40, 9,
        fill_top=theme.accent, fill_bot=theme.accent_dark,
        outline=theme.accent_border, lip_h=2.5, lip_col=theme.lip_shadow,
        scale=S,
    )
    draw.polygon([(202 * S, 153 * S), (202 * S, 163 * S), (210 * S, 158 * S)], fill=rgba(0xFFFFFF, 255))
    txt(draw, 244, 158, "开始专注", 10.2, 0xFFFFFF, bold=True, anchor="mm", scale=S)

    draw_rr(
        canvas, 164, 188, 146, 40, 9,
        fill_top=theme.alt_top, fill_bot=theme.alt_bot,
        outline=theme.alt_border, top_hl=theme.card_hi, top_hl_alpha=50,
        lip_h=2.5, lip_col=theme.lip_shadow,
        scale=S,
    )
    txt(draw, 237, 208, "重置时间", 9.6, theme.text_pri, bold=True, anchor="mm", scale=S)
    return canvas


def render_calculator_screen(theme: Theme) -> Image.Image:
    canvas = make_bg(theme, W, H)
    draw = ImageDraw.Draw(canvas)
    draw_subpage_topbar(canvas, draw, theme, "计算器", "键帽实体风格")

    draw_rr(canvas, 8, 28, 304, 204, 10, fill_top=theme.card_top, fill_bot=theme.card_bot, outline=theme.card_border, top_hl=theme.card_hi, top_hl_alpha=55, scale=S)
    draw_rr(canvas, 16, 35, 288, 46, 6, fill_top=theme.lcd_top, fill_bot=theme.lcd_bot, outline=theme.lcd_border, scale=S)
    txt(draw, 24, 45, "实用运算", 6.8, theme.lcd_sub, anchor="lm", scale=S)
    txt(draw, 296, 45, "128 × 4 =", 8.8, theme.lcd_sub, bold=True, anchor="rm", scale=S)
    txt(draw, 296, 66, "512", 21.0, theme.lcd_fg, bold=True, anchor="rm", scale=S)

    keys = [
        "7", "8", "9", "÷",
        "4", "5", "6", "×",
        "1", "2", "3", "-",
        "C", "0", "=", "+",
    ]
    for idx, k in enumerate(keys):
        r, c = divmod(idx, 4)
        kx = 16 + c * 73
        ky = 89 + r * 35
        kw, kh = 68, 29

        if k == "=":
            ftop, fbot, ol, tcol = theme.accent, theme.accent_dark, theme.accent_border, 0xFFFFFF
        elif k in ("÷", "×", "-", "+"):
            ftop, fbot, ol, tcol = theme.alt_top, theme.alt_bot, theme.alt_border, theme.accent
        elif k == "C":
            ftop, fbot, ol, tcol = theme.accent_tint, theme.alt_bot, theme.accent_border, theme.accent
        else:
            ftop, fbot, ol, tcol = theme.card_top, theme.alt_bot, theme.alt_border, theme.text_pri

        draw_rr(canvas, kx, ky, kw, kh, 6, fill_top=ftop, fill_bot=fbot, outline=ol, lip_h=2.0, lip_col=theme.lip_shadow, scale=S)
        txt(draw, kx + kw / 2.0, ky + kh / 2.0, k, 11.0, tcol, bold=True, anchor="mm", scale=S)

    return canvas


def render_settings_screen(theme: Theme) -> Image.Image:
    canvas = make_bg(theme, W, H)
    draw = ImageDraw.Draw(canvas)
    draw_subpage_topbar(canvas, draw, theme, "系统设置", "持久化保存")

    txt(draw, 12, 34, "界面主题风格（切换即时生效）", 7.6, theme.text_sec, bold=True, anchor="lm", scale=S)

    themes_meta = [
        ("经典深色", "默认主题", 0x12151C, 0x2E7CF6, True, 10),
        ("初雪", "日间白底", 0xF2F4F8, 0x2563EB, False, 112),
        ("深邃", "OLED 纯黑", 0x000000, 0x3B82F6, False, 214),
    ]
    for name, tag, bg_c, acc_c, active, tx in themes_meta:
        ol = theme.accent if active else theme.card_border
        draw_rr(canvas, tx, 45, 96, 62, 8, fill_top=theme.card_top, fill_bot=theme.card_bot, outline=ol, scale=S)
        draw_rr(canvas, tx + 8, 53, 24, 16, 4, fill_top=bg_c, fill_bot=bg_c, outline=theme.alt_border, scale=S)
        draw.ellipse([(tx + 38) * S, 56 * S, (tx + 48) * S, 66 * S], fill=rgba(acc_c, 255))
        if active:
            txt(draw, tx + 88, 61, "已启用", 6.6, theme.accent, bold=True, anchor="rm", scale=S)
        txt(draw, tx + 8, 82, name, 8.2, theme.text_pri, bold=True, anchor="lm", scale=S)
        txt(draw, tx + 8, 96, tag, 6.8, theme.text_sec, anchor="lm", scale=S)

    txt(draw, 12, 120, "设备运行参数与连接", 7.6, theme.text_sec, bold=True, anchor="lm", scale=S)
    cards = [
        ("屏幕背光调节", "当前亮度：85%", "滑块就绪", 10, 130),
        ("无线网络", "已连接家庭路由", "192.168.1.108", 164, 130),
        ("内存分布", "扩展内存：8.0 兆", "高速缓存正常", 10, 182),
        ("姿态与存储", "六轴传感就绪", "存储卡已挂载", 164, 182),
    ]
    for title, main_v, sub_v, cx, cy in cards:
        draw_rr(canvas, cx, cy, 146, 46, 8, fill_top=theme.card_top, fill_bot=theme.card_bot, outline=theme.card_border, scale=S)
        txt(draw, cx + 10, cy + 12, title, 7.2, theme.text_sec, anchor="lm", scale=S)
        txt(draw, cx + 10, cy + 27, main_v, 8.4, theme.text_pri, bold=True, anchor="lm", scale=S)
        txt(draw, cx + 136, cy + 36, sub_v, 6.4, theme.accent, anchor="rm", scale=S)

    return canvas


def render_files_screen(theme: Theme) -> Image.Image:
    canvas = make_bg(theme, W, H)
    draw = ImageDraw.Draw(canvas)
    draw_subpage_topbar(canvas, draw, theme, "文件与任务", "存储卡就绪")

    draw_rr(canvas, 10, 30, 146, 22, 6, fill_top=theme.accent, fill_bot=theme.accent_dark, outline=theme.accent_border, scale=S)
    txt(draw, 83, 41, "文件管理", 7.8, 0xFFFFFF, bold=True, anchor="mm", scale=S)
    draw_rr(canvas, 164, 30, 146, 22, 6, fill_top=theme.alt_top, fill_bot=theme.alt_bot, outline=theme.alt_border, scale=S)
    txt(draw, 237, 41, "后台任务", 7.8, theme.text_sec, bold=True, anchor="mm", scale=S)

    draw_rr(canvas, 10, 58, 248, 22, 6, fill_top=theme.card_top, fill_bot=theme.card_bot, outline=theme.card_border, scale=S)
    txt(draw, 18, 69, "/存储卡/桌面资源/", 7.2, theme.text_sec, anchor="lm", scale=S)
    draw_rr(canvas, 264, 58, 46, 22, 6, fill_top=theme.alt_top, fill_bot=theme.alt_bot, outline=theme.alt_border, scale=S)
    txt(draw, 287, 69, "上级", 7.2, theme.text_pri, bold=True, anchor="mm", scale=S)

    rows = [
        ("目录", "我的音乐专辑", "包含 12 首音频", "", 88),
        ("音频", "晴天.mp3", "4.2 兆字节 · 呼出音乐", "播放", 136),
        ("安装包", "racer_v2.gla", "148 千字节 · 含自定义图标", "安装", 184),
    ]
    for tag, name, meta, action, ry in rows:
        draw_rr(canvas, 10, ry, 300, 42, 8, fill_top=theme.card_top, fill_bot=theme.card_bot, outline=theme.card_border, scale=S)
        draw_rr(canvas, 18, ry + 11, 42, 20, 5, fill_top=theme.alt_top, fill_bot=theme.alt_bot, outline=theme.alt_border, scale=S)
        txt(draw, 39, ry + 21, tag, 7.0, theme.accent if tag == "安装包" else theme.text_pri, bold=True, anchor="mm", scale=S)
        txt(draw, 68, ry + 15, name, 8.2, theme.text_pri, bold=True, anchor="lm", scale=S)
        txt(draw, 68, ry + 30, meta, 6.8, theme.text_sec, anchor="lm", scale=S)
        if action:
            draw_rr(canvas, 256, ry + 10, 46, 22, 6, fill_top=theme.accent_tint, fill_bot=theme.accent_tint, outline=theme.accent_border, scale=S)
            txt(draw, 279, ry + 21, action, 7.2, theme.accent, bold=True, anchor="mm", scale=S)

    return canvas


def build_sheet_v2_2_boundary() -> Image.Image:
    t = THEME_CLASSIC_DARK
    canvas = make_bg(t, W, H)
    draw = ImageDraw.Draw(canvas)

    draw_rr(canvas, 24, 18, 912, 56, 12, fill_top=t.card_top, fill_bot=t.card_bot, outline=t.card_border, scale=1)
    draw_rr(canvas, 40, 32, 8, 28, 4, fill_top=t.accent, fill_bot=t.accent_dark, scale=1)
    txt(draw, 60, 40, "系统界面与独立音乐应用边界说明", 6.8, t.text_pri, bold=True, anchor="lm", scale=1)
    txt(draw, 60, 60, "改系统壳绝不碰独立音乐界面，采用正常现代配色与标准彩色应用图标", 4.0, t.text_sec, anchor="lm", scale=1)
    draw_rr(canvas, 738, 31, 180, 30, 8, fill_top=t.accent_tint, fill_bot=t.accent_tint, outline=t.accent_border, scale=1)
    txt(draw, 828, 46, "音乐原版页面零改动", 4.2, t.accent, bold=True, anchor="mm", scale=1)

    draw_rr(canvas, 24, 90, 444, 608, 12, fill_top=t.card_top, fill_bot=t.card_bot, outline=t.card_border, scale=1)
    txt(draw, 44, 114, "1. 系统侧（清爽现代配色 + 标准彩色图标）", 5.0, t.text_pri, bold=True, anchor="lm", scale=1)
    home_shot = render_home_screen(t).resize((404, 303), Image.Resampling.LANCZOS)
    draw_rr(canvas, 42, 134, 408, 307, 10, fill_top=t.bg_top, fill_bot=t.bg_top, outline=t.alt_border, scale=1)
    canvas.paste(home_shot, (44, 136))

    bullets_left = [
        "经典深色 / 初雪 / 深邃，三套高对比现代视觉主题",
        "音乐/番茄/计算/应用/文件均配标准彩色圆角图标底座",
        "文案全面中文化：无刻意拼接英文缩写与多余角标",
        "优先加载应用目录自带图片，缺省时生成标准彩色图标",
    ]
    for i, line in enumerate(bullets_left):
        by = 472 + i * 48
        draw_rr(canvas, 44, by, 404, 38, 7, fill_top=t.alt_top, fill_bot=t.alt_bot, outline=t.alt_border, scale=1)
        draw.ellipse([58, by + 16, 65, by + 23], fill=rgba(t.accent, 255))
        txt(draw, 74, by + 19, line, 4.1, t.text_pri, anchor="lm", scale=1)

    draw_rr(canvas, 492, 90, 444, 608, 12, fill_top=t.card_top, fill_bot=t.card_bot, outline=t.card_border, scale=1)
    txt(draw, 512, 114, "2. 独立音乐应用（保持原版原样）", 5.2, t.text_pri, bold=True, anchor="lm", scale=1)

    for idx, fname, cap in (
        (0, "ui_preview_2_music_elf.png", "三页签与列表界面（原版）"),
        (1, "ui_preview_3_music_tabs.png", "播放器主界面（原版）"),
    ):
        sx = 512 + idx * 208
        draw_rr(canvas, sx, 134, 198, 174, 8, fill_top=t.bg_top, fill_bot=t.bg_top, outline=t.alt_border, scale=1)
        p = os.path.join(OUT_DIR, fname)
        if os.path.isfile(p):
            im = Image.open(p).convert("RGBA").resize((190, 142), Image.Resampling.LANCZOS)
            canvas.paste(im, (sx + 4, 138))
        else:
            txt(draw, sx + 99, 208, fname, 4.0, t.text_mut, anchor="mm", scale=1)
        txt(draw, sx + 99, 294, cap, 3.8, t.text_sec, anchor="mm", scale=1)

    bullets_right = [
        "严格零修改：ui_manager.c / .h、ui_theme.h、hub_player.c",
        "应用包零改动：app_sdk_template/apps/com.glam.music/*",
        "切入音乐视图时：系统主动隐藏顶部栏与返回主页胶囊",
        "桌面播放卡片仅读取状态回调文字，不干涉音乐应用界面",
    ]
    for i, line in enumerate(bullets_right):
        by = 336 + i * 54
        draw_rr(canvas, 512, by, 404, 42, 7, fill_top=t.alt_top, fill_bot=t.alt_bot, outline=t.alt_border, scale=1)
        draw.ellipse([526, by + 18, 533, by + 25], fill=rgba(t.status_ok, 255))
        txt(draw, 542, by + 21, line, 4.1, t.text_pri, anchor="lm", scale=1)

    draw_rr(canvas, 512, 574, 404, 96, 8, fill_top=t.accent_tint, fill_bot=t.accent_tint, outline=t.accent_border, scale=1)
    txt(draw, 528, 602, "版本构建核验保护线", 4.6, t.accent, bold=True, anchor="lm", scale=1)
    txt(draw, 528, 632, "所有音乐界面源码哈希与重构前基线完全一致", 4.1, t.text_pri, anchor="lm", scale=1)
    txt(draw, 528, 654, "完整升级前压缩包已备份留存于工作区根目录", 3.9, t.text_sec, anchor="lm", scale=1)

    return canvas


def build_sheet_v2_4_quad() -> Image.Image:
    t = THEME_CLASSIC_DARK
    canvas = make_bg(t, W, H)
    draw = ImageDraw.Draw(canvas)

    draw_rr(canvas, 20, 14, 920, 42, 9, fill_top=t.card_top, fill_bot=t.card_bot, outline=t.card_border, scale=1)
    txt(draw, 36, 35, "系统内建核心功能页全览（经典深色默认主题 · 清爽现代布局）", 5.2, t.text_pri, bold=True, anchor="lm", scale=1)
    txt(draw, 924, 35, "320 × 240 等比呈现", 3.9, t.accent, bold=True, anchor="rm", scale=1)

    quads = [
        ("01 · 番茄工作台", render_pomodoro_screen(t), 20, 68),
        ("02 · 实体键帽计算器", render_calculator_screen(t), 490, 68),
        ("03 · 系统设置与三主题", render_settings_screen(t), 20, 392),
        ("04 · 文件管理与后台任务", render_files_screen(t), 490, 392),
    ]
    for title, screen_img, qx, qy in quads:
        draw_rr(canvas, qx, qy, 450, 312, 10, fill_top=t.card_top, fill_bot=t.card_bot, outline=t.card_border, scale=1)
        txt(draw, qx + 14, qy + 18, title, 4.4, t.text_pri, bold=True, anchor="lm", scale=1)
        shot = screen_img.resize((360, 270), Image.Resampling.LANCZOS)
        draw_rr(canvas, qx + 43, qy + 32, 364, 274, 8, fill_top=t.bg_top, fill_bot=t.bg_top, outline=t.alt_border, scale=1)
        canvas.paste(shot, (qx + 45, qy + 34))

    return canvas


def build_sheet_v2_5_themes() -> Image.Image:
    base = THEME_CLASSIC_DARK
    canvas = make_bg(base, W, H)
    draw = ImageDraw.Draw(canvas)

    draw_rr(canvas, 20, 16, 920, 50, 10, fill_top=base.card_top, fill_bot=base.card_bot, outline=base.card_border, scale=1)
    txt(draw, 38, 41, "G'lam OS 三套实时主题对比（标准彩色图标 · 经典深色 / 初雪 / 深邃）", 5.2, base.text_pri, bold=True, anchor="lm", scale=1)

    themes = [THEME_CLASSIC_DARK, THEME_CLEAN_LIGHT, THEME_SOFT_TEAL]
    col_w = 294
    for i, th in enumerate(themes):
        cx = 20 + i * (col_w + 19)
        draw_rr(canvas, cx, 80, col_w, 618, 11, fill_top=base.card_top, fill_bot=base.card_bot, outline=th.accent if i == 0 else base.card_border, scale=1)

        txt(draw, cx + 16, 106, th.name, 5.8, base.text_pri, bold=True, anchor="lm", scale=1)
        if i == 0:
            draw_rr(canvas, cx + 216, 95, 64, 22, 6, fill_top=th.accent_tint, fill_bot=th.accent_tint, outline=th.accent_border, scale=1)
            txt(draw, cx + 248, 106, "默认启用", 3.6, th.accent, bold=True, anchor="mm", scale=1)
        txt(draw, cx + 16, 130, th.sub, 3.6, base.text_sec, anchor="lm", scale=1)

        shot = render_home_screen(th).resize((266, 200), Image.Resampling.LANCZOS)
        draw_rr(canvas, cx + 12, 148, 270, 204, 8, fill_top=th.bg_top, fill_bot=th.bg_top, outline=base.alt_border, scale=1)
        canvas.paste(shot, (cx + 14, 150))

        swatches = [
            ("主背景", th.bg_top, f"#{th.bg_top:06x}"),
            ("卡片面", th.card_top, f"#{th.card_top:06x}"),
            ("凹陷框", th.lcd_top, f"#{th.lcd_top:06x}"),
            ("主文字", th.text_pri, f"#{th.text_pri:06x}"),
            ("点缀色", th.accent, f"#{th.accent:06x}"),
        ]
        for s_idx, (s_role, s_col, s_hex) in enumerate(swatches):
            sy = 372 + s_idx * 56
            draw_rr(canvas, cx + 14, sy, col_w - 28, 44, 7, fill_top=base.alt_top, fill_bot=base.alt_bot, outline=base.alt_border, scale=1)
            draw_rr(canvas, cx + 24, sy + 10, 32, 24, 5, fill_top=s_col, fill_bot=s_col, outline=base.card_border, scale=1)
            txt(draw, cx + 68, sy + 22, s_role, 4.2, base.text_pri, bold=True, anchor="lm", scale=1)
            txt(draw, cx + col_w - 26, sy + 22, s_hex, 4.0, base.text_sec, anchor="rm", scale=1)

    return canvas


def main():
    os.makedirs(OUT_DIR, exist_ok=True)
    outputs = {
        "ui_v2_1_bento_home.png": render_home_screen(THEME_CLASSIC_DARK),
        "ui_v2_2_music_studio.png": build_sheet_v2_2_boundary(),
        "ui_v2_3_music_discovery.png": render_app_center_screen(THEME_CLASSIC_DARK),
        "ui_v2_4_apps_quad_showcase.png": build_sheet_v2_4_quad(),
        "ui_v2_5_theme_palettes.png": build_sheet_v2_5_themes(),
    }
    for fname, im in outputs.items():
        out_path = os.path.join(OUT_DIR, fname)
        im.convert("RGB").save(out_path, "PNG", optimize=True)
        print(f"Wrote {out_path} ({im.size[0]}x{im.size[1]})")


if __name__ == "__main__":
    main()
