#pragma once

#include "randomx/cache.hpp"
#include "randomx/dataset.hpp"
#include "stratum/stratum.hpp"

#include <randomx.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace unique_rx::core {

using Hash = std::array<std::uint8_t, 32>;

struct Share {
    std::uint64_t generation = 0;
    std::string job_id;
    std::uint32_t nonce = 0;
    Hash hash{};
    std::uint64_t target = 0;
};

struct DifficultyResult {
    double difficulty = 0.0;
    std::uint64_t at_unix_seconds = 0;
};

struct BestResults {
    double estimated_hashes = 0.0;
    std::vector<DifficultyResult> top_difficulties;
};

struct MiningWork {
    Job job;
    std::shared_ptr<randomx_cache> cache;
    std::shared_ptr<randomx_dataset> dataset;
    randomx_flags flags = RANDOMX_FLAG_DEFAULT;
    bool full_memory = true;
    std::uint64_t generation = 0;
};

struct MiningState {
    std::atomic<bool> stop{false};
    int cpu_priority = 2;
    std::atomic<std::uint64_t> hashes{0};
    std::atomic<std::uint64_t> found{0};
    std::atomic<std::uint64_t> submitted{0};
    std::atomic<std::uint64_t> accepted{0};
    std::atomic<std::uint64_t> rejected{0};
    std::atomic<std::uint64_t> stale_shares{0};
    std::atomic<std::uint64_t> expired_shares{0};
    std::atomic<std::uint64_t> dropped_shares{0};
    std::atomic<std::int64_t> last_share_latency_ms{-1};
    std::atomic<std::uint64_t> reconnects{0};
    std::atomic<bool> large_pages_active{false};

    mutable std::mutex best_mutex;
    BestResults best_results;
    void record_accepted_share(double actual_difficulty, double target_difficulty);
    BestResults best_snapshot() const;

    // NyxRX Metrics::register_thread port: real counters per hashing worker.
    std::vector<std::shared_ptr<std::atomic<std::uint64_t>>> worker_hashes;
    void initialize_worker_counters(std::uint32_t count);

    mutable std::mutex work_mutex;
    std::shared_ptr<const MiningWork> work;

    std::mutex share_mutex;
    std::deque<Share> shares;

    std::mutex error_mutex;
    std::string worker_error;

    std::shared_ptr<const MiningWork> current_work();
    void set_work(std::shared_ptr<const MiningWork> next);
    void report_worker_error(const std::string& error);
};

bool hash_meets_target(const Hash& hash, std::uint64_t target) noexcept;

void hash_worker(MiningState& state, std::uint32_t worker_id,
                 std::uint32_t worker_count, std::uint64_t maximum_hashes);

} // namespace unique_rx::core
