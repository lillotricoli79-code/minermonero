#include "mining.hpp"

#include <utility>

namespace unique_rx::core {

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
