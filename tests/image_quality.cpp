#include "fixtures.hpp"

#include <retrofsr/retrofsr.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <vector>

namespace {

using retrofsr::AlphaMode;
using retrofsr::ColorSpace;
using retrofsr::Dither;
using retrofsr::Filter;
using retrofsr::Options;
using retrofsr::PixelFormat;
using retrofsr::Sharpen;

std::vector<std::byte> upscale_to(const std::vector<std::byte>& src, std::uint32_t sw, std::uint32_t sh,
                                  std::uint32_t dw, std::uint32_t dh, PixelFormat dst_format, Options options) {
    std::vector<std::byte> dst(static_cast<std::size_t>(dw) * dh * retrofsr::bytes_per_pixel(dst_format));
    const retrofsr::ImageView src_view{src.data(), sw, sh, 0, PixelFormat::rgba8};
    const retrofsr::MutableImageView dst_view{dst.data(), dw, dh, 0, dst_format};
    options.thread_count = 1;
    REQUIRE(retrofsr::upscale(src_view, dst_view, options) == retrofsr::Status::ok);
    return dst;
}

// Shallow gray ramp that quantizes to only a handful of 5-bit levels: the banding case.
std::vector<std::byte> shallow_gradient(std::uint32_t w, std::uint32_t h) {
    std::vector<std::byte> src(static_cast<std::size_t>(w) * h * 4);
    for (std::uint32_t y = 0; y < h; ++y) {
        for (std::uint32_t x = 0; x < w; ++x) {
            const std::size_t i = (static_cast<std::size_t>(y) * w + x) * 4;
            const auto v = static_cast<std::uint8_t>(96 + (x * 64u) / (w - 1));
            src[i + 0] = static_cast<std::byte>(v);
            src[i + 1] = static_cast<std::byte>(v);
            src[i + 2] = static_cast<std::byte>(v);
            src[i + 3] = static_cast<std::byte>(255);
        }
    }
    return src;
}

std::vector<std::byte> filled_rgba(std::uint32_t w, std::uint32_t h, std::uint8_t r, std::uint8_t g, std::uint8_t b) {
    std::vector<std::byte> image(static_cast<std::size_t>(w) * h * 4);
    for (std::size_t i = 0; i < image.size(); i += 4) {
        image[i + 0] = static_cast<std::byte>(r);
        image[i + 1] = static_cast<std::byte>(g);
        image[i + 2] = static_cast<std::byte>(b);
        image[i + 3] = static_cast<std::byte>(255);
    }
    return image;
}

[[nodiscard]] int red5_from_565(const std::vector<std::byte>& image, std::size_t pixel_index) {
    const auto lo = static_cast<std::uint8_t>(image[pixel_index * 2]);
    const auto hi = static_cast<std::uint8_t>(image[pixel_index * 2 + 1]);
    const auto packed = static_cast<std::uint16_t>(lo | (hi << 8));
    return (packed >> 11) & 0x1f;
}

} // namespace

TEST_CASE("ordered dithering is deterministic and disabled by default", "[dither]") {
    const auto src = shallow_gradient(64, 8);

    Options options{};
    options.filter = Filter::bilinear;
    options.color_space = ColorSpace::linear;
    options.sharpening = 0.0f;
    REQUIRE(options.dither == Dither::none);

    const auto plain_a = upscale_to(src, 64, 8, 128, 16, PixelFormat::rgb565, options);
    const auto plain_b = upscale_to(src, 64, 8, 128, 16, PixelFormat::rgb565, options);
    REQUIRE(plain_a == plain_b);

    options.dither = Dither::ordered;
    const auto dithered_a = upscale_to(src, 64, 8, 128, 16, PixelFormat::rgb565, options);
    const auto dithered_b = upscale_to(src, 64, 8, 128, 16, PixelFormat::rgb565, options);
    REQUIRE(dithered_a == dithered_b);
    REQUIRE(dithered_a != plain_a);
}

