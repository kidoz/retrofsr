#!/usr/bin/env python3
"""Build a lossless, offline five-way spatial-filter review from P6 frames or synthetic scenes.

Python standard library only. Run --help for capture and reproduction options.
"""

import argparse
import hashlib
import html
import json
import math
import struct
import subprocess
import zlib
from collections.abc import Sequence
from pathlib import Path
from typing import NotRequired, TypedDict

MODES = ("fsr1", "fsr1-noise", "fsr1-coherence", "fsr1-both", "nearest")
SCENES = ("diagonal", "thin-line", "checkerboard", "hud", "noise", "pixel-art")
GLYPH = ("10101110111", "10100100100", "11100100110", "10100100100", "10101110111")


class Metrics(TypedDict):
    baseline_mae: float
    baseline_max: int
    reference_mae: NotRequired[float]
    temporal_error_delta: NotRequired[float]


class Frame(TypedDict):
    source: str
    outputs: list[str]
    differences: list[str]
    input_sha256: str
    original: str | None
    reference: NotRequired[str]


class ReviewSequence(TypedDict):
    name: str
    frames: list[Frame]
    metrics: dict[str, list[Metrics]]
    width: int
    height: int


def positive_int(value: str) -> int:
    result = int(value)
    if result < 1:
        raise argparse.ArgumentTypeError("must be positive")
    return result


def read_ppm(path: Path) -> tuple[int, int, bytes]:
    data = path.read_bytes()
    pos = 0

    def token() -> bytes:
        nonlocal pos
        while pos < len(data):
            if data[pos] in b" \t\r\n":
                pos += 1
            elif data[pos] == ord("#"):
                end = data.find(b"\n", pos)
                pos = len(data) if end < 0 else end + 1
            else:
                break
        start = pos
        while pos < len(data) and data[pos] not in b" \t\r\n#":
            pos += 1
        return data[start:pos]

    if token() != b"P6":
        raise ValueError(f"{path}: expected binary P6 PPM")
    width, height, maximum = (int(token()) for _ in range(3))
    if width < 1 or height < 1 or maximum != 255:
        raise ValueError(f"{path}: expected positive dimensions and 8-bit RGB")
    if pos >= len(data) or data[pos] not in b" \t\r\n":
        raise ValueError(f"{path}: missing raster separator")
    # The delimiter is one whitespace byte, or the common CRLF pair. Do not strip
    # arbitrary whitespace: the first raster bytes can themselves be whitespace.
    pos += 2 if data[pos : pos + 2] == b"\r\n" else 1
    raster = data[pos:]
    if len(raster) != width * height * 3:
        raise ValueError(f"{path}: incorrect raster length")
    return width, height, raster


def write_ppm(path: Path, width: int, height: int, raster: bytes) -> None:
    path.write_bytes(f"P6\n{width} {height}\n255\n".encode() + raster)


def write_png(path: Path, width: int, height: int, raster: bytes) -> None:
    """Lossless RGB8 PNG; no palette reduction, chroma subsampling or image filtering."""

    def chunk(kind: bytes, payload: bytes) -> bytes:
        return (
            struct.pack(">I", len(payload)) + kind + payload + struct.pack(">I", zlib.crc32(kind + payload))
        )

    rows = b"".join(b"\0" + raster[y * width * 3 : (y + 1) * width * 3] for y in range(height))
    path.write_bytes(
        b"\x89PNG\r\n\x1a\n"
        + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0))
        + chunk(b"IDAT", zlib.compress(rows))
        + chunk(b"IEND", b"")
    )


def write_animation(path: Path, width: int, height: int, frames: list[bytes], fps: float) -> None:
    """Lossless APNG with one shared clock for all five panels."""

    def chunk(kind: bytes, payload: bytes) -> bytes:
        return (
            struct.pack(">I", len(payload)) + kind + payload + struct.pack(">I", zlib.crc32(kind + payload))
        )

    data = bytearray(b"\x89PNG\r\n\x1a\n")
    data += chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0))
    data += chunk(b"acTL", struct.pack(">II", len(frames), 0))
    sequence = 0
    for index, raster in enumerate(frames):
        data += chunk(
            b"fcTL", struct.pack(">IIIIIHHBB", sequence, width, height, 0, 0, 1000, round(fps * 1000), 0, 0)
        )
        sequence += 1
        rows = b"".join(b"\0" + raster[y * width * 3 : (y + 1) * width * 3] for y in range(height))
        compressed = zlib.compress(rows)
        if index == 0:
            data += chunk(b"IDAT", compressed)
        else:
            data += chunk(b"fdAT", struct.pack(">I", sequence) + compressed)
            sequence += 1
    data += chunk(b"IEND", b"")
    path.write_bytes(data)


