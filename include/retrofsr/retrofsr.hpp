#pragma once

#include <cstddef>
#include <cstdint>

namespace retrofsr {

/// Packed source/destination pixel layouts supported by the CPU implementation.
enum class PixelFormat : std::uint8_t {
    rgba8,
    bgra8,
    argb8,
    abgr8,
    xrgb8,
    xbgr8,
    rgbx8,
    bgrx8,
    rgb565,
    bgr565,
    gray8,
};

/// Spatial resampling filter.
enum class Filter : std::uint8_t {
    /// Exact source pixel copy. Best for integer pixel-art scaling.
    nearest,
    /// Classic bilinear filtering.
    bilinear,
    /// Pixel-art friendly bilinear: mostly crisp with narrow blended seams.
    sharp_bilinear,
    /// Bicubic Catmull-Rom reconstruction.
    catmull_rom,
    /// Lanczos3 reconstruction.
    lanczos3,
    /// FSR-1-inspired spatial upsampling: Lanczos-like filter with reduced radius near strong edges.
    edge_adaptive_lanczos,
};

/// How RGB values should be interpreted during interpolation.
enum class ColorSpace : std::uint8_t {
    /// Convert 8-bit sRGB to linear light before filtering, then encode back to sRGB.
    srgb,
    /// Treat channels as linear normalized values. Faster, useful for non-color data.
    linear,
};

/// Alpha handling for filtering.
enum class AlphaMode : std::uint8_t {
    /// Ignore input alpha and write fully opaque alpha where the destination format has alpha.
    ignore,
    /// Input/output RGB are straight-alpha; filtering is internally premultiplied to avoid fringes.
    straight,
    /// Input/output RGB are already premultiplied.
    premultiplied,
};

/// Non-owning source image view.
struct ImageView {
    const std::byte* pixels = nullptr;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    /// Bytes between rows. Use 0 for tightly packed rows.
    std::ptrdiff_t stride_bytes = 0;
    PixelFormat format = PixelFormat::rgba8;
};

/// Non-owning destination image view.
struct MutableImageView {
    std::byte* pixels = nullptr;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    /// Bytes between rows. Use 0 for tightly packed rows.
    std::ptrdiff_t stride_bytes = 0;
    PixelFormat format = PixelFormat::rgba8;
};

/// Upscaling configuration.
struct Options {
    Filter filter = Filter::edge_adaptive_lanczos;
    ColorSpace color_space = ColorSpace::srgb;
    AlphaMode alpha = AlphaMode::ignore;

    /// 0 disables sharpening. Good FSR-like values are about 0.10 to 0.35.
    float sharpening = 0.20f;

    /// Edge sensitivity for edge_adaptive_lanczos. Higher values reduce ringing more aggressively.
    float edge_strength = 1.0f;

    /// 0 = choose automatically. 1 = single-threaded.
    std::uint32_t thread_count = 0;
};

/// Suggested settings for 2D console pixel art, HUDs, and tile/sprite output.
[[nodiscard]] constexpr Options retro_pixel_options() noexcept {
    Options options{};
    options.filter = Filter::sharp_bilinear;
    options.color_space = ColorSpace::srgb;
    options.alpha = AlphaMode::ignore;
    options.sharpening = 0.0f;
    options.edge_strength = 0.0f;
    options.thread_count = 0;
    return options;
}

/// Suggested settings for already-filtered emulator frames, 3D games, or blended video.
[[nodiscard]] constexpr Options fsr_like_options() noexcept {
    Options options{};
    options.filter = Filter::edge_adaptive_lanczos;
    options.color_space = ColorSpace::srgb;
    options.alpha = AlphaMode::ignore;
    options.sharpening = 0.20f;
    options.edge_strength = 1.0f;
    options.thread_count = 0;
    return options;
}

enum class Status : std::uint8_t {
    ok,
    null_pointer,
    invalid_size,
    invalid_stride,
    unsupported_format,
    same_buffer_unsupported,
};

/// Number of bytes per pixel for a supported format, or 0 if unknown.
[[nodiscard]] std::size_t bytes_per_pixel(PixelFormat format) noexcept;

/// Validate an upscaling request without performing the operation.
[[nodiscard]] Status validate(ImageView src, MutableImageView dst) noexcept;

/// Human-readable status text; string has static lifetime.
[[nodiscard]] const char* status_message(Status status) noexcept;

/// Upscale src into dst. Source and destination memory must not overlap.
///
/// The function allocates a temporary floating-point image of dst.width * dst.height pixels.
/// It may throw std::bad_alloc if allocation fails. All argument errors are returned as Status.
[[nodiscard]] Status upscale(ImageView src, MutableImageView dst, Options options = {});

} // namespace retrofsr
