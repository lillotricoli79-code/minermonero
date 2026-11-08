#include "msr.hpp"

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <iterator>
#include <cstring>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <intrin.h>
#include <winreg.h>
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

inline constexpr std::uint32_t kIntelMiscFeatureControl = 0x1A4;
inline constexpr std::uint64_t kRandomXIntelMsrValue = 0xF;

bool is_intel_cpu() {
    char vendor[13]{};
#ifdef _WIN32
    int registers[4]{};
    __cpuid(registers, 0);
    std::memcpy(vendor, &registers[1], sizeof(int));
    std::memcpy(vendor + 4, &registers[3], sizeof(int));
    std::memcpy(vendor + 8, &registers[2], sizeof(int));
#elif defined(__i386__) || defined(__x86_64__)
    unsigned int eax = 0, ebx = 0, ecx = 0, edx = 0;
    if (!__get_cpuid(0, &eax, &ebx, &ecx, &edx)) return false;
    std::memcpy(vendor, &ebx, sizeof(ebx));
    std::memcpy(vendor + 4, &edx, sizeof(edx));
    std::memcpy(vendor + 8, &ecx, sizeof(ecx));
#else
    return false;
#endif
    return std::string(vendor) == "GenuineIntel";
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
        std::filesystem::current_path() / L"msr" / L"IntelMSR.bin",
    };
    for (const auto& candidate : candidates) {
        if (std::filesystem::is_regular_file(candidate)) return candidate;
    }
    return candidates.front();
}

#else

