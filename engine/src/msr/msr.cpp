#include "msr.hpp"
#include "presets.hpp"

#include <algorithm>
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
#ifdef _MSC_VER
#include <intrin.h>
#else
#include <cpuid.h>
#endif
#else
#include <cerrno>
#include <fcntl.h>
#include <thread>
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
    std::ifstream online_file("/sys/devices/system/cpu/online");
    std::string online;
    if (online_file && std::getline(online_file, online)) {
        try {
            std::stringstream ranges(online);
            std::string item;
            std::vector<unsigned int> cpus;
            while (std::getline(ranges, item, ',')) {
                const auto dash = item.find('-');
                if (dash == std::string::npos) {
                    cpus.push_back(static_cast<unsigned int>(std::stoul(item)));
                    continue;
                }
                const auto first = static_cast<unsigned int>(std::stoul(item.substr(0, dash)));
                const auto last = static_cast<unsigned int>(std::stoul(item.substr(dash + 1)));
                if (last < first || last - first > 4096) {
                    throw std::runtime_error("invalid Linux online CPU range");
                }
                for (unsigned int cpu = first;; ++cpu) {
                    cpus.push_back(cpu);
                    if (cpu == last) break;
                }
            }
            if (!cpus.empty()) return cpus;
        } catch (const std::exception&) {
            // Fall back to the contiguous hardware_concurrency range below.
        }
    }

    const auto count = std::max(1U, std::thread::hardware_concurrency());
    std::vector<unsigned int> cpus;
    cpus.reserve(count);
    for (unsigned int cpu = 0; cpu < count; ++cpu) cpus.push_back(cpu);
    return cpus;
}

struct LinuxRegisterOriginal {
    std::uint64_t register_id = 0;
    std::uint64_t value = 0;
    bool modified = false;
};

