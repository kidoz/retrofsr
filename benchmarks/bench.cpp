// Throughput/latency benchmark for common emulator frame sizes.
//
//   just bench            (configures -Dbenchmarks=true, builds, runs)
//   ./buildDir/retrofsr_bench [thread_count]
//
// Each case upscales the source 2x per axis with default options (sharpening 0.20)
// for the given filter. Reported throughput counts destination pixels.

#include <retrofsr/retrofsr.hpp>

#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string_view>
#include <vector>

namespace {

struct FrameSize {
    std::uint32_t width;
    std::uint32_t height;
};

constexpr FrameSize frame_sizes[] = {
    {256, 240}, {320, 240}, {640, 480}, {1280, 720}, {1920, 1080},
};

struct FilterCase {
    retrofsr::Filter filter;
    const char* name;
};

constexpr FilterCase filter_cases[] = {
    {retrofsr::Filter::nearest, "nearest"},
    {retrofsr::Filter::bilinear, "bilinear"},
    {retrofsr::Filter::sharp_bilinear, "sharp_bilinear"},
    {retrofsr::Filter::catmull_rom, "catmull_rom"},
    {retrofsr::Filter::lanczos3, "lanczos3"},
    {retrofsr::Filter::edge_adaptive_lanczos, "edge_adaptive"},
    {retrofsr::Filter::easu, "easu"},
};

std::vector<std::byte> make_frame(std::uint32_t width, std::uint32_t height) {
    std::vector<std::byte> frame(static_cast<std::size_t>(width) * height * 4);
    for (std::uint32_t y = 0; y < height; ++y) {
        for (std::uint32_t x = 0; x < width; ++x) {
            const std::size_t i = (static_cast<std::size_t>(y) * width + x) * 4;
            frame[i + 0] = static_cast<std::byte>(x & 0xff);
            frame[i + 1] = static_cast<std::byte>(y & 0xff);
            frame[i + 2] = static_cast<std::byte>(((x ^ y) & 8u) != 0 ? 220 : 40);
            frame[i + 3] = static_cast<std::byte>(255);
        }
    }
    return frame;
}

[[nodiscard]] double run_once(const std::vector<std::byte>& src, FrameSize s, std::vector<std::byte>& dst,
                              retrofsr::Options options) {
    const retrofsr::ImageView src_view{src.data(), s.width, s.height, 0, retrofsr::PixelFormat::rgba8};
    const retrofsr::MutableImageView dst_view{dst.data(), s.width * 2, s.height * 2, 0, retrofsr::PixelFormat::rgba8};
    const auto start = std::chrono::steady_clock::now();
    const auto status = retrofsr::upscale(src_view, dst_view, options);
    const auto stop = std::chrono::steady_clock::now();
    if (status != retrofsr::Status::ok) {
        std::fprintf(stderr, "upscale failed: %s\n", retrofsr::status_message(status));
        return -1.0;
    }
    return std::chrono::duration<double, std::milli>(stop - start).count();
}

} // namespace

int main(int argc, char** argv) {
    std::uint32_t threads = 0;
    if (argc > 1) {
        const std::string_view arg = argv[1];
        std::uint32_t parsed = 0;
        const auto [ptr, ec] = std::from_chars(arg.data(), arg.data() + arg.size(), parsed);
        if (ec != std::errc{} || ptr != arg.data() + arg.size()) {
            std::fprintf(stderr, "usage: retrofsr_bench [thread_count]\n");
            return 2;
        }
        threads = parsed;
    }

    std::printf("retrofsr benchmark: 2x upscale per axis, rgba8, default options (sharpening 0.20)\n");
    std::printf("threads: %s\n\n", threads == 0 ? "auto" : argv[1]);
    std::printf("%-15s %-22s %12s %12s %12s\n", "filter", "src -> dst", "avg ms", "min ms", "Mpix/s");

    for (const auto s : frame_sizes) {
        const auto src = make_frame(s.width, s.height);
        std::vector<std::byte> dst(static_cast<std::size_t>(s.width) * 2 * s.height * 2 * 4);
        const double dst_mpix = static_cast<double>(s.width) * 2.0 * static_cast<double>(s.height) * 2.0 / 1.0e6;

        for (const auto filter_case : filter_cases) {
            retrofsr::Options options{};
            options.filter = filter_case.filter;
            options.thread_count = threads;

            const double warmup_ms = run_once(src, s, dst, options);
            if (warmup_ms < 0.0) {
                return 1;
            }
            // Aim for roughly half a second per case, bounded to keep the suite fast.
            int iterations = warmup_ms > 0.0 ? static_cast<int>(500.0 / warmup_ms) : 30;
            iterations = iterations < 3 ? 3 : (iterations > 30 ? 30 : iterations);

            double total_ms = 0.0;
            double min_ms = warmup_ms;
            for (int i = 0; i < iterations; ++i) {
                const double ms = run_once(src, s, dst, options);
                if (ms < 0.0) {
                    return 1;
                }
                total_ms += ms;
                min_ms = ms < min_ms ? ms : min_ms;
            }
            const double avg_ms = total_ms / iterations;

            char size_text[32];
            std::snprintf(size_text, sizeof size_text, "%ux%u -> %ux%u", s.width, s.height, s.width * 2, s.height * 2);
            std::printf("%-15s %-22s %12.3f %12.3f %12.1f\n", filter_case.name, size_text, avg_ms, min_ms,
                        dst_mpix / (avg_ms / 1000.0));
        }
        std::printf("\n");
    }
    return 0;
}
