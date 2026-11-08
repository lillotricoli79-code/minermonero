#pragma once

#include <nlohmann/json.hpp>

#include <cstdint>

namespace unique_rx {

std::uint32_t select_cpu_threads(const nlohmann::json& cpu_config);

} // namespace unique_rx
