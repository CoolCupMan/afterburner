#!/usr/bin/env python3
"""Generate an .ico (16..256 px) procedurally - no dependencies.

usage: gen_icon.py out.ico [accent_hex]   e.g. gen_icon.py build/x.ico 22d3ee
The accent colour starts the gauge gradient and fills the hub, so each app of
the suite gets its own recognisable icon."""
import math, struct, sys, zlib

ACCENT = tuple(int((sys.argv[2] if len(sys.argv) > 2 else '22d3ee')[i:i + 2], 16) for i in (0, 2, 4))

def lerp(a, b, t): return a + (b - a) * t

def pixel(x, y, n):
    # normalized coords, 4x supersampled by caller
    cx = cy = n / 2
    dx, dy = x - cx, y - cy
    r = math.hypot(dx, dy) / (n / 2)
    ang = (math.degrees(math.atan2(dy, dx)) + 360) % 360
    # rounded-square background
    m = 0.14 * n
    qx = max(abs(dx) - (n / 2 - m), 0); qy = max(abs(dy) - (n / 2 - m), 0)
    if math.hypot(qx, qy) > m * 0.98: return (0, 0, 0, 0)
    col = (17, 20, 28, 255)
    # gauge ring 135deg..405deg, gradient cyan->amber->red
    if 0.58 < r < 0.80:
        a = (ang - 135) % 360
        if a <= 270:
            t = a / 270
            stops = [ACCENT, tuple((a + b) // 2 for a, b in zip(ACCENT, (0xfb, 0xbf, 0x24))), (0xfb, 0xbf, 0x24), (0xff, 0x4d, 0x5e)]
            seg = min(int(t * 3), 2); f = t * 3 - seg
            c0, c1 = stops[seg], stops[seg + 1]
            col = tuple(int(lerp(c0[i], c1[i], f)) for i in range(3)) + (255,)
        else:
            col = (40, 46, 60, 255)
    # needle towards ~70%
    na = math.radians(135 + 270 * 0.72)
    ux, uy = math.cos(na), math.sin(na)
    t = dx * ux + dy * uy
    d = abs(-dx * uy + dy * ux)
    if 0 < t < n * 0.36 and d < n * 0.035: col = (232, 235, 242, 255)
    if r < 0.16: col = ACCENT + (255,)
    return col

def render(n):
    ss = 4
    rows = []
    for y in range(n):
        row = []
        for x in range(n):
            acc = [0, 0, 0, 0]
            for sy in range(ss):
                for sx in range(ss):
                    p = pixel(x + (sx + .5) / ss, y + (sy + .5) / ss, n)
                    a = p[3]
                    for i in range(3): acc[i] += p[i] * a
                    acc[3] += a
            a = acc[3] / (ss * ss)
            if acc[3]:
                row.append(tuple(int(acc[i] / acc[3]) for i in range(3)) + (int(a),))
            else:
                row.append((0, 0, 0, 0))
        rows.append(row)
    return rows

def png(n, rows):
    raw = b''.join(b'\0' + bytes(c for px in row for c in px) for row in rows)
    def chunk(t, d): return struct.pack('>I', len(d)) + t + d + struct.pack('>I', zlib.crc32(t + d) & 0xffffffff)
    return b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', n, n, 8, 6, 0, 0, 0)) + \
        chunk(b'IDAT', zlib.compress(raw, 9)) + chunk(b'IEND', b'')

def bmp(n, rows):
    hdr = struct.pack('<IiiHHIIiiII', 40, n, n * 2, 1, 32, 0, 0, 0, 0, 0, 0)
    px = b''.join(bytes((p[2], p[1], p[0], p[3])) for row in reversed(rows) for p in row)
    mask_row = ((n + 31) // 32) * 4
    return hdr + px + b'\0' * mask_row * n

sizes = [16, 24, 32, 48, 64, 128, 256]
images = []
for n in sizes:
    rows = render(n)
    images.append(png(n, rows) if n >= 128 else bmp(n, rows))
out = struct.pack('<HHH', 0, 1, len(sizes))
off = 6 + 16 * len(sizes)
for n, data in zip(sizes, images):
    out += struct.pack('<BBBBHHII', n % 256, n % 256, 0, 0, 1, 32, len(data), off)
    off += len(data)
out += b''.join(images)
open(sys.argv[1] if len(sys.argv) > 1 else 'res/app.ico', 'wb').write(out)
