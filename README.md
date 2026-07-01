# retrofsr

`retrofsr` is a tiny C++23 CPU spatial upscaling library intended for emulator frontends and render backends. It is **FSR-1-inspired**, but it is not a copy of AMD's shader code. The implementation is clean-room C++ and MIT-licensed.

It provides two practical presets:

- `retrofsr::retro_pixel_options()` for NES, Master System, Mega Drive/Genesis, SNES-style pixel art. This uses sharp bilinear filtering and no post-sharpening.
- `retrofsr::fsr_like_options()` for already-filtered frames, 3D emulator output, blended video, or games that benefit from spatial edge-aware upscaling plus contrast-adaptive sharpening.

## Features

- C++23, no third-party runtime dependencies.
- Meson build system.
- CPU implementation; no Vulkan, D3D, OpenGL, or compute API required.
- Supports `rgba8`, `bgra8`, `argb8`, `abgr8`, `xrgb8`, `xbgr8`, `rgbx8`, `bgrx8`, `rgb565`, `bgr565`, and `gray8`.
- Filters: nearest, bilinear, sharp bilinear, Catmull-Rom, Lanczos3, edge-adaptive Lanczos.
- Optional sRGB-aware interpolation and optional straight/premultiplied alpha handling.

## Build

```sh
meson setup build
meson compile -C build
meson test -C build
```

Install:

```sh
meson install -C build
```

Disable examples/tests:

```sh
meson setup build -Dexamples=false -Dtests=false
```

## Minimal use

```cpp
#include <retrofsr/retrofsr.hpp>
#include <vector>
#include <cstddef>
#include <cstdint>
#include <stdexcept>

void scale_frame(const std::uint8_t* src_rgba,
                 std::uint32_t src_w,
                 std::uint32_t src_h,
                 std::vector<std::byte>& dst_rgba,
                 std::uint32_t dst_w,
                 std::uint32_t dst_h) {
    dst_rgba.resize(static_cast<std::size_t>(dst_w) * dst_h * 4);

    retrofsr::ImageView src{
        .pixels = reinterpret_cast<const std::byte*>(src_rgba),
        .width = src_w,
        .height = src_h,
        .stride_bytes = 0,
        .format = retrofsr::PixelFormat::rgba8,
    };

    retrofsr::MutableImageView dst{
        .pixels = dst_rgba.data(),
        .width = dst_w,
        .height = dst_h,
        .stride_bytes = 0,
        .format = retrofsr::PixelFormat::rgba8,
    };

    auto options = retrofsr::retro_pixel_options();
    const auto status = retrofsr::upscale(src, dst, options);
    if (status != retrofsr::Status::ok) {
        throw std::runtime_error(retrofsr::status_message(status));
    }
}
```

## Example tool

The example reads and writes binary PPM (`P6`) images:

```sh
./build/scale_ppm input.ppm output.ppm 1280 960 retro
./build/scale_ppm input.ppm output.ppm 1280 960 fsr
```

Modes: `retro`, `fsr`, `nearest`, `bilinear`, `catmull`, `lanczos`.

## Notes for emulators

For exact integer scaling, `Filter::nearest` is fastest and sharpest. For fractional window sizes such as 256x240 to 1280x720, `retro_pixel_options()` often looks better than naive bilinear because it keeps most texel interiors crisp and blends only near uneven boundaries.

For N64, PS1, Saturn 3D, or emulator output that already includes antialiasing or texture filtering, try `fsr_like_options()` and tune `Options::sharpening` between `0.10f` and `0.35f`.

This library is not bit-equivalent to AMD FidelityFX Super Resolution 1.0. It follows the same high-level idea of spatial upscaling plus optional robust sharpening, but uses a simple portable CPU algorithm that is easy to integrate into emulators.
