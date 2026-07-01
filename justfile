set shell := ["sh", "-eu", "-c"]

build_dir := env_var_or_default("BUILD_DIR", "buildDir")
format_files := "include/retrofsr/retrofsr.hpp src/retrofsr.cpp tests/basic.cpp tests/golden.cpp tests/golden_support.hpp tests/gen_golden_data.cpp tests/fixtures.hpp tests/image_quality.cpp examples/scale_ppm.cpp tools/retrofsr_compare.cpp benchmarks/bench.cpp"
tidy_files := "src/retrofsr.cpp tests/basic.cpp tests/golden.cpp tests/gen_golden_data.cpp tests/image_quality.cpp examples/scale_ppm.cpp tools/retrofsr_compare.cpp benchmarks/bench.cpp"

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

# Regenerate golden-image expectations after an intentional image-quality change.
golden: build
    ./"{{build_dir}}"/retrofsr_golden_gen > tests/golden_data.hpp

# Image-quality regression suite: golden images plus quality/dither/alpha property tests.
quality: build
    ./"{{build_dir}}"/retrofsr_basic_test "[golden],[quality],[dither],[alpha],[fixtures]"

# Compare two PPM images: prints MSE/PSNR/SSIM; pass --min-psnr/--min-ssim thresholds.
compare a b *args: build
    ./"{{build_dir}}"/retrofsr_compare "{{a}}" "{{b}}" {{args}}

# Build and run throughput/latency benchmarks for common emulator sizes.
bench:
    meson setup "{{build_dir}}" --reconfigure -Dbenchmarks=true > /dev/null
    meson compile -C "{{build_dir}}" retrofsr_bench
    ./"{{build_dir}}"/retrofsr_bench

format:
    clang-format -i {{format_files}}

format-check:
    clang-format --dry-run --Werror {{format_files}}

tidy: setup
    @sdk="$(xcrun --show-sdk-path 2>/dev/null || true)"; \
    if [ -n "$sdk" ]; then sdk_arg="--extra-arg=-isysroot$sdk"; else sdk_arg=""; fi; \
    clang-tidy -p "{{build_dir}}" $sdk_arg {{tidy_files}}

tidy-fix: setup
    @sdk="$(xcrun --show-sdk-path 2>/dev/null || true)"; \
    if [ -n "$sdk" ]; then sdk_arg="--extra-arg=-isysroot$sdk"; else sdk_arg=""; fi; \
    clang-tidy -p "{{build_dir}}" $sdk_arg --fix {{tidy_files}}

lint: format-check tidy

check: format-check tidy test
