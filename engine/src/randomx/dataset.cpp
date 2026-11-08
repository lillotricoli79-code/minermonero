#include "dataset.hpp"

#include <algorithm>
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
    const unsigned long base_items = item_count / worker_count;
    const unsigned long remainder = item_count % worker_count;

    std::vector<std::jthread> workers;
    workers.reserve(worker_count);
    unsigned long start_item = 0;
    for (std::uint32_t worker = 0; worker < worker_count; ++worker) {
        const unsigned long count = base_items + (worker < remainder ? 1UL : 0UL);
        workers.emplace_back([dataset, cache, start_item, count] {
            randomx_init_dataset(dataset.get(), cache.get(), start_item, count);
        });
        start_item += count;
    }
    for (auto& worker : workers) worker.join();

    return dataset;
}

} // namespace unique_rx
