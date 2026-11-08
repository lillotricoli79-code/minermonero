#include "msr.hpp"
#include "presets.hpp"
#include "msr/transaction.hpp"
#include "logging/log.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <iterator>
#include <optional>
#include <stdexcept>
#include <string>
#include <sstream>
#include <system_error>
#include <utility>
#include <vector>

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
#include <winreg.h>
#include <openssl/evp.h>
#include "platform/affinity.hpp"
#ifdef _MSC_VER
#include <intrin.h>
#else
#include <cpuid.h>
#endif
#else
#ifdef __linux__
#include "platform/linux_cpu_set.hpp"
#endif
#include <cerrno>
#include <fcntl.h>
#include <thread>
#include <sched.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#if defined(__i386__) || defined(__x86_64__)
#include <cpuid.h>
#endif
#endif

namespace unique_rx {
namespace {

struct CpuIdentity {
    std::string vendor;
    std::uint32_t family = 0;
    std::uint32_t model = 0;
};

std::optional<CpuIdentity> detect_cpu_identity() {
    char vendor[13]{};
    std::uint32_t eax = 0;
#if defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86))
    int vendor_registers[4]{};
    __cpuid(vendor_registers, 0);
    std::memcpy(vendor, &vendor_registers[1], 4);
    std::memcpy(vendor + 4, &vendor_registers[3], 4);
    std::memcpy(vendor + 8, &vendor_registers[2], 4);
    int feature_registers[4]{};
    __cpuidex(feature_registers, 1, 0);
    eax = static_cast<std::uint32_t>(feature_registers[0]);
#elif defined(__i386__) || defined(__x86_64__)
    unsigned int ebx = 0, ecx = 0, edx = 0;
    if (!__get_cpuid(0, &eax, &ebx, &ecx, &edx)) return std::nullopt;
    std::memcpy(vendor, &ebx, 4);
    std::memcpy(vendor + 4, &edx, 4);
    std::memcpy(vendor + 8, &ecx, 4);
    if (!__get_cpuid(1, &eax, &ebx, &ecx, &edx)) return std::nullopt;
#else
    return std::nullopt;
#endif
    const std::uint32_t base_family = (eax >> 8) & 0x0f;
    const std::uint32_t base_model = (eax >> 4) & 0x0f;
    const std::uint32_t extended_family = (eax >> 20) & 0xff;
    const std::uint32_t extended_model = (eax >> 16) & 0x0f;
    const std::uint32_t family =
        base_family == 0x0f ? base_family + extended_family : base_family;
    const std::uint32_t model =
        (base_family == 0x06 || base_family == 0x0f)
            ? (extended_model << 4) | base_model
            : base_model;
    return CpuIdentity{std::string(vendor), family, model};
}

#ifdef _WIN32

using PawnioOpen = HRESULT(WINAPI*)(PHANDLE);
using PawnioLoad = HRESULT(WINAPI*)(HANDLE, const UCHAR*, SIZE_T);
using PawnioExecute = HRESULT(WINAPI*)(
    HANDLE, PCSTR, const ULONG64*, SIZE_T, PULONG64, SIZE_T, PSIZE_T);
using PawnioClose = HRESULT(WINAPI*)(HANDLE);

struct ThreadAffinity {
    GROUP_AFFINITY previous{};
    bool active = false;

    ThreadAffinity(WORD group, BYTE processor) {
        GROUP_AFFINITY desired{};
        desired.Group = group;
        desired.Mask = static_cast<KAFFINITY>(1) << processor;
        if (!SetThreadGroupAffinity(GetCurrentThread(), &desired, &previous)) {
            throw std::system_error(
                static_cast<int>(GetLastError()), std::system_category(),
                "SetThreadGroupAffinity failed");
        }
        active = true;
    }

    ~ThreadAffinity() {
        if (active) SetThreadGroupAffinity(GetCurrentThread(), &previous, nullptr);
    }

