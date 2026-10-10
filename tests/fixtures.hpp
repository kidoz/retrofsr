#pragma once

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace fixtures {

enum class MotionPattern : std::uint8_t { diagonal, thin_line, corner, checkerboard, transparent_sprite };

inline constexpr std::array motion_patterns = {MotionPattern::diagonal, MotionPattern::thin_line, MotionPattern::corner,
                                               MotionPattern::checkerboard, MotionPattern::transparent_sprite};

/// Analytic scene moving horizontally in source-pixel units. Integrate each pixel
/// with an 8x8 grid. Rendering the same scene at a smaller pixel_size independently
/// supplies high-resolution references, including fractional-motion phases.
/// Transparent sprites are premultiplied; opaque scenes use gray levels 32..224.
[[nodiscard]] inline std::vector<std::byte> make_motion_rgba(MotionPattern pattern, std::uint32_t width,
                                                             std::uint32_t height, float offset,
                                                             float pixel_size = 1.0f) {
    std::vector<std::byte> image(static_cast<std::size_t>(width) * height * 4);
    for (std::uint32_t y = 0; y < height; ++y) {
        for (std::uint32_t x = 0; x < width; ++x) {
            unsigned covered = 0;
            for (unsigned j = 0; j < 8; ++j) {
                for (unsigned i = 0; i < 8; ++i) {
                    const float sx =
                        (static_cast<float>(x) + (static_cast<float>(i) + 0.5f) / 8.0f) * pixel_size - offset;
                    const float sy = (static_cast<float>(y) + (static_cast<float>(j) + 0.5f) / 8.0f) * pixel_size;
                    bool inside = false;
                    switch (pattern) {
                    case MotionPattern::diagonal:
                        inside = sx > sy;
                        break;
                    case MotionPattern::thin_line:
                        inside = std::abs(sx - sy) < 0.75f;
                        break;
                    case MotionPattern::corner:
                        inside = sx > 12.0f && sy > 12.0f;
                        break;
                    case MotionPattern::checkerboard:
                        inside =
                            (static_cast<int>(std::floor(sx / 2.0f)) + static_cast<int>(std::floor(sy / 2.0f))) % 2 ==
                            0;
                        break;
                    case MotionPattern::transparent_sprite:
                        inside = sx > 8.0f && sx < 22.0f && sy > 8.0f && sy < 22.0f;
                        break;
                    }
                    covered += inside ? 1u : 0u;
                }
            }
            const float coverage = static_cast<float>(covered) / 64.0f;
            const bool transparent = pattern == MotionPattern::transparent_sprite;
            const auto color =
                static_cast<std::byte>(std::lround(transparent ? 192.0f * coverage : 32.0f + 192.0f * coverage));
            const auto alpha = static_cast<std::byte>(std::lround(transparent ? 255.0f * coverage : 255.0f));
            const std::size_t p = (static_cast<std::size_t>(y) * width + x) * 4;
            image[p] = image[p + 1] = image[p + 2] = color;
            image[p + 3] = alpha;
        }
    }
    return image;
}

/// Deterministic rgba8 test patterns exercising the failure modes that matter for
/// upscaling: hard edges, shallow gradients, checkerboards, transparency, and noise.
enum class Pattern : std::uint8_t {
    edges,
    gradient,
    checkerboard,
    transparency,
    noise,
};

inline constexpr std::array patterns = {
    Pattern::edges, Pattern::gradient, Pattern::checkerboard, Pattern::transparency, Pattern::noise,
};

[[nodiscard]] inline std::vector<std::byte> make_rgba(Pattern pattern, std::uint32_t width, std::uint32_t height) {
    std::vector<std::byte> image(static_cast<std::size_t>(width) * height * 4);
    std::uint32_t rng = 0x9e3779b9u;
    const auto next_random = [&rng]() noexcept {
        rng = rng * 1664525u + 1013904223u;
        return static_cast<std::uint8_t>(rng >> 24);
    };

    for (std::uint32_t y = 0; y < height; ++y) {
        for (std::uint32_t x = 0; x < width; ++x) {
            const std::size_t i = (static_cast<std::size_t>(y) * width + x) * 4;
            std::uint8_t r = 0;
            std::uint8_t g = 0;
            std::uint8_t b = 0;
            std::uint8_t a = 255;
            switch (pattern) {
            case Pattern::edges: {
                const bool bright = (x < width / 2) != (y < height / 2);
                r = g = b = bright ? 230 : 25;
                break;
            }
            case Pattern::gradient:
                r = static_cast<std::uint8_t>((x * 255u) / (width - 1));
                g = static_cast<std::uint8_t>((y * 255u) / (height - 1));
                b = static_cast<std::uint8_t>(((x + y) * 255u) / (width + height - 2));
                break;
            case Pattern::checkerboard:
                r = g = b = ((x ^ y) & 1u) ? 255 : 0;
                break;
            case Pattern::transparency:
                // Warm color under an alpha ramp, with a hard fully-transparent cut on the
                // left quarter (the jump exercises the near-zero-alpha unpremultiply path).
                r = 220;
                g = 60;
                b = 40;
                a = x < width / 4 ? 0 : static_cast<std::uint8_t>((x * 255u) / (width - 1));
                break;
            case Pattern::noise:
                r = next_random();
                g = next_random();
                b = next_random();
                break;
            }
            image[i + 0] = static_cast<std::byte>(r);
            image[i + 1] = static_cast<std::byte>(g);
            image[i + 2] = static_cast<std::byte>(b);
            image[i + 3] = static_cast<std::byte>(a);
        }
    }
    return image;
}

} // namespace fixtures
