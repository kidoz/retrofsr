#pragma once

#include <retrofsr/retrofsr.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <vector>

namespace golden {

inline constexpr std::uint32_t src_width = 8;
inline constexpr std::uint32_t src_height = 7;
inline constexpr std::uint32_t dst_width = 13;
inline constexpr std::uint32_t dst_height = 11;
inline constexpr std::size_t dst_bytes = static_cast<std::size_t>(dst_width) * dst_height * 4;

/// One golden rendering configuration.
struct Config {
    retrofsr::Filter filter = retrofsr::Filter::nearest;
    retrofsr::ColorSpace color_space = retrofsr::ColorSpace::srgb;
    retrofsr::Sharpen sharpen = retrofsr::Sharpen::contrast_adaptive;
};

inline constexpr std::array color_spaces = {
    retrofsr::ColorSpace::srgb,
    retrofsr::ColorSpace::linear,
    retrofsr::ColorSpace::perceptual,
};

inline constexpr std::array filters = {
    retrofsr::Filter::nearest,     retrofsr::Filter::bilinear, retrofsr::Filter::sharp_bilinear,
    retrofsr::Filter::catmull_rom, retrofsr::Filter::lanczos3, retrofsr::Filter::edge_adaptive_lanczos,
    retrofsr::Filter::easu,
};

/// Every filter in every color space with the default sharpener, plus the RCAS-like
/// sharpener over the EASU-like filter in every color space.
[[nodiscard]] constexpr auto make_configs() noexcept {
    std::array<Config, filters.size() * color_spaces.size() + color_spaces.size()> configs{};
    std::size_t i = 0;
    for (const auto filter : filters) {
        for (const auto color_space : color_spaces) {
            configs[i++] = {filter, color_space, retrofsr::Sharpen::contrast_adaptive};
        }
    }
    for (const auto color_space : color_spaces) {
        configs[i++] = {retrofsr::Filter::easu, color_space, retrofsr::Sharpen::robust_contrast_adaptive};
    }
    return configs;
}

inline constexpr auto configs = make_configs();

/// Deterministic source frame with gradients, a hard vertical edge, and a checkerboard.
[[nodiscard]] inline std::vector<std::byte> make_source() {
    std::vector<std::byte> src(static_cast<std::size_t>(src_width) * src_height * 4);
    for (std::uint32_t y = 0; y < src_height; ++y) {
        for (std::uint32_t x = 0; x < src_width; ++x) {
            const std::size_t i = (static_cast<std::size_t>(y) * src_width + x) * 4;
            const std::uint32_t checker = ((x ^ y) & 1u) * 255u;
            src[i + 0] = static_cast<std::byte>((x * 255u) / (src_width - 1));
            src[i + 1] = static_cast<std::byte>((y * 128u) / (src_height - 1) + checker / 2u);
            src[i + 2] = static_cast<std::byte>(x < src_width / 2 ? 32 : 224);
            src[i + 3] = static_cast<std::byte>(255);
        }
    }
    return src;
}

/// Render one golden configuration single-threaded.
[[nodiscard]] inline std::vector<std::byte> render(Config config, const std::vector<std::byte>& src) {
    std::vector<std::byte> dst(dst_bytes);
    const retrofsr::ImageView src_view{src.data(), src_width, src_height, 0, retrofsr::PixelFormat::rgba8};
    const retrofsr::MutableImageView dst_view{dst.data(), dst_width, dst_height, 0, retrofsr::PixelFormat::rgba8};

    retrofsr::Options options{};
    options.filter = config.filter;
    options.color_space = config.color_space;
    options.sharpen = config.sharpen;
    options.sharpening = 0.20f;
    options.edge_strength = 1.0f;
    options.thread_count = 1;

    if (retrofsr::upscale(src_view, dst_view, options) != retrofsr::Status::ok) {
        throw std::runtime_error("golden render failed");
    }
    return dst;
}

} // namespace golden
