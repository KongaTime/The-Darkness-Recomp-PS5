#!/usr/bin/env python3
"""The Darkness on PS5 - the home-screen icon, drawn here instead of shipped.

    app/ps5/tools/title-art.py [--out app/ps5/sce_sys]

The console will not register a home-screen entry for a title whose sce_sys has
no icon0.png. ShadowMountPlus copies the staged icon into the installed app and
abandons the install when there is nothing to copy

    [ACTION] Installing: The Darkness (PPSA99301)
    [COPY] Failed to copy staged icon: .../sce_sys/icon0.png -> /user/app/...

which is what "Installing: The Darkness" followed by nothing on the home screen
was. So the title ships its own 512x512 icon, drawn by this script with nothing
but the standard library: the build machines have no Pillow, and the game's own
cover art is the player's, not ours to check in.

The picture is an eclipse with the Darkness eating through it - a black disc in
a crimson rim, one lit tear across the face, and the name under it.

Copyright (C) 2026 KongaTime
SPDX-License-Identifier: MIT
"""

import argparse
import math
import struct
import zlib
from pathlib import Path

SIZE = 512
CENTRE = (256.0, 206.0)
RADIUS = 126.0

# A 5x7 face for the name: chunky on purpose, it reads at icon size.
GLYPHS = {
    "T": ("#####", "..#..", "..#..", "..#..", "..#..", "..#..", "..#.."),
    "H": ("#...#", "#...#", "#...#", "#####", "#...#", "#...#", "#...#"),
    "E": ("#####", "#....", "#....", "####.", "#....", "#....", "#####"),
    "D": ("####.", "#...#", "#...#", "#...#", "#...#", "#...#", "####."),
    "A": (".###.", "#...#", "#...#", "#####", "#...#", "#...#", "#...#"),
    "R": ("####.", "#...#", "#...#", "####.", "#.#..", "#..#.", "#...#"),
    "K": ("#...#", "#..#.", "#.#..", "##...", "#.#..", "#..#.", "#...#"),
    "N": ("#...#", "##..#", "#.#.#", "#..##", "#...#", "#...#", "#...#"),
    "S": (".####", "#....", "#....", ".###.", "....#", "....#", "####."),
}

# The tear: a jagged polyline across the disc, wider in the middle.
TEAR = ((182, 88), (216, 146), (192, 180), (252, 210), (226, 246),
        (286, 276), (258, 312), (312, 336))


def smoothstep(edge0, edge1, value):
    t = (value - edge0) / (edge1 - edge0)
    t = min(1.0, max(0.0, t))
    return t * t * (3.0 - 2.0 * t)


def segment_distance(px, py, ax, ay, bx, by):
    vx, vy = bx - ax, by - ay
    length = vx * vx + vy * vy
    t = 0.0 if length == 0.0 else ((px - ax) * vx + (py - ay) * vy) / length
    t = min(1.0, max(0.0, t))
    dx, dy = px - (ax + vx * t), py - (ay + vy * t)
    return math.hypot(dx, dy)


def tear_distance(x, y):
    """Distance to the tear, and how far along it the nearest point is."""
    best, along = 1e9, 0.0
    for index in range(len(TEAR) - 1):
        ax, ay = TEAR[index]
        bx, by = TEAR[index + 1]
        vx, vy = bx - ax, by - ay
        length = vx * vx + vy * vy
        t = 0.0 if length == 0.0 else ((x - ax) * vx + (y - ay) * vy) / length
        t = min(1.0, max(0.0, t))
        distance = math.hypot(x - (ax + vx * t), y - (ay + vy * t))
        if distance < best:
            best = distance
            along = (index + t) / (len(TEAR) - 1)
    return best, along


def background(x, y):
    """Dark room, warm floor, vignette - the base the eclipse sits on."""
    v = y / SIZE
    r = 9.0 + 13.0 * v * v
    g = 9.0 + 5.0 * v * v
    b = 12.0 + 7.0 * v * v
    edge = smoothstep(0.22, 0.78, math.hypot(x / SIZE - 0.5, v - 0.52))
    damp = 1.0 - 0.55 * edge
    return r * damp, g * damp, b * damp


