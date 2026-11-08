# SPDX-License-Identifier: Apache-2.0 OR MIT
# Copyright (c) 2026 Go Kudo <zeriyoshi@gmail.com>
"""SDR stills and their HDR truth for the light predictor (needs numpy).

    python3 -I scripts/predictor/make_data.py HDRI_DIR OUT

Each equirectangular HDRI (fetch.py) gives 4 crops of its horizon band (384 x 256, a quarter turn
apart). A crop is exposed so its median luminance is 0.18 x 2^j (j = -1, 0, 1, 0 by the crop),
rolled off above 0.8 like a camera (the shoulder reaching 1) and written as an 8-bit sRGB PNG;
its exposed linear luminance is kept beside it (.npy) for the labels (train.py).
"""

import os
import struct
import sys
import zlib

import numpy as np

CROP_W = 384
CROP_H = 256
EXPOSURES = (-1.0, 0.0, 1.0, 0.0)


def read_hdr(path):
    """A Radiance RGBE file (new run-length scanlines) as float32 RGB."""
    data = open(path, "rb").read()
    end = data.index(b"\n\n") + 2
    line_end = data.index(b"\n", end)
    parts = data[end:line_end].split()
    height, width = int(parts[1]), int(parts[3])
    pos = line_end + 1
    rgbe = np.zeros((height, width, 4), dtype=np.uint8)
    for y in range(height):
        if data[pos] != 2 or data[pos + 1] != 2:
            flat = np.frombuffer(data, dtype=np.uint8, count=width * 4, offset=pos)
            rgbe[y] = flat.reshape(width, 4)
            pos += width * 4
            continue
        pos += 4
        for c in range(4):
            x = 0
            row = rgbe[y, :, c]
            while x < width:
                n = data[pos]
                pos += 1
                if n > 128:
                    n -= 128
                    row[x:x + n] = data[pos]
                    pos += 1
                else:
                    row[x:x + n] = np.frombuffer(data, dtype=np.uint8, count=n, offset=pos)
                    pos += n
                x += n
    e = rgbe[:, :, 3].astype(np.int32)
    scale = np.where(e > 0, np.ldexp(1.0, e - 136), 0.0).astype(np.float32)
    return rgbe[:, :, :3].astype(np.float32) * scale[:, :, None]


def write_png(path, rgb):
    height, width, _ = rgb.shape
    raw = b"".join(b"\x00" + rgb[y].tobytes() for y in range(height))

    def chunk(kind, body):
        return struct.pack(">I", len(body)) + kind + body + struct.pack(">I", zlib.crc32(kind + body))

    png = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b"")
    open(path, "wb").write(png)


def shoulder(x):
    knee = 0.8
    return np.where(x <= knee, x, knee + (1.0 - knee) * (1.0 - np.exp(-(x - knee) / (1.0 - knee))))


def srgb(x):
    x = np.clip(x, 0.0, 1.0)
    return np.where(x <= 0.0031308, x * 12.92, 1.055 * np.power(x, 1.0 / 2.4) - 0.055)


def main(argv):
    if len(argv) < 3:
        print(__doc__, file=sys.stderr)
        return 2
    source, out = argv[1], argv[2]
    os.makedirs(out, exist_ok=True)
    count = 0
    for name in sorted(f for f in os.listdir(source) if f.endswith(".hdr")):
        hdr = read_hdr(os.path.join(source, name))
        height, width, _ = hdr.shape
        top = (height - CROP_H) // 2
        for k, exposure in enumerate(EXPOSURES):
            left = (k * width) // 4
            columns = [(left + x) % width for x in range(CROP_W)]
            crop = hdr[top:top + CROP_H][:, columns]
            luma = 0.2126 * crop[:, :, 0] + 0.7152 * crop[:, :, 1] + 0.0722 * crop[:, :, 2]
            median = float(np.median(luma))
            if median <= 0.0:
                continue
            gain = 0.18 * 2.0 ** exposure / median
            linear = crop * gain
            sdr = np.round(srgb(shoulder(linear)) * 255.0).astype(np.uint8)
            stem = f"{name[:-4]}_{k}"
            write_png(os.path.join(out, stem + ".png"), sdr)
            np.save(os.path.join(out, stem + ".npy"), (luma * gain).astype(np.float32))
            count += 1
    print(f"{count} SDR stills in {out}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
