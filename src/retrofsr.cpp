#include <retrofsr/retrofsr.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <numbers>
#include <thread>
#include <vector>

namespace retrofsr {
namespace {

struct Color4 {
    float r = 0.0f;
    float g = 0.0f;
    float b = 0.0f;
    float a = 1.0f;
};

[[nodiscard]] constexpr float clamp01(float v) noexcept {
    return std::clamp(v, 0.0f, 1.0f);
}

[[nodiscard]] constexpr Color4 operator+(Color4 x, Color4 y) noexcept {
    return {x.r + y.r, x.g + y.g, x.b + y.b, x.a + y.a};
}

[[nodiscard]] constexpr Color4 operator-(Color4 x, Color4 y) noexcept {
    return {x.r - y.r, x.g - y.g, x.b - y.b, x.a - y.a};
}

[[nodiscard]] constexpr Color4 operator*(Color4 x, float s) noexcept {
    return {x.r * s, x.g * s, x.b * s, x.a * s};
}

[[nodiscard]] constexpr Color4 operator/(Color4 x, float s) noexcept {
    return {x.r / s, x.g / s, x.b / s, x.a / s};
}

constexpr Color4& operator+=(Color4& x, Color4 y) noexcept {
    x.r += y.r;
    x.g += y.g;
    x.b += y.b;
    x.a += y.a;
    return x;
}

[[nodiscard]] constexpr Color4 clamp_color(Color4 c) noexcept {
    return {clamp01(c.r), clamp01(c.g), clamp01(c.b), clamp01(c.a)};
}

[[nodiscard]] constexpr float luma(Color4 c) noexcept {
    // Rec. 709 luma coefficients. Inputs are already in the selected working space.
    return 0.2126f * c.r + 0.7152f * c.g + 0.0722f * c.b;
}

[[nodiscard]] constexpr std::ptrdiff_t stride_or_tight(std::uint32_t width, PixelFormat format,
                                                       std::ptrdiff_t stride) noexcept {
    return stride == 0 ? static_cast<std::ptrdiff_t>(width * bytes_per_pixel(format)) : stride;
}

[[nodiscard]] constexpr bool has_alpha(PixelFormat format) noexcept {
    switch (format) {
    case PixelFormat::rgba8:
    case PixelFormat::bgra8:
    case PixelFormat::argb8:
    case PixelFormat::abgr8:
        return true;
    case PixelFormat::xrgb8:
    case PixelFormat::xbgr8:
    case PixelFormat::rgbx8:
    case PixelFormat::bgrx8:
    case PixelFormat::rgb565:
    case PixelFormat::bgr565:
    case PixelFormat::gray8:
        return false;
    }
    return false;
}

[[nodiscard]] float srgb_to_linear_byte(std::uint8_t value) noexcept {
    const float c = static_cast<float>(value) / 255.0f;
    if (c <= 0.04045f) {
        return c / 12.92f;
    }
    return std::pow((c + 0.055f) / 1.055f, 2.4f);
}

[[nodiscard]] const std::array<float, 256>& srgb_table() noexcept {
    static const std::array<float, 256> table = [] {
        std::array<float, 256> values{};
        for (std::size_t i = 0; i < values.size(); ++i) {
            values[i] = srgb_to_linear_byte(static_cast<std::uint8_t>(i));
        }
        return values;
    }();
    return table;
}

[[nodiscard]] float from_u8(std::uint8_t value, ColorSpace color_space) noexcept {
    if (color_space == ColorSpace::srgb) {
        return srgb_table()[value];
    }
    return static_cast<float>(value) / 255.0f;
}

[[nodiscard]] float encode_channel(float value, ColorSpace color_space) noexcept {
    value = clamp01(value);
    if (color_space == ColorSpace::srgb) {
        if (value <= 0.0031308f) {
            return 12.92f * value;
        }
        return clamp01(1.055f * std::pow(value, 1.0f / 2.4f) - 0.055f);
    }
    return value;
}

// Quantize an encoded [0, 1] channel to [0, max_value]. dither is the rounding offset in
// [0, 1); 0.5 reproduces round-to-nearest, an ordered-dither offset trades banding for
// deterministic high-frequency noise.
[[nodiscard]] std::uint16_t quantize_channel(float encoded, std::uint16_t max_value, float dither) noexcept {
    const auto scaled = static_cast<int>(clamp01(encoded) * static_cast<float>(max_value) + dither);
    return static_cast<std::uint16_t>(std::clamp(scaled, 0, static_cast<int>(max_value)));
}

[[nodiscard]] std::uint8_t to_u8(float value, ColorSpace color_space, float dither = 0.5f) noexcept {
    return static_cast<std::uint8_t>(quantize_channel(encode_channel(value, color_space), 255, dither));
}

[[nodiscard]] constexpr float bayer4(std::uint32_t x, std::uint32_t y) noexcept {
    constexpr std::array<std::uint8_t, 16> matrix = {0, 8, 2, 10, 12, 4, 14, 6, 3, 11, 1, 9, 15, 7, 13, 5};
    return (static_cast<float>(matrix[(y & 3u) * 4u + (x & 3u)]) + 0.5f) / 16.0f;
}

[[nodiscard]] const std::byte* row_ptr(ImageView image, std::int32_t y) noexcept {
    return image.pixels +
           static_cast<std::ptrdiff_t>(y) * stride_or_tight(image.width, image.format, image.stride_bytes);
}

[[nodiscard]] std::byte* row_ptr(MutableImageView image, std::int32_t y) noexcept {
    return image.pixels +
           static_cast<std::ptrdiff_t>(y) * stride_or_tight(image.width, image.format, image.stride_bytes);
}

[[nodiscard]] Color4 load_raw_pixel(const std::byte* p, PixelFormat format, ColorSpace color_space) noexcept {
    auto u8 = [](std::byte b) noexcept { return static_cast<std::uint8_t>(b); };

    switch (format) {
    case PixelFormat::rgba8:
        return {from_u8(u8(p[0]), color_space), from_u8(u8(p[1]), color_space), from_u8(u8(p[2]), color_space),
                static_cast<float>(u8(p[3])) / 255.0f};
    case PixelFormat::bgra8:
        return {from_u8(u8(p[2]), color_space), from_u8(u8(p[1]), color_space), from_u8(u8(p[0]), color_space),
                static_cast<float>(u8(p[3])) / 255.0f};
    case PixelFormat::argb8:
        return {from_u8(u8(p[1]), color_space), from_u8(u8(p[2]), color_space), from_u8(u8(p[3]), color_space),
                static_cast<float>(u8(p[0])) / 255.0f};
    case PixelFormat::abgr8:
        return {from_u8(u8(p[3]), color_space), from_u8(u8(p[2]), color_space), from_u8(u8(p[1]), color_space),
                static_cast<float>(u8(p[0])) / 255.0f};
    case PixelFormat::xrgb8:
        return {from_u8(u8(p[1]), color_space), from_u8(u8(p[2]), color_space), from_u8(u8(p[3]), color_space), 1.0f};
    case PixelFormat::xbgr8:
        return {from_u8(u8(p[3]), color_space), from_u8(u8(p[2]), color_space), from_u8(u8(p[1]), color_space), 1.0f};
    case PixelFormat::rgbx8:
        return {from_u8(u8(p[0]), color_space), from_u8(u8(p[1]), color_space), from_u8(u8(p[2]), color_space), 1.0f};
    case PixelFormat::bgrx8:
        return {from_u8(u8(p[2]), color_space), from_u8(u8(p[1]), color_space), from_u8(u8(p[0]), color_space), 1.0f};
    case PixelFormat::rgb565: {
        const auto packed = static_cast<std::uint16_t>(u8(p[0]) | (static_cast<std::uint16_t>(u8(p[1])) << 8));
        const auto r5 = static_cast<std::uint8_t>((packed >> 11) & 0x1f);
        const auto g6 = static_cast<std::uint8_t>((packed >> 5) & 0x3f);
        const auto b5 = static_cast<std::uint8_t>(packed & 0x1f);
        const auto r8 = static_cast<std::uint8_t>((r5 << 3) | (r5 >> 2));
        const auto g8 = static_cast<std::uint8_t>((g6 << 2) | (g6 >> 4));
        const auto b8 = static_cast<std::uint8_t>((b5 << 3) | (b5 >> 2));
        return {from_u8(r8, color_space), from_u8(g8, color_space), from_u8(b8, color_space), 1.0f};
    }
    case PixelFormat::bgr565: {
        const auto packed = static_cast<std::uint16_t>(u8(p[0]) | (static_cast<std::uint16_t>(u8(p[1])) << 8));
        const auto b5 = static_cast<std::uint8_t>((packed >> 11) & 0x1f);
        const auto g6 = static_cast<std::uint8_t>((packed >> 5) & 0x3f);
        const auto r5 = static_cast<std::uint8_t>(packed & 0x1f);
        const auto r8 = static_cast<std::uint8_t>((r5 << 3) | (r5 >> 2));
        const auto g8 = static_cast<std::uint8_t>((g6 << 2) | (g6 >> 4));
        const auto b8 = static_cast<std::uint8_t>((b5 << 3) | (b5 >> 2));
        return {from_u8(r8, color_space), from_u8(g8, color_space), from_u8(b8, color_space), 1.0f};
    }
    case PixelFormat::gray8: {
        const float v = from_u8(u8(p[0]), color_space);
        return {v, v, v, 1.0f};
    }
    }
    return {};
}

void store_raw_pixel(std::byte* p, PixelFormat format, Color4 c, ColorSpace color_space, bool force_opaque,
                     float dither = 0.5f) noexcept {
    auto b = [](std::uint8_t v) noexcept { return static_cast<std::byte>(v); };

    c = clamp_color(c);
    const auto r = to_u8(c.r, color_space, dither);
    const auto g = to_u8(c.g, color_space, dither);
    const auto bl = to_u8(c.b, color_space, dither);
    const auto a = force_opaque ? std::uint8_t{255} : to_u8(c.a, ColorSpace::linear, dither);

    switch (format) {
    case PixelFormat::rgba8:
        p[0] = b(r);
        p[1] = b(g);
        p[2] = b(bl);
        p[3] = b(a);
        break;
    case PixelFormat::bgra8:
        p[0] = b(bl);
        p[1] = b(g);
        p[2] = b(r);
        p[3] = b(a);
        break;
    case PixelFormat::argb8:
        p[0] = b(a);
        p[1] = b(r);
        p[2] = b(g);
        p[3] = b(bl);
        break;
    case PixelFormat::abgr8:
        p[0] = b(a);
        p[1] = b(bl);
        p[2] = b(g);
        p[3] = b(r);
        break;
    case PixelFormat::xrgb8:
        p[0] = b(255);
        p[1] = b(r);
        p[2] = b(g);
        p[3] = b(bl);
        break;
    case PixelFormat::xbgr8:
        p[0] = b(255);
        p[1] = b(bl);
        p[2] = b(g);
        p[3] = b(r);
        break;
    case PixelFormat::rgbx8:
        p[0] = b(r);
        p[1] = b(g);
        p[2] = b(bl);
        p[3] = b(255);
        break;
    case PixelFormat::bgrx8:
        p[0] = b(bl);
        p[1] = b(g);
        p[2] = b(r);
        p[3] = b(255);
        break;
    case PixelFormat::rgb565: {
        // Quantize straight from the encoded floats: one rounding step, and the dither
        // offset lands on the real 5/6-bit grid instead of an intermediate 8-bit one.
        const auto r5 = quantize_channel(encode_channel(c.r, color_space), 31, dither);
        const auto g6 = quantize_channel(encode_channel(c.g, color_space), 63, dither);
        const auto b5 = quantize_channel(encode_channel(c.b, color_space), 31, dither);
        const auto packed = static_cast<std::uint16_t>((r5 << 11) | (g6 << 5) | b5);
        p[0] = b(static_cast<std::uint8_t>(packed & 0xff));
        p[1] = b(static_cast<std::uint8_t>(packed >> 8));
        break;
    }
    case PixelFormat::bgr565: {
        const auto b5 = quantize_channel(encode_channel(c.b, color_space), 31, dither);
        const auto g6 = quantize_channel(encode_channel(c.g, color_space), 63, dither);
        const auto r5 = quantize_channel(encode_channel(c.r, color_space), 31, dither);
        const auto packed = static_cast<std::uint16_t>((b5 << 11) | (g6 << 5) | r5);
        p[0] = b(static_cast<std::uint8_t>(packed & 0xff));
        p[1] = b(static_cast<std::uint8_t>(packed >> 8));
        break;
    }
    case PixelFormat::gray8: {
        const auto gray = to_u8(luma(c), color_space, dither);
        p[0] = b(gray);
        break;
    }
    }
}

[[nodiscard]] Color4 load_source(ImageView image, std::int32_t x, std::int32_t y, const Options& options) noexcept {
    x = std::clamp<std::int32_t>(x, 0, static_cast<std::int32_t>(image.width) - 1);
    y = std::clamp<std::int32_t>(y, 0, static_cast<std::int32_t>(image.height) - 1);

    const auto* p = row_ptr(image, y) + static_cast<std::ptrdiff_t>(x) * bytes_per_pixel(image.format);
    Color4 c = load_raw_pixel(p, image.format, options.color_space);

    if (options.alpha == AlphaMode::ignore || !has_alpha(image.format)) {
        c.a = 1.0f;
        return c;
    }

    c.a = clamp01(c.a);
    if (options.alpha == AlphaMode::straight) {
        c.r *= c.a;
        c.g *= c.a;
        c.b *= c.a;
    }
    return c;
}

[[nodiscard]] Color4 prepare_for_store(Color4 c, const Options& options, PixelFormat dst_format) noexcept {
    c = clamp_color(c);
    if (options.alpha == AlphaMode::ignore || !has_alpha(dst_format)) {
        c.a = 1.0f;
        return c;
    }
    if (options.alpha == AlphaMode::straight) {
        if (c.a > 1.0e-6f) {
            c.r /= c.a;
            c.g /= c.a;
            c.b /= c.a;
        } else {
            c.r = 0.0f;
            c.g = 0.0f;
            c.b = 0.0f;
        }
    }
    return clamp_color(c);
}

[[nodiscard]] constexpr float sinc(float x) noexcept {
    x = std::abs(x);
    if (x < 1.0e-5f) {
        return 1.0f;
    }
    const float pix = std::numbers::pi_v<float> * x;
    return std::sin(pix) / pix;
}

[[nodiscard]] constexpr float lanczos_kernel(float x, float radius) noexcept {
    x = std::abs(x);
    if (x >= radius) {
        return 0.0f;
    }
    return sinc(x) * sinc(x / radius);
}

[[nodiscard]] constexpr float catmull_rom_kernel(float x) noexcept {
    x = std::abs(x);
    if (x < 1.0f) {
        return 1.5f * x * x * x - 2.5f * x * x + 1.0f;
    }
    if (x < 2.0f) {
        return -0.5f * x * x * x + 2.5f * x * x - 4.0f * x + 2.0f;
    }
    return 0.0f;
}

// Cap on the widened per-axis kernel support during minification: sampling is O(r^2)
// per pixel and non-separable, so extreme minification should pre-reduce instead.
inline constexpr float max_minify_support = 8.0f;

// Clamp a requested minification factor so radius * minify never exceeds the support cap.
[[nodiscard]] constexpr float clamp_minify(float minify, float radius) noexcept {
    return std::clamp(minify, 1.0f, max_minify_support / radius);
}

[[nodiscard]] constexpr float smooth_fraction_for_sharp_bilinear(float fraction, float upscale_factor) noexcept {
    if (upscale_factor <= 1.0f) {
        return fraction;
    }
    // Narrow the blend region as the upscale factor grows. This keeps texel centers crisp
    // while still smoothing non-integer output-size boundaries.
    return clamp01((fraction - 0.5f) * upscale_factor + 0.5f);
}

[[nodiscard]] Color4 sample_nearest(ImageView image, float sx, float sy, const Options& options) noexcept {
    const auto ix = static_cast<std::int32_t>(std::floor(sx + 0.5f));
    const auto iy = static_cast<std::int32_t>(std::floor(sy + 0.5f));
    return load_source(image, ix, iy, options);
}

[[nodiscard]] Color4 sample_bilinear(ImageView image, float sx, float sy, const Options& options,
                                     float fraction_scale_x = 1.0f, float fraction_scale_y = 1.0f) noexcept {
    const auto ix = static_cast<std::int32_t>(std::floor(sx));
    const auto iy = static_cast<std::int32_t>(std::floor(sy));
    float fx = sx - static_cast<float>(ix);
    float fy = sy - static_cast<float>(iy);

    fx = smooth_fraction_for_sharp_bilinear(fx, fraction_scale_x);
    fy = smooth_fraction_for_sharp_bilinear(fy, fraction_scale_y);

    const Color4 c00 = load_source(image, ix, iy, options);
    const Color4 c10 = load_source(image, ix + 1, iy, options);
    const Color4 c01 = load_source(image, ix, iy + 1, options);
    const Color4 c11 = load_source(image, ix + 1, iy + 1, options);

    const Color4 cx0 = c00 * (1.0f - fx) + c10 * fx;
    const Color4 cx1 = c01 * (1.0f - fx) + c11 * fx;
    return cx0 * (1.0f - fy) + cx1 * fy;
}

[[nodiscard]] Color4 sample_catmull_rom(ImageView image, float sx, float sy, const Options& options,
                                        float minify_x = 1.0f, float minify_y = 1.0f) noexcept {
    minify_x = clamp_minify(minify_x, 2.0f);
    minify_y = clamp_minify(minify_y, 2.0f);
    const float support_x = 2.0f * minify_x;
    const float support_y = 2.0f * minify_y;

    const auto min_x = static_cast<std::int32_t>(std::ceil(sx - support_x));
    const auto max_x = static_cast<std::int32_t>(std::floor(sx + support_x));
    const auto min_y = static_cast<std::int32_t>(std::ceil(sy - support_y));
    const auto max_y = static_cast<std::int32_t>(std::floor(sy + support_y));

    Color4 sum{};
    float weight_sum = 0.0f;
    for (std::int32_t yy = min_y; yy <= max_y; ++yy) {
        const float wy = catmull_rom_kernel((sy - static_cast<float>(yy)) / minify_y);
        if (wy == 0.0f) {
            continue;
        }
        for (std::int32_t xx = min_x; xx <= max_x; ++xx) {
            const float wx = catmull_rom_kernel((sx - static_cast<float>(xx)) / minify_x);
            const float w = wx * wy;
            if (w == 0.0f) {
                continue;
            }
            sum += load_source(image, xx, yy, options) * w;
            weight_sum += w;
        }
    }
    if (std::abs(weight_sum) > 1.0e-7f) {
        sum = sum / weight_sum;
    }
    return sum;
}

// Widened triangle (tent) reconstruction for minified bilinear-family sampling; at
// minify == 1 this is exactly bilinear, so it is only dispatched when minifying.
[[nodiscard]] Color4 sample_triangle(ImageView image, float sx, float sy, const Options& options, float minify_x,
                                     float minify_y) noexcept {
    minify_x = clamp_minify(minify_x, 1.0f);
    minify_y = clamp_minify(minify_y, 1.0f);

    const auto min_x = static_cast<std::int32_t>(std::ceil(sx - minify_x));
    const auto max_x = static_cast<std::int32_t>(std::floor(sx + minify_x));
    const auto min_y = static_cast<std::int32_t>(std::ceil(sy - minify_y));
    const auto max_y = static_cast<std::int32_t>(std::floor(sy + minify_y));

    Color4 sum{};
    float weight_sum = 0.0f;
    for (std::int32_t yy = min_y; yy <= max_y; ++yy) {
        const float wy = 1.0f - std::abs(sy - static_cast<float>(yy)) / minify_y;
        if (wy <= 0.0f) {
            continue;
        }
        for (std::int32_t xx = min_x; xx <= max_x; ++xx) {
            const float wx = 1.0f - std::abs(sx - static_cast<float>(xx)) / minify_x;
            if (wx <= 0.0f) {
                continue;
            }
            const float w = wx * wy;
            sum += load_source(image, xx, yy, options) * w;
            weight_sum += w;
        }
    }
    if (weight_sum > 1.0e-7f) {
        sum = sum / weight_sum;
    }
    return sum;
}

[[nodiscard]] Color4 sample_lanczos(ImageView image, float sx, float sy, const Options& options, float radius_x,
                                    float radius_y, float minify_x = 1.0f, float minify_y = 1.0f) noexcept {
    radius_x = std::max(radius_x, 1.0f);
    radius_y = std::max(radius_y, 1.0f);
    // Minification stretches the kernel so each output pixel averages a proportionally
    // wider source neighborhood (the standard anti-aliasing approach). minify == 1 is
    // bit-identical to the plain upscaling path.
    minify_x = clamp_minify(minify_x, radius_x);
    minify_y = clamp_minify(minify_y, radius_y);
    const float support_x = radius_x * minify_x;
    const float support_y = radius_y * minify_y;

    const auto min_x = static_cast<std::int32_t>(std::ceil(sx - support_x));
    const auto max_x = static_cast<std::int32_t>(std::floor(sx + support_x));
    const auto min_y = static_cast<std::int32_t>(std::ceil(sy - support_y));
    const auto max_y = static_cast<std::int32_t>(std::floor(sy + support_y));

    Color4 sum{};
    float weight_sum = 0.0f;
    for (std::int32_t yy = min_y; yy <= max_y; ++yy) {
        const float wy = lanczos_kernel((sy - static_cast<float>(yy)) / minify_y, radius_y);
        if (wy == 0.0f) {
            continue;
        }
        for (std::int32_t xx = min_x; xx <= max_x; ++xx) {
            const float wx = lanczos_kernel((sx - static_cast<float>(xx)) / minify_x, radius_x);
            const float w = wx * wy;
            if (w == 0.0f) {
                continue;
            }
            sum += load_source(image, xx, yy, options) * w;
            weight_sum += w;
        }
    }
    if (std::abs(weight_sum) > 1.0e-7f) {
        sum = sum / weight_sum;
    }
    return sum;
}

[[nodiscard]] constexpr float easu_kernel(float d2) noexcept {
    // Lanczos2-like polynomial window evaluated on squared distance: unit at 0, zero
    // crossings at distance 1 and 2, one negative lobe in between.
    if (d2 >= 4.0f) {
        return 0.0f;
    }
    const float base = 0.4f * d2 - 1.0f;
    const float window = 0.25f * d2 - 1.0f;
    return ((25.0f / 16.0f) * base * base - (25.0f / 16.0f - 1.0f)) * window * window;
}

// Minified EASU-family sampling: the edge-direction/anisotropy stage assumes upscaling,
// so minification uses the same Lanczos2-like kernel widened per axis, with the
// anti-ringing clamp taken over the whole contributing window.
[[nodiscard]] Color4 sample_easu_minified(ImageView image, float sx, float sy, const Options& options, float minify_x,
                                          float minify_y) noexcept {
    minify_x = clamp_minify(minify_x, 2.0f);
    minify_y = clamp_minify(minify_y, 2.0f);
    const float support_x = 2.0f * minify_x;
    const float support_y = 2.0f * minify_y;

    const auto min_x = static_cast<std::int32_t>(std::ceil(sx - support_x));
    const auto max_x = static_cast<std::int32_t>(std::floor(sx + support_x));
    const auto min_y = static_cast<std::int32_t>(std::ceil(sy - support_y));
    const auto max_y = static_cast<std::int32_t>(std::floor(sy + support_y));

    Color4 sum{};
    float weight_sum = 0.0f;
    Color4 lo{1.0f, 1.0f, 1.0f, 1.0f};
    Color4 hi{0.0f, 0.0f, 0.0f, 0.0f};
    for (std::int32_t yy = min_y; yy <= max_y; ++yy) {
        const float dy = (sy - static_cast<float>(yy)) / minify_y;
        for (std::int32_t xx = min_x; xx <= max_x; ++xx) {
            const float dx = (sx - static_cast<float>(xx)) / minify_x;
            const float w = easu_kernel(dx * dx + dy * dy);
            if (w == 0.0f) {
                continue;
            }
            const Color4 t = load_source(image, xx, yy, options);
            sum += t * w;
            weight_sum += w;
            lo = {std::min(lo.r, t.r), std::min(lo.g, t.g), std::min(lo.b, t.b), std::min(lo.a, t.a)};
            hi = {std::max(hi.r, t.r), std::max(hi.g, t.g), std::max(hi.b, t.b), std::max(hi.a, t.a)};
        }
    }
    if (weight_sum <= 1.0e-5f) {
        return sample_triangle(image, sx, sy, options, minify_x, minify_y);
    }
    const Color4 out = sum / weight_sum;
    return {std::clamp(out.r, lo.r, hi.r), std::clamp(out.g, lo.g, hi.g), std::clamp(out.b, lo.b, hi.b),
            std::clamp(out.a, lo.a, hi.a)};
}

[[nodiscard]] Color4 sample_edge_adaptive_lanczos(ImageView image, float sx, float sy, const Options& options,
                                                  float minify_x = 1.0f, float minify_y = 1.0f) noexcept {
    const auto ix = static_cast<std::int32_t>(std::floor(sx + 0.5f));
    const auto iy = static_cast<std::int32_t>(std::floor(sy + 0.5f));

    const float left = luma(load_source(image, ix - 1, iy, options));
    const float right = luma(load_source(image, ix + 1, iy, options));
    const float up = luma(load_source(image, ix, iy - 1, options));
    const float down = luma(load_source(image, ix, iy + 1, options));

    const float gx = std::abs(right - left);
    const float gy = std::abs(down - up);
    const float sensitivity = std::max(0.0f, options.edge_strength);
    const float tx = clamp01(gx * 4.5f * sensitivity);
    const float ty = clamp01(gy * 4.5f * sensitivity);

    // Shrink the filter radius across strong local gradients to reduce halos around hard emulator edges.
    const float radius_x = std::lerp(3.0f, 1.35f, tx);
    const float radius_y = std::lerp(3.0f, 1.35f, ty);
    return sample_lanczos(image, sx, sy, options, radius_x, radius_y, minify_x, minify_y);
}

[[nodiscard]] Color4 sample_easu(ImageView image, float sx, float sy, const Options& options) noexcept {
    const auto base_x = static_cast<std::int32_t>(std::floor(sx));
    const auto base_y = static_cast<std::int32_t>(std::floor(sy));
    const float fx = sx - static_cast<float>(base_x);
    const float fy = sy - static_cast<float>(base_y);

    // 4x4 neighborhood; taps[j][i] is source texel (base_x - 1 + i, base_y - 1 + j).
    std::array<std::array<Color4, 4>, 4> taps{};
    std::array<std::array<float, 4>, 4> lum{};
    for (std::int32_t j = 0; j < 4; ++j) {
        for (std::int32_t i = 0; i < 4; ++i) {
            taps[j][i] = load_source(image, base_x - 1 + i, base_y - 1 + j, options);
            lum[j][i] = luma(taps[j][i]);
        }
    }

    // Adaptive edge direction: bilinearly blended central-difference gradients at the 2x2
    // quad nearest the sample point. The gradient points across the local edge.
    float dir_x = 0.0f;
    float dir_y = 0.0f;
    float tensor_xx = 0.0f;
    float tensor_xy = 0.0f;
    float tensor_yy = 0.0f;
    const auto accumulate_gradient = [&](std::int32_t i, std::int32_t j, float w) noexcept {
        const float gx = lum[j][i + 1] - lum[j][i - 1];
        const float gy = lum[j + 1][i] - lum[j - 1][i];
        dir_x += gx * w;
        dir_y += gy * w;
        if (options.easu_edge_coherence) {
            tensor_xx += gx * gx * w;
            tensor_xy += gx * gy * w;
            tensor_yy += gy * gy * w;
        }
    };
    accumulate_gradient(1, 1, (1.0f - fx) * (1.0f - fy));
    accumulate_gradient(2, 1, fx * (1.0f - fy));
    accumulate_gradient(1, 2, (1.0f - fx) * fy);
    accumulate_gradient(2, 2, fx * fy);

    // Strong coherent diagonal edges stretch the kernel along the edge, which smooths
    // stair-stepping. The stretch is orientation-dependent as in reference EASU: an
    // axis-aligned edge gets an isotropic kernel, because widening helps only where the
    // sampling grid staircases. The kernel is never tightened across the gradient — that
    // would push every tap of a hard edge into the negative lobe and invert the weights.
    float grad_x = 1.0f;
    float grad_y = 0.0f;
    float stretch = 0.0f;
    const float dir_len2 = dir_x * dir_x + dir_y * dir_y;
    if (dir_len2 > 1.0e-8f) {
        const float dir_len = std::sqrt(dir_len2);
        grad_x = dir_x / dir_len;
        grad_y = dir_y / dir_len;
        // 0 for axis-aligned gradients, 1 at 45 degrees.
        const float diagonality = 2.0f * std::min(grad_x * grad_x, grad_y * grad_y);
        stretch = clamp01(dir_len * 2.0f) * diagonality * clamp01(options.edge_strength);
        if (options.easu_edge_coherence) {
            // BLADE-style structure-tensor coherence: aligned gradients have one
            // nonzero eigenvalue; conflicting directions reduce confidence. Only
            // attenuate the existing stretch, retaining its axis-aligned safeguard.
            const float trace = tensor_xx + tensor_yy;
            const float delta = std::hypot(tensor_xx - tensor_yy, 2.0f * tensor_xy);
            const float major = std::sqrt(std::max(0.0f, 0.5f * (trace + delta)));
            const float minor = std::sqrt(std::max(0.0f, 0.5f * (trace - delta)));
            const float coherence = major + minor > 1.0e-6f ? (major - minor) / (major + minor) : 0.0f;
            stretch *= clamp01(coherence);
        }
    }
    const float along_scale = 1.0f / (1.0f + stretch);

    Color4 sum{};
    float weight_sum = 0.0f;
    for (std::int32_t j = 0; j < 4; ++j) {
        const float oy = static_cast<float>(base_y - 1 + j) - sy;
        for (std::int32_t i = 0; i < 4; ++i) {
            const float ox = static_cast<float>(base_x - 1 + i) - sx;
            const float across = ox * grad_x + oy * grad_y;
            const float along = (oy * grad_x - ox * grad_y) * along_scale;
            const float w = easu_kernel(across * across + along * along);
            if (w == 0.0f) {
                continue;
            }
            sum += taps[j][i] * w;
            weight_sum += w;
        }
    }

    // The nearest 2x2 taps always carry positive weight, so a healthy sum is strictly
    // positive; anything else means the kernel degenerated — fall back to bilinear.
    Color4 out;
    if (weight_sum > 1.0e-5f) {
        out = sum / weight_sum;
    } else {
        out = sample_bilinear(image, sx, sy, options);
    }

    // Anti-ringing: clamp to the min/max of the nearest 2x2 source quad so the negative
    // kernel lobes cannot overshoot around hard edges.
    Color4 lo = taps[1][1];
    Color4 hi = taps[1][1];
    for (std::int32_t j = 1; j <= 2; ++j) {
        for (std::int32_t i = 1; i <= 2; ++i) {
            const Color4 t = taps[j][i];
            lo = {std::min(lo.r, t.r), std::min(lo.g, t.g), std::min(lo.b, t.b), std::min(lo.a, t.a)};
            hi = {std::max(hi.r, t.r), std::max(hi.g, t.g), std::max(hi.b, t.b), std::max(hi.a, t.a)};
        }
    }
    return {std::clamp(out.r, lo.r, hi.r), std::clamp(out.g, lo.g, hi.g), std::clamp(out.b, lo.b, hi.b),
            std::clamp(out.a, lo.a, hi.a)};
}

/// Per-upscale constants hoisted out of the per-pixel loops.
struct SampleParams {
    /// dst/src upscale factors, used by sharp_bilinear's fraction narrowing.
    float fraction_scale_x = 1.0f;
    float fraction_scale_y = 1.0f;
    /// Per-axis minification factors (>= 1; 1 when upscaling on that axis). Kernels are
    /// widened by these so downscaled output is anti-aliased instead of point-sampled.
    float minify_x = 1.0f;
    float minify_y = 1.0f;
};

[[nodiscard]] Color4 sample_filter(ImageView image, float sx, float sy, SampleParams params,
                                   const Options& options) noexcept {
    const bool minifying = params.minify_x > 1.0f || params.minify_y > 1.0f;
    switch (options.filter) {
    case Filter::nearest:
        // Deliberate point sampling; documented as aliasing under minification.
        return sample_nearest(image, sx, sy, options);
    case Filter::bilinear:
    case Filter::sharp_bilinear:
        // sharp_bilinear's fraction narrowing only applies when upscaling; when either
        // axis minifies, both behave as the widened triangle reconstruction.
        if (minifying) {
            return sample_triangle(image, sx, sy, options, params.minify_x, params.minify_y);
        }
        if (options.filter == Filter::sharp_bilinear) {
            return sample_bilinear(image, sx, sy, options, params.fraction_scale_x, params.fraction_scale_y);
        }
        return sample_bilinear(image, sx, sy, options);
    case Filter::catmull_rom:
        return sample_catmull_rom(image, sx, sy, options, params.minify_x, params.minify_y);
    case Filter::lanczos3:
        return sample_lanczos(image, sx, sy, options, 3.0f, 3.0f, params.minify_x, params.minify_y);
    case Filter::edge_adaptive_lanczos:
        return sample_edge_adaptive_lanczos(image, sx, sy, options, params.minify_x, params.minify_y);
    case Filter::easu:
        if (minifying) {
            return sample_easu_minified(image, sx, sy, options, params.minify_x, params.minify_y);
        }
        return sample_easu(image, sx, sy, options);
    }
    return sample_edge_adaptive_lanczos(image, sx, sy, options, params.minify_x, params.minify_y);
}

template<typename Fn> void parallel_rows(std::uint32_t height, std::uint32_t requested_threads, Fn&& fn) {
    if (height == 0) {
        return;
    }

    std::uint32_t threads = requested_threads;
    if (threads == 0) {
        const auto hw = std::thread::hardware_concurrency();
        threads = hw == 0 ? 1u : hw;
    }
    threads = std::clamp<std::uint32_t>(threads, 1u, height);

    // For tiny emulator frames, thread startup can cost more than the work.
    if (threads == 1 || height < 32) {
        fn(0, height);
        return;
    }

    std::vector<std::jthread> workers;
    workers.reserve(threads);
    for (std::uint32_t t = 0; t < threads; ++t) {
        // 64-bit partition math: height can be up to INT32_MAX, so height * threads
        // would wrap a 32-bit product and hand overlapping row ranges to workers.
        const auto y0 = static_cast<std::uint32_t>(static_cast<std::uint64_t>(height) * t / threads);
        const auto y1 = static_cast<std::uint32_t>(static_cast<std::uint64_t>(height) * (t + 1) / threads);
        workers.emplace_back([&fn, y0, y1] { fn(y0, y1); });
    }
}

// True when sharpening should respect alpha: the work buffer holds premultiplied color, so
// unrestrained sharpening near transparency creates RGB energy the alpha cannot carry (halos).
[[nodiscard]] constexpr bool alpha_aware_sharpen(const Options& options) noexcept {
    return options.alpha_aware_sharpening && options.alpha != AlphaMode::ignore;
}

void apply_sharpen(std::vector<Color4>& image, std::uint32_t width, std::uint32_t height, const Options& options) {
    const float sharpening = std::clamp(options.sharpening, 0.0f, 1.0f);
    if (sharpening <= 0.0f || width == 0 || height == 0) {
        return;
    }
    const std::uint32_t thread_count = options.thread_count;
    const bool alpha_aware = alpha_aware_sharpen(options);

    const float base_amount = sharpening * 0.85f;
    const auto sharpen_one = [base_amount, alpha_aware](Color4 c, Color4 n, Color4 s, Color4 w,
                                                        Color4 e) noexcept -> Color4 {
        const float l0 = luma(c);
        const float lmin = std::min({l0, luma(n), luma(s), luma(w), luma(e)});
        const float lmax = std::max({l0, luma(n), luma(s), luma(w), luma(e)});
        const float range = lmax - lmin;
        const float adaptive = range / (range + 0.10f);
        float amount = base_amount * adaptive;
        if (alpha_aware) {
            amount *= clamp01(c.a);
        }

        const Color4 blur = (n + s + w + e) * 0.25f;
        Color4 out = c + (c - blur) * amount;

        const float margin = 0.06f + range * 0.08f;
        const float min_r = std::min({c.r, n.r, s.r, w.r, e.r}) - margin;
        const float max_r = std::max({c.r, n.r, s.r, w.r, e.r}) + margin;
        const float min_g = std::min({c.g, n.g, s.g, w.g, e.g}) - margin;
        const float max_g = std::max({c.g, n.g, s.g, w.g, e.g}) + margin;
        const float min_b = std::min({c.b, n.b, s.b, w.b, e.b}) - margin;
        const float max_b = std::max({c.b, n.b, s.b, w.b, e.b}) + margin;

        out.r = std::clamp(out.r, min_r, max_r);
        out.g = std::clamp(out.g, min_g, max_g);
        out.b = std::clamp(out.b, min_b, max_b);
        out.a = c.a;

        if (alpha_aware) {
            const float cap = clamp01(c.a);
            out.r = std::min(out.r, cap);
            out.g = std::min(out.g, cap);
            out.b = std::min(out.b, cap);
        }
        return clamp_color(out);
    };

    const std::vector<Color4> src = image;
    parallel_rows(height, thread_count, [&](std::uint32_t y0, std::uint32_t y1) {
        for (std::uint32_t y = y0; y < y1; ++y) {
            Color4* out_row = image.data() + static_cast<std::size_t>(y) * width;
            const Color4* mid = src.data() + static_cast<std::size_t>(y) * width;
            const Color4* up = src.data() + static_cast<std::size_t>(y == 0 ? y : y - 1) * width;
            const Color4* down = src.data() + static_cast<std::size_t>(y + 1 >= height ? y : y + 1) * width;

            // Border columns clamp the horizontal neighbors to the edge texel.
            out_row[0] = sharpen_one(mid[0], up[0], down[0], mid[0], mid[width > 1 ? 1 : 0]);
            if (width > 1) {
                out_row[width - 1] =
                    sharpen_one(mid[width - 1], up[width - 1], down[width - 1], mid[width - 2], mid[width - 1]);
            }
            for (std::uint32_t x = 1; x + 1 < width; ++x) {
                out_row[x] = sharpen_one(mid[x], up[x], down[x], mid[x - 1], mid[x + 1]);
            }
        }
    });
}

void apply_rcas(std::vector<Color4>& image, std::uint32_t width, std::uint32_t height, const Options& options) {
    const float sharpening = std::clamp(options.sharpening, 0.0f, 1.0f);
    if (sharpening <= 0.0f || width == 0 || height == 0) {
        return;
    }
    const std::uint32_t thread_count = options.thread_count;
    const bool alpha_aware = alpha_aware_sharpen(options);

    // Largest negative lobe that keeps the sharpened result inside the local value range.
    constexpr float lobe_limit = 0.25f - 1.0f / 16.0f;

    const auto rcas_one = [sharpening, alpha_aware, noise_suppression = options.rcas_noise_suppression](
                              Color4 c, Color4 n, Color4 s, Color4 w, Color4 e) noexcept -> Color4 {
        // Per-channel peak-limited lobe: never push the center past the point where the
        // result would leave [0, 1] given the local cross minimum/maximum.
        const auto channel_lobe = [](float cn, float cs, float cw, float ce) noexcept {
            constexpr float eps = 1.0e-4f;
            const float mn = std::min(std::min(cn, cs), std::min(cw, ce));
            const float mx = std::max(std::max(cn, cs), std::max(cw, ce));
            const float hit_min = mn / (4.0f * mx + eps);
            const float hit_max = (1.0f - mx) / (4.0f * mn - 4.0f - eps);
            return std::max(-hit_min, hit_max);
        };
        const float lobe_r = channel_lobe(n.r, s.r, w.r, e.r);
        const float lobe_g = channel_lobe(n.g, s.g, w.g, e.g);
        const float lobe_b = channel_lobe(n.b, s.b, w.b, e.b);
        float lobe = std::max(-lobe_limit, std::min(std::max({lobe_r, lobe_g, lobe_b}), 0.0f)) * sharpening;
        if (noise_suppression) {
            // Adapted from AMD FSR 1 RCAS noise detection; see LICENSE.
            // Normalize a cross high-pass by local contrast, then attenuate the lobe
            // by up to half. A flat neighborhood has zero residual and keeps its gain.
            const float lc = luma(c);
            const float ln = luma(n);
            const float ls = luma(s);
            const float lw = luma(w);
            const float le = luma(e);
            const float range = std::max({lc, ln, ls, lw, le}) - std::min({lc, ln, ls, lw, le});
            const float residual = std::abs(0.25f * (ln + ls + lw + le) - lc);
            lobe *= 1.0f - 0.5f * clamp01(residual / std::max(range, 1.0e-6f));
        }
        if (alpha_aware) {
            lobe *= clamp01(c.a);
        }

        const float rcp = 1.0f / (4.0f * lobe + 1.0f);
        Color4 out = ((n + s + w + e) * lobe + c) * rcp;
        out.a = c.a;
        if (alpha_aware) {
            const float cap = clamp01(c.a);
            out.r = std::min(out.r, cap);
            out.g = std::min(out.g, cap);
            out.b = std::min(out.b, cap);
        }
        return clamp_color(out);
    };

    const std::vector<Color4> src = image;
    parallel_rows(height, thread_count, [&](std::uint32_t y0, std::uint32_t y1) {
        for (std::uint32_t y = y0; y < y1; ++y) {
            Color4* out_row = image.data() + static_cast<std::size_t>(y) * width;
            const Color4* mid = src.data() + static_cast<std::size_t>(y) * width;
            const Color4* up = src.data() + static_cast<std::size_t>(y == 0 ? y : y - 1) * width;
            const Color4* down = src.data() + static_cast<std::size_t>(y + 1 >= height ? y : y + 1) * width;

            // Border columns clamp the horizontal neighbors to the edge texel.
            out_row[0] = rcas_one(mid[0], up[0], down[0], mid[0], mid[width > 1 ? 1 : 0]);
            if (width > 1) {
                out_row[width - 1] =
                    rcas_one(mid[width - 1], up[width - 1], down[width - 1], mid[width - 2], mid[width - 1]);
            }
            for (std::uint32_t x = 1; x + 1 < width; ++x) {
                out_row[x] = rcas_one(mid[x], up[x], down[x], mid[x - 1], mid[x + 1]);
            }
        }
    });
}

[[nodiscard]] bool dimensions_overflow(std::uint32_t w, std::uint32_t h) noexcept {
    if (w == 0 || h == 0) {
        return true;
    }
    // Sampling math and clamping run through std::int32_t, so any dimension past INT32_MAX
    // would overflow those casts (and make std::clamp's [lo, hi] invalid). Reject them here.
    constexpr auto max_dim = static_cast<std::uint32_t>(std::numeric_limits<std::int32_t>::max());
    if (w > max_dim || h > max_dim) {
        return true;
    }
    constexpr std::uint64_t max_pixels =
        static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max() / sizeof(Color4));
    return static_cast<std::uint64_t>(w) * static_cast<std::uint64_t>(h) > max_pixels;
}

// Best-effort overlap detection over the actual byte extents each view touches. Assumes the
// dimensions/strides have already been validated. Pointers are compared as integers to avoid
// forming out-of-bounds pointers; this is reliable on the flat address spaces we target.
[[nodiscard]] bool buffers_overlap(ImageView src, MutableImageView dst) noexcept {
    const auto extent = [](std::uintptr_t begin, std::uint32_t w, std::uint32_t h, PixelFormat format,
                           std::ptrdiff_t stride) noexcept -> std::uintptr_t {
        const auto row = static_cast<std::uintptr_t>(stride_or_tight(w, format, stride));
        const auto last_row_bytes = static_cast<std::uintptr_t>(w) * bytes_per_pixel(format);
        return begin + static_cast<std::uintptr_t>(h - 1) * row + last_row_bytes;
    };
    const auto src_begin = reinterpret_cast<std::uintptr_t>(src.pixels);
    const auto dst_begin = reinterpret_cast<std::uintptr_t>(dst.pixels);
    const auto src_end = extent(src_begin, src.width, src.height, src.format, src.stride_bytes);
    const auto dst_end = extent(dst_begin, dst.width, dst.height, dst.format, dst.stride_bytes);
    return src_begin < dst_end && dst_begin < src_end;
}

[[nodiscard]] bool stride_valid(std::uint32_t width, PixelFormat format, std::ptrdiff_t stride) noexcept {
    const auto bpp = bytes_per_pixel(format);
    if (bpp == 0) {
        return false;
    }
    if (stride < 0) {
        return false;
    }
    const auto min_stride = static_cast<std::ptrdiff_t>(width * bpp);
    return stride == 0 || stride >= min_stride;
}

// Reject views whose byte extent cannot be addressed with ptrdiff_t: row_ptr computes
// y * stride in signed arithmetic and buffers_overlap sums the full extent, so an
// oversized stride would silently wrap both. Dimensions and stride minimums are
// already validated when this runs.
[[nodiscard]] bool extent_overflows(std::uint32_t w, std::uint32_t h, PixelFormat format,
                                    std::ptrdiff_t stride) noexcept {
    const auto row = static_cast<std::uint64_t>(stride_or_tight(w, format, stride));
    const auto last_row = static_cast<std::uint64_t>(w) * bytes_per_pixel(format);
    constexpr auto limit = static_cast<std::uint64_t>(std::numeric_limits<std::ptrdiff_t>::max());
    return static_cast<std::uint64_t>(h - 1) > (limit - last_row) / row;
}

} // namespace