def eclipse(colour, glow, x, y):
    """The disc, its rim and the tear, written into the colour and glow planes."""
    r, g, b = colour
    cx, cy = CENTRE
    d = math.hypot(x - cx, y - cy)

    outside = smoothstep(RADIUS - 1.4, RADIUS + 1.4, d)
    # Halo: a crimson bloom the disc is cut out of.
    if outside > 0.0:
        falloff = math.exp(-(d - RADIUS) / 78.0)
        core = math.exp(-(d - RADIUS) / 17.0)
        glow = (glow[0] + (62.0 * falloff + 96.0 * core) * outside,
                glow[1] + (13.0 * falloff + 20.0 * core) * outside,
                glow[2] + (17.0 * falloff + 27.0 * core) * outside)
    # The disc is the one thing that hides light instead of adding it.
    inside = 1.0 - outside
    if inside > 0.0:
        shade = 1.0 - 0.35 * smoothstep(RADIUS * 0.55, RADIUS, d)
        colour = (r * (1.0 - inside) + 4.0 * shade * inside,
                  g * (1.0 - inside) + 4.0 * shade * inside,
                  b * (1.0 - inside) + 7.0 * shade * inside)

    # Rim: a thin hot ring, brighter at the top left where the light is.
    ring = math.exp(-((d - RADIUS) / 1.7) ** 2)
    if ring > 0.01:
        angle = math.atan2(y - cy, x - cx)
        bias = 0.55 + 0.45 * math.cos(angle + 2.2)
        glow = (glow[0] + 245.0 * ring * bias,
                glow[1] + 70.0 * ring * bias,
                glow[2] + 82.0 * ring * bias)

    # The tear: white-hot where it is narrow, crimson where it widens out.
    if d < RADIUS + 26.0:
        distance, along = tear_distance(x, y)
        width = 1.1 + 2.0 * math.sin(math.pi * along)
        if distance < width * 5.0:
            core = math.exp(-((distance / (width * 0.75)) ** 2))
            bloom = math.exp(-(distance / (width * 3.2)))
            glow = (glow[0] + (238.0 * core + 96.0 * bloom),
                    glow[1] + (176.0 * core + 24.0 * bloom),
                    glow[2] + (168.0 * core + 32.0 * bloom))
    return colour, glow


def wisps(glow):
    """Smoke pulled off the disc: soft stamps along a few curves."""
    for index, (base_x, drift, bend, phase) in enumerate(
            ((150.0, -46.0, 22.0, 0.0), (232.0, 34.0, -18.0, 1.7),
             (318.0, -30.0, 26.0, 3.1), (204.0, 12.0, 30.0, 4.4))):
        steps = 90
        for step in range(steps):
            t = step / (steps - 1)
            y = CENTRE[1] - RADIUS * 0.55 - t * 190.0
            x = base_x + drift * t + bend * math.sin(t * 2.6 + phase)
            radius = 7.0 + 5.0 * t
            alpha = 0.30 * (1.0 - t) ** 1.6
            if y < 6.0:
                continue
            span = int(radius * 2.0) + 1
            for oy in range(-span, span + 1):
                for ox in range(-span, span + 1):
                    px, py = int(x) + ox, int(y) + oy
                    if 0 <= px < SIZE and 0 <= py < SIZE:
                        weight = math.exp(-((ox * ox + oy * oy) / (radius * radius)) * 2.2) * alpha
                        offset = (py * SIZE + px) * 3
                        glow[offset] += 58.0 * weight
                        glow[offset + 1] += 12.0 * weight
                        glow[offset + 2] += 16.0 * weight


def text_mask(scale=5, tracking=1, top=404):
    """The name as a coverage mask, centred, with its left edge returned."""
    cells = "THE DARKNESS"
    unit = 5 + tracking
    width = len(cells) * unit - tracking
    left = (SIZE - width * scale) // 2
    mask = [0.0] * (SIZE * SIZE)
    for cell, character in enumerate(cells):
        if character == " ":
            continue
        for gy, row in enumerate(GLYPHS[character]):
            for gx, on in enumerate(row):
                if on != "#":
                    continue
                x0 = left + (cell * unit + gx) * scale
                y0 = top + gy * scale
                for yy in range(y0, y0 + scale):
                    row_offset = yy * SIZE
                    for xx in range(x0, x0 + scale):
                        mask[row_offset + xx] = 1.0
    return mask, left, width * scale


