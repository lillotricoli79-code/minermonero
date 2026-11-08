#include "benchmark.hpp"
#include "dataset.hpp"
#include "platform/affinity.hpp"

#include <randomx.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <exception>
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

using Cache = std::shared_ptr<randomx_cache>;
using Vm = std::unique_ptr<randomx_vm, VmRelease>;
using Hash = std::array<std::uint8_t, RANDOMX_HASH_SIZE>;

} // namespace

bool run_benchmark(unsigned int seconds, unsigned int lanes,
                   bool use_jit, bool full_memory, std::string_view key,
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
    Cache cache(randomx_alloc_cache(flags), CacheRelease{});
    if (!cache && (flags & RANDOMX_FLAG_LARGE_PAGES) != RANDOMX_FLAG_DEFAULT) {
        flags = static_cast<randomx_flags>(
            static_cast<int>(flags) & ~static_cast<int>(RANDOMX_FLAG_LARGE_PAGES));
        cache = Cache(randomx_alloc_cache(flags), CacheRelease{});
    }
    if (!cache) {
        std::cerr << "Allocazione cache RandomX non riuscita.\n";
        return false;
    }
    randomx_init_cache(cache.get(), key.data(), key.size());

    std::shared_ptr<randomx_dataset> dataset;
    if (full_memory) {
        const auto init_threads = std::max(1U, std::thread::hardware_concurrency());
        try {
            dataset = create_randomx_dataset(cache, init_threads, flags);
        } catch (const std::exception& error) {
            std::cerr << "Inizializzazione dataset RandomX non riuscita: "
                      << error.what() << '\n';
            return false;
        }
    }

    const auto vm_flags = full_memory
        ? static_cast<randomx_flags>(flags | RANDOMX_FLAG_FULL_MEM)
        : flags;
    std::vector<Vm> vms;
    vms.reserve(lanes);
    for (unsigned int lane = 0; lane < lanes; ++lane) {
        Vm vm(randomx_create_vm(
            vm_flags, full_memory ? nullptr : cache.get(),
            full_memory ? dataset.get() : nullptr));
        if (!vm) {
            std::cerr << "Creazione VM non riuscita per lane " << lane << ".\n";
            return false;
        }
        vms.push_back(std::move(vm));
    }

    std::vector<std::uint64_t> lane_hashes(lanes, 0);
    std::vector<Hash> last_hash(lanes);
    std::vector<std::exception_ptr> worker_errors(lanes);
    std::vector<std::jthread> workers;
    workers.reserve(lanes);
    std::atomic<bool> failed{false};
    const auto started = std::chrono::steady_clock::now();
    const auto deadline = started + std::chrono::seconds(seconds);

    try {
        for (unsigned int lane = 0; lane < lanes; ++lane) {
            workers.emplace_back([&, lane] {
                try {
                    if (!platform::pin_current_thread(lane)) {
                        std::cerr << "Benchmark lane " << lane
                                  << ": affinity unavailable; continuing without pinning\n";
                    }
                    std::string input(payload);
                    const auto nonce_offset = input.size();
                    input.resize(nonce_offset + sizeof(std::uint32_t));
                    std::uint32_t nonce = lane;
                    const auto write_nonce_le = [&](std::uint32_t value) {
                        input[nonce_offset] = static_cast<char>(value);
                        input[nonce_offset + 1] = static_cast<char>(value >> 8);
                        input[nonce_offset + 2] = static_cast<char>(value >> 16);
                        input[nonce_offset + 3] = static_cast<char>(value >> 24);
                    };
                    write_nonce_le(nonce);
                    randomx_calculate_hash_first(vms[lane].get(), input.data(), input.size());
                    while (!failed.load(std::memory_order_acquire) &&
                           std::chrono::steady_clock::now() < deadline) {
                        nonce += lanes;
                        write_nonce_le(nonce);
                        randomx_calculate_hash_next(vms[lane].get(), input.data(), input.size(),
                                                    last_hash[lane].data());
                        ++lane_hashes[lane];
                    }
                    if (!failed.load(std::memory_order_acquire)) {
                        randomx_calculate_hash_last(vms[lane].get(), last_hash[lane].data());
                        ++lane_hashes[lane];
                    }
                } catch (...) {
                    worker_errors[lane] = std::current_exception();
                    failed.store(true, std::memory_order_release);
                }
            });
        }
    } catch (const std::exception& error) {
        failed.store(true, std::memory_order_release);
        for (auto& worker : workers) if (worker.joinable()) worker.join();
        std::cerr << "Avvio worker benchmark non riuscito: " << error.what() << '\n';
        return false;
    }
    for (auto& worker : workers) if (worker.joinable()) worker.join();
    for (const auto& worker_error : worker_errors) {
        if (!worker_error) continue;
        try {
            std::rethrow_exception(worker_error);
        } catch (const std::exception& error) {
            std::cerr << "Worker benchmark non riuscito: " << error.what() << '\n';
        } catch (...) {
            std::cerr << "Worker benchmark non riuscito con errore sconosciuto.\n";
        }
        return false;
    }

    const auto elapsed = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - started).count();
    std::uint64_t total_hashes = 0;
    for (const auto lane_count : lane_hashes) total_hashes += lane_count;
    std::cout << "UNIQUE RX · RandomX benchmark\n"
              << "Mode: " << (use_jit ? "JIT" : "interpreter") << "\n"
              << "Lanes: " << lanes << "\n"
              << "Memory: " << (full_memory ? "full dataset" : "light reference") << "\n"
              << "RandomX flags: 0x" << std::hex
              << static_cast<unsigned int>(vm_flags) << std::dec << "\n"
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
