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
            config["cpu"]["threads"] = thread_override;
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
            ? randomx_config["full-memory"].get<bool>() : select_auto_memory_mode();
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
        }
        const auto wrmsr = randomx_config.value("wrmsr", nlohmann::json());
        // Imported XMRig configurations may contain a boolean, a preset index,
        // or an explicit register list. Only the boolean true can select the
        // native CPU-specific preset. Numeric/list forms are accepted as
        // configuration data but are never written by this native backend.
        const bool msr_enabled = wrmsr.is_boolean() && wrmsr.get<bool>();
        const bool msr_custom_requested =
            wrmsr.is_array() || wrmsr.is_number_integer();
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
                flags &= ~RANDOMX_FLAG_HARD_AES;
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
            logging::info("config", "cpu.asm accepted; RandomX upstream dispatch selects the supported native code path automatically");
        }
        bool cpu_yield = true; // XMRig CpuConfig default.
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
        auto runtime_prefetch_mode = std::make_shared<std::atomic<int>>(prefetch_mode);
        const auto cpu_summary = cpu_summary_json();
        // XMRig-compatible persistent scratchpad pool: false disables it,
        // true sizes it for the active workers, and a number sets capacity.
        std::size_t memory_pool_capacity = 0;
        if (cpu_config.contains("memory-pool") && !cpu_config["memory-pool"].is_null()) {
            const auto& memory_pool = cpu_config["memory-pool"];
            const auto automatic_memory_pool_capacity = [&] {
                std::uint64_t l3_bytes = 0;
                if (cpu_summary.contains("l3_cache_bytes") &&
                    cpu_summary["l3_cache_bytes"].is_number()) {
                    l3_bytes = cpu_summary["l3_cache_bytes"].get<std::uint64_t>();
                }
                const auto l3_pages = static_cast<std::size_t>(l3_bytes / (2ULL * 1024ULL * 1024ULL));
                return std::max<std::size_t>(pool_threads, l3_pages);
            };
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
                [&state, desired_threads, automatic_threads, runtime_prefetch_mode, &cpu_summary, &config, &pools, &msr_status, full_memory, large_pages_requested, jit_huge_pages, started](std::stop_token stop_token) {
                    std::string command_line;
                    std::uint64_t previous_hashes = state.hashes.load(std::memory_order_relaxed);
                    std::array<std::uint64_t, 256> previous_worker_hashes{};
                    std::array<double, 256> worker_hashrates{};
                    std::deque<double> shell_rate_history;
                    auto previous_sample = Clock::now();
                    double shell_hashrate = 0.0;
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
                                      << "  graph           grafico ASCII hashrate ultimi 60 secondi\n"
                                      << "  cpu             CPU, core, thread e feature rilevate\n"
                                      << "  logs [N]        ultimi N log (default 30, max 200)\n"
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
                            const auto first_sample = shell_rate_history.size() > 60
                                ? shell_rate_history.end() - 60 : shell_rate_history.begin();
                            const double peak = first_sample == shell_rate_history.end()
                                ? 0.0 : *std::max_element(first_sample, shell_rate_history.end());
                            std::cout << "Hashrate ultimi "
                                      << std::distance(first_sample, shell_rate_history.end())
                                      << " secondi (picco " << std::fixed << std::setprecision(2)
                                      << peak << " H/s)\n";
                            for (auto sample = first_sample; sample != shell_rate_history.end(); ++sample) {
                                const double rate = *sample;
                                const auto bars = peak > 0.0
                                    ? static_cast<std::size_t>(rate / peak * 40.0) : 0U;
                                std::cout << std::setw(9) << std::fixed << std::setprecision(1)
                                          << rate << " H/s | " << std::string(bars, '#') << '\n';
                            }
                        } else if (command == "cpu") {
                            std::cout << cpu_summary.dump(2) << '\n';
                        } else if (command == "logs") {
                            std::size_t count = 30;
                            if (input >> value) {
                                try { count = std::clamp<std::size_t>(std::stoul(value), 1, 200); }
                                catch (...) { count = 30; }
                            }
                            for (const auto& entry : logging::recent_entries(count)) {
                                std::cout << '[' << entry.timestamp << "] " << entry.level
                                          << ' ' << entry.category << ' ' << entry.message << '\n';
                            }
                        } else if (command == "pool") {
                            for (std::size_t index = 0; index < pools.size(); ++index) {
                                const auto& endpoint = pools[index];
                                std::cout << (index == 0 ? "Pool primario: " : "Pool failover: ")
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

                    std::cout << "Digita help per i comandi UNIQUE RX.\n";
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
                            (void)_getch();
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
                            if (!command_line.empty()) process_command(command_line);
                            command_line.clear();
                        } else if (character == '\b' || character == 127) {
                            if (!command_line.empty()) command_line.pop_back();
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
                logging::info("cpu", "share accepted (response " +
                    std::to_string(details.value("response_id", std::uint64_t{0})) + ")");
            } else if (event == "SHARE_REJECTED") {
                const auto response = details.value("response", nlohmann::json::object());
                const auto reason = response.contains("error") && !response["error"].is_null()
                    ? format_rpc_error(response["error"]) : response.dump();
                logging::warning("cpu", "share rejected: " + reason);
            } else if (event == "SHARE_STALE") {
                logging::warning("cpu", "stale share: " +
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
                const nlohmann::json reconnect_status = {
                    {"state", "RICONNESSIONE"}, {"managed", true},
                    {"error", error.what()},
                    {"hashrate", {{"10s", nullptr}, {"60s", nullptr}, {"15m", nullptr}}},
                    {"shares", {{"found", state.found.load()},
                                 {"submitted", state.submitted.load()},
                                 {"accepted", state.accepted.load()},
                                 {"rejected", state.rejected.load()},
                                 {"stale", state.stale_shares.load()},
                                 {"expired", state.expired_shares.load()},
                                 {"dropped", state.dropped_shares.load()},
                                 {"pending", 0},
                                 {"total", state.accepted.load() + state.rejected.load() + state.stale_shares.load()}}},
                    {"uptime_seconds", reconnect_uptime},
                    {"algorithm", "rx/0"}, {"job_id", nullptr}, {"height", nullptr}, {"job_difficulty", nullptr},
                    {"threads", pool_threads}, {"cpu_priority", state.cpu_priority}, {"cpu", cpu_summary},
                    {"workers", collect_worker_status()},
                    {"best", collect_best_status()},
                    {"msr_active", msr.active()}, {"msr_status", msr_status}, {"full_memory", full_memory},
                    {"large_pages_requested", large_pages_requested},
                    {"large_pages_active", state.large_pages_active.load(std::memory_order_relaxed)},
                    {"system", {
                        {"cpu_percent", reconnect_optional(reconnect_system.system_cpu_percent)},
                        {"process_cpu_percent", reconnect_optional(reconnect_system.process_cpu_percent)},
                        {"cpu_frequency_mhz", reconnect_optional(reconnect_system.cpu_frequency_mhz)},
                        {"memory_used_bytes", reconnect_optional(reconnect_system.memory_used_bytes)},
                        {"process_memory_bytes", reconnect_optional(reconnect_system.process_memory_bytes)},
                        {"temperature_c", reconnect_optional(reconnect_system.temperature_c)},
                        {"package_power_watts", reconnect_optional(reconnect_system.package_power_watts)},
                        {"on_battery", reconnect_system.on_battery ? nlohmann::json(*reconnect_system.on_battery) : nlohmann::json(nullptr)}
                    }},
                    {"pool", pool.host + ":" + pool.port}, {"connected", false},
                    {"pool_latency_ms", state.last_share_latency_ms.load(std::memory_order_relaxed) >= 0 ? nlohmann::json(state.last_share_latency_ms.load(std::memory_order_relaxed)) : nlohmann::json(nullptr)},
                    {"reconnects", state.reconnects.load(std::memory_order_relaxed)},
                };
                if (machine_status_channel_required()) {
                    std::cout << "UNIQUE_RX_STATUS " << reconnect_status.dump() << '\n' << std::flush;
                }
                logging::warning("net", "switching pool; retrying in " +
                    std::to_string(retry_delay.count()) + "s");
                ++pool_failures;
                if (retries_per_pool == 0 || pool_failures >= retries_per_pool) {
                    pool_index = (pool_index + 1) % pools.size();
                    pool_failures = 0;
                    logging::warning("net", "retry limit reached; switching to next configured pool");
                }
                auto remaining_delay =
                    std::chrono::duration_cast<std::chrono::milliseconds>(retry_delay);
                while (remaining_delay > std::chrono::milliseconds::zero() &&
                       !g_stop_requested && !state.stop.load(std::memory_order_acquire)) {
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