struct LinuxOriginal {
    int fd = -1;
    std::vector<LinuxRegisterOriginal> registers;
};

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

    struct WindowsOriginal {
        WORD group = 0;
        BYTE processor = 0;
        std::uint32_t register_id = 0;
        std::uint64_t value = 0;
        bool modified = false;
    };
    std::vector<WindowsOriginal> originals;

    void initialize_pawnio() {
        if (handle != nullptr) return;
        const auto dll_path = pawnio_library_path();
        library = dll_path.empty()
            ? LoadLibraryW(L"PawnIOLib.dll")
            : LoadLibraryW(dll_path.c_str());
        if (library == nullptr) {
            throw std::runtime_error(
                "PawnIO is not installed or PawnIOLib.dll cannot be loaded. "
                "Install the official signed PawnIO driver first.");
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

    void apply(const MsrPreset& preset) {
        if (active) return;
        if (preset.writes.empty()) throw std::runtime_error("MSR preset contains no writes");
        originals.clear();
        initialize_pawnio();
        const WORD groups = GetActiveProcessorGroupCount();
        if (groups == 0) throw std::runtime_error("Windows reports no active processor groups");

        for (WORD group = 0; group < groups; ++group) {
            const DWORD processors = GetActiveProcessorCount(group);
            for (DWORD processor = 0; processor < processors; ++processor) {
                if (processor >= sizeof(KAFFINITY) * 8) {
                    throw std::runtime_error("processor group exceeds supported affinity mask");
                }
                ThreadAffinity affinity(group, static_cast<BYTE>(processor));
                for (const auto& write : preset.writes) {
                    const auto register_id = static_cast<std::uint32_t>(write.register_id);
                    originals.push_back({group, static_cast<BYTE>(processor),
                                         register_id, read_current_msr(register_id), false});
                }
            }
        }

        try {
            for (std::size_t index = 0; index < originals.size(); ++index) {
                auto& original = originals[index];
                const auto& write = preset.writes[index % preset.writes.size()];
                ThreadAffinity affinity(original.group, original.processor);
                const auto value = (write.value & write.mask) |
                                   (original.value & ~write.mask);
                original.modified = true;
                write_current_msr(original.register_id, value);
                if (read_current_msr(original.register_id) != value) {
                    throw std::runtime_error("MSR read-back verification failed");
                }
            }
            active = true;
        } catch (...) {
            restore();
            throw;
        }
    }

    void restore() noexcept {
        bool restoration_pending = false;
        for (auto& original : originals) {
            if (!original.modified) continue;
            try {
                ThreadAffinity affinity(original.group, original.processor);
                write_current_msr(original.register_id, original.value);
                if (read_current_msr(original.register_id) != original.value) {
                    throw std::runtime_error("MSR restore read-back verification failed");
                }
                original.modified = false;
            } catch (const std::exception& error) {
                restoration_pending = true;
                std::cerr << "MSR restore warning: " << error.what() << '\n';
            } catch (...) {
                restoration_pending = true;
                std::cerr << "MSR restore warning: unknown error\n";
            }
            restoration_pending = restoration_pending || original.modified;
        }
        active = restoration_pending;
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
    std::vector<LinuxOriginal> originals;

    void apply(const MsrPreset& preset) {
        if (active) return;
        if (preset.writes.empty()) throw std::runtime_error("MSR preset contains no writes");
        originals.clear();
        const auto cpus = online_cpu_ids();
        originals.reserve(cpus.size());

        try {
            for (const unsigned int cpu : cpus) {
                const auto path = std::filesystem::path("/dev/cpu") /
                    std::to_string(cpu) / "msr";
                const int fd = ::open(path.c_str(), O_RDWR | O_CLOEXEC);
                if (fd < 0) {
                    throw std::system_error(errno, std::generic_category(),
                                            "cannot open " + path.string());
                }
                originals.push_back({fd, {}});
                auto& cpu_originals = originals.back();
                cpu_originals.registers.reserve(preset.writes.size());
                for (const auto& write : preset.writes) {
                    std::uint64_t original = 0;
                    const auto offset = static_cast<off_t>(write.register_id);
                    const auto bytes_read = ::pread(fd, &original, sizeof(original), offset);
                    if (bytes_read != static_cast<ssize_t>(sizeof(original))) {
                        const int error = bytes_read < 0 ? errno : EIO;
                        throw std::system_error(error, std::generic_category(),
                            "cannot read MSR " + std::to_string(write.register_id) +
                            " from " + path.string());
                    }
                    cpu_originals.registers.push_back(
                        {write.register_id, original, false});
                }
            }

            for (auto& cpu_originals : originals) {
                for (std::size_t index = 0; index < preset.writes.size(); ++index) {
                    const auto& write = preset.writes[index];
                    auto& snapshot = cpu_originals.registers[index];
                    const auto value = (write.value & write.mask) |
                                       (snapshot.value & ~write.mask);
                    snapshot.modified = true;
                    const auto offset = static_cast<off_t>(write.register_id);
                    const auto written = ::pwrite(
                        cpu_originals.fd, &value, sizeof(value), offset);
                    if (written != static_cast<ssize_t>(sizeof(value))) {
                        const int error = written < 0 ? errno : EIO;
                        throw std::system_error(error, std::generic_category(),
                            "cannot write MSR " + std::to_string(write.register_id));
                    }
                    std::uint64_t current_value = 0;
                    const auto bytes_read = ::pread(
                        cpu_originals.fd, &current_value, sizeof(current_value), offset);
                    if (bytes_read != static_cast<ssize_t>(sizeof(current_value)) ||
                        current_value != value) {
                        throw std::runtime_error("MSR read-back verification failed");
                    }
                }
            }
            active = true;
        } catch (...) {
            restore();
            throw;
        }
    }

    void restore() noexcept {
        bool restoration_pending = false;
        for (auto& cpu_originals : originals) {
            bool cpu_pending = false;
            for (auto it = cpu_originals.registers.rbegin();
                 it != cpu_originals.registers.rend(); ++it) {
                if (!it->modified) continue;
                const auto offset = static_cast<off_t>(it->register_id);
                const auto written = ::pwrite(
                    cpu_originals.fd, &it->value, sizeof(it->value), offset);
                if (written != static_cast<ssize_t>(sizeof(it->value))) {
                    cpu_pending = true;
                    std::cerr << "MSR restore warning: failed to restore register "
                              << it->register_id << '\n';
                    continue;
                }
                std::uint64_t restored_value = 0;
                const auto bytes_read = ::pread(
                    cpu_originals.fd, &restored_value, sizeof(restored_value), offset);
                if (bytes_read == static_cast<ssize_t>(sizeof(restored_value)) &&
                    restored_value == it->value) {
                    it->modified = false;
                } else {
                    cpu_pending = true;
                    std::cerr << "MSR restore warning: read-back verification failed for register "
                              << it->register_id << '\n';
                }
            }
            for (const auto& reg : cpu_originals.registers) {
                cpu_pending = cpu_pending || reg.modified;
            }
            restoration_pending = restoration_pending || cpu_pending;
            if (cpu_originals.fd >= 0 && !cpu_pending) {
                ::close(cpu_originals.fd);
                cpu_originals.fd = -1;
            }
        }
        active = restoration_pending;
    }
#endif

    ~Impl() {
        restore();
#ifdef _WIN32
        close_pawnio();
#else
        // A final close is required even if the kernel rejected a restore write.
        for (auto& original : originals) {
            if (original.fd >= 0) {
                ::close(original.fd);
                original.fd = -1;
            }
        }
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
