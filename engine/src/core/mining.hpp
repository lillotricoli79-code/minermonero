#pragma once

#include "stratum/stratum.hpp"

#include <randomx.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>

namespace unique_rx::core {

using Hash = std::array<std::uint8_t, RANDOMX_HASH_SIZE>;

bool hash_meets_target(const Hash& hash, std::uint64_t target) noexcept;

struct MiningWork {
    Job job;
    std::shared_ptr<randomx_cache> cache;
    std::shared_ptr<randomx_dataset> dataset;
    randomx_flags flags;
    bool full_memory;
    std::uint64_t generation;
};

struct Share {
    std::uint64_t generation;
    std::string job_id;
    std::uint32_t nonce;
    Hash hash;
};

struct MiningState {
    std::mutex work_mutex;
    std::shared_ptr<const MiningWork> work;
    std::atomic<bool> stop{false};
    std::atomic<std::uint64_t> hashes{0};
    std::atomic<std::uint64_t> found{0};
    std::atomic<std::uint64_t> submitted{0};
    std::atomic<std::uint64_t> accepted{0};
    std::atomic<std::uint64_t> rejected{0};
    std::atomic<std::uint64_t> stale_shares{0};
    std::atomic<std::uint64_t> expired_shares{0};
    std::atomic<std::uint64_t> dropped_shares{0};
    std::mutex share_mutex;
    std::deque<Share> shares;
    std::mutex error_mutex;
    std::string worker_error;

    std::shared_ptr<const MiningWork> current_work();
    void set_work(std::shared_ptr<const MiningWork> next);
    void report_worker_error(const std::string& error);
};

void hash_worker(MiningState& state, std::uint32_t worker_id,
                 std::uint32_t worker_count, std::uint64_t maximum_hashes);

} // namespace unique_rx::core
