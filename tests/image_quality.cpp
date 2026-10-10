#include "fixtures.hpp"

#include <retrofsr/retrofsr.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <utility>
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

[[nodiscard]] std::uint32_t red5_from_565(const std::vector<std::byte>& image, std::size_t pixel_index) {
    const auto lo = static_cast<std::uint8_t>(image[pixel_index * 2]);
    const auto hi = static_cast<std::uint8_t>(image[pixel_index * 2 + 1]);
    const auto packed = static_cast<std::uint32_t>(lo) | (static_cast<std::uint32_t>(hi) << 8u);
    return (packed >> 11u) & 0x1fu;
}

} // namespace

TEST_CASE("sharpened output respects padded rows and narrow image borders", "[sharpen][stride]") {
    const auto src = shallow_gradient(8, 7);
    for (const auto sharpen : {Sharpen::contrast_adaptive, Sharpen::robust_contrast_adaptive}) {
        for (const auto format : {PixelFormat::rgba8, PixelFormat::bgra8, PixelFormat::argb8, PixelFormat::abgr8,
                                  PixelFormat::xrgb8, PixelFormat::xbgr8, PixelFormat::rgbx8, PixelFormat::bgrx8,
                                  PixelFormat::rgb565, PixelFormat::bgr565, PixelFormat::gray8}) {
            for (const auto [dw, dh] : {std::pair{1u, 1u},
                                        {1u, 35u},
                                        {2u, 35u},
                                        {35u, 1u},
                                        {13u, 37u},
                                        {63u, 35u},
                                        {64u, 35u},
                                        {65u, 35u},
                                        {127u, 35u},
                                        {128u, 35u},
                                        {129u, 37u}}) {
                CAPTURE(static_cast<int>(sharpen), static_cast<int>(format), dw, dh);
                Options options = retrofsr::fsr1_style_options();
                options.sharpen = sharpen;
                options.sharpening = 0.7f;
                options.dither = Dither::ordered;
                options.thread_count = 1;
                const auto packed = upscale_to(src, 8, 7, dw, dh, format, options);

                const std::size_t row_bytes = dw * retrofsr::bytes_per_pixel(format);
                const std::size_t stride = row_bytes + 7;
                constexpr std::size_t guard = 19;
                constexpr std::byte sentinel{0xa5};
                std::vector<std::byte> padded(guard + stride * dh + guard, sentinel);
                const retrofsr::ImageView src_view{src.data(), 8, 7, 0, PixelFormat::rgba8};
                const retrofsr::MutableImageView dst_view{padded.data() + guard, dw, dh,
                                                          static_cast<std::ptrdiff_t>(stride), format};
                options.thread_count = 4;
                REQUIRE(retrofsr::upscale(src_view, dst_view, options) == retrofsr::Status::ok);

                // Compare the entire allocation, including untouched row padding and guards.
                std::vector<std::byte> expected(padded.size(), sentinel);
                for (std::uint32_t y = 0; y < dh; ++y) {
                    std::copy_n(packed.data() + y * row_bytes, row_bytes, expected.data() + guard + y * stride);
                }
                REQUIRE(padded == expected);
            }
        }
    }
}

TEST_CASE("CAS preserves an affine ramp across block boundaries", "[sharpen]") {
    constexpr std::uint32_t width = 129;
    constexpr std::uint32_t height = 35;
    auto src = filled_rgba(width, height, 0, 0, 0);
    for (std::uint32_t y = 0; y < height; ++y) {
        for (std::uint32_t x = 0; x < width; ++x) {
            const auto i = (static_cast<std::size_t>(y) * width + x) * 4;
            src[i] = static_cast<std::byte>(x + 7);
            src[i + 1] = static_cast<std::byte>(255 - x);
            src[i + 2] = static_cast<std::byte>(y * 5);
        }
    }
    Options options{};
    options.filter = Filter::nearest;
    options.color_space = ColorSpace::linear;
    options.sharpen = Sharpen::contrast_adaptive;
    options.sharpening = 1.0f;
    options.thread_count = 4;
    const auto out = upscale_to(src, width, height, width, height, PixelFormat::rgba8, options);
    // An affine field has no interior detail to sharpen. Exclude clamped image borders.
    for (std::uint32_t y = 1; y + 1 < height; ++y) {
        for (std::uint32_t x = 1; x + 1 < width; ++x) {
            CAPTURE(x, y);
            const auto i = (static_cast<std::size_t>(y) * width + x) * 4;
            for (std::size_t channel = 0; channel < 4; ++channel) {
                REQUIRE(out[i + channel] == src[i + channel]);
            }
        }
    }
}

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
                    const auto r5p = red5_from_565(plain, p);
                    const auto r5d = red5_from_565(dithered, p);
                    plain_sum += (r5p << 3u) | (r5p >> 2u);
                    dith_sum += (r5d << 3u) | (r5d >> 2u);
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
            src[i] = static_cast<std::byte>((i * 31 + 7) & 0xffu);
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

