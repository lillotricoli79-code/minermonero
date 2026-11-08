#include "mining.hpp"

#include <utility>

namespace unique_rx::core {

bool hash_meets_target(const Hash& hash, std::uint64_t target) noexcept {
    std::uint64_t value = 0;
    for (std::size_t index = 0; index < sizeof(value); ++index) {
        value |= static_cast<std::uint64_t>(hash[24 + index]) << (index * 8);
    }
    return value < target;
}

std::shared_ptr<const MiningWork> MiningState::current_work() {
    std::lock_guard lock(work_mutex);
    return work;
}

void MiningState::set_work(std::shared_ptr<const MiningWork> next) {
    std::lock_guard lock(work_mutex);
    work = std::move(next);
}

void MiningState::report_worker_error(const std::string& error) {
    {
        std::lock_guard lock(error_mutex);
        if (worker_error.empty()) worker_error = error;
    }
    stop.store(true, std::memory_order_release);
}

} // namespace unique_rx::core
