#include "topology.hpp"

#include <algorithm>
#include <cmath>
#include <thread>

namespace unique_rx {

std::uint32_t select_cpu_threads(const nlohmann::json& cpu_config) {
    if (!cpu_config.value("enabled", true)) return 0;

    const auto logical_threads = std::max(1U, std::thread::hardware_concurrency());
    const auto hint = std::min(cpu_config.value("max-threads-hint", 0U), 100U);
    const auto selected = hint == 0
        ? std::max(1U, logical_threads / 2)
        : std::clamp(static_cast<std::uint32_t>(
              std::lround(logical_threads * (hint / 100.0))),
              1U, logical_threads);

    return std::min(selected, 64U);
}

} // namespace unique_rx