std::size_t bytes_per_pixel(PixelFormat format) noexcept {
    switch (format) {
    case PixelFormat::rgba8:
    case PixelFormat::bgra8:
    case PixelFormat::argb8:
    case PixelFormat::abgr8:
    case PixelFormat::xrgb8:
    case PixelFormat::xbgr8:
    case PixelFormat::rgbx8:
    case PixelFormat::bgrx8:
        return 4;
    case PixelFormat::rgb565:
    case PixelFormat::bgr565:
        return 2;
    case PixelFormat::gray8:
        return 1;
    }
    return 0;
}

Status validate(ImageView src, MutableImageView dst) noexcept {
    if (src.pixels == nullptr || dst.pixels == nullptr) {
        return Status::null_pointer;
    }
    if (dimensions_overflow(src.width, src.height) || dimensions_overflow(dst.width, dst.height)) {
        return Status::invalid_size;
    }
    if (bytes_per_pixel(src.format) == 0 || bytes_per_pixel(dst.format) == 0) {
        return Status::unsupported_format;
    }
    if (!stride_valid(src.width, src.format, src.stride_bytes) ||
        !stride_valid(dst.width, dst.format, dst.stride_bytes)) {
        return Status::invalid_stride;
    }
    if (extent_overflows(src.width, src.height, src.format, src.stride_bytes) ||
        extent_overflows(dst.width, dst.height, dst.format, dst.stride_bytes)) {
        return Status::invalid_stride;
    }
    if (buffers_overlap(src, dst)) {
        return Status::same_buffer_unsupported;
    }
    return Status::ok;
}

