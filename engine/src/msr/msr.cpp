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
#include <winsvc.h>
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

constexpr wchar_t kWinRingServiceName[] = L"WinRing0_1_2_0";
constexpr wchar_t kWinRingDeviceName[] = L"\\\\.\\WinRing0_1_2_0";
constexpr DWORD kWinRingReadMsrIoctl =
    CTL_CODE(40000, 0x821, METHOD_BUFFERED, FILE_ANY_ACCESS);
constexpr DWORD kWinRingWriteMsrIoctl =
    CTL_CODE(40000, 0x822, METHOD_BUFFERED, FILE_ANY_ACCESS);

struct ThreadAffinity {
    GROUP_AFFINITY previous{};
    bool active = false;
    bool restore_on_exit = true;

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
    HANDLE driver = INVALID_HANDLE_VALUE;
    SC_HANDLE manager = nullptr;
    SC_HANDLE service = nullptr;
    bool reuse_service = false;

    std::vector<msr_detail::RegisterSnapshot> snapshots;
    std::vector<std::pair<WORD, DWORD>> msr_processors;

    void close_winring() noexcept {
        if (driver != INVALID_HANDLE_VALUE) {
            CloseHandle(driver);
            driver = INVALID_HANDLE_VALUE;
        }
        if (service != nullptr) {
            if (!reuse_service) {
                SERVICE_STATUS status{};
                if (!ControlService(service, SERVICE_CONTROL_STOP, &status)) {
                    const DWORD error = GetLastError();
                    if (error != ERROR_SERVICE_NOT_ACTIVE &&
                        error != ERROR_SERVICE_CANNOT_ACCEPT_CTRL) {
                        try {
                            logging::warning("msr", "WinRing0 stop request failed: " +
                                std::to_string(error));
                        } catch (...) {}
                    }
                }
                if (!DeleteService(service)) {
                    try {
                        logging::warning("msr", "WinRing0 service removal failed: " +
                            std::to_string(GetLastError()));
                    } catch (...) {}
                }
            }
            CloseServiceHandle(service);
            service = nullptr;
        }
        if (manager != nullptr) {
            CloseServiceHandle(manager);
            manager = nullptr;
        }
    }

    void initialize_winring() {
        if (driver != INVALID_HANDLE_VALUE) return;

        manager = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_ALL_ACCESS);
        if (manager == nullptr) {
            const DWORD error = GetLastError();
            throw std::system_error(static_cast<int>(error), std::system_category(),
                error == ERROR_ACCESS_DENIED
                    ? "WinRing0 requires administrator privileges"
                    : "cannot open Windows Service Control Manager");
        }

        service = OpenServiceW(manager, kWinRingServiceName, SERVICE_ALL_ACCESS);
        if (service != nullptr) {
            SERVICE_STATUS status{};
            if (!QueryServiceStatus(service, &status)) {
                throw std::system_error(static_cast<int>(GetLastError()),
                    std::system_category(), "cannot query existing WinRing0 service");
            }
            if (status.dwCurrentState == SERVICE_RUNNING) {
                reuse_service = true;
            } else {
                if (!DeleteService(service)) {
                    throw std::system_error(static_cast<int>(GetLastError()),
                        std::system_category(), "cannot remove stopped WinRing0 service");
                }
                CloseServiceHandle(service);
                service = nullptr;
            }
        }

