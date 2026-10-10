// Repeated whole-frame timings. Google Benchmark's real time includes the scaler's
// internal workers; its benchmark runner itself remains single-threaded.
#include <retrofsr/retrofsr.hpp>

#include <benchmark/benchmark.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace {

struct FrameSize {
    std::uint32_t width;
    std::uint32_t height;
};

constexpr std::array frame_sizes{
    FrameSize{256, 240}, FrameSize{320, 240}, FrameSize{640, 480}, FrameSize{1280, 720}, FrameSize{1920, 1080},
};

struct FilterCase {
    retrofsr::Filter filter;
    const char* name;
};

constexpr std::array filter_cases{
    FilterCase{retrofsr::Filter::nearest, "nearest"},
    FilterCase{retrofsr::Filter::bilinear, "bilinear"},
    FilterCase{retrofsr::Filter::sharp_bilinear, "sharp_bilinear"},
    FilterCase{retrofsr::Filter::catmull_rom, "catmull_rom"},
    FilterCase{retrofsr::Filter::lanczos3, "lanczos3"},
    FilterCase{retrofsr::Filter::edge_adaptive_lanczos, "edge_adaptive"},
    FilterCase{retrofsr::Filter::easu, "easu"},
};

std::vector<std::byte> make_frame(FrameSize size) {
    std::vector<std::byte> frame(static_cast<std::size_t>(size.width) * size.height * 4);
    for (std::uint32_t y = 0; y < size.height; ++y) {
        for (std::uint32_t x = 0; x < size.width; ++x) {
            const std::size_t i = (static_cast<std::size_t>(y) * size.width + x) * 4;
            frame[i] = static_cast<std::byte>(x & 0xffu);
            frame[i + 1] = static_cast<std::byte>(y & 0xffu);
            frame[i + 2] = static_cast<std::byte>(((x ^ y) & 8u) != 0 ? 220 : 40);
            frame[i + 3] = static_cast<std::byte>(255);
        }
    }
    return frame;
}

void run_case(benchmark::State& state, FrameSize size, retrofsr::Options options) {
    options.thread_count = static_cast<std::uint32_t>(state.range(0));
    const auto source = make_frame(size);
    std::vector<std::byte> destination(static_cast<std::size_t>(size.width) * size.height * 16);
    const retrofsr::ImageView input{source.data(), size.width, size.height, 0, retrofsr::PixelFormat::rgba8};
    const retrofsr::MutableImageView output{destination.data(), size.width * 2, size.height * 2, 0,
                                            retrofsr::PixelFormat::rgba8};
    // Source/destination allocation and one warmup are outside the timed loop.
    const auto warmup = retrofsr::upscale(input, output, options);
    if (warmup != retrofsr::Status::ok) {
        state.SkipWithError(retrofsr::status_message(warmup));
        return;
    }
    for ([[maybe_unused]] auto iteration : state) {
        const auto result = retrofsr::upscale(input, output, options);
        if (result != retrofsr::Status::ok) {
            state.SkipWithError(retrofsr::status_message(result));
            break;
        }
        benchmark::DoNotOptimize(destination.data());
        benchmark::ClobberMemory();
    }
    state.counters["Mpix/s"] = benchmark::Counter(static_cast<double>(size.width) * size.height * 4 / 1.0e6,
                                                  benchmark::Counter::kIsIterationInvariantRate);
}

void register_mode(const std::string& name, retrofsr::Options options) {
    for (const auto size : frame_sizes) {
        const auto case_name = name + "/" + std::to_string(size.width) + "x" + std::to_string(size.height);
        benchmark::RegisterBenchmark(case_name.c_str(),
                                     [size, options](benchmark::State& state) { run_case(state, size, options); })
            ->ArgName("workers")
            ->Arg(0)
            ->Arg(1)
            ->Arg(4)
            ->UseRealTime()
            ->Unit(benchmark::kMillisecond);
    }
}

} // namespace

int main(int argc, char** argv) {
    // Preserve the old seven-filter matrix: default sRGB + CAS sharpening 0.20.
    for (const auto filter_case : filter_cases) {
        retrofsr::Options options{};
        options.filter = filter_case.filter;
        register_mode(std::string("legacy/") + filter_case.name, options);
    }
    for (unsigned flags = 0; flags < 4; ++flags) {
        auto options = retrofsr::fsr1_style_options();
        options.rcas_noise_suppression = (flags & 1u) != 0;
        options.easu_edge_coherence = (flags & 2u) != 0;
        constexpr std::array names{"fsr1/baseline", "fsr1/noise", "fsr1/coherence", "fsr1/both"};
        register_mode(names[flags], options);
    }
    auto nearest = retrofsr::fsr_like_options();
    nearest.filter = retrofsr::Filter::nearest;
    nearest.sharpening = 0;
    register_mode("pixel_art/nearest", nearest);

    benchmark::Initialize(&argc, argv);
    if (benchmark::ReportUnrecognizedArguments(argc, argv)) {
        return 2;
    }
    benchmark::AddCustomContext("retrofsr_version", RETROFSR_BENCH_VERSION);
    benchmark::AddCustomContext("compiler", RETROFSR_BENCH_COMPILER);
    benchmark::AddCustomContext("build_type", RETROFSR_BENCH_BUILD_TYPE);
    benchmark::AddCustomContext("simd", RETROFSR_BENCH_SIMD);
    benchmark::AddCustomContext("workload", "rgb-gradient-xor-v1; rgba8; 2x per axis; workers=0 means auto");
    benchmark::AddCustomContext("timing", "wall time; includes internal allocation and worker startup; excludes IO");
    benchmark::RunSpecifiedBenchmarks();
    benchmark::Shutdown();
}
