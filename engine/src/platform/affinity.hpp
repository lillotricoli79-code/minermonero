#pragma once

#include <cstdint>
#include <vector>
#include <algorithm>
#include <map>
#include <set>
#include <tuple>
#include <utility>

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
#include "platform/linux_cpu_set.hpp"
#include <pthread.h>
#include <sched.h>
#include <sys/resource.h>
#include <filesystem>
#include <fstream>
#include <string>
#endif

namespace unique_rx::platform {

// C++ adaptation of NyxRX hardware.rs HardwareProfile::detect_worker_cpu_order
// and detect_numa_nodes (MIT; see LICENSES/NYXRX-MIT.txt). It preserves the
// physical-core-first, NUMA-aware worker ordering while using native Windows
// processor groups and Linux process cpusets/sysfs.
// Upstream: https://github.com/MaleficentAd1803/nyxrx
#ifdef _WIN32
inline std::vector<std::pair<WORD, DWORD>> available_processors() {
    // Windows 10 normally constrains a process to one processor group;
    // Windows 11 can span groups. Query the groups actually assigned to this
    // process instead of counting every active CPU in the whole machine.
    std::vector<WORD> groups(64);
    USHORT group_count = static_cast<USHORT>(groups.size());
    if (GetProcessGroupAffinity(GetCurrentProcess(), &group_count, groups.data()) &&
        group_count != 0) {
        groups.resize(group_count);
    } else {
        groups.clear();
        const WORD active_groups = GetActiveProcessorGroupCount();
        for (WORD group = 0; group < active_groups; ++group) groups.push_back(group);
    }

    DWORD_PTR process_mask = 0;
    DWORD_PTR system_mask = 0;
    const bool has_single_group_mask = groups.size() == 1 &&
        GetProcessAffinityMask(GetCurrentProcess(), &process_mask, &system_mask) != 0 &&
        process_mask != 0;

    std::vector<std::pair<WORD, DWORD>> result;
    for (const WORD group : groups) {
        const DWORD active = GetActiveProcessorCount(group);
        const DWORD limit = std::min<DWORD>(
            active, static_cast<DWORD>(sizeof(KAFFINITY) * 8));
        for (DWORD processor = 0; processor < limit; ++processor) {
            if (has_single_group_mask &&
                (process_mask & (static_cast<DWORD_PTR>(1) << processor)) == 0) {
                continue;
            }
            result.emplace_back(group, processor);
        }
    }
    return result;
}

inline std::vector<std::pair<WORD, DWORD>> ordered_processors() {
    // Interleave physical cores across NUMA nodes before placing SMT siblings.
    const auto allowed = available_processors();
    const std::set<std::pair<WORD, DWORD>> allowed_set(allowed.begin(), allowed.end());
    std::map<WORD, std::vector<std::vector<std::pair<WORD, DWORD>>>> nodes;
    DWORD bytes = 0;
    GetLogicalProcessorInformationEx(RelationProcessorCore, nullptr, &bytes);
    if (GetLastError() == ERROR_INSUFFICIENT_BUFFER && bytes != 0) {
        std::vector<unsigned char> storage(bytes);
        auto* item = reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(storage.data());
        if (GetLogicalProcessorInformationEx(RelationProcessorCore, item, &bytes)) {
            DWORD offset = 0;
            while (offset < bytes) {
                item = reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(storage.data() + offset);
                if (bytes - offset < sizeof(DWORD) * 2 ||
                    item->Size < sizeof(DWORD) * 2 ||
                    item->Size > bytes - offset) break;
                if (item->Relationship == RelationProcessorCore) {
                    std::vector<std::pair<WORD, DWORD>> siblings;
                    WORD node = 0;
                    const auto& core = item->Processor;
                    for (WORD g = 0; g < core.GroupCount; ++g) {
                        const auto& mask = core.GroupMask[g];
                        for (DWORD bit = 0; bit < sizeof(KAFFINITY) * 8; ++bit) {
                            if ((mask.Mask & (static_cast<KAFFINITY>(1) << bit)) == 0 ||
                                !allowed_set.contains({mask.Group, bit})) continue;
                            PROCESSOR_NUMBER processor{};
                            processor.Group = mask.Group;
                            processor.Number = static_cast<BYTE>(bit);
                            USHORT processor_node = 0;
                            if (GetNumaProcessorNodeEx(&processor, &processor_node)) {
                                node = processor_node;
                            }
                            siblings.emplace_back(mask.Group, bit);
                        }
                    }
                    if (!siblings.empty()) nodes[node].push_back(std::move(siblings));
                }
                offset += item->Size;
            }
        }
    }
    std::vector<std::pair<WORD, DWORD>> result;
    for (std::size_t sibling = 0; ; ++sibling) {
        bool added = false;
        for (const auto& [node, cores] : nodes) {
            (void)node;
            for (const auto& core : cores) {
                if (sibling < core.size()) {
                    result.push_back(core[sibling]);
                    added = true;
                }
            }
        }
        if (!added) break;
    }
    if (result.empty()) result = allowed;
    return result;
}
#elif defined(__linux__)
inline std::vector<unsigned int> ordered_processors(
    const std::vector<unsigned int>& allowed) {
    // NUMA node/package remain the outer ordering keys. Within each memory
    // domain, prioritize physical cores with the highest advertised maximum
    // frequency before SMT siblings. The frequency ranking is a C++ port of
    // XMRMiner getBigCoreIds() (GPL-3.0; see LICENSES/XMR-MINER-GPL-3.0.txt).
    struct CoreInfo {
        int node = -1;
        int package = -1;
        int core = -1;
        std::uint64_t max_frequency_khz = 0;
        std::vector<unsigned int> siblings;
    };
    std::map<std::tuple<int, int, int>, CoreInfo> grouped;
    for (const auto cpu : allowed) {
        const auto cpu_root = std::filesystem::path("/sys/devices/system/cpu") /
            ("cpu" + std::to_string(cpu));
        const auto topology = cpu_root / "topology";
        int package = -1, core = static_cast<int>(cpu), node = -1;
        std::ifstream(topology / "physical_package_id") >> package;
        std::ifstream(topology / "core_id") >> core;

        std::uint64_t max_frequency = 0;
        std::ifstream frequency_file(cpu_root / "cpufreq/cpuinfo_max_freq");
        frequency_file >> max_frequency;

        std::error_code ec;
        for (const auto& entry : std::filesystem::directory_iterator(cpu_root, ec)) {
            const auto name = entry.path().filename().string();
            if (name.size() > 4 && name.starts_with("node") &&
                std::all_of(name.begin() + 4, name.end(),
                    [](unsigned char ch) { return ch >= '0' && ch <= '9'; })) {
                try { node = std::stoi(name.substr(4)); } catch (...) { node = -1; }
                break;
            }
        }
        auto& group = grouped[{node, package, core}];
        group.node = node;
        group.package = package;
        group.core = core;
        group.max_frequency_khz = std::max(group.max_frequency_khz, max_frequency);
        group.siblings.push_back(cpu);
    }

    std::vector<CoreInfo> cores;
    cores.reserve(grouped.size());
    for (auto& entry : grouped) {
        auto& core = entry.second;
        std::sort(core.siblings.begin(), core.siblings.end());
        cores.push_back(std::move(core));
    }
    std::stable_sort(cores.begin(), cores.end(), [](const CoreInfo& left, const CoreInfo& right) {
        if (left.node != right.node) return left.node < right.node;
        if (left.package != right.package) return left.package < right.package;
        if (left.max_frequency_khz != right.max_frequency_khz)
            return left.max_frequency_khz > right.max_frequency_khz;
        return left.core < right.core;
    });

    std::vector<unsigned int> result;
    result.reserve(allowed.size());
    for (std::size_t sibling = 0; ; ++sibling) {
        bool added = false;
        for (const auto& core : cores) {
            if (sibling < core.siblings.size()) {
                result.push_back(core.siblings[sibling]);
                added = true;
            }
        }
        if (!added) break;
    }
    return result;
}
#endif

inline int numa_node_for_logical_cpu(std::uint32_t logical_cpu) noexcept {
    try {
#ifdef _WIN32
        const auto processors = ordered_processors();
        if (logical_cpu >= processors.size()) return -1;
        PROCESSOR_NUMBER processor{};
        processor.Group = processors[logical_cpu].first;
        processor.Number = static_cast<BYTE>(processors[logical_cpu].second);
        USHORT node = 0;
        return GetNumaProcessorNodeEx(&processor, &node) ? static_cast<int>(node) : -1;
#elif defined(__linux__)
        const auto allowed = linux_cpu::allowed_cpus();
        const auto processors = ordered_processors(allowed);
        if (logical_cpu >= processors.size()) return -1;
        const auto cpu_root = std::filesystem::path("/sys/devices/system/cpu") /
            ("cpu" + std::to_string(processors[logical_cpu]));
        std::error_code ec;
        for (const auto& entry : std::filesystem::directory_iterator(cpu_root, ec)) {
            const auto name = entry.path().filename().string();
            if (name.size() > 4 && name.starts_with("node") &&
                std::all_of(name.begin() + 4, name.end(),
                    [](unsigned char ch) { return ch >= '0' && ch <= '9'; })) {
                return std::stoi(name.substr(4));
            }
        }
#endif
    } catch (...) {
    }
    return -1;
}

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
        const auto allowed = linux_cpu::allowed_cpus();
        const auto processors = ordered_processors(allowed);
        if (logical_cpu >= processors.size()) return false;
        return linux_cpu::pin_current_thread(processors[logical_cpu]);
#else
        (void)logical_cpu;
        return false;
#endif
    } catch (...) {
        return false;
    }
}

