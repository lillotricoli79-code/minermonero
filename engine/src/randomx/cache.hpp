#pragma once

#include <randomx.h>

#include <array>
#include <cstdint>
#include <memory>

namespace unique_rx {

std::shared_ptr<randomx_cache> create_randomx_cache(
    const std::array<std::uint8_t, 32>& seed, randomx_flags& flags);

} // namespace unique_rx
