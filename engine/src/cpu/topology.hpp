#pragma once

#include <nlohmann/json.hpp>

#include <cstdint>

namespace unique_rx {

std::uint32_t recommended_cpu_threads(std::uint32_t logical_threads,
                                     std::uint32_t physical_cores,
                                     std::uint32_t max_threads_hint);
std::uint32_t limit_cpu_threads_by_quota(std::uint32_t requested,
                                         std::uint64_t quota_us,
                                         std::uint64_t period_us);
std::uint32_t select_cpu_threads(const nlohmann::json& cpu_config);

} // namespace unique_rx
