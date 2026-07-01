// Image-quality comparison tool for regression checks: prints MSE, PSNR, and a mean
// SSIM computed on luma over 8x8 windows for two binary PPM (P6) images of equal size.
//
//   retrofsr_compare a.ppm b.ppm [--min-psnr <dB>] [--min-ssim <0..1>]
//
// Exit codes: 0 = comparison done and thresholds met, 1 = threshold violated,
// 2 = usage or file error.

#include <charconv>
#include <cmath>
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

struct RgbImage {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::vector<std::uint8_t> rgb;
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

std::uint32_t read_u32_token(std::istream& in) {
    skip_ws_and_comments(in);
    unsigned long long value = 0;
    in >> value;
    if (!in || value > std::numeric_limits<std::uint32_t>::max()) {
        throw std::runtime_error("invalid PPM integer token");
    }
    return static_cast<std::uint32_t>(value);
}

RgbImage read_ppm(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        throw std::runtime_error("cannot open input file: " + path);
    }

    std::string magic;
    in >> magic;
    if (magic != "P6") {
        throw std::runtime_error("only binary PPM P6 is supported: " + path);
    }

    RgbImage image;
    image.width = read_u32_token(in);
    image.height = read_u32_token(in);
    const auto max_value = read_u32_token(in);
    if (image.width == 0 || image.height == 0 || max_value != 255) {
        throw std::runtime_error("PPM must have non-zero size and max value 255: " + path);
    }
    in.get();

    const auto pixels = static_cast<std::uint64_t>(image.width) * image.height;
    if (pixels > std::numeric_limits<std::size_t>::max() / 3) {
        throw std::runtime_error("image dimensions are too large: " + path);
    }
    image.rgb.resize(static_cast<std::size_t>(pixels) * 3);
    in.read(reinterpret_cast<char*>(image.rgb.data()), static_cast<std::streamsize>(image.rgb.size()));
    if (!in) {
        throw std::runtime_error("truncated PPM data: " + path);
    }
    return image;
}

[[nodiscard]] double luma_at(const RgbImage& image, std::uint32_t x, std::uint32_t y) noexcept {
    const std::size_t i = (static_cast<std::size_t>(y) * image.width + x) * 3;
    return 0.2126 * image.rgb[i] + 0.7152 * image.rgb[i + 1] + 0.0722 * image.rgb[i + 2];
}

[[nodiscard]] double mean_squared_error(const RgbImage& a, const RgbImage& b) noexcept {
    double sum = 0.0;
    for (std::size_t i = 0; i < a.rgb.size(); ++i) {
        const double diff = static_cast<double>(a.rgb[i]) - static_cast<double>(b.rgb[i]);
        sum += diff * diff;
    }
    return sum / static_cast<double>(a.rgb.size());
}

// Mean SSIM on luma over non-overlapping 8x8 windows (partial edge windows included).
[[nodiscard]] double mean_ssim(const RgbImage& a, const RgbImage& b) noexcept {
    constexpr double c1 = (0.01 * 255.0) * (0.01 * 255.0);
    constexpr double c2 = (0.03 * 255.0) * (0.03 * 255.0);
    constexpr std::uint32_t window = 8;

    double ssim_sum = 0.0;
    std::size_t window_count = 0;
    for (std::uint32_t wy = 0; wy < a.height; wy += window) {
        for (std::uint32_t wx = 0; wx < a.width; wx += window) {
            const std::uint32_t x1 = std::min(wx + window, a.width);
            const std::uint32_t y1 = std::min(wy + window, a.height);
            const auto n = static_cast<double>((x1 - wx) * (y1 - wy));

            double mean_a = 0.0;
            double mean_b = 0.0;
            for (std::uint32_t y = wy; y < y1; ++y) {
                for (std::uint32_t x = wx; x < x1; ++x) {
                    mean_a += luma_at(a, x, y);
                    mean_b += luma_at(b, x, y);
                }
            }
            mean_a /= n;
            mean_b /= n;

            double var_a = 0.0;
            double var_b = 0.0;
            double covar = 0.0;
            for (std::uint32_t y = wy; y < y1; ++y) {
                for (std::uint32_t x = wx; x < x1; ++x) {
                    const double da = luma_at(a, x, y) - mean_a;
                    const double db = luma_at(b, x, y) - mean_b;
                    var_a += da * da;
                    var_b += db * db;
                    covar += da * db;
                }
            }
            var_a /= n;
            var_b /= n;
            covar /= n;

            const double numerator = (2.0 * mean_a * mean_b + c1) * (2.0 * covar + c2);
            const double denominator = (mean_a * mean_a + mean_b * mean_b + c1) * (var_a + var_b + c2);
            ssim_sum += numerator / denominator;
            ++window_count;
        }
    }
    return window_count == 0 ? 1.0 : ssim_sum / static_cast<double>(window_count);
}

double parse_double(std::string_view text, const char* what) {
    double value = 0.0;
    const auto* end = text.data() + text.size();
    const auto [ptr, ec] = std::from_chars(text.data(), end, value);
    if (ec != std::errc{} || ptr != end) {
        throw std::runtime_error(std::string("invalid ") + what + ": " + std::string(text));
    }
    return value;
}

} // namespace

int main(int argc, char** argv) {
    try {
        if (argc < 3) {
            std::cerr << "usage: retrofsr_compare <a.ppm> <b.ppm> [--min-psnr <dB>] [--min-ssim <0..1>]\n";
            return 2;
        }

        double min_psnr = -1.0;
        double min_ssim = -1.0;
        for (int i = 3; i < argc; ++i) {
            const std::string_view arg = argv[i];
            if (arg == "--min-psnr" && i + 1 < argc) {
                min_psnr = parse_double(argv[++i], "--min-psnr");
            } else if (arg == "--min-ssim" && i + 1 < argc) {
                min_ssim = parse_double(argv[++i], "--min-ssim");
            } else {
                throw std::runtime_error("unknown argument: " + std::string(arg));
            }
        }

        const RgbImage a = read_ppm(argv[1]);
        const RgbImage b = read_ppm(argv[2]);
        if (a.width != b.width || a.height != b.height) {
            std::cerr << "retrofsr_compare: size mismatch " << a.width << 'x' << a.height << " vs " << b.width << 'x'
                      << b.height << '\n';
            return 1;
        }

        const double mse = mean_squared_error(a, b);
        const double psnr = mse <= 0.0 ? std::numeric_limits<double>::infinity()
                                       : 10.0 * std::log10((255.0 * 255.0) / mse);
        const double ssim = mean_ssim(a, b);

        std::cout << "size: " << a.width << 'x' << a.height << '\n';
        std::cout << "mse: " << mse << '\n';
        std::cout << "psnr_db: " << psnr << '\n';
        std::cout << "ssim: " << ssim << '\n';

        if (min_psnr >= 0.0 && psnr < min_psnr) {
            std::cerr << "retrofsr_compare: PSNR " << psnr << " below threshold " << min_psnr << '\n';
            return 1;
        }
        if (min_ssim >= 0.0 && ssim < min_ssim) {
            std::cerr << "retrofsr_compare: SSIM " << ssim << " below threshold " << min_ssim << '\n';
            return 1;
        }
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "retrofsr_compare: " << e.what() << '\n';
        return 2;
    }
}
