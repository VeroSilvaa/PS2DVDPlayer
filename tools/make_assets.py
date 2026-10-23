#!/usr/bin/env python3
"""
Gera os recursos graficos da interface (rodar no conteiner):

    docker compose run --rm ps2dvdplayer sh tools/make_assets.sh

- assets/font_body.png/.h    Noto Sans 18 px (texto normal)
- assets/font_large.png/.h   Noto Sans 26 px (titulos de opcoes)
                             atlas de bytes 32..255 (Windows-1252:
                             acentos, travessao, aspas), 16 por linha,
                             tons de cinza; .h com celula, linha de base
                             e largura de cada caractere
- assets/logo.png            "PS2DVD" + "Player" em Noto Sans Bold, como
                             no interface-model.png (nao usa o logotipo
                             oficial "PS2")
- assets/buttons.png/.h      botoes recortados do
                             "Kit de Botões de Controle PlayStation.png"
- assets/preview_*.png       previas ampliadas (nao vao para o ELF)

Base: tools/make_assets.py do PS2FileManager (mesmo autor).
Fontes: Noto Sans (licenca OFL, third_party/fonts).
"""

import os
import sys

from PIL import Image, ImageDraw, ImageFont, ImageFilter

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ASSETS = os.path.join(ROOT, "assets")

FONT_SANS = os.path.join(ROOT, "third_party", "fonts", "NotoSans-Regular.ttf")
FONT_BOLD = os.path.join(ROOT, "third_party", "fonts", "NotoSans-Bold.ttf")

FIRST_CHAR, LAST_CHAR = 32, 255
PER_ROW = 16

# (nome, tamanho em px): texto normal e titulos (tela 640x480)
FONTS = [("body", 18), ("large", 26)]


def char_for_byte(code):
    try:
        return bytes([code]).decode("cp1252")
    except UnicodeDecodeError:
        return None      # 0x81, 0x8D, 0x8F, 0x90, 0x9D: sem caractere


