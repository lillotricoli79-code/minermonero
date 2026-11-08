#pragma once

#include <cstdint>
#include <vector>
#include <algorithm>
#include <map>
#include <tuple>

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
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <utility>
#endif

namespace unique_rx::platform {

#ifdef _WIN32
inline std::vector<std::pair<WORD, DWORD>> ordered_processors() {
    // Interleave physical cores across NUMA nodes before placing SMT siblings.
    std::map<WORD, std::vector<std::vector<std::pair<WORD, DWORD>>>> nodes;
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
                    WORD node = 0;
                    const auto& core = item->Processor;
                    for (WORD g = 0; g < core.GroupCount; ++g) {
                        const auto& mask = core.GroupMask[g];
                        for (DWORD bit = 0; bit < sizeof(KAFFINITY) * 8; ++bit) {
                            if ((mask.Mask & (static_cast<KAFFINITY>(1) << bit)) == 0) continue;
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
    if (result.empty()) {
        const WORD groups = GetActiveProcessorGroupCount();
        for (WORD group = 0; group < groups; ++group)
            for (DWORD cpu = 0; cpu < GetActiveProcessorCount(group); ++cpu)
                result.emplace_back(group, cpu);
    }
    return result;
}
#elif defined(__linux__)
inline std::vector<unsigned int> ordered_processors(
    const std::vector<unsigned int>& allowed) {
    // NUMA node is the outer ordering key: workers are spread over memory
    // domains before SMT siblings are considered within each physical core.
    std::map<std::tuple<int, int, int>, std::vector<unsigned int>> cores;
    for (const auto cpu : allowed) {
        const auto cpu_root = std::filesystem::path("/sys/devices/system/cpu") /
            ("cpu" + std::to_string(cpu));
        const auto topology = cpu_root / "topology";
        int package = -1, core = static_cast<int>(cpu), node = -1;
        std::ifstream(topology / "physical_package_id") >> package;
        std::ifstream(topology / "core_id") >> core;
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
        cores[{node, package, core}].push_back(cpu);
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

} // namespace unique_rx::platform
