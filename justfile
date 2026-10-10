set shell := ["sh", "-eu", "-c"]

build_dir := env_var_or_default("BUILD_DIR", "buildDir")
# Derived from git so new files cannot silently skip lint. benchmarks/ is excluded
# from tidy because its compile commands live in the bench build dir, not the default one.
format_files := `git ls-files '*.cpp' '*.hpp' | tr '\n' ' '`
tidy_files := `git ls-files '*.cpp' | grep -v '^benchmarks/' | tr '\n' ' '`

default:
    @just --list

setup:
    @if [ -d "{{build_dir}}" ]; then \
        meson setup "{{build_dir}}" --reconfigure; \
    else \
        meson setup "{{build_dir}}"; \
    fi

build: setup
    meson compile -C "{{build_dir}}"

test: build
    meson test -C "{{build_dir}}"

# Regenerate the on-disk golden images (tests/data/golden) after an intentional
# image-quality change; review the diffs before committing. Builds only the generator.
golden: setup
    meson compile -C "{{build_dir}}" retrofsr_golden_gen
    ./"{{build_dir}}"/retrofsr_golden_gen tests/data/golden

# Python quality gates and the full regression suite, including visual-report checks.
quality: python-check build
    meson test -C "{{build_dir}}" --print-errorlogs

# Compare two PPM images: prints MSE/PSNR/SSIM; pass --min-psnr/--min-ssim thresholds.
compare a b *args: build
    ./"{{build_dir}}"/retrofsr_compare "{{a}}" "{{b}}" {{args}}

# Build and run throughput/latency benchmarks for common emulator sizes.
# Uses a dedicated build dir so -Dbenchmarks=true never sticks to the default one
# (a stale option there breaks reconfigure against trees that lack the option).
bench_dir := env_var_or_default("BENCH_DIR", "buildBench")
bench-setup:
    @if [ -d "{{bench_dir}}" ]; then \
        meson setup "{{bench_dir}}" --reconfigure -Dbenchmarks=true > /dev/null; \
    else \
        meson setup "{{bench_dir}}" -Dbenchmarks=true > /dev/null; \
    fi

bench-build: bench-setup
    meson compile -C "{{bench_dir}}" retrofsr_bench

# Repeated wall-time measurements; JSON retains individual repetitions.
# Pass --benchmark_filter=... for a subset and --benchmark_out=... to keep a baseline.
[positional-arguments]
bench *args: bench-build
    ./"{{bench_dir}}"/retrofsr_bench --benchmark_repetitions=5 --benchmark_min_time=0.2s --benchmark_enable_random_interleaving=true --benchmark_display_aggregates_only=true --benchmark_out="{{bench_dir}}/latest.json" --benchmark_out_format=json "$@"

format:
    clang-format -i {{format_files}}
    uv run --locked ruff format

format-check:
    clang-format --dry-run --Werror {{format_files}}
    uv run --locked ruff format --check

# Reproducible Python 3.14 environment, lint/format, and strict type checking.
python-setup:
    uv sync --locked

python-check:
    uv run --locked ruff check
    uv run --locked ruff format --check
    uv run --locked mypy

tidy: setup
    @sdk="$(xcrun --show-sdk-path 2>/dev/null || true)"; \
    if [ -n "$sdk" ]; then sdk_arg="--extra-arg=-isysroot$sdk"; else sdk_arg=""; fi; \
    clang-tidy -p "{{build_dir}}" $sdk_arg {{tidy_files}}

tidy-fix: setup
    @sdk="$(xcrun --show-sdk-path 2>/dev/null || true)"; \
    if [ -n "$sdk" ]; then sdk_arg="--extra-arg=-isysroot$sdk"; else sdk_arg=""; fi; \
    clang-tidy -p "{{build_dir}}" $sdk_arg --fix {{tidy_files}}

lint: python-check format-check tidy

check: python-check format-check tidy test

# Produce a new offline report directory. Example: just visual-review buildVisualReview/run1
visual-review output *args: build
    uv run --locked python tools/visual_compare.py --tool "{{build_dir}}/scale_ppm" --output "{{output}}" {{args}}
