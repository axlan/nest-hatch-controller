"""Convert a GIF (or a still GIF/PNG) into the nest-picker night-mode animation.

    uv run --project python gif2anim IN.gif OUT.anim [--preview PREVIEW.png]   (from the repo root)

Every frame is cropped to fill the 320x320 screen (scaled to cover, centered),
reduced to its own 256-color palette and stored uncompressed, so the Nest can
memory-map the file and draw frames with a palette lookup.

File format (little-endian):
  header (16 B): "NANM" | u16 version=1 | u16 width=320 | u16 height=320 | u16 frames | u32 reserved
  frame  (103,172 B each): u16 delay_ms | u16 reserved | 256*3 RGB palette | 320*320 indices

GIF tips: square and at least 320x320, subject inside the central circle (the
round lens hides the corners), dark background, a seamless loop of at most 80 frames.
"""

from __future__ import annotations

import argparse
import struct
import sys

from PIL import Image, ImageDraw, ImageOps, ImageSequence, UnidentifiedImageError

SIZE = 320
MAX_FRAMES = 80
MAX_BYTES = 8 * 1024 * 1024
HEADER = struct.Struct("<4sHHHHI")
FRAME_BYTES = 4 + 256 * 3 + SIZE * SIZE


def fail(msg: str) -> None:
    print(f"gif2anim: {msg}", file=sys.stderr)
    sys.exit(1)


def frame_delay(info: dict) -> int:
    d = int(info.get("duration") or 0)
    if d <= 0:
        d = 100  # browsers treat 0 as ~100 ms
    return max(20, min(d, 10000))


def load_frames(path: str) -> list[tuple[Image.Image, int]]:
    try:
        img = Image.open(path)
    except (UnidentifiedImageError, OSError) as e:
        fail(f"{path} is not a readable image ({e})")
    n = getattr(img, "n_frames", 1)
    if n > MAX_FRAMES:
        fail(f"{path} has {n} frames; the limit is {MAX_FRAMES}. Trim it or drop every other frame.")
    frames = []
    for f in ImageSequence.Iterator(img):
        # convert() composites the frame with GIF disposal applied (full picture).
        rgb = f.convert("RGB")
        fitted = ImageOps.fit(rgb, (SIZE, SIZE), Image.LANCZOS, centering=(0.5, 0.5))
        frames.append((fitted, frame_delay(f.info)))
    if not frames:
        fail(f"{path} has no frames")
    return frames


def encode(frames: list[tuple[Image.Image, int]]) -> tuple[bytes, list[Image.Image]]:
    out = bytearray(HEADER.pack(b"NANM", 1, SIZE, SIZE, len(frames), 0))
    shown = []
    for img, delay in frames:
        q = img.quantize(256, dither=Image.Dither.FLOYDSTEINBERG)
        pal = (q.getpalette() or [])[: 256 * 3]
        pal += [0] * (256 * 3 - len(pal))
        out += struct.pack("<HH", delay, 0)
        out += bytes(pal)
        out += q.tobytes()
        shown.append(q.convert("RGB"))
    return bytes(out), shown


def write_preview(path: str, frames: list[Image.Image]) -> None:
    frames = frames[:16]
    cols = min(4, len(frames))
    rows = (len(frames) + cols - 1) // cols
    sheet = Image.new("RGB", (cols * SIZE, rows * SIZE), "black")
    mask = Image.new("L", (SIZE, SIZE), 0)
    ImageDraw.Draw(mask).ellipse((0, 0, SIZE - 1, SIZE - 1), fill=255)  # the round screen
    for i, f in enumerate(frames):
        sheet.paste(f, ((i % cols) * SIZE, (i // cols) * SIZE), mask)
    sheet.save(path)


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("input")
    ap.add_argument("output")
    ap.add_argument("--preview", help="write a PNG contact sheet (round-masked) of up to 16 frames")
    args = ap.parse_args()

    frames = load_frames(args.input)
    data, shown = encode(frames)
    if len(data) > MAX_BYTES:
        fail(f"output would be {len(data) / 1e6:.1f} MB; the limit is {MAX_BYTES / 1e6:.1f} MB. Use fewer frames.")
    assert len(data) == HEADER.size + len(frames) * FRAME_BYTES
    with open(args.output, "wb") as f:
        f.write(data)
    if args.preview:
        write_preview(args.preview, shown)

    loop_ms = sum(d for _, d in frames)
    print(f"gif2anim: {len(frames)} frame(s), {loop_ms / 1000:.2f} s loop, "
          f"{len(data) / 1e6:.2f} MB -> {args.output}")


if __name__ == "__main__":
    main()
