#include <retrofsr/retrofsr.hpp>

#include <charconv>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace {

struct RgbaImage {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::vector<std::byte> rgba;
};

void skip_ws_and_comments(std::istream& in) {
    while (true) {
        in >> std::ws;
        if (in.peek() != '#') {
            return;
        }
        std::string ignored;
        std::getline(in, ignored);
    }
}

std::size_t checked_buffer_bytes(std::uint32_t width, std::uint32_t height, std::size_t bytes_per_pixel) {
    constexpr auto size_max = std::numeric_limits<std::size_t>::max();
    const auto pixels = static_cast<std::uint64_t>(width) * static_cast<std::uint64_t>(height);
    if (bytes_per_pixel != 0 && pixels > size_max / bytes_per_pixel) {
        throw std::runtime_error("image dimensions are too large");
    }
    return static_cast<std::size_t>(pixels) * bytes_per_pixel;
}

std::uint32_t parse_dimension(const char* text, const char* what) {
    const char* const end = text + std::string_view{text}.size();
    std::uint64_t value = 0;
    const auto [ptr, ec] = std::from_chars(text, end, value);
    // Cap at the library's documented dimension limit (INT32_MAX) here, before any
    // buffer is allocated: an absurd size must fail fast, not thrash the allocator.
    constexpr std::uint64_t max_dimension = std::numeric_limits<std::int32_t>::max();
    if (ec != std::errc{} || ptr != end || value == 0 || value > max_dimension) {
        throw std::runtime_error(std::string("invalid ") + what + ": " + text);
    }
    return static_cast<std::uint32_t>(value);
}

std::uint32_t read_u32_token(std::istream& in) {
    skip_ws_and_comments(in);
    unsigned long long value = 0;
    in >> value;
    if (!in || value > std::numeric_limits<std::uint32_t>::max()) {
        throw std::runtime_error("invalid PPM integer token");
    }
    return static_cast<std::uint32_t>(value);
}

RgbaImage read_ppm(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        throw std::runtime_error("cannot open input file: " + path);
    }

    std::string magic;
    in >> magic;
    if (magic != "P6") {
        throw std::runtime_error("only binary PPM P6 is supported");
    }

    const auto width = read_u32_token(in);
    const auto height = read_u32_token(in);
    const auto max_value = read_u32_token(in);
    if (width == 0 || height == 0 || max_value != 255) {
        throw std::runtime_error("PPM must have non-zero size and max value 255");
    }
    // Same dimension cap as the CLI arguments (the library's documented limit): reject
    // before allocating, so an absurd header fails fast instead of thrashing the allocator.
    constexpr auto max_dimension = static_cast<std::uint32_t>(std::numeric_limits<std::int32_t>::max());
    if (width > max_dimension || height > max_dimension) {
        throw std::runtime_error("image dimensions are too large");
    }
    in.get(); // consume one whitespace byte after max value

    std::vector<std::byte> rgb(checked_buffer_bytes(width, height, 3));
    in.read(reinterpret_cast<char*>(rgb.data()), static_cast<std::streamsize>(rgb.size()));
    if (!in) {
        throw std::runtime_error("truncated PPM data");
    }

    RgbaImage image;
    image.width = width;
    image.height = height;
    image.rgba.resize(checked_buffer_bytes(width, height, 4));
    for (std::size_t i = 0, j = 0; i < rgb.size(); i += 3, j += 4) {
        image.rgba[j + 0] = rgb[i + 0];
        image.rgba[j + 1] = rgb[i + 1];
        image.rgba[j + 2] = rgb[i + 2];
        image.rgba[j + 3] = static_cast<std::byte>(255);
    }
    return image;
}

void write_ppm(const std::string& path, const RgbaImage& image) {
    std::ofstream out(path, std::ios::binary);
    if (!out) {
        throw std::runtime_error("cannot open output file: " + path);
    }
    out << "P6\n" << image.width << ' ' << image.height << "\n255\n";
    for (std::size_t i = 0; i < image.rgba.size(); i += 4) {
        out.put(static_cast<char>(image.rgba[i + 0]));
        out.put(static_cast<char>(image.rgba[i + 1]));
        out.put(static_cast<char>(image.rgba[i + 2]));
    }
}

