#!/usr/bin/env python3
"""
Generador de iconos y sprites anti-aliased (supersampling 4x con Pillow)
para que la interfaz de PC de Wii U Transfer luzca nítida y moderna.
"""

from pathlib import Path
from PIL import Image, ImageDraw, ImageTk

COL_HEADER  = (11, 57, 84, 255)    # #0B3954
COL_CYAN    = (0, 174, 239, 255)   # #00AEEF
COL_CYAN_LT = (168, 216, 236, 255) # #A8D8EC
COL_WHITE   = (255, 255, 255, 255)


def make_rounded_rect_img(
    width: int,
    height: int,
    radius: int,
    fill: tuple[int, int, int, int] | str,
    border: tuple[int, int, int, int] | str | None = None,
    border_w: int = 1,
    bg: tuple[int, int, int, int] | str | None = None,
) -> Image.Image:
    s = 4
    w, h = max(4, width), max(4, height)
    back = bg if bg is not None else (0, 0, 0, 0)
    img = Image.new("RGBA", (w * s, h * s), back)
    draw = ImageDraw.Draw(img)
    pad = max(0, (border_w * s) // 2)
    draw.rounded_rectangle(
        [pad, pad, w * s - 1 - pad, h * s - 1 - pad],
        radius=max(0, radius * s),
        fill=fill,
        outline=border,
        width=border_w * s if border else 0,
    )
    return img.resize((w, h), Image.Resampling.LANCZOS)


def make_rounded_corners_9slice(
    radius: int,
    fill: tuple[int, int, int, int] | str,
    border: tuple[int, int, int, int] | str | None = None,
    border_w: int = 1,
    bg: tuple[int, int, int, int] | str | None = None,
) -> tuple[Image.Image, Image.Image, Image.Image, Image.Image]:
    """Genera una sola vez las 4 esquinas anti-aliased (tl, tr, bl, br) de tamaño radius x radius."""
    r = max(2, radius)
    sz = r * 2 + 8
    base = make_rounded_rect_img(
        sz, sz, r, fill=fill, border=border, border_w=border_w, bg=bg
    )
    tl = base.crop((0, 0, r, r))
    tr = base.crop((sz - r, 0, sz, r))
    bl = base.crop((0, sz - r, r, sz))
    br = base.crop((sz - r, sz - r, sz, sz))
    return tl, tr, bl, br


def make_gamepad_icon(
    width: int,
    height: int,
    tile_bg: tuple[int, int, int, int] | None = None,
) -> Image.Image:
    """Dibuja el GamePad blanco ancho redondeado + 3 ondas Wi-Fi supersampleado 4x."""
    s = 4
    W, H = width * s, height * s
    img = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    draw = ImageDraw.Draw(img)

    if tile_bg is not None:
        draw.rounded_rectangle([0, 0, W - 1, H - 1], radius=int(min(W, H) * 0.22), fill=tile_bg)

    cx = W // 2
    # Espacio para ondas arriba + cuerpo del GamePad abajo
    pad_x = int(W * 0.08)
    gp_w = W - 2 * pad_x
    gp_h = int(H * 0.56)
    gp_x0 = cx - gp_w // 2
    gp_y0 = H - gp_h - int(H * 0.06)
    gp_x1 = gp_x0 + gp_w
    gp_y1 = gp_y0 + gp_h

    # 3 ondas Wi-Fi curvas centradas encima del GamePad
    arc_cy = gp_y0 + int(H * 0.04)
    stroke = max(4, int(H * 0.036))
    for i, frac in enumerate((0.14, 0.23, 0.32)):
        rx = int(W * frac)
        ry = int(H * (frac * 1.05))
        col = COL_WHITE if i < 2 else COL_CYAN_LT
        draw.arc(
            [cx - rx, arc_cy - ry - int(H * 0.06), cx + rx, arc_cy + ry - int(H * 0.06)],
            start=222,
            end=318,
            fill=col,
            width=stroke,
        )

    # Cuerpo principal blanco del Wii U GamePad (forma ergonómica redondeada)
    rad = int(gp_h * 0.36)
    draw.rounded_rectangle([gp_x0, gp_y0, gp_x1, gp_y1], radius=rad, fill=COL_WHITE)

    # Pantalla interior oscura redondeada (combina con el header oscuro)
    scr_w = int(gp_w * 0.49)
    scr_h = int(gp_h * 0.68)
    scr_x0 = cx - scr_w // 2
    scr_y0 = gp_y0 + (gp_h - scr_h) // 2
    draw.rounded_rectangle(
        [scr_x0, scr_y0, scr_x0 + scr_w, scr_y0 + scr_h],
        radius=max(4, int(scr_h * 0.12)),
        fill=COL_HEADER,
    )

    # Stick izquierdo + Cruceta (+)
    lx = gp_x0 + int(gp_w * 0.135)
    rx = gp_x1 - int(gp_w * 0.135)
    top_y = gp_y0 + int(gp_h * 0.29)
    bot_y = gp_y0 + int(gp_h * 0.66)
    st_r = max(3, int(gp_h * 0.105))

    draw.ellipse([lx - st_r, top_y - st_r, lx + st_r, top_y + st_r], fill=COL_HEADER)
    draw.ellipse([rx - st_r, top_y - st_r, rx + st_r, top_y + st_r], fill=COL_HEADER)

    # Cruceta izquierda
    dp_arm = max(3, int(gp_h * 0.13))
    dp_th = max(2, int(gp_h * 0.048))
    draw.rounded_rectangle(
        [lx - dp_arm, bot_y - dp_th, lx + dp_arm, bot_y + dp_th],
        radius=max(1, dp_th // 2),
        fill=COL_HEADER,
    )
    draw.rounded_rectangle(
        [lx - dp_th, bot_y - dp_arm, lx + dp_th, bot_y + dp_arm],
        radius=max(1, dp_th // 2),
        fill=COL_HEADER,
    )

    # 4 botones ABXY en rombo a la derecha
    br = max(2, int(gp_h * 0.05))
    b_off = max(4, int(gp_h * 0.12))
    for dx, dy in ((0, -b_off), (0, b_off), (-b_off, 0), (b_off, 0)):
        bx, by = rx + dx, bot_y + dy
        draw.ellipse([bx - br, by - br, bx + br, by + br], fill=COL_HEADER)

    return img.resize((width, height), Image.Resampling.LANCZOS)


def ensure_pc_assets(assets_dir: Path) -> tuple[Path, Path]:
    assets_dir.mkdir(parents=True, exist_ok=True)
    app_icon_path = assets_dir / "app_icon.png"
    header_icon_path = assets_dir / "header_icon.png"

    make_gamepad_icon(64, 64, tile_bg=COL_HEADER).save(app_icon_path)
    make_gamepad_icon(74, 58, tile_bg=None).save(header_icon_path)
    return app_icon_path, header_icon_path


def to_tk_image(pil_img: Image.Image) -> ImageTk.PhotoImage:
    return ImageTk.PhotoImage(pil_img)
