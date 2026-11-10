#include "stratum.hpp"
#include "core/mining.hpp"
#include "cpu/topology.hpp"
#include "randomx/cache.hpp"
#include "randomx/dataset.hpp"
#include "msr/msr.hpp"
#include "platform/socket.hpp"
#include "stratum/transport.hpp"
#include "logging/log.hpp"
#include "telemetry/system.hpp"
#include "hashvault/api.hpp"

#ifdef _WIN32
#include <windows.h>
#include <io.h>
#include <conio.h>
#else
#include <unistd.h>
#include <poll.h>
#endif
#include <cstdio>

#include <randomx.h>
#include "virtual_memory.h"


#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cmath>
#include <csignal>
#include <cstring>
#include <deque>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <iomanip>
#include <limits>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <sstream>
#include <string>
#include <thread>
#include <stop_token>
#include <unordered_map>
#ifdef __linux__
#include <sys/sysinfo.h>
#include <sys/ioctl.h>
#endif

namespace unique_rx {
namespace {

using core::Hash;
using core::MiningState;
using core::MiningWork;
using core::Share;
using core::hash_worker;

volatile std::sig_atomic_t g_stop_requested = 0;

// The backend reads machine telemetry from redirected stdout. When the miner
// is launched directly in a console, keep that JSON off-screen and show only
// the human-readable XMRig-style shell.
bool machine_status_channel_required() {
#ifdef _WIN32
    return _isatty(_fileno(stdout)) == 0;
#else
    return ::isatty(STDOUT_FILENO) == 0;
#endif
}

bool console_input_available() {
#ifdef _WIN32
    return _isatty(_fileno(stdin)) != 0;
#else
    return ::isatty(STDIN_FILENO) != 0;
#endif
}

// Publish the real engine snapshot independently of stdout so a miner started
// by the user's main BAT can be observed by the local dashboard. This does not
// alter Stratum messages, authentication, or config parsing.
bool write_status_snapshot(const std::filesystem::path& path,
                           const nlohmann::json& status) noexcept {
    auto temporary = path;
    temporary += ".tmp";
    try {
        {
            std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
            if (!output) return false;
            output << status.dump() << '\n';
            output.flush();
            if (!output) {
                output.close();
                std::error_code ignored;
                std::filesystem::remove(temporary, ignored);
                return false;
            }
        }
#ifdef _WIN32
        if (!MoveFileExW(temporary.c_str(), path.c_str(),
                         MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            DeleteFileW(temporary.c_str());
            return false;
        }
#else
        std::error_code error;
        std::filesystem::rename(temporary, path, error);
        if (error) {
            std::error_code ignored;
            std::filesystem::remove(temporary, ignored);
            return false;
        }
#endif
        return true;
    } catch (...) {
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
        return false;
    }
}

std::uint64_t current_process_id() noexcept {
#ifdef _WIN32
    return static_cast<std::uint64_t>(GetCurrentProcessId());
#else
    return static_cast<std::uint64_t>(::getpid());
#endif
}

std::uint64_t available_physical_memory_bytes() noexcept {
#ifdef _WIN32
    MEMORYSTATUSEX status{};
    status.dwLength = sizeof(status);
    return GlobalMemoryStatusEx(&status) ? status.ullAvailPhys : 0;
#elif defined(__linux__)
    struct sysinfo info{};
    if (::sysinfo(&info) != 0) return 0;
    return static_cast<std::uint64_t>(info.freeram) *
           static_cast<std::uint64_t>(info.mem_unit);
#else
    return 0;
#endif
}

void request_stop(int) {
    g_stop_requested = 1;
}

#ifdef _WIN32
BOOL WINAPI console_stop_handler(DWORD) {
    g_stop_requested = 1;
    return TRUE;
}
#endif

std::string hex_encode(const std::uint8_t* bytes, std::size_t length) {
    static constexpr char digits[] = "0123456789abcdef";
    std::string result(length * 2, '0');
    for (std::size_t index = 0; index < length; ++index) {
        result[index * 2] = digits[bytes[index] >> 4];
        result[index * 2 + 1] = digits[bytes[index] & 0x0f];
    }
    return result;
}


} // namespace

int run_miner(const std::filesystem::path& config_path,
              std::uint64_t maximum_hashes,
              std::uint32_t thread_override) {
    g_stop_requested = 0;
    std::signal(SIGINT, request_stop);
    std::signal(SIGTERM, request_stop);
#ifdef _WIN32
    SetConsoleCtrlHandler(console_stop_handler, TRUE);
#endif
    telemetry::SystemTelemetry system_telemetry;
    MiningState state;
    std::vector<std::jthread> workers;
    std::vector<std::shared_ptr<std::atomic<bool>>> worker_stop_flags;
    struct StopWorkers {
        MiningState& state;
        ~StopWorkers() { state.stop.store(true, std::memory_order_release); }
    } stop_workers{state};
    std::jthread shell_input_thread;

    using Clock = std::chrono::steady_clock;
    constexpr auto kShareResponseTimeout = std::chrono::seconds(60);
    // Match RxMiner's 8192-entry pending-submit backpressure ceiling.
    constexpr std::size_t kMaximumPendingShares = 8192;

    // Open the persistent engine log before CPU/MSR initialization so early
    // hardware and configuration diagnostics are recorded as well.
    logging::set_file(config_path.parent_path() / "unique-rx-stratum.log");
    // The asynchronous logger owns a writer thread. Always drain and join it
    // before run_miner returns, including CLI process shutdown and exceptions.
    struct CloseLoggerOnExit {
        ~CloseLoggerOnExit() {
            logging::flush();
            logging::close_file();
        }
    } close_logger_on_exit;
    const auto status_path = config_path.parent_path() / "unique-rx-status.json";
    const auto command_path = config_path.parent_path() / "unique-rx-command.json";

    try {
        std::ifstream file(config_path);
        if (!file) throw std::runtime_error("cannot open config.json");
        auto config = nlohmann::json::parse(file);
        if (!config.is_object())
            throw std::runtime_error("XMRig config root must be a JSON object");
        if (config.contains("cpu") && config["cpu"].is_boolean()) {
            config["cpu"] = nlohmann::json{{"enabled", config["cpu"].get<bool>()}};
        } else if (config.contains("cpu") && !config["cpu"].is_object()) {
            throw std::runtime_error("cpu must be an object or boolean, as in XMRig config");
        }
        if (config.contains("randomx") && !config["randomx"].is_object()) {
            throw std::runtime_error("randomx must be an object, as in XMRig config");
        }
        if (config.contains("log-file") && !config["log-file"].is_null()) {
            if (!config["log-file"].is_string())
                throw std::runtime_error("log-file must be a string path");
            std::filesystem::path log_path = config["log-file"].get<std::string>();
            if (log_path.is_relative()) log_path = config_path.parent_path() / log_path;
            logging::set_file(log_path);
        }
        logging::Config logger_config;
        if (config.contains("colors") && !config["colors"].is_null()) {
            if (!config["colors"].is_boolean())
                throw std::runtime_error("colors must be boolean");
            logger_config.colored_output = config["colors"].get<bool>();
        }
        // Keep normal output at XMRig's non-verbose level. The imported
        // "verbose" key is accepted for config compatibility but does not
        // expose internal engine/debug diagnostics in the user log.
        if (config.contains("verbose") && !config["verbose"].is_null() &&
            !config["verbose"].is_boolean())
            throw std::runtime_error("verbose must be boolean");
        // In an interactive native shell, the TUI owns the terminal surface:
        // keep structured XMRig-style records in the rotating log/history pane,
        // never interleave asynchronous-looking log lines with the live screen.
        // When stdout is redirected, emit only the machine-readable status channel.
        logger_config.console_output =
            !console_input_available() && !machine_status_channel_required();
        logging::configure(logger_config);
        if (config.contains("background") && !config["background"].is_null() &&
            config["background"].get<bool>())
            logging::warning("config", "background mode is not supported; keep the miner attached to its console/backend");
        if (config.contains("autosave") && !config["autosave"].is_null() &&
            config["autosave"].get<bool>())
            logging::warning("config", "autosave is recognized but runtime changes are not written back to config.json automatically");
        if (config.contains("pause-on-battery") && !config["pause-on-battery"].is_null() &&
            config["pause-on-battery"].get<bool>())
            logging::warning("config", "pause-on-battery is not implemented by the native engine; stop the miner manually when required");
        if (config.contains("http") && config["http"].is_object() &&
            config["http"].contains("enabled") && !config["http"]["enabled"].is_null() &&
            config["http"]["enabled"].get<bool>())
            logging::warning("config", "XMRig http API settings are recognized but the integrated dashboard/backend owns the local API; no separate XMRig HTTP listener is started");
        if (config.contains("donate-level"))
            logging::warning("config", "donate-level is not applied: UNIQUE RX does not redirect mining time to a developer pool");
        if (config.contains("syslog") && config["syslog"].is_boolean() &&
            config["syslog"].get<bool>())
            logging::warning("config", "syslog output is not implemented; file and console logging remain active");
        if (config.contains("title") && !config["title"].is_null())
            logging::info("config", "title setting accepted; console title management is not enabled in this native build");
        if (thread_override > 0) {
            if (thread_override > 256)
                throw std::invalid_argument("--threads must be between 1 and 256");
            if (!config.contains("cpu") || !config["cpu"].is_object())
                config["cpu"] = nlohmann::json::object();
            auto& cpu = config["cpu"];
            if (cpu.contains("rx") && cpu["rx"].is_object()) {
                cpu["rx"]["threads"] = thread_override;
            } else if (cpu.contains("rx") && cpu["rx"].is_array()) {
                const auto previous_profile = cpu["rx"];
                nlohmann::json overridden_profile = nlohmann::json::array();
                for (std::uint32_t index = 0; index < thread_override; ++index) {
                    if (index < previous_profile.size()) {
                        overridden_profile.push_back(previous_profile[index]);
                    } else {
                        overridden_profile.push_back(-1);
                    }
                }
                cpu["rx"] = std::move(overridden_profile);
            } else {
                cpu["threads"] = thread_override;
            }
            logging::info("cpu", "thread count overridden from shell: " +
                std::to_string(thread_override));
        }
        // Accept the same top-level algorithm and RandomX mode fields used by
        // XMRig config.json. This engine intentionally implements rx/0 only.
        if (config.contains("algo") && !config["algo"].is_null()) {
            const auto algorithm = config["algo"].get<std::string>();
            if (algorithm != "rx/0" && algorithm != "rx" &&
                algorithm != "randomx" && algorithm != "randomx/0") {
                throw std::runtime_error("unsupported algo '" + algorithm +
                                         "': UNIQUE RX supports RandomX rx/0 only");
            }
        }
        const auto pools = parse_pool_configs(config);
        std::string user_agent = config.contains("user-agent") && !config["user-agent"].is_null()
            ? config["user-agent"].get<std::string>()
            : std::string("UniqueRX/1.0 (C++ RandomX; XMRig-compatible Stratum)");
        if (user_agent.empty() || user_agent.size() > 256 ||
            user_agent.find_first_of("\r\n") != std::string::npos)
            throw std::runtime_error("user-agent must contain 1 to 256 characters without newlines");
        const auto randomx_config = config.value("randomx", nlohmann::json::object());
        const auto select_auto_memory_mode = [&] {
            const auto available = available_physical_memory_bytes();
            const auto dataset_bytes =
                static_cast<std::uint64_t>(randomx_dataset_item_count()) * 64ULL;
            const auto scratchpad_bytes =
                static_cast<std::uint64_t>(pools.front().threads) * 2ULL * 1024ULL * 1024ULL;
            constexpr std::uint64_t reserve_bytes = 512ULL * 1024ULL * 1024ULL;
            const auto required = dataset_bytes + scratchpad_bytes + reserve_bytes;
            const bool use_full = available != 0 && available >= required;
            logging::info("memory", "RandomX auto mode selected " +
                std::string(use_full ? "fast/full dataset" : "light/cache") +
                " | available RAM " + std::to_string(available / (1024ULL * 1024ULL)) +
                " MiB | estimated fast requirement " +
                std::to_string(required / (1024ULL * 1024ULL)) + " MiB");
            return use_full;
        };
        bool full_memory = randomx_config.contains("full-memory")
            ? randomx_config["full-memory"].get<bool>() : true;
        if (randomx_config.contains("mode") && !randomx_config["mode"].is_null()) {
            const auto& configured_mode = randomx_config["mode"];
            std::string mode;
            if (configured_mode.is_string()) {
                mode = configured_mode.get<std::string>();
            } else if (configured_mode.is_number_integer()) {
                const auto numeric_mode = configured_mode.get<std::int64_t>();
                if (numeric_mode < 0 || numeric_mode > 2)
                    throw std::runtime_error("randomx.mode numeric value must be 0 (auto), 1 (fast), or 2 (light)");
                mode = numeric_mode == 0 ? "auto" : numeric_mode == 1 ? "fast" : "light";
            } else {
                throw std::runtime_error("randomx.mode must be auto, fast, light, or XMRig numeric mode 0..2");
            }
            std::transform(mode.begin(), mode.end(), mode.begin(),
                [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
            if (mode == "fast") full_memory = true;
            else if (mode == "light") full_memory = false;
            else if (mode == "auto") full_memory = select_auto_memory_mode();
            else throw std::runtime_error("randomx.mode must be auto, fast, or light");
        } else if (!randomx_config.contains("full-memory")) {
            // XMRig default is auto: choose fast/full dataset only when RAM
            // headroom is sufficient; explicit legacy full-memory overrides it.
            full_memory = select_auto_memory_mode();
        }
        const auto wrmsr = randomx_config.value("wrmsr", nlohmann::json());
        // Imported XMRig configurations may contain a boolean, a preset index,
        // or an explicit register list. Only the boolean true can select the
        // native CPU-specific preset. Numeric/list forms are accepted as
        // configuration data but are never written by this native backend.
        const bool msr_enabled = wrmsr.is_boolean() && wrmsr.get<bool>();
        const bool msr_custom_requested =
            wrmsr.is_array() ||
            (wrmsr.is_number_integer() && wrmsr.get<std::int64_t>() >= 0);
        const bool msr_explicitly_disabled = !msr_enabled;
        if (!wrmsr.is_null() && !wrmsr.is_boolean() &&
            !wrmsr.is_array() && !wrmsr.is_number_integer()) {
            throw std::runtime_error("randomx.wrmsr must be boolean, integer preset, or XMRig register array");
        }
        if (wrmsr.is_number_integer()) {
            const auto preset = wrmsr.get<std::int64_t>();
            if (preset < -1 || preset > 15)
                throw std::runtime_error("randomx.wrmsr numeric preset must be -1..15");
        }
        if (wrmsr.is_array()) {
            for (const auto& item : wrmsr) {
                if (!item.is_array() || item.size() < 2 ||
                    !item[0].is_number_integer() || !item[1].is_number_integer()) {
                    throw std::runtime_error("randomx.wrmsr entries must be XMRig [register, value, optional mask] arrays");
                }
            }
        }
        MsrManager msr(randomx_config.value("msr-module", std::string()));
        std::string msr_status;
        if (msr_explicitly_disabled) {
            msr_status = msr_custom_requested
                ? "Configurazione XMRig MSR personalizzata riconosciuta ma non applicata dal backend nativo"
                : "Disattivato (default XMRig/configurazione)";
            if (msr_custom_requested) {
                logging::warning("msr", "XMRig custom/numeric wrmsr profile parsed but not applied; native backend only supports its guarded CPU-specific preset. No custom register writes were attempted.");
            } else {
                logging::info("msr", "MSR tuning disabled (XMRig default or explicit configuration)");
            }
        } else {
            try {
                msr.apply_randomx_preset();
                msr_status = "Preset CPU applicato; ripristino automatico alla chiusura";
                logging::info("msr", "CPU-specific RandomX preset applied; original registers will be restored on exit");
            } catch (const std::exception& error) {
                // MSR tuning is an optional optimization. An unknown CPU, a missing
                // driver, or unavailable permissions must never prevent rx/0 mining.
                msr_status = std::string("Non disponibile: ") + error.what();
                logging::warning("msr", std::string("tuning skipped safely: ") + error.what() +
                                  "; continuing with standard CPU settings");
            }
        }

        // This guard is declared after MsrManager so worker threads are stopped
        // and joined before the MSR manager restores the original CPU registers.
        struct JoinWorkersBeforeMsrRestore {
            MiningState& state;
            std::vector<std::jthread>& workers;
            ~JoinWorkersBeforeMsrRestore() {
                state.stop.store(true, std::memory_order_release);
                for (auto& worker : workers) {
                    if (worker.joinable()) worker.join();
                }
            }
        } join_workers_before_msr_restore{state, workers};

        platform::NetworkRuntime network;
        std::array<std::uint8_t, 32> active_seed{};
        bool have_seed = false;
        std::shared_ptr<randomx_cache> cache;
        std::shared_ptr<randomx_dataset> dataset;
        auto flags = randomx_get_flags();
        const auto cpu_config = config.value("cpu", nlohmann::json::object());
        if (cpu_config.contains("hw-aes") && !cpu_config["hw-aes"].is_null()) {
            if (!cpu_config["hw-aes"].is_boolean())
                throw std::runtime_error("cpu.hw-aes must be boolean or null");
            if (!cpu_config["hw-aes"].get<bool>()) {
                flags = static_cast<randomx_flags>(static_cast<unsigned int>(flags) & ~static_cast<unsigned int>(RANDOMX_FLAG_HARD_AES));
                logging::info("cpu", "hardware AES disabled by XMRig-compatible cpu.hw-aes setting");
            }
        }
        int cpu_priority = -1; // XMRig-compatible null/default: leave OS priority unchanged.
        if (cpu_config.contains("priority") && !cpu_config["priority"].is_null()) {
            if (!cpu_config["priority"].is_number_integer())
                throw std::runtime_error("cpu.priority must be an integer or null");
            const auto configured_priority = cpu_config["priority"].get<std::int64_t>();
            if (configured_priority < -1 || configured_priority > 5)
                throw std::runtime_error("cpu.priority must be between -1 and 5");
            cpu_priority = static_cast<int>(configured_priority);
        }
        state.cpu_priority.store(cpu_priority, std::memory_order_relaxed);
        if (randomx_config.contains("numa") && !randomx_config["numa"].is_null()) {
            if (!randomx_config["numa"].is_boolean()) {
                logging::warning("config", "randomx.numa node-list form is not supported; use true or false");
            } else {
                state.cpu_numa_enabled.store(randomx_config["numa"].get<bool>(),
                                             std::memory_order_relaxed);
            }
        }
        if (randomx_config.contains("rdmsr") && !randomx_config["rdmsr"].is_null()) {
            if (!randomx_config["rdmsr"].is_boolean())
                throw std::runtime_error("randomx.rdmsr must be boolean");
            logging::info("config", randomx_config["rdmsr"].get<bool>()
                ? "randomx.rdmsr recognized; native RandomX engine does not require MSR reads"
                : "randomx.rdmsr=false accepted; native engine does not read MSRs for mining");
        }
        if (randomx_config.contains("cache_qos") && !randomx_config["cache_qos"].is_null() &&
            randomx_config["cache_qos"].get<bool>()) {
            logging::warning("config", "randomx.cache_qos is recognized but cache QoS allocation is not implemented");
        }
        if (cpu_config.contains("asm") && !cpu_config["asm"].is_null()) {
            const auto& assembly = cpu_config["asm"];
            if (!assembly.is_boolean() && !assembly.is_string())
                throw std::runtime_error("cpu.asm must be boolean or one of none, auto, intel, ryzen, bulldozer");
            if (assembly.is_string()) {
                auto name = assembly.get<std::string>();
                std::transform(name.begin(), name.end(), name.begin(),
                    [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
                if (name != "none" && name != "auto" && name != "intel" &&
                    name != "ryzen" && name != "bulldozer")
                    throw std::runtime_error("cpu.asm must be none, auto, intel, ryzen, or bulldozer");
            }
            logging::warning("config", "cpu.asm XMRig selection is parsed; RandomX JIT instruction dispatch remains governed by the detected CPU features and is not overridden by this field");
        }
        bool cpu_yield = false; // Throughput-first default; users can enable XMRig-style yielding explicitly.
        if (cpu_config.contains("yield") && !cpu_config["yield"].is_null()) {
            if (!cpu_config["yield"].is_boolean())
                throw std::runtime_error("cpu.yield must be boolean or null");
            cpu_yield = cpu_config["yield"].get<bool>();
        }
        state.cpu_yield.store(cpu_yield, std::memory_order_relaxed);

        // XMRig enables RandomX large pages by default. Keep an explicit false
        // opt-out, but do not silently disable this important optimization.
        bool large_pages_requested = true;
        if (cpu_config.contains("huge-pages") && !cpu_config["huge-pages"].is_null()) {
            const auto& huge_pages = cpu_config["huge-pages"];
            if (huge_pages.is_boolean()) {
                large_pages_requested = huge_pages.get<bool>();
            } else if (huge_pages.is_number_unsigned() || huge_pages.is_number_integer()) {
                // XMRig accepts a page-size value as well as bool. RandomX's
                // public allocator exposes standard large pages as one flag,
                // so any positive supported request maps to that allocator.
                if (huge_pages.is_number_integer() &&
                    huge_pages.get<std::int64_t>() < 0) {
                    throw std::runtime_error("cpu.huge-pages must be boolean or a non-negative page size");
                }
                large_pages_requested = huge_pages.get<std::uint64_t>() > 0;
            } else {
                throw std::runtime_error("cpu.huge-pages must be boolean or an integer page size");
            }
        }
        if (large_pages_requested) flags |= RANDOMX_FLAG_LARGE_PAGES;
        if (randomx_config.value("1gb-pages", false)) {
            logging::warning("memory",
                "randomx.1gb-pages is recognized but this RandomX backend exposes only standard large-page allocation; 1 GiB pages are not claimed or forced");
        }

        // Accept both the README's legacy key and XMRig's native spelling.
        const bool jit_huge_pages = cpu_config.contains("huge-pages-jit")
            ? cpu_config.at("huge-pages-jit").get<bool>()
            : cpu_config.value("jit-huge-pages", false);
        rxm_set_jit_huge_pages(jit_huge_pages ? 1 : 0);

        int prefetch_mode = 1;
        if (cpu_config.contains("prefetch-mode")) {
            prefetch_mode = cpu_config.at("prefetch-mode").get<int>();
        } else if (randomx_config.contains("scratchpad_prefetch_mode")) {
            prefetch_mode = randomx_config.at("scratchpad_prefetch_mode").get<int>();
        }
        if (prefetch_mode < 0 || prefetch_mode > 3)
            throw std::runtime_error("cpu.prefetch-mode/randomx.scratchpad_prefetch_mode must be between 0 and 3");
        rxm_set_prefetch_mode(prefetch_mode);
        std::uint64_t generation = 0;
        std::uint32_t pool_threads = pools.front().threads;
        const std::uint32_t automatic_threads = pool_threads;
        std::uint32_t dataset_init_threads = pool_threads;
        if (randomx_config.contains("init") && !randomx_config["init"].is_null()) {
            if (!randomx_config["init"].is_number_integer())
                throw std::runtime_error("randomx.init must be an integer");
            const auto requested = randomx_config["init"].get<std::int64_t>();
            if (requested < -1 || requested > 256)
                throw std::runtime_error("randomx.init must be -1 (auto) or between 0 and 256");
            if (requested > 0)
                dataset_init_threads = std::min(static_cast<std::uint32_t>(requested),
                                                detected_logical_threads());
        }
        if (randomx_config.contains("init-avx2") &&
            !randomx_config["init-avx2"].is_null()) {
            logging::info("config", "randomx.init-avx2 recognized; RandomX upstream selects the available AVX2 dataset initializer automatically");
        }
        state.initialize_worker_counters(256);
        auto desired_threads = std::make_shared<std::atomic<std::uint32_t>>(pool_threads);
        // A tiny local file-based control channel lets the dashboard operate
        // a miner launched independently by the preserved main BAT.
        const auto consume_dashboard_command = [&] {
            std::error_code exists_error;
            if (!std::filesystem::exists(command_path, exists_error) || exists_error) return;
            nlohmann::json request;
            try {
                std::ifstream input(command_path, std::ios::binary);
                if (!input) return;
                input >> request;
            } catch (...) {
                std::error_code ignored;
                std::filesystem::remove(command_path, ignored);
                logging::warning("dashboard", "ignored unreadable local control request");
                return;
            }
            std::error_code ignored;
            std::filesystem::remove(command_path, ignored);
            if (!request.is_object()) return;
            const auto issued = request.value("timestamp_unix_ms", std::int64_t{0});
            const auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();
            if (issued <= 0 || now_ms - issued > 15000 || issued - now_ms > 5000) return;
            const auto target_pid = request.value("target_pid", std::uint64_t{0});
            if (target_pid != 0 && target_pid != current_process_id()) return;
            const auto command = request.value("command", std::string());
            if (command == "stop") {
                logging::info("dashboard", "graceful stop requested by local dashboard");
                state.stop.store(true, std::memory_order_release);
            } else if (command == "threads") {
                const auto value = request.value("value", nlohmann::json());
                if (value.is_string() && value.get<std::string>() == "auto") {
                    desired_threads->store(automatic_threads, std::memory_order_release);
                    logging::info("cpu", "dashboard restored automatic worker count: " +
                        std::to_string(automatic_threads));
                } else if (value.is_number_integer()) {
                    const auto requested = value.get<std::int64_t>();
                    if (requested >= 1 && requested <= 256) {
                        desired_threads->store(static_cast<std::uint32_t>(requested),
                                                std::memory_order_release);
                        logging::info("cpu", "dashboard requested " +
                            std::to_string(requested) + " worker(s)");
                    } else {
                        logging::warning("cpu", "dashboard thread request outside 1..256 ignored");
                    }
                }
            }
        };
        auto active_pool_index = std::make_shared<std::atomic<std::size_t>>(0);
        auto runtime_prefetch_mode = std::make_shared<std::atomic<int>>(prefetch_mode);
        const auto cpu_summary = cpu_summary_json();
        std::jthread status_heartbeat_thread;
        nlohmann::json startup_status = {
            {"state", "AVVIO"}, {"managed", true}, {"error", nullptr},
            {"hashrate", {{"10s", nullptr}, {"60s", nullptr}, {"15m", nullptr}}},
            {"shares", {{"found", 0}, {"submitted", 0}, {"accepted", 0},
                        {"rejected", 0}, {"stale", 0}, {"pending", 0},
                        {"expired", 0}, {"dropped", 0}, {"total", 0}}},
            {"uptime_seconds", 0}, {"algorithm", "rx/0"},
            {"job_id", nullptr}, {"height", nullptr}, {"threads", pool_threads},
            {"cpu", cpu_summary}, {"workers", nlohmann::json::array()},
            {"pool", pools.front().host + ":" + pools.front().port},
            {"connected", false}, {"pid", current_process_id()},
        };
        startup_status["timestamp_unix_ms"] =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();
        (void)write_status_snapshot(status_path, startup_status);
        // Keep the snapshot fresh while slow RandomX dataset initialization or
        // pool connection is in progress. The heartbeat stops with this process.
        status_heartbeat_thread = std::jthread(
            [status_path, &consume_dashboard_command](std::stop_token stop_token) {
                auto next_touch = std::chrono::steady_clock::now();
                while (!stop_token.stop_requested()) {
                    consume_dashboard_command();
                    if (stop_token.stop_requested()) break;
                    const auto now = std::chrono::steady_clock::now();
                    if (now >= next_touch) {
                        std::error_code error;
                        if (std::filesystem::exists(status_path, error) && !error) {
                            std::filesystem::last_write_time(
                                status_path, std::filesystem::file_time_type::clock::now(), error);
                        }
                        next_touch = now + std::chrono::seconds(1);
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                }
            });
        // XMRig-compatible persistent scratchpad pool: false disables it,
        // true sizes it for the active workers, and a number sets capacity.
        const auto automatic_memory_pool_capacity = [&] {
            std::uint64_t l3_bytes = 0;
            if (cpu_summary.contains("l3_cache_bytes") &&
                cpu_summary["l3_cache_bytes"].is_number()) {
                l3_bytes = cpu_summary["l3_cache_bytes"].get<std::uint64_t>();
            }
            const auto l3_pages = static_cast<std::size_t>(
                l3_bytes / (2ULL * 1024ULL * 1024ULL));
            return std::max<std::size_t>(pool_threads, l3_pages);
        };
        // Reuse 2 MiB RandomX worker scratchpads by default. XMRig-style
        // false/0 remains an explicit opt-out; -1/true requests auto sizing.
        std::size_t memory_pool_capacity = automatic_memory_pool_capacity();
        if (cpu_config.contains("memory-pool") && !cpu_config["memory-pool"].is_null()) {
            const auto& memory_pool = cpu_config["memory-pool"];
            if (memory_pool.is_boolean()) {
                memory_pool_capacity = memory_pool.get<bool>()
                    ? automatic_memory_pool_capacity() : 0;
            } else if (memory_pool.is_number_integer()) {
                const auto requested = memory_pool.get<std::int64_t>();
                if (requested < -1 || requested > 256)
                    throw std::runtime_error("cpu.memory-pool must be false, true, -1, or an integer from 0 to 256");
                memory_pool_capacity = requested <= 0
                    ? (requested == -1 ? automatic_memory_pool_capacity() : 0)
                    : static_cast<std::size_t>(requested);
            } else {
                throw std::runtime_error("cpu.memory-pool must be boolean or integer");
            }
        }
        rxm_set_memory_pool(memory_pool_capacity);
        logging::info("memory", memory_pool_capacity == 0
            ? "RandomX scratchpad memory pool disabled"
            : "RandomX scratchpad memory pool capacity " +
                std::to_string(memory_pool_capacity) + " pages");
        auto started = Clock::now();
        const auto configured_log_interval = [&](const char* key, std::uint32_t fallback) {
            if (!config.contains(key) || config[key].is_null()) return fallback;
            if (!config[key].is_number_integer())
                throw std::runtime_error(std::string(key) + " must be an integer number of seconds");
            const auto value = config[key].get<std::int64_t>();
            if (value < 0 || value > 86400)
                throw std::runtime_error(std::string(key) + " must be between 0 and 86400 seconds");
            return static_cast<std::uint32_t>(value);
        };
        // XMRig-compatible timing controls; the existing 5-second speed line
        // remains the default when no explicit print-time is supplied.
        const auto print_interval = configured_log_interval("print-time", 5);
        const auto health_interval = configured_log_interval("health-print-time", 60);
        std::vector<std::uint64_t> worker_previous_hashes(pool_threads, 0);
        std::vector<double> worker_rates(pool_threads, 0.0);
        std::vector<Clock::time_point> worker_sample_times(pool_threads, started);
        if (console_input_available()) {
            shell_input_thread = std::jthread(
                [&state, desired_threads, active_pool_index, automatic_threads, runtime_prefetch_mode, &cpu_summary, &config, &pools, &msr_status, full_memory, large_pages_requested, jit_huge_pages, started](std::stop_token stop_token) {
                    std::string command_line;
                    std::uint64_t previous_hashes = state.hashes.load(std::memory_order_relaxed);
                    std::array<std::uint64_t, 256> previous_worker_hashes{};
                    std::array<double, 256> worker_hashrates{};
                    std::deque<double> shell_rate_history;
                    std::size_t log_scroll = 0;
                    hashvault::Snapshot hashvault_snapshot;
                    std::mutex hashvault_mutex;
                    const std::string hashvault_wallet = pools.empty() ? std::string() : pools.front().wallet;
                    std::jthread hashvault_worker([&](std::stop_token api_stop) {
                        while (!api_stop.stop_requested()) {
                            hashvault::Snapshot next;
                            hashvault::refresh(next, hashvault_wallet, false, "daily", 10);
                            {
                                std::lock_guard lock(hashvault_mutex);
                                hashvault_snapshot = std::move(next);
                            }
                            for (int second = 0; second < 60 && !api_stop.stop_requested(); ++second)
                                std::this_thread::sleep_for(std::chrono::seconds(1));
                        }
                    });
                    telemetry::SystemTelemetry shell_telemetry;
                    auto previous_sample = Clock::now();
                    double shell_hashrate = 0.0;
                    auto last_command_at = Clock::time_point::min();
                    const auto average_rate = [&](std::size_t window) {
                        const auto count = std::min(window, shell_rate_history.size());
                        if (count == 0) return 0.0;
                        double total = 0.0;
                        auto it = shell_rate_history.end() - static_cast<std::ptrdiff_t>(count);
                        for (; it != shell_rate_history.end(); ++it) total += *it;
                        return total / static_cast<double>(count);
                    };
#ifdef _WIN32
                    HANDLE shell_output = GetStdHandle(STD_OUTPUT_HANDLE);
                    DWORD shell_console_mode = 0;
                    if (shell_output != INVALID_HANDLE_VALUE &&
                        GetConsoleMode(shell_output, &shell_console_mode)) {
                        SetConsoleMode(shell_output, shell_console_mode |
                            ENABLE_VIRTUAL_TERMINAL_PROCESSING);
                    }
#endif
                    const auto render_operator_screen = [&] {
                        std::ostringstream left_screen;
                        auto* const original_output = std::cout.rdbuf(left_screen.rdbuf());
                        const auto active_index = std::min(
                            active_pool_index->load(std::memory_order_relaxed), pools.size() - 1);
                        const auto& active_pool = pools[active_index];
                        const auto work = state.current_work();
                        const auto features = cpu_summary.value("features", nlohmann::json::object());
                        const auto rate_text = [](double rate) {
                            const char* units[] = {"H/s", "kH/s", "MH/s", "GH/s"};
                            std::size_t unit = 0;
                            while (rate >= 1000.0 && unit < 3) { rate /= 1000.0; ++unit; }
                            std::ostringstream out;
                            out << std::fixed << std::setprecision(2) << rate << ' ' << units[unit];
                            return out.str();
                        };
                        const auto line = [](const std::string& label, const std::string& value) {
                            std::cout << "  " << std::left << std::setw(18) << label
                                      << " " << value << '\n';
                        };
                        const auto terminal_dimensions = []() {
                            std::array<std::size_t, 2> dimensions{100, 40};
#ifdef _WIN32
                            CONSOLE_SCREEN_BUFFER_INFO info{};
                            const auto output = GetStdHandle(STD_OUTPUT_HANDLE);
                            if (output != INVALID_HANDLE_VALUE &&
                                GetConsoleScreenBufferInfo(output, &info)) {
                                dimensions[0] = static_cast<std::size_t>(
                                    info.srWindow.Right - info.srWindow.Left + 1);
                                dimensions[1] = static_cast<std::size_t>(
                                    info.srWindow.Bottom - info.srWindow.Top + 1);
                            }
#elif defined(__linux__)
                            struct winsize size{};
                            if (::ioctl(STDOUT_FILENO, TIOCGWINSZ, &size) == 0) {
                                if (size.ws_col > 0) dimensions[0] = size.ws_col;
                                if (size.ws_row > 0) dimensions[1] = size.ws_row;
                            }
#endif
                            return dimensions;
                        }();
                        const auto terminal_columns = terminal_dimensions[0];
                        const auto terminal_rows = terminal_dimensions[1];
                        // Adapt the native shell to the real console width without
                        // resizing or otherwise changing the user's terminal window.
                        const bool split_columns = terminal_columns >= 150;
                        const auto shell_width = split_columns
                            ? std::clamp<std::size_t>(terminal_columns * 58 / 100, 92, 120)
                            : std::clamp<std::size_t>(terminal_columns, 80, 180);
                        const auto right_width = split_columns
                            ? terminal_columns - shell_width - 5 : 0;
                        const auto separator = std::string(shell_width - 4, '-');
                        const auto visible_log_lines = std::clamp<std::size_t>(
                            terminal_rows > 36 ? terminal_rows - 36 : 3, 3, 18);
                        const auto uptime = std::chrono::duration_cast<std::chrono::seconds>(
                            Clock::now() - started).count();
                        const auto system_sample = shell_telemetry.sample();
                        std::cout << "\x1b[2J\x1b[H"
                                  << "\x1b[1;36m"
                                  << "  U N I Q U E   R X   |   MONERO RANDOMX CPU MINER"
                                  << "\x1b[0m\n"
                                  << "  " << std::string(shell_width - 4, '=') << "\n";
                        line("ALGORITMO", "RandomX rx/0");
                        line("CPU", cpu_summary.value("brand", std::string("non disponibile")));
                        line("FEATURE", std::string("AES ") + (features.value("aes", false) ? "ON" : "OFF") +
                            " | SSSE3 " + (features.value("ssse3", false) ? "ON" : "OFF") +
                            " | AVX2 " + (features.value("avx2", false) ? "ON" : "OFF") +
                            " | VAES " + (features.value("vaes", false) ? "ON" : "OFF") +
                            " | BMI2 " + (features.value("bmi2", false) ? "ON" : "OFF"));
                        line("TOPOLOGIA", std::to_string(cpu_summary.value("numa_nodes", 0U)) +
                            " NUMA | L3 " + std::to_string(
                                cpu_summary.value("l3_cache_bytes", std::uint64_t{0}) / (1024ULL * 1024ULL)) +
                            " MiB");
                        line("CORE / WORKER", std::to_string(cpu_summary.value("physical_cores", 0U)) +
                            " fisici | " + std::to_string(cpu_summary.value("logical_threads", 0U)) +
                            " logici | " + std::to_string(desired_threads->load()) + " worker");
                        std::ostringstream worker_summary;
                        const auto worker_columns = std::max<std::size_t>(
                            4, std::min<std::size_t>(12, (shell_width - 30) / 20));
                        const auto visible_workers = std::min<std::size_t>(
                            desired_threads->load(),
                            std::min<std::size_t>(worker_hashrates.size(), worker_columns));
                        for (std::size_t id = 0; id < visible_workers; ++id) {
                            if (id != 0) worker_summary << " | ";
                            worker_summary << 'W' << id << ' ' << rate_text(worker_hashrates[id]);
                        }
                        line("WORKER RATE", worker_summary.str().empty() ? "in inizializzazione" : worker_summary.str());
                        std::cout << "  " + separator + "\n";
                        std::cout << "  HASHRATE  10s " << std::setw(16) << rate_text(average_rate(10))
                                  << "  60s " << std::setw(16) << rate_text(average_rate(60))
                                  << "  15m " << rate_text(average_rate(900)) << '\n';
                        line("HASHES TOTAL", std::to_string(state.hashes.load(std::memory_order_relaxed)));
                        const auto graph_begin = shell_rate_history.size() > 900
                            ? shell_rate_history.end() - 900 : shell_rate_history.begin();
                        const auto graph_count = static_cast<std::size_t>(
                            std::distance(graph_begin, shell_rate_history.end()));
                        std::array<double, 60> graph_buckets{};
                        double graph_peak = 0.0;
                        for (std::size_t bucket = 0; bucket < graph_buckets.size(); ++bucket) {
                            const auto first = graph_count * bucket / graph_buckets.size();
                            const auto last = graph_count * (bucket + 1) / graph_buckets.size();
                            if (last <= first) continue;
                            double sum = 0.0;
                            for (auto offset = first; offset < last; ++offset)
                                sum += *(graph_begin + static_cast<std::ptrdiff_t>(offset));
                            graph_buckets[bucket] = sum / static_cast<double>(last - first);
                            graph_peak = std::max(graph_peak, graph_buckets[bucket]);
                        }
                        std::cout << "  TREND 15m ";
                        static constexpr char graph_levels[] = " .:-=+*#@";
                        for (const auto rate : graph_buckets) {
                            const auto level = graph_peak > 0.0
                                ? static_cast<std::size_t>(std::clamp(rate / graph_peak, 0.0, 1.0) * 8.0)
                                : 0U;
                            std::cout << graph_levels[level];
                        }
                        std::cout << '\n'
                                  << "  " + separator + "\n";
                        const auto accepted = state.accepted.load();
                        const auto rejected = state.rejected.load();
                        const auto stale = state.stale_shares.load();
                        const auto resolved = accepted + rejected + stale;
                        const auto acceptance = resolved > 0
                            ? 100.0 * static_cast<double>(accepted) / static_cast<double>(resolved)
                            : 0.0;
                        std::cout << "  SHARES    found " << state.found.load()
                                  << " | sent " << state.submitted.load()
                                  << " | \x1b[32maccepted " << accepted
                                  << "\x1b[0m | \x1b[31mrejected " << rejected
                                  << "\x1b[0m | stale " << stale
                                  << " | pending " << state.pending.load()
                                  << " | expired " << state.expired_shares.load()
                                  << " | lost " << state.dropped_shares.load()
                                  << " | accepted " << std::fixed << std::setprecision(1)
                                  << acceptance << "%\n";
                        const auto best = state.best_snapshot();
                        const auto difficulty_text = [](double difficulty) {
                            if (!(difficulty > 0.0) || !std::isfinite(difficulty))
                                return std::string("n/d");
                            std::ostringstream value;
                            value << std::fixed << std::setprecision(2) << difficulty;
                            return value.str();
                        };
                        const auto job_difficulty = work && work->job.target != 0
                            ? static_cast<double>(std::numeric_limits<std::uint64_t>::max()) /
                              static_cast<double>(work->job.target)
                            : 0.0;
                        const auto best_share_difficulty = !best.top_difficulties.empty()
                            ? best.top_difficulties.front().difficulty : 0.0;
                        const auto estimated_effort = best_share_difficulty > 0.0
                            ? best.estimated_hashes / best_share_difficulty * 100.0
                            : 0.0;
                        const auto now_unix_seconds =
                            std::chrono::duration_cast<std::chrono::seconds>(
                                std::chrono::system_clock::now().time_since_epoch()).count();
                        std::cout << "  DIFFICULTY  JOB " << difficulty_text(job_difficulty)
                                  << " | BEST SHARE " << difficulty_text(best_share_difficulty)
                                  << " | EFFORT STIMATO "
                                  << (best_share_difficulty > 0.0
                                      ? difficulty_text(estimated_effort) + "%"
                                      : std::string("n/d")) << '\n';
                        std::cout << "  BEST DIFFICULTIES (accepted shares)  #  DIFF       EFFORT     AGE\n";
                        for (std::size_t row = 0; row < 5; ++row) {
                            std::ostringstream entries;
                            for (std::size_t column = 0; column < 2; ++column) {
                                const auto index = row + column * 5;
                                if (column != 0) entries << "   ||   ";
                                if (index >= best.top_difficulties.size()) {
                                    entries << std::setw(2) << (index + 1)
                                            << "  --          --         --";
                                    continue;
                                }
                                const auto& result = best.top_difficulties[index];
                                const auto effort = result.difficulty > 0.0
                                    ? best.estimated_hashes / result.difficulty * 100.0 : 0.0;
                                const auto age_seconds = now_unix_seconds >
                                    static_cast<std::int64_t>(result.at_unix_seconds)
                                    ? static_cast<std::uint64_t>(now_unix_seconds) -
                                      result.at_unix_seconds : 0;
                                std::ostringstream age;
                                if (age_seconds >= 3600)
                                    age << age_seconds / 3600 << "h" << (age_seconds % 3600) / 60 << "m";
                                else if (age_seconds >= 60)
                                    age << age_seconds / 60 << "m" << age_seconds % 60 << "s";
                                else
                                    age << age_seconds << "s";
                                entries << std::setw(2) << (index + 1) << "  "
                                        << std::setw(10) << difficulty_text(result.difficulty)
                                        << "  " << std::setw(8) << difficulty_text(effort) << "%"
                                        << "  " << std::setw(7) << age.str();
                            }
                            std::cout << "  " << entries.str() << '\n';
                        }
                        if (work && work->job.target != 0) {
                            std::ostringstream target_text;
                            target_text << "0x" << std::hex << std::uppercase
                                        << std::setw(16) << std::setfill('0')
                                        << work->job.target;
                            line("SHARE TARGET", target_text.str());
                        } else {
                            line("SHARE TARGET", "in attesa del job");
                        }
                        line("POOL", active_pool.host + ":" + active_pool.port +
                            (active_pool.tls ? " | TLS" : " | TCP") +
                            (active_pool.keepalive ? " | keepalive" : ""));
                        line("CONNESSIONE", work ? "connesso - job attivo" : "attesa job / riconnessione");
                        line("JOB / HEIGHT", work ? work->job.id + " / " +
                            (work->job.height ? std::to_string(*work->job.height) : "n/d")
                            : std::string("in attesa del job"));
                        line("RANDOMX SEED", work
                            ? hex_encode(work->job.seed.data(), work->job.seed.size()).substr(0, 16) + "..."
                            : std::string("n/d"));
                        line("RECONNECT", std::to_string(state.reconnects.load()) +
                            " | RTT share " +
                            (state.last_share_latency_ms.load() >= 0
                                ? std::to_string(state.last_share_latency_ms.load()) + " ms" : "n/d"));
                        line("UPTIME", std::to_string(uptime) + " secondi");
                        line("MEMORIA", std::string(full_memory ? "RandomX FAST / dataset" : "RandomX LIGHT / cache") +
                            " | dataset " + (work && work->dataset ? "ready" : "in attesa") +
                            " | huge pages " + (state.large_pages_active.load() ? "attive" : "non attive/non confermate"));
                        line("RUNTIME", "JIT HP " + std::string(jit_huge_pages ? "ON" : "OFF") +
                            " | prefetch " + std::to_string(runtime_prefetch_mode->load()) +
                            " | NUMA " + std::to_string(cpu_summary.value("numa_nodes", 0U)) +
                            " | MSR " + msr_status);
                        const auto optional_metric = [](const auto& value, const char* suffix) {
                            if (!value) return std::string("n/d");
                            std::ostringstream out;
                            out << std::fixed << std::setprecision(1) << *value << suffix;
                            return out.str();
                        };
                        const auto memory_mib = [](const auto& bytes) {
                            if (!bytes) return std::string("n/d");
                            std::ostringstream out;
                            out << std::fixed << std::setprecision(0)
                                << static_cast<double>(*bytes) / (1024.0 * 1024.0) << " MiB";
                            return out.str();
                        };
                        line("SYSTEM CPU", optional_metric(system_sample.system_cpu_percent, "%") +
                            " | miner " + optional_metric(system_sample.process_cpu_percent, "%") +
                            " | " + optional_metric(system_sample.cpu_frequency_mhz, " MHz"));
                        line("SYSTEM HW", "RAM " + memory_mib(system_sample.memory_used_bytes) +
                            " | processo " + memory_mib(system_sample.process_memory_bytes) +
                            " | temp " + optional_metric(system_sample.temperature_c, " C") +
                            " | power " + optional_metric(system_sample.package_power_watts, " W"));
                        std::cout << "  " + separator + "\n"
                                  << "  LOG EVENTI  [logs up/down/tail]  "
                                  << (log_scroll == 0 ? "LIVE" : "SCROLL -" + std::to_string(log_scroll))
                                  << "\n";
                        const auto entries = logging::recent_entries(2048);
                        log_scroll = std::min(log_scroll, entries.size());
                        const auto log_end = entries.size() - log_scroll;
                        const auto log_begin = log_end > visible_log_lines
                            ? log_end - visible_log_lines : 0;
                        for (std::size_t index = log_begin; index < log_end; ++index) {
                            const auto& entry = entries[index];
                            std::string message = entry.message;
                            const auto max_message = shell_width > 70 ? shell_width - 70 : 10;
                            if (message.size() > max_message) message.resize(max_message);
                            const char* color = entry.level == "ERROR" || entry.level == "FATAL"
                                ? "\x1b[31;1m" : entry.level == "WARN" ? "\x1b[33;1m"
                                : entry.level == "INFO" ? "\x1b[36m" : "\x1b[0m";
                            std::cout << "  " << color << '[' << entry.timestamp << "] "
                                      << std::left << std::setw(5) << entry.level
                                      << ' ' << std::setw(7) << entry.category
                                      << "\x1b[0m " << message << '\n';
                        }
                        if (entries.empty()) std::cout << "  (nessun evento registrato)\n";
                        std::cout << "  " + separator << "\n"
                                  << "  LOG SCROLL: [ logs up ] piu' vecchi | [ logs down ] recenti | [ logs tail ] live\n"
                                  << "  COMANDO > " << command_line;
                        std::cout.rdbuf(original_output);
                        const auto left_text = left_screen.str();
                        if (!split_columns) {
                            std::cout << left_text << std::flush;
                            return;
                        }
                        std::vector<std::string> right_lines;
                        right_lines.emplace_back("HASHVAULT API v3");
                        right_lines.emplace_back(std::string(right_width, '='));
                        hashvault::Snapshot api_data;
                        {
                            std::lock_guard lock(hashvault_mutex);
                            api_data = hashvault_snapshot;
                        }
                        const auto number = [](const nlohmann::json& object,
                                               const char* key) -> std::string {
                            if (!object.is_object() || !object.contains(key) ||
                                !object[key].is_number()) return "n/d";
                            std::ostringstream out;
                            out << std::fixed << std::setprecision(2)
                                << object[key].get<double>();
                            return out.str();
                        };
                        const auto object_at = [](const nlohmann::json& object,
                                                  const char* key) -> nlohmann::json {
                            return object.is_object() && object.contains(key) &&
                                object[key].is_object() ? object[key] : nlohmann::json::object();
                        };
                        const auto collective = object_at(object_at(api_data.pool, "pool_statistics"), "collective");
                        const auto solo_stats = object_at(object_at(api_data.pool, "pool_statistics"), "solo");
                        const auto network = object_at(api_data.pool, "network_statistics");
                        const auto market = object_at(api_data.pool, "market");
                        const auto config_api = object_at(api_data.pool, "config");
                        right_lines.emplace_back(api_data.pool_ready ? "POOL: ONLINE" :
                            "POOL: " + (api_data.pool_error.empty() ? std::string("attesa dati") : api_data.pool_error));
                        right_lines.emplace_back("NETWORK / MONERO");
                        right_lines.emplace_back("Height: " + number(network, "height"));
                        right_lines.emplace_back("Difficulty: " + number(network, "difficulty"));
                        right_lines.emplace_back("Block reward: " + number(network, "value") +
                            " atomic");
                        right_lines.emplace_back(" ");
                        right_lines.emplace_back("POOL PPLNS");
                        right_lines.emplace_back("Hashrate: " + number(collective, "hashRate") + " H/s");
                        right_lines.emplace_back("Miners: " + number(collective, "miners"));
                        right_lines.emplace_back("Effort: " + number(collective, "currentEffort") + "%");
                        right_lines.emplace_back("Shares: " + number(collective, "validShares") +
                            " / invalid " + number(collective, "invalidShares"));
                        right_lines.emplace_back(" ");
                        right_lines.emplace_back("POOL SOLO");
                        right_lines.emplace_back("Hashrate: " + number(solo_stats, "hashRate") + " H/s");
                        right_lines.emplace_back("Miners: " + number(solo_stats, "miners"));
                        right_lines.emplace_back("Effort: " + number(solo_stats, "currentEffort") + "%");
                        right_lines.emplace_back(" ");
                        right_lines.emplace_back("WALLET / " + (hashvault_wallet.empty()
                            ? std::string("non configurato")
                            : hashvault_wallet.substr(0, 12) + "..."));
                        right_lines.emplace_back(api_data.wallet_ready ? "Wallet API: ONLINE" :
                            "Wallet API: " + (api_data.wallet_error.empty() ?
                                std::string("in attesa") : api_data.wallet_error));
                        if (api_data.wallet_ready && api_data.wallet.is_object()) {
                            right_lines.emplace_back("Campi wallet:");
                            std::size_t shown = 0;
                            for (auto it = api_data.wallet.begin();
                                 it != api_data.wallet.end() && shown < 5; ++it, ++shown) {
                                right_lines.emplace_back(it.key() + ": " +
                                    (it.value().is_primitive() ? it.value().dump() : "[dati]"));
                            }
                        }
                        right_lines.emplace_back(" ");
                        right_lines.emplace_back("PROFIT CALCULATOR / STIMA");
                        right_lines.emplace_back("Miner: " + rate_text(shell_hashrate));
                        const double difficulty = network.value("difficulty", 0.0);
                        const double reward_atomic = network.value("value", 0.0);
                        const double divisor = config_api.value("sigDivisor", 1.0e12);
                        const double block_seconds = config_api.value("coinDiffTarget", 120.0);
                        const double fee_percent = config_api.value("pplns_fee", 0.0);
                        const double reward_xmr = divisor > 0.0 ? reward_atomic / divisor : 0.0;
                        const double daily_xmr = difficulty > 0.0
                            ? shell_hashrate * 86400.0 / difficulty * reward_xmr *
                              std::max(0.0, 1.0 - fee_percent / 100.0) : 0.0;
                        right_lines.emplace_back("Effort corrente: " + number(collective, "currentEffort") + "%");
                        right_lines.emplace_back("Pool fee: " + std::to_string(fee_percent) + "%");
                        right_lines.emplace_back("Expected XMR/day: " +
                            (difficulty > 0.0 ? std::to_string(daily_xmr) : std::string("n/d")));
                        right_lines.emplace_back("Solo expected block:");
                        right_lines.emplace_back(shell_hashrate > 0.0
                            ? std::to_string(difficulty / shell_hashrate / 3600.0) + " hours"
                            : "n/d");
                        right_lines.emplace_back("Revenue / day:");
                        right_lines.emplace_back("BTC: " + (difficulty > 0.0
                            ? std::to_string(daily_xmr * market.value("price_btc", 0.0)) : "n/d"));
                        right_lines.emplace_back("USD: " + (difficulty > 0.0
                            ? std::to_string(daily_xmr * market.value("price_usd", 0.0)) : "n/d"));
                        right_lines.emplace_back("EUR: " + (difficulty > 0.0
                            ? std::to_string(daily_xmr * market.value("price_eur", 0.0)) : "n/d"));
                        right_lines.emplace_back("RUB: " + (difficulty > 0.0
                            ? std::to_string(daily_xmr * market.value("price_rub", 0.0)) : "n/d"));
                        right_lines.emplace_back(" ");
                        right_lines.emplace_back("MARKET");
                        right_lines.emplace_back("XMR/BTC: " + number(market, "price_btc"));
                        right_lines.emplace_back("XMR/USD: " + number(market, "price_usd"));
                        right_lines.emplace_back("XMR/EUR: " + number(market, "price_eur"));
                        right_lines.emplace_back("XMR/RUB: " + number(market, "price_rub"));
                        right_lines.emplace_back(" ");
                        right_lines.emplace_back("Aggiornamento API: 60s");
                        right_lines.emplace_back("Stime indicative, non garantite");
                        std::istringstream left_rows(left_text);
                        std::string left_line;
                        std::size_t row = 0;
                        while (std::getline(left_rows, left_line)) {
                            const auto visible_left = left_line.size() > shell_width
                                ? left_line.substr(0, shell_width) : left_line;
                            std::cout << visible_left;
                            if (row < right_lines.size()) {
                                const auto padding = shell_width > visible_left.size()
                                    ? shell_width - visible_left.size() : 0;
                                std::cout << std::string(padding, ' ') << "  |  "
                                          << right_lines[row].substr(0, right_width);
                            }
                            std::cout << '\\n';
                            ++row;
                        }
                        while (row < right_lines.size()) {
                            std::cout << std::string(shell_width, ' ') << "  |  "
                                      << right_lines[row].substr(0, right_width) << '\\n';
                            ++row;
                        }
                        std::cout << std::flush;
                    };
                    const auto process_command = [&](const std::string& line) {
                        std::istringstream input(line);
                        std::string command;
                        std::string value;
                        input >> command;
                        if (command == "help") {
                            std::cout << "Comandi shell UNIQUE RX:\n"
                                      << "  status          stato, hashrate, pool e uptime\n"
                                      << "  shares          accepted/rejected/stale/pending\n"
                                      << "  best            difficoltà migliori ed effort stimato\n"
                                      << "  workers         hashrate e contatori per worker\n"
                                      << "  graph           grafico hashrate ASCII ultimi 15 minuti\n"
                                      << "  cpu             CPU, core, thread e feature rilevate\n"
                                      << "  logs [N]        mostra ultimi N log (default 30, max 200)\n"
                                      << "  logs up/down    scorri il pannello log (vecchi/recenti)\n"
                                      << "  logs tail       torna agli eventi in tempo reale\n"
                                      << "  pool            endpoint e opzioni pool (segreti nascosti)\n"
                                      << "  memory          profilo RandomX, pagine e prefetch\n"
                                      << "  config          configurazione XMRig (credenziali oscurate)\n"
                                      << "  threads N       imposta worker in tempo reale (1-256)\n"
                                      << "  threads auto    ripristina il numero iniziale configurato\n"
                                      << "  set cpu.rx N|auto regola i worker RandomX in tempo reale\n"
                                      << "  set prefetch-mode N (0-3) modifica il prefetch JIT live\n"
                                      << "  set cpu.priority -1..5 aggiorna la priorità dei worker\n"
                                      << "  set cpu.yield on|off abilita/disabilita yield XMRig\n"
                                      << "  set huge-pages on|off (richiede riavvio)\n"
                                      << "  set randomx.mode fast|light (richiede riavvio)\n"
                                      << "  help            mostra questo aiuto\n"
                                      << "  quit             arresta ordinatamente il miner\n";
                        } else if (command == "threads" && (input >> value)) {
                            if (value == "auto") {
                                desired_threads->store(automatic_threads, std::memory_order_release);
                                std::cout << "Ripristino numero iniziale: "
                                          << automatic_threads << " thread\n";
                            } else {
                                try {
                                    std::size_t used = 0;
                                    const auto requested = std::stoul(value, &used, 10);
                                    if (used != value.size() || requested == 0 || requested > 256) {
                                        std::cout << "Valore non valido. Usa un numero da 1 a 256.\n";
                                        return;
                                    }
                                    desired_threads->store(static_cast<std::uint32_t>(requested),
                                                            std::memory_order_release);
                                    std::cout << "Richiesti " << requested
                                              << " thread; applicazione in corso...\n";
                                } catch (...) {
                                    std::cout << "Valore non valido. Usa threads N oppure threads auto.\n";
                                }
                            }
                        } else if (command == "set") {
                            std::string setting;
                            std::string setting_value;
                            if (!(input >> setting >> setting_value)) {
                                std::cout << "Uso: set prefetch-mode N | set huge-pages on|off | set randomx.mode fast|light\n";
                            } else if (setting == "cpu.rx" || setting == "cpu.threads") {
                                if (setting_value == "auto") {
                                    desired_threads->store(automatic_threads, std::memory_order_release);
                                    logging::info("cpu", "runtime RandomX thread profile restored to automatic startup count " +
                                        std::to_string(automatic_threads));
                                    std::cout << "Thread RandomX ripristinati: " << automatic_threads << '\n';
                                } else try {
                                    std::size_t used = 0;
                                    const auto requested = std::stoul(setting_value, &used, 10);
                                    if (used != setting_value.size() || requested == 0 || requested > 256) {
                                        std::cout << setting << " deve essere compreso tra 1 e 256, oppure auto\n";
                                    } else {
                                        desired_threads->store(static_cast<std::uint32_t>(requested),
                                                                std::memory_order_release);
                                        logging::info("cpu", "runtime " + setting + " set to " + std::to_string(requested));
                                    }
                                } catch (...) {
                                    std::cout << "Valore " << setting << " non valido\n";
                                }
                            } else if (setting == "prefetch-mode" ||
                                       setting == "randomx.scratchpad_prefetch_mode") {
                                try {
                                    std::size_t used = 0;
                                    const int mode = std::stoi(setting_value, &used, 10);
                                    if (used != setting_value.size() || mode < 0 || mode > 3) {
                                        std::cout << "prefetch-mode deve essere 0, 1, 2 o 3\n";
                                    } else {
                                        rxm_set_prefetch_mode(mode);
                                        runtime_prefetch_mode->store(mode, std::memory_order_release);
                                        state.vm_config_generation.fetch_add(1, std::memory_order_acq_rel);
                                        logging::info("cpu", "runtime prefetch mode changed to " + std::to_string(mode) +
                                            "; RandomX worker VMs will rebuild with the new JIT setting");
                                        std::cout << "Prefetch mode applicato: " << mode << '\n';
                                    }
                                } catch (...) {
                                    std::cout << "Valore prefetch-mode non valido\n";
                                }
                            } else if (setting == "cpu.priority") {
                                try {
                                    std::size_t used = 0;
                                    const int priority = std::stoi(setting_value, &used, 10);
                                    if (used != setting_value.size() || priority < -1 || priority > 5) {
                                        std::cout << "cpu.priority deve essere compreso tra -1 e 5\n";
                                    } else {
                                        state.cpu_priority.store(priority, std::memory_order_release);
                                        logging::info("cpu", "runtime CPU priority changed to " + std::to_string(priority));
                                        std::cout << "Priorità richiesta; applicazione entro il prossimo blocco nonce per ogni worker.\n";
                                    }
                                } catch (...) {
                                    std::cout << "Valore cpu.priority non valido\n";
                                }
                            } else if (setting == "cpu.yield") {
                                if (setting_value == "on" || setting_value == "true" || setting_value == "1") {
                                    state.cpu_yield.store(true, std::memory_order_release);
                                    logging::info("cpu", "cpu.yield enabled at runtime");
                                } else if (setting_value == "off" || setting_value == "false" || setting_value == "0") {
                                    state.cpu_yield.store(false, std::memory_order_release);
                                    logging::info("cpu", "cpu.yield disabled at runtime");
                                } else {
                                    std::cout << "cpu.yield accetta on/off\n";
                                }
                            } else if (setting == "huge-pages" || setting == "cpu.huge-pages" ||
                                       setting == "randomx.mode" || setting == "cpu.hw-aes" ||
                                       setting == "cpu.huge-pages-jit" || setting == "cpu.memory-pool" ||
                                       setting == "cpu.asm" || setting == "randomx.numa" ||
                                       setting == "randomx.1gb-pages" || setting == "randomx.init" ||
                                       setting == "randomx.init-avx2" || setting == "randomx.rdmsr" ||
                                       setting == "randomx.wrmsr" || setting == "pools") {
                                std::cout << setting << " è un'impostazione di inizializzazione RandomX: modifica config.json e riavvia il miner per applicarla.\n";
                            } else {
                                std::cout << "Impostazione non supportata dalla shell: " << setting << '\n';
                            }
                        } else if (command == "status") {
                            const auto hashes = state.hashes.load(std::memory_order_relaxed);
                            const auto average_rate = [&](std::size_t window) {
                                const auto count = std::min(window, shell_rate_history.size());
                                if (count == 0) return 0.0;
                                double total = 0.0;
                                auto it = shell_rate_history.end() - static_cast<std::ptrdiff_t>(count);
                                for (; it != shell_rate_history.end(); ++it) total += *it;
                                return total / static_cast<double>(count);
                            };
                            std::cout << "Hashrate: " << std::fixed << std::setprecision(2)
                                      << shell_hashrate << " H/s | 10s " << average_rate(10)
                                      << " | 60s " << average_rate(60)
                                      << " | 15m " << average_rate(900)
                                      << " | total hashes: " << hashes
                                      << " | workers: " << desired_threads->load()
                                      << " | pool: " << pools[std::min(active_pool_index->load(std::memory_order_relaxed), pools.size() - 1)].host
                                      << ':' << pools[std::min(active_pool_index->load(std::memory_order_relaxed), pools.size() - 1)].port
                                      << " | reconnects: " << state.reconnects.load()
                                      << " | share latency: "
                                      << (state.last_share_latency_ms.load(std::memory_order_relaxed) >= 0
                                          ? std::to_string(state.last_share_latency_ms.load(std::memory_order_relaxed)) + " ms"
                                          : std::string("n/a"))
                                      << " | uptime: " << std::chrono::duration_cast<std::chrono::seconds>(
                                             Clock::now() - started).count() << " s\n";
                        } else if (command == "shares") {
                            std::cout << "Found " << state.found.load()
                                      << " | submitted " << state.submitted.load()
                                      << " | accepted " << state.accepted.load()
                                      << " | rejected " << state.rejected.load()
                                      << " | stale " << state.stale_shares.load()
                                      << " | pending " << state.pending.load()
                                      << " | expired " << state.expired_shares.load()
                                      << " | dropped " << state.dropped_shares.load() << '\n';
                        } else if (command == "best") {
                            const auto best = state.best_snapshot();
                            std::cout << "Hash stimati: " << std::fixed << std::setprecision(2)
                                      << best.estimated_hashes << '\n';
                            for (std::size_t index = 0; index < best.top_difficulties.size(); ++index) {
                                const auto& result = best.top_difficulties[index];
                                std::cout << '#' << (index + 1) << " difficulty "
                                          << result.difficulty << " | unix "
                                          << result.at_unix_seconds << '\n';
                            }
                            if (!best.top_difficulties.empty() &&
                                best.top_difficulties.front().difficulty > 0.0) {
                                std::cout << "Effort stimato: "
                                          << best.estimated_hashes /
                                             best.top_difficulties.front().difficulty * 100.0
                                          << "%\n";
                            }
                        } else if (command == "workers") {
                            for (std::uint32_t id = 0; id < desired_threads->load() &&
                                 id < worker_hashrates.size(); ++id) {
                                std::cout << "Worker #" << id << " | "
                                          << std::fixed << std::setprecision(2)
                                          << worker_hashrates[id] << " H/s | hashes "
                                          << (id < state.worker_hashes.size() && state.worker_hashes[id]
                                              ? state.worker_hashes[id]->load(std::memory_order_relaxed) : 0)
                                          << '\n';
                            }
                        } else if (command == "graph") {
                            const auto first_sample = shell_rate_history.size() > 900
                                ? shell_rate_history.end() - 900 : shell_rate_history.begin();
                            const auto sample_count = static_cast<std::size_t>(
                                std::distance(first_sample, shell_rate_history.end()));
                            std::array<double, 60> buckets{};
                            double peak = 0.0;
                            for (std::size_t bucket = 0; bucket < buckets.size(); ++bucket) {
                                const auto first = sample_count * bucket / buckets.size();
                                const auto last = sample_count * (bucket + 1) / buckets.size();
                                if (last <= first) continue;
                                double sum = 0.0;
                                for (auto offset = first; offset < last; ++offset)
                                    sum += *(first_sample + static_cast<std::ptrdiff_t>(offset));
                                buckets[bucket] = sum / static_cast<double>(last - first);
                                peak = std::max(peak, buckets[bucket]);
                            }
                            std::cout << "Hashrate ultimi 15 minuti · picco "
                                      << std::fixed << std::setprecision(2) << peak << " H/s\n";
                            for (int row = 8; row >= 1; --row) {
                                std::cout << "  | ";
                                for (const auto rate : buckets) {
                                    const auto level = peak > 0.0
                                        ? static_cast<int>(std::clamp(rate / peak, 0.0, 1.0) * 8.0)
                                        : 0;
                                    std::cout << (level >= row ? '#' : ' ');
                                }
                                std::cout << '\n';
                            }
                            std::cout << "  +------------------------------------------------------------+\n"
                                      << "    15m fa                                               adesso\n";
                        } else if (command == "cpu") {
                            std::cout << cpu_summary.dump(2) << '\n';
                        } else if (command == "logs") {
                            if (!(input >> value) || value == "tail") {
                                log_scroll = 0;
                                std::cout << "Pannello log in tempo reale.\n";
                            } else if (value == "up") {
                                log_scroll = std::min<std::size_t>(2048, log_scroll + 8);
                                std::cout << "Pannello log: eventi precedenti.\n";
                            } else if (value == "down") {
                                log_scroll = log_scroll > 8 ? log_scroll - 8 : 0;
                                std::cout << "Pannello log: eventi piu' recenti.\n";
                            } else {
                                std::size_t count = 30;
                                try { count = std::clamp<std::size_t>(std::stoul(value), 1, 200); }
                                catch (...) { count = 30; }
                                log_scroll = 0;
                                for (const auto& entry : logging::recent_entries(count)) {
                                    std::cout << '[' << entry.timestamp << "] " << entry.level
                                              << ' ' << entry.category << ' ' << entry.message << '\n';
                                }
                            }
                        } else if (command == "pool") {
                            for (std::size_t index = 0; index < pools.size(); ++index) {
                                const auto& endpoint = pools[index];
                                const auto active_index = active_pool_index->load(std::memory_order_relaxed);
                                std::cout << (index == active_index ? "[ATTIVA] " :
                                              index == 0 ? "Pool primario: " : "Pool failover: ")
                                          << endpoint.host << ':' << endpoint.port
                                          << " | TLS " << (endpoint.tls ? "on" : "off")
                                          << " | keepalive " << (endpoint.keepalive ? "on" : "off")
                                          << " | login " << (endpoint.wallet.empty() ? "non configurato" : "configurato")
                                          << '\n';
                            }
                        } else if (command == "memory") {
                            std::cout << "RandomX mode: " << (full_memory ? "fast/full dataset" : "light/cache")
                                      << " | huge pages richieste: " << (large_pages_requested ? "si" : "no")
                                      << " | allocazione large pages: "
                                      << (state.large_pages_active.load(std::memory_order_relaxed) ? "attiva" : "non attiva/non confermata")
                                      << " | JIT huge pages: " << (jit_huge_pages ? "si" : "no")
                                      << " | prefetch mode: " << runtime_prefetch_mode->load(std::memory_order_relaxed)
                                      << " | MSR: " << msr_status
                                      << "\nNota: richiesta non equivale ad allocazione riuscita; controllare i log.\n";
                        } else if (command == "config") {
                            auto safe = config;
                            if (safe.contains("pools") && safe["pools"].is_array()) {
                                for (auto& endpoint : safe["pools"]) {
                                    if (!endpoint.is_object()) continue;
                                    if (endpoint.contains("user")) endpoint["user"] = "<redacted>";
                                    if (endpoint.contains("pass")) endpoint["pass"] = "<redacted>";
                                }
                            }
                            if (safe.contains("http") && safe["http"].is_object() &&
                                safe["http"].contains("access-token")) {
                                safe["http"]["access-token"] = "<redacted>";
                            }
                            std::cout << safe.dump(2) << '\n';
                        } else if (command == "quit") {
                            std::cout << "Arresto ordinato richiesto...\n";
                            state.stop.store(true, std::memory_order_release);
                        } else if (!command.empty()) {
                            std::cout << "Comando sconosciuto. Digita help.\n";
                        }
                        std::cout << std::flush;
                    };

                    render_operator_screen();
                    while (!stop_token.stop_requested() &&
                           !state.stop.load(std::memory_order_acquire)) {
                        const auto now = Clock::now();
                        if (now - previous_sample >= std::chrono::seconds(1)) {
                            const auto hashes = state.hashes.load(std::memory_order_relaxed);
                            const auto elapsed = std::chrono::duration<double>(now - previous_sample).count();
                            if (elapsed > 0.0) {
                                shell_hashrate =
                                    static_cast<double>(hashes - previous_hashes) / elapsed;
                                for (std::size_t id = 0; id < worker_hashrates.size(); ++id) {
                                    if (id >= state.worker_hashes.size() || !state.worker_hashes[id]) continue;
                                    const auto worker_hashes =
                                        state.worker_hashes[id]->load(std::memory_order_relaxed);
                                    worker_hashrates[id] = static_cast<double>(
                                        worker_hashes - previous_worker_hashes[id]) / elapsed;
                                    previous_worker_hashes[id] = worker_hashes;
                                }
                                shell_rate_history.push_back(shell_hashrate);
                                if (shell_rate_history.size() > 900) shell_rate_history.pop_front();
                            }
                            if (command_line.empty() &&
                                (last_command_at == Clock::time_point::min() ||
                                 now - last_command_at >= std::chrono::seconds(2))) {
                                render_operator_screen();
                            }
                            previous_hashes = hashes;
                            previous_sample = now;
                        }
                        char character = 0;
#ifdef _WIN32
                        if (!_kbhit()) {
                            Sleep(50);
                            continue;
                        }
                        const int key = _getch();
                        if (key == 0 || key == 224) {
                            const int scan = _getch();
                            if (scan == 73) log_scroll = std::min<std::size_t>(2048, log_scroll + 8);
                            else if (scan == 81) log_scroll = log_scroll > 8 ? log_scroll - 8 : 0;
                            continue;
                        }
                        character = static_cast<char>(key);
                        if (key == 13) {
                            std::cout << '\n' << std::flush;
                        } else if (key == 8) {
                            std::cout << "\b \b" << std::flush;
                        } else if (key >= 32 && key <= 126) {
                            std::cout << character << std::flush;
                        }
#else
                        pollfd descriptor{};
                        descriptor.fd = STDIN_FILENO;
                        descriptor.events = POLLIN;
                        const int ready = ::poll(&descriptor, 1, 50);
                        if (ready == 0) continue;
                        if (ready < 0) continue;
                        if ((descriptor.revents & (POLLIN | POLLHUP)) == 0) continue;
                        const auto count = ::read(STDIN_FILENO, &character, 1);
                        if (count <= 0) break;
#endif
                        if (character == '\r' || character == '\n') {
                            if (!command_line.empty()) {
                                process_command(command_line);
                                last_command_at = Clock::now();
                            }
                            command_line.clear();
                        } else if (character == '\b' || character == 127) {
                            if (!command_line.empty()) command_line.pop_back();
                        } else if (command_line.empty() && character == '[') {
                            log_scroll = std::min<std::size_t>(2048, log_scroll + 8);
                        } else if (command_line.empty() && character == ']') {
                            log_scroll = log_scroll > 8 ? log_scroll - 8 : 0;
                        } else if (character >= 32 && character <= 126 &&
                                   command_line.size() < 128) {
                            command_line.push_back(character);
                        }
                    }
                });
        }

        // NyxRX metrics.rs MetricsSnapshot port: rates come from actual worker
        // counter deltas; no synthetic hashrate is generated for the dashboard.
        const auto collect_best_status = [&]() {
            const auto best = state.best_snapshot();
            nlohmann::json top = nlohmann::json::array();
            for (const auto& result : best.top_difficulties) {
                top.push_back({{"difficulty", result.difficulty},
                               {"at_unix_seconds", result.at_unix_seconds}});
            }
            const double effort = !best.top_difficulties.empty() &&
                best.top_difficulties.front().difficulty > 0.0
                ? best.estimated_hashes / best.top_difficulties.front().difficulty * 100.0
                : 0.0;
            return nlohmann::json{{"estimated_hashes", best.estimated_hashes},
                                  {"effort_percent", effort},
                                  {"top_difficulties", std::move(top)}};
        };
        const auto collect_worker_status = [&]() {
            nlohmann::json result = nlohmann::json::array();
            const auto sample_time = Clock::now();
            for (std::uint32_t worker = 0; worker < pool_threads; ++worker) {
                const auto hashes = state.worker_hashes[worker]->load(std::memory_order_relaxed);
                const auto elapsed = std::chrono::duration<double>(
                    sample_time - worker_sample_times[worker]).count();
                if (elapsed >= 0.2) {
                    worker_rates[worker] = static_cast<double>(
                        hashes - worker_previous_hashes[worker]) / elapsed;
                    worker_previous_hashes[worker] = hashes;
                    worker_sample_times[worker] = sample_time;
                }
                result.push_back({
                    {"id", worker}, {"hashes", hashes},
                    {"hashrate_hps", worker_rates[worker]},
                    {"active", worker < workers.size() && worker_rates[worker] > 0.0}
                });
            }
            return result;
        };
        logging::info("init", "==============================================================");
        logging::info("init", "                 UNIQUE-RX  |  RANDOMX MINER");
        logging::info("init", "==============================================================");
        logging::info("miner", "UniqueRX native C++ engine starting");
        logging::info("algo", "RandomX rx/0");
        logging::info("pool", pools.front().host + ":" + pools.front().port +
            (pools.front().tls ? " (TLS)" : " (TCP)") +
            (pools.size() > 1 ? " | failover endpoints: " + std::to_string(pools.size()) : ""));
        logging::info("config", std::to_string(pools.size()) +
            " enabled pool endpoint(s); wallet and password are hidden");
        logging::info("cpu", std::to_string(pool_threads) + " mining thread(s) configured");
        logging::info("memory", full_memory ? "full RandomX dataset mode" : "light RandomX cache mode");
        logging::info("msr", msr.active() ? "ACTIVE | CPU preset applied" :
            (msr_explicitly_disabled ? "DISABLED by configuration" : "INACTIVE | standard CPU registers"));
        const auto cpu_features = cpu_summary.value("features", nlohmann::json::object());
        std::string cpu_line = cpu_summary.value("brand", std::string("Unknown CPU")) +
            " | " + std::to_string(cpu_summary.value("physical_cores", 0U)) +
            " cores / " + std::to_string(cpu_summary.value("logical_threads", 0U)) +
            " threads";
        std::string enabled_features;
        for (const auto& feature : {"aes", "avx2", "bmi2", "ssse3", "vaes"}) {
            if (cpu_features.value(feature, false)) {
                if (!enabled_features.empty()) enabled_features += ", ";
                enabled_features += feature;
            }
        }
        if (!enabled_features.empty()) cpu_line += " | " + enabled_features;
        logging::info("cpu", cpu_line);
        logging::info("cpu", std::to_string(pool_threads) + " mining thread(s) selected");
        logging::info("algo", "RandomX rx/0");
        logging::info("memory", full_memory ? "full dataset mode" : "light cache mode");
        auto last_console_speed = started;
        auto last_health_print = started;
        std::string last_share_issue;
        auto last_share_issue_at = Clock::time_point::min();
        std::uint64_t suppressed_share_issues = 0;
        const auto log_share_issue = [&](const std::string& message) {
            const auto now = Clock::now();
            if (message != last_share_issue ||
                last_share_issue_at == Clock::time_point::min() ||
                now - last_share_issue_at >= std::chrono::seconds(60)) {
                std::string record = message;
                if (suppressed_share_issues != 0) {
                    record += " (" + std::to_string(suppressed_share_issues) +
                        " eventi ripetuti soppressi)";
                }
                logging::warning("cpu", record);
                last_share_issue = message;
                last_share_issue_at = now;
                suppressed_share_issues = 0;
            } else {
                ++suppressed_share_issues;
            }
        };
        const auto log_event = [&](const std::string& event,
                                   const nlohmann::json& details) {
            if (event == "CONNECTED") {
                logging::info("net", "connected to " + details.value("host", std::string()) +
                    ":" + details.value("port", std::string()));
            } else if (event == "LOGIN_ACCEPTED") {
                logging::info("net", "pool login accepted; session " +
                    details.value("session_id", std::string()) + ", job " +
                    details.value("job_id", std::string()));
            } else if (event == "NEW_JOB") {
                std::string message = "new job " + details.value("job_id", std::string());
                if (details.contains("height") && !details["height"].is_null())
                    message += ", height " + details["height"].dump();
                logging::info("net", message + ", algorithm rx/0");
            } else if (event == "SUBMIT_SENT" || event == "SUBMIT_RESPONSE") {
                // Per-share transport chatter is intentionally omitted from the
                // operator shell; accepted/rejected counters remain visible.
            } else if (event == "SHARE_ACCEPTED") {
                // Accepted shares are counted in the periodic summary; do not
                // flood the operator console with one line per valid share.
            } else if (event == "SHARE_REJECTED") {
                const auto response = details.value("response", nlohmann::json::object());
                const auto reason = response.contains("error") && !response["error"].is_null()
                    ? format_rpc_error(response["error"]) : response.dump();
                log_share_issue("share rejected: " + reason);
            } else if (event == "SHARE_STALE") {
                log_share_issue("stale share: " +
                    details.value("error", std::string("pool marked job stale")));
            } else if (event == "POOL_EXCEPTION") {
                logging::error("net", details.value("host", std::string()) + ":" +
                    details.value("port", std::string()) + " disconnected: " +
                    details.value("error", std::string("unknown error")));
            } else if (event == "UNMATCHED_RESPONSE") {
                logging::warning("net", "unmatched pool response: " + details.dump());
            }
        };
        auto last_sample_time = started;
        std::deque<std::pair<Clock::time_point, std::uint64_t>> samples;
struct PendingShare {
            Clock::time_point submitted_at;
            double actual_difficulty = 0.0;
            double target_difficulty = 0.0;
        };
        std::unordered_map<std::uint64_t, PendingShare> pending;
        std::size_t pool_index = 0;
        std::uint32_t pool_failures = 0;
        std::uint32_t retries_per_pool = 5;
        std::uint32_t retry_pause_seconds = 5;
        if (config.contains("retries") && !config["retries"].is_null()) {
            if (!config["retries"].is_number_unsigned() && !config["retries"].is_number_integer())
                throw std::runtime_error("retries must be a non-negative integer");
            const auto value = config["retries"].get<std::int64_t>();
            if (value < 0 || value > 1000) throw std::runtime_error("retries must be between 0 and 1000");
            retries_per_pool = static_cast<std::uint32_t>(value);
        }
        if (config.contains("retry-pause") && !config["retry-pause"].is_null()) {
            if (!config["retry-pause"].is_number_unsigned() && !config["retry-pause"].is_number_integer())
                throw std::runtime_error("retry-pause must be a non-negative integer");
            const auto value = config["retry-pause"].get<std::int64_t>();
            if (value < 0 || value > 3600) throw std::runtime_error("retry-pause must be between 0 and 3600 seconds");
            retry_pause_seconds = static_cast<std::uint32_t>(value);
        }
        std::chrono::seconds retry_delay{retry_pause_seconds};

        const auto install_job = [&](Job next_job) {
            const auto previous_work = state.current_work();
            const bool same_work = previous_work &&
                previous_work->job.same_work(next_job);
            if (!have_seed || next_job.seed != active_seed) {
                // NyxRX engine.rs clears the active job before building a new
                // RandomX epoch. Do the same here: workers must not continue
                // hashing the old seed while the network thread initializes a
                // potentially multi-minute dataset for the new epoch.
                state.set_work(nullptr);
                // Build the next epoch resources off to the side. If cache or
                // dataset creation fails, keep the previous seed/cache pair
                // coherent for a later pool reconnect.
                auto next_flags = flags;
                auto next_cache = create_randomx_cache(next_job.seed, next_flags);
                std::shared_ptr<randomx_dataset> next_dataset;
                logging::info("memory", "RandomX cache initialized for new seed");
                if (full_memory) {
                    logging::info("memory", "initializing full RandomX dataset");
                    std::cout << "Initializing RandomX full dataset..." << std::flush;
                    next_dataset = create_randomx_dataset(
                        next_cache, dataset_init_threads, next_flags);
                    std::cout << " ready\n";
                    logging::info("memory", "full RandomX dataset ready");
                }
                cache = std::move(next_cache);
                dataset = std::move(next_dataset);
                flags = next_flags;
                const bool dataset_large_pages_active =
                    (next_flags & RANDOMX_FLAG_LARGE_PAGES) != 0;
                state.large_pages_active.store(
                    dataset_large_pages_active, std::memory_order_relaxed);
                logging::info("memory", dataset_large_pages_active
                    ? "RandomX cache/dataset large pages allocated"
                    : (large_pages_requested
                        ? "RandomX cache/dataset using standard pages (large-page allocation unavailable)"
                        : "RandomX large pages disabled by configuration"));
                active_seed = next_job.seed;
                have_seed = true;
            }
            const auto next_generation =
                same_work ? previous_work->generation : ++generation;
            auto work = std::make_shared<MiningWork>(MiningWork{
                std::move(next_job), cache, dataset, flags, full_memory, next_generation,
            });
            state.set_work(std::move(work));
        };

        while (!state.stop.load(std::memory_order_acquire) && !g_stop_requested) {
            const auto& pool = pools[pool_index];
            state.set_work(nullptr);
            pending.clear();
            state.pending.store(0, std::memory_order_relaxed);
            state.last_share_latency_ms.store(-1, std::memory_order_relaxed);
            std::uint64_t next_id = 2;
            std::string session_id;
            bool session_nicehash = pool.nicehash;
            auto next_keepalive = Clock::time_point::max();
            try {
                StratumTransport transport;
                transport.connect_to(pool);
                log_event("CONNECTED", {{"host", pool.host}, {"port", pool.port}});
                transport.send_json(make_login_request(1, pool, user_agent));
                const auto login_deadline = Clock::now() + std::chrono::seconds(30);

                const auto record_share_response = [&](const nlohmann::json& message) {
                    const auto parsed_id = parse_rpc_response_id(message);
                    if (!parsed_id) return;
                    const std::uint64_t response_id = *parsed_id;
                    const auto pending_share = pending.find(response_id);
                    if (pending_share == pending.end()) {
                        log_event("UNMATCHED_RESPONSE", message);
                        return;
                    }
                    // NyxRX PoolStatus latency: measure real submit-to-response RTT.
                    const auto latency_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        Clock::now() - pending_share->second.submitted_at).count();
                    state.last_share_latency_ms.store(latency_ms, std::memory_order_relaxed);
                    const auto accepted_share_data = pending_share->second;
                    pending.erase(pending_share);
                    state.pending.store(pending.size(), std::memory_order_relaxed);

                    const bool has_error = rpc_response_failed(message);
                    const auto result_status = message.contains("result") &&
                        message["result"].is_object()
                        ? message["result"].value("status", std::string())
                        : std::string();
                    const bool explicit_rejection = !result_status.empty() &&
                        result_status != "OK" && result_status != "ok";
                    const bool accepted = !has_error && !explicit_rejection;
                    log_event("SUBMIT_RESPONSE", message);
                    std::string error_text = has_error
                        ? (message.contains("error") && !message["error"].is_null()
                            ? format_rpc_error(message["error"])
                            : message.value("result", nlohmann::json()).dump())
                        : std::string();
                    std::transform(error_text.begin(), error_text.end(), error_text.begin(),
                        [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
                    const bool stale = !accepted &&
                        (error_text.find("stale") != std::string::npos ||
                         error_text.find("job expired") != std::string::npos ||
                         error_text.find("job not found") != std::string::npos);
                    if (accepted) {
                        log_event("SHARE_ACCEPTED", {{"response_id", response_id}});
                        state.accepted.fetch_add(1, std::memory_order_relaxed);
                        state.record_accepted_share(accepted_share_data.actual_difficulty,
                                                    accepted_share_data.target_difficulty);
                    } else if (stale) {
                        log_event("SHARE_STALE", {{"response_id", response_id},
                                                  {"error", error_text}});
                        state.stale_shares.fetch_add(1, std::memory_order_relaxed);
                    } else {
                        log_event("SHARE_REJECTED", {{"response_id", response_id},
                                                     {"response", message}});
                        state.rejected.fetch_add(1, std::memory_order_relaxed);
                    }
                };

                const auto send_ready_shares = [&] {
                    std::deque<Share> ready;
                    const auto available_slots = kMaximumPendingShares - pending.size();
                    if (available_slots == 0) return;
                    {
                        std::lock_guard lock(state.share_mutex);
                        const auto take = std::min(available_slots, state.shares.size());
                        for (std::size_t index = 0; index < take; ++index) {
                            ready.push_back(std::move(state.shares.front()));
                            state.shares.pop_front();
                        }
                    }
                    for (auto it = ready.begin(); it != ready.end(); ++it) {
                        const auto& share = *it;
                        const auto work = state.current_work();
                        if (!work || work->generation != share.generation || session_id.empty()) {
                            state.stale_shares.fetch_add(1, std::memory_order_relaxed);
                            continue;
                        }
                        const auto submit_id = next_id++;
                        const auto nonce_bytes = encode_nonce_le(share.nonce);
                        const nlohmann::json submit_request = make_submit_request(
                            submit_id, session_id, share.job_id,
                            hex_encode(nonce_bytes.data(), nonce_bytes.size()),
                            hex_encode(share.hash.data(), share.hash.size()));
                        log_event("SUBMIT_SENT", submit_request);
                        try {
                            transport.send_json(submit_request);
                        } catch (...) {
                            // The current write may have been partially transmitted,
                            // so account it as dropped. Preserve later unsent shares;
                            // after reconnect they will be revalidated against the new job.
                            state.dropped_shares.fetch_add(1, std::memory_order_relaxed);
                            std::lock_guard lock(state.share_mutex);
                            for (auto rest = ready.end(); rest != it + 1;) {
                                --rest;
                                state.shares.push_front(std::move(*rest));
                            }
                            throw;
                        }
std::uint64_t hash_value = 0;
                        for (std::size_t byte = 0; byte < 8; ++byte) {
                            hash_value |= static_cast<std::uint64_t>(share.hash[24 + byte]) << (byte * 8);
                        }
                        const double actual_difficulty = hash_value == 0
                            ? std::numeric_limits<double>::max()
                            : static_cast<double>(std::numeric_limits<std::uint64_t>::max()) /
                              static_cast<double>(hash_value);
                        const double target_difficulty = share.target == 0 ? 0.0
                            : static_cast<double>(std::numeric_limits<std::uint64_t>::max()) /
                              static_cast<double>(share.target);
                        pending.emplace(submit_id, PendingShare{
                            Clock::now(), actual_difficulty, target_difficulty});
                        state.pending.store(pending.size(), std::memory_order_relaxed);
                        state.submitted.fetch_add(1, std::memory_order_relaxed);
                    }
                };

                while (!state.stop.load(std::memory_order_acquire) && !g_stop_requested) {
                    for (const auto& line : transport.read_lines(2)) {
                        const auto message = nlohmann::json::parse(line);
                        if (is_job_notification(message)) {
                            auto job = parse_job(message.at("params"));
                            apply_nicehash_extension(job, session_nicehash);
                            log_event("NEW_JOB", {{"job_id", job.id},
                                {"height", job.height ? nlohmann::json(*job.height)
                                                      : nlohmann::json(nullptr)}});
                            install_job(std::move(job));
                        } else if (parse_rpc_response_id(message).value_or(0) == 1) {
                            session_id = validate_login_response(message);
                            const auto& result = message.at("result");
                            session_nicehash = pool.nicehash || login_advertises_nicehash(result);
                            log_event("LOGIN_ACCEPTED", {{"session_id", session_id},
                                {"job_id", result.at("job").value("job_id", std::string())}});
                            if (pool.keepalive) {
                                next_keepalive = Clock::now() + std::chrono::seconds(60);
                            }
                            auto job = parse_job(result.at("job"));
                            apply_nicehash_extension(job, session_nicehash);
                            // The job included in the successful login response is
                            // the first active job too; expose it in the same log
                            // stream as subsequent mining.notify jobs.
                            log_event("NEW_JOB", {{"job_id", job.id},
                                {"height", job.height ? nlohmann::json(*job.height)
                                                      : nlohmann::json(nullptr)}});
                            install_job(std::move(job));
                            pool_failures = 0;
                            retry_delay = std::chrono::seconds(retry_pause_seconds);
                        } else if (message.contains("id")) {
                            record_share_response(message);
                        }
                    }
                    const auto now = Clock::now();
                    if (session_id.empty() && now >= login_deadline) {
                        throw std::runtime_error("pool login timed out");
                    }
                    if (pool.keepalive && !session_id.empty() && now >= next_keepalive) {
                        transport.send_json(make_keepalive_request(next_id++, session_id));
                        next_keepalive = now + std::chrono::seconds(
                            pool.keepalive_seconds == 0 ? 60U : pool.keepalive_seconds);
                    }
                    for (auto it = pending.begin(); it != pending.end();) {
                        if (now - it->second.submitted_at >= kShareResponseTimeout) {
                            it = pending.erase(it);
                            state.pending.store(pending.size(), std::memory_order_relaxed);
                            state.expired_shares.fetch_add(1, std::memory_order_relaxed);
                        } else {
                            ++it;
                        }
                    }

                    const auto work = state.current_work();
                    if (!work || session_id.empty()) continue;

                    const auto requested_threads = desired_threads->load(std::memory_order_acquire);
                    const auto allowed_threads = std::max(1U, detected_logical_threads());
                    const auto effective_requested = std::min(requested_threads, allowed_threads);
                    if (effective_requested != pool_threads) {
                        for (const auto& flag : worker_stop_flags) {
                            flag->store(true, std::memory_order_release);
                        }
                        for (auto& worker : workers) {
                            if (worker.joinable()) worker.join();
                        }
                        workers.clear();
                        worker_stop_flags.clear();
                        pool_threads = effective_requested;
                        worker_previous_hashes.resize(pool_threads, 0);
                        worker_rates.resize(pool_threads, 0.0);
                        worker_sample_times.resize(pool_threads, Clock::now());
                        logging::info("cpu", "live thread regulator applied: " +
                            std::to_string(pool_threads) + " worker(s)");
                        if (requested_threads > allowed_threads) {
                            logging::warning("cpu", "requested thread count limited to " +
                                std::to_string(allowed_threads) +
                                " processors available to this process");
                        }
                    }

                    if (workers.empty()) {
                        for (std::uint32_t worker = 0; worker < pool_threads; ++worker) {
                            auto worker_stop = std::make_shared<std::atomic<bool>>(false);
                            worker_stop_flags.push_back(worker_stop);
                            std::optional<std::int64_t> affinity_override;
                            if (worker < pool.cpu_affinities.size()) {
                                affinity_override = pool.cpu_affinities[worker];
                            }
                            workers.emplace_back(hash_worker, std::ref(state), worker,
                                                 pool_threads, maximum_hashes,
                                                 std::ref(*worker_stop), affinity_override);
                        }
                    }

                    send_ready_shares();
                    if (now - last_sample_time >= std::chrono::seconds(1)) {
                        const auto total_hashes = state.hashes.load(std::memory_order_relaxed);
                        samples.emplace_back(now, total_hashes);
                        while (!samples.empty() &&
                               now - samples.front().first > std::chrono::seconds(900)) {
                            samples.pop_front();
                        }
                        const auto rate_for = [&](std::chrono::seconds window) {
                            if (samples.size() < 2) return 0.0;
                            const auto threshold = now - window;
                            auto start_sample = samples.front();
                            for (const auto& sample : samples) {
                                if (sample.first <= threshold) start_sample = sample;
                            }
                            const auto duration = std::chrono::duration<double>(
                                now - start_sample.first).count();
                            return duration > 0
                                ? (total_hashes - start_sample.second) / duration : 0.0;
                        };
                        const auto uptime =
                            std::chrono::duration_cast<std::chrono::seconds>(now - started).count();
                        const auto system_sample = system_telemetry.sample();
                        const auto optional_number = [](const auto& value) -> nlohmann::json {
                            return value ? nlohmann::json(*value) : nlohmann::json(nullptr);
                        };
                        nlohmann::json status = {
                            {"state", "ATTIVO"}, {"managed", true}, {"error", nullptr},
                            {"hashrate", {{"10s", rate_for(std::chrono::seconds(10))},
                                           {"60s", rate_for(std::chrono::seconds(60))},
                                           {"15m", rate_for(std::chrono::seconds(900))}}},
                            {"shares", {{"found", state.found.load()},
                                         {"submitted", state.submitted.load()},
                                         {"accepted", state.accepted.load()},
                                         {"rejected", state.rejected.load()},
                                         {"stale", state.stale_shares.load()},
                                         {"expired", state.expired_shares.load()},
                                         {"dropped", state.dropped_shares.load()},
                                         {"pending", pending.size()},
                                         {"total", state.accepted.load() + state.rejected.load() + state.stale_shares.load()}}},
                            {"uptime_seconds", uptime},
                            // XMRMiner StratumJob.getDifficulty() port at 131d20e363e41491ece6086e9e72fcbdc5edddcb: UINT64_MAX / pool target.
                            {"algorithm", "rx/0"}, {"job_id", work->job.id},
                            {"job_difficulty", work->job.target == 0 ? nlohmann::json(nullptr) : nlohmann::json(static_cast<double>(std::numeric_limits<std::uint64_t>::max()) / static_cast<double>(work->job.target))},
                            {"height", work->job.height
                                ? nlohmann::json(*work->job.height)
                                : nlohmann::json(nullptr)},
                            {"threads", pool_threads}, {"cpu_priority", state.cpu_priority.load(std::memory_order_relaxed)}, {"cpu", cpu_summary},
                            {"workers", collect_worker_status()},
                            {"best", collect_best_status()},
                            {"msr_active", msr.active()}, {"msr_status", msr_status}, {"full_memory", full_memory},
                            {"large_pages_requested", large_pages_requested},
                            {"large_pages_active", state.large_pages_active.load(std::memory_order_relaxed)},
                            {"system", {
                                {"cpu_percent", optional_number(system_sample.system_cpu_percent)},
                                {"process_cpu_percent", optional_number(system_sample.process_cpu_percent)},
                                {"cpu_frequency_mhz", optional_number(system_sample.cpu_frequency_mhz)},
                                {"memory_used_bytes", optional_number(system_sample.memory_used_bytes)},
                                {"process_memory_bytes", optional_number(system_sample.process_memory_bytes)},
                                {"temperature_c", optional_number(system_sample.temperature_c)},
                                {"package_power_watts", optional_number(system_sample.package_power_watts)},
                                {"on_battery", system_sample.on_battery ? nlohmann::json(*system_sample.on_battery) : nlohmann::json(nullptr)}
                            }},
                            {"pool", pool.host + ":" + pool.port}, {"connected", true},
                            {"pool_latency_ms", state.last_share_latency_ms.load(std::memory_order_relaxed) >= 0 ? nlohmann::json(state.last_share_latency_ms.load(std::memory_order_relaxed)) : nlohmann::json(nullptr)},
                            {"reconnects", state.reconnects.load(std::memory_order_relaxed)},
                        };
                        status["timestamp_unix_ms"] =
                            std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::system_clock::now().time_since_epoch()).count();
                        status["pid"] = current_process_id();
                        (void)write_status_snapshot(status_path, status);
                        if (machine_status_channel_required()) {
                            std::cout << "UNIQUE_RX_STATUS " << status.dump() << '\n' << std::flush;
                        }
                        if (print_interval > 0 &&
                            now - last_console_speed >= std::chrono::seconds(print_interval)) {
                            const auto speed_text = [](double rate) {
                                const char* units[] = {"H/s", "kH/s", "MH/s", "GH/s"};
                                std::size_t unit = 0;
                                while (rate >= 1000.0 && unit < 3) {
                                    rate /= 1000.0;
                                    ++unit;
                                }
                                std::ostringstream value;
                                value.setf(std::ios::fixed);
                                value.precision(2);
                                value << rate << ' ' << units[unit];
                                return value.str();
                            };
                            logging::info("speed", "10s " + speed_text(rate_for(std::chrono::seconds(10))) +
                                " | 60s " + speed_text(rate_for(std::chrono::seconds(60))) +
                                " | 15m " + speed_text(rate_for(std::chrono::seconds(900))));
                            logging::info("shares", "found " + std::to_string(state.found.load()) +
                                " | accepted " + std::to_string(state.accepted.load()) +
                                " | rejected " + std::to_string(state.rejected.load()) +
                                " | stale " + std::to_string(state.stale_shares.load()) +
                                " | pending " + std::to_string(pending.size()) +
                                " | expired " + std::to_string(state.expired_shares.load()) +
                                " | dropped " + std::to_string(state.dropped_shares.load()));
                            std::ostringstream activity;
                            activity << "uptime " << uptime << "s | height ";
                            if (work->job.height) activity << *work->job.height;
                            else activity << "n/a";
                            activity << " | job " << work->job.id;
                            logging::info("status", activity.str());
                            last_console_speed = now;
                        }
                        if (health_interval > 0 &&
                            now - last_health_print >= std::chrono::seconds(health_interval)) {
                            std::ostringstream health;
                            health << "CPU process "
                                   << (system_sample.process_cpu_percent
                                       ? std::to_string(*system_sample.process_cpu_percent) + "%"
                                       : std::string("n/a"))
                                   << " | system "
                                   << (system_sample.system_cpu_percent
                                       ? std::to_string(*system_sample.system_cpu_percent) + "%"
                                       : std::string("n/a"))
                                   << " | frequency "
                                   << (system_sample.cpu_frequency_mhz
                                       ? std::to_string(*system_sample.cpu_frequency_mhz) + " MHz"
                                       : std::string("n/a"))
                                   << " | memory "
                                   << (system_sample.process_memory_bytes
                                       ? std::to_string(*system_sample.process_memory_bytes / (1024 * 1024)) + " MiB"
                                       : std::string("n/a"))
                                   << " | temperature "
                                   << (system_sample.temperature_c
                                       ? std::to_string(*system_sample.temperature_c) + " C"
                                       : std::string("n/a"))
                                   << " | power "
                                   << (system_sample.package_power_watts
                                       ? std::to_string(*system_sample.package_power_watts) + " W"
                                       : std::string("n/a"));
                            logging::info("health", health.str());
                            last_health_print = now;
                        }
                        last_sample_time = now;
                    }
                }

                if (state.stop.load(std::memory_order_acquire) || g_stop_requested) {
                    // Stop and join every worker before draining the queue: a worker
                    // may still be completing its final pipelined hash/share.
                    state.stop.store(true, std::memory_order_release);
                    for (auto& worker : workers) {
                        if (worker.joinable()) worker.join();
                    }

                    const auto queued_shares = [&] {
                        std::lock_guard lock(state.share_mutex);
                        return !state.shares.empty();
                    };
                    const auto drain_deadline = Clock::now() + std::chrono::seconds(2);
                    while (Clock::now() < drain_deadline) {
                        send_ready_shares();
                        if (pending.empty() && !queued_shares()) break;
                        for (const auto& line : transport.read_lines(10)) {
                            record_share_response(nlohmann::json::parse(line));
                        }
                    }
                    if (!pending.empty()) {
                        state.expired_shares.fetch_add(pending.size(), std::memory_order_relaxed);
                        pending.clear();
                        state.pending.store(0, std::memory_order_relaxed);
                    }
                    {
                        std::lock_guard lock(state.share_mutex);
                        if (!state.shares.empty()) {
                            state.dropped_shares.fetch_add(state.shares.size(),
                                                           std::memory_order_relaxed);
                            state.shares.clear();
                        }
                    }
                    break;
                }
            } catch (const std::exception& error) {
                if (state.stop.load(std::memory_order_acquire) || g_stop_requested) break;
                state.reconnects.fetch_add(1, std::memory_order_relaxed);
                log_event("POOL_EXCEPTION", {{"host", pool.host}, {"port", pool.port},
                                             {"error", error.what()}});
                state.set_work(nullptr);
                if (!pending.empty()) {
                    state.expired_shares.fetch_add(pending.size(), std::memory_order_relaxed);
                    pending.clear();
                    state.pending.store(0, std::memory_order_relaxed);
                }
                // A share queued for the previous authenticated pool session must
                // never leak into a different session after failover/reconnect,
                // even if the replacement pool happens to reuse the same job id.
                {
                    std::lock_guard lock(state.share_mutex);
                    state.dropped_shares.fetch_add(state.shares.size(),
                                                   std::memory_order_relaxed);
                    state.shares.clear();
                }
                const auto reconnect_uptime =
                    std::chrono::duration_cast<std::chrono::seconds>(Clock::now() - started).count();
                const auto reconnect_system = system_telemetry.sample();
                const auto reconnect_optional = [](const auto& value) -> nlohmann::json {
                    return value ? nlohmann::json(*value) : nlohmann::json(nullptr);
                };
                nlohmann::json reconnect_status = nlohmann::json::object();
                reconnect_status["state"] = "RICONNESSIONE";
                reconnect_status["managed"] = true;
                reconnect_status["error"] = error.what();
                reconnect_status["hashrate"] = nlohmann::json::object({
                    {"10s", nullptr}, {"60s", nullptr}, {"15m", nullptr}
                });
                nlohmann::json reconnect_shares = nlohmann::json::object();
                reconnect_shares["found"] = state.found.load();
                reconnect_shares["submitted"] = state.submitted.load();
                reconnect_shares["accepted"] = state.accepted.load();
                reconnect_shares["rejected"] = state.rejected.load();
                reconnect_shares["stale"] = state.stale_shares.load();
                reconnect_shares["expired"] = state.expired_shares.load();
                reconnect_shares["dropped"] = state.dropped_shares.load();
                reconnect_shares["pending"] = pending.size();
                reconnect_shares["total"] = state.accepted.load() + state.rejected.load() +
                                            state.stale_shares.load();
                reconnect_status["shares"] = std::move(reconnect_shares);
                reconnect_status["uptime_seconds"] = reconnect_uptime;
                reconnect_status["algorithm"] = "rx/0";
                reconnect_status["job_id"] = nullptr;
                reconnect_status["height"] = nullptr;
                reconnect_status["job_difficulty"] = nullptr;
                reconnect_status["threads"] = pool_threads;
                reconnect_status["cpu_priority"] = state.cpu_priority.load(std::memory_order_relaxed);
                reconnect_status["cpu"] = cpu_summary;
                reconnect_status["workers"] = collect_worker_status();
                reconnect_status["best"] = collect_best_status();
                reconnect_status["msr_active"] = msr.active();
                reconnect_status["msr_status"] = msr_status;
                reconnect_status["full_memory"] = full_memory;
                reconnect_status["large_pages_requested"] = large_pages_requested;
                reconnect_status["large_pages_active"] =
                    state.large_pages_active.load(std::memory_order_relaxed);
                nlohmann::json reconnect_system_json = nlohmann::json::object();
                reconnect_system_json["cpu_percent"] =
                    reconnect_optional(reconnect_system.system_cpu_percent);
                reconnect_system_json["process_cpu_percent"] =
                    reconnect_optional(reconnect_system.process_cpu_percent);
                reconnect_system_json["cpu_frequency_mhz"] =
                    reconnect_optional(reconnect_system.cpu_frequency_mhz);
                reconnect_system_json["memory_used_bytes"] =
                    reconnect_optional(reconnect_system.memory_used_bytes);
                reconnect_system_json["process_memory_bytes"] =
                    reconnect_optional(reconnect_system.process_memory_bytes);
                reconnect_system_json["temperature_c"] =
                    reconnect_optional(reconnect_system.temperature_c);
                reconnect_system_json["package_power_watts"] =
                    reconnect_optional(reconnect_system.package_power_watts);
                reconnect_system_json["on_battery"] =
                    reconnect_system.on_battery.has_value()
                        ? nlohmann::json(reconnect_system.on_battery.value())
                        : nlohmann::json(nullptr);
                reconnect_status["system"] = std::move(reconnect_system_json);
                reconnect_status["pool"] = pool.host + ":" + pool.port;
                reconnect_status["connected"] = false;
                const auto reconnect_latency =
                    state.last_share_latency_ms.load(std::memory_order_relaxed);
                reconnect_status["pool_latency_ms"] =
                    reconnect_latency >= 0 ? nlohmann::json(reconnect_latency)
                                           : nlohmann::json(nullptr);
                reconnect_status["reconnects"] =
                    state.reconnects.load(std::memory_order_relaxed);
                reconnect_status["timestamp_unix_ms"] =
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::system_clock::now().time_since_epoch()).count();
                reconnect_status["pid"] = current_process_id();
                (void)write_status_snapshot(status_path, reconnect_status);
                if (machine_status_channel_required()) {
                    std::cout << "UNIQUE_RX_STATUS " << reconnect_status.dump() << '\n' << std::flush;
                }
                logging::warning("net", "switching pool; retrying in " +
                    std::to_string(retry_delay.count()) + "s");
                ++pool_failures;
                if (retries_per_pool == 0 || pool_failures >= retries_per_pool) {
                    pool_index = (pool_index + 1) % pools.size();
                    active_pool_index->store(pool_index, std::memory_order_release);
                    pool_failures = 0;
                    logging::warning("net", "retry limit reached; switching to next configured pool");
                }
                auto remaining_delay =
                    std::chrono::duration_cast<std::chrono::milliseconds>(retry_delay);
                auto next_reconnect_snapshot = Clock::now();
                while (remaining_delay > std::chrono::milliseconds::zero() &&
                       !g_stop_requested && !state.stop.load(std::memory_order_acquire)) {
                    if (state.stop.load(std::memory_order_acquire)) break;
                    if (Clock::now() >= next_reconnect_snapshot) {
                        reconnect_status["timestamp_unix_ms"] =
                            std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::system_clock::now().time_since_epoch()).count();
                        reconnect_status["pid"] = current_process_id();
                        (void)write_status_snapshot(status_path, reconnect_status);
                        next_reconnect_snapshot = Clock::now() + std::chrono::seconds(1);
                    }
                    const auto step = std::min(
                        remaining_delay, std::chrono::milliseconds(100));
                    std::this_thread::sleep_for(step);
                    remaining_delay -= step;
                }
                retry_delay = std::min(retry_delay * 2, std::chrono::seconds(30));
            }
        }

        state.stop.store(true, std::memory_order_release);
        for (auto& worker : workers) if (worker.joinable()) worker.join();
        logging::info("miner", "mining workers stopped; final MSR restoration follows");
        {
            std::lock_guard lock(state.error_mutex);
            if (!state.worker_error.empty()) throw std::runtime_error(state.worker_error);
        }
        return 0;
    } catch (const std::exception& error) {
        logging::error("miner", std::string("mining error: ") + error.what());
        return 1;
    } catch (...) {
        logging::error("miner", "mining error: unknown exception");
        return 1;
    }
}
} // namespace unique_rx