TEST_CASE("ordered dithering reduces banding error on rgb565 gradients", "[dither]") {
    constexpr std::uint32_t dw = 128;
    constexpr std::uint32_t dh = 16;
    const auto src = shallow_gradient(64, 8);

    Options options{};
    options.filter = Filter::bilinear;
    options.color_space = ColorSpace::linear;
    options.sharpening = 0.0f;

    // 8-bit output as the near-continuous reference for the same resample.
    const auto reference = upscale_to(src, 64, 8, dw, dh, PixelFormat::rgba8, options);
    const auto plain = upscale_to(src, 64, 8, dw, dh, PixelFormat::rgb565, options);
    options.dither = Dither::ordered;
    const auto dithered = upscale_to(src, 64, 8, dw, dh, PixelFormat::rgb565, options);

    // Compare 4x4 block averages of the red channel: dithering must not lose DC accuracy,
    // and on a shallow ramp it should recover precision the 5-bit grid throws away.
    double plain_error = 0.0;
    double dithered_error = 0.0;
    for (std::uint32_t by = 0; by < dh; by += 4) {
        for (std::uint32_t bx = 0; bx < dw; bx += 4) {
            double ref_sum = 0.0;
            double plain_sum = 0.0;
            double dith_sum = 0.0;
            for (std::uint32_t y = by; y < by + 4; ++y) {
                for (std::uint32_t x = bx; x < bx + 4; ++x) {
                    const std::size_t p = static_cast<std::size_t>(y) * dw + x;
                    ref_sum += static_cast<double>(static_cast<std::uint8_t>(reference[p * 4]));
                    // Expand 5-bit red back to 8-bit the same way the loader does.
                    const int r5p = red5_from_565(plain, p);
                    const int r5d = red5_from_565(dithered, p);
                    plain_sum += (r5p << 3) | (r5p >> 2);
                    dith_sum += (r5d << 3) | (r5d >> 2);
                }
            }
            plain_error += std::abs(plain_sum - ref_sum);
            dithered_error += std::abs(dith_sum - ref_sum);
        }
    }
    REQUIRE(dithered_error < plain_error);
}

TEST_CASE("alpha-aware sharpening keeps premultiplied output valid", "[alpha]") {
    // Opaque white square on fully transparent black: valid premultiplied input.
    constexpr std::uint32_t sw = 12;
    constexpr std::uint32_t sh = 12;
    std::vector<std::byte> src(static_cast<std::size_t>(sw) * sh * 4);
    for (std::uint32_t y = 0; y < sh; ++y) {
        for (std::uint32_t x = 0; x < sw; ++x) {
            const std::size_t i = (static_cast<std::size_t>(y) * sw + x) * 4;
            const bool inside = x >= 3 && x < 9 && y >= 3 && y < 9;
            const std::uint8_t v = inside ? 255 : 0;
            src[i + 0] = static_cast<std::byte>(v);
            src[i + 1] = static_cast<std::byte>(v);
            src[i + 2] = static_cast<std::byte>(v);
            src[i + 3] = static_cast<std::byte>(v);
        }
    }

    for (const auto sharpen : {Sharpen::contrast_adaptive, Sharpen::robust_contrast_adaptive}) {
        CAPTURE(static_cast<int>(sharpen));

        Options options{};
        options.filter = Filter::bilinear;
        options.color_space = ColorSpace::linear;
        options.alpha = AlphaMode::premultiplied;
        options.sharpen = sharpen;
        options.sharpening = 1.0f;
        REQUIRE(options.alpha_aware_sharpening);

        const auto aware = upscale_to(src, sw, sh, 29, 27, PixelFormat::rgba8, options);
        for (std::size_t p = 0; p < aware.size(); p += 4) {
            const int alpha = static_cast<std::uint8_t>(aware[p + 3]);
            for (std::size_t ch = 0; ch < 3; ++ch) {
                const int v = static_cast<std::uint8_t>(aware[p + ch]);
                CAPTURE(p, ch, v, alpha);
                // The invariant holds exactly: filtering weights RGB and alpha identically,
                // the alpha-aware cap clamps in float, and quantization is monotonic.
                REQUIRE(v <= alpha);
            }
        }

        // The control must actually change behavior around the transparent boundary.
        options.alpha_aware_sharpening = false;
        const auto unaware = upscale_to(src, sw, sh, 29, 27, PixelFormat::rgba8, options);
        REQUIRE(aware != unaware);
    }
}

