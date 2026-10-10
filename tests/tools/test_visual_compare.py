#!/usr/bin/env python3
"""End-to-end checks for capture reports, mode isolation, and safe report destinations."""

import hashlib
import json
import struct
import subprocess
import sys
import tempfile
import zlib
from pathlib import Path


def read_rgb_png(path: Path) -> tuple[int, int, bytes]:
    """Decode our RGB8 PNG outputs to check pixel identity, not just file signatures."""
    data = path.read_bytes()
    width, height = struct.unpack(">II", data[16:24])
    compressed = bytearray()
    offset = 8
    while offset < len(data):
        size = struct.unpack(">I", data[offset : offset + 4])[0]
        if data[offset + 4 : offset + 8] == b"IDAT":
            compressed.extend(data[offset + 8 : offset + 8 + size])
        offset += size + 12
    rows = zlib.decompress(compressed)
    stride = width * 3 + 1
    assert len(rows) == height * stride
    assert all(rows[y * stride] == 0 for y in range(height))
    return width, height, b"".join(rows[y * stride + 1 : (y + 1) * stride] for y in range(height))


def main() -> None:
    tool = Path(sys.argv[1]).resolve()
    script = Path(__file__).resolve().parents[2] / "tools" / "visual_compare.py"
    with tempfile.TemporaryDirectory() as tmp_dir:
        tmp = Path(tmp_dir)
        source = tmp / "capture"
        source.mkdir()
        # Deliberately start the raster with whitespace-valued pixels.
        raster = bytes((9, 10, 13)) + bytes((i * 37) % 256 for i in range(8 * 6 * 3 - 3))
        ppm = source / "0000.ppm"
        ppm.write_bytes(b"P6\n# comment\n8 6\n255\n" + raster)
        output = tmp / "report"
        command = [
            sys.executable,
            str(script),
            "--tool",
            str(tool),
            "--input",
            str(source),
            "--output",
            str(output),
            "--scale",
            "1.5",
            "--frames",
            "1",
        ]
        subprocess.run(command, check=True, capture_output=True, timeout=120)
        manifest = json.loads((output / "manifest.json").read_text())
        assert manifest["kind"] == "capture"
        sequence = manifest["sequences"][0]
        assert (sequence["width"], sequence["height"]) == (12, 9)
        frame = sequence["frames"][0]
        assert frame["input_sha256"] == hashlib.sha256(raster).hexdigest()
        assert "reference" not in frame
        assert len(frame["outputs"]) == 5
        for image in frame["outputs"] + frame["differences"]:
            assert (output / image).read_bytes().startswith(b"\x89PNG\r\n\x1a\n")
        assert sequence["metrics"]["fsr1"][0]["baseline_mae"] == 0
        assert sequence["metrics"]["fsr1-both"][0]["baseline_mae"] > 0
        assert "reference_mae" not in sequence["metrics"]["fsr1"][0]
        assert (output / "static.html").is_file()
        assert b"acTL" in (output / "sequence-0/motion.png").read_bytes()
        assert "__MANIFEST__" not in (output / "index.html").read_text()
        original = (output / "manifest.json").read_bytes()
        assert subprocess.run(command, capture_output=True, timeout=120).returncode == 1
        assert (output / "manifest.json").read_bytes() == original
        # Synthetic reports supply references and a temporal error after frame 1.
        synthetic = tmp / "synthetic"
        subprocess.run(
            [
                sys.executable,
                str(script),
                "--tool",
                str(tool),
                "--synthetic",
                "--frames",
                "2",
                "--output",
                str(synthetic),
                "--scale",
                "2",
            ],
            check=True,
            capture_output=True,
            timeout=120,
        )
        generated = json.loads((synthetic / "manifest.json").read_text())
        assert len(generated["sequences"]) == 6
        for scene in generated["sequences"]:
            assert "reference" in scene["frames"][0]
            nearest_index = generated["modes"].index("nearest")
            source = (synthetic / scene["frames"][0]["source"]).with_suffix(".ppm").read_bytes()
            raster = source.split(b"\n", 3)[3]
            nearest_path = synthetic / scene["frames"][0]["outputs"][nearest_index]
            width, height, nearest = read_rgb_png(nearest_path)
            assert (width, height) == (128, 96)
            expected = b"".join(
                b"".join(raster[(y * 64 + x) * 3 : (y * 64 + x + 1) * 3] * 2 for x in range(64)) * 2
                for y in range(48)
            )
            assert nearest == expected, "nearest at 2x must repeat source pixels exactly"
            pw, ph, preview = read_rgb_png(
                synthetic / scene["frames"][0]["source"].split("/")[0] / "preview.png"
            )
            assert (pw, ph) == (256, 288)
            _, _, preview_nearest = read_rgb_png(synthetic / scene["frames"][1]["outputs"][nearest_index])
            for y in range(height):
                row_start = ((height * 2 + y) * pw) * 3
                assert (
                    preview[row_start : row_start + width * 3]
                    == preview_nearest[y * width * 3 : (y + 1) * width * 3]
                )
                assert preview[row_start + width * 3 : row_start + pw * 3] == bytes(width * 3)
            for mode in generated["modes"]:
                assert "temporal_error_delta" in scene["metrics"][mode][1]
        print("visual comparison report checks passed")


if __name__ == "__main__":
    main()
