#pragma once

#include "fixtures.hpp"

#include <retrofsr/retrofsr.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>

namespace golden {

/// Source pattern used for a golden case.
enum class Source : std::uint8_t {
    /// Gradients, a hard vertical edge, and a checkerboard (opaque).
    standard,
    /// The transparency fixture: warm color under an alpha ramp with a hard cut.
    transparency,
};

/// One golden rendering configuration. The list below is the single source of truth
/// shared by the generator and the test; the on-disk manifest mirrors it for review.
struct Config {
    retrofsr::Filter filter = retrofsr::Filter::nearest;
    retrofsr::ColorSpace color_space = retrofsr::ColorSpace::srgb;
    retrofsr::Sharpen sharpen = retrofsr::Sharpen::contrast_adaptive;
    retrofsr::Dither dither = retrofsr::Dither::none;
    retrofsr::AlphaMode alpha = retrofsr::AlphaMode::ignore;
    retrofsr::PixelFormat dst_format = retrofsr::PixelFormat::rgba8;
    Source source = Source::standard;
    float sharpening = 0.20f;
    std::uint32_t src_width = 8;
    std::uint32_t src_height = 7;
    std::uint32_t dst_width = 13;
    std::uint32_t dst_height = 11;
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

/// Full matrix: every filter in every color space, the RCAS sharpener over easu,
/// dithered and undithered low-bit destinations, straight/premultiplied alpha,
/// sharpening extremes, 2:1 downscales per filter, and one mixed-axis resize.
[[nodiscard]] constexpr auto make_configs() noexcept {
    constexpr std::size_t count = filters.size() * color_spaces.size() // base upscale matrix
                                  + color_spaces.size()                // easu + rcas
                                  + 4                                  // rgb565/gray8, +/- dither
                                  + 3                                  // alpha modes
                                  + 2                                  // sharpening extremes
                                  + filters.size()                     // 2:1 downscale
                                  + 1;                                 // mixed-axis resize
    std::array<Config, count> configs{};
    std::size_t i = 0;

    for (const auto filter : filters) {
        for (const auto color_space : color_spaces) {
            configs[i].filter = filter;
            configs[i].color_space = color_space;
            ++i;
        }
    }
    for (const auto color_space : color_spaces) {
        configs[i].filter = retrofsr::Filter::easu;
        configs[i].color_space = color_space;
        configs[i].sharpen = retrofsr::Sharpen::robust_contrast_adaptive;
        ++i;
    }
    for (const auto format : {retrofsr::PixelFormat::rgb565, retrofsr::PixelFormat::gray8}) {
        for (const auto dither : {retrofsr::Dither::none, retrofsr::Dither::ordered}) {
            configs[i].filter = retrofsr::Filter::bilinear;
            configs[i].color_space = retrofsr::ColorSpace::linear;
            configs[i].dst_format = format;
            configs[i].dither = dither;
            ++i;
        }
    }
    for (const auto alpha : {retrofsr::AlphaMode::straight, retrofsr::AlphaMode::premultiplied}) {
        configs[i].filter = retrofsr::Filter::easu;
        configs[i].color_space = retrofsr::ColorSpace::linear;
        configs[i].sharpen = retrofsr::Sharpen::robust_contrast_adaptive;
        configs[i].alpha = alpha;
        configs[i].source = Source::transparency;
        ++i;
    }
    configs[i].filter = retrofsr::Filter::bilinear;
    configs[i].color_space = retrofsr::ColorSpace::linear;
    configs[i].alpha = retrofsr::AlphaMode::straight;
    configs[i].source = Source::transparency;
    ++i;
    for (const float sharpening : {0.0f, 0.35f}) {
        configs[i].filter = retrofsr::Filter::easu;
        configs[i].sharpen = retrofsr::Sharpen::robust_contrast_adaptive;
        configs[i].sharpening = sharpening;
        ++i;
    }
    for (const auto filter : filters) {
        configs[i].filter = filter;
        configs[i].src_width = 16;
        configs[i].src_height = 14;
        configs[i].dst_width = 8;
        configs[i].dst_height = 7;
        ++i;
    }
    configs[i].filter = retrofsr::Filter::easu;
    configs[i].src_width = 8;
    configs[i].src_height = 14;
    configs[i].dst_width = 13;
    configs[i].dst_height = 7;
    ++i;

    return configs;
}

inline constexpr auto configs = make_configs();

/// Deterministic opaque source frame with gradients, a hard vertical edge, and a
/// checkerboard, at any size >= 2x2.
[[nodiscard]] inline std::vector<std::byte> make_standard_source(std::uint32_t width, std::uint32_t height) {
    std::vector<std::byte> src(static_cast<std::size_t>(width) * height * 4);
    for (std::uint32_t y = 0; y < height; ++y) {
        for (std::uint32_t x = 0; x < width; ++x) {
            const std::size_t i = (static_cast<std::size_t>(y) * width + x) * 4;
            const std::uint32_t checker = ((x ^ y) & 1u) * 255u;
            src[i + 0] = static_cast<std::byte>((x * 255u) / (width - 1));
            src[i + 1] = static_cast<std::byte>((y * 128u) / (height - 1) + checker / 2u);
            src[i + 2] = static_cast<std::byte>(x < width / 2 ? 32 : 224);
            src[i + 3] = static_cast<std::byte>(255);
        }
    }
    return src;
}

[[nodiscard]] inline std::vector<std::byte> make_case_source(const Config& config) {
    if (config.source == Source::transparency) {
        return fixtures::make_rgba(fixtures::Pattern::transparency, config.src_width, config.src_height);
    }
    return make_standard_source(config.src_width, config.src_height);
}

/// Render one golden configuration single-threaded.
[[nodiscard]] inline std::vector<std::byte> render(const Config& config) {
    const auto src = make_case_source(config);
    std::vector<std::byte> dst(static_cast<std::size_t>(config.dst_width) * config.dst_height *
                               retrofsr::bytes_per_pixel(config.dst_format));

    const retrofsr::ImageView src_view{src.data(), config.src_width, config.src_height, 0,
                                       retrofsr::PixelFormat::rgba8};
    const retrofsr::MutableImageView dst_view{dst.data(), config.dst_width, config.dst_height, 0, config.dst_format};

    retrofsr::Options options{};
    options.filter = config.filter;
    options.color_space = config.color_space;
    options.sharpen = config.sharpen;
    options.dither = config.dither;
    options.alpha = config.alpha;
    options.sharpening = config.sharpening;
    options.edge_strength = 1.0f;
    options.thread_count = 1;

    if (retrofsr::upscale(src_view, dst_view, options) != retrofsr::Status::ok) {
        throw std::runtime_error("golden render failed");
    }
    return dst;
}

/// Stable case name encoding the configuration; doubles as the golden file stem.
[[nodiscard]] inline std::string case_name(const Config& config) {
    static constexpr std::array filter_names = {
        "nearest", "bilinear", "sharp_bilinear", "catmull_rom", "lanczos3", "edge_adaptive", "easu"};
    static constexpr std::array cs_names = {"srgb", "linear", "perceptual"};

    std::string name = filter_names[static_cast<std::size_t>(config.filter)];
    name += '-';
    name += cs_names[static_cast<std::size_t>(config.color_space)];
    name += config.sharpen == retrofsr::Sharpen::robust_contrast_adaptive ? "-rcas" : "-ca";
    if (config.dst_format == retrofsr::PixelFormat::rgb565) {
        name += "-rgb565";
    } else if (config.dst_format == retrofsr::PixelFormat::gray8) {
        name += "-gray8";
    }
    if (config.dither == retrofsr::Dither::ordered) {
        name += "-dither";
    }
    if (config.alpha == retrofsr::AlphaMode::straight) {
        name += "-straight";
    } else if (config.alpha == retrofsr::AlphaMode::premultiplied) {
        name += "-premult";
    }
    if (config.sharpening != 0.20f) {
        name += "-sharp";
        name += std::to_string(std::lround(config.sharpening * 100.0f));
    }
    if (config.dst_width < config.src_width && config.dst_height < config.src_height) {
        name += "-down2x";
    } else if (config.dst_height < config.src_height) {
        name += "-mixed";
    }
    return name;
}

/// Golden file name: PAM for byte-per-channel formats, raw .bin for packed 565.
[[nodiscard]] inline std::string golden_file_name(const Config& config) {
    const bool pam =
        config.dst_format != retrofsr::PixelFormat::rgb565 && config.dst_format != retrofsr::PixelFormat::bgr565;
    return case_name(config) + (pam ? ".pam" : ".bin");
}

/// PAM (P7) header for the formats we store; 565 cases use raw .bin instead.
[[nodiscard]] inline std::string pam_header(const Config& config) {
    const bool gray = config.dst_format == retrofsr::PixelFormat::gray8;
    std::string header = "P7\n";
    header += "WIDTH " + std::to_string(config.dst_width) + "\n";
    header += "HEIGHT " + std::to_string(config.dst_height) + "\n";
    header += "DEPTH " + std::string(gray ? "1" : "4") + "\n";
    header += "MAXVAL 255\n";
    header += "TUPLTYPE " + std::string(gray ? "GRAYSCALE" : "RGB_ALPHA") + "\n";
    header += "ENDHDR\n";
    return header;
}

/// Load a golden file's pixel payload (skipping the PAM header when present).
[[nodiscard]] inline std::vector<std::byte> load_golden(const std::string& path, const Config& config) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        throw std::runtime_error("cannot open golden file: " + path + " (regenerate with: just golden)");
    }
    std::vector<char> raw((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());

    std::size_t offset = 0;
    if (golden_file_name(config).ends_with(".pam")) {
        const std::string expected_header = pam_header(config);
        if (raw.size() < expected_header.size() ||
            !std::equal(expected_header.begin(), expected_header.end(), raw.begin())) {
            throw std::runtime_error("unexpected PAM header in " + path);
        }
        offset = expected_header.size();
    }

    std::vector<std::byte> pixels(raw.size() - offset);
    for (std::size_t i = 0; i < pixels.size(); ++i) {
        pixels[i] = static_cast<std::byte>(raw[offset + i]);
    }
    return pixels;
}

/// Directory containing golden files: $RETROFSR_TEST_DATA/golden, or the repo-relative
/// default when run from the source root (as `just quality` does).
[[nodiscard]] inline std::string golden_dir() {
    if (const char* env = std::getenv("RETROFSR_TEST_DATA")) {
        return std::string(env) + "/golden";
    }
    return "tests/data/golden";
}

} // namespace golden
