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
    /// FSR-1-inspired EASU-like upsampling: adaptive edge direction, anisotropic Lanczos2-style
    /// weighting, and a local min/max anti-ringing clamp. Best quality for filtered/3D frames.
    easu,
};

/// How RGB values should be interpreted during interpolation.
enum class ColorSpace : std::uint8_t {
    /// Convert 8-bit sRGB to linear light before filtering, then encode back to sRGB.
    srgb,
    /// Treat channels as linear normalized values. Faster, useful for non-color data.
    linear,
    /// Filter gamma-encoded values directly, without linearization. This matches how FSR 1
    /// operates on perceptual/post-tonemap color and preserves apparent contrast; use it with
    /// Filter::easu for FSR-style output. Same cost as linear.
    perceptual,
};

/// Post-upscale sharpening algorithm. Options::sharpening controls the strength for both.
enum class Sharpen : std::uint8_t {
    /// Unsharp mask with local contrast adaptation and per-channel overshoot clamping.
    contrast_adaptive,
    /// RCAS-like robust contrast-adaptive sharpening (FSR-1-inspired): a peak-limited negative
    /// lobe that resists ringing and avoids amplifying already-saturated detail.
    robust_contrast_adaptive,
};

/// Output quantization dithering.
enum class Dither : std::uint8_t {
    /// Round to the nearest representable value.
    none,
    /// Deterministic 4x4 ordered (Bayer) dithering. Reduces banding on rgb565, bgr565,
    /// gray8, and other 8-bit outputs; stable across runs for identical inputs.
    ordered,
};

/// Alpha handling for filtering.
enum class AlphaMode : std::uint8_t {
    /// Ignore input alpha and write fully opaque alpha where the destination format has alpha.
    ignore,
    /// Input/output RGB are straight-alpha; filtering is internally premultiplied to avoid fringes.
    /// When the destination format has no alpha channel, the premultiplied result is written
    /// as-is, i.e. the image is composited over black.
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

    /// Sharpening algorithm applied when sharpening > 0.
    Sharpen sharpen = Sharpen::contrast_adaptive;

    /// Output dithering applied when quantizing to the destination format.
    Dither dither = Dither::none;

    /// When alpha is straight or premultiplied, scale sharpening by local alpha and keep
    /// sharpened premultiplied RGB within [0, alpha] to avoid halos around transparency.
    bool alpha_aware_sharpening = true;

    /// Edge sensitivity for edge_adaptive_lanczos and easu. Higher values reduce ringing and
    /// stair-stepping more aggressively.
    float edge_strength = 1.0f;

    /// 0 = choose automatically. 1 = single-threaded.
    std::uint32_t thread_count = 0;

    /// Reduce RCAS sharpening of locally noise-like detail. Only used by
    /// robust_contrast_adaptive; this limits amplification, it does not denoise.
    /// Off by default because intentional dithering and fine textures can resemble noise.
    bool rcas_noise_suppression = false;

    /// Experimental: attenuate EASU directional stretching where neighboring gradients
    /// disagree. Only affects EASU upscaling, not its minification fallback.
    /// Off by default pending visual evaluation on emulator content.
    bool easu_edge_coherence = false;
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

/// Common FSR-style quality presets expressed as source-to-destination scale factors.
enum class QualityPreset : std::uint8_t {
    /// 1.5x per axis.
    quality,
    /// 1.7x per axis.
    balanced,
    /// 2.0x per axis.
    performance,
    /// 3.0x per axis.
    ultra_performance,
};

/// Per-axis upscale factor for a quality preset.
[[nodiscard]] constexpr float quality_preset_scale(QualityPreset preset) noexcept {
    switch (preset) {
    case QualityPreset::quality:
        return 1.5f;
    case QualityPreset::balanced:
        return 1.7f;
    case QualityPreset::performance:
        return 2.0f;
    case QualityPreset::ultra_performance:
        return 3.0f;
    }
    return 1.5f;
}

/// A width/height pair, used for preset size helpers.
struct Extent {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
};

/// Recommended source (render) size for the given output size under a preset.
/// Rounds to the nearest pixel and never returns a zero dimension.
[[nodiscard]] constexpr Extent quality_preset_source_size(QualityPreset preset, std::uint32_t output_width,
                                                          std::uint32_t output_height) noexcept {
    // Scale factors as exact integer ratios: 1.5 = 3/2, 1.7 = 17/10, 2.0 = 2/1, 3.0 = 3/1.
    std::uint64_t num = 2;
    std::uint64_t den = 3;
    switch (preset) {
    case QualityPreset::quality:
        num = 2;
        den = 3;
        break;
    case QualityPreset::balanced:
        num = 10;
        den = 17;
        break;
    case QualityPreset::performance:
        num = 1;
        den = 2;
        break;
    case QualityPreset::ultra_performance:
        num = 1;
        den = 3;
        break;
    }
    const auto scaled_w = (static_cast<std::uint64_t>(output_width) * num + den / 2) / den;
    const auto scaled_h = (static_cast<std::uint64_t>(output_height) * num + den / 2) / den;
    return {static_cast<std::uint32_t>(scaled_w == 0 && output_width != 0 ? 1 : scaled_w),
            static_cast<std::uint32_t>(scaled_h == 0 && output_height != 0 ? 1 : scaled_h)};
}

/// FSR-1-style spatial preset: EASU-like filtering plus RCAS-like sharpening on
/// gamma-encoded (perceptual) color. This is not official AMD FSR; it follows the
/// same high-level recipe with a portable CPU implementation.
[[nodiscard]] constexpr Options fsr1_style_options(QualityPreset preset = QualityPreset::quality) noexcept {
    Options options{};
    options.filter = Filter::easu;
    options.color_space = ColorSpace::perceptual;
    options.alpha = AlphaMode::ignore;
    options.sharpen = Sharpen::robust_contrast_adaptive;
    options.edge_strength = 1.0f;
    options.thread_count = 0;
    switch (preset) {
    case QualityPreset::quality:
        options.sharpening = 0.20f;
        break;
    case QualityPreset::balanced:
        options.sharpening = 0.25f;
        break;
    case QualityPreset::performance:
        options.sharpening = 0.30f;
        break;
    case QualityPreset::ultra_performance:
        options.sharpening = 0.35f;
        break;
    }
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

/// Resize src into dst. Source and destination memory must not overlap.
///
/// Upscaling is the primary use; downscaling is supported and anti-aliased by widening
/// the filter kernel per axis (capped at 8 source texels of support, so pre-reduce for
/// extreme minification). Exceptions: Filter::nearest stays point-sampled and will
/// alias; sharp_bilinear behaves as bilinear when an axis minifies; easu falls back to
/// a widened isotropic kernel with its anti-ringing clamp.
///
/// With sharpening enabled the function allocates a floating-point image of
/// dst.width * dst.height pixels and sharpens directly into dst, so image scratch
/// memory is width * height * 16 bytes. CAS additionally uses 1 KiB of stack scratch
/// per worker. With sharpening disabled it samples
/// directly into dst and only allocates a small per-column coordinate table.
///
/// All argument errors are returned as Status. Allocation and threading failures
/// propagate as exceptions: std::bad_alloc or std::length_error from allocation, and
/// std::system_error if worker threads cannot be created.
[[nodiscard]] Status upscale(ImageView src, MutableImageView dst, Options options = {});

} // namespace retrofsr
