#include "core/mining.hpp"

#include <chrono>
#include <cstring>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

namespace unique_rx::core {
namespace {

struct VmRelease {
    void operator()(randomx_vm* vm) const {
        if (vm != nullptr) randomx_destroy_vm(vm);
    }
};

using Vm = std::unique_ptr<randomx_vm, VmRelease>;

void write_nonce(std::vector<std::uint8_t>& blob, std::uint32_t nonce) {
    std::memcpy(blob.data() + kNonceOffset, &nonce, sizeof(nonce));
}

bool meets_target(const Hash& hash, std::uint64_t target) {
    std::uint64_t high = 0;
    std::memcpy(&high, hash.data() + 24, sizeof(high));
    return high < target;
}

inline constexpr std::size_t kMaximumQueuedShares = 4096;

} // namespace

void hash_worker(MiningState& state, std::uint32_t worker_id,
                 std::uint32_t worker_count, std::uint64_t maximum_hashes) {
    if (worker_count == 0 || worker_id >= worker_count) {
        state.report_worker_error("invalid CPU worker index or worker count");
        return;
    }

    Vm vm;
    std::shared_ptr<const MiningWork> active_work;
    std::vector<std::uint8_t> blob;
    std::uint64_t cursor = 0;
    try {
        while (!state.stop.load(std::memory_order_acquire)) {
            const auto work = state.current_work();
            if (!work) {
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
                continue;
            }
            if (!active_work || active_work->generation != work->generation) {
                auto vm_flags = work->flags;
                if (work->full_memory) vm_flags |= RANDOMX_FLAG_FULL_MEM;
                vm.reset(randomx_create_vm(
                    vm_flags,
                    work->full_memory ? nullptr : work->cache.get(),
                    work->full_memory ? work->dataset.get() : nullptr));
                if (!vm) throw std::runtime_error("worker VM creation failed");
                active_work = work;
                blob = work->job.blob;
                cursor = 0;
            }

            const auto nonce64 = static_cast<std::uint64_t>(work->job.nonce) +
                worker_id + cursor * worker_count;
            const auto nonce = static_cast<std::uint32_t>(nonce64);
            write_nonce(blob, nonce);
            Hash hash{};
            randomx_calculate_hash(vm.get(), blob.data(), blob.size(), hash.data());
            ++cursor;
            const auto count = state.hashes.fetch_add(1, std::memory_order_relaxed) + 1;

            const auto latest_work = state.current_work();
            if (latest_work && latest_work->generation == work->generation &&
                meets_target(hash, work->job.target)) {
                {
                    std::lock_guard lock(state.share_mutex);
                    if (state.shares.size() < kMaximumQueuedShares) {
                        state.shares.push_back({work->generation, work->job.id, nonce, hash});
                    } else {
                        state.dropped_shares.fetch_add(1, std::memory_order_relaxed);
                    }
                }
                state.found.fetch_add(1, std::memory_order_relaxed);
            }
            if (maximum_hashes != 0 && count >= maximum_hashes) {
                state.stop.store(true, std::memory_order_release);
            }
        }
    } catch (const std::exception& error) {
        state.report_worker_error(error.what());
    } catch (...) {
        // Keep an unexpected worker exception from terminating the whole process.
        state.report_worker_error("worker failed with an unknown exception");
    }
}

} // namespace unique_rx::core
