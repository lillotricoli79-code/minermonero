#include "core/mining.hpp"
#include "platform/affinity.hpp"

#include <algorithm>

#include <chrono>
#include <cstring>
#include <iostream>
#include <limits>
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
    if (blob.size() < kNonceOffset + sizeof(nonce)) {
        throw std::runtime_error("RandomX job blob is too short for its nonce");
    }
    const auto encoded = encode_nonce_le(nonce);
    std::copy(encoded.begin(), encoded.end(), blob.begin() + kNonceOffset);
}

bool meets_target(const Hash& hash, std::uint64_t target) {
    std::uint64_t high = 0;
    std::memcpy(&high, hash.data() + 24, sizeof(high));
    return high < target;
}

inline constexpr std::size_t kMaximumQueuedShares = 4096;

std::uint64_t complete_hash(MiningState& state,
                             const std::shared_ptr<const MiningWork>& work,
                             std::uint32_t nonce, const Hash& hash) {
    const auto count = state.hashes.fetch_add(1, std::memory_order_relaxed) + 1;
    const auto latest_work = state.current_work();
    if (latest_work && latest_work->generation == work->generation) {
        if (meets_target(hash, latest_work->job.target)) {
            {
                std::lock_guard lock(state.share_mutex);
                if (state.shares.size() < kMaximumQueuedShares) {
                    state.shares.push_back({
                        latest_work->generation, latest_work->job.id, nonce, hash});
                } else {
                    state.dropped_shares.fetch_add(1, std::memory_order_relaxed);
                }
            }
            state.found.fetch_add(1, std::memory_order_relaxed);
        }
    } else if (meets_target(hash, work->job.target)) {
        state.stale_shares.fetch_add(1, std::memory_order_relaxed);
    }
    return count;
}

} // namespace

void hash_worker(MiningState& state, std::uint32_t worker_id,
                 std::uint32_t worker_count, std::uint64_t maximum_hashes) {
    if (worker_count == 0 || worker_id >= worker_count) {
        state.report_worker_error("invalid CPU worker index or worker count");
        return;
    }
    if (!platform::pin_current_thread(worker_id)) {
        std::cerr << "CPU worker " << worker_id
                  << ": affinity unavailable; continuing without pinning\n";
    }

    Vm vm;
    std::shared_ptr<const MiningWork> active_work;
    std::shared_ptr<const MiningWork> pending_work;
    std::vector<std::uint8_t> blob;
    std::uint64_t cursor = 0;
    std::uint64_t exhausted_generation = 0;
    std::uint32_t pending_nonce = 0;
    bool pipeline_active = false;
    Hash hash{};

    const auto flush_pending = [&]() {
        if (!pipeline_active || !vm || !pending_work) {
            pipeline_active = false;
            pending_work.reset();
            return std::uint64_t{0};
        }

        randomx_calculate_hash_last(vm.get(), hash.data());
        pipeline_active = false;
        auto completed_work = std::move(pending_work);
        pending_work.reset();
        const auto count = complete_hash(
            state, completed_work, pending_nonce, hash);
        if (maximum_hashes != 0 && count >= maximum_hashes) {
            state.stop.store(true, std::memory_order_release);
        }
        return count;
    };

    try {
        while (!state.stop.load(std::memory_order_acquire)) {
            const auto work = state.current_work();
            if (!work) {
                flush_pending();
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
                continue;
            }

            if (exhausted_generation == work->generation) {
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
                continue;
            }
            if (exhausted_generation != 0) exhausted_generation = 0;

            const bool job_changed =
                !active_work || active_work->generation != work->generation;
            const bool vm_flags_changed =
                !vm || !active_work || active_work->flags != work->flags ||
                active_work->full_memory != work->full_memory;
            const bool memory_changed =
                active_work && (active_work->cache != work->cache ||
                               active_work->dataset != work->dataset);

            if (pipeline_active && (job_changed || vm_flags_changed || memory_changed)) {
                flush_pending();
                if (state.stop.load(std::memory_order_acquire)) break;
            }

            if (vm_flags_changed) {
                auto vm_flags = work->flags;
                if (work->full_memory) vm_flags |= RANDOMX_FLAG_FULL_MEM;
                if (work->full_memory && !work->dataset) {
                    throw std::runtime_error("RandomX dataset is missing");
                }
                if (!work->full_memory && !work->cache) {
                    throw std::runtime_error("RandomX cache is missing");
                }
                vm.reset(randomx_create_vm(
                    vm_flags,
                    work->full_memory ? nullptr : work->cache.get(),
                    work->full_memory ? work->dataset.get() : nullptr));
                if (!vm) throw std::runtime_error("worker VM creation failed");
            } else if (memory_changed) {
                if (work->full_memory) {
                    if (!work->dataset) {
                        throw std::runtime_error("new RandomX dataset is missing");
                    }
                    randomx_vm_set_dataset(vm.get(), work->dataset.get());
                } else {
                    if (!work->cache) {
                        throw std::runtime_error("new RandomX cache is missing");
                    }
                    randomx_vm_set_cache(vm.get(), work->cache.get());
                }
            }

            if (job_changed) {
                blob = work->job.blob;
                cursor = 0;
            }
            active_work = work;

            const auto nonce64 = static_cast<std::uint64_t>(work->job.nonce) +
                worker_id + cursor * worker_count;
            if (nonce64 > std::numeric_limits<std::uint32_t>::max()) {
                flush_pending();
                exhausted_generation = work->generation;
                continue;
            }
            const auto nonce = static_cast<std::uint32_t>(nonce64);
            write_nonce(blob, nonce);

            if (!pipeline_active) {
                randomx_calculate_hash_first(vm.get(), blob.data(), blob.size());
                pipeline_active = true;
            } else {
                randomx_calculate_hash_next(
                    vm.get(), blob.data(), blob.size(), hash.data());
                if (pending_work) {
                    const auto count = complete_hash(
                        state, pending_work, pending_nonce, hash);
                    if (maximum_hashes != 0 && count >= maximum_hashes) {
                        state.stop.store(true, std::memory_order_release);
                    }
                }
            }

            pending_work = work;
            pending_nonce = nonce;
            ++cursor;
        }

        if (maximum_hashes == 0 ||
            state.hashes.load(std::memory_order_relaxed) < maximum_hashes) {
            flush_pending();
        }
    } catch (const std::exception& error) {
        try {
            if (maximum_hashes == 0 ||
                state.hashes.load(std::memory_order_relaxed) < maximum_hashes) {
                flush_pending();
            }
        } catch (...) {
        }
        state.report_worker_error(error.what());
    } catch (...) {
        try {
            if (maximum_hashes == 0 ||
                state.hashes.load(std::memory_order_relaxed) < maximum_hashes) {
                flush_pending();
            }
        } catch (...) {
        }
        state.report_worker_error("worker failed with an unknown exception");
    }
}

} // namespace unique_rx::core
