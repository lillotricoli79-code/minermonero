#include "stratum.hpp"
#include "core/mining.hpp"
#include "cpu/topology.hpp"
#include "randomx/cache.hpp"
#include "randomx/dataset.hpp"
#include "msr/msr.hpp"
#include "platform/socket.hpp"
#include "stratum/transport.hpp"

#ifdef _WIN32
#include <windows.h>
#endif

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
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>

namespace unique_rx {
namespace {

using core::Hash;
using core::MiningState;
using core::MiningWork;
using core::Share;
using core::hash_worker;

volatile std::sig_atomic_t g_stop_requested = 0;

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
              std::uint64_t maximum_hashes) {
    g_stop_requested = 0;
    std::signal(SIGINT, request_stop);
    std::signal(SIGTERM, request_stop);
#ifdef _WIN32
    SetConsoleCtrlHandler(console_stop_handler, TRUE);
#endif
    MiningState state;
    std::vector<std::jthread> workers;
    struct StopWorkers {
        MiningState& state;
        ~StopWorkers() { state.stop.store(true, std::memory_order_release); }
    } stop_workers{state};

    using Clock = std::chrono::steady_clock;
    constexpr auto kShareResponseTimeout = std::chrono::seconds(60);
    // Match RxMiner's 8192-entry pending-submit backpressure ceiling.
    constexpr std::size_t kMaximumPendingShares = 8192;

    try {
        std::ifstream file(config_path);
        if (!file) throw std::runtime_error("cannot open config.json");
        const auto config = nlohmann::json::parse(file);
        const auto pools = parse_pool_configs(config);
        const auto randomx_config = config.value("randomx", nlohmann::json::object());
        const bool full_memory = randomx_config.value("full-memory", true);
        const auto wrmsr = randomx_config.value("wrmsr", nlohmann::json());
        const bool msr_explicitly_disabled =
            (wrmsr.is_boolean() && !wrmsr.get<bool>()) ||
            (wrmsr.is_number_integer() && wrmsr.get<std::int64_t>() < 0);
        if (!wrmsr.is_null() && !wrmsr.is_boolean() &&
            !(wrmsr.is_number_integer() && wrmsr.get<std::int64_t>() < 0)) {
            throw std::runtime_error(
                "Native engine accepts automatic MSR tuning only; custom register "
                "writes are intentionally unsupported.");
        }
        MsrManager msr(randomx_config.value("msr-module", std::string()));
        if (!msr_explicitly_disabled) {
            try {
                msr.apply_randomx_preset();
                std::cout << "Verified CPU-specific RandomX MSR preset applied; "
                             "original registers will be restored on exit.\n";
            } catch (const std::exception& error) {
                // MSR tuning is an optional optimization. An unknown CPU, a missing
                // driver, or unavailable permissions must never prevent rx/0 mining.
                std::cerr << "MSR tuning skipped safely: " << error.what()
                          << "; continuing with standard CPU settings.\n";
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
        if (cpu_config.value("huge-pages", false)) flags |= RANDOMX_FLAG_LARGE_PAGES;
        // Explicit JIT huge pages are optional and fall back to normal pages
        // when the OS privilege/reservation is unavailable.
        rxm_set_jit_huge_pages(cpu_config.value("jit-huge-pages", false) ? 1 : 0);
        const auto prefetch_mode = cpu_config.value("prefetch-mode", 1);
        if (prefetch_mode < 0 || prefetch_mode > 3)
            throw std::runtime_error("cpu.prefetch-mode must be between 0 and 3");
        rxm_set_prefetch_mode(prefetch_mode);
        std::uint64_t generation = 0;
        const auto pool_threads = pools.front().threads;
        const auto cpu_summary = cpu_summary_json();
        // Keep released 2 MiB RandomX scratchpads for reuse during VM recreation.
        rxm_set_memory_pool(pool_threads);
        auto started = Clock::now();
        std::ofstream stratum_log(config_path.parent_path() / "unique-rx-stratum.log",
                                  std::ios::app);
        const auto log_event = [&](const std::string& event,
                                   const nlohmann::json& details) {
            const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                Clock::now() - started).count();
            const std::string line = "[+" + std::to_string(elapsed) + "ms] " +
                event + " " + details.dump();
            std::cout << line << '\n' << std::flush;
            if (stratum_log) stratum_log << line << '\n' << std::flush;
        };
        auto last_sample_time = started;
        std::deque<std::pair<Clock::time_point, std::uint64_t>> samples;
        std::unordered_map<std::uint64_t, Clock::time_point> pending;
        std::size_t pool_index = 0;
        std::chrono::seconds retry_delay{1};

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
                if (full_memory) {
                    std::cout << "Initializing RandomX full dataset..." << std::flush;
                    next_dataset = create_randomx_dataset(
                        next_cache, pool_threads, next_flags);
                    std::cout << " ready\n";
                }
                cache = std::move(next_cache);
                dataset = std::move(next_dataset);
                flags = next_flags;
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
            std::uint64_t next_id = 2;
            std::string session_id;
            bool session_nicehash = pool.nicehash;
            auto next_keepalive = Clock::time_point::max();
            try {
                StratumTransport transport;
                transport.connect_to(pool);
                log_event("CONNECTED", {{"host", pool.host}, {"port", pool.port}});
                transport.send_json(make_login_request(1, pool,
                    "UniqueRX/1.0 (C++ RandomX; XMRig-compatible Stratum)"));
                const auto login_deadline = Clock::now() + std::chrono::seconds(30);

                const auto record_share_response = [&](const nlohmann::json& message) {
                    const auto parsed_id = parse_rpc_response_id(message);
                    if (!parsed_id) return;
                    const std::uint64_t response_id = *parsed_id;
                    if (pending.find(response_id) == pending.end()) {
                        log_event("UNMATCHED_RESPONSE", message);
                        return;
                    }
                    pending.erase(response_id);

                    const bool has_error = rpc_response_failed(message);
                    // XMRig treats an error-free JSON-RPC response as accepted;
                    // NyxRX additionally validates an explicit result.status.
                    const bool accepted = !has_error;
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
                        pending.emplace(submit_id, Clock::now());
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
                            std::cout << "Pool login accepted; job " << job.id << '\n';
                            install_job(std::move(job));
                            retry_delay = std::chrono::seconds(1);
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
                        next_keepalive = now + std::chrono::seconds(60);
                    }
                    for (auto it = pending.begin(); it != pending.end();) {
                        if (now - it->second >= kShareResponseTimeout) {
                            it = pending.erase(it);
                            state.expired_shares.fetch_add(1, std::memory_order_relaxed);
                        } else {
                            ++it;
                        }
                    }

                    const auto work = state.current_work();
                    if (!work || session_id.empty()) continue;
                    if (workers.empty()) {
                        for (std::uint32_t worker = 0; worker < pool_threads; ++worker) {
                            workers.emplace_back(hash_worker, std::ref(state), worker,
                                                 pool_threads, maximum_hashes);
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
                            {"algorithm", "rx/0"}, {"job_id", work->job.id},
                            {"height", work->job.height
                                ? nlohmann::json(*work->job.height)
                                : nlohmann::json(nullptr)},
                            {"threads", pool_threads}, {"cpu", cpu_summary},
                            {"msr_active", msr.active()}, {"full_memory", full_memory},
                            {"pool", pool.host + ":" + pool.port}, {"connected", true},
                        };
                        std::cout << "UNIQUE_RX_STATUS " << status.dump() << '\n' << std::flush;
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
                    {"algorithm", "rx/0"}, {"job_id", nullptr}, {"height", nullptr},
                    {"threads", pool_threads}, {"cpu", cpu_summary},
                    {"msr_active", msr.active()}, {"full_memory", full_memory},
                    {"pool", pool.host + ":" + pool.port}, {"connected", false},
                };
                std::cout << "UNIQUE_RX_STATUS " << reconnect_status.dump() << '\n' << std::flush;
                std::cerr << "Pool " << pool.host << ':' << pool.port
                          << " disconnected: " << error.what()
                          << "; switching pool and retrying in " << retry_delay.count()
                          << "s\n";
                pool_index = (pool_index + 1) % pools.size();
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
        {
            std::lock_guard lock(state.error_mutex);
            if (!state.worker_error.empty()) throw std::runtime_error(state.worker_error);
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "UNIQUE RX mining error: " << error.what() << '\n';
        return 1;
    } catch (...) {
        std::cerr << "UNIQUE RX mining error: unknown exception\n";
        return 1;
    }
}
} // namespace unique_rx