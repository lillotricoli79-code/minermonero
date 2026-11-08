#pragma once

#include <cstdint>
#include <vector>
#include <algorithm>

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
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <utility>
#endif

namespace unique_rx::platform {

#ifdef _WIN32
inline std::vector<std::pair<WORD, DWORD>> ordered_processors() {
    std::vector<std::vector<std::pair<WORD, DWORD>>> cores;
    DWORD bytes = 0;
    GetLogicalProcessorInformationEx(RelationProcessorCore, nullptr, &bytes);
    if (GetLastError() == ERROR_INSUFFICIENT_BUFFER && bytes != 0) {
        std::vector<unsigned char> storage(bytes);
        auto* item = reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(storage.data());
        if (GetLogicalProcessorInformationEx(RelationProcessorCore, item, &bytes)) {
            DWORD offset = 0;
            while (offset + sizeof(SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX) <= bytes) {
                item = reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(storage.data() + offset);
                if (item->Size == 0 || offset + item->Size > bytes) break;
                if (item->Relationship == RelationProcessorCore) {
                    std::vector<std::pair<WORD, DWORD>> siblings;
                    const auto& core = item->Processor;
                    for (WORD g = 0; g < core.GroupCount; ++g) {
                        const auto& mask = core.GroupMask[g];
                        for (DWORD bit = 0; bit < sizeof(KAFFINITY) * 8; ++bit) {
                            if ((mask.Mask & (static_cast<KAFFINITY>(1) << bit)) != 0)
                                siblings.emplace_back(mask.Group, bit);
                        }
                    }
                    if (!siblings.empty()) cores.push_back(std::move(siblings));
                }
                offset += item->Size;
            }
        }
    }
    std::vector<std::pair<WORD, DWORD>> result;
    for (std::size_t sibling = 0; ; ++sibling) {
        bool added = false;
        for (const auto& core : cores) {
            if (sibling < core.size()) {
                result.push_back(core[sibling]);
                added = true;
            }
        }
        if (!added) break;
    }
    if (result.empty()) {
        const WORD groups = GetActiveProcessorGroupCount();
        for (WORD group = 0; group < groups; ++group)
            for (DWORD cpu = 0; cpu < GetActiveProcessorCount(group); ++cpu)
                result.emplace_back(group, cpu);
    }
    return result;
}
#elif defined(__linux__)
inline std::vector<unsigned int> ordered_processors(const cpu_set_t& allowed) {
    std::map<std::pair<int, int>, std::vector<unsigned int>> cores;
    for (int cpu = 0; cpu < CPU_SETSIZE; ++cpu) {
        if (!CPU_ISSET(cpu, &allowed)) continue;
        const auto root = std::filesystem::path("/sys/devices/system/cpu") /
            ("cpu" + std::to_string(cpu)) / "topology";
        int package = -1, core = cpu;
        std::ifstream(root / "physical_package_id") >> package;
        std::ifstream(root / "core_id") >> core;
        cores[{package, core}].push_back(static_cast<unsigned int>(cpu));
    }
    std::vector<std::vector<unsigned int>> siblings;
    siblings.reserve(cores.size());
    for (auto& entry : cores) {
        auto& cpus = entry.second;
        std::sort(cpus.begin(), cpus.end());
        siblings.push_back(cpus);
    }
    std::vector<unsigned int> result;
    for (std::size_t sibling = 0; ; ++sibling) {
        bool added = false;
        for (const auto& core : siblings) {
            if (sibling < core.size()) {
                result.push_back(core[sibling]);
                added = true;
            }
        }
        if (!added) break;
    }
    return result;
}
#endif

inline bool pin_current_thread(std::uint32_t logical_cpu) noexcept {
    try {
#ifdef _WIN32
        const auto processors = ordered_processors();
        if (logical_cpu >= processors.size()) return false;
        GROUP_AFFINITY affinity{};
        affinity.Group = processors[logical_cpu].first;
        affinity.Mask = static_cast<KAFFINITY>(1) << processors[logical_cpu].second;
        return SetThreadGroupAffinity(GetCurrentThread(), &affinity, nullptr) != 0;
#elif defined(__linux__)
        cpu_set_t allowed;
        if (sched_getaffinity(0, sizeof(allowed), &allowed) != 0) return false;
        const auto processors = ordered_processors(allowed);
        if (logical_cpu >= processors.size()) return false;
        cpu_set_t affinity;
        CPU_ZERO(&affinity);
        CPU_SET(processors[logical_cpu], &affinity);
        return pthread_setaffinity_np(pthread_self(), sizeof(affinity), &affinity) == 0;
#else
        (void)logical_cpu;
        return false;
#endif
    } catch (...) {
        return false;
    }
}

} // namespace unique_rx::platform
