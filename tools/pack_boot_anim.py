#!/usr/bin/env python3
"""
Pack an input video into G'lam OS Motion Video (.gmv) format for 320x240 LCD playback.

Each frame is encoded as Baseline DCT JPEG (SOF0, 4:2:0, non-progressive, standard Huffman)
for maximum compatibility and speed with ESP32-S3 ROM tjpgd.
"""

from __future__ import annotations

import argparse
import glob
import io
import os
import shutil
import struct
import subprocess
import sys
import zlib
from pathlib import Path
from PIL import Image, ImageChops


GMV_MAGIC = b"GMV1"
GMV_VERSION = 1
HEADER_STRUCT_PREFIX = "<4sHHHHIIII"  # 28 bytes before header_crc32
HEADER_STRUCT_FULL = "<4sHHHHIIIII32s"  # 64 bytes total


def find_ffmpeg(explicit_path: str | None) -> str:
    """Locate ffmpeg executable via CLI override, PATH, or known local installs."""
    if explicit_path:
        if os.path.isfile(explicit_path):
            return explicit_path
        raise FileNotFoundError(f"Specified ffmpeg not found: {explicit_path}")

    found = shutil.which("ffmpeg")
    if found:
        return found

    candidates = [
        r"C:\Users\Mark\AppData\Roaming\bilibili\ffmpeg\ffmpeg.exe",
    ]
    candidates.extend(sorted(glob.glob(r"C:\Users\Mark\AppData\Local\JianyingPro\Apps\*\ffmpeg.exe"), reverse=True))
    for path in candidates:
        if os.path.isfile(path):
            return path

    raise FileNotFoundError("Could not locate ffmpeg.exe. Pass --ffmpeg <path>.")


def validate_baseline_jpeg(data: bytes, frame_idx: int) -> None:
    """Verify encoded bytes represent a valid Baseline SOF0 (non-progressive) JPEG."""
    if len(data) < 4 or data[:2] != b"\xff\xd8" or data[-2:] != b"\xff\xd9":
        raise ValueError(f"Frame {frame_idx}: invalid JPEG SOI/EOI markers")

    pos = 2
    has_sof0 = False
    while pos + 2 <= len(data):
        if data[pos] != 0xFF:
            raise ValueError(f"Frame {frame_idx}: malformed JPEG marker stream at {pos}")
        while pos < len(data) and data[pos] == 0xFF:
            pos += 1
        if pos >= len(data):
            break
        marker = data[pos]
        pos += 1
        if marker == 0xD9:  # EOI
            break
        if marker == 0x00 or (0xD0 <= marker <= 0xD7) or marker == 0x01:
            continue
        if marker == 0xDA:  # SOS - compressed scan starts
            break
        if pos + 2 > len(data):
            raise ValueError(f"Frame {frame_idx}: truncated segment at {pos}")
        seg_len = struct.unpack(">H", data[pos : pos + 2])[0]
        if seg_len < 2 or pos + seg_len > len(data):
            raise ValueError(f"Frame {frame_idx}: invalid segment length {seg_len}")
        if marker == 0xC0:
            has_sof0 = True
        elif marker == 0xC2:
            raise ValueError(f"Frame {frame_idx}: progressive JPEG (SOF2) is unsupported by ROM tjpgd")
        pos += seg_len

    if not has_sof0:
        raise ValueError(f"Frame {frame_idx}: missing SOF0 baseline marker")


def decode_video_frames(
    ffmpeg_bin: str,
    input_video: str,
    width: int,
    height: int,
    fps: int,
    fit: str,
) -> bytes:
    """Invoke ffmpeg to decode, resample, and scale/crop/pad frames to raw RGB24."""
    if fit == "crop":
        vf = f"fps={fps},scale={width}:{height}:force_original_aspect_ratio=increase:flags=lanczos,crop={width}:{height}"
    else:
        vf = f"fps={fps},scale={width}:{height}:force_original_aspect_ratio=decrease:flags=lanczos,pad={width}:{height}:(ow-iw)/2:(oh-ih)/2:black"

    cmd = [
        ffmpeg_bin,
        "-hide_banner",
        "-loglevel",
        "error",
        "-i",
        input_video,
        "-vf",
        vf,
        "-f",
        "rawvideo",
        "-pix_fmt",
        "rgb24",
        "-",
    ]
    proc = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=False)
    if proc.returncode != 0:
        err = proc.stderr.decode("utf-8", errors="replace").strip()
        raise RuntimeError(f"ffmpeg failed (exit {proc.returncode}): {err}")
    return proc.stdout


def build_black_weight_lut(black_cut: int, black_knee: int) -> list[int] | None:
    """Build 256-entry smoothstep weight LUT (0..255) to crush dark background haze."""
    if black_cut <= 0 or black_knee <= black_cut:
        return None
    span = float(black_knee - black_cut)
    lut: list[int] = []
    for m in range(256):
        if m <= black_cut:
            lut.append(0)
        elif m >= black_knee:
            lut.append(255)
        else:
            t = (m - black_cut) / span
            lut.append(int(round((t * t * (3.0 - 2.0 * t)) * 255.0)))
    return lut


def preclean_dark_background(im: Image.Image, weight_lut: list[int] | None) -> Image.Image:
    """Suppress dark teal haze/compression noise to pure black via smoothstep gain."""
    if weight_lut is None:
        return im
    r, g, b = im.split()
    mx = ImageChops.lighter(ImageChops.lighter(r, g), b)
    mask = mx.point(weight_lut)
    return ImageChops.multiply(im, Image.merge("RGB", (mask, mask, mask)))