TEST_CASE("straight-alpha sharpening does not leak hidden background color", "[alpha]") {
    // Opaque red square over fully transparent green: the green must never appear.
    constexpr std::uint32_t sw = 12;
    constexpr std::uint32_t sh = 12;
    std::vector<std::byte> src(static_cast<std::size_t>(sw) * sh * 4);
    for (std::uint32_t y = 0; y < sh; ++y) {
        for (std::uint32_t x = 0; x < sw; ++x) {
            const std::size_t i = (static_cast<std::size_t>(y) * sw + x) * 4;
            const bool inside = x >= 3 && x < 9 && y >= 3 && y < 9;
            src[i + 0] = static_cast<std::byte>(inside ? 255 : 0);
            src[i + 1] = static_cast<std::byte>(inside ? 0 : 255);
            src[i + 2] = static_cast<std::byte>(0);
            src[i + 3] = static_cast<std::byte>(inside ? 255 : 0);
        }
    }

    Options options{};
    options.filter = Filter::easu;
    options.color_space = ColorSpace::linear;
    options.alpha = AlphaMode::straight;
    options.sharpen = Sharpen::robust_contrast_adaptive;
    options.sharpening = 1.0f;

    const auto out = upscale_to(src, sw, sh, 29, 27, PixelFormat::rgba8, options);
    for (std::size_t p = 0; p < out.size(); p += 4) {
        const int green = static_cast<std::uint8_t>(out[p + 1]);
        const int alpha = static_cast<std::uint8_t>(out[p + 3]);
        CAPTURE(p, green, alpha);
        // Green energy is exactly zero after internal premultiplication; allow one
        // quantization step of paranoia only.
        REQUIRE(green <= 1);
        if (alpha == 0) {
            REQUIRE(static_cast<std::uint8_t>(out[p + 0]) == 0);
            REQUIRE(static_cast<std::uint8_t>(out[p + 2]) == 0);
        }
    }
}

TEST_CASE("easu reconstructs hard axis-aligned edges monotonically", "[quality][easu]") {
    // Regression for the anisotropy defect: tightening the kernel across the gradient
    // pushed every tap of a hard edge into the negative lobe, inverting tap weights and
    // producing a reversed bright/dark fringe (profile like 0, 255, 0, 255).
    constexpr std::uint32_t sw = 16;
    constexpr std::uint32_t sh = 16;
    constexpr std::uint32_t dw = 48;
    constexpr std::uint32_t dh = 48;

    for (const bool vertical : {true, false}) {
        CAPTURE(vertical);
        std::vector<std::byte> src(static_cast<std::size_t>(sw) * sh * 4);
        for (std::uint32_t y = 0; y < sh; ++y) {
            for (std::uint32_t x = 0; x < sw; ++x) {
                const std::size_t i = (static_cast<std::size_t>(y) * sw + x) * 4;
                const std::uint8_t v = (vertical ? x : y) < sw / 2 ? 0 : 255;
                src[i + 0] = static_cast<std::byte>(v);
                src[i + 1] = static_cast<std::byte>(v);
                src[i + 2] = static_cast<std::byte>(v);
                src[i + 3] = static_cast<std::byte>(255);
            }
        }

        Options options{};
        options.filter = Filter::easu;
        options.color_space = ColorSpace::linear;
        options.sharpening = 0.0f;
        const auto out = upscale_to(src, sw, sh, dw, dh, PixelFormat::rgba8, options);

        // Every row (or column) profile crossing the edge must be non-decreasing.
        for (std::uint32_t line = 0; line < (vertical ? dh : dw); ++line) {
            int prev = -1;
            for (std::uint32_t step = 0; step < (vertical ? dw : dh); ++step) {
                const std::uint32_t x = vertical ? step : line;
                const std::uint32_t y = vertical ? line : step;
                const int v = static_cast<std::uint8_t>(out[(static_cast<std::size_t>(y) * dw + x) * 4]);
                CAPTURE(line, step, prev, v);
                REQUIRE(v >= prev);
                prev = v;
            }
        }
    }
}

