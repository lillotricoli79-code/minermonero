#include "stratum/stratum.hpp"
#include "randomx/benchmark.hpp"
#include "logging/log.hpp"

#include <cstdint>
#include <charconv>
#include <algorithm>
#include <filesystem>
#include <exception>
#include <iostream>
#include <string>
#include <stdexcept>
#include <string_view>
#include <system_error>

namespace {

std::uint64_t parse_unsigned64(const char* value, std::uint64_t fallback) {
    if (value == nullptr) return fallback;
    const std::string_view text(value);
    if (text.empty() || !std::all_of(text.begin(), text.end(), [](unsigned char ch) {
            return ch >= '0' && ch <= '9';
        })) {
        throw std::invalid_argument(
            std::string("invalid unsigned numeric argument: ") + value);
    }
    std::uint64_t parsed = 0;
    const auto result = std::from_chars(text.data(), text.data() + text.size(), parsed, 10);
    if (result.ec != std::errc{} || result.ptr != text.data() + text.size()) {
        throw std::invalid_argument(
            std::string("invalid or out-of-range unsigned numeric argument: ") + value);
    }
    return parsed;
}

unsigned int parse_unsigned(const char* value, unsigned int fallback) {
    const auto parsed = parse_unsigned64(value, fallback);
    if (parsed > UINT32_MAX) {
        throw std::invalid_argument(std::string("numeric argument exceeds 32-bit range: ") + value);
    }
    return static_cast<unsigned int>(parsed);
}

void print_usage(const char* executable) {
    std::cout << "Usage: " << executable
              << " --benchmark|--benchmark-full|--benchmark-interpreter|--benchmark-full-interpreter [seconds] [lanes:1..4] [seed] [input]\n";
    std::cout << "       " << executable
              << " --mine [config.json] [max-hashes:0=unlimited]\n";
    std::cout << "       " << executable
              << " --benchmark [seconds] [lanes:1..4] [seed] [input] [prefetch-mode:0..3] [jit-huge-pages:0|1]\n";
}

} // namespace

int main(int argc, char** argv) {
    try {
        if (argc > 1 && std::string_view(argv[1]) == "--mine") {
            std::filesystem::path config_path = "config.json";
            std::uint64_t maximum_hashes = 0;
            std::uint32_t thread_override = 0;
            int index = 2;
            if (index < argc && std::string_view(argv[index]).substr(0, 2) != "--") {
                config_path = argv[index++];
            }
            while (index < argc) {
                const std::string_view option(argv[index++]);
                if (option == "--threads" || option == "--max-hashes") {
                    if (index >= argc)
                        throw std::invalid_argument(std::string(option) + " requires a value");
                } else if (!option.starts_with("--")) {
                    if (maximum_hashes != 0)
                        throw std::invalid_argument("maximum hashes specified more than once");
                    maximum_hashes = parse_unsigned64(std::string(option).c_str(), 0);
                    continue;
                }
                if (option == "--threads") {
                    const auto value = parse_unsigned(argv[index++], 0);
                    if (value == 0 || value > 256)
                        throw std::invalid_argument("--threads must be between 1 and 256");
                    thread_override = value;
                } else if (option == "--max-hashes") {
                    maximum_hashes = parse_unsigned64(argv[index++], 0);
                } else {
                    throw std::invalid_argument("unknown --mine option: " + std::string(option));
                }
            }
            return unique_rx::run_miner(config_path, maximum_hashes, thread_override);
        }
        if (argc < 2 || std::string_view(argv[1]) == "--help") {
            print_usage(argv[0]);
            return argc < 2 ? 2 : 0;
        }
        const std::string_view command(argv[1]);
        const bool full_memory =
            command == "--benchmark-full" || command == "--benchmark-full-interpreter";
        const bool use_jit =
            command == "--benchmark" || command == "--benchmark-full";
        if (!use_jit && command != "--benchmark-interpreter" &&
            command != "--benchmark-full-interpreter") {
            print_usage(argv[0]);
            return 2;
        }

        if (argc > 8) {
            print_usage(argv[0]);
            return 2;
        }
        const auto seconds = parse_unsigned(argc > 2 ? argv[2] : nullptr, 10);
        const auto lanes = parse_unsigned(argc > 3 ? argv[3] : nullptr, 1);
        const std::string_view key = argc > 4 ? argv[4] : "UNIQUE-RX reference seed";
        const std::string_view input = argc > 5 ? argv[5] : "UNIQUE-RX reference input";
        const auto prefetch_mode = parse_unsigned(argc > 6 ? argv[6] : nullptr, 1);
        const auto jit_huge_pages = parse_unsigned(argc > 7 ? argv[7] : nullptr, 0);
        if (prefetch_mode > 3 || jit_huge_pages > 1) {
            print_usage(argv[0]);
            return 2;
        }
        return unique_rx::run_benchmark(seconds, lanes, use_jit, full_memory,
                                        key, input, prefetch_mode,
                                        jit_huge_pages != 0) ? 0 : 1;
    } catch (const std::exception& error) {
        unique_rx::logging::error("fatal", error.what());
        return 1;
    } catch (...) {
        unique_rx::logging::error("fatal", "unknown exception");
        return 1;
    }
}
