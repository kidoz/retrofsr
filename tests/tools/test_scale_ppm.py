#!/usr/bin/env python3
"""Meson test for the scale_ppm example CLI.

Usage: test_scale_ppm.py <path-to-scale_ppm>

Checks every documented mode end-to-end (upscale and downscale), output header
and payload size, and the error contract: exit 2 for wrong usage, exit 1 for
invalid inputs.
"""

import subprocess
import sys
import tempfile
from pathlib import Path

MODES = [
    "retro",
    "fsr",
    "fsr1",
    "fsr1-noise",
    "fsr1-coherence",
    "fsr1-both",
    "fsr1-quality",
    "fsr1-balanced",
    "fsr1-performance",
    "fsr1-ultra",
    "nearest",
    "bilinear",
    "catmull",
    "lanczos",
    "easu",
]

FAILURES: list[str] = []


def check(condition: bool, message: str) -> None:
    if not condition:
        FAILURES.append(message)
        print(f"FAIL: {message}")
    else:
        print(f"ok: {message}")


def write_ppm(path: Path, width: int, height: int, magic: bytes = b"P6") -> None:
    data = bytearray()
    for y in range(height):
        for x in range(width):
            data += bytes(((x * 31) % 256, (y * 53) % 256, ((x ^ y) & 1) * 255))
    path.write_bytes(magic + b"\n%d %d\n255\n" % (width, height) + bytes(data))


def read_header(path: Path) -> tuple[bytes, bytes, bytes, bytes]:
    payload = path.read_bytes()
    magic, size, maxval, rest = payload.split(b"\n", 3)
    return magic, size, maxval, rest


def run(tool: str, *args: str | Path | int | float) -> subprocess.CompletedProcess[str]:
    return subprocess.run([tool, *map(str, args)], capture_output=True, text=True, timeout=120)


def main() -> int:
    tool = sys.argv[1]
    with tempfile.TemporaryDirectory() as tmp_dir:
        tmp = Path(tmp_dir)
        src = tmp / "in.ppm"
        write_ppm(src, 16, 12)

        # Every mode upscales to the requested size with a correct P6 header.
        payloads = {}
        for mode in MODES:
            out = tmp / f"out-{mode}.ppm"
            r = run(tool, src, out, 40, 30, mode)
            check(r.returncode == 0, f"mode {mode} exits 0")
            if r.returncode == 0:
                magic, size, maxval, rest = read_header(out)
                check(
                    magic == b"P6" and size == b"40 30" and maxval == b"255" and len(rest) == 40 * 30 * 3,
                    f"mode {mode} writes a well-formed 40x30 P6",
                )
                payloads[mode] = rest

        # Content sanity: materially different filters must not collapse to identical
        # output (a regression falling back to one filter would pass the checks above).
        if "nearest" in payloads and "lanczos" in payloads:
            check(payloads["nearest"] != payloads["lanczos"], "nearest and lanczos produce different pixels")
        if "fsr1-quality" in payloads and "fsr1-ultra" in payloads:
            check(
                payloads["fsr1-quality"] != payloads["fsr1-ultra"],
                "fsr1-quality and fsr1-ultra produce different pixels",
            )

        # Downscaling works too (anti-aliased since 0.5).
        for mode in ("fsr1-noise", "fsr1-coherence", "fsr1-both"):
            check(payloads.get(mode) != payloads.get("fsr1"), f"{mode} exercises an experimental path")

        out = tmp / "down.ppm"
        r = run(tool, src, out, 8, 6, "easu")
        check(r.returncode == 0, "downscale exits 0")
        if r.returncode == 0:
            magic, size, maxval, rest = read_header(out)
            check(size == b"8 6" and len(rest) == 8 * 6 * 3, "downscale writes a well-formed 8x6 P6")

        # Default mode (no fifth argument) is accepted.
        check(run(tool, src, tmp / "default.ppm", 32, 24).returncode == 0, "default mode exits 0")

        # Usage errors: exit 2.
        check(run(tool).returncode == 2, "no arguments exits 2")
        check(run(tool, src, tmp / "x.ppm", 32).returncode == 2, "missing height exits 2")
        check(run(tool, src, tmp / "x.ppm", 1, 1, "retro", "extra").returncode == 2, "extra argument exits 2")

        # Invalid inputs: exit 1 with a diagnostic.
        r = run(tool, src, tmp / "x.ppm", 0, 30)
        check(r.returncode == 1 and "invalid" in r.stderr, "zero width is rejected")
        check(
            run(tool, src, tmp / "x.ppm", 99999999999, 30).returncode == 1, "out-of-range width is rejected"
        )
        check(
            run(tool, src, tmp / "x.ppm", 4294967295, 30).returncode == 1, "unrepresentable size is rejected"
        )
        check(run(tool, src, tmp / "x.ppm", 32, 24, "bogus").returncode == 1, "unknown mode is rejected")
        check(
            run(tool, tmp / "missing.ppm", tmp / "x.ppm", 32, 24).returncode == 1, "missing input is rejected"
        )

        bad = tmp / "bad.ppm"
        write_ppm(bad, 8, 8, magic=b"P5")
        check(run(tool, bad, tmp / "x.ppm", 16, 16).returncode == 1, "non-P6 input is rejected")


    if FAILURES:
        print(f"{len(FAILURES)} check(s) failed")
        return 1
    print("all checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
