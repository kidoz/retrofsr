set shell := ["sh", "-eu", "-c"]

build_dir := env_var_or_default("BUILD_DIR", "buildDir")
# Derived from git so new files cannot silently skip lint. golden_data.hpp is
# generated (clang-format off); benchmarks/ is excluded from tidy because its
# compile commands live in the bench build dir, not the default one.
format_files := `git ls-files '*.cpp' '*.hpp' | grep -v 'tests/golden_data.hpp' | tr '\n' ' '`
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

# Regenerate golden-image expectations after an intentional image-quality change.
# Builds only the generator: a full build would compile the golden test, whose
# static_assert fails exactly when the matrix changed and regeneration is needed.
golden: setup
    meson compile -C "{{build_dir}}" retrofsr_golden_gen
    ./"{{build_dir}}"/retrofsr_golden_gen > tests/golden_data.hpp

# Image-quality regression suite: golden images plus quality/dither/alpha property tests.
quality: build
    ./"{{build_dir}}"/retrofsr_basic_test "[golden],[quality],[dither],[alpha],[fixtures]"

# Compare two PPM images: prints MSE/PSNR/SSIM; pass --min-psnr/--min-ssim thresholds.
compare a b *args: build
    ./"{{build_dir}}"/retrofsr_compare "{{a}}" "{{b}}" {{args}}

# Build and run throughput/latency benchmarks for common emulator sizes.
# Uses a dedicated build dir so -Dbenchmarks=true never sticks to the default one
# (a stale option there breaks reconfigure against trees that lack the option).
bench_dir := env_var_or_default("BENCH_DIR", "buildBench")
bench:
    @if [ -d "{{bench_dir}}" ]; then \
        meson setup "{{bench_dir}}" --reconfigure -Dbenchmarks=true > /dev/null; \
    else \
        meson setup "{{bench_dir}}" -Dbenchmarks=true > /dev/null; \
    fi
    meson compile -C "{{bench_dir}}" retrofsr_bench
    ./"{{bench_dir}}"/retrofsr_bench

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
