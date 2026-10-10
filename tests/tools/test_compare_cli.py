#!/usr/bin/env python3
"""Meson test for the retrofsr_compare CLI.

Usage: test_compare_cli.py <path-to-retrofsr_compare>

Exercises the documented contract: metrics on stdout, exit 0 when thresholds
pass, exit 1 when a threshold is violated, exit 2 for usage/file errors.
"""

import subprocess
import sys
import tempfile
from collections.abc import Callable
from pathlib import Path

FAILURES: list[str] = []


def check(condition: bool, message: str) -> None:
    if not condition:
        FAILURES.append(message)
        print(f"FAIL: {message}")
    else:
        print(f"ok: {message}")


def write_ppm(
    path: Path,
    width: int,
    height: int,
    pixel_fn: Callable[[int, int], tuple[int, int, int]],
    maxval: int = 255,
    magic: bytes = b"P6",
    truncate: int = 0,
) -> None:
    data = bytearray()
    for y in range(height):
        for x in range(width):
            data += bytes(pixel_fn(x, y))
    if truncate:
        data = data[:-truncate]
    path.write_bytes(magic + b"\n%d %d\n%d\n" % (width, height, maxval) + bytes(data))


def run(tool: str, *args: str | Path | int | float) -> subprocess.CompletedProcess[str]:
    return subprocess.run([tool, *map(str, args)], capture_output=True, text=True, timeout=60)


def metric(stdout: str, name: str) -> float:
    for line in stdout.splitlines():
        if line.startswith(name + ":"):
            return float(line.split(":", 1)[1])
    raise AssertionError(f"metric {name} not found in output: {stdout!r}")


def main() -> int:
    tool = sys.argv[1]
    with tempfile.TemporaryDirectory() as tmp_dir:
        tmp = Path(tmp_dir)
        base = tmp / "base.ppm"
        same = tmp / "same.ppm"
        noisy = tmp / "noisy.ppm"
        noisier = tmp / "noisier.ppm"
        small = tmp / "small.ppm"

        def gradient(x: int, y: int) -> tuple[int, int, int]:
            return ((x * 4) % 256, (y * 5) % 256, 128)

        def with_noise(amplitude: int) -> Callable[[int, int], tuple[int, int, int]]:
            def fn(x: int, y: int) -> tuple[int, int, int]:
                r, g, b = gradient(x, y)
                n = ((x * 7 + y * 13) % (2 * amplitude + 1)) - amplitude
                return (max(0, min(255, r + n)), max(0, min(255, g + n)), b)

            return fn

        write_ppm(base, 64, 48, gradient)
        write_ppm(same, 64, 48, gradient)
        write_ppm(noisy, 64, 48, with_noise(2))
        write_ppm(noisier, 64, 48, with_noise(9))
        write_ppm(small, 32, 48, gradient)

        # Identical images: exit 0, mse 0, infinite PSNR, SSIM 1.
        r = run(tool, base, same)
        check(r.returncode == 0, "identical images exit 0")
        check(metric(r.stdout, "mse") == 0.0, "identical images have mse 0")
        check(metric(r.stdout, "psnr_db") == float("inf"), "identical images have infinite PSNR")
        check(metric(r.stdout, "ssim") == 1.0, "identical images have SSIM 1")

        # Noise ordering: more noise means lower PSNR and lower SSIM.
        r1 = run(tool, base, noisy)
        r2 = run(tool, base, noisier)
        check(r1.returncode == 0 and r2.returncode == 0, "noisy comparisons exit 0 without thresholds")
        check(metric(r1.stdout, "psnr_db") > metric(r2.stdout, "psnr_db"), "PSNR orders by noise level")
        check(metric(r1.stdout, "ssim") > metric(r2.stdout, "ssim"), "SSIM orders by noise level")

        # Thresholds: pass -> 0, violation -> 1.
        psnr = metric(r1.stdout, "psnr_db")
        check(run(tool, base, noisy, "--min-psnr", psnr - 5).returncode == 0, "met PSNR threshold exits 0")
        check(
            run(tool, base, noisy, "--min-psnr", psnr + 5).returncode == 1, "violated PSNR threshold exits 1"
        )
        check(run(tool, base, noisy, "--min-ssim", "0.5").returncode == 0, "met SSIM threshold exits 0")
        check(
            run(tool, base, noisy, "--min-ssim", "0.9999999").returncode == 1,
            "violated SSIM threshold exits 1",
        )

        # Usage/file errors: exit 2.
        check(run(tool).returncode == 2, "no arguments exits 2")
        check(run(tool, base, small).returncode == 2, "size mismatch exits 2")
        check(run(tool, base, tmp / "missing.ppm").returncode == 2, "missing file exits 2")
        r = run(tool, base, noisy, "--min-psnr")
        check(r.returncode == 2, "missing threshold value exits 2")
        check("missing value" in r.stderr, "missing threshold value is reported clearly")
        check(run(tool, base, noisy, "--bogus").returncode == 2, "unknown argument exits 2")
        check(
            run(tool, base, noisy, "--min-psnr", "not-a-number").returncode == 2,
            "non-numeric threshold exits 2",
        )

        malformed = tmp / "malformed.ppm"
        write_ppm(malformed, 8, 8, gradient, magic=b"P5")
        check(run(tool, base, malformed).returncode == 2, "non-P6 magic exits 2")
        write_ppm(malformed, 8, 8, gradient, maxval=999)
        check(run(tool, malformed, malformed).returncode == 2, "unsupported maxval exits 2")
        write_ppm(malformed, 64, 48, gradient, truncate=100)
        check(run(tool, base, malformed).returncode == 2, "truncated pixel data exits 2")

    if FAILURES:
        print(f"{len(FAILURES)} check(s) failed")
        return 1
    print("all checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
