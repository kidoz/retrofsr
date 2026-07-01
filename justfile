set shell := ["sh", "-eu", "-c"]

build_dir := env_var_or_default("BUILD_DIR", "buildDir")
format_files := "include/retrofsr/retrofsr.hpp src/retrofsr.cpp tests/basic.cpp examples/scale_ppm.cpp"
tidy_files := "src/retrofsr.cpp tests/basic.cpp examples/scale_ppm.cpp"

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