TEST_CASE("spatial research controls are opt-in and scoped to their algorithms", "[quality]") {
    for (const auto options :
         {Options{}, retrofsr::retro_pixel_options(), retrofsr::fsr_like_options(), retrofsr::fsr1_style_options()}) {
        REQUIRE(options.rcas_noise_suppression == false);
        REQUIRE(options.easu_edge_coherence == false);
    }
    const auto src = fixtures::make_rgba(fixtures::Pattern::noise, 12, 12);
    Options options{};
    options.filter = Filter::bilinear;
    options.thread_count = 1;
    const auto baseline = upscale_to(src, 12, 12, 24, 24, PixelFormat::rgba8, options);
    options.rcas_noise_suppression = true;
    options.easu_edge_coherence = true;
    REQUIRE(upscale_to(src, 12, 12, 24, 24, PixelFormat::rgba8, options) == baseline);

    // EASU's widened isotropic minification path must remain unchanged.
    options.filter = Filter::easu;
    options.sharpening = 0.0f;
    const auto down = upscale_to(src, 12, 12, 6, 6, PixelFormat::rgba8, options);
    options.easu_edge_coherence = false;
    REQUIRE(upscale_to(src, 12, 12, 6, 6, PixelFormat::rgba8, options) == down);
}

TEST_CASE("RCAS noise suppression reduces amplification and preserves flat fields", "[quality][rcas]") {
    auto src = fixtures::make_rgba(fixtures::Pattern::noise, 32, 32);
    for (std::size_t p = 0; p < src.size(); p += 4) {
        const auto v = static_cast<std::byte>(118 + static_cast<unsigned>(src[p]) % 21);
        src[p] = src[p + 1] = src[p + 2] = v;
    }
    for (const auto cs : {ColorSpace::linear, ColorSpace::perceptual, ColorSpace::srgb}) {
        CAPTURE(static_cast<int>(cs));
        auto options = retrofsr::fsr1_style_options();
        options.filter = Filter::nearest; // Isolate sharpening from reconstruction.
        options.color_space = cs;
        options.sharpening = 1.0f;
        options.thread_count = 1;
        const auto plain = upscale_to(src, 32, 32, 32, 32, PixelFormat::rgba8, options);
        options.rcas_noise_suppression = true;
        const auto suppressed = upscale_to(src, 32, 32, 32, 32, PixelFormat::rgba8, options);
        double input_error = 0.0;
        double plain_error = 0.0;
        double suppressed_error = 0.0;
        for (std::size_t p = 0; p < src.size(); p += 4) {
            const double input = static_cast<int>(src[p]) - 128;
            input_error += input * input;
            const double a = static_cast<int>(plain[p]) - 128;
            const double b = static_cast<int>(suppressed[p]) - 128;
            plain_error += a * a;
            suppressed_error += b * b;
        }
        REQUIRE(suppressed_error < plain_error);
        REQUIRE(suppressed_error > input_error); // Sharpening still operates; not a bypass.

        options.filter = Filter::easu;
        options.easu_edge_coherence = true;
        for (const auto value : {0, 128, 255}) {
            const auto flat = filled_rgba(8, 8, value, value, value);
            const auto result = upscale_to(flat, 8, 8, 13, 15, PixelFormat::rgba8, options);
            for (std::size_t p = 0; p < result.size(); p += 4) {
                REQUIRE(std::abs(static_cast<int>(result[p]) - value) <= 1);
                REQUIRE(result[p + 3] == std::byte{255});
            }
        }
    }
}

TEST_CASE("EASU coherence changes corners while preserving an aligned linear ramp", "[quality][easu]") {
    auto options = retrofsr::fsr1_style_options();
    options.sharpening = 0.0f;
    options.thread_count = 1;
    const auto corner = fixtures::make_motion_rgba(fixtures::MotionPattern::corner, 32, 32, 0.0f);
    const auto legacy = upscale_to(corner, 32, 32, 48, 48, PixelFormat::rgba8, options);
    options.easu_edge_coherence = true;
    const auto coherent = upscale_to(corner, 32, 32, 48, 48, PixelFormat::rgba8, options);
    REQUIRE(coherent != legacy);
    for (std::size_t p = 0; p < coherent.size(); p += 4) {
        REQUIRE(static_cast<int>(coherent[p]) >= 32);
        REQUIRE(static_cast<int>(coherent[p]) <= 224);
    }

    auto ramp = filled_rgba(16, 16, 0, 0, 0);
    for (unsigned y = 0; y < 16; ++y) {
        for (unsigned x = 0; x < 16; ++x) {
            const std::size_t p = (static_cast<std::size_t>(y) * 16 + x) * 4;
            ramp[p] = ramp[p + 1] = ramp[p + 2] = static_cast<std::byte>(32 + 4 * x + 4 * y);
        }
    }
    const auto adapted = upscale_to(ramp, 16, 16, 32, 32, PixelFormat::rgba8, options);
    options.easu_edge_coherence = false;
    const auto original = upscale_to(ramp, 16, 16, 32, 32, PixelFormat::rgba8, options);
    // Exclude clamped borders, where the ramp's gradient is no longer uniform.
    for (unsigned y = 4; y < 28; ++y) {
        for (unsigned x = 4; x < 28; ++x) {
            const auto p = (y * 32 + x) * 4;
            REQUIRE(std::abs(static_cast<int>(adapted[p]) - static_cast<int>(original[p])) <= 1);
        }
    }
}

