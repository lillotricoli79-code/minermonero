#include "topology.hpp"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <optional>
#include <sstream>
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

std::optional<std::uint32_t> parse_quota(const std::string& quota_text,
                                         const std::string& period_text) {
    try {
        std::size_t q_used = 0;
        std::size_t p_used = 0;
        const auto quota = std::stoull(quota_text, &q_used, 10);
        const auto period = std::stoull(period_text, &p_used, 10);
        if (q_used != quota_text.size() || p_used != period_text.size() ||
            quota == 0 || period == 0) return std::nullopt;
        const auto count = quota / period + (quota % period != 0 ? 1ULL : 0ULL);
        return static_cast<std::uint32_t>(std::min<std::uint64_t>(
            std::max<std::uint64_t>(1, count), UINT32_MAX));
    } catch (...) {
        return std::nullopt;
    }
}

#ifdef __linux__
std::optional<std::uint32_t> quota_in_directory(const std::filesystem::path& dir) {
    {
        std::ifstream file(dir / "cpu.max");
        std::string quota, period;
        if (file >> quota >> period && quota != "max") {
            if (auto result = parse_quota(quota, period)) return result;
        }
    }
    {
        std::ifstream quota_file(dir / "cpu.cfs_quota_us");
        std::ifstream period_file(dir / "cpu.cfs_period_us");
        std::string quota, period;
        if (quota_file >> quota && period_file >> period) {
            if (auto result = parse_quota(quota, period)) return result;
        }
    }
    return std::nullopt;
}

std::optional<std::uint32_t> available_cgroup_cpu_quota() {
    std::vector<std::filesystem::path> starts;
    std::ifstream membership("/proc/self/cgroup");
    std::string line;
    while (std::getline(membership, line)) {
        const auto first = line.find(':');
        const auto second = first == std::string::npos
            ? std::string::npos : line.find(':', first + 1);
        if (first == std::string::npos || second == std::string::npos) continue;
        const auto controllers = line.substr(first + 1, second - first - 1);
        const auto relative = line.substr(second + 1);
        const auto suffix = relative.substr(relative.starts_with('/') ? 1 : 0);
        if (controllers.empty()) {
            starts.emplace_back(std::filesystem::path("/sys/fs/cgroup") / suffix);
        } else if (controllers.find("cpu") != std::string::npos) {
            starts.emplace_back(std::filesystem::path("/sys/fs/cgroup/cpu") / suffix);
        }
    }

    std::optional<std::uint32_t> best;
    for (const auto& start : starts) {
        auto current = start;
        for (;;) {
            if (const auto value = quota_in_directory(current)) {
                best = best ? std::min(*best, *value) : value;
            }
            if (current == current.root_path()) break;
            const auto parent = current.parent_path();
            if (parent == current) break;
            current = parent;
        }
    }
    if (!best) best = quota_in_directory("/sys/fs/cgroup");
    return best;
}
#endif

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

std::uint32_t limit_cpu_threads_by_quota(std::uint32_t requested,
                                         std::uint64_t quota_us,
                                         std::uint64_t period_us) {
    if (requested == 0 || quota_us == 0 || period_us == 0) return requested;
    const auto count = quota_us / period_us + (quota_us % period_us != 0 ? 1ULL : 0ULL);
    return std::min(requested, static_cast<std::uint32_t>(
        std::min<std::uint64_t>(std::max<std::uint64_t>(1, count), UINT32_MAX)));
}

std::uint32_t select_cpu_threads(const nlohmann::json& cpu_config) {
    if (!cpu_config.value("enabled", true)) return 0;

    const auto logical_threads = available_logical_threads();
    const auto physical_cores = available_physical_cores(logical_threads);
    auto selected = recommended_cpu_threads(
        logical_threads, physical_cores,
        cpu_config.value("max-threads-hint", 0U));
#ifdef __linux__
    if (const auto quota = available_cgroup_cpu_quota()) {
        selected = std::min(selected, *quota);
    }
#endif
    return selected;
}

} // namespace unique_rx
