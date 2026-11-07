#include <randomx.h>
#include "stratum.hpp"

#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

struct CacheRelease {
    void operator()(randomx_cache* cache) const {
        if (cache != nullptr) randomx_release_cache(cache);
    }
};

struct VmRelease {
    void operator()(randomx_vm* vm) const {
        if (vm != nullptr) randomx_destroy_vm(vm);
    }
};

using Cache = std::unique_ptr<randomx_cache, CacheRelease>;
using Vm = std::unique_ptr<randomx_vm, VmRelease>;
using Hash = std::array<std::uint8_t, RANDOMX_HASH_SIZE>;

unsigned int parse_unsigned(const char* value, unsigned int fallback) {
    if (value == nullptr) return fallback;
    try {
        const auto parsed = std::stoul(value);
        return parsed <= UINT32_MAX ? static_cast<unsigned int>(parsed) : 0;
    } catch (const std::exception&) {
        return 0;
    }
}

bool run_benchmark(unsigned int seconds, unsigned int lanes,
                   bool use_jit, std::string_view key,
                   std::string_view payload) {
    if (seconds == 0 || seconds > 3600 || lanes == 0 || lanes > 4 ||
        key.empty() || payload.empty()) {
        std::cerr << "Durata (1..3600), lane (1..4), seed e input sono obbligatori.\n";
        return false;
    }

    auto flags = randomx_get_flags();
    if (!use_jit) {
        flags = static_cast<randomx_flags>(
            static_cast<int>(flags) & ~static_cast<int>(RANDOMX_FLAG_JIT));
    }
    Cache cache(randomx_alloc_cache(flags));
    if (!cache) {
        std::cerr << "Allocazione cache RandomX non riuscita.\n";
        return false;
    }
    randomx_init_cache(cache.get(), key.data(), key.size());

    std::vector<Vm> vms;
    vms.reserve(lanes);
    for (unsigned int lane = 0; lane < lanes; ++lane) {
        Vm vm(randomx_create_vm(flags, cache.get(), nullptr));
        if (!vm) {
            std::cerr << "Creazione VM non riuscita per lane " << lane << ".\n";
            return false;
        }
        vms.push_back(std::move(vm));
    }

    std::vector<std::uint64_t> lane_hashes(lanes, 0);
    std::vector<Hash> last_hash(lanes);
    std::vector<std::thread> workers;
    workers.reserve(lanes);
    const auto started = std::chrono::steady_clock::now();
    const auto deadline = started + std::chrono::seconds(seconds);

    for (unsigned int lane = 0; lane < lanes; ++lane) {
        workers.emplace_back([&, lane] {
            std::string input(payload);
            const auto nonce_offset = input.size();
            input.resize(nonce_offset + sizeof(std::uint32_t));
            std::uint32_t nonce = lane;
            std::memcpy(input.data() + nonce_offset, &nonce, sizeof(nonce));
            randomx_calculate_hash_first(vms[lane].get(), input.data(), input.size());
            while (std::chrono::steady_clock::now() < deadline) {
                nonce += lanes;
                std::memcpy(input.data() + nonce_offset, &nonce, sizeof(nonce));
                randomx_calculate_hash_next(vms[lane].get(), input.data(), input.size(),
                                            last_hash[lane].data());
                ++lane_hashes[lane];
            }
            randomx_calculate_hash_last(vms[lane].get(), last_hash[lane].data());
            ++lane_hashes[lane];
        });
    }
    for (auto& worker : workers) worker.join();

    const auto elapsed = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - started).count();
    std::uint64_t total_hashes = 0;
    for (const auto lane_count : lane_hashes) total_hashes += lane_count;
    std::cout << "UNIQUE RX · RandomX light reference\n"
              << "Mode: " << (use_jit ? "JIT" : "interpreter") << "\n"
              << "Lanes: " << lanes << "\n"
              << "RandomX flags: 0x" << std::hex
              << static_cast<unsigned int>(flags) << std::dec << "\n"
              << "Hashes: " << total_hashes << "\n"
              << "Elapsed: " << std::fixed << std::setprecision(3) << elapsed << " s\n"
              << "Rate: " << std::setprecision(2)
              << total_hashes / elapsed << " H/s\n"
              << "Last hash (lane 0): ";
    for (const auto byte : last_hash.front()) {
        std::cout << std::hex << std::setw(2) << std::setfill('0')
                  << static_cast<unsigned int>(byte);
    }
    std::cout << std::dec << '\n';
    return true;
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
    return run_benchmark(seconds, lanes, use_jit, key, input) ? 0 : 1;
}