    ThreadAffinity(const ThreadAffinity&) = delete;
    ThreadAffinity& operator=(const ThreadAffinity&) = delete;
};

std::filesystem::path executable_directory() {
    std::wstring buffer(32768, L'\0');
    const DWORD length = GetModuleFileNameW(
        nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (length == 0 || length >= buffer.size()) {
        throw std::runtime_error("cannot resolve miner executable directory");
    }
    buffer.resize(length);
    return std::filesystem::path(buffer).parent_path();
}

std::filesystem::path pawnio_library_path() {
    std::vector<std::filesystem::path> candidates;
    wchar_t install_path[32768]{};
    DWORD bytes = sizeof(install_path);
    if (RegGetValueW(
            HKEY_LOCAL_MACHINE,
            L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\PawnIO",
            L"InstallLocation", RRF_RT_REG_SZ, nullptr, install_path, &bytes) == ERROR_SUCCESS) {
        candidates.emplace_back(std::filesystem::path(install_path) / L"PawnIOLib.dll");
    }
    if (const wchar_t* program_files = _wgetenv(L"ProgramFiles")) {
        candidates.emplace_back(
            std::filesystem::path(program_files) / L"PawnIO" / L"PawnIOLib.dll");
    }
    candidates.emplace_back(executable_directory() / L"PawnIOLib.dll");
    for (const auto& candidate : candidates) {
        if (std::filesystem::is_regular_file(candidate)) return candidate;
    }
    return {};
}

std::filesystem::path default_module_path() {
    const auto directory = executable_directory();
    const std::vector<std::filesystem::path> candidates{
        directory / L"msr" / L"IntelMSR.bin",
        directory / L"IntelMSR.bin",
        directory.parent_path() / L"msr" / L"IntelMSR.bin",
        directory.parent_path().parent_path() / L"msr" / L"IntelMSR.bin",
        std::filesystem::current_path() / L"msr" / L"IntelMSR.bin",
    };
    for (const auto& candidate : candidates) {
        if (std::filesystem::is_regular_file(candidate)) return candidate;
    }
    return candidates.front();
}

#else

std::vector<unsigned int> online_cpu_ids() {
    std::vector<unsigned int> cpus;
    std::ifstream online_file("/sys/devices/system/cpu/online");
    std::string online;
    if (online_file && std::getline(online_file, online)) {
        try {
            std::stringstream ranges(online);
            std::string item;
            while (std::getline(ranges, item, ',')) {
                if (item.empty()) throw std::runtime_error("empty CPU range");
                const auto dash = item.find('-');
                if (dash == std::string::npos) {
                    cpus.push_back(static_cast<unsigned int>(std::stoul(item)));
                    continue;
                }
                const auto first = static_cast<unsigned int>(std::stoul(item.substr(0, dash)));
                const auto last = static_cast<unsigned int>(std::stoul(item.substr(dash + 1)));
                if (last < first || last >= (1U << 20)) {
                    throw std::runtime_error("invalid Linux online CPU range");
                }
                for (unsigned int cpu = first;; ++cpu) {
                    cpus.push_back(cpu);
                    if (cpu == last) break;
                }
            }
        } catch (const std::exception&) {
            cpus.clear();
        }
    }

    if (cpus.empty()) {
        const auto count = std::max(1U, std::thread::hardware_concurrency());
        cpus.reserve(count);
        for (unsigned int cpu = 0; cpu < count; ++cpu) cpus.push_back(cpu);
    }

    // Always intersect with the process cpuset, including when the kernel
    // supplied the online list. Use a dynamically sized mask so MSR tuning
    // cannot silently skip CPUs above the legacy CPU_SETSIZE limit.
#ifdef __linux__
    const auto allowed = platform::linux_cpu::allowed_cpus();
    if (allowed.empty()) {
        throw std::runtime_error(
            "cannot determine process CPU affinity; refusing unsafe MSR writes");
    }
    cpus.erase(std::remove_if(cpus.begin(), cpus.end(), [&](unsigned int cpu) {
        return std::find(allowed.begin(), allowed.end(), cpu) == allowed.end();
    }), cpus.end());
    if (cpus.empty()) {
        throw std::runtime_error(
            "no online CPUs are available to this process for MSR tuning");
    }
#endif
    return cpus;
}

#endif

} // namespace

struct MsrManager::Impl {
    explicit Impl(std::filesystem::path path) : module_path(std::move(path)) {}

    std::filesystem::path module_path;
    bool active = false;

#ifdef _WIN32
    HMODULE library = nullptr;
    HANDLE handle = nullptr;
    PawnioOpen open = nullptr;
    PawnioLoad load = nullptr;
    PawnioExecute execute = nullptr;
    PawnioClose close = nullptr;

    std::vector<msr_detail::RegisterSnapshot> snapshots;
    std::vector<std::pair<WORD, DWORD>> msr_processors;

