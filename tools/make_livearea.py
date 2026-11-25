#!/usr/bin/env python3
"""LiveArea de DeltaVita (estilo VitaJ2ME): pic0 960x544, bg0 840x500, startup 280x158, icon0 128x128.
Usa el ícono original de DeltaDroid/tools/make_icon.py. Salida PNG indexado de 8 bits, como exige la Vita.
Uso: make_livearea.py <ruta a DeltaDroid/tools/make_icon.py> <carpeta sce_sys>"""
import importlib.util
import math
import os
import sys

from PIL import Image, ImageDraw, ImageFilter, ImageFont

HELV = "/System/Library/Fonts/Helvetica.ttc"
TOP, BOT = (64, 36, 170), (16, 58, 150)   # violeta -> azul, más oscuros que el ícono para que este resalte


def font(px, bold):
    return ImageFont.truetype(HELV, px, index=1 if bold else 0)


def backdrop(w, h, grid):
    """Degradado diagonal + viñeta radial (+ cuadrícula sutil como el bg0 de VitaJ2ME)."""
    img = Image.new("RGB", (w, h))
    px = img.load()
    cx, cy, rmax = w * 0.5, h * 0.42, math.hypot(w, h) * 0.62
    for y in range(h):
        for x in range(w):
            t = (x / w * 0.35 + y / h * 0.65)
            base = [a + (b - a) * t for a, b in zip(TOP, BOT)]
            v = 1.12 - 0.62 * min(1, math.hypot(x - cx, y - cy) / rmax)  # centro más claro
            px[x, y] = tuple(max(0, min(255, int(c * v))) for c in base)
    if grid:
        over = Image.new("RGBA", (w, h), (0, 0, 0, 0))
        d = ImageDraw.Draw(over)
        for g in range(0, w, 40):
            d.line([(g, 0), (g, h)], fill=(255, 255, 255, 14))
        for g in range(0, h, 40):
            d.line([(0, g), (w, g)], fill=(255, 255, 255, 14))
        img = Image.alpha_composite(img.convert("RGBA"), over).convert("RGB")
    return img


def rounded_icon(make_icon, size):
    ic = make_icon.icon(size * 4).resize((size, size), Image.LANCZOS).convert("RGBA")
    mask = Image.new("L", (size, size), 0)
    ImageDraw.Draw(mask).rounded_rectangle((0, 0, size - 1, size - 1), radius=int(size * 0.22), fill=255)
    ic.putalpha(mask)
    shadow = Image.new("RGBA", (size + 40, size + 40), (0, 0, 0, 0))
    ImageDraw.Draw(shadow).rounded_rectangle((20, 26, size + 20, size + 26), radius=int(size * 0.22), fill=(0, 0, 0, 110))
    shadow = shadow.filter(ImageFilter.GaussianBlur(10))
    shadow.alpha_composite(ic, (20, 20))
    return shadow


def wordmark(d, x, y, px, center=False):
    """'Delta' en negrita + 'Vita' fina en celeste, como 'Vita' + 'J2ME'."""
    a, b = font(px, True), font(px, False)
    wa = d.textlength("Delta", font=a)
    wb = d.textlength("Vita", font=b)
    if center:
        x -= (wa + wb) / 2
    d.text((x, y), "Delta", font=a, fill=(255, 255, 255))
    d.text((x + wa, y), "Vita", font=b, fill=(170, 200, 255))
    return wa + wb


def pal(im):
    """8 bits indexado con difuminado: en Pillow el dither solo se aplica al usar una paleta ya calculada."""
    rgb = im.convert("RGB")
    return rgb.quantize(palette=rgb.quantize(256, method=Image.Quantize.FASTOCTREE), dither=Image.Dither.FLOYDSTEINBERG)


def main(make_icon_path, out):
    spec = importlib.util.spec_from_file_location("make_icon", make_icon_path)
    make_icon = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(make_icon)
    os.makedirs(os.path.join(out, "livearea/contents"), exist_ok=True)

    # pic0: pantalla de presentación
    pic = backdrop(960, 544, grid=False).convert("RGBA")
    pic.alpha_composite(rounded_icon(make_icon, 132), (int(480 - 86), 108))
    d = ImageDraw.Draw(pic)
    wordmark(d, 480, 292, 62, center=True)
    sub = "Classic consoles for PS Vita"
    f = font(24, False)
    d.text((480 - d.textlength(sub, font=f) / 2, 378), sub, font=f, fill=(205, 215, 245))
    tag = "Unofficial · inspired by Delta"
    f2 = font(17, False)
    d.text((480 - d.textlength(tag, font=f2) / 2, 414), tag, font=f2, fill=(160, 170, 215))
    pal(pic).save(os.path.join(out, "pic0.png"))

    # bg0: fondo de la LiveArea (la cuadrícula da textura sin competir con la burbuja)
    pal(backdrop(840, 500, grid=True)).save(os.path.join(out, "livearea/contents/bg0.png"))

    # startup: botón de inicio
    st = backdrop(280, 158, grid=False).convert("RGBA")
    st.alpha_composite(rounded_icon(make_icon, 58), (2, 30))
    d = ImageDraw.Draw(st)
    wordmark(d, 92, 52, 25)
    d.text((93, 86), "Classic consoles", font=font(14, False), fill=(205, 215, 245))
    pal(st).save(os.path.join(out, "livearea/contents/startup.png"))

    # icon0
    pal(make_icon.icon(512).resize((128, 128), Image.LANCZOS)).save(os.path.join(out, "icon0.png"))

    with open(os.path.join(out, "livearea/contents/template.xml"), "w") as fh:
        fh.write('<?xml version="1.0" encoding="utf-8"?>\n\n<livearea style="a1" format-ver="01.00" content-rev="1">\n'
                 '\t<livearea-background>\n\t\t<image>bg0.png</image>\n\t</livearea-background>\n\n'
                 '\t<gate>\n\t\t<startup-image>startup.png</startup-image>\n\t</gate>\n</livearea>\n')


if __name__ == "__main__":
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    main(sys.argv[1], sys.argv[2])