struct LinuxOriginal {
    int fd = -1;
    std::uint64_t value = 0;
    bool modified = false;
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
        std::uint64_t value = 0;
        bool modified = false;
    };
    std::vector<WindowsOriginal> originals;

    void initialize_pawnio() {
        if (handle != nullptr) return;
        const auto dll_path = pawnio_library_path();
        if (dll_path.empty()) {
            throw std::runtime_error(
                "PawnIO is not installed. Install the official signed PawnIO driver first.");
        }
        library = LoadLibraryW(dll_path.c_str());
        if (library == nullptr) {
            throw std::system_error(
                static_cast<int>(GetLastError()), std::system_category(),
                "cannot load PawnIOLib.dll");
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
                "IntelMSR.bin not found. Run scripts/windows/setup-msr.ps1 first.");
        }
        const std::vector<UCHAR> module(
            (std::istreambuf_iterator<char>(module_file)),
            std::istreambuf_iterator<char>());

        if (FAILED(open(&handle)) || handle == nullptr) {
            throw std::runtime_error(
                "PawnIO driver could not be opened; check installation and administrator rights");
        }
        if (FAILED(load(handle, module.data(), module.size()))) {
            close(handle);
            handle = nullptr;
            throw std::runtime_error(
                "PawnIO rejected IntelMSR.bin; use the signed official module release");
        }
    }

    std::uint64_t read_current_msr() {
        const ULONG64 input[1]{kIntelMiscFeatureControl};
        ULONG64 output[1]{};
        SIZE_T returned = 0;
        if (FAILED(execute(handle, "ioctl_read_msr", input, 1, output, 1, &returned)) ||
            returned != 1) {
            throw std::runtime_error("PawnIO failed to read Intel MSR 0x1A4");
        }
        return output[0];
    }

    void write_current_msr(std::uint64_t value) {
        const ULONG64 input[2]{kIntelMiscFeatureControl, value};
        SIZE_T returned = 0;
        if (FAILED(execute(handle, "ioctl_write_msr", input, 2, nullptr, 0, &returned))) {
            throw std::runtime_error("PawnIO failed to write Intel MSR 0x1A4");
        }
    }

    void apply() {
        if (active) return;
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
                originals.push_back({group, static_cast<BYTE>(processor),
                                     read_current_msr(), false});
            }
        }

        try {
            for (auto& original : originals) {
                ThreadAffinity affinity(original.group, original.processor);
                write_current_msr(kRandomXIntelMsrValue);
                original.modified = true;
                if (read_current_msr() != kRandomXIntelMsrValue) {
                    throw std::runtime_error("Intel MSR 0x1A4 read-back verification failed");
                }
            }
            active = true;
        } catch (...) {
            restore();
            throw;
        }
    }

    void restore() noexcept {
        for (auto& original : originals) {
            if (!original.modified) continue;
            try {
                ThreadAffinity affinity(original.group, original.processor);
                write_current_msr(original.value);
                original.modified = false;
            } catch (const std::exception& error) {
                std::cerr << "MSR restore warning: " << error.what() << '\n';
            } catch (...) {
                std::cerr << "MSR restore warning: unknown error\n";
            }
        }
        active = false;
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

    void apply() {
        if (active) return;
        originals.clear();
        const auto count = std::max(1U, std::thread::hardware_concurrency());
        originals.reserve(count);
        for (unsigned int cpu = 0; cpu < count; ++cpu) {
            const auto path = std::filesystem::path("/dev/cpu") /
                std::to_string(cpu) / "msr";
            const int fd = ::open(path.c_str(), O_RDWR | O_CLOEXEC);
            if (fd < 0) {
                throw std::system_error(errno, std::generic_category(),
                                        "cannot open " + path.string());
            }
            std::uint64_t original = 0;
            if (::pread(fd, &original, sizeof(original), kIntelMiscFeatureControl) !=
                static_cast<ssize_t>(sizeof(original))) {
                const int error = errno;
                ::close(fd);
                throw std::system_error(error, std::generic_category(),
                                        "cannot read MSR 0x1A4 from " + path.string());
            }
            originals.push_back({fd, original, false});
        }

        try {
            for (auto& original : originals) {
                if (::pwrite(original.fd, &kRandomXIntelMsrValue,
                             sizeof(kRandomXIntelMsrValue), kIntelMiscFeatureControl) !=
                    static_cast<ssize_t>(sizeof(kRandomXIntelMsrValue))) {
                    throw std::system_error(errno, std::generic_category(),
                                            "cannot write Intel MSR 0x1A4");
                }
                original.modified = true;
                std::uint64_t current_value = 0;
                if (::pread(original.fd, &current_value, sizeof(current_value),
                            kIntelMiscFeatureControl) !=
                        static_cast<ssize_t>(sizeof(current_value)) ||
                    current_value != kRandomXIntelMsrValue) {
                    throw std::runtime_error("Intel MSR 0x1A4 read-back verification failed");
                }
            }
            active = true;
        } catch (...) {
            restore();
            throw;
        }
    }

    void restore() noexcept {
        for (auto& original : originals) {
            if (original.modified) {
                if (::pwrite(original.fd, &original.value, sizeof(original.value),
                             kIntelMiscFeatureControl) !=
                    static_cast<ssize_t>(sizeof(original.value))) {
                    std::cerr << "MSR restore warning: failed to restore MSR 0x1A4\n";
                } else {
                    original.modified = false;
                }
            }
            if (original.fd >= 0) {
                ::close(original.fd);
                original.fd = -1;
            }
        }
        active = false;
    }
#endif

    ~Impl() {
        restore();
#ifdef _WIN32
        close_pawnio();
#endif
    }
};

MsrManager::MsrManager(std::filesystem::path module_path)
    : impl_(std::make_unique<Impl>(std::move(module_path))) {}

MsrManager::~MsrManager() = default;
MsrManager::MsrManager(MsrManager&&) noexcept = default;
MsrManager& MsrManager::operator=(MsrManager&&) noexcept = default;

void MsrManager::apply_randomx_preset() {
    if (!is_intel_cpu()) {
        throw std::runtime_error(
            "Native RandomX MSR preset currently supports Intel CPUs only");
    }
    impl_->apply();
}

void MsrManager::restore() noexcept {
    if (impl_) impl_->restore();
}

bool MsrManager::active() const noexcept {
    return impl_ && impl_->active;
}

} // namespace unique_rx