        // Reuse a WinRing0 device already loaded by another compatible process.
        driver = CreateFileW(kWinRingDeviceName, GENERIC_READ | GENERIC_WRITE,
            0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (driver != INVALID_HANDLE_VALUE) {
            reuse_service = true;
            return;
        }
        if (reuse_service) {
            throw std::system_error(static_cast<int>(GetLastError()),
                std::system_category(), "existing WinRing0 service is running but its device cannot be opened");
        }

        const auto driver_path = executable_directory() / L"WinRing0x64.sys";
        if (!std::filesystem::is_regular_file(driver_path)) {
            throw std::runtime_error(
                "WinRing0x64.sys was not found beside unique-rx.exe");
        }

        service = CreateServiceW(manager, kWinRingServiceName, kWinRingServiceName,
            SERVICE_ALL_ACCESS, SERVICE_KERNEL_DRIVER, SERVICE_DEMAND_START,
            SERVICE_ERROR_NORMAL, driver_path.c_str(), nullptr, nullptr, nullptr,
            nullptr, nullptr);
        if (service == nullptr) {
            throw std::system_error(static_cast<int>(GetLastError()),
                std::system_category(), "cannot install WinRing0 kernel driver service");
        }

        if (!StartServiceW(service, 0, nullptr)) {
            const DWORD error = GetLastError();
            if (error != ERROR_SERVICE_ALREADY_RUNNING) {
                throw std::system_error(static_cast<int>(error), std::system_category(),
                    "cannot start WinRing0 kernel driver");
            }
        }

        driver = CreateFileW(kWinRingDeviceName, GENERIC_READ | GENERIC_WRITE,
            0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (driver == INVALID_HANDLE_VALUE) {
            throw std::system_error(static_cast<int>(GetLastError()),
                std::system_category(), "cannot connect to WinRing0 device");
        }
    }

    std::uint64_t read_current_msr(std::uint32_t register_id) {
        std::uint64_t value = 0;
        DWORD returned = 0;
        if (driver == INVALID_HANDLE_VALUE ||
            !DeviceIoControl(driver, kWinRingReadMsrIoctl,
                &register_id, sizeof(register_id), &value, sizeof(value),
                &returned, nullptr) || returned != sizeof(value)) {
            throw std::system_error(static_cast<int>(GetLastError()),
                std::system_category(), "WinRing0 failed to read MSR " +
                std::to_string(register_id));
        }
        return value;
    }

    void write_current_msr(std::uint32_t register_id, std::uint64_t value) {
        struct WriteInput {
            std::uint32_t register_id;
            std::uint32_t value_words[2];
        } input{};
        static_assert(sizeof(WriteInput) == 12);
        input.register_id = register_id;
        std::memcpy(input.value_words, &value, sizeof(value));
        DWORD output = 0;
        DWORD returned = 0;
        if (driver == INVALID_HANDLE_VALUE ||
            !DeviceIoControl(driver, kWinRingWriteMsrIoctl,
                &input, sizeof(input), &output, sizeof(output),
                &returned, nullptr)) {
            throw std::system_error(static_cast<int>(GetLastError()),
                std::system_category(), "WinRing0 failed to write MSR " +
                std::to_string(register_id));
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
        initialize_winring();
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
        if (restore_on_exit) {
            restore();
            if (active) restore();
        }
        try {
            if (!restore_on_exit && active) {
                logging::warning("msr", "original CPU register values intentionally left modified by randomx.rdmsr=false");
            } else if (had_snapshots && !active) {
                logging::info("msr", "original CPU register values restored");
            } else if (active) {
                logging::error("msr", "register restore remains pending after retry");
            }
        } catch (...) {
            // Logging must never interfere with register restoration or shutdown.
        }
#ifdef _WIN32
        close_winring();
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

void MsrManager::apply_randomx_preset(std::optional<std::uint64_t> intel_prefetch_value) {
    const auto cpu = detect_cpu_identity();
    if (!cpu) {
        throw std::runtime_error("cannot identify CPU vendor/family/model for MSR tuning");
    }
    std::optional<MsrPreset> preset;
    if (intel_prefetch_value && cpu->vendor == "GenuineIntel") {
        if (*intel_prefetch_value > 0x0f) {
            throw std::invalid_argument("Intel RandomX prefetch preset must be between 0 and 15");
        }
        preset = MsrPreset{"intel_custom", {
            MsrWrite{0x1a4, *intel_prefetch_value, UINT64_MAX}
        }};
    } else {
        preset = msr_preset_for_cpu(cpu->vendor, cpu->family, cpu->model);
    }
    if (!preset) {
        throw std::runtime_error(
            "no verified RandomX MSR preset exists for this CPU family/model");
    }
#ifdef _WIN32
    if (cpu->vendor != "GenuineIntel") {
        throw std::runtime_error(
            "Windows MSR backend currently supports Intel WinRing0 only");
    }
#endif
    impl_->apply(*preset);
    logging::info("msr", "applied verified preset " + std::string(preset->name) +
        " to " + cpu->vendor + " family " + std::to_string(cpu->family) +
        " model 0x" + [&] {
            std::ostringstream value;
            value << std::hex << cpu->model;
            return value.str();
        }());
}

void MsrManager::apply_custom_writes(const std::vector<MsrWrite>& writes) {
    if (writes.empty()) {
        throw std::invalid_argument("custom MSR register list is empty");
    }
    for (const auto& item : writes) {
        if (item.register_id == 0 || item.register_id > UINT32_MAX) {
            throw std::invalid_argument("custom MSR register address must be between 1 and 0xffffffff");
        }
    }
    impl_->apply(MsrPreset{"custom", writes});
    logging::info("msr", "applied " + std::to_string(writes.size()) +
        " custom XMRig MSR register entries");
}

void MsrManager::set_restore_on_exit(bool enabled) noexcept {
    if (impl_) impl_->restore_on_exit = enabled;
}

void MsrManager::restore() noexcept {
    if (impl_) impl_->restore();
}

bool MsrManager::active() const noexcept {
    return impl_ && impl_->active;
}

} // namespace unique_rx
