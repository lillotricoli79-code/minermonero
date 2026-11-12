#include "stratum/stratum.hpp"
#include "randomx/benchmark.hpp"
#include "logging/log.hpp"

#include <cstdint>
#include <charconv>
#include <algorithm>
#include <filesystem>
#include <exception>
#include <iostream>
#include <optional>
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
              << " [-c|--config config.json] [-o|--url pool:port] [-u|--user WALLET] [-p|--pass PASSWORD]\n";
    std::cout << "       " << executable
              << " [--rig-id WORKER] [--tls] [--tls-fingerprint SHA256] [-t|--threads N] [--max-hashes N]\n";
    std::cout << "       " << executable
              << " --benchmark|--benchmark-full|--benchmark-interpreter|--benchmark-full-interpreter [seconds] [lanes:1..4] [seed] [input]\n";
    std::cout << "       --mine remains available as a backward-compatible alias. With no arguments, mining starts from config.json.\n";
}

bool is_benchmark_command(std::string_view command) {
    return command == "--benchmark" || command == "--benchmark-full" ||
           command == "--benchmark-interpreter" ||
           command == "--benchmark-full-interpreter";
}

int run_benchmark_cli(int argc, char** argv) {
    const std::string_view command(argv[1]);
    const bool full_memory =
        command == "--benchmark-full" || command == "--benchmark-full-interpreter";
    const bool use_jit =
        command == "--benchmark" || command == "--benchmark-full";

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
}

} // namespace

int main(int argc, char** argv) {
    try {
        if (argc > 1 && is_benchmark_command(argv[1])) {
            return run_benchmark_cli(argc, argv);
        }

        std::filesystem::path config_path = "config.json";
        std::uint64_t maximum_hashes = 0;
        std::uint32_t thread_override = 0;
        unique_rx::MinerCliOptions cli_options;
        bool config_selected = false;
        const bool legacy_mine_prefix =
            argc > 1 && std::string_view(argv[1]) == "--mine";
        int index = legacy_mine_prefix ? 2 : 1;

        while (index < argc) {
            std::string_view argument(argv[index++]);
            if (argument == "--help" || argument == "-h") {
                print_usage(argv[0]);
                return 0;
            }
            if (argument == "--mine") {
                continue;
            }

            std::optional<std::string_view> inline_value;
            const auto equals = argument.find('=');
            if (equals != std::string_view::npos) {
                inline_value = argument.substr(equals + 1);
                argument = argument.substr(0, equals);
            }

            const auto take_value = [&](const char* option_name) -> std::string {
                if (inline_value) {
                    if (inline_value->empty()) {
                        throw std::invalid_argument(std::string(option_name) + " requires a value");
                    }
                    return std::string(*inline_value);
                }
                if (index >= argc) {
                    throw std::invalid_argument(std::string(option_name) + " requires a value");
                }
                return argv[index++];
            };

            if (argument == "-c" || argument == "--config") {
                config_path = take_value("--config");
                config_selected = true;
            } else if (argument == "-o" || argument == "--url") {
                cli_options.url = take_value("--url");
            } else if (argument == "-u" || argument == "--user") {
                cli_options.user = take_value("--user");
            } else if (argument == "-p" || argument == "--pass") {
                cli_options.password = take_value("--pass");
            } else if (argument == "--rig-id") {
                cli_options.worker = take_value("--rig-id");
            } else if (argument == "--tls") {
                if (inline_value) throw std::invalid_argument("--tls does not take a value");
                cli_options.tls = true;
            } else if (argument == "--tls-fingerprint") {
                cli_options.tls_fingerprint = take_value("--tls-fingerprint");
            } else if (argument == "--keepalive") {
                if (inline_value) throw std::invalid_argument("--keepalive does not take a value");
                cli_options.keepalive = true;
            } else if (argument == "-t" || argument == "--threads") {
                const auto value = parse_unsigned(take_value("--threads").c_str(), 0);
                if (value == 0 || value > 256) {
                    throw std::invalid_argument("--threads must be between 1 and 256");
                }
                thread_override = value;
            } else if (argument == "--max-hashes") {
                maximum_hashes = parse_unsigned64(take_value("--max-hashes").c_str(), 0);
            } else if (!argument.empty() && argument.front() != '-') {
                if (!config_selected) {
                    config_path = std::string(argument);
                    config_selected = true;
                } else if (legacy_mine_prefix && maximum_hashes == 0) {
                    maximum_hashes = parse_unsigned64(std::string(argument).c_str(), 0);
                } else {
                    throw std::invalid_argument("unexpected positional argument: " + std::string(argument));
                }
            } else {
                throw std::invalid_argument("unknown option: " + std::string(argument));
            }
        }

        return unique_rx::run_miner(config_path, maximum_hashes,
                                    thread_override, std::move(cli_options));
    } catch (const std::exception& error) {
        unique_rx::logging::error("fatal", error.what());
        return 1;
    } catch (...) {
        unique_rx::logging::error("fatal", "unknown exception");
        return 1;
    }
}
