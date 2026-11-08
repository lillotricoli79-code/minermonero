#include "cache.hpp"

#include <iostream>
#include <stdexcept>

namespace unique_rx {

std::shared_ptr<randomx_cache> create_randomx_cache(
    const std::array<std::uint8_t, 32>& seed, randomx_flags& flags) {
    auto* cache = randomx_alloc_cache(flags);
    if (cache == nullptr && (flags & RANDOMX_FLAG_LARGE_PAGES) != 0) {
        flags = static_cast<randomx_flags>(
            static_cast<int>(flags) & ~static_cast<int>(RANDOMX_FLAG_LARGE_PAGES));
        std::cerr << "RandomX large pages unavailable; using standard cache pages.\\n";
        cache = randomx_alloc_cache(flags);
    }
    if (cache == nullptr) {
        throw std::runtime_error("RandomX cache allocation failed");
    }

    std::shared_ptr<randomx_cache> owned_cache(
        cache, [](randomx_cache* value) { randomx_release_cache(value); });
    randomx_init_cache(owned_cache.get(), seed.data(), seed.size());
    return owned_cache;
}

} // namespace unique_rx
