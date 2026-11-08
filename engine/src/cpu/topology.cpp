#include "topology.hpp"

#include <algorithm>
#include <cmath>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#ifdef _WIN32
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#elif defined(__linux__)
#include <fstream>
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

// RandomX benefits from starting with one worker per physical core. This
// follows the physical-core-first policy used by mature RandomX miners rather
// than assuming that half of every machine's logical CPUs are physical cores.
std::uint32_t available_physical_cores(std::uint32_t logical_fallback) {
#ifdef _WIN32
    DWORD bytes = 0;
    GetLogicalProcessorInformationEx(RelationProcessorCore, nullptr, &bytes);
    if (GetLastError() == ERROR_INSUFFICIENT_BUFFER && bytes != 0) {
        std::vector<unsigned char> storage(bytes);
        auto* item = reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(
            storage.data());
        if (GetLogicalProcessorInformationEx(RelationProcessorCore, item, &bytes)) {
            std::uint32_t count = 0;
            DWORD offset = 0;
            while (offset + sizeof(SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX) <= bytes) {
                item = reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(
                    storage.data() + offset);
                if (item->Size == 0 || offset + item->Size > bytes) break;
                if (item->Relationship == RelationProcessorCore) ++count;
                offset += item->Size;
            }
            if (count != 0) return std::min(count, logical_fallback);
        }
    }
#elif defined(__linux__)
    cpu_set_t allowed;
    if (sched_getaffinity(0, sizeof(allowed), &allowed) == 0) {
        std::set<std::pair<int, int>> cores;
        for (int cpu = 0; cpu < CPU_SETSIZE; ++cpu) {
            if (!CPU_ISSET(cpu, &allowed)) continue;
            const auto root = std::string("/sys/devices/system/cpu/cpu") +
                std::to_string(cpu) + "/topology/";
            std::ifstream package_file(root + "physical_package_id");
            std::ifstream core_file(root + "core_id");
            int package = 0;
            int core = 0;
            if (package_file >> package && core_file >> core) {
                cores.emplace(package, core);
            }
        }
        if (!cores.empty()) {
            return std::min(static_cast<std::uint32_t>(cores.size()),
                            logical_fallback);
        }
    }
#endif
    // If the OS does not expose physical topology, avoid an arbitrary 50%
    // throttle: use the available logical count and let the user tune it.
    return std::max(1U, logical_fallback);
}

} // namespace

std::uint32_t recommended_cpu_threads(std::uint32_t logical_threads,
                                     std::uint32_t physical_cores,
                                     std::uint32_t max_threads_hint) {
    logical_threads = std::max(1U, logical_threads);
    physical_cores = std::clamp(physical_cores, 1U, logical_threads);
    const auto hint = std::min(max_threads_hint, 100U);
    if (hint == 0) return physical_cores;
    return std::clamp(static_cast<std::uint32_t>(
        std::lround(logical_threads * (hint / 100.0))), 1U, logical_threads);
}

std::uint32_t select_cpu_threads(const nlohmann::json& cpu_config) {
    if (!cpu_config.value("enabled", true)) return 0;

    const auto logical_threads = available_logical_threads();
    const auto physical_cores = available_physical_cores(logical_threads);
    return recommended_cpu_threads(
        logical_threads, physical_cores,
        cpu_config.value("max-threads-hint", 0U));
}

} // namespace unique_rx
