#include "golden_data.hpp"
#include "golden_support.hpp"

#include <retrofsr/retrofsr.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <vector>

namespace {

// Absorbs libm/rounding differences between toolchains without hiding real image changes.
constexpr int golden_tolerance = 2;

static_assert(golden::configs.size() == golden::expected.size(), "regenerate tests/golden_data.hpp: just golden");

using retrofsr::ColorSpace;
using retrofsr::Extent;
using retrofsr::Filter;
using retrofsr::Options;
using retrofsr::QualityPreset;
using retrofsr::Sharpen;

static_assert(retrofsr::quality_preset_scale(QualityPreset::quality) == 1.5f);
static_assert(retrofsr::quality_preset_scale(QualityPreset::balanced) == 1.7f);
static_assert(retrofsr::quality_preset_scale(QualityPreset::performance) == 2.0f);
static_assert(retrofsr::quality_preset_scale(QualityPreset::ultra_performance) == 3.0f);

constexpr Extent quality_1080 = retrofsr::quality_preset_source_size(QualityPreset::quality, 1920, 1080);
static_assert(quality_1080.width == 1280 && quality_1080.height == 720);
constexpr Extent balanced_720 = retrofsr::quality_preset_source_size(QualityPreset::balanced, 1280, 720);
static_assert(balanced_720.width == 753 && balanced_720.height == 424);
constexpr Extent performance_1080 = retrofsr::quality_preset_source_size(QualityPreset::performance, 1920, 1080);
static_assert(performance_1080.width == 960 && performance_1080.height == 540);
constexpr Extent ultra_tiny = retrofsr::quality_preset_source_size(QualityPreset::ultra_performance, 1, 1);
static_assert(ultra_tiny.width == 1 && ultra_tiny.height == 1);
constexpr Extent zero = retrofsr::quality_preset_source_size(QualityPreset::quality, 0, 0);
static_assert(zero.width == 0 && zero.height == 0);

constexpr Options fsr1_defaults = retrofsr::fsr1_style_options();
static_assert(fsr1_defaults.filter == Filter::easu);
static_assert(fsr1_defaults.color_space == ColorSpace::perceptual);
static_assert(fsr1_defaults.sharpen == Sharpen::robust_contrast_adaptive);
static_assert(retrofsr::fsr1_style_options(QualityPreset::ultra_performance).sharpening == 0.35f);

std::vector<std::byte> upscale_rgba(const std::vector<std::byte>& src, std::uint32_t sw, std::uint32_t sh,
                                    std::uint32_t dw, std::uint32_t dh, Options options) {
    std::vector<std::byte> dst(static_cast<std::size_t>(dw) * dh * 4);
    const retrofsr::ImageView src_view{src.data(), sw, sh, 0, retrofsr::PixelFormat::rgba8};
    const retrofsr::MutableImageView dst_view{dst.data(), dw, dh, 0, retrofsr::PixelFormat::rgba8};
    options.thread_count = 1;
    REQUIRE(retrofsr::upscale(src_view, dst_view, options) == retrofsr::Status::ok);
    return dst;
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

} // namespace

TEST_CASE("upscale matches golden images for all filters and color spaces", "[golden]") {
    const auto src = golden::make_source();
    for (std::size_t case_index = 0; case_index < golden::configs.size(); ++case_index) {
        const auto& config = golden::configs[case_index];
        CAPTURE(case_index, static_cast<int>(config.filter), static_cast<int>(config.color_space),
                static_cast<int>(config.sharpen));

        const auto out = golden::render(config, src);
        REQUIRE(out.size() == golden::expected[case_index].size());

        int worst_diff = 0;
        std::size_t worst_index = 0;
        std::size_t total_diff = 0;
        for (std::size_t i = 0; i < out.size(); ++i) {
            const int diff = std::abs(static_cast<int>(static_cast<std::uint8_t>(out[i])) -
                                      static_cast<int>(golden::expected[case_index][i]));
            total_diff += static_cast<std::size_t>(diff);
            if (diff > worst_diff) {
                worst_diff = diff;
                worst_index = i;
            }
        }
        CAPTURE(worst_index);
        REQUIRE(worst_diff <= golden_tolerance);
        // Libm scatter is a handful of isolated +/-1s; a systematic shift of every byte
        // (a real image change) fails this even when it sneaks under the per-byte bound.
        const double mean_diff = static_cast<double>(total_diff) / static_cast<double>(out.size());
        CAPTURE(mean_diff);
        REQUIRE(mean_diff <= 0.25);
    }
}

TEST_CASE("flat input stays flat for every filter, color space, and sharpener", "[quality]") {
    const auto src = filled_rgba(6, 6, 180, 90, 45);
    const std::uint8_t expected[4] = {180, 90, 45, 255};

    for (const auto filter : golden::filters) {
        for (const auto color_space : golden::color_spaces) {
            for (const auto sharpen : {Sharpen::contrast_adaptive, Sharpen::robust_contrast_adaptive}) {
                CAPTURE(static_cast<int>(filter), static_cast<int>(color_space), static_cast<int>(sharpen));

                Options options{};
                options.filter = filter;
                options.color_space = color_space;
                options.sharpen = sharpen;
                options.sharpening = 0.30f;
                const auto out = upscale_rgba(src, 6, 6, 13, 9, options);

                for (std::size_t i = 0; i < out.size(); ++i) {
                    const int diff = std::abs(static_cast<int>(static_cast<std::uint8_t>(out[i])) - expected[i % 4]);
                    CAPTURE(i);
                    REQUIRE(diff <= 1);
                }
            }
        }
    }
}

TEST_CASE("easu anti-ringing clamp prevents overshoot on a hard edge", "[quality][easu]") {
    constexpr std::uint32_t sw = 8;
    constexpr std::uint32_t sh = 8;
    std::vector<std::byte> src(static_cast<std::size_t>(sw) * sh * 4);
    for (std::uint32_t y = 0; y < sh; ++y) {
        for (std::uint32_t x = 0; x < sw; ++x) {
            const std::size_t i = (static_cast<std::size_t>(y) * sw + x) * 4;
            const std::uint8_t v = x < sw / 2 ? 64 : 192;
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
    const auto out = upscale_rgba(src, sw, sh, 23, 17, options);

    for (std::size_t i = 0; i < out.size(); ++i) {
        if (i % 4 == 3) {
            continue; // alpha stays 255
        }
        const auto v = static_cast<std::uint8_t>(out[i]);
        CAPTURE(i, static_cast<int>(v));
        REQUIRE(v >= 63);
        REQUIRE(v <= 193);
    }
}

TEST_CASE("rcas-like sharpening increases edge contrast without leaving the valid range", "[quality][rcas]") {
    constexpr std::uint32_t sw = 8;
    constexpr std::uint32_t sh = 8;
    std::vector<std::byte> src(static_cast<std::size_t>(sw) * sh * 4);
    for (std::uint32_t y = 0; y < sh; ++y) {
        for (std::uint32_t x = 0; x < sw; ++x) {
            const std::size_t i = (static_cast<std::size_t>(y) * sw + x) * 4;
            const std::uint8_t v = x < sw / 2 ? 96 : 160;
            src[i + 0] = static_cast<std::byte>(v);
            src[i + 1] = static_cast<std::byte>(v);
            src[i + 2] = static_cast<std::byte>(v);
            src[i + 3] = static_cast<std::byte>(255);
        }
    }

    Options soft{};
    soft.filter = Filter::bilinear;
    soft.color_space = ColorSpace::linear;
    soft.sharpening = 0.0f;

    Options sharp = soft;
    sharp.sharpen = Sharpen::robust_contrast_adaptive;
    sharp.sharpening = 1.0f;

    const auto blurred = upscale_rgba(src, sw, sh, 17, 13, soft);
    const auto sharpened = upscale_rgba(src, sw, sh, 17, 13, sharp);

    // Contrast across the middle row must not decrease when RCAS-like sharpening is enabled.
    const auto row_contrast = [](const std::vector<std::byte>& image) {
        constexpr std::uint32_t dw = 17;
        constexpr std::uint32_t dh = 13;
        int lo = 255;
        int hi = 0;
        int prev = -1;
        int max_step = 0;
        for (std::uint32_t x = 0; x < dw; ++x) {
            const std::size_t i = ((static_cast<std::size_t>(dh / 2) * dw) + x) * 4;
            const int v = static_cast<int>(static_cast<std::uint8_t>(image[i]));
            lo = std::min(lo, v);
            hi = std::max(hi, v);
            if (prev >= 0) {
                max_step = std::max(max_step, std::abs(v - prev));
            }
            prev = v;
        }
        return std::pair{hi - lo, max_step};
    };

    const auto [blurred_range, blurred_step] = row_contrast(blurred);
    const auto [sharp_range, sharp_step] = row_contrast(sharpened);
    REQUIRE(sharp_step > blurred_step);
    REQUIRE(sharp_range >= blurred_range);
}
