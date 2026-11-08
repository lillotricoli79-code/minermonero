#pragma once

#if defined(__linux__)
#include <cerrno>
#include <cstdint>
#include <pthread.h>
#include <sched.h>
#include <vector>

namespace unique_rx::platform::linux_cpu {

// Dynamically size CPU masks: fixed cpu_set_t / CPU_SETSIZE truncates large
// hosts and can silently omit CPUs exposed by containers or high-core servers.
inline std::vector<unsigned int> allowed_cpus() noexcept {
    for (std::size_t words = 16; words <= 16384; words *= 2) {
        std::vector<unsigned long> mask(words, 0UL);
        const auto bytes = mask.size() * sizeof(unsigned long);
        if (sched_getaffinity(0, bytes, reinterpret_cast<cpu_set_t*>(mask.data())) == 0) {
            std::vector<unsigned int> cpus;
            constexpr unsigned int bits = sizeof(unsigned long) * 8U;
            for (std::size_t word = 0; word < mask.size(); ++word) {
                auto value = mask[word];
                while (value != 0) {
                    const auto bit = static_cast<unsigned int>(__builtin_ctzl(value));
                    const auto cpu = word * bits + bit;
                    if (cpu <= UINT32_MAX) cpus.push_back(static_cast<unsigned int>(cpu));
                    value &= value - 1;
                }
            }
            return cpus;
        }
        if (errno != EINVAL) break;
    }
    return {};
}

inline bool pin_current_thread(unsigned int cpu) noexcept {
    constexpr std::size_t bits = sizeof(unsigned long) * 8U;
    if (cpu >= (1U << 20)) return false;
    std::vector<unsigned long> mask(static_cast<std::size_t>(cpu) / bits + 1, 0UL);
    mask[cpu / bits] |= 1UL << (cpu % bits);
    return pthread_setaffinity_np(
        pthread_self(), mask.size() * sizeof(unsigned long),
        reinterpret_cast<const cpu_set_t*>(mask.data())) == 0;
}

} // namespace unique_rx::platform::linux_cpu
#endif