// XMRig Platform_unix.cpp / Platform_win.cpp priority mapping (GPL-3.0;
// see LICENSES/XMRIG-GPL-3.0.txt). Priority -1 preserves the OS default.
inline bool set_current_thread_priority(int priority) noexcept {
    if (priority == -1) return true;
    try {
#ifdef _WIN32
        int native_priority = THREAD_PRIORITY_IDLE;
        switch (priority) {
        case 1: native_priority = THREAD_PRIORITY_BELOW_NORMAL; break;
        case 2: native_priority = THREAD_PRIORITY_NORMAL; break;
        case 3: native_priority = THREAD_PRIORITY_ABOVE_NORMAL; break;
        case 4: native_priority = THREAD_PRIORITY_HIGHEST; break;
        case 5: native_priority = THREAD_PRIORITY_TIME_CRITICAL; break;
        default: return false;
        }
        return SetThreadPriority(GetCurrentThread(), native_priority) != 0;
#elif defined(__linux__)
        int nice_value = 19;
        switch (priority) {
        case 0: nice_value = 19; break;
        case 1: nice_value = 5; break;
        case 2: nice_value = 0; break;
        case 3: nice_value = -5; break;
        case 4: nice_value = -10; break;
        case 5: nice_value = -15; break;
        default: return false;
        }
        const bool nice_applied = setpriority(PRIO_PROCESS, 0, nice_value) == 0;
#ifdef SCHED_IDLE
        if (priority == 0) {
            sched_param parameters{};
            if (sched_setscheduler(0, SCHED_IDLE, &parameters) != 0) {
                (void)sched_setscheduler(0, SCHED_BATCH, &parameters);
            }
        }
#endif
        return nice_applied;
#else
        (void)priority;
        return false;
#endif
    } catch (...) {
        return false;
    }
}

} // namespace unique_rx::platform
