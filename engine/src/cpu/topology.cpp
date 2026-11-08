#include "topology.hpp"

#include <algorithm>
#include <cstdint>
#ifdef __linux__
#include "platform/linux_cpu_set.hpp"
#endif
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
        if (quota_text.empty() || quota_text.front() == '-' ||
            period_text.empty() || period_text.front() == '-') return std::nullopt;
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
        } else {
            std::stringstream controller_list(controllers);
            std::string controller;
            bool has_cpu = false;
            while (std::getline(controller_list, controller, ',')) {
                if (controller == "cpu") has_cpu = true;
            }
            if (has_cpu) {
                starts.emplace_back(std::filesystem::path("/sys/fs/cgroup/cpu") / suffix);
                starts.emplace_back(std::filesystem::path("/sys/fs/cgroup/cpu,cpuacct") / suffix);
            }
        }
    }

    std::optional<std::uint32_t> best;
    for (const auto& start : starts) {
        auto current = start;
        for (;;) {
            if (const auto value = quota_in_directory(current)) {
                if (best) best = std::min(*best, *value);
                else best = *value;
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
    const auto cpus = platform::linux_cpu::allowed_cpus();
    if (!cpus.empty()) return static_cast<std::uint32_t>(cpus.size());
#endif
    return std::max(1U, std::thread::hardware_concurrency());
}

// RandomX benefits from starting with one worker per physical core. This
// follows the physical-core-first policy used by mature RandomX miners rather
// than assuming that half of every machine's logical CPUs are physical cores.
// NyxRX's RandomX hardware profile uses roughly 2 MiB of shared L3
// per worker as a conservative automatic thread limit. Detect the total
// visible L3 cache once, with an OS topology fallback when unavailable.
std::optional<std::uint64_t> available_l3_cache_bytes() {
#ifdef _WIN32
    DWORD bytes = 0;
    GetLogicalProcessorInformationEx(RelationCache, nullptr, &bytes);
    if (GetLastError() == ERROR_INSUFFICIENT_BUFFER && bytes != 0) {
        std::vector<unsigned char> storage(bytes);
        auto* item = reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(
            storage.data());
        if (GetLogicalProcessorInformationEx(RelationCache, item, &bytes)) {
            std::uint64_t total = 0;
            DWORD offset = 0;
            while (offset + sizeof(SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX) <= bytes) {
                item = reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(
                    storage.data() + offset);
                if (item->Size == 0 || offset + item->Size > bytes) break;
                if (item->Relationship == RelationCache &&
                    item->Cache.Level == 3 && item->Cache.CacheSize != 0) {
                    total += item->Cache.CacheSize;
                }
                offset += item->Size;
            }
            if (total != 0) return total;
        }
    }
#elif defined(__linux__)
    std::set<std::string> shared_l3;
    std::uint64_t total = 0;
    std::error_code ec;
    const std::filesystem::path cpu_root("/sys/devices/system/cpu");
    for (const auto& entry : std::filesystem::directory_iterator(cpu_root, ec)) {
        if (ec) break;
        const auto name = entry.path().filename().string();
        if (!name.starts_with("cpu") || name.size() <= 3 ||
            !std::all_of(name.begin() + 3, name.end(),
                         [](unsigned char ch) { return ch >= '0' && ch <= '9'; })) {
            continue;
        }
        const auto cache_root = entry.path() / "cache";
        std::error_code cache_ec;
        for (const auto& cache : std::filesystem::directory_iterator(cache_root, cache_ec)) {
            if (cache_ec) break;
            std::ifstream level_file(cache.path() / "level");
            unsigned int level = 0;
            if (!(level_file >> level) || level != 3) continue;
            std::ifstream type_file(cache.path() / "type");
            std::string type;
            if (!(type_file >> type) || (type != "Unified" && type != "Data")) continue;
            std::ifstream shared_file(cache.path() / "shared_cpu_list");
            std::string shared;
            if (!(shared_file >> shared)) shared = cache.path().string();
            if (!shared_l3.insert(shared).second) continue;
            std::ifstream size_file(cache.path() / "size");
            std::string size;
            if (!(size_file >> size) || size.empty()) continue;
            std::uint64_t multiplier = 1;
            const char suffix = size.back();
            if (suffix == 'K' || suffix == 'k') { multiplier = 1024; size.pop_back(); }
            else if (suffix == 'M' || suffix == 'm') { multiplier = 1024 * 1024; size.pop_back(); }
            else if (suffix == 'G' || suffix == 'g') { multiplier = 1024ULL * 1024ULL * 1024ULL; size.pop_back(); }
            try {
                std::size_t used = 0;
                const auto amount = std::stoull(size, &used, 10);
                if (used == size.size() && amount <= UINT64_MAX / multiplier)
                    total += amount * multiplier;
            } catch (...) {
            }
        }
    }
    if (total != 0) return total;
#endif
    return std::nullopt;
}

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
    const auto allowed = platform::linux_cpu::allowed_cpus();
    if (!allowed.empty()) {
        std::set<std::pair<int, int>> cores;
        for (const auto cpu : allowed) {
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
    // An explicit thread count takes precedence over the percentage hint.
    // Zero (or an absent key) keeps automatic physical-core/cache selection.
    std::uint32_t selected = 0;
    bool explicit_thread_count = false;
    if (cpu_config.contains("threads")) {
        const auto& configured = cpu_config["threads"];
        if (!configured.is_number_unsigned() && !configured.is_number_integer()) {
            throw std::invalid_argument("cpu.threads must be an integer");
        }
        std::uint64_t requested = 0;
        if (configured.is_number_unsigned()) {
            requested = configured.get<std::uint64_t>();
        } else {
            const auto signed_request = configured.get<std::int64_t>();
            if (signed_request < 0) {
                throw std::invalid_argument("cpu.threads must be between 0 and 256");
            }
            requested = static_cast<std::uint64_t>(signed_request);
        }
        if (requested > 256) {
            throw std::invalid_argument("cpu.threads must be between 0 and 256");
        }
        if (requested > 0) {
            explicit_thread_count = true;
            selected = std::min(logical_threads, static_cast<std::uint32_t>(requested));
        }
    }
    const auto max_threads_hint = cpu_config.value("max-threads-hint", 0U);
    if (selected == 0) {
        selected = recommended_cpu_threads(
            logical_threads, physical_cores, max_threads_hint);
    }
    if (!explicit_thread_count && max_threads_hint == 0) {
        constexpr std::uint64_t randomx_l3_per_thread = 2ULL * 1024ULL * 1024ULL;
        if (const auto l3 = available_l3_cache_bytes()) {
            const auto cache_threads = static_cast<std::uint32_t>(
                std::max<std::uint64_t>(1, *l3 / randomx_l3_per_thread));
            selected = std::min(selected, cache_threads);
        }
    }
#ifdef __linux__
    if (const auto quota = available_cgroup_cpu_quota()) {
        selected = std::min(selected, *quota);
    }
#endif
    return selected;
}

} // namespace unique_rx
