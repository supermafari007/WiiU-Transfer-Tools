#!/usr/bin/env python3
"""
Genera:
  1. assets/icon.png (128x128), tv.png (1280x720), drc.png (854x480)
  2. source/font_atlas.hpp con fuentes proporcionales anti-aliased Segoe UI (nativas 720p)
     y el icono de Wii U GamePad + 3 ondas Wi-Fi compuesto en RGBX32 (kGamepadIconRgbx).
"""

import struct
import wave
from pathlib import Path
from PIL import Image, ImageDraw, ImageFont

FONT_REG_PATH = Path(r"C:\Windows\Fonts\segoeui.ttf")
FONT_BLD_PATH = Path(r"C:\Windows\Fonts\segoeuib.ttf")

COL_BG      = (15, 20, 27, 255)    # #0F141B
COL_HEADER  = (11, 57, 84, 255)    # #0B3954
COL_CYAN    = (0, 174, 239, 255)   # #00AEEF
COL_CYAN_LT = (168, 216, 236, 255) # #A8D8EC
COL_WHITE   = (255, 255, 255, 255)


def draw_gamepad_rgba(width: int, height: int) -> Image.Image:
    s = 4
    W, H = width * s, height * s
    img = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    draw = ImageDraw.Draw(img)

    cx = W // 2
    pad_x = int(W * 0.06)
    gp_w = W - 2 * pad_x
    gp_h = int(H * 0.56)
    gp_x0 = cx - gp_w // 2
    gp_y0 = H - gp_h - int(H * 0.05)
    gp_x1 = gp_x0 + gp_w
    gp_y1 = gp_y0 + gp_h

    # 3 ondas Wi-Fi curvas encima del GamePad
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

    # Cuerpo principal blanco redondeado
    rad = int(gp_h * 0.36)
    draw.rounded_rectangle([gp_x0, gp_y0, gp_x1, gp_y1], radius=rad, fill=COL_WHITE)

    # Pantalla interior oscura redondeada
    scr_w = int(gp_w * 0.49)
    scr_h = int(gp_h * 0.68)
    scr_x0 = cx - scr_w // 2
    scr_y0 = gp_y0 + (gp_h - scr_h) // 2
    draw.rounded_rectangle(
        [scr_x0, scr_y0, scr_x0 + scr_w, scr_y0 + scr_h],
        radius=max(4, int(scr_h * 0.12)),
        fill=COL_HEADER,
    )

    lx = gp_x0 + int(gp_w * 0.135)
    rx = gp_x1 - int(gp_w * 0.135)
    top_y = gp_y0 + int(gp_h * 0.29)
    bot_y = gp_y0 + int(gp_h * 0.66)
    st_r = max(3, int(gp_h * 0.105))

    draw.ellipse([lx - st_r, top_y - st_r, lx + st_r, top_y + st_r], fill=COL_HEADER)
    draw.ellipse([rx - st_r, top_y - st_r, rx + st_r, top_y + st_r], fill=COL_HEADER)

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

    br = max(2, int(gp_h * 0.05))
    b_off = max(4, int(gp_h * 0.12))
    for dx, dy in ((0, -b_off), (0, b_off), (-b_off, 0), (b_off, 0)):
        bx, by = rx + dx, bot_y + dy
        draw.ellipse([bx - br, by - br, bx + br, by + br], fill=COL_HEADER)

    return img.resize((width, height), Image.Resampling.LANCZOS)


