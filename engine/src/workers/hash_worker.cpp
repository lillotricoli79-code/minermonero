#include "core/mining.hpp"
#include "logging/log.hpp"
#include "platform/affinity.hpp"
#include "virtual_memory.h"

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

// RxMiner's bounded Submitter channel uses QUEUE_LIMIT=8192.
inline constexpr std::size_t kMaximumQueuedShares = 8192;

std::uint64_t complete_hash(MiningState& state, std::uint32_t worker_id,
                             std::uint64_t& local_worker_hashes,
                             std::uint64_t& unpublished_hashes,
                             const MiningWork& work,
                             std::uint32_t nonce, const Hash& hash,
                             std::uint64_t maximum_hashes) {
    std::uint64_t count = 0;
    if (maximum_hashes != 0) {
        // Finite benchmark/test runs need an exact global ceiling. Normal pool
        // mining does not: avoid a contended atomic read-modify-write on every
        // RandomX hash by publishing each worker's counter in small batches.
        count = state.hashes.load(std::memory_order_relaxed);
        for (;;) {
            if (count >= maximum_hashes) {
                state.stop.store(true, std::memory_order_release);
                return count;
            }
            if (state.hashes.compare_exchange_weak(
                    count, count + 1, std::memory_order_relaxed,
                    std::memory_order_relaxed)) {
                ++count;
                break;
            }
        }
    } else {
        // Normal mining has no global hash ceiling. Keep the hot path entirely
        // worker-local: reading state.hashes here made every worker contend on
        // the same cache line once per hash even though counters are published
        // in batches. The dashboard can tolerate this bounded publication lag.
        count = 0;
    }

    ++local_worker_hashes;
    if (maximum_hashes == 0) {
        ++unpublished_hashes;
        if (unpublished_hashes >= 64) {
            state.hashes.fetch_add(unpublished_hashes, std::memory_order_relaxed);
            unpublished_hashes = 0;
        }
        count = local_worker_hashes;
    }
    if (worker_id < state.worker_hashes.size() && state.worker_hashes[worker_id] &&
        ((local_worker_hashes & 0x3fU) == 0 ||
         (maximum_hashes != 0 && count >= maximum_hashes))) {
        state.worker_hashes[worker_id]->store(local_worker_hashes, std::memory_order_relaxed);
    }

    // Almost every RandomX result is below the pool's share target. Test it
    // before taking either shared mutex: the hot path must not serialize all
    // CPU workers on a queue lock or a current-work lookup.
    if (hash_meets_target(hash, work.job.target)) {
        // Serialize only actual share candidates with queue insertion. This
        // closes the reconnect/job-generation race without locking per hash.
        std::lock_guard lock(state.share_mutex);
        const auto latest_work = state.current_work();
        if (latest_work && latest_work->generation == work.generation) {
            if (state.shares.size() < kMaximumQueuedShares) {
                state.shares.push_back({
                    latest_work->generation, latest_work->job.id, nonce, hash, latest_work->job.target});
            } else {
                state.dropped_shares.fetch_add(1, std::memory_order_relaxed);
            }
            state.found.fetch_add(1, std::memory_order_relaxed);
        } else {
            state.stale_shares.fetch_add(1, std::memory_order_relaxed);
        }
    }
    return count;
}

struct WorkerCounterPublisher {
    MiningState* state = nullptr;
    std::atomic<std::uint64_t>* published = nullptr;
    std::uint64_t* local = nullptr;
    std::uint64_t* unpublished = nullptr;
    ~WorkerCounterPublisher() {
        if (state && unpublished && *unpublished != 0) {
            state->hashes.fetch_add(*unpublished, std::memory_order_relaxed);
        }
        if (published && local) published->store(*local, std::memory_order_relaxed);
    }
};

} // namespace

