#pragma once

#include <string_view>

namespace unique_rx {

bool run_benchmark(unsigned int seconds, unsigned int lanes, bool use_jit,
                   bool full_memory, std::string_view key, std::string_view payload);

} // namespace unique_rx