def make_font(name, size):
    """
    assets/font_<name>.png + .h. A celula sai das medidas da fonte:
    altura = ascendente + descendente, largura = maior avanco + 2.
    """
    font = ImageFont.truetype(FONT_SANS, size)
    ascent, descent = font.getmetrics()
    count = LAST_CHAR - FIRST_CHAR + 1
    rows = (count + PER_ROW - 1) // PER_ROW

    chars = []
    for code in range(FIRST_CHAR, LAST_CHAR + 1):
        ch = char_for_byte(code)
        ok = ch is not None and ch.isprintable() and ch != "\x7f"
        chars.append(ch if ok else None)

    cell_w = max(round(font.getlength(c)) for c in chars if c) + 2
    cell_h = ascent + descent + 2
    baseline = ascent + 1

    atlas = Image.new("L", (PER_ROW * cell_w, rows * cell_h), 0)
    draw = ImageDraw.Draw(atlas)
    widths = []

    for i, ch in enumerate(chars):
        width = 0

        if ch is not None:
            x = (i % PER_ROW) * cell_w
            y = (i // PER_ROW) * cell_h

            # 1 px de margem a esquerda (o ui.c desenha em x - 1)
            draw.text((x + 1, y + baseline), ch, fill=255, font=font, anchor="ls")
            width = min(cell_w - 1, round(font.getlength(ch)))

        widths.append(width)

    upper = name.upper()
    atlas.save(os.path.join(ASSETS, "font_%s.png" % name), optimize=True)

    with open(os.path.join(ASSETS, "font_%s.h" % name), "w") as out:
        out.write("/* Gerado por tools/make_assets.py: Noto Sans %d px, bytes %d..%d (Windows-1252). */\n"
                  % (size, FIRST_CHAR, LAST_CHAR))
        out.write("#define FONT_%s_CELL_W %d\n#define FONT_%s_CELL_H %d\n\n"
                  % (upper, cell_w, upper, cell_h))
        out.write("static const unsigned char font_%s_width[%d] = {\n" % (name, count))
        for i in range(0, count, 16):
            out.write("    " + ", ".join("%2d" % w for w in widths[i:i + 16]) + ",\n")
        out.write("};\n")

    preview = atlas.resize((atlas.width * 2, atlas.height * 2), Image.NEAREST)
    preview.save(os.path.join(ASSETS, "preview_font_%s.png" % name))

    print("fonte %s (%d px): celula %dx%d, atlas %dx%d"
          % (name, size, cell_w, cell_h, atlas.width, atlas.height))


def gradient(size, top, bottom):
    w, h = size
    img = Image.new("RGB", (w, h))
    d = ImageDraw.Draw(img)

    for y in range(h):
        t = y / max(1, h - 1)
        d.line([(0, y), (w, y)], fill=tuple(round(a + (b - a) * t) for a, b in zip(top, bottom)))

    return img


def make_logo():
    """
    assets/logo.png: "PS2DVD" (azul forte) + "Player" (azul claro) em
    Noto Sans Bold, como no interface-model.png, com brilho azul por
    baixo. Desenhado em 4x e reduzido (suavizacao).
    """
    scale = 4
    size = 40 * scale
    parts = [("PS2DVD", (0x7C, 0xC4, 0xFF), (0x1E, 0x6C, 0xFF)),
             ("Player", (0xF0, 0xF6, 0xFF), (0x8C, 0xBC, 0xFF))]

    font = ImageFont.truetype(FONT_BOLD, size)
    text = "".join(p[0] for p in parts)
    left, top, right, bottom = font.getbbox(text)
    pad = 6 * scale
    w = right - left + 2 * pad
    h = bottom - top + 2 * pad

    letters = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    mask_all = Image.new("L", (w, h), 0)
    x = pad - left

    for part, c_top, c_bottom in parts:
        mask = Image.new("L", (w, h), 0)
        ImageDraw.Draw(mask).text((x, pad - top), part, fill=255, font=font)
        ImageDraw.Draw(mask_all).text((x, pad - top), part, fill=255, font=font)

        colored = gradient((w, h), c_top, c_bottom).convert("RGBA")
        colored.putalpha(mask)
        letters = Image.alpha_composite(letters, colored)

        x += font.getlength(part)

    glow_alpha = mask_all.filter(ImageFilter.GaussianBlur(3 * scale)).point(lambda v: int(min(255, v * 2) * 0.45))
    glow = Image.new("RGBA", (w, h), (0x2F, 0x7B, 0xFF, 0))
    glow.putalpha(glow_alpha)

    logo = Image.alpha_composite(glow, letters)
    logo = logo.resize((w // scale, h // scale), Image.LANCZOS)
    logo.save(os.path.join(ASSETS, "logo.png"), optimize=True)

    preview = Image.new("RGBA", logo.size, (0x0C, 0x1C, 0x40, 255))
    Image.alpha_composite(preview, logo).resize((logo.width * 2, logo.height * 2), Image.NEAREST) \
        .save(os.path.join(ASSETS, "preview_logo.png"))

    print("logo: %dx%d" % logo.size)


BUTTONS_SRC = os.path.join(ROOT, "Kit de Botões de Controle PlayStation.png")

# Recortes do kit (1536x1024), na ordem do UiButton (include/ui.h).
# ("circle", cx, cy, r) | ("rrect", x0, y0, x1, y1, raio) |
# ("poly", [pontos]) | None (desenhado pelo codigo), e o tamanho final.
BUTTONS = [
    ("cross",    ("circle", 243, 166, 128), (24, 24)),
    ("circle",   ("circle", 565, 166, 128), (24, 24)),
    ("square",   ("circle", 1270, 166, 128), (24, 24)),
    ("triangle", ("circle", 919, 166, 128), (24, 24)),
    ("l1",       ("rrect", 118, 337, 374, 474, 34), (36, 20)),
    ("r1",       ("rrect", 799, 337, 1061, 474, 34), (36, 20)),
    ("select",   None, None),
    ("start",    None, None),
    # Direcional: a tampa da tecla "cima" do kit com uma seta clara
    # desenhada por cima (a seta original some no tamanho pequeno).
    ("up",       ("key", "up"), (22, 22)),
    ("down",     ("key", "down"), (22, 22)),
    ("left",     ("key", "left"), (22, 22)),
    ("right",    ("key", "right"), (22, 22)),
    ("l2",       ("rrect", 444, 337, 702, 474, 34), (36, 20)),
    ("r2",       ("rrect", 1149, 337, 1415, 474, 34), (36, 20)),
]


def make_buttons():
    """
    assets/buttons.png (atlas RGBA, uma linha) + assets/buttons.h
    (retangulo de cada botao). Cada botao e recortado do kit com uma
    mascara (fundo transparente) e reduzido com alfa pre-multiplicado.
    """
    src = Image.open(BUTTONS_SRC).convert("RGBA")
    icons = []

    for name, shape, size in BUTTONS:

        if shape is None:
            icons.append((name, None))
            continue

        mask = Image.new("L", src.size, 0)
        md = ImageDraw.Draw(mask)

        if shape[0] == "circle":
            _, cx, cy, r = shape
            box = (cx - r, cy - r, cx + r, cy + r)
            md.ellipse(box, fill=255)
        elif shape[0] == "key":
            # Parte quadrada da tecla "cima" (sem a ponta).
            box = (238, 672, 364, 768)
            md.rounded_rectangle(box, 20, fill=255)
        elif shape[0] == "rrect":
            _, x0, y0, x1, y1, radius = shape
            box = (x0, y0, x1, y1)
            md.rounded_rectangle(box, radius, fill=255)
        else:
            points = shape[1]
            box = (min(p[0] for p in points), min(p[1] for p in points),
                   max(p[0] for p in points), max(p[1] for p in points))
            md.polygon(points, fill=255)

        piece = src.copy()
        piece.putalpha(mask)
        piece = piece.crop(box)

        if shape[0] == "key":
            # Tampa sem a seta original: estica uma faixa lisa da
            # borda esquerda, depois desenha a seta.
            w, h = piece.size
            plain = piece.crop((0, 0, 40, h))
            cap = Image.new("RGBA", (w, h), (0, 0, 0, 0))
            cap.paste(plain, (0, 0))
            middle = piece.crop((30, 0, 40, h)).resize((w - 80, h))
            cap.paste(middle, (40, 0))
            cap.paste(plain.transpose(Image.FLIP_LEFT_RIGHT), (w - 40, 0))
            cap.putalpha(piece.getchannel("A"))
            piece = cap.resize((max(w, h), max(w, h)), Image.LANCZOS)

            s = piece.width
            arrows = {
                "up":    [(s * 0.5, s * 0.24), (s * 0.78, s * 0.70), (s * 0.22, s * 0.70)],
                "down":  [(s * 0.22, s * 0.30), (s * 0.78, s * 0.30), (s * 0.5, s * 0.76)],
                "left":  [(s * 0.24, s * 0.5), (s * 0.70, s * 0.22), (s * 0.70, s * 0.78)],
                "right": [(s * 0.76, s * 0.5), (s * 0.30, s * 0.22), (s * 0.30, s * 0.78)],
            }
            ImageDraw.Draw(piece).polygon(arrows[shape[1]], fill=(0xD8, 0xDE, 0xEA, 255))

        icon = piece.convert("RGBa").resize(size, Image.LANCZOS).convert("RGBA")
        icons.append((name, icon))

    width = sum(icon.width + 1 for _, icon in icons if icon)
    height = max(icon.height for _, icon in icons if icon)

    atlas = Image.new("RGBA", (width, height), (0, 0, 0, 0))
    rects = []
    x = 0

    for name, icon in icons:
        if icon is None:
            rects.append((name, 0, 0, 0, 0))
            continue
        atlas.paste(icon, (x, 0))
        rects.append((name, x, 0, icon.width, icon.height))
        x += icon.width + 1

    atlas.save(os.path.join(ASSETS, "buttons.png"), optimize=True)

    with open(os.path.join(ASSETS, "buttons.h"), "w") as out:
        out.write("/* Gerado por tools/make_assets.py: retangulos do assets/buttons.png,\n"
                  "   na ordem do UiButton (largura 0 = desenhado pelo codigo). */\n")
        out.write("static const unsigned short button_rects[%d][4] = {\n" % len(rects))
        for name, rx, ry, rw, rh in rects:
            out.write("    { %3d, %2d, %2d, %2d },   /* %s */\n" % (rx, ry, rw, rh, name))
        out.write("};\n")

    # Previa ampliada sobre o azul da interface (nao vai para o ELF).
    preview = Image.new("RGBA", (width, height), (0x0B, 0x16, 0x30, 255))
    preview = Image.alpha_composite(preview, atlas)
    preview.resize((width * 4, height * 4), Image.NEAREST).save(os.path.join(ASSETS, "preview_buttons.png"))

    print("botoes: atlas %dx%d" % (width, height))

def main():
    os.makedirs(ASSETS, exist_ok=True)

    if len(sys.argv) > 1 and sys.argv[1] == "buttons":
        make_buttons()
        return 0

    for name, size in FONTS:
        make_font(name, size)

    make_logo()
    make_buttons()
    return 0


if __name__ == "__main__":
    sys.exit(main())
