#!/usr/bin/env python3
"""Convierte una .deltaskin de Delta en una para DeltaVita.

Toma la representación iphone/standard (667x375, casi la misma proporción que la pantalla de la Vita)
en vertical y horizontal, y renderiza sus imágenes PDF a PNG con transparencia al tamaño exacto de la
Vita (960x544 / 544x960), porque en la consola no hay renderizador de PDF.

Uso: deltaskin2vita.py entrada.deltaskin salida.deltaskin
Requiere poppler (pdftocairo) y Pillow.
"""
import io
import json
import os
import subprocess
import sys
import tempfile
import zipfile

from PIL import Image

VIEW = {"landscape": (960, 544), "portrait": (544, 960)}


def render(data: bytes, name: str, w: int, h: int) -> bytes:
    w, h = max(1, w), max(1, h)
    if name.lower().endswith(".pdf"):
        with tempfile.TemporaryDirectory() as tmp:
            src = os.path.join(tmp, "in.pdf")
            with open(src, "wb") as f:
                f.write(data)
            out = os.path.join(tmp, "out")
            subprocess.run(["pdftocairo", "-png", "-transp", "-singlefile", "-scale-to-x", str(w), "-scale-to-y", str(h), src, out],
                           check=True, capture_output=True)
            img = Image.open(out + ".png").convert("RGBA")
    else:
        img = Image.open(io.BytesIO(data)).convert("RGBA").resize((w, h), Image.LANCZOS)
    buf = io.BytesIO()
    img.save(buf, "PNG", optimize=True)
    return buf.getvalue()


def convert(inp: str, outp: str) -> None:
    z = zipfile.ZipFile(inp)
    info = json.loads(z.read("info.json"))
    reps = info["representations"]
    dev = reps.get("iphone") or reps.get("ipad")
    if not dev:
        sys.exit(f"{inp}: sin representación de iPhone ni iPad")
    files: dict[str, bytes] = {}
    out_dev: dict = {"standard": {}}
    for ori, (vw, vh) in VIEW.items():
        rep = (dev.get("standard") or {}).get(ori) or (dev.get("edgeToEdge") or {}).get(ori)
        if not rep:
            print(f"  aviso: {os.path.basename(inp)} no tiene {ori}")
            continue
        ms = rep["mappingSize"]
        k = min(vw / ms["width"], vh / ms["height"])
        assets = rep["assets"]
        src = next(assets[key] for key in ("resizable", "large", "medium", "small") if key in assets)
        png = f"vita_{ori}.png"
        files[png] = render(z.read(src), src, round(ms["width"] * k), round(ms["height"] * k))
        rep["assets"] = {"large": png}
        for item in rep.get("items", []):
            t = item.get("thumbstick")
            if t:
                tpng = f"vita_{ori}_thumbstick.png"
                if tpng not in files:
                    files[tpng] = render(z.read(t["name"]), t["name"], round(t["width"] * k), round(t["height"] * k))
                t["name"] = tpng
        out_dev["standard"][ori] = rep
    if not out_dev["standard"]:
        sys.exit(f"{inp}: nada que convertir")
    info["representations"] = {"iphone": out_dev}
    with zipfile.ZipFile(outp, "w", zipfile.ZIP_DEFLATED) as o:
        o.writestr("info.json", json.dumps(info, ensure_ascii=False))
        for n, data in files.items():
            o.writestr(n, data)


if __name__ == "__main__":
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    convert(sys.argv[1], sys.argv[2])