def contact_sheet(frames: list[bytes], width: int, height: int) -> bytes:
    result = bytearray()
    padded = [*frames]
    if len(padded) % 2:
        padded.append(bytes(width * height * 3))
    for left in range(0, len(padded), 2):
        right = left + 1
        for y in range(height):
            start, end = y * width * 3, (y + 1) * width * 3
            result += padded[left][start:end] + padded[right][start:end]
    return bytes(result)


def write_static_report(output: Path, reports: list[ReviewSequence], kind: str, scale: float) -> None:
    sections = []
    for index, report in enumerate(reports):
        folder = f"sequence-{index}"
        frame = min(3, len(report["frames"]) - 1)
        sections.append(
            f"<section><h2>{html.escape(report['name'])}</h2>"
            "<p>Top: baseline / noise suppression. Middle: edge coherence / both. Bottom: nearest / blank.</p>"
            f'<img src="{folder}/preview.png" alt="Five-way still comparison, frame {frame + 1}">'
            f'<p><a href="{folder}/motion.png">Open synchronized lossless animation</a> · '
            f'<a href="{folder}/{frame:04d}-reference.png">Analytic reference</a></p></section>'
            if kind == "synthetic"
            else f"<section><h2>{html.escape(report['name'])}</h2>"
            "<p>Top: baseline / noise suppression. Middle: edge coherence / both. Bottom: nearest / blank.</p>"
            f'<img src="{folder}/preview.png" alt="Five-way still comparison">'
            f'<p><a href="{folder}/motion.png">Open synchronized lossless animation</a></p></section>'
        )
    output.joinpath("static.html").write_text(
        '<!doctype html><html lang="en"><meta charset="utf-8"><title>retrofsr visual contact sheets</title>'
        "<style>body{font:16px system-ui;background:#131820;color:#e7edf4;margin:24px}a{color:#9bd3ff}"
        "img{image-rendering:pixelated;width:768px;max-width:100%}section{margin:40px 0}</style>"
        f"<h1>retrofsr · {html.escape(kind)} comparisons at {scale}×</h1>"
        "<p>Lossless stills and synchronized APNG playback. No JavaScript required.</p>"
        "<p>Nearest is an unsharpened pixel-art reference. Neither experimental option is promoted.</p>"
        '<a href="index.html">Interactive frame and difference inspector</a>' + "".join(sections) + "</html>"
    )


def scene_color(scene: str, x: float, y: float, phase: float) -> tuple[int, int, int]:
    x -= phase
    if scene == "diagonal":
        bright = x > y + 8
    elif scene == "thin-line":
        bright = abs(x - 0.7 * y - 10) < 0.65
    elif scene == "checkerboard":
        bright = (math.floor(x / 2) + math.floor(y / 2)) % 2 == 0
    elif scene == "hud":
        # Pixel-aligned glyph strokes and a border; scrolling tests subpixel phases.
        gx, gy = math.floor((x - 8) / 2), math.floor((y - 14) / 2)
        bright = 0 <= gy < len(GLYPH) and 0 <= gx < len(GLYPH[0]) and GLYPH[gy][gx] == "1"
        bright |= 4 < x < 58 and (abs(y - 7) < 0.5 or abs(y - 38) < 0.5)
    elif scene == "noise":
        # The reference is the clean scene. Deterministic grain is added only to LR.
        bright = x > 32
        return (144, 136, 128) if bright else (112, 104, 96)
    else:
        # Small colored sprite and hard stair-step silhouettes, with intentional dither.
        ix, iy = math.floor(x), math.floor(y)
        if 18 <= ix < 42 and 12 <= iy < 36:
            if (ix < 24 or ix >= 36) and iy < 18:
                return (24, 32, 56)
            return (224, 104, 40) if (ix + iy) % 2 else (248, 184, 72)
        return (32, 64, 96) if iy < 38 else (48, 112, 64)
    return (224, 216, 192) if bright else (32, 40, 56)