def pack_gmv(
    input_video: str,
    output_path: str,
    width: int,
    height: int,
    fps: int,
    quality: int,
    fit: str,
    max_bytes: int,
    ffmpeg_bin: str,
    black_cut: int = 18,
    black_knee: int = 44,
) -> None:
    """Convert input video into .gmv binary container and write to output_path."""
    raw = decode_video_frames(ffmpeg_bin, input_video, width, height, fps, fit)
    frame_bytes = width * height * 3
    if len(raw) == 0 or len(raw) % frame_bytes != 0:
        raise ValueError(f"Unexpected raw RGB24 byte length {len(raw)} for {width}x{height}")

    frame_count = len(raw) // frame_bytes
    if frame_count == 0:
        raise ValueError("No video frames decoded")

    weight_lut = build_black_weight_lut(black_cut, black_knee)
    payload_parts: list[bytes] = []
    frame_sizes: list[int] = []

    for idx in range(frame_count):
        chunk = raw[idx * frame_bytes : (idx + 1) * frame_bytes]
        im = Image.frombytes("RGB", (width, height), chunk)
        im = preclean_dark_background(im, weight_lut)
        buf = io.BytesIO()
        im.save(
            buf,
            format="JPEG",
            quality=quality,
            subsampling=2,
            progressive=False,
            optimize=False,
        )
        jpg = buf.getvalue()
        validate_baseline_jpeg(jpg, idx)
        frame_sizes.append(len(jpg))
        payload_parts.append(struct.pack("<I", len(jpg)))
        payload_parts.append(jpg)

    payload = b"".join(payload_parts)
    payload_size = len(payload)
    max_frame_bytes = max(frame_sizes)
    payload_crc32 = zlib.crc32(payload) & 0xFFFFFFFF

    header_prefix = struct.pack(
        HEADER_STRUCT_PREFIX,
        GMV_MAGIC,
        GMV_VERSION,
        width,
        height,
        fps,
        frame_count,
        max_frame_bytes,
        payload_size,
        payload_crc32,
    )
    header_crc32 = zlib.crc32(header_prefix) & 0xFFFFFFFF
    header = struct.pack(
        HEADER_STRUCT_FULL,
        GMV_MAGIC,
        GMV_VERSION,
        width,
        height,
        fps,
        frame_count,
        max_frame_bytes,
        payload_size,
        payload_crc32,
        header_crc32,
        b"\x00" * 32,
    )

    total_size = len(header) + payload_size
    if total_size > max_bytes:
        raise ValueError(f"Output size {total_size} bytes exceeds max limit {max_bytes} bytes")

    out_file = Path(output_path)
    out_file.parent.mkdir(parents=True, exist_ok=True)
    out_file.write_bytes(header + payload)

    avg_kb = (sum(frame_sizes) / len(frame_sizes)) / 1024.0
    max_kb = max_frame_bytes / 1024.0
    duration_s = frame_count / float(fps)
    print(f"Packed GMV1 successfully: {out_file}")
    print(f"  Resolution  : {width}x{height} ({fit})")
    print(f"  Frames/FPS  : {frame_count} frames @ {fps} fps ({duration_s:.2f} s)")
    print(f"  Total Size  : {total_size} bytes ({total_size / 1024.0:.1f} KB)")
    print(f"  Frame Size  : avg {avg_kb:.2f} KB, max {max_kb:.2f} KB")
    print(f"  Payload CRC : 0x{payload_crc32:08X}")
    print(f"  Header CRC  : 0x{header_crc32:08X}")


def main() -> int:
    default_out = str(Path(__file__).resolve().parent.parent / "os_firmware" / "assets" / "boot_anim.gmv")
    parser = argparse.ArgumentParser(description="Convert video to G'lam OS boot_anim.gmv")
    parser.add_argument("input_video", help="Path to source video file")
    parser.add_argument("-o", "--output", default=default_out, help="Output .gmv path")
    parser.add_argument("--width", type=int, default=320)
    parser.add_argument("--height", type=int, default=240)
    parser.add_argument("--fps", type=int, default=15)
    parser.add_argument("--quality", type=int, default=88)
    parser.add_argument("--fit", choices=["crop", "pad"], default="crop")
    parser.add_argument("--black-cut", type=int, default=18, help="Max-channel floor crushed to 0 (0=disable)")
    parser.add_argument("--black-knee", type=int, default=44, help="Upper smoothstep knee for black preclean")
    parser.add_argument("--max-bytes", type=int, default=7 * 1024 * 1024)
    parser.add_argument("--ffmpeg", default=None, help="Explicit ffmpeg.exe path")
    args = parser.parse_args()

    if not os.path.isfile(args.input_video):
        print(f"Error: input video not found: {args.input_video}", file=sys.stderr)
        return 1

    ffmpeg_bin = find_ffmpeg(args.ffmpeg)
    pack_gmv(
        input_video=args.input_video,
        output_path=args.output,
        width=args.width,
        height=args.height,
        fps=args.fps,
        quality=args.quality,
        fit=args.fit,
        max_bytes=args.max_bytes,
        ffmpeg_bin=ffmpeg_bin,
        black_cut=args.black_cut,
        black_knee=args.black_knee,
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