def make_splash_image(w: int, h: int, is_icon: bool) -> Image.Image:
    if is_icon:
        img = Image.new("RGBA", (w, h), COL_HEADER)
        draw = ImageDraw.Draw(img)
        draw.rectangle([0, h - 10, w, h], fill=COL_CYAN)
        icon = draw_gamepad_rgba(104, 82)
        img.alpha_composite(icon, ((w - 104) // 2, (h - 82) // 2 - 4))
        return img

    img = Image.new("RGBA", (w, h), COL_BG)
    draw = ImageDraw.Draw(img)
    bar_h = max(14, h // 11)
    draw.rectangle([0, h - bar_h, w, h], fill=COL_HEADER)
    draw.rectangle([0, h - bar_h, w, h - bar_h + 4], fill=COL_CYAN)

    icon_w = int(h * 0.32)
    icon_h = int(icon_w * 0.78)
    icon = draw_gamepad_rgba(icon_w, icon_h)
    img.alpha_composite(icon, ((w - icon_w) // 2, int(h * 0.22)))

    font_path = FONT_BLD_PATH if FONT_BLD_PATH.exists() else None
    if font_path:
        f_title = ImageFont.truetype(str(font_path), int(h * 0.08))
        f_sub = ImageFont.truetype(str(FONT_REG_PATH), int(h * 0.038))
    else:
        f_title = ImageFont.load_default()
        f_sub = ImageFont.load_default()

    title = "Wii U Transfer Tools"
    sub = "Desarrollado por El Diuca  -  Ver. 2.0.1  (Modo USB / Modo Red)"
    tb = draw.textbbox((0, 0), title, font=f_title)
    sb = draw.textbbox((0, 0), sub, font=f_sub)
    draw.text(((w - (tb[2] - tb[0])) // 2, int(h * 0.59)), title, fill=COL_WHITE, font=f_title)
    draw.text(((w - (sb[2] - sb[0])) // 2, int(h * 0.72)), sub, fill=COL_CYAN_LT, font=f_sub)
    return img


def load_ttf(bold: bool, px: int) -> ImageFont.FreeTypeFont:
    p = FONT_BLD_PATH if (bold and FONT_BLD_PATH.exists()) else FONT_REG_PATH
    return ImageFont.truetype(str(p), px)


def generate_font_atlas_hpp(out_hpp: Path) -> None:
    # Tamaños nativos 720p (1280x720)
    specs = [
        ("FontHeader", True, 36),
        ("FontTitle", True, 25),
        ("FontBody", False, 20),
        ("FontSmall", False, 17),
    ]

    alpha_blob = bytearray()
    font_tables: list[tuple[str, int, int, list[tuple[int, int, int, int, int, int]]]] = []

    for name, is_bold, px in specs:
        font = load_ttf(is_bold, px)
        ascent, descent = font.getmetrics()
        line_h = ascent + descent
        glyphs = []
        for code in range(0x20, 0x7F):
            ch = chr(code)
            adv = int(round(font.getlength(ch)))
            bbox = font.getbbox(ch)
            if bbox is None:
                glyphs.append((0, 0, 0, 0, max(1, adv), 0))
                continue
            x0, y0, x1, y1 = bbox
            gw = max(0, x1 - x0)
            gh = max(0, y1 - y0)
            if gw == 0 or gh == 0:
                glyphs.append((0, 0, 0, 0, max(1, adv), 0))
                continue

            canvas = Image.new("L", (gw + 4, gh + 4), 0)
            cdraw = ImageDraw.Draw(canvas)
            cdraw.text((2 - x0, 2 - y0), ch, fill=255, font=font)
            crop = canvas.crop((2, 2, 2 + gw, 2 + gh))
            offset = len(alpha_blob)
            alpha_blob.extend(crop.tobytes())
            glyphs.append((gw, gh, x0, y0, max(1, adv), offset))

        font_tables.append((name, line_h, ascent, glyphs))

    # Icono GamePad 100x76 compuesto sobre el header oscuro #0B3954 en RGBX32 (0xRRGGBB00)
    icon_w, icon_h = 100, 76
    icon_rgba = draw_gamepad_rgba(icon_w, icon_h)
    icon_comp = Image.new("RGBA", (icon_w, icon_h), COL_HEADER)
    icon_comp.alpha_composite(icon_rgba, (0, 0))
    icon_rgbx: list[int] = []
    for r, g, b, _a in icon_comp.getdata():
        icon_rgbx.append((r << 24) | (g << 16) | (b << 8))

    lines: list[str] = [
        "#pragma once",
        "#include <cstdint>",
        "",
        "// Archivo autogenerado por generate_assets.py (720p Segoe UI + GamePad RGBX32)",
        "namespace wutui::atlas {",
        "",
        "struct GlyphMetric {",
        "    uint8_t width;",
        "    uint8_t height;",
        "    int8_t  xOff;",
        "    int8_t  yOff;",
        "    uint8_t advance;",
        "    uint32_t dataOffset;",
        "};",
        "",
        "struct BitmapFont {",
        "    uint8_t lineHeight;",
        "    uint8_t ascent;",
        "    const GlyphMetric* glyphs; // 95 glifos: 0x20..0x7E",
        "};",
        "",
        f"constexpr int kGamepadIconW = {icon_w};",
        f"constexpr int kGamepadIconH = {icon_h};",
        f"static const uint32_t kGamepadIconRgbx[{len(icon_rgbx)}] = {{",
    ]

    for i in range(0, len(icon_rgbx), 12):
        chunk = ",".join(f"0x{v:08X}u" for v in icon_rgbx[i : i + 12])
        lines.append(f"    {chunk},")
    lines.append("};")
    lines.append("")

    lines.append(f"static const uint8_t kGlyphAlphaData[{len(alpha_blob)}] = {{")
    for i in range(0, len(alpha_blob), 24):
        chunk = ",".join(str(b) for b in alpha_blob[i : i + 24])
        lines.append(f"    {chunk},")
    lines.append("};")
    lines.append("")

    for name, line_h, ascent, glyphs in font_tables:
        lines.append(f"static const GlyphMetric kMetrics_{name}[95] = {{")
        for gw, gh, xo, yo, adv, off in glyphs:
            lines.append(f"    {{{gw}, {gh}, {xo}, {yo}, {adv}, {off}}},")
        lines.append("};")
        lines.append(f"static const BitmapFont k{name} = {{{line_h}, {ascent}, kMetrics_{name}}};")
        lines.append("")

    lines.append("} // namespace wutui::atlas")
    out_hpp.write_text("\n".join(lines) + "\n", encoding="utf-8")


# Tablas IMA ADPCM (4 bits por muestra, compresión 4:1 desde PCM16)
IMA_INDEX_TABLE = (-1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8)
IMA_STEP_TABLE = (
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17,
    19, 21, 23, 25, 28, 31, 34, 37, 41, 45,
    50, 55, 60, 66, 73, 80, 88, 97, 107, 118,
    130, 143, 157, 173, 190, 209, 230, 253, 279, 307,
    337, 371, 408, 449, 494, 544, 598, 658, 724, 796,
    876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066,
    2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358,
    5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899,
    15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767,
)


def encode_ima_channel(samples: list[int]) -> bytes:
    pred = 0
    idx = 0
    out = bytearray((len(samples) + 1) // 2)
    out_pos = 0
    low_nibble = True

    for s in samples:
        step = IMA_STEP_TABLE[idx]
        diff = s - pred
        nib = 8 if diff < 0 else 0
        if diff < 0:
            diff = -diff

        vpdiff = step >> 3
        if diff >= step:
            nib |= 4
            diff -= step
            vpdiff += step
        step >>= 1
        if diff >= step:
            nib |= 2
            diff -= step
            vpdiff += step
        step >>= 1
        if diff >= step:
            nib |= 1
            vpdiff += step

        if nib & 8:
            pred -= vpdiff
        else:
            pred += vpdiff

        if pred > 32767:
            pred = 32767
        elif pred < -32768:
            pred = -32768

        idx += IMA_INDEX_TABLE[nib]
        if idx < 0:
            idx = 0
        elif idx > 88:
            idx = 88

        if low_nibble:
            out[out_pos] = nib & 0x0F
            low_nibble = False
        else:
            out[out_pos] |= (nib & 0x0F) << 4
            out_pos += 1
            low_nibble = True

    return bytes(out)


def convert_wav_to_wia_and_raw(wav_path: Path, out_wia: Path, out_raw_be: Path | None = None) -> None:
    with wave.open(str(wav_path), "rb") as w:
        channels = w.getnchannels()
        sampwidth = w.getsampwidth()
        rate = w.getframerate()
        nframes = w.getnframes()
        raw = w.readframes(nframes)

    if sampwidth != 2 or channels not in (1, 2):
        raise ValueError(f"Formato WAV no soportado en {wav_path}: ch={channels}, width={sampwidth}")

    total_shorts = nframes * channels
    shorts = struct.unpack(f"<{total_shorts}h", raw)
    if channels == 2:
        left = list(shorts[0::2])
        right = list(shorts[1::2])
    else:
        left = list(shorts)
        right = list(shorts)

    if out_raw_be is not None:
        # Splash screen RGBA big-endian + audio LPCM16 big-endian opcional
        interleaved_be = bytearray(nframes * 4)
        struct.pack_into(f">{total_shorts}h", interleaved_be, 0, *shorts)
        out_raw_be.write_bytes(interleaved_be)

    # Decimar 48000 -> 24000 Hz con filtro [1, 2, 1]/4 para eliminar aliasing y reducir tamaño a la mitad
    if rate == 48000:
        n2 = nframes // 2
        l_ds = [0] * n2
        r_ds = [0] * n2
        for i in range(n2):
            i2 = i * 2
            l0 = left[i2]
            l1 = left[i2 + 1]
            l2 = left[i2 + 2] if (i2 + 2) < nframes else l1
            r0 = right[i2]
            r1 = right[i2 + 1]
            r2 = right[i2 + 2] if (i2 + 2) < nframes else r1
            l_ds[i] = (l0 + 2 * l1 + l2) >> 2
            r_ds[i] = (r0 + 2 * r1 + r2) >> 2
        left, right = l_ds, r_ds
        out_rate = 24000
    else:
        out_rate = rate

    # Asegurar número par de muestras por canal
    if len(left) & 1:
        left.append(left[-1])
        right.append(right[-1])

    num_samples = len(left)
    adpcm_l = encode_ima_channel(left)
    adpcm_r = encode_ima_channel(right)

    # Cabecera WIA1 (16 bytes big-endian): magic 'WIA1', sampleRate(u32), numSamples(u32), channels(u16), reserved(u16)
    hdr = struct.pack(">4sIIHH", b"WIA1", out_rate, num_samples, 2, 0)
    out_wia.write_bytes(hdr + adpcm_l + adpcm_r)


def generate_splash_screen_hpp(tv_img: Image.Image, out_hpp: Path) -> None:
    w, h = tv_img.size
    rgba = tv_img.convert("RGBA")
    rle_pairs: list[tuple[int, int]] = []
    prev_col = -1
    run_len = 0

    for r, g, b, _a in rgba.getdata():
        col = (r << 24) | (g << 16) | (b << 8)
        if col == prev_col and run_len < 0xFFFFFFFF:
            run_len += 1
        else:
            if run_len > 0:
                rle_pairs.append((run_len, prev_col))
            prev_col = col
            run_len = 1
    if run_len > 0:
        rle_pairs.append((run_len, prev_col))

    lines = [
        "#pragma once",
        "#include <cstdint>",
        "",
        "// Splash screen 1280x720 comprimido en RLE (RGBX32 big-endian)",
        "namespace wutui::splash {",
        f"constexpr int kSplashW = {w};",
        f"constexpr int kSplashH = {h};",
        f"constexpr uint32_t kSplashRleCount = {len(rle_pairs)};",
        f"static const uint32_t kSplashRleData[{len(rle_pairs) * 2}] = {{",
    ]
    flat = []
    for cnt, col in rle_pairs:
        flat.append(f"0x{cnt:08X}u")
        flat.append(f"0x{col:08X}u")
    for i in range(0, len(flat), 12):
        lines.append("    " + ",".join(flat[i : i + 12]) + ",")
    lines.append("};")
    lines.append("} // namespace wutui::splash")
    out_hpp.write_text("\n".join(lines) + "\n", encoding="utf-8")


def main() -> None:
    root = Path(__file__).resolve().parent
    assets_dir = root / "assets"
    content_dir = root / "content"
    assets_dir.mkdir(parents=True, exist_ok=True)
    content_dir.mkdir(parents=True, exist_ok=True)

    icon_img = make_splash_image(128, 128, True)
    tv_img = make_splash_image(1280, 720, False)
    drc_img = make_splash_image(854, 480, False)

    icon_img.save(assets_dir / "icon.png")
    tv_img.save(assets_dir / "tv.png")
    drc_img.save(assets_dir / "drc.png")

    atlas_path = root / "source" / "font_atlas.hpp"
    generate_font_atlas_hpp(atlas_path)

    splash_hpp_path = root / "source" / "splash_screen.hpp"
    generate_splash_screen_hpp(tv_img, splash_hpp_path)

    # Convertir sonidos desde Sonidos/ hacia content/ (para empaquetar en .wuhb)
    sounds_dir = root.parent.parent / "Sonidos"
    intro_wav = sounds_dir / "Pantalla Inicio.wav"
    bgm_wav = sounds_dir / "Tema de Fondo.wav"
    if intro_wav.exists():
        convert_wav_to_wia_and_raw(intro_wav, content_dir / "intro.wia")
        print("Audio intro generado:", content_dir / "intro.wia")
    if bgm_wav.exists():
        convert_wav_to_wia_and_raw(bgm_wav, content_dir / "bgm.wia")
        print("Audio BGM generado:", content_dir / "bgm.wia")

    print("Assets 720p, splash_screen.hpp, font_atlas.hpp y audios generados en:", root)


if __name__ == "__main__":
    main()