TEST_CASE("gray8 output supports deterministic ordered dithering", "[dither][formats]") {
    const auto src = shallow_gradient(64, 8);

    Options options{};
    options.filter = Filter::bilinear;
    options.color_space = ColorSpace::linear;
    options.sharpening = 0.0f;

    const auto plain_a = upscale_to(src, 64, 8, 128, 16, PixelFormat::gray8, options);
    const auto plain_b = upscale_to(src, 64, 8, 128, 16, PixelFormat::gray8, options);
    REQUIRE(plain_a == plain_b);

    options.dither = Dither::ordered;
    const auto dithered_a = upscale_to(src, 64, 8, 128, 16, PixelFormat::gray8, options);
    const auto dithered_b = upscale_to(src, 64, 8, 128, 16, PixelFormat::gray8, options);
    REQUIRE(dithered_a == dithered_b);
    REQUIRE(dithered_a != plain_a);
}

TEST_CASE("every pixel format upscales end-to-end", "[formats]") {
    constexpr PixelFormat formats[] = {
        PixelFormat::rgba8,  PixelFormat::bgra8,  PixelFormat::argb8, PixelFormat::abgr8,
        PixelFormat::xrgb8,  PixelFormat::xbgr8,  PixelFormat::rgbx8, PixelFormat::bgrx8,
        PixelFormat::rgb565, PixelFormat::bgr565, PixelFormat::gray8,
    };
    constexpr std::uint32_t sw = 8;
    constexpr std::uint32_t sh = 6;
    constexpr std::uint32_t dw = 19;
    constexpr std::uint32_t dh = 13;

    for (const auto format : formats) {
        CAPTURE(static_cast<int>(format));
        const std::size_t bpp = retrofsr::bytes_per_pixel(format);
        std::vector<std::byte> src(static_cast<std::size_t>(sw) * sh * bpp);
        for (std::size_t i = 0; i < src.size(); ++i) {
            src[i] = static_cast<std::byte>((i * 31 + 7) & 0xff);
        }
        std::vector<std::byte> dst(static_cast<std::size_t>(dw) * dh * bpp);

        const retrofsr::ImageView src_view{src.data(), sw, sh, 0, format};
        const retrofsr::MutableImageView dst_view{dst.data(), dw, dh, 0, format};
        Options options{};
        options.filter = Filter::easu;
        options.thread_count = 1;
        REQUIRE(retrofsr::upscale(src_view, dst_view, options) == retrofsr::Status::ok);

        bool any_nonzero = false;
        for (const std::byte b : dst) {
            if (static_cast<std::uint8_t>(b) != 0) {
                any_nonzero = true;
                break;
            }
        }
        REQUIRE(any_nonzero);
    }
}

