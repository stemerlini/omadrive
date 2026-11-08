#!/usr/bin/env python3
"""Draws the pixel-art icon and README banner as SVG (and PNG via rsvg-convert)."""
import random, subprocess, pathlib

HERE = pathlib.Path(__file__).resolve().parent
ROOT = HERE.parent

PALETTE = {
    'K': '#0e0e10',  # outline
    'Y': '#ffd60a',  # body
    'H': '#fff3a0',  # body highlight
    'D': '#c9a400',  # body shade
    'S': '#9aa3b2',  # shutter
    'L': '#cfd6e2',  # shutter highlight
    's': '#2b2d33',  # shutter slot
    'W': '#f4efe1',  # label
    'C': '#4cc9f0',  # cloud
    'c': '#2f7fd8',  # cloud shade
    'w': '#ffffff',  # cloud highlight
}

FLOPPY = [
    '.KKKKKKKKKKKKKKKKK..',
    'KHHHKLLLLLLLLLKYYKK.',
    'KHYYKLSSSsssSSKYYYKK',
    'KHYYKLSSSsssSSKYYYYK',
    'KHYYKLSSSsssSSKYYYYK',
    'KHYYKLSSSsssSSKYYYYK',
    'KHYYKKKKKKKKKKKYYYYK',
    'KHYYYYYYYYYYYYYYYYDK',
    'KHYKKKKKKKKKKKKKKYDK',
    'KHYKWWWWWWWWWWWWKYDK',
    'KHYKWWWWWCCCWWWWKYDK',
    'KHYKWWWWCwwCCWWWKYDK',
    'KHYKWWCCCwCCCCWWKYDK',
    'KHYKWCwCCCCCCCCWKYDK',
    'KHYKWCCCCCCCCCcWKYDK',
    'KHYKWWccccccccWWKYDK',
    'KHYKWWWWWWWWWWWWKYDK',
    'KHYKWWWWWWWWWWWWKYDK',
    'KYDDDDDDDDDDDDDDDDDK',
    '.KKKKKKKKKKKKKKKKKK.',
]

FONT = {
    'A': '01110 10001 10001 11111 10001 10001 10001', 'B': '11110 10001 10001 11110 10001 10001 11110',
    'C': '01110 10001 10000 10000 10000 10001 01110', 'D': '11110 10001 10001 10001 10001 10001 11110',
    'E': '11111 10000 10000 11110 10000 10000 11111', 'F': '11111 10000 10000 11110 10000 10000 10000',
    'G': '01110 10001 10000 10111 10001 10001 01111', 'H': '10001 10001 10001 11111 10001 10001 10001',
    'I': '01110 00100 00100 00100 00100 00100 01110', 'J': '00111 00010 00010 00010 00010 10010 01100',
    'K': '10001 10010 10100 11000 10100 10010 10001', 'L': '10000 10000 10000 10000 10000 10000 11111',
    'M': '10001 11011 10101 10101 10001 10001 10001', 'N': '10001 10001 11001 10101 10011 10001 10001',
    'O': '01110 10001 10001 10001 10001 10001 01110', 'P': '11110 10001 10001 11110 10000 10000 10000',
    'Q': '01110 10001 10001 10001 10101 10010 01101', 'R': '11110 10001 10001 11110 10100 10010 10001',
    'S': '01111 10000 10000 01110 00001 00001 11110', 'T': '11111 00100 00100 00100 00100 00100 00100',
    'U': '10001 10001 10001 10001 10001 10001 01110', 'V': '10001 10001 10001 10001 10001 01010 00100',
    'W': '10001 10001 10001 10101 10101 10101 01010', 'X': '10001 10001 01010 00100 01010 10001 10001',
    'Y': '10001 10001 01010 00100 00100 00100 00100', 'Z': '11111 00001 00010 00100 01000 10000 11111',
    '+': '00000 00100 00100 11111 00100 00100 00000', '-': '00000 00000 00000 11111 00000 00000 00000',
    '/': '00001 00010 00010 00100 01000 01000 10000', ' ': '00000 00000 00000 00000 00000 00000 00000',
}


def pixels(grid, x0, y0, size, palette=PALETTE):
    out = []
    for y, row in enumerate(grid):
        for x, ch in enumerate(row):
            if ch in palette:
                out.append(f'<rect x="{x0 + x * size}" y="{y0 + y * size}" width="{size}" height="{size}" fill="{palette[ch]}"/>')
    return out


def text_pixels(text, x0, y0, size, color):
    out = []
    for i, ch in enumerate(text):
        rows = FONT[ch].split()
        for y, row in enumerate(rows):
            for x, bit in enumerate(row):
                if bit == '1':
                    out.append(f'<rect x="{x0 + (i * 6 + x) * size}" y="{y0 + y * size}" width="{size}" height="{size}" fill="{color}"/>')
    return out


def text_width(text, size):
    return (len(text) * 6 - 1) * size


