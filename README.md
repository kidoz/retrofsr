# retrofsr

`retrofsr` is a tiny C++23 CPU spatial upscaling library intended for emulator frontends and render backends. It is **FSR-1-inspired**, not a bit-equivalent implementation of AMD FSR. The implementation is portable C++ and MIT-licensed; the optional RCAS noise detector adapts AMD code with attribution in [LICENSE](LICENSE).

It provides three practical presets:

- `retrofsr::retro_pixel_options()` for NES, Master System, Mega Drive/Genesis, SNES-style pixel art. This uses sharp bilinear filtering and no post-sharpening.
- `retrofsr::fsr_like_options()` for already-filtered frames, 3D emulator output, blended video, or games that benefit from spatial edge-aware upscaling plus contrast-adaptive sharpening.
- `retrofsr::fsr1_style_options(preset)` for the most faithful FSR-1-style result: an EASU-like edge-adaptive anisotropic filter plus RCAS-like robust sharpening, operating on gamma-encoded (perceptual) color the way FSR 1 does.

## Features

- C++23, no third-party runtime dependencies.
- Meson build system.
- CPU implementation; no Vulkan, D3D, OpenGL, or compute API required.
- Supports `rgba8`, `bgra8`, `argb8`, `abgr8`, `xrgb8`, `xbgr8`, `rgbx8`, `bgrx8`, `rgb565`, `bgr565`, and `gray8`.
- Filters: nearest, bilinear, sharp bilinear, Catmull-Rom, Lanczos3, edge-adaptive Lanczos, EASU-like edge-adaptive anisotropic.
- Sharpeners: contrast-adaptive unsharp mask, RCAS-like robust contrast-adaptive sharpening.
- Color modes: sRGB (linearized), linear, and perceptual (gamma-encoded, FSR-1 style).
- Quality preset helpers for common FSR scale factors: Quality 1.5x, Balanced 1.7x, Performance 2.0x, Ultra Performance 3.0x.
- Optional straight/premultiplied alpha handling with alpha-aware sharpening that avoids halos around transparency.
- Optional deterministic ordered dithering (`Options::dither`) for `rgb565`, `bgr565`, `gray8`, and 8-bit outputs; off by default.
- When sharpening is disabled, upscaling streams directly into the destination without the intermediate float image.

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

Other options: `-Dtools=false` skips the `retrofsr_compare` metrics tool, `-Dbenchmarks=true` builds `retrofsr_bench`, and `-Dsimd=true` enables vectorization-friendly math flags (`-fno-math-errno`, `-fno-trapping-math`); the default build is the scalar fallback of the same kernels.

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

Modes: `retro`, `fsr`, `fsr1`, `fsr1-noise`, `fsr1-coherence`, `fsr1-both`, `fsr1-quality`, `fsr1-balanced`, `fsr1-performance`, `fsr1-ultra`, `nearest`, `bilinear`, `catmull`, `lanczos`, `easu`.

## Choosing a mode

**Filter presets:**

- **Retro pixel** (`retro_pixel_options()`): pixel art and tile/sprite output where you want crisp texels. Sharp bilinear, no sharpening.
- **FSR-like** (`fsr_like_options()`): edge-adaptive Lanczos in linearized sRGB. A good general default for filtered/3D frames when you want physically plausible blending.
- **FSR-1-style** (`fsr1_style_options(preset)`): EASU-like filtering plus RCAS-like sharpening on perceptual color. Closest to how FSR 1 actually behaves; best for already-tonemapped 3D emulator output. Feed it post-tonemap color, and apply it before film grain or UI composition.

**Color modes** (`Options::color_space`):

- `srgb`: converts 8-bit sRGB to linear light before filtering and encodes back afterwards. Most physically correct blending; slightly more expensive.
- `linear`: treats channel values as already-linear data. Use for non-color data or when the input really is linear.
- `perceptual`: filters gamma-encoded values directly, as FSR 1 does. Preserves apparent contrast and costs the same as `linear`. Use with `Filter::easu` for FSR-style output.

**Quality presets** map output size to a recommended render size:

| Preset | Scale per axis | Example (1920x1080 output) |
|---|---|---|
| `QualityPreset::quality` | 1.5x | 1280x720 |
| `QualityPreset::balanced` | 1.7x | 1129x635 |
| `QualityPreset::performance` | 2.0x | 960x540 |
| `QualityPreset::ultra_performance` | 3.0x | 640x360 |

Use `retrofsr::quality_preset_source_size(preset, out_w, out_h)` to compute the source size, and `fsr1_style_options(preset)` for matching sharpening strength.

## Experimental spatial controls (0.6)

Both controls are opt-in; existing presets keep their previous output:

```cpp
auto options = retrofsr::fsr1_style_options();
options.rcas_noise_suppression = true;
options.easu_edge_coherence = true;
```

`rcas_noise_suppression` reduces RCAS sharpening of locally noise-like detail.
It limits noise amplification rather than removing noise, and can also reduce
sharpening of intentional dithering or fine textures. It only affects
`Sharpen::robust_contrast_adaptive` when sharpening is enabled.

`easu_edge_coherence` uses agreement between local gradients to attenuate EASU
kernel stretching at corners and conflicting texture directions. It only affects
EASU upscaling; the minification fallback is unchanged. It is an experiment,
not a claim of improved quality on every image. Compare moving content as well
as still frames before selecting it for a frontend.

Version 0.6 changes the `Options` layout and shared-library ABI; rebuild callers.
The shared-library ABI version is 0.6. No runtime dependencies were added.

## Quality and performance tooling

With [`just`](https://github.com/casey/just) installed:

- `just quality` — image-quality regression suite: golden images plus dither, alpha, and anti-ringing property tests.
- `just golden` — regenerate golden expectations after an intentional image-quality change (review the diff before committing).
- `just compare a.ppm b.ppm --min-psnr 40 --min-ssim 0.98` — MSE/PSNR/SSIM between two PPM images with optional pass/fail thresholds (`retrofsr_compare`).
- `just bench` — throughput and latency per filter for 256x240, 320x240, 640x480, 1280x720, and 1920x1080 sources at 2x upscale (`retrofsr_bench`).

## Downscaling

`upscale()` also downscales, and does it with proper anti-aliasing: the filter kernel is
widened per axis by the minification factor (the same approach as stb_image_resize,
Pillow, and MATLAB `imresize`), so shrinking a checkerboard converges to its mean
instead of shimmering. Mixed-axis resizes (one axis up, one down) are handled per axis.

Caveats:

- `Filter::nearest` stays point-sampled and will alias — by design, for exact pixel drops.
- `Filter::sharp_bilinear` behaves as plain (widened) bilinear when an axis downscales.
- `Filter::easu` switches to a widened isotropic kernel with its anti-ringing clamp;
  the edge-directed logic only applies when upscaling.
- Widened support is capped at 8 source texels per axis; for minification beyond ~4:1,
  pre-reduce in steps.

## Notes for emulators

For exact integer scaling, `Filter::nearest` is fastest and sharpest. For fractional window sizes such as 256x240 to 1280x720, `retro_pixel_options()` often looks better than naive bilinear because it keeps most texel interiors crisp and blends only near uneven boundaries.

For N64, PS1, Saturn 3D, or emulator output that already includes antialiasing or texture filtering, try `fsr_like_options()` and tune `Options::sharpening` between `0.10f` and `0.35f`.

This library is not bit-equivalent to AMD FidelityFX Super Resolution 1.0. It follows the same high-level idea of spatial upscaling plus optional robust sharpening, but uses a simple portable CPU algorithm that is easy to integrate into emulators.
