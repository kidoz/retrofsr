#!/usr/bin/env python3
"""End-to-end checks for capture reports, mode isolation, and safe report destinations."""

import hashlib
import json
import subprocess
import sys
import tempfile
from pathlib import Path


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
        assert len(frame["outputs"]) == 4
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
                "1.5",
            ],
            check=True,
            capture_output=True,
            timeout=120,
        )
        generated = json.loads((synthetic / "manifest.json").read_text())
        assert len(generated["sequences"]) == 6
        for scene in generated["sequences"]:
            assert "reference" in scene["frames"][0]
            for mode in generated["modes"]:
                assert "temporal_error_delta" in scene["metrics"][mode][1]
        print("visual comparison report checks passed")


if __name__ == "__main__":
    main()
