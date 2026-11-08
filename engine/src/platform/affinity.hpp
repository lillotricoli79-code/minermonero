#pragma once

#include <cstdint>

#ifdef _WIN32
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif
#ifndef WINVER
#define WINVER _WIN32_WINNT
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#elif defined(__linux__)
#include <pthread.h>
#include <sched.h>
#endif

namespace unique_rx::platform {

inline bool pin_current_thread(std::uint32_t logical_cpu) noexcept {
#ifdef _WIN32
    DWORD remaining = logical_cpu;
    const WORD group_count = GetActiveProcessorGroupCount();
    for (WORD group = 0; group < group_count; ++group) {
        const DWORD count = GetActiveProcessorCount(group);
        if (remaining >= count) {
            remaining -= count;
            continue;
        }
        if (remaining >= sizeof(KAFFINITY) * 8) return false;
        GROUP_AFFINITY affinity{};
        affinity.Group = group;
        affinity.Mask = static_cast<KAFFINITY>(1) << remaining;
        return SetThreadGroupAffinity(GetCurrentThread(), &affinity, nullptr) != 0;
    }
    return false;
#elif defined(__linux__)
    cpu_set_t allowed;
    if (sched_getaffinity(0, sizeof(allowed), &allowed) != 0) return false;
    std::uint32_t selected = 0;
    for (std::uint32_t cpu = 0; cpu < CPU_SETSIZE; ++cpu) {
        if (!CPU_ISSET(cpu, &allowed)) continue;
        if (selected++ != logical_cpu) continue;
        cpu_set_t affinity;
        CPU_ZERO(&affinity);
        CPU_SET(cpu, &affinity);
        return pthread_setaffinity_np(pthread_self(), sizeof(affinity), &affinity) == 0;
    }
    return false;
#else
    (void)logical_cpu;
    return false;
#endif
}

} // namespace unique_rx::platform
