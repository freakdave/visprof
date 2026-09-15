#!/usr/bin/env python3
# Copyright (c) 2026 David Reichelt. SPDX-License-Identifier: MIT
"""Ask a running Dreamcast program for a screen capture, and make a PNG of it.

The program links libvisprof and calls visprof_capture_poll() once a frame.
That function looks for a request file; this tool writes it. dc-tool serves the
host directory it was started in as /pc on the console, so both sides see the
same `captures` folder: this tool writes `captures/request.txt`, the console
writes `captures/<name>.ppm`, and this tool converts it.

    capture.py --root ~/Dev/FDV/bin/Dreamcast --name shot1 --out shot1.png
    capture.py --ppm captures/shot1.ppm --out shot1.png

Standard library only: PNG is written here with zlib, no Pillow.
"""

import argparse
import os
import struct
import sys
import time
import zlib


def read_ppm(path):
    """Returns (width, height, rgb_bytes) from a binary P6 file."""
    with open(path, "rb") as handle:
        data = handle.read()

    fields = []
    at = 0
    while len(fields) < 4:
        while at < len(data) and data[at:at + 1].isspace():
            at += 1
        if at < len(data) and data[at:at + 1] == b"#":      # a comment line
            while at < len(data) and data[at:at + 1] != b"\n":
                at += 1
            continue
        start = at
        while at < len(data) and not data[at:at + 1].isspace():
            at += 1
        if at == start:
            raise ValueError("%s: truncated PPM header" % path)
        fields.append(data[start:at])
    at += 1                                                  # one separator

    if fields[0] != b"P6":
        raise ValueError("%s: not a binary PPM (%r)" % (path, fields[0]))
    width, height, top = (int(fields[1]), int(fields[2]), int(fields[3]))
    if top != 255:
        raise ValueError("%s: only 8 bits per channel are supported" % path)

    body = data[at:at + width * height * 3]
    if len(body) != width * height * 3:
        raise ValueError("%s: short body, %d of %d bytes"
                         % (path, len(body), width * height * 3))
    return width, height, body


def write_png(path, width, height, body):
    """Writes 8-bit RGB as a PNG. Every row carries filter type 0."""
    raw = bytearray()
    stride = width * 3
    for y in range(height):
        raw.append(0)
        raw += body[y * stride:(y + 1) * stride]

    def chunk(kind, payload):
        out = struct.pack(">I", len(payload)) + kind + payload
        return out + struct.pack(">I", zlib.crc32(kind + payload) & 0xFFFFFFFF)

    header = struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0)
    with open(path, "wb") as handle:
        handle.write(b"\x89PNG\r\n\x1a\n")
        handle.write(chunk(b"IHDR", header))
        handle.write(chunk(b"IDAT", zlib.compress(bytes(raw), 9)))
        handle.write(chunk(b"IEND", b""))


def wait_for_file(path, timeout_s):
    """Waits for the file to exist and stop growing. Returns its size."""
    deadline = time.time() + timeout_s
    last_size = -1
    steady_since = None
    while time.time() < deadline:
        try:
            size = os.path.getsize(path)
        except OSError:
            time.sleep(0.2)
            continue
        if size != last_size:
            last_size = size
            steady_since = time.time()
        elif size > 0 and time.time() - steady_since >= 1.0:
            return size
        time.sleep(0.2)
    return -1


def convert(ppm_path, png_path):
    width, height, body = read_ppm(ppm_path)
    write_png(png_path, width, height, body)
    print("%s %dx%d %d bytes (from %s, %d bytes)"
          % (png_path, width, height, os.path.getsize(png_path),
             ppm_path, os.path.getsize(ppm_path)))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", help="dc-tool host directory, served as /pc")
    parser.add_argument("--name", default="shot",
                        help="base name; letters, digits, - and _, up to 40")
    parser.add_argument("--out", help="PNG to write; default <name>.png")
    parser.add_argument("--timeout", type=float, default=60.0,
                        help="seconds to wait for the picture (default 60)")
    parser.add_argument("--ppm", help="convert this PPM instead of asking")
    args = parser.parse_args()

    if args.ppm:
        convert(args.ppm, args.out or os.path.splitext(args.ppm)[0] + ".png")
        return 0

    if not args.root:
        parser.error("--root is required unless --ppm is given")
    if not args.name or len(args.name) > 40 or not all(
            c.isalnum() or c in "-_" for c in args.name):
        parser.error("--name must be up to 40 letters, digits, - or _")

    folder = os.path.join(args.root, "captures")
    os.makedirs(folder, exist_ok=True)     # dc-load cannot create it itself
    ppm_path = os.path.join(folder, args.name + ".ppm")
    png_path = args.out or (args.name + ".png")

    if os.path.exists(ppm_path):
        os.remove(ppm_path)                # never convert a stale picture
    with open(os.path.join(folder, "request.txt"), "w") as handle:
        handle.write(args.name + "\n")
    print("asked for %s; waiting up to %.0f s" % (ppm_path, args.timeout))

    started = time.time()
    if wait_for_file(ppm_path, args.timeout) < 0:
        print("no picture after %.0f s. Is the program running, and does it "
              "call visprof_capture_poll()?" % args.timeout, file=sys.stderr)
        return 1
    print("picture arrived after %.1f s" % (time.time() - started))
    convert(ppm_path, png_path)
    return 0


if __name__ == "__main__":
    sys.exit(main())
