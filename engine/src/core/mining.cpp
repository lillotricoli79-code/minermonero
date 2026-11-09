#include "mining.hpp"

#include <utility>
#include <algorithm>
#include <chrono>
#include <cmath>

namespace unique_rx::core {

bool hash_meets_target(const Hash& hash, std::uint64_t target) noexcept {
    std::uint64_t value = 0;
    for (std::size_t index = 0; index < sizeof(value); ++index) {
        value |= static_cast<std::uint64_t>(hash[24 + index]) << (index * 8);
    }
    return value < target;
}

void MiningState::record_accepted_share(double actual_difficulty, double target_difficulty) {
    const auto now = std::chrono::system_clock::now().time_since_epoch();
    const auto unix_seconds = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::seconds>(now).count());
    std::lock_guard lock(best_mutex);
    if (std::isfinite(target_difficulty) && target_difficulty > 0.0) {
        best_results.estimated_hashes += target_difficulty;
    }
    if (!std::isfinite(actual_difficulty) || actual_difficulty <= 0.0) return;
    best_results.top_difficulties.push_back({actual_difficulty, unix_seconds});
    std::sort(best_results.top_difficulties.begin(), best_results.top_difficulties.end(),
        [](const DifficultyResult& left, const DifficultyResult& right) {
            return left.difficulty > right.difficulty;
        });
    if (best_results.top_difficulties.size() > 10) best_results.top_difficulties.resize(10);
}

BestResults MiningState::best_snapshot() const {
    std::lock_guard lock(best_mutex);
    return best_results;
}

void MiningState::initialize_worker_counters(std::uint32_t count) {
    worker_hashes.clear();
    worker_hashes.reserve(count);
    for (std::uint32_t worker = 0; worker < count; ++worker) {
        worker_hashes.push_back(std::make_shared<std::atomic<std::uint64_t>>(0));
    }
}

std::shared_ptr<const MiningWork> MiningState::current_work() {
    std::lock_guard lock(work_mutex);
    return work;
}

void MiningState::set_work(std::shared_ptr<const MiningWork> next) {
    std::lock_guard lock(work_mutex);
    if (next && (!work || work->generation != next->generation)) {
        std::lock_guard nonce_lock(nonce_mutex);
        nonce_cursor.store(0, std::memory_order_relaxed);
        nonce_generation.store(next->generation, std::memory_order_release);
    }
    work = std::move(next);
}

std::uint64_t MiningState::next_nonce_offset(std::uint64_t generation) {
    if (nonce_generation.load(std::memory_order_acquire) != generation) {
        std::lock_guard lock(nonce_mutex);
        if (nonce_generation.load(std::memory_order_relaxed) != generation) {
            nonce_cursor.store(0, std::memory_order_relaxed);
            nonce_generation.store(generation, std::memory_order_release);
        }
    }
    return nonce_cursor.fetch_add(1, std::memory_order_relaxed);
}

void MiningState::report_worker_error(const std::string& error) {
    {
        std::lock_guard lock(error_mutex);
        if (worker_error.empty()) worker_error = error;
    }
    stop.store(true, std::memory_order_release);
}

} // namespace unique_rx::core
