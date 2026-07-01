#include <retrofsr/retrofsr.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <vector>

namespace {

bool run_filter(retrofsr::Filter filter) {
    constexpr std::uint32_t sw = 8;
    constexpr std::uint32_t sh = 8;
    constexpr std::uint32_t dw = 31;
    constexpr std::uint32_t dh = 29;

    std::vector<std::byte> src(static_cast<std::size_t>(sw) * sh * 4);
    for (std::uint32_t y = 0; y < sh; ++y) {
        for (std::uint32_t x = 0; x < sw; ++x) {
            const std::size_t i = (static_cast<std::size_t>(y) * sw + x) * 4;
            src[i + 0] = static_cast<std::byte>((x * 255) / (sw - 1));
            src[i + 1] = static_cast<std::byte>((y * 255) / (sh - 1));
            src[i + 2] = static_cast<std::byte>(((x ^ y) & 1) ? 255 : 0);
            src[i + 3] = static_cast<std::byte>(255);
        }
    }

    std::vector<std::byte> dst(static_cast<std::size_t>(dw) * dh * 4);

    retrofsr::ImageView src_view{
        .pixels = src.data(),
        .width = sw,
        .height = sh,
        .stride_bytes = 0,
        .format = retrofsr::PixelFormat::rgba8,
    };
    retrofsr::MutableImageView dst_view{
        .pixels = dst.data(),
        .width = dw,
        .height = dh,
        .stride_bytes = 0,
        .format = retrofsr::PixelFormat::bgra8,
    };

    auto options = retrofsr::fsr_like_options();
    options.filter = filter;
    options.thread_count = 1;
    if (retrofsr::upscale(src_view, dst_view, options) != retrofsr::Status::ok) {
        return false;
    }

    for (std::byte byte : dst) {
        if (static_cast<unsigned char>(byte) != 0) {
            return true;
        }
    }
    return false;
}

} // namespace

TEST_CASE("upscale produces non-empty output for every filter", "[upscale]") {
    const retrofsr::Filter filters[] = {
        retrofsr::Filter::nearest,     retrofsr::Filter::bilinear, retrofsr::Filter::sharp_bilinear,
        retrofsr::Filter::catmull_rom, retrofsr::Filter::lanczos3, retrofsr::Filter::edge_adaptive_lanczos,
    };
    for (retrofsr::Filter filter : filters) {
        CAPTURE(static_cast<int>(filter));
        REQUIRE(run_filter(filter));
    }
}

TEST_CASE("validate rejects unsafe inputs", "[validate]") {
    std::vector<std::byte> buf(256);
    const retrofsr::ImageView src{buf.data(), 4, 4, 0, retrofsr::PixelFormat::gray8};
    const retrofsr::MutableImageView dst{buf.data() + 128, 8, 8, 0, retrofsr::PixelFormat::gray8};

    SECTION("disjoint buffers are accepted") {
        REQUIRE(retrofsr::validate(src, dst) == retrofsr::Status::ok);
    }

    SECTION("null pixel pointers are rejected") {
        retrofsr::ImageView bad = src;
        bad.pixels = nullptr;
        REQUIRE(retrofsr::validate(bad, dst) == retrofsr::Status::null_pointer);
    }

    SECTION("zero dimensions are rejected") {
        retrofsr::ImageView bad = src;
        bad.width = 0;
        REQUIRE(retrofsr::validate(bad, dst) == retrofsr::Status::invalid_size);
    }

    SECTION("dimensions beyond the signed sampling range are rejected") {
        retrofsr::ImageView bad = src;
        bad.width = 0xffffffffu;
        bad.height = 1;
        REQUIRE(retrofsr::validate(bad, dst) == retrofsr::Status::invalid_size);

        retrofsr::MutableImageView bad_dst = dst;
        bad_dst.height = 0x80000000u;
        REQUIRE(retrofsr::validate(src, bad_dst) == retrofsr::Status::invalid_size);
    }

    SECTION("strides smaller than a packed row are rejected") {
        retrofsr::ImageView bad = src;
        bad.stride_bytes = 3; // packed gray8 row is 4 bytes
        REQUIRE(retrofsr::validate(bad, dst) == retrofsr::Status::invalid_stride);
    }

    SECTION("negative strides are rejected") {
        retrofsr::ImageView bad = src;
        bad.stride_bytes = -4;
        REQUIRE(retrofsr::validate(bad, dst) == retrofsr::Status::invalid_stride);
    }

    SECTION("identical buffers are rejected") {
        const retrofsr::MutableImageView same{buf.data(), 4, 4, 0, retrofsr::PixelFormat::gray8};
        REQUIRE(retrofsr::validate(src, same) == retrofsr::Status::same_buffer_unsupported);
    }

    SECTION("partially overlapping buffers are rejected") {
        // dst starts inside src's 16-byte extent.
        const retrofsr::MutableImageView overlapping{buf.data() + 4, 4, 4, 0, retrofsr::PixelFormat::gray8};
        REQUIRE(retrofsr::validate(src, overlapping) == retrofsr::Status::same_buffer_unsupported);

        // src starts inside dst's extent.
        const retrofsr::ImageView src_after{buf.data() + 132, 2, 2, 0, retrofsr::PixelFormat::gray8};
        REQUIRE(retrofsr::validate(src_after, dst) == retrofsr::Status::same_buffer_unsupported);
    }

    SECTION("buffers adjacent without overlap are accepted") {
        // src occupies [0, 16); dst starting at 16 must be fine.
        const retrofsr::MutableImageView adjacent{buf.data() + 16, 4, 4, 0, retrofsr::PixelFormat::gray8};
        REQUIRE(retrofsr::validate(src, adjacent) == retrofsr::Status::ok);
    }
}

TEST_CASE("upscale supports rgb565 with retro pixel options", "[upscale][rgb565]") {
    constexpr std::uint32_t sw = 4;
    constexpr std::uint32_t sh = 4;
    constexpr std::uint32_t dw = 16;
    constexpr std::uint32_t dh = 16;
    std::vector<std::byte> src(static_cast<std::size_t>(sw) * sh * 2, std::byte{0xff});
    std::vector<std::byte> dst(static_cast<std::size_t>(dw) * dh * 2);

    const retrofsr::ImageView src_view{src.data(), sw, sh, 0, retrofsr::PixelFormat::rgb565};
    const retrofsr::MutableImageView dst_view{dst.data(), dw, dh, 0, retrofsr::PixelFormat::rgb565};

    auto options = retrofsr::retro_pixel_options();
    options.thread_count = 1;
    REQUIRE(retrofsr::upscale(src_view, dst_view, options) == retrofsr::Status::ok);
}
