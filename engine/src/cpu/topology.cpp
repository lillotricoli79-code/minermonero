#include "topology.hpp"

#include <algorithm>
#include <cmath>
#include <thread>

#ifdef _WIN32
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#elif defined(__linux__)
#include <sched.h>
#endif

namespace unique_rx {
namespace {

std::uint32_t available_logical_threads() {
#ifdef _WIN32
    std::uint32_t count = 0;
    const WORD groups = GetActiveProcessorGroupCount();
    for (WORD group = 0; group < groups; ++group) {
        count += GetActiveProcessorCount(group);
    }
    if (count != 0) return count;
#elif defined(__linux__)
    cpu_set_t allowed;
    if (sched_getaffinity(0, sizeof(allowed), &allowed) == 0) {
        const int count = CPU_COUNT(&allowed);
        if (count > 0) return static_cast<std::uint32_t>(count);
    }
#endif
    return std::max(1U, std::thread::hardware_concurrency());
}

} // namespace

std::uint32_t select_cpu_threads(const nlohmann::json& cpu_config) {
    if (!cpu_config.value("enabled", true)) return 0;

    const auto logical_threads = available_logical_threads();
    const auto hint = std::min(cpu_config.value("max-threads-hint", 0U), 100U);
    const auto selected = hint == 0
        ? std::max(1U, logical_threads / 2)
        : std::clamp(static_cast<std::uint32_t>(
              std::lround(logical_threads * (hint / 100.0))),
              1U, logical_threads);

    return std::min(selected, 64U);
}

} // namespace unique_rx
