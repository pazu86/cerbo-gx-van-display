"""Converts an image into the boot splash: a 1024x600 baseline JPEG the firmware decodes at boot.

    pip install pillow numpy
    python tools/make_splash.py assets/splash-source.png [--anchor 0.5] [--quality 90]

Writes firmware/main/splash/splash.jpg (embedded by main/CMakeLists.txt) and a preview PNG of
what the panel will show. Why JPEG: with SPIRAM_XIP_FROM_PSRAM everything embedded in the
firmware is copied into PSRAM for good, and a raw 1024x600 RGB565 image (1.2 MB) doesn't fit.
splash.c decodes the JPEG (ROM TJpgDec) into a temporary buffer and frees it afterwards.

An image that isn't 1024x600 is scaled to cover the panel and cropped; --anchor picks where the
vertical crop comes from (0 = keep the top, 0.5 = centre, 1 = keep the bottom). The preview
applies the same 4x4 Bayer dither down to RGB565 as the firmware.
"""
import argparse
import io
import pathlib

import numpy as np
from PIL import Image

W, H = 1024, 600
ROOT = pathlib.Path(__file__).resolve().parent.parent
OUT = ROOT / "firmware" / "main" / "splash" / "splash.jpg"
BAYER = np.array([[0, 8, 2, 10], [12, 4, 14, 6], [3, 11, 1, 9], [15, 7, 13, 5]], dtype=np.float32) / 16.0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("src")
    ap.add_argument("--anchor", type=float, default=0.5, help="vertical crop position, 0 = top .. 1 = bottom")
    ap.add_argument("--quality", type=int, default=90)
    args = ap.parse_args()

    img = Image.open(args.src).convert("RGB")
    if img.size != (W, H):
        scale = max(W / img.width, H / img.height)
        img = img.resize((round(img.width * scale), round(img.height * scale)), Image.LANCZOS)
        x = (img.width - W) // 2
        y = round((img.height - H) * args.anchor)
        img = img.crop((x, y, x + W, y + H))

    # Baseline (TJpgDec can't do progressive), 4:2:0
    buf = io.BytesIO()
    img.save(buf, "JPEG", quality=args.quality, subsampling=2, progressive=False, optimize=True)
    OUT.parent.mkdir(parents=True, exist_ok=True)
    OUT.write_bytes(buf.getvalue())

    # Preview: the decoded JPEG, dithered to RGB565 like splash.c does
    rgb = np.asarray(Image.open(io.BytesIO(buf.getvalue())).convert("RGB"), dtype=np.int32)
    bayer = (np.tile(BAYER, (H // 4 + 1, W // 4))[:H, :, None] * 16).astype(np.int32)   # 0..15
    steps = np.array([8, 4, 8], dtype=np.int32)
    q = np.minimum((rgb + bayer * steps // 16) // steps, np.array([31, 63, 31]))
    preview = np.stack([(q[..., 0] << 3) | (q[..., 0] >> 2), (q[..., 1] << 2) | (q[..., 1] >> 4),
                        (q[..., 2] << 3) | (q[..., 2] >> 2)], axis=-1).astype(np.uint8)
    Image.fromarray(preview).save(OUT.with_suffix(".preview.png"))
    print(f"wrote {OUT} ({OUT.stat().st_size} bytes) and {OUT.with_suffix('.preview.png').name}")


if __name__ == "__main__":
    main()