def synthetic_frame(
    scene: str, width: int, height: int, phase: float, pixel_size: float = 1.0, noisy: bool = False
) -> bytes:
    raster = bytearray()
    for y in range(height):
        for x in range(width):
            total = [0, 0, 0]
            # Independent 4x4 area integration at LR and HR; an approximate analytic
            # reference, not a reconstruction by any filter under test.
            for j in range(4):
                for i in range(4):
                    color = scene_color(
                        scene, (x + (i + 0.5) / 4) * pixel_size, (y + (j + 0.5) / 4) * pixel_size, phase
                    )
                    for ch in range(3):
                        total[ch] += color[ch]
            noise = 0
            if scene == "noise" and noisy:
                seed = (x * 73856093 ^ y * 19349663 ^ round(phase * 4) * 83492791) & 0xFFFFFFFF
                noise = (seed * 1664525 + 1013904223) % 17 - 8
            raster.extend(max(0, min(255, round(v / 16) + noise)) for v in total)
    return bytes(raster)


def mean_abs(a: Sequence[int], b: Sequence[int]) -> float:
    return sum(abs(x - y) for x, y in zip(a, b, strict=True)) / len(a)


def build_report(args: argparse.Namespace) -> None:
    tool = args.tool.resolve()
    if not tool.is_file():
        raise ValueError(f"missing scaler executable: {tool}; build scale_ppm first")
    if not math.isfinite(args.scale) or args.scale < 1:
        raise ValueError("scale must be finite and at least 1")
    if not math.isfinite(args.fps) or not 1 <= args.fps <= 60:
        raise ValueError("fps must be in [1, 60]")
    sequences: list[tuple[str, list[Path] | None]] = []
    if args.input:
        input_paths: list[Path] = sorted(args.input.glob("*.ppm"))[: args.frames]
        if not input_paths:
            raise ValueError("input folder contains no .ppm frames (use zero-padded names)")
        sequences = [(args.input.name, input_paths)]
    else:
        sequences = [(name, None) for name in SCENES]
    # Refuse to overwrite a previous report or an input directory.
    args.output.mkdir(parents=True, exist_ok=False)
    reports: list[ReviewSequence] = []
    manifest: dict[str, object] = {
        "kind": "synthetic" if args.synthetic else "capture",
        "scale": args.scale,
        "fps": args.fps,
        "modes": MODES,
        "color_space": "perceptual",
        "sharpening": 0.20,
        "mode_options": {
            mode: {
                "color_space": "srgb" if mode == "nearest" else "perceptual",
                "sharpening": 0.0 if mode == "nearest" else 0.20,
            }
            for mode in MODES
        },
        "scaler_sha256": hashlib.sha256(tool.read_bytes()).hexdigest(),
        "sequences": reports,
    }
    root = Path(__file__).resolve().parent.parent
    manifest["implementation_sha256"] = {
        name: hashlib.sha256((root / name).read_bytes()).hexdigest()
        for name in ("src/retrofsr.cpp", "include/retrofsr/retrofsr.hpp")
    }
    for seq_index, (name, paths) in enumerate(sequences):
        folder = args.output / f"sequence-{seq_index}"
        folder.mkdir()
        sequence: ReviewSequence = {
            "name": name,
            "frames": [],
            "metrics": {mode: [] for mode in MODES},
            "width": 0,
            "height": 0,
        }
        contacts: list[bytes] = []
        previous_errors: dict[str, list[int]] = {}
        previous_size = None
        count = len(paths) if paths is not None else args.frames
        for frame in range(count):
            reference = None
            if paths is None:
                sw, sh = 64, 48
                src = synthetic_frame(name, sw, sh, frame * 0.25, noisy=True)
            else:
                sw, sh, src = read_ppm(paths[frame])
            if previous_size and previous_size != (sw, sh):
                raise ValueError("all frames in a sequence must have identical dimensions")
            previous_size = (sw, sh)
            dw, dh = round(sw * args.scale), round(sh * args.scale)
            if paths is None:
                reference = synthetic_frame(name, dw, dh, frame * 0.25, pixel_size=1 / args.scale)
            prefix = folder / f"{frame:04d}"
            source_ppm = prefix.with_suffix(".ppm")
            write_ppm(source_ppm, sw, sh, src)
            write_png(prefix.with_suffix(".png"), sw, sh, src)
            entry: Frame = {
                "source": f"sequence-{seq_index}/{frame:04d}.png",
                "outputs": [],
                "differences": [],
                "input_sha256": hashlib.sha256(src).hexdigest(),
                "original": str(paths[frame]) if paths else None,
            }
            if reference is not None:
                ref_name = f"{frame:04d}-reference.png"
                write_png(folder / ref_name, dw, dh, reference)
                entry["reference"] = f"sequence-{seq_index}/{ref_name}"
            baseline = None
            rendered: list[bytes] = []
            for mode in MODES:
                result_ppm = folder / f"{frame:04d}-{mode}.ppm"
                subprocess.run(
                    [str(tool), str(source_ppm), str(result_ppm), str(dw), str(dh), mode],
                    check=True,
                    capture_output=True,
                    timeout=120,
                )
                width, height, result = read_ppm(result_ppm)
                if (width, height) != (dw, dh):
                    raise ValueError("scaler returned incorrect output dimensions")
                if baseline is None:
                    baseline = result
                rendered.append(result)
                png = result_ppm.with_suffix(".png")
                write_png(png, dw, dh, result)
                diff_name = f"{frame:04d}-{mode}-diff.png"
                write_png(
                    folder / diff_name,
                    dw,
                    dh,
                    bytes(min(255, abs(a - b) * 8) for a, b in zip(result, baseline, strict=True)),
                )
                entry["outputs"].append(f"sequence-{seq_index}/{png.name}")
                entry["differences"].append(f"sequence-{seq_index}/{diff_name}")
                metric: Metrics = {
                    "baseline_mae": mean_abs(result, baseline),
                    "baseline_max": max(abs(a - b) for a, b in zip(result, baseline, strict=True)),
                }
                if reference is not None:
                    errors = [a - b for a, b in zip(result, reference, strict=True)]
                    metric["reference_mae"] = mean_abs(result, reference)
                    if mode in previous_errors:
                        metric["temporal_error_delta"] = mean_abs(errors, previous_errors[mode])
                    previous_errors[mode] = errors
                sequence["metrics"][mode].append(metric)
                result_ppm.unlink()  # Exact RGB pixels remain in lossless PNGs.
            contacts.append(contact_sheet(rendered, dw, dh))
            sequence["frames"].append(entry)
            sequence["width"], sequence["height"] = dw, dh
        write_png(
            folder / "preview.png",
            sequence["width"] * 2,
            sequence["height"] * ((len(MODES) + 1) // 2),
            contacts[min(3, len(contacts) - 1)],
        )
        write_animation(
            folder / "motion.png",
            sequence["width"] * 2,
            sequence["height"] * ((len(MODES) + 1) // 2),
            contacts,
            args.fps,
        )
        reports.append(sequence)
    (args.output / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    write_static_report(args.output, reports, "synthetic" if args.synthetic else "capture", args.scale)
    template = Path(__file__).with_suffix(".html").read_text()
    # Embedded JSON supports file:// use without a server or fetch; neutralize HTML delimiters.
    embedded = json.dumps(manifest).replace("<", "\\u003c").replace(">", "\\u003e").replace("&", "\\u0026")
    (args.output / "index.html").write_text(template.replace("__MANIFEST__", embedded))
    print(args.output.resolve() / "index.html")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    source = parser.add_mutually_exclusive_group(required=True)
    source.add_argument("--synthetic", action="store_true", help="generate labeled analytic motion scenes")
    source.add_argument(
        "--input", type=Path, help="folder of equally-sized opaque P6 .ppm frames, sorted by name"
    )
    parser.add_argument("--tool", type=Path, default=Path("buildDir/scale_ppm"))
    parser.add_argument(
        "--output", type=Path, required=True, help="new report directory; existing paths are rejected"
    )
    parser.add_argument("--scale", type=float, default=2.0)
    parser.add_argument(
        "--frames", type=positive_int, default=16, help="maximum input frames, or synthetic sequence length"
    )
    parser.add_argument(
        "--fps",
        type=float,
        default=8.0,
        help="review playback rate (set to the capture rate for real-time playback)",
    )
    args = parser.parse_args()
    try:
        build_report(args)
    except (OSError, ValueError, subprocess.SubprocessError) as error:
        parser.exit(1, f"visual_compare: {error}\n")


if __name__ == "__main__":
    main()