TEST_CASE("experimental spatial paths preserve alpha and threaded determinism", "[quality][alpha][threads]") {
    for (const auto alpha : {AlphaMode::straight, AlphaMode::premultiplied}) {
        auto src = fixtures::make_rgba(fixtures::Pattern::transparency, 24, 24);
        if (alpha == AlphaMode::premultiplied) {
            for (std::size_t p = 0; p < src.size(); p += 4) {
                for (std::size_t ch = 0; ch < 3; ++ch) {
                    src[p + ch] = static_cast<std::byte>(static_cast<unsigned>(src[p + ch]) *
                                                         static_cast<unsigned>(src[p + 3]) / 255u);
                }
            }
        }
        for (const bool noise : {false, true}) {
            for (const bool coherence : {false, true}) {
                auto options = retrofsr::fsr1_style_options();
                options.alpha = alpha;
                options.rcas_noise_suppression = noise;
                options.easu_edge_coherence = coherence;
                options.sharpening = 1.0f;
                options.thread_count = 1;
                const auto single = upscale_to(src, 24, 24, 48, 48, PixelFormat::rgba8, options);
                for (const auto threads : {0u, 4u}) {
                    options.thread_count = threads;
                    REQUIRE(upscale_to(src, 24, 24, 48, 48, PixelFormat::rgba8, options) == single);
                }
                for (std::size_t p = 0; p < single.size(); p += 4) {
                    for (std::size_t ch = 0; ch < 3; ++ch) {
                        if (alpha == AlphaMode::premultiplied) {
                            REQUIRE(single[p + ch] <= single[p + 3]);
                        }
                        if (single[p + 3] == std::byte{0}) {
                            REQUIRE(single[p + ch] == std::byte{0});
                        }
                    }
                }
            }
        }
    }
}

TEST_CASE("moving scenes stay within spatial and temporal error budgets", "[quality][motion]") {
    // Independently integrate the analytic scene at input and output resolutions.
    // Temporal error compares changes in reconstruction error against the moving
    // reference, not consecutive raw frames (which would penalize legitimate motion).
    for (const auto pattern : fixtures::motion_patterns) {
        for (const unsigned extent : {48u, 64u, 96u}) {
            CAPTURE(static_cast<int>(pattern), extent);
            double spatial[2] = {};
            double temporal[2] = {};
            std::vector<int> previous_error[2];
            std::size_t spatial_samples = 0;
            std::size_t temporal_samples = 0;
            for (unsigned frame = 0; frame < 8; ++frame) {
                const float offset = static_cast<float>(frame) * 0.25f;
                const auto src = fixtures::make_motion_rgba(pattern, 32, 32, offset);
                const auto ref = fixtures::make_motion_rgba(pattern, extent, extent, offset, 32.0f / extent);
                for (unsigned mode = 0; mode < 2; ++mode) {
                    auto options = retrofsr::fsr1_style_options();
                    options.alpha = AlphaMode::premultiplied;
                    options.thread_count = 1;
                    options.rcas_noise_suppression = mode != 0;
                    options.easu_edge_coherence = mode != 0;
                    const auto out = upscale_to(src, 32, 32, extent, extent, PixelFormat::rgba8, options);
                    std::vector<int> errors;
                    for (unsigned y = 8; y < extent - 8; ++y) {
                        for (unsigned x = 8; x < extent - 8; ++x) {
                            const std::size_t p = (static_cast<std::size_t>(y) * extent + x) * 4;
                            const int error = static_cast<int>(out[p]) - static_cast<int>(ref[p]);
                            spatial[mode] += std::abs(error);
                            if (frame > 0) {
                                temporal[mode] += std::abs(error - previous_error[mode][errors.size()]);
                            }
                            errors.push_back(error);
                        }
                    }
                    if (mode == 0) {
                        spatial_samples += errors.size();
                        if (frame > 0) {
                            temporal_samples += errors.size();
                        }
                    }
                    previous_error[mode] = std::move(errors);
                }
            }
            const double baseline_spatial = spatial[0] / spatial_samples;
            const double experimental_spatial = spatial[1] / spatial_samples;
            const double baseline_temporal = temporal[0] / temporal_samples;
            const double experimental_temporal = temporal[1] / temporal_samples;
            CAPTURE(baseline_spatial, experimental_spatial, baseline_temporal, experimental_temporal);
            // Experiment gate: <=5% regression plus half an 8-bit code value for
            // quantization. These are synthetic guardrails, not a perceptual verdict.
            REQUIRE(experimental_spatial <= baseline_spatial * 1.05 + 0.5);
            REQUIRE(experimental_temporal <= baseline_temporal * 1.05 + 0.5);
        }
    }
}
