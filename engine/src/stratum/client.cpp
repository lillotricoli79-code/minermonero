#include "stratum.hpp"
#include "core/mining.hpp"
#include "randomx/cache.hpp"
#include "randomx/dataset.hpp"
#include "msr/msr.hpp"
#include "platform/socket.hpp"
#include "stratum/transport.hpp"

#include <randomx.h>


#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
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
    MiningState state;
    std::vector<std::jthread> workers;
    struct StopWorkers {
        MiningState& state;
        ~StopWorkers() { state.stop.store(true, std::memory_order_release); }
    } stop_workers{state};

    using Clock = std::chrono::steady_clock;
    constexpr auto kShareResponseTimeout = std::chrono::seconds(60);
    constexpr std::size_t kMaximumPendingShares = 4096;

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
                "Native engine supports the Intel automatic RandomX MSR preset only; "
                "use XMRig for custom MSR configurations.");
        }
        MsrManager msr(randomx_config.value("msr-module", std::string()));
        if (!msr_explicitly_disabled) {
            msr.apply_randomx_preset();
            std::cout << "Intel RandomX MSR preset applied and will be restored on exit.\n";
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
        std::uint64_t generation = 0;
        const auto pool_threads = pools.front().threads;
        auto started = Clock::now();
        auto last_sample_time = started;
        std::deque<std::pair<Clock::time_point, std::uint64_t>> samples;
        std::unordered_map<std::uint64_t, Clock::time_point> pending;
        std::size_t pool_index = 0;
        std::chrono::seconds retry_delay{1};

        const auto install_job = [&](Job next_job) {
            if (!have_seed || next_job.seed != active_seed) {
                cache = create_randomx_cache(next_job.seed, flags);
                dataset.reset();
                if (full_memory) {
                    std::cout << "Initializing RandomX full dataset..." << std::flush;
                    dataset = create_randomx_dataset(cache, pool_threads, flags);
                    std::cout << " ready\n";
                }
                active_seed = next_job.seed;
                have_seed = true;
            }
            auto work = std::make_shared<MiningWork>(MiningWork{
                std::move(next_job), cache, dataset, flags, full_memory, ++generation,
            });
            state.set_work(std::move(work));
        };

        while (!state.stop.load(std::memory_order_acquire)) {
            const auto& pool = pools[pool_index];
            state.set_work(nullptr);
            pending.clear();
            std::uint64_t next_id = 2;
            std::string session_id;
            auto next_keepalive = Clock::time_point::max();
            try {
                StratumTransport transport;
                transport.connect_to(pool);
                std::cout << "Connected to pool " << pool.host << ':' << pool.port << '\n';
                transport.send_json({{"id", 1}, {"jsonrpc", "2.0"}, {"method", "login"},
                    {"params", {{"login", pool.wallet}, {"pass", pool.password},
                                 {"rigid", pool.worker}, {"agent", "UNIQUE-RX/0.1"},
                                 {"algo", nlohmann::json::array({"rx/0"})}}}});
                const auto login_deadline = Clock::now() + std::chrono::seconds(30);

                const auto record_share_response = [&](const nlohmann::json& message) {
                    if (!message.contains("id")) return;
                    std::uint64_t response_id = 0;
                    try {
                        response_id = message["id"].get<std::uint64_t>();
                    } catch (const nlohmann::json::exception&) {
                        return;
                    }
                    if (pending.erase(response_id) == 0) return;

                    const bool has_error =
                        message.contains("error") && !message["error"].is_null();
                    const bool accepted = !has_error && message.contains("result") &&
                        message["result"].is_object() &&
                        message["result"].value("status", std::string()) == "OK";
                    if (accepted) {
                        std::cout << "Share accepted\n";
                        state.accepted.fetch_add(1, std::memory_order_relaxed);
                    } else {
                        std::cout << "Share rejected: ";
                        if (has_error) std::cout << message["error"].dump();
                        else if (message.contains("result")) std::cout << message["result"].dump();
                        else std::cout << "missing or invalid submit result";
                        std::cout << '\n';
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
                    for (const auto& share : ready) {
                        const auto work = state.current_work();
                        if (!work || work->generation != share.generation || session_id.empty()) {
                            state.stale_shares.fetch_add(1, std::memory_order_relaxed);
                            continue;
                        }
                        const auto submit_id = next_id++;
                        std::array<std::uint8_t, 4> nonce_bytes{};
                        std::memcpy(nonce_bytes.data(), &share.nonce, nonce_bytes.size());
                        transport.send_json({{"id", submit_id}, {"jsonrpc", "2.0"},
                            {"method", "submit"}, {"params", {{"id", session_id},
                            {"job_id", share.job_id},
                            {"nonce", hex_encode(nonce_bytes.data(), nonce_bytes.size())},
                            {"result", hex_encode(share.hash.data(), share.hash.size())}}}});
                        pending.emplace(submit_id, Clock::now());
                        state.submitted.fetch_add(1, std::memory_order_relaxed);
                    }
                };

                while (!state.stop.load(std::memory_order_acquire)) {
                    for (const auto& line : transport.read_lines(2)) {
                        const auto message = nlohmann::json::parse(line);
                        if (message.value("method", std::string()) == "job") {
                            auto job = parse_job(message.at("params"));
                            std::cout << "New job " << job.id << " height "
                                      << message["params"].value("height", 0ULL) << '\n';
                            install_job(std::move(job));
                        } else if (message.value("id", 0ULL) == 1) {
                            if (message.contains("error") && !message["error"].is_null()) {
                                throw std::runtime_error("pool rejected login: " + message["error"].dump());
                            }
                            const auto& result = message.at("result");
                            if (result.value("status", std::string()) != "OK") {
                                throw std::runtime_error("pool login status is not OK");
                            }
                            session_id = result.at("id").get<std::string>();
                            if (pool.keepalive) {
                                next_keepalive = Clock::now() + std::chrono::seconds(60);
                            }
                            auto job = parse_job(result.at("job"));
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
                        transport.send_json({{"id", next_id++}, {"jsonrpc", "2.0"},
                            {"method", "keepalived"}, {"params", {{"id", session_id}}}});
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
                                         {"total", state.accepted.load() + state.rejected.load()}}},
                            {"uptime_seconds", uptime},
                            {"pool", pool.host + ":" + pool.port}, {"connected", true},
                        };
                        std::cout << "UNIQUE_RX_STATUS " << status.dump() << '\n' << std::flush;
                        last_sample_time = now;
                    }
                }

                if (state.stop.load(std::memory_order_acquire)) {
                    send_ready_shares();
                    const auto drain_deadline = Clock::now() + std::chrono::milliseconds(250);
                    while (!pending.empty() && Clock::now() < drain_deadline) {
                        for (const auto& line : transport.read_lines(10)) {
                            record_share_response(nlohmann::json::parse(line));
                        }
                    }
                    break;
                }
            } catch (const std::exception& error) {
                if (state.stop.load(std::memory_order_acquire)) break;
                state.set_work(nullptr);
                if (!pending.empty()) {
                    state.expired_shares.fetch_add(pending.size(), std::memory_order_relaxed);
                    pending.clear();
                }
                std::cerr << "Pool " << pool.host << ':' << pool.port
                          << " disconnected: " << error.what()
                          << "; switching pool and retrying in " << retry_delay.count()
                          << "s\n";
                pool_index = (pool_index + 1) % pools.size();
                std::this_thread::sleep_for(retry_delay);
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
    }
}
} // namespace unique_rx