def icon_svg():
    # 24x24 grid: the 20x20 floppy with a hard pixel shadow.
    shadow = {k: '#000000' for k in PALETTE}
    body = pixels(FLOPPY, 2, 2, 1)
    shade = [r.replace('fill="#000000"', 'fill="#000000" fill-opacity="0.35"') for r in pixels(FLOPPY, 3, 3, 1, shadow)]
    return ('<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 24 24" width="256" height="256" shape-rendering="crispEdges">\n'
            + '\n'.join(shade + body) + '\n</svg>\n')


def banner_svg(width=1280, height=400):
    rnd = random.Random(7)
    horizon = 300
    parts = [f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {width} {height}" width="{width}" height="{height}">',
             '<defs>',
             '<linearGradient id="sky" x1="0" y1="0" x2="0" y2="1"><stop offset="0" stop-color="#0b0420"/>'
             '<stop offset="0.6" stop-color="#2a0b4a"/><stop offset="1" stop-color="#7a1a6e"/></linearGradient>',
             '<linearGradient id="sun" x1="0" y1="0" x2="0" y2="1"><stop offset="0" stop-color="#ffd60a"/>'
             '<stop offset="1" stop-color="#ff2a6d"/></linearGradient>',
             '<linearGradient id="floor" x1="0" y1="0" x2="0" y2="1"><stop offset="0" stop-color="#1a0630"/>'
             '<stop offset="1" stop-color="#05010d"/></linearGradient>',
             f'<clipPath id="sunclip"><rect x="0" y="0" width="{width}" height="{horizon}"/></clipPath>',
             '</defs>',
             f'<rect width="{width}" height="{height}" fill="url(#sky)"/>']
    # Stars.
    for _ in range(90):
        x, y = rnd.randrange(0, width, 4), rnd.randrange(0, horizon - 60, 4)
        s = rnd.choice([2, 2, 2, 4])
        parts.append(f'<rect x="{x}" y="{y}" width="{s}" height="{s}" fill="#ffffff" fill-opacity="{rnd.choice([0.35, 0.6, 0.9])}"/>')
    # Striped synthwave sun sitting on the horizon.
    cx, r = width / 2, 85
    sun = [f'<circle cx="{cx}" cy="{horizon}" r="{r}" fill="url(#sun)"/>']
    y, gap = horizon - 40, 3
    while y < horizon:
        sun.append(f'<rect x="{cx - r}" y="{y}" width="{2 * r}" height="{gap}" fill="#2a0b4a" fill-opacity="0.9"/>')
        y += 11
        gap += 2
    parts.append('<g clip-path="url(#sunclip)" opacity="0.9">' + ''.join(sun) + '</g>')
    # Perspective grid floor.
    parts.append(f'<rect x="0" y="{horizon}" width="{width}" height="{height - horizon}" fill="url(#floor)"/>')
    parts.append(f'<rect x="0" y="{horizon}" width="{width}" height="3" fill="#ff2a6d"/>')
    grid = []
    for i in range(-24, 25):
        x_far = cx + i * 40
        x_near = cx + i * 260
        grid.append(f'<line x1="{x_far}" y1="{horizon}" x2="{x_near}" y2="{height}"/>')
    d = 6
    while horizon + d < height:
        grid.append(f'<line x1="0" y1="{horizon + d}" x2="{width}" y2="{horizon + d}"/>')
        d = int(d * 1.55) + 4
    parts.append('<g stroke="#ff2a6d" stroke-opacity="0.55" stroke-width="2">' + ''.join(grid) + '</g>')

    # Icon, title and tagline, centred as a group.
    title, tag = 'OMADRIVE', 'FILE MANAGER FOR OMARCHY  -  LOCAL + ONEDRIVE'
    ts, gs, isz = 14, 3, 7
    icon_w = 20 * isz
    block = icon_w + 48 + text_width(title, ts)
    x0 = (width - block) / 2
    ty = 70
    shadow = {k: '#000000' for k in PALETTE}
    parts += [r.replace('fill="#000000"', 'fill="#000000" fill-opacity="0.45"') for r in pixels(FLOPPY, x0 + isz, ty - 14 + isz, isz, shadow)]
    parts += pixels(FLOPPY, x0, ty - 14, isz)
    tx = x0 + icon_w + 48
    parts += text_pixels(title, tx + ts // 2, ty + ts // 2, ts, '#ff2a6d')
    parts += text_pixels(title, tx, ty, ts, '#ffd60a')
    tagx = tx + (text_width(title, ts) - text_width(tag, gs)) / 2
    parts += text_pixels(tag, tagx + gs, ty + 7 * ts + 22 + gs, gs, '#000000')
    parts += text_pixels(tag, tagx, ty + 7 * ts + 22, gs, '#7af7ff')
    parts.append('</svg>')
    return '\n'.join(parts) + '\n'


if __name__ == '__main__':
    (ROOT / 'pkgbuild/omadrive.svg').write_text(icon_svg())
    (HERE / 'banner.svg').write_text(banner_svg())
    subprocess.run(['rsvg-convert', '-o', str(HERE / 'banner.png'), str(HERE / 'banner.svg')], check=True)
    subprocess.run(['rsvg-convert', '-w', '256', '-h', '256', '-o', str(HERE / 'icon.png'), str(ROOT / 'pkgbuild/omadrive.svg')], check=True)