    void initialize_pawnio() {
        if (handle != nullptr) return;
        const auto dll_path = pawnio_library_path();
        if (dll_path.empty()) {
            throw std::runtime_error(
                "PawnIO is not installed or PawnIOLib.dll was not found in a trusted location. "
                "Install the official signed PawnIO driver first.");
        }
        library = LoadLibraryExW(
            dll_path.c_str(), nullptr,
            LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (library == nullptr) {
            throw std::runtime_error(
                "PawnIOLib.dll could not be loaded from its resolved absolute path");
        }
        open = reinterpret_cast<PawnioOpen>(GetProcAddress(library, "pawnio_open"));
        load = reinterpret_cast<PawnioLoad>(GetProcAddress(library, "pawnio_load"));
        execute = reinterpret_cast<PawnioExecute>(GetProcAddress(library, "pawnio_execute"));
        close = reinterpret_cast<PawnioClose>(GetProcAddress(library, "pawnio_close"));
        if (!open || !load || !execute || !close) {
            throw std::runtime_error("PawnIOLib.dll is missing required exports");
        }

        if (module_path.empty()) module_path = default_module_path();
        std::ifstream module_file(module_path, std::ios::binary);
        if (!module_file) {
            throw std::runtime_error(
                "IntelMSR.bin not found. Place the official PawnIO Intel MSR module in the miner's msr directory.");
        }
        const std::vector<UCHAR> module(
            (std::istreambuf_iterator<char>(module_file)),
            std::istreambuf_iterator<char>());
        if (module.empty()) {
            throw std::runtime_error("IntelMSR.bin is empty or unreadable");
        }

        // Pin the exact official PawnIO.Modules 0.2.11 Intel module. PawnIO
        // validates the module signature; this hash additionally rejects a
        // replaced, truncated or unrelated module before it is submitted.
        static constexpr std::array<unsigned char, 32> expected_sha256{
            0xd6, 0xed, 0x85, 0xd6, 0x5a, 0xb1, 0x7a, 0x22,
            0xf8, 0x13, 0xef, 0x98, 0x20, 0x7d, 0x6d, 0x53,
            0x71, 0x55, 0xee, 0x2d, 0xed, 0x59, 0x76, 0xa2,
            0x1c, 0xb4, 0x84, 0x13, 0xc9, 0xb9, 0x2e, 0x5f,
        };
        std::array<unsigned char, EVP_MAX_MD_SIZE> actual_sha256{};
        unsigned int actual_sha256_size = 0;
        if (EVP_Digest(module.data(), module.size(), actual_sha256.data(),
                       &actual_sha256_size, EVP_sha256(), nullptr) != 1 ||
            actual_sha256_size != expected_sha256.size() ||
            !std::equal(expected_sha256.begin(), expected_sha256.end(),
                        actual_sha256.begin())) {
            throw std::runtime_error(
                "IntelMSR.bin SHA-256 mismatch; expected the official PawnIO.Modules 0.2.11 Intel module");
        }

        const HRESULT open_result = open(&handle);
        if (FAILED(open_result) || handle == nullptr) {
            if (handle != nullptr) {
                close(handle);
                handle = nullptr;
            }
            throw std::runtime_error(
                "PawnIO driver could not be opened; check installation and administrator rights");
        }
        if (FAILED(load(handle, module.data(), module.size()))) {
            close(handle);
            handle = nullptr;
            throw std::runtime_error(
                "PawnIO rejected IntelMSR.bin; use the official module matching the pinned SHA-256");
        }
    }

    std::uint64_t read_current_msr(std::uint32_t register_id) {
        const ULONG64 input[1]{register_id};
        ULONG64 output[1]{};
        SIZE_T returned = 0;
        if (FAILED(execute(handle, "ioctl_read_msr", input, 1, output, 1, &returned)) ||
            returned != 1) {
            throw std::runtime_error("PawnIO failed to read the requested MSR");
        }
        return output[0];
    }

    void write_current_msr(std::uint32_t register_id, std::uint64_t value) {
        const ULONG64 input[2]{register_id, value};
        SIZE_T returned = 0;
        if (FAILED(execute(handle, "ioctl_write_msr", input, 2, nullptr, 0, &returned))) {
            throw std::runtime_error("PawnIO failed to write the requested MSR");
        }
    }

    std::uint64_t read_for_cpu(std::size_t cpu_index, std::uint64_t register_id) {
        if (cpu_index >= msr_processors.size()) {
            throw std::runtime_error("invalid CPU index in MSR transaction");
        }
        const auto [group, processor] = msr_processors[cpu_index];
        if (processor >= sizeof(KAFFINITY) * 8) {
            throw std::runtime_error("processor group exceeds supported affinity mask");
        }
        ThreadAffinity affinity(group, static_cast<BYTE>(processor));
        return read_current_msr(static_cast<std::uint32_t>(register_id));
    }

    void write_for_cpu(std::size_t cpu_index, std::uint64_t register_id,
                       std::uint64_t value) {
        if (cpu_index >= msr_processors.size()) {
            throw std::runtime_error("invalid CPU index in MSR transaction");
        }
        const auto [group, processor] = msr_processors[cpu_index];
        if (processor >= sizeof(KAFFINITY) * 8) {
            throw std::runtime_error("processor group exceeds supported affinity mask");
        }
        ThreadAffinity affinity(group, static_cast<BYTE>(processor));
        write_current_msr(static_cast<std::uint32_t>(register_id), value);
    }

    void apply(const MsrPreset& preset) {
        if (active) return;
        if (preset.writes.empty()) throw std::runtime_error("MSR preset contains no writes");
        snapshots.clear();
        initialize_pawnio();
        msr_processors = platform::available_processors();
        if (msr_processors.empty()) {
            throw std::runtime_error("Windows reports no processors available to this process");
        }
        std::vector<std::size_t> cpu_indices;
        cpu_indices.reserve(msr_processors.size());
        for (std::size_t index = 0; index < msr_processors.size(); ++index) {
            cpu_indices.push_back(index);
        }
        const auto read = [this](std::size_t cpu, std::uint64_t reg) {
            return read_for_cpu(cpu, reg);
        };
        const auto write = [this](std::size_t cpu, std::uint64_t reg, std::uint64_t value) {
            write_for_cpu(cpu, reg, value);
        };
        const auto log = [](const char* message) {
            logging::warning("msr", std::string("restore warning: ") + message);
        };
        try {
            msr_detail::apply_registers(cpu_indices, preset.writes,
                                        snapshots, read, write, log);
            active = true;
        } catch (...) {
            restore();
            throw;
        }
    }

    void restore() noexcept {
        const auto read = [this](std::size_t cpu, std::uint64_t reg) {
            return read_for_cpu(cpu, reg);
        };
        const auto write = [this](std::size_t cpu, std::uint64_t reg, std::uint64_t value) {
            write_for_cpu(cpu, reg, value);
        };
        const auto log = [](const char* message) {
            logging::warning("msr", std::string("restore warning: ") + message);
        };
        active = msr_detail::restore_registers(snapshots, read, write, log);
    }

    void close_pawnio() noexcept {
        if (handle != nullptr && close != nullptr) {
            close(handle);
            handle = nullptr;
        }
        if (library != nullptr) {
            FreeLibrary(library);
            library = nullptr;
        }
    }
#else
    std::vector<int> cpu_fds;
    std::vector<msr_detail::RegisterSnapshot> snapshots;
    int lock_fd = -1;

    void acquire_process_lock() {
        if (lock_fd >= 0) return;
        lock_fd = ::open("/run/lock/rxminer-msr.lock",
                         O_CREAT | O_RDWR | O_CLOEXEC | O_NOFOLLOW, 0600);
        if (lock_fd < 0) {
            throw std::system_error(errno, std::generic_category(),
                                    "cannot open shared MSR lock");
        }
        if (::flock(lock_fd, LOCK_EX | LOCK_NB) != 0) {
            const int error = errno;
            ::close(lock_fd);
            lock_fd = -1;
            throw std::system_error(error, std::generic_category(),
                                    "another miner owns the MSR settings lock");
        }
    }

    void release_process_lock() noexcept {
        if (lock_fd >= 0) {
            ::flock(lock_fd, LOCK_UN);
            ::close(lock_fd);
            lock_fd = -1;
        }
    }

    void close_cpu_fds() noexcept {
        for (int& fd : cpu_fds) {
            if (fd >= 0) {
                ::close(fd);
                fd = -1;
            }
        }
    }

    std::uint64_t read_for_cpu(std::size_t cpu_index, std::uint64_t register_id) {
        if (cpu_index >= cpu_fds.size() || cpu_fds[cpu_index] < 0) {
            throw std::runtime_error("invalid CPU descriptor in MSR transaction");
        }
        std::uint64_t value = 0;
        const auto offset = static_cast<off_t>(register_id);
        const auto count = ::pread(cpu_fds[cpu_index], &value, sizeof(value), offset);
        if (count != static_cast<ssize_t>(sizeof(value))) {
            const int error = count < 0 ? errno : EIO;
            throw std::system_error(error, std::generic_category(),
                                    "cannot read MSR " + std::to_string(register_id));
        }
        return value;
    }

    void write_for_cpu(std::size_t cpu_index, std::uint64_t register_id,
                       std::uint64_t value) {
        if (cpu_index >= cpu_fds.size() || cpu_fds[cpu_index] < 0) {
            throw std::runtime_error("invalid CPU descriptor in MSR transaction");
        }
        const auto offset = static_cast<off_t>(register_id);
        const auto count = ::pwrite(cpu_fds[cpu_index], &value, sizeof(value), offset);
        if (count != static_cast<ssize_t>(sizeof(value))) {
            const int error = count < 0 ? errno : EIO;
            throw std::system_error(error, std::generic_category(),
                                    "cannot write MSR " + std::to_string(register_id));
        }
    }

    void apply(const MsrPreset& preset) {
        if (active) return;
        if (preset.writes.empty()) throw std::runtime_error("MSR preset contains no writes");
        cpu_fds.clear();
        snapshots.clear();

        try {
            acquire_process_lock();
            const auto cpus = online_cpu_ids();
            if (cpus.empty()) {
                throw std::runtime_error("no online CPU is available to this process for MSR tuning");
            }
            cpu_fds.reserve(cpus.size());
            for (const unsigned int cpu : cpus) {
                const auto path = std::filesystem::path("/dev/cpu") /
                    std::to_string(cpu) / "msr";
                const int fd = ::open(path.c_str(), O_RDWR | O_CLOEXEC);
                if (fd < 0) {
                    throw std::system_error(errno, std::generic_category(),
                                            "cannot open " + path.string());
                }
                cpu_fds.push_back(fd);
            }
            std::vector<std::size_t> cpu_indices;
            cpu_indices.reserve(cpu_fds.size());
            for (std::size_t index = 0; index < cpu_fds.size(); ++index) {
                cpu_indices.push_back(index);
            }
            const auto read = [this](std::size_t cpu, std::uint64_t reg) {
                return read_for_cpu(cpu, reg);
            };
            const auto write = [this](std::size_t cpu, std::uint64_t reg, std::uint64_t value) {
                write_for_cpu(cpu, reg, value);
            };
            const auto log = [](const char* message) {
                std::cerr << "MSR restore warning: " << message << '\n';
            };
            msr_detail::apply_registers(cpu_indices, preset.writes,
                                        snapshots, read, write, log);
            active = true;
        } catch (...) {
            restore();
            throw;
        }
    }

    void restore() noexcept {
        const auto read = [this](std::size_t cpu, std::uint64_t reg) {
            return read_for_cpu(cpu, reg);
        };
        const auto write = [this](std::size_t cpu, std::uint64_t reg, std::uint64_t value) {
            write_for_cpu(cpu, reg, value);
        };
        const auto log = [](const char* message) {
            logging::warning("msr", std::string("restore warning: ") + message);
        };
        active = msr_detail::restore_registers(snapshots, read, write, log);
        if (!active) {
            close_cpu_fds();
            release_process_lock();
        }
    }
#endif

    ~Impl() {
        const bool had_snapshots = !snapshots.empty();
        restore();
        if (active) restore();
        try {
            if (had_snapshots && !active) {
                logging::info("msr", "original CPU register values restored");
            } else if (active) {
                logging::error("msr", "register restore remains pending after retry");
            }
        } catch (...) {
            // Logging must never interfere with register restoration or shutdown.
        }
#ifdef _WIN32
        close_pawnio();
#else
        // A final close is required even if the kernel rejected a restore write.
        close_cpu_fds();
        release_process_lock();
#endif
    }
};

MsrManager::MsrManager(std::filesystem::path module_path)
    : impl_(std::make_unique<Impl>(std::move(module_path))) {}

MsrManager::~MsrManager() = default;
MsrManager::MsrManager(MsrManager&&) noexcept = default;
MsrManager& MsrManager::operator=(MsrManager&&) noexcept = default;

void MsrManager::apply_randomx_preset() {
    const auto cpu = detect_cpu_identity();
    if (!cpu) {
        throw std::runtime_error("cannot identify CPU vendor/family/model for MSR tuning");
    }
    const auto preset = msr_preset_for_cpu(cpu->vendor, cpu->family, cpu->model);
    if (!preset) {
        throw std::runtime_error(
            "no verified RandomX MSR preset exists for this CPU family/model");
    }
#ifdef _WIN32
    if (cpu->vendor != "GenuineIntel") {
        throw std::runtime_error(
            "Windows MSR backend currently supports Intel PawnIO only");
    }
#endif
    impl_->apply(*preset);
}

void MsrManager::restore() noexcept {
    if (impl_) impl_->restore();
}

bool MsrManager::active() const noexcept {
    return impl_ && impl_->active;
}

} // namespace unique_rx
