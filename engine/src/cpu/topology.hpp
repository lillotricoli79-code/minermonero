#pragma once

#include <nlohmann/json.hpp>

#include <cstdint>

namespace unique_rx {

std::uint32_t recommended_cpu_threads(std::uint32_t logical_threads,
                                     std::uint32_t physical_cores,
                                     std::uint32_t max_threads_hint);
std::uint32_t select_cpu_threads(const nlohmann::json& cpu_config);

} // namespace unique_rx
