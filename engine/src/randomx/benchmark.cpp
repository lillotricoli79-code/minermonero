#include "benchmark.hpp"

#include <randomx.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace unique_rx {
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

} // namespace

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

} // namespace unique_rx