TEST_CASE("multithreaded upscaling matches single-threaded output exactly", "[threads]") {
    // dst height must be >= 32 or parallel_rows short-circuits to a single thread.
    const auto src = fixtures::make_rgba(fixtures::Pattern::edges, 24, 40);

    for (const auto sharpen : {Sharpen::contrast_adaptive, Sharpen::robust_contrast_adaptive}) {
        CAPTURE(static_cast<int>(sharpen));
        Options options{};
        options.filter = Filter::easu;
        options.sharpen = sharpen;
        options.sharpening = 0.30f;

        options.thread_count = 1;
        const auto single = upscale_to(src, 24, 40, 64, 96, PixelFormat::rgba8, options);
        options.thread_count = 4;
        const auto four = upscale_to(src, 24, 40, 64, 96, PixelFormat::rgba8, options);
        options.thread_count = 0;
        const auto automatic = upscale_to(src, 24, 40, 64, 96, PixelFormat::rgba8, options);

        REQUIRE(single == four);
        REQUIRE(single == automatic);
    }
}

TEST_CASE("easu handles edge_strength extremes within source bounds", "[quality][easu]") {
    const auto src = fixtures::make_rgba(fixtures::Pattern::edges, 16, 12);

    for (const float edge_strength : {0.0f, 4.0f}) {
        CAPTURE(edge_strength);
        Options options{};
        options.filter = Filter::easu;
        options.color_space = ColorSpace::linear;
        options.sharpening = 0.0f;
        options.edge_strength = edge_strength;
        const auto out = upscale_to(src, 16, 12, 41, 29, PixelFormat::rgba8, options);

        // The edges fixture only contains 25 and 230; anti-ringing must hold at both
        // an isotropic (0) and an over-range (4) edge strength.
        for (std::size_t i = 0; i < out.size(); ++i) {
            if (i % 4 == 3) {
                continue;
            }
            const auto v = static_cast<std::uint8_t>(out[i]);
            CAPTURE(i, static_cast<int>(v));
            REQUIRE(v >= 24);
            REQUIRE(v <= 231);
        }
    }
}

TEST_CASE("downscaling is anti-aliased for every widened filter", "[downscale]") {
    // A checkerboard downscaled 2:1 and 3:1 must converge to its mid-gray mean when the
    // kernel is widened; point sampling would return only 0 or 255.
    constexpr std::uint32_t sw = 36;
    constexpr std::uint32_t sh = 36;
    const auto src = fixtures::make_rgba(fixtures::Pattern::checkerboard, sw, sh);

    for (const auto filter : {Filter::bilinear, Filter::sharp_bilinear, Filter::catmull_rom, Filter::lanczos3,
                              Filter::edge_adaptive_lanczos, Filter::easu}) {
        for (const std::uint32_t dw : {18u, 12u}) {
            CAPTURE(static_cast<int>(filter), dw);

            Options options{};
            options.filter = filter;
            options.color_space = ColorSpace::linear;
            options.sharpening = 0.0f;
            const auto out = upscale_to(src, sw, sh, dw, dw, PixelFormat::rgba8, options);

            for (std::size_t i = 0; i < out.size(); ++i) {
                if (i % 4 == 3) {
                    continue;
                }
                const int v = static_cast<std::uint8_t>(out[i]);
                CAPTURE(i, v);
                REQUIRE(v >= 96);
                REQUIRE(v <= 160);
            }
        }
    }

    // Contrast case: nearest stays point-sampled by design, so it must NOT converge.
    Options nearest{};
    nearest.filter = Filter::nearest;
    nearest.color_space = ColorSpace::linear;
    nearest.sharpening = 0.0f;
    const auto aliased = upscale_to(src, sw, sh, 18, 18, PixelFormat::rgba8, nearest);
    bool any_extreme = false;
    for (std::size_t i = 0; i < aliased.size(); i += 4) {
        const int v = static_cast<std::uint8_t>(aliased[i]);
        if (v < 96 || v > 160) {
            any_extreme = true;
            break;
        }
    }
    REQUIRE(any_extreme);
}

