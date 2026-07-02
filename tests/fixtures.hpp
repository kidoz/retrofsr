#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace fixtures {

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
