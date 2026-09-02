#!/usr/bin/env python3
"""Rasterize one design-owned SVG into an RGBA PNG for the LVGLImage step.

SVG sources are the single editable truth for image assets: colors must be
baked into the file (no ``currentColor``), and the manifest record carries the
export width/height. Rendering uses 4x supersampling so 16px-grid iconography
(例如 QWeather 图标)downscales without aliasing.
"""

import argparse
import sys

import fitz
from PIL import Image

SUPERSCAN = 4


def rasterize(svg_path: str, png_path: str, width: int, height: int) -> int:
    with open(svg_path, "r", encoding="utf-8") as handle:
        text = handle.read()
    if "currentColor" in text:
        print(
            f"{svg_path}: currentColor must be replaced with an explicit hex color",
            file=sys.stderr,
        )
        return 1
    document = fitz.open("svg", text.encode("utf-8"))
    page = document[0]
    rect = page.rect
    if rect.width <= 0 or rect.height <= 0:
        print(f"{svg_path}: empty viewBox", file=sys.stderr)
        return 1
    matrix = fitz.Matrix(
        width * SUPERSCAN / rect.width, height * SUPERSCAN / rect.height
    )
    pixmap = page.get_pixmap(matrix=matrix, alpha=True)
    image = Image.frombytes("RGBA", (pixmap.width, pixmap.height), pixmap.samples)
    image = image.resize((width, height), Image.LANCZOS)
    image.save(png_path)
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--width", type=int, required=True)
    parser.add_argument("--height", type=int, required=True)
    parser.add_argument("--output", required=True)
    parser.add_argument("source")
    args = parser.parse_args()
    if args.width < 1 or args.height < 1:
        print("width/height must be positive", file=sys.stderr)
        return 1
    return rasterize(args.source, args.output, args.width, args.height)


if __name__ == "__main__":
    sys.exit(main())
