#include "topology.hpp"
#include "platform/affinity.hpp"
#include "cpu.hpp"

#include <algorithm>
#include <array>
#include <cstring>
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
#include <tuple>
#include <utility>
#include <vector>

#if defined(_M_X64) || defined(__x86_64__)
  #if defined(_MSC_VER)
    #include <intrin.h>
  #else
    #include <cpuid.h>
  #endif
#endif

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
    const auto processors = platform::available_processors();
    if (!processors.empty()) {
        return static_cast<std::uint32_t>(processors.size());
    }
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
            while (offset < bytes) {
                item = reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(
                    storage.data() + offset);
                if (bytes - offset < sizeof(DWORD) * 2 ||
                    item->Size < sizeof(DWORD) * 2 ||
                    item->Size > bytes - offset) break;
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
    std::set<unsigned int> allowed_cpus;
    for (const auto cpu : platform::linux_cpu::allowed_cpus()) {
        allowed_cpus.insert(cpu);
    }
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
        // Count only cache domains that overlap the CPUs visible to this
        // process. A container restricted to one NUMA node must not use the
        // host's unrelated L3 caches to overestimate its RandomX cache budget.
        if (!allowed_cpus.empty()) {
            try {
                const auto cpu_id = static_cast<unsigned int>(
                    std::stoul(name.substr(3)));
                if (!allowed_cpus.contains(cpu_id)) continue;
            } catch (...) {
                continue;
            }
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
    const auto allowed = platform::available_processors();
    const std::set<std::pair<WORD, DWORD>> allowed_set(allowed.begin(), allowed.end());
    DWORD bytes = 0;
    GetLogicalProcessorInformationEx(RelationProcessorCore, nullptr, &bytes);
    if (GetLastError() == ERROR_INSUFFICIENT_BUFFER && bytes != 0) {
        std::vector<unsigned char> storage(bytes);
        auto* item = reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(
            storage.data());
        if (GetLogicalProcessorInformationEx(RelationProcessorCore, item, &bytes)) {
            std::uint32_t count = 0;
            DWORD offset = 0;
            while (offset < bytes) {
                item = reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(
                    storage.data() + offset);
                if (bytes - offset < sizeof(DWORD) * 2 ||
                    item->Size < sizeof(DWORD) * 2 ||
                    item->Size > bytes - offset) break;
                if (item->Relationship == RelationProcessorCore) {
                    bool visible = false;
                    for (WORD group = 0; group < item->Processor.GroupCount; ++group) {
                        const auto& mask = item->Processor.GroupMask[group];
                        for (DWORD bit = 0; bit < sizeof(KAFFINITY) * 8; ++bit) {
                            if ((mask.Mask & (static_cast<KAFFINITY>(1) << bit)) != 0 &&
                                allowed_set.contains({mask.Group, bit})) {
                                visible = true;
                                break;
                            }
                        }
                        if (visible) break;
                    }
                    if (visible) ++count;
                }
                offset += item->Size;
            }
            if (count != 0) return std::min(count, logical_fallback);
        }
    }
#elif defined(__linux__)
    const auto allowed = platform::linux_cpu::allowed_cpus();
    if (!allowed.empty()) {
        std::set<std::tuple<int, int, int>> cores;
        for (const auto cpu : allowed) {
            const auto cpu_root = std::filesystem::path("/sys/devices/system/cpu") /
                ("cpu" + std::to_string(cpu));
            const auto topology = cpu_root / "topology";
            std::ifstream package_file(topology / "physical_package_id");
            std::ifstream core_file(topology / "core_id");
            int package = 0;
            int core = 0;
            int node = -1;
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
            if (package_file >> package && core_file >> core) {
                cores.emplace(node, package, core);
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

std::uint32_t detected_logical_threads() {
    return available_logical_threads();
}

nlohmann::json cpu_summary_json() {
    std::string vendor = "Unknown";
    std::string brand;
    std::uint32_t family = 0, model = 0, stepping = 0;
#if defined(_M_X64) || defined(__x86_64__)
    const auto cpuid = [](int leaf, int subleaf, int info[4]) {
    #if defined(_MSC_VER)
        __cpuidex(info, leaf, subleaf);
    #else
        __cpuid_count(leaf, subleaf, info[0], info[1], info[2], info[3]);
    #endif
    };
    int info[4]{};
    cpuid(0, 0, info);
    const int max_leaf = info[0];
    char vendor_id[13]{};
    std::memcpy(vendor_id + 0, &info[1], 4);
    std::memcpy(vendor_id + 4, &info[3], 4);
    std::memcpy(vendor_id + 8, &info[2], 4);
    vendor = vendor_id;
    if (vendor == "GenuineIntel") vendor = "Intel";
    else if (vendor == "AuthenticAMD") vendor = "AMD";
    if (max_leaf >= 1) {
        cpuid(1, 0, info);
        const auto eax = static_cast<std::uint32_t>(info[0]);
        const auto base_family = (eax >> 8) & 0x0f;
        const auto base_model = (eax >> 4) & 0x0f;
        const auto extended_family = (eax >> 20) & 0xff;
        const auto extended_model = (eax >> 16) & 0x0f;
        family = base_family == 0x0f ? base_family + extended_family : base_family;
        model = (base_family == 0x06 || base_family == 0x0f)
            ? (extended_model << 4) | base_model : base_model;
        stepping = eax & 0x0f;
    }
    int extended[4]{};
    cpuid(0x80000000, 0, extended);
    if (static_cast<std::uint32_t>(extended[0]) >= 0x80000004U) {
        char brand_id[49]{};
        for (int leaf = 0; leaf < 3; ++leaf) {
            cpuid(0x80000002 + leaf, 0, info);
            std::memcpy(brand_id + leaf * 16, info, 16);
        }
        brand = brand_id;
        const auto first = brand.find_first_not_of(" \t\r\n");
        const auto last = brand.find_last_not_of(" \t\r\n");
        brand = first == std::string::npos ? std::string() : brand.substr(first, last - first + 1);
    }
#elif defined(__aarch64__) || defined(_M_ARM64)
    vendor = "ARM64";
    brand = "ARM64 processor";
#endif
    const auto logical = available_logical_threads();
    const auto physical = available_physical_cores(logical);
    std::uint32_t numa_nodes = 0;
#ifdef _WIN32
    {
        std::set<USHORT> nodes;
        for (const auto& item : platform::available_processors()) {
            PROCESSOR_NUMBER processor{};
            processor.Group = item.first;
            processor.Number = static_cast<BYTE>(item.second);
            USHORT node = 0;
            if (GetNumaProcessorNodeEx(&processor, &node)) nodes.insert(node);
        }
        numa_nodes = static_cast<std::uint32_t>(nodes.size());
    }
#elif defined(__linux__)
    {
        std::set<int> nodes;
        for (const auto cpu_id : platform::linux_cpu::allowed_cpus()) {
            const auto root = std::filesystem::path("/sys/devices/system/cpu") /
                ("cpu" + std::to_string(cpu_id));
            std::error_code ec;
            for (const auto& entry : std::filesystem::directory_iterator(root, ec)) {
                const auto name = entry.path().filename().string();
                if (name.size() > 4 && name.starts_with("node") &&
                    std::all_of(name.begin() + 4, name.end(),
                        [](unsigned char ch) { return ch >= '0' && ch <= '9'; })) {
                    try { nodes.insert(std::stoi(name.substr(4))); } catch (...) {}
                    break;
                }
            }
        }
        numa_nodes = static_cast<std::uint32_t>(nodes.size());
    }
#endif
    const auto l3 = available_l3_cache_bytes().value_or(0);
    if (brand.empty()) brand = vendor;
    return {
        {"vendor", vendor}, {"brand", brand}, {"family", family},
        {"model", model}, {"stepping", stepping},
        {"logical_threads", logical}, {"physical_cores", physical},
        {"numa_nodes", numa_nodes}, {"l3_cache_bytes", l3},
        {"features", {
            {"aes", randomx::cpu.hasAes()},
            {"ssse3", randomx::cpu.hasSsse3()},
            {"avx2", randomx::cpu.hasAvx2()},
            {"vaes", randomx::cpu.hasVaes()},
            {"bmi2", randomx::cpu.hasBmi2()}
        }}
    };
}

std::uint32_t recommended_cpu_threads(std::uint32_t logical_threads,
                                     std::uint32_t physical_cores,
                                     std::uint32_t max_threads_hint) {
    logical_threads = std::max(1U, logical_threads);
    physical_cores = std::clamp(physical_cores, 1U, logical_threads);
    const auto hint = std::min(max_threads_hint, 100U);
    if (hint == 0) return physical_cores;

    // XMRig's max-threads-hint is a utilization hint for automatic profiles,
    // not a request to spawn that percentage of SMT/logical processors.
    // RandomX's automatic profile starts from physical cores, then applies
    // the shared-L3 limit in select_cpu_threads().
    return std::clamp(static_cast<std::uint32_t>(
        std::lround(physical_cores * (hint / 100.0))), 1U, physical_cores);
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
    // XMRig stores the RandomX worker profile under "rx" (CpuThreads
    // profile); retain the older UNIQUE RX "threads" alias for compatibility.
    const char* profile_key = cpu_config.contains("rx") ? "rx" : "threads";
    if (cpu_config.contains(profile_key) && !cpu_config[profile_key].is_null()) {
        const auto& configured = cpu_config[profile_key];
        std::uint64_t requested = 0;
        if (configured.is_array()) {
            // XMRig CpuThreads array form: each entry is a CPU affinity index
            // or [intensity, affinity]. RandomX uses one VM per entry; its
            // intensity is therefore retained as profile metadata only.
            for (const auto& thread : configured) {
                if (thread.is_number_integer()) {
                    const auto affinity = thread.get<std::int64_t>();
                    if (affinity < -1)
                        throw std::invalid_argument("cpu.threads affinity must be -1 or a non-negative CPU index");
                } else if (thread.is_array() && thread.size() >= 2 &&
                           thread[0].is_number_integer() &&
                           thread[1].is_number_integer()) {
                    const auto signed_intensity = thread[0].get<std::int64_t>();
                    const auto affinity = thread[1].get<std::int64_t>();
                    if (signed_intensity < 0 || signed_intensity > 5 || affinity < -1)
                        throw std::invalid_argument("cpu.threads entry must use [intensity:0..5, affinity:-1 or CPU index]");
                } else {
                    throw std::invalid_argument("cpu.threads array entries must be affinity integers or [intensity, affinity]");
                }
                ++requested;
            }
        } else if (configured.is_object()) {
            // XMRig compact object form: {"threads": N, "affinity": mask,
            // "intensity": N}. The worker count is the requested lane count.
            if (configured.contains("threads")) {
                const auto& count = configured["threads"];
                if (!count.is_number_unsigned() && !count.is_number_integer())
                    throw std::invalid_argument("cpu.threads.threads must be an integer");
                if (count.is_number_unsigned()) {
                    requested = count.get<std::uint64_t>();
                } else {
                    const auto signed_count = count.get<std::int64_t>();
                    if (signed_count < 0)
                        throw std::invalid_argument("cpu.threads.threads must be between 0 and 256");
                    requested = static_cast<std::uint64_t>(signed_count);
                }
            }
            if (configured.contains("intensity") &&
                (!configured["intensity"].is_number_integer() ||
                 configured["intensity"].get<std::int64_t>() < 0 ||
                 configured["intensity"].get<std::int64_t>() > 5)) {
                throw std::invalid_argument("cpu.threads.intensity must be between 0 and 5");
            }
            if (configured.contains("affinity") &&
                !configured["affinity"].is_number_integer() &&
                !configured["affinity"].is_string()) {
                throw std::invalid_argument("cpu.threads.affinity must be an integer or mask string");
            }
        } else if (configured.is_number_unsigned() || configured.is_number_integer()) {
            if (configured.is_number_unsigned()) {
                requested = configured.get<std::uint64_t>();
            } else {
                const auto signed_request = configured.get<std::int64_t>();
                if (signed_request < 0)
                    throw std::invalid_argument("cpu.threads must be between 0 and 256");
                requested = static_cast<std::uint64_t>(signed_request);
            }
        } else {
            throw std::invalid_argument("cpu.threads must be an integer, XMRig thread array, or thread profile object");
        }
        if (requested > 256)
            throw std::invalid_argument("cpu.threads must contain between 0 and 256 workers");
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
    if (!explicit_thread_count) {
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


std::vector<std::int64_t> select_cpu_affinities(const nlohmann::json& cpu_config) {
    std::vector<std::int64_t> result;
    // Match XMRig's RandomX profile key first; legacy UNIQUE RX
    // configurations using "threads" remain accepted.
    const char* profile_key = cpu_config.contains("rx") ? "rx" : "threads";
    if (!cpu_config.contains(profile_key)) return result;
    const auto& configured = cpu_config[profile_key];
    if (configured.is_array()) {
        result.reserve(configured.size());
        for (const auto& thread : configured) {
            std::int64_t affinity = -1;
            if (thread.is_number_integer()) {
                affinity = thread.get<std::int64_t>();
            } else if (thread.is_array() && thread.size() >= 2 &&
                       thread[1].is_number_integer()) {
                affinity = thread[1].get<std::int64_t>();
            } else {
                throw std::invalid_argument("cpu.threads array contains an invalid XMRig thread entry");
            }
            if (affinity < -1)
                throw std::invalid_argument("cpu.threads affinity must be -1 or a non-negative CPU index");
            result.push_back(affinity);
        }
        return result;
    }
    if (!configured.is_object()) return result;

    std::uint64_t count = 0;
    if (configured.contains("threads")) {
        const auto& value = configured["threads"];
        if (value.is_number_unsigned()) count = value.get<std::uint64_t>();
        else if (value.is_number_integer()) {
            const auto signed_count = value.get<std::int64_t>();
            if (signed_count < 0) throw std::invalid_argument("cpu.threads.threads cannot be negative");
            count = static_cast<std::uint64_t>(signed_count);
        } else {
            throw std::invalid_argument("cpu.threads.threads must be an integer");
        }
    }
    if (count > 256) throw std::invalid_argument("cpu.threads.threads cannot exceed 256");
    if (count == 0) return result;

    std::int64_t mask = -1;
    if (configured.contains("affinity")) {
        const auto& value = configured["affinity"];
        if (value.is_number_integer()) {
            mask = value.get<std::int64_t>();
        } else if (value.is_string()) {
            const auto text = value.get<std::string>();
            const auto hex_prefix = text.find("0x");
            const auto start = hex_prefix == std::string::npos ? 0 : hex_prefix + 2;
            const int base = hex_prefix == std::string::npos ? 10 : 16;
            if (start >= text.size()) throw std::invalid_argument("cpu.threads.affinity mask is empty");
            std::size_t used = 0;
            try {
                mask = std::stoll(text.substr(start), &used, base);
            } catch (...) {
                throw std::invalid_argument("cpu.threads.affinity mask is invalid");
            }
            if (used != text.size() - start)
                throw std::invalid_argument("cpu.threads.affinity mask is invalid");
        } else {
            throw std::invalid_argument("cpu.threads.affinity must be an integer or mask string");
        }
    }
    if (mask < -1) throw std::invalid_argument("cpu.threads.affinity must be -1 or a non-negative mask");
    result.reserve(static_cast<std::size_t>(count));
    if (mask == -1) {
        result.assign(static_cast<std::size_t>(count), -1);
        return result;
    }
    const auto bits = static_cast<std::uint64_t>(mask);
    for (std::uint64_t bit = 0; bit < 64 && result.size() < count; ++bit) {
        if ((bits & (std::uint64_t{1} << bit)) != 0)
            result.push_back(static_cast<std::int64_t>(bit));
    }
    while (result.size() < count) result.push_back(-1);
    return result;
}

} // namespace unique_rx