// RandomX pipelined worker loop adapted from XMRig CpuWorker<N>::start()
// (src/backend/cpu/CpuWorker.cpp): preserve the first/next/last VM pipeline
// and per-worker nonce stream, integrated with UniqueRX generation-safe work
// handoff and bounded share queue. GPL-3.0; see LICENSES/XMRIG-GPL-3.0.txt.
void hash_worker(MiningState& state, std::uint32_t worker_id,
                 std::uint32_t worker_count, std::uint64_t maximum_hashes,
                 std::atomic<bool>& worker_stop,
                 std::optional<std::int64_t> affinity_override) {
    if (worker_count == 0 || worker_id >= worker_count) {
        state.report_worker_error("invalid CPU worker index or worker count");
        return;
    }
    std::uint64_t local_worker_hashes = 0;
    std::uint64_t unpublished_hashes = 0;
    auto* published_worker_counter =
        worker_id < state.worker_hashes.size() && state.worker_hashes[worker_id]
            ? state.worker_hashes[worker_id].get() : nullptr;
    WorkerCounterPublisher counter_publisher{
        &state, published_worker_counter, &local_worker_hashes, &unpublished_hashes};
    const bool pinned = affinity_override
        ? (*affinity_override >= 0 &&
           platform::pin_current_thread_by_id(static_cast<std::uint32_t>(*affinity_override)))
        : platform::pin_current_thread(worker_id);
    int applied_priority = state.cpu_priority.load(std::memory_order_relaxed);
    (void)platform::set_current_thread_priority(applied_priority);
    // Scratchpad mappings are reused only by workers on the same NUMA node.
    const int allocator_node = state.cpu_numa_enabled.load(std::memory_order_relaxed) && pinned
        ? (affinity_override
            ? platform::numa_node_for_cpu_id(static_cast<std::uint32_t>(*affinity_override))
            : platform::numa_node_for_logical_cpu(worker_id))
        : -1;
    rxm_set_allocator_node(allocator_node);

    Vm vm;
    std::shared_ptr<const MiningWork> active_work;
    std::shared_ptr<const MiningWork> published_work;
    const MiningWork* pending_work = nullptr;
    std::uint64_t observed_work_revision = UINT64_MAX;
    std::vector<std::uint8_t> blob;
    std::uint64_t exhausted_generation = 0;
    std::uint64_t nonce_block_generation = 0;
    std::uint64_t nonce_block_next = 0;
    std::uint64_t nonce_block_end = 0;
    // Match XMRig CpuWorker's 32768-nonce reservation cadence while
    // keeping generation-safe, shared allocation for live worker resizing.
    constexpr std::uint64_t kNonceReservationSize = 32768;
    std::uint32_t pending_nonce = 0;
    bool pipeline_active = false;
    std::uint64_t active_vm_config_generation = UINT64_MAX;
    Hash hash{};

    const auto flush_pending = [&]() {
        if (!pipeline_active || !vm || pending_work == nullptr) {
            pipeline_active = false;
            pending_work = nullptr;
            return std::uint64_t{0};
        }

        randomx_calculate_hash_last(vm.get(), hash.data());
        pipeline_active = false;
        const MiningWork* completed_work = pending_work;
        pending_work = nullptr;
        const auto count = complete_hash(
            state, worker_id, local_worker_hashes, unpublished_hashes, *completed_work, pending_nonce, hash, maximum_hashes);
        if (maximum_hashes != 0 && count >= maximum_hashes) {
            state.stop.store(true, std::memory_order_release);
        }
        // XMRig CpuWorker honors cpu.yield between completed hash rounds.
        // Keep the default off for maximum throughput; enable only when the
        // imported XMRig configuration explicitly requests it.
        if (state.cpu_yield.load(std::memory_order_relaxed)) {
            std::this_thread::yield();
        }
        return count;
    };

    try {
        while (!state.stop.load(std::memory_order_acquire) &&
               !worker_stop.load(std::memory_order_acquire)) {
            const auto work_revision = state.work_revision.load(std::memory_order_acquire);
            if (work_revision != observed_work_revision) {
                published_work = state.current_work();
                observed_work_revision = work_revision;
            }
            const auto& work = published_work;
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

            // Keep shared_ptr reference counting out of the per-hash hot path.
            // active_work owns the immutable snapshot while pending_work is a
            // non-owning pointer into it; finish the old pipeline before replacing it.
            const bool work_snapshot_changed =
                !active_work || active_work.get() != work.get();
            const auto requested_vm_config_generation =
                state.vm_config_generation.load(std::memory_order_acquire);
            const bool vm_flags_changed =
                !vm || !active_work || active_work->flags != work->flags ||
                active_work->full_memory != work->full_memory ||
                active_vm_config_generation != requested_vm_config_generation;
            const bool memory_changed =
                active_work && (active_work->cache != work->cache ||
                               active_work->dataset != work->dataset);

            if (pipeline_active && (work_snapshot_changed || vm_flags_changed || memory_changed)) {
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
                if (!vm && (vm_flags & RANDOMX_FLAG_LARGE_PAGES)) {
                    // Large pages are optional for the worker scratchpad too.
                    // Retry with ordinary pages instead of failing the miner.
                    vm_flags = static_cast<randomx_flags>(
                        static_cast<int>(vm_flags) &
                        ~static_cast<int>(RANDOMX_FLAG_LARGE_PAGES));
                    vm.reset(randomx_create_vm(
                        vm_flags,
                        work->full_memory ? nullptr : work->cache.get(),
                        work->full_memory ? work->dataset.get() : nullptr));
                    if (vm) {
                        state.large_pages_active.store(false, std::memory_order_relaxed);
                        std::cerr << "RandomX worker scratchpad huge pages unavailable; "
                                     "using standard pages.\n";
                    }
                }
                if (!vm) throw std::runtime_error("worker VM creation failed");
                active_vm_config_generation = requested_vm_config_generation;
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

            if (work_snapshot_changed) {
                blob = work->job.blob;
                active_work = work;
            }

            // Each worker reserves a small disjoint nonce range. Hashes
            // inside the range require no shared atomic operation; new ranges
            // remain unique when the worker count changes at runtime.
            if (nonce_block_generation != work->generation ||
                nonce_block_next >= nonce_block_end) {
                nonce_block_generation = work->generation;
                nonce_block_next = 0;
                nonce_block_end = 0;
                const auto block_start = state.next_nonce_offset(
                    work->generation, kNonceReservationSize);
                if (block_start == UINT64_MAX ||
                    block_start >= nonce_space(work->job)) {
                    flush_pending();
                    exhausted_generation = work->generation;
                    continue;
                }
                nonce_block_next = block_start;
                nonce_block_end = block_start + std::min(
                    kNonceReservationSize, nonce_space(work->job) - block_start);
            }
            const auto nonce_offset = nonce_block_next++;
            if ((nonce_offset & 0xffU) == 0) {
                const int requested_priority = state.cpu_priority.load(std::memory_order_relaxed);
                if (requested_priority != applied_priority) {
                    if (platform::set_current_thread_priority(requested_priority)) {
                        applied_priority = requested_priority;
                    } else {
                        logging::warning("cpu", "worker #" + std::to_string(worker_id) +
                            " could not apply updated priority " + std::to_string(requested_priority));
                    }
                }
            }
            const auto nonce = nonce_at_offset(work->job, nonce_offset);
            write_nonce(blob, nonce);

            if (!pipeline_active) {
                randomx_calculate_hash_first(vm.get(), blob.data(), blob.size());
                pipeline_active = true;
            } else {
                randomx_calculate_hash_next(
                    vm.get(), blob.data(), blob.size(), hash.data());
                if (pending_work != nullptr) {
                    const auto count = complete_hash(
                        state, worker_id, local_worker_hashes, unpublished_hashes, *pending_work, pending_nonce, hash, maximum_hashes);
                    if (maximum_hashes != 0 && count >= maximum_hashes) {
                        state.stop.store(true, std::memory_order_release);
                    }
                }
            }

            pending_work = active_work.get();
            pending_nonce = nonce;
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
