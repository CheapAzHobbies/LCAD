#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Draws LCAD's icon for Windows (.ico) and macOS (.icns).

The icon is packaging/io.github.cheapazhobbies.LCAD.svg. Windows and macOS want it as bitmaps up
to 256 and 1024 pixels, so this draws the same shapes with Pillow, at four times the size, and
scales them down for smooth edges. Run it again after changing the SVG.

Usage: scripts/make-platform-icons.py
"""

from pathlib import Path

from PIL import Image, ImageDraw

ROOT = Path(__file__).resolve().parent.parent
OVERSAMPLE = 4


def draw(size: int) -> Image.Image:
    """The icon at `size` pixels square, as the SVG's 256-unit view box draws it."""
    big = size * OVERSAMPLE
    scale = big / 256.0
    image = Image.new("RGBA", (big, big), (0, 0, 0, 0))
    pen = ImageDraw.Draw(image)

    def at(*points):
        return [(x * scale, y * scale) for x, y in points]

    pen.rounded_rectangle(
        [8 * scale, 8 * scale, 248 * scale, 248 * scale], radius=52 * scale, fill="#2e3136"
    )
    pen.polygon(at((128, 44), (200, 86), (128, 128), (56, 86)), fill="#3d8bfd")
    pen.polygon(at((56, 86), (128, 128), (128, 212), (56, 170)), fill="#c9d1db")
    pen.polygon(at((128, 128), (200, 86), (200, 170), (128, 212)), fill="#8f99a6")
    width = round(5 * scale)
    # Round past the start once more, so that the corner there is joined like the others.
    outline = at(
        (128, 44), (200, 86), (200, 170), (128, 212), (56, 170), (56, 86), (128, 44), (200, 86)
    )
    pen.line(outline, fill="#1e2024", width=width, joint="curve")
    pen.line(at((56, 86), (128, 128), (200, 86)), fill="#1e2024", width=width, joint="curve")
    pen.line(at((128, 128), (128, 212)), fill="#1e2024", width=width)
    return image.resize((size, size), Image.Resampling.LANCZOS)


def main() -> None:
    largest = draw(1024)
    windows = ROOT / "packaging" / "windows" / "LCAD.ico"
    draw(256).save(windows, sizes=[(s, s) for s in (16, 24, 32, 48, 64, 128, 256)])
    macos = ROOT / "packaging" / "macos" / "LCAD.icns"
    largest.save(macos)
    print(f"Wrote {windows.relative_to(ROOT)} and {macos.relative_to(ROOT)}")


if __name__ == "__main__":
    main()
