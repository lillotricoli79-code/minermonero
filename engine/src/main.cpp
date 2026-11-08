#include "stratum.hpp"
#include "randomx/benchmark.hpp"

#include <cstdint>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>

namespace {

unsigned int parse_unsigned(const char* value, unsigned int fallback) {
    if (value == nullptr) return fallback;
    try {
        const auto parsed = std::stoul(value);
        return parsed <= UINT32_MAX ? static_cast<unsigned int>(parsed) : 0;
    } catch (const std::exception&) {
        return 0;
    }
}

void print_usage(const char* executable) {
    std::cout << "Usage: " << executable
              << " --benchmark|--benchmark-interpreter [seconds] [lanes:1..4] [seed] [input]\n";
    std::cout << "       " << executable
              << " --mine [config.json] [max-hashes:0=unlimited]\n";
}

} // namespace

int main(int argc, char** argv) {
    if (argc > 1 && std::string_view(argv[1]) == "--mine") {
        const std::filesystem::path config_path = argc > 2 ? argv[2] : "config.json";
        const auto maximum_hashes = parse_unsigned(argc > 3 ? argv[3] : nullptr, 0);
        return unique_rx::run_miner(config_path, maximum_hashes);
    }
    if (argc < 2 || std::string_view(argv[1]) == "--help") {
        print_usage(argv[0]);
        return argc < 2 ? 2 : 0;
    }
    const std::string_view command(argv[1]);
    const bool use_jit = command == "--benchmark";
    if (!use_jit && command != "--benchmark-interpreter") {
        print_usage(argv[0]);
        return 2;
    }

    const auto seconds = parse_unsigned(argc > 2 ? argv[2] : nullptr, 10);
    const auto lanes = parse_unsigned(argc > 3 ? argv[3] : nullptr, 1);
    const std::string_view key = argc > 4 ? argv[4] : "UNIQUE-RX reference seed";
    const std::string_view input = argc > 5 ? argv[5] : "UNIQUE-RX reference input";
    return unique_rx::run_benchmark(seconds, lanes, use_jit, key, input) ? 0 : 1;
}