TEST_CASE("flat input stays flat when downscaling", "[downscale]") {
    const auto src = filled_rgba(32, 24, 180, 90, 45);
    const std::uint8_t expected[4] = {180, 90, 45, 255};

    for (const auto filter : {Filter::nearest, Filter::bilinear, Filter::sharp_bilinear, Filter::catmull_rom,
                              Filter::lanczos3, Filter::edge_adaptive_lanczos, Filter::easu}) {
        for (const auto color_space : {ColorSpace::srgb, ColorSpace::linear, ColorSpace::perceptual}) {
            CAPTURE(static_cast<int>(filter), static_cast<int>(color_space));

            Options options{};
            options.filter = filter;
            options.color_space = color_space;
            options.sharpening = 0.20f;
            const auto out = upscale_to(src, 32, 24, 13, 9, PixelFormat::rgba8, options);

            for (std::size_t i = 0; i < out.size(); ++i) {
                const int diff = std::abs(static_cast<int>(static_cast<std::uint8_t>(out[i])) - expected[i % 4]);
                CAPTURE(i);
                REQUIRE(diff <= 1);
            }
        }
    }
}

TEST_CASE("mixed-axis resize keeps hard edges monotonic", "[downscale][easu]") {
    // Vertical black|white edge; x upscales 2.5x while y downscales 2x.
    constexpr std::uint32_t sw = 16;
    constexpr std::uint32_t sh = 40;
    constexpr std::uint32_t dw = 40;
    constexpr std::uint32_t dh = 20;
    std::vector<std::byte> src(static_cast<std::size_t>(sw) * sh * 4);
    for (std::uint32_t y = 0; y < sh; ++y) {
        for (std::uint32_t x = 0; x < sw; ++x) {
            const std::size_t i = (static_cast<std::size_t>(y) * sw + x) * 4;
            const std::uint8_t v = x < sw / 2 ? 0 : 255;
            src[i + 0] = static_cast<std::byte>(v);
            src[i + 1] = static_cast<std::byte>(v);
            src[i + 2] = static_cast<std::byte>(v);
            src[i + 3] = static_cast<std::byte>(255);
        }
    }

    // Only filters with monotone step responses: easu (anti-ring clamped) and bilinear
    // (non-negative weights). lanczos3 rings at step edges by design and is excluded.
    for (const auto filter : {Filter::easu, Filter::bilinear}) {
        CAPTURE(static_cast<int>(filter));
        Options options{};
        options.filter = filter;
        options.color_space = ColorSpace::linear;
        options.sharpening = 0.0f;
        const auto out = upscale_to(src, sw, sh, dw, dh, PixelFormat::rgba8, options);

        // Allow one code point of wiggle for quantization.
        for (std::uint32_t y = 0; y < dh; ++y) {
            int prev = -2;
            for (std::uint32_t x = 0; x < dw; ++x) {
                const int v = static_cast<std::uint8_t>(out[(static_cast<std::size_t>(y) * dw + x) * 4]);
                CAPTURE(y, x, prev, v);
                REQUIRE(v >= prev - 1);
                prev = v;
            }
        }
    }
}

TEST_CASE("generated fixtures upscale cleanly with every filter", "[fixtures]") {
    for (const auto pattern : fixtures::patterns) {
        const auto src = fixtures::make_rgba(pattern, 16, 12);
        for (const auto filter : {Filter::nearest, Filter::bilinear, Filter::sharp_bilinear, Filter::catmull_rom,
                                  Filter::lanczos3, Filter::edge_adaptive_lanczos, Filter::easu}) {
            CAPTURE(static_cast<int>(pattern), static_cast<int>(filter));

            Options options{};
            options.filter = filter;
            options.alpha = pattern == fixtures::Pattern::transparency ? AlphaMode::straight : AlphaMode::ignore;
            const auto out = upscale_to(src, 16, 12, 37, 25, PixelFormat::rgba8, options);

            bool any_nonzero = false;
            for (const std::byte b : out) {
                if (static_cast<std::uint8_t>(b) != 0) {
                    any_nonzero = true;
                    break;
                }
            }
            REQUIRE(any_nonzero);
        }
    }
}