const char* status_message(Status status) noexcept {
    switch (status) {
    case Status::ok:
        return "ok";
    case Status::null_pointer:
        return "source or destination pixel pointer is null";
    case Status::invalid_size:
        return "image dimensions are zero or too large";
    case Status::invalid_stride:
        return "row stride is smaller than a packed row";
    case Status::unsupported_format:
        return "unsupported pixel format";
    case Status::same_buffer_unsupported:
        return "source and destination buffers overlap or are identical";
    }
    return "unknown status";
}

Status upscale(ImageView src, MutableImageView dst, Options options) {
    if (const auto status = validate(src, dst); status != Status::ok) {
        return status;
    }

    options.sharpening = std::clamp(options.sharpening, 0.0f, 1.0f);
    options.edge_strength = std::max(0.0f, options.edge_strength);

    const float scale_x = static_cast<float>(src.width) / static_cast<float>(dst.width);
    const float scale_y = static_cast<float>(src.height) / static_cast<float>(dst.height);
    const SampleParams params{
        static_cast<float>(dst.width) / static_cast<float>(src.width),
        static_cast<float>(dst.height) / static_cast<float>(src.height),
        std::max(1.0f, scale_x),
        std::max(1.0f, scale_y),
    };

    // Hoisted per-column source coordinates; same expression the per-pixel path used.
    std::vector<float> sx_table(dst.width);
    for (std::uint32_t x = 0; x < dst.width; ++x) {
        sx_table[x] = (static_cast<float>(x) + 0.5f) * scale_x - 0.5f;
    }

    const bool force_opaque = options.alpha == AlphaMode::ignore || !has_alpha(dst.format);
    const std::size_t dst_bpp = bytes_per_pixel(dst.format);
    const auto store_row = [&](std::uint32_t y, auto&& pixel_at) {
        auto* row = row_ptr(dst, static_cast<std::int32_t>(y));
        for (std::uint32_t x = 0; x < dst.width; ++x) {
            const Color4 c = prepare_for_store(pixel_at(x), options, dst.format);
            const float dither = options.dither == Dither::ordered ? bayer4(x, y) : 0.5f;
            store_raw_pixel(row + static_cast<std::ptrdiff_t>(x * dst_bpp), dst.format, c, options.color_space,
                            force_opaque, dither);
        }
    };

    if (options.sharpening <= 0.0f) {
        // No second pass needed: sample straight into the destination and skip the
        // intermediate float image entirely.
        parallel_rows(dst.height, options.thread_count, [&](std::uint32_t y0, std::uint32_t y1) {
            for (std::uint32_t y = y0; y < y1; ++y) {
                const float sy = (static_cast<float>(y) + 0.5f) * scale_y - 0.5f;
                store_row(y, [&](std::uint32_t x) { return sample_filter(src, sx_table[x], sy, params, options); });
            }
        });
        return Status::ok;
    }

    std::vector<Color4> work(static_cast<std::size_t>(dst.width) * static_cast<std::size_t>(dst.height));

    parallel_rows(dst.height, options.thread_count, [&](std::uint32_t y0, std::uint32_t y1) {
        for (std::uint32_t y = y0; y < y1; ++y) {
            const float sy = (static_cast<float>(y) + 0.5f) * scale_y - 0.5f;
            Color4* row = work.data() + static_cast<std::size_t>(y) * dst.width;
            for (std::uint32_t x = 0; x < dst.width; ++x) {
                row[x] = sample_filter(src, sx_table[x], sy, params, options);
            }
        }
    });

    if (options.sharpen == Sharpen::robust_contrast_adaptive) {
        apply_rcas(work, dst.width, dst.height, options);
    } else {
        apply_sharpen(work, dst.width, dst.height, options);
    }

    parallel_rows(dst.height, options.thread_count, [&](std::uint32_t y0, std::uint32_t y1) {
        for (std::uint32_t y = y0; y < y1; ++y) {
            const Color4* row = work.data() + static_cast<std::size_t>(y) * dst.width;
            store_row(y, [&](std::uint32_t x) { return row[x]; });
        }
    });

    return Status::ok;
}

} // namespace retrofsr
