#!/usr/bin/env python3
"""Check benchmark coverage and machine-readable wall-clock results without timing thresholds."""

import json
import subprocess
import sys
import tempfile
from pathlib import Path


def main() -> None:
    executable = str(Path(sys.argv[1]).resolve())
    listed = subprocess.check_output([executable, "--benchmark_list_tests=true"], text=True).splitlines()
    legacy = ["nearest", "bilinear", "sharp_bilinear", "catmull_rom", "lanczos3", "edge_adaptive", "easu"]
    modes = [f"legacy/{mode}" for mode in legacy]
    modes += [f"fsr1/{mode}" for mode in ["baseline", "noise", "coherence", "both"]]
    modes += ["pixel_art/nearest"]
    expected = {
        f"{mode}/{size}/workers:{workers}/real_time"
        for mode in modes
        for size in ["256x240", "320x240", "640x480", "1280x720", "1920x1080"]
        for workers in [0, 1, 4]
    }
    assert set(listed) == expected
    assert len(listed) == len(expected)
    with tempfile.TemporaryDirectory() as directory:
        output = Path(directory) / "bench.json"
        subprocess.run(
            [
                executable,
                "--benchmark_filter=^(fsr1/.*|pixel_art/nearest)/256x240/workers:1/",
                "--benchmark_min_time=1x",
                "--benchmark_repetitions=3",
                "--benchmark_out_format=json",
                f"--benchmark_out={output}",
            ],
            check=True,
            capture_output=True,
            timeout=90,
        )
        document = json.loads(output.read_text())
        assert document["context"]["retrofsr_version"]
        assert "wall time" in document["context"]["timing"]
        entries = document["benchmarks"]
        raw = [entry for entry in entries if entry["run_type"] == "iteration"]
        assert len(raw) == 15
        assert len({entry["run_name"] for entry in raw}) == 5
        for entry in raw:
            assert not entry.get("error_occurred", False)
            assert entry["iterations"] == 1
            assert entry["time_unit"] == "ms"
            assert entry["real_time"] > 0
            assert entry["Mpix/s"] > 0
            # Counter must describe output pixels using wall time (not main-thread CPU time).
            expected_rate = (256 * 240 * 4 / 1e6) / (entry["real_time"] / 1000)
            assert abs(entry["Mpix/s"] / expected_rate - 1) < 0.01
        medians = [entry for entry in entries if entry.get("aggregate_name") == "median"]
        assert len(medians) == 5
    invalid = subprocess.run([executable, "--not-a-benchmark-option"], capture_output=True, timeout=10)
    assert invalid.returncode != 0
    print("benchmark matrix and JSON checks passed")


if __name__ == "__main__":
    main()