def blur(mask, radius=5):
    """Three box passes: cheap, and a smooth flower is all a glow needs."""
    size = SIZE
    pass_radius = max(1, radius // 2)
    current = mask
    for _ in range(3):
        horizontal = [0.0] * (size * size)
        for y in range(size):
            row = y * size
            total = sum(current[row:row + pass_radius + 1])
            for x in range(size):
                if x + pass_radius + 1 < size:
                    total += current[row + x + pass_radius + 1]
                if x - pass_radius > 0:
                    total -= current[row + x - pass_radius]
                horizontal[row + x] = total
        vertical = [0.0] * (size * size)
        for x in range(size):
            total = 0.0
            for y in range(pass_radius + 1):
                total += horizontal[y * size + x]
            for y in range(size):
                if y + pass_radius + 1 < size:
                    total += horizontal[(y + pass_radius + 1) * size + x]
                if y - pass_radius > 0:
                    total -= horizontal[(y - pass_radius) * size + x]
                vertical[y * size + x] = total
        current = [value / (2 * pass_radius + 1) ** 2 for value in vertical]
    return current


def render():
    glow = [0.0] * (SIZE * SIZE * 3)
    wisps(glow)

    colour = [0.0] * (SIZE * SIZE * 3)
    for y in range(SIZE):
        row = y * SIZE
        for x in range(SIZE):
            px_colour = background(x, y)
            index = (row + x) * 3
            px_colour, px_glow = eclipse(px_colour, (glow[index], glow[index + 1], glow[index + 2]), x, y)
            colour[index], colour[index + 1], colour[index + 2] = px_colour
            glow[index], glow[index + 1], glow[index + 2] = px_glow

    mask, left, text_width = text_mask()
    flower = blur(mask, 7)
    for index in range(SIZE * SIZE):
        offset = index * 3
        weight = flower[index]
        if weight > 0.002:
            glow[offset] += 300.0 * weight
            glow[offset + 1] += 70.0 * weight
            glow[offset + 2] += 84.0 * weight

    pixels = bytearray(SIZE * SIZE * 3)
    for index in range(SIZE * SIZE):
        offset = index * 3
        grain = ((index * 2654435761) >> 8) % 5 - 2
        for channel in range(3):
            value = colour[offset + channel] + glow[offset + channel] + grain
            pixels[offset + channel] = 0 if value < 0.0 else (255 if value > 255.0 else int(value))

    # The letters themselves, crisp on top of their own glow.
    for index in range(SIZE * SIZE):
        if mask[index] == 0.0:
            continue
        offset = index * 3
        pixels[offset] = 242
        pixels[offset + 1] = 236
        pixels[offset + 2] = 228

    return pixels


def write_png(path, pixels):
    def chunk(tag, payload):
        body = tag + payload
        return struct.pack(">I", len(payload)) + body + struct.pack(">I", zlib.crc32(body) & 0xFFFFFFFF)

    raw = bytearray()
    stride = SIZE * 3
    for y in range(SIZE):
        raw.append(0)
        raw += pixels[y * stride:(y + 1) * stride]
    header = struct.pack(">IIBBBBB", SIZE, SIZE, 8, 2, 0, 0, 0)
    path.write_bytes(
        b"\x89PNG\r\n\x1a\n"
        + chunk(b"IHDR", header)
        + chunk(b"IDAT", zlib.compress(bytes(raw), 9))
        + chunk(b"IEND", b"")
    )


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--out", default=None, help="sce_sys folder (default app/ps5/sce_sys)")
    args = parser.parse_args()

    out = Path(args.out) if args.out else Path(__file__).resolve().parent.parent / "sce_sys"
    out.mkdir(parents=True, exist_ok=True)
    path = out / "icon0.png"
    write_png(path, render())
    print(f"wrote {path} ({path.stat().st_size:,} bytes, {SIZE}x{SIZE})")


if __name__ == "__main__":
    main()
