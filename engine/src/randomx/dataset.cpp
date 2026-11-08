#include "dataset.hpp"
#include "platform/affinity.hpp"

#include <algorithm>
#include <atomic>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <vector>

namespace unique_rx {

std::shared_ptr<randomx_dataset> create_randomx_dataset(
    const std::shared_ptr<randomx_cache>& cache,
    std::uint32_t initialization_threads, randomx_flags& flags) {
    if (!cache) throw std::invalid_argument("RandomX dataset requires an initialized cache");

    const auto dataset_flags = flags & RANDOMX_FLAG_LARGE_PAGES;
    auto* raw_dataset = randomx_alloc_dataset(dataset_flags);
    if (raw_dataset == nullptr && dataset_flags != RANDOMX_FLAG_DEFAULT) {
        flags = static_cast<randomx_flags>(
            static_cast<int>(flags) & ~static_cast<int>(RANDOMX_FLAG_LARGE_PAGES));
        std::cerr << "RandomX large pages unavailable; using standard dataset pages.\n";
        raw_dataset = randomx_alloc_dataset(RANDOMX_FLAG_DEFAULT);
    }
    if (raw_dataset == nullptr) {
        throw std::runtime_error("RandomX full dataset allocation failed");
    }

    std::shared_ptr<randomx_dataset> dataset(
        raw_dataset, [](randomx_dataset* value) { randomx_release_dataset(value); });

    const unsigned long item_count = randomx_dataset_item_count();
    if (item_count == 0) throw std::runtime_error("RandomX dataset has no items");

    const auto worker_count = std::max(
        1U, std::min(initialization_threads, static_cast<std::uint32_t>(item_count)));

    // RxMiner randomx.rs (MIT), pinned at 5fe52dde2650c623fd0472804152a94711889dce:
    // dynamically claim 65,536-item (4 MiB) chunks. Static equal partitions
    // leave the whole dataset build waiting for the slowest core; bounded
    // atomic claims let faster cores finish additional ranges without putting
    // synchronization inside RandomX's per-item initialization loop.
    constexpr unsigned long kDatasetChunkItems = 1UL << 16;
    std::atomic<unsigned long> next_item{0};
    std::vector<std::jthread> workers;
    workers.reserve(worker_count);
    for (std::uint32_t worker = 0; worker < worker_count; ++worker) {
        workers.emplace_back([dataset, cache, &next_item, item_count, worker] {
            // First-touch dataset pages from the same interleaved physical-core
            // order used by the mining workers. This spreads initialization
            // traffic across visible NUMA nodes when the OS permits pinning.
            (void)platform::pin_current_thread(worker);
            for (;;) {
                const auto start_item = next_item.fetch_add(
                    kDatasetChunkItems, std::memory_order_relaxed);
                if (start_item >= item_count) break;
                const auto count = std::min(
                    kDatasetChunkItems, item_count - start_item);
                randomx_init_dataset(dataset.get(), cache.get(), start_item, count);
            }
        });
    }
    for (auto& worker : workers) worker.join();

    return dataset;
}

} // namespace unique_rx
