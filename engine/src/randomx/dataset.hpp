#pragma once

#include <randomx.h>

#include <cstdint>
#include <memory>

namespace unique_rx {

std::shared_ptr<randomx_dataset> create_randomx_dataset(
    const std::shared_ptr<randomx_cache>& cache,
    std::uint32_t initialization_threads, randomx_flags& flags);

} // namespace unique_rx