retrofsr::Options options_from_mode(const std::string& mode) {
    if (mode == "retro") {
        return retrofsr::retro_pixel_options();
    }
    if (mode == "fsr") {
        return retrofsr::fsr_like_options();
    }
    if (mode == "fsr1") {
        return retrofsr::fsr1_style_options();
    }
    if (mode == "fsr1-noise" || mode == "fsr1-coherence" || mode == "fsr1-both") {
        auto options = retrofsr::fsr1_style_options();
        options.rcas_noise_suppression = mode != "fsr1-coherence";
        options.easu_edge_coherence = mode != "fsr1-noise";
        return options;
    }
    if (mode == "fsr1-quality") {
        return retrofsr::fsr1_style_options(retrofsr::QualityPreset::quality);
    }
    if (mode == "fsr1-balanced") {
        return retrofsr::fsr1_style_options(retrofsr::QualityPreset::balanced);
    }
    if (mode == "fsr1-performance") {
        return retrofsr::fsr1_style_options(retrofsr::QualityPreset::performance);
    }
    if (mode == "fsr1-ultra") {
        return retrofsr::fsr1_style_options(retrofsr::QualityPreset::ultra_performance);
    }

    auto options = retrofsr::fsr_like_options();
    options.sharpening = 0.0f;
    if (mode == "nearest") {
        options.filter = retrofsr::Filter::nearest;
    } else if (mode == "bilinear") {
        options.filter = retrofsr::Filter::bilinear;
    } else if (mode == "catmull") {
        options.filter = retrofsr::Filter::catmull_rom;
    } else if (mode == "lanczos") {
        options.filter = retrofsr::Filter::lanczos3;
    } else if (mode == "easu") {
        options.filter = retrofsr::Filter::easu;
    } else {
        throw std::runtime_error("unknown mode: " + mode);
    }
    return options;
}

} // namespace

int main(int argc, char** argv) {
    try {
        if (argc < 5 || argc > 6) {
            std::cerr << "usage: scale_ppm <input.ppm> <output.ppm> <width> <height> "
                         "[retro|fsr|fsr1|fsr1-quality|fsr1-balanced|fsr1-performance|fsr1-ultra"
                         "|fsr1-noise|fsr1-coherence|fsr1-both"
                         "|nearest|bilinear|catmull|lanczos|easu]\n";
            return 2;
        }

        const std::string input_path = argv[1];
        const std::string output_path = argv[2];
        const auto dst_w = parse_dimension(argv[3], "width");
        const auto dst_h = parse_dimension(argv[4], "height");
        const std::string mode = argc == 6 ? argv[5] : "retro";

        const RgbaImage src_image = read_ppm(input_path);
        RgbaImage dst_image;
        dst_image.width = dst_w;
        dst_image.height = dst_h;
        dst_image.rgba.resize(checked_buffer_bytes(dst_w, dst_h, 4));

        const retrofsr::ImageView src{
            .pixels = src_image.rgba.data(),
            .width = src_image.width,
            .height = src_image.height,
            .stride_bytes = 0,
            .format = retrofsr::PixelFormat::rgba8,
        };
        const retrofsr::MutableImageView dst{
            .pixels = dst_image.rgba.data(),
            .width = dst_image.width,
            .height = dst_image.height,
            .stride_bytes = 0,
            .format = retrofsr::PixelFormat::rgba8,
        };

        auto options = options_from_mode(mode);
        if (const auto status = retrofsr::upscale(src, dst, options); status != retrofsr::Status::ok) {
            throw std::runtime_error(retrofsr::status_message(status));
        }

        write_ppm(output_path, dst_image);
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "scale_ppm: " << e.what() << '\n';
        return 1;
    }
}
