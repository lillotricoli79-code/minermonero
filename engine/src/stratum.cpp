#include "stratum/stratum.hpp"
#include "core/mining.hpp"
#include "randomx/cache.hpp"
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
    try {
        std::ifstream file(config_path);
        if (!file) throw std::runtime_error("cannot open config.json");
        const auto config = nlohmann::json::parse(file);
        const auto pool = parse_pool_config(config);
        const auto randomx_config = config.value("randomx", nlohmann::json::object());
        const auto wrmsr = randomx_config.value("wrmsr", nlohmann::json());
        const bool msr_explicitly_disabled =
            (wrmsr.is_boolean() && !wrmsr.get<bool>()) ||
            (wrmsr.is_number_integer() && wrmsr.get<std::int64_t>() < 0);
        if (!msr_explicitly_disabled) {
            throw std::runtime_error(
                "Native engine MSR is not implemented. Set randomx.wrmsr to false "
                "only if you explicitly accept mining without MSR, or use XMRig "
                "for automatic, Fast or custom MSR presets.");
        }

        platform::NetworkRuntime network;
        StratumTransport transport;
        transport.connect_to(pool);
        transport.send_json({{"id", 1}, {"jsonrpc", "2.0"}, {"method", "login"},
            {"params", {{"login", pool.wallet}, {"pass", pool.password},
                         {"rigid", pool.worker}, {"agent", "UNIQUE-RX/0.1"},
                         {"algo", nlohmann::json::array({"rx/0"})}}}});

        std::string session_id;
        std::array<std::uint8_t, 32> active_seed{};
        bool have_seed = false;
        std::shared_ptr<randomx_cache> cache;
        const auto flags = randomx_get_flags();
        std::uint64_t generation = 0;
        using Clock = std::chrono::steady_clock;
        constexpr auto kShareResponseTimeout = std::chrono::seconds(60);
        constexpr std::size_t kMaximumPendingShares = 4096;
        std::unordered_map<std::uint64_t, Clock::time_point> pending;
        std::uint64_t next_id = 2;
        auto started = std::chrono::steady_clock::now();
        auto last_sample_time = started;
        std::deque<std::pair<std::chrono::steady_clock::time_point, std::uint64_t>> samples;

        const auto install_job = [&](Job next_job) {
            if (!have_seed || next_job.seed != active_seed) {
                cache = create_randomx_cache(next_job.seed, flags);
                active_seed = next_job.seed;
                have_seed = true;
            }
            auto work = std::make_shared<MiningWork>(MiningWork{
                std::move(next_job), cache, flags, ++generation,
            });
            state.set_work(std::move(work));
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
                    auto job = parse_job(result.at("job"));
                    std::cout << "Pool login accepted; job " << job.id << '\n';
                    install_job(std::move(job));
                } else if (message.contains("id")) {
                    const auto response_id = message["id"].get<std::uint64_t>();
                    if (pending.erase(response_id) != 0) {
                        const bool rejected = message.contains("error") && !message["error"].is_null();
                        std::cout << (rejected ? "Share rejected: " : "Share accepted\n");
                        if (rejected) std::cout << message["error"].dump() << '\n';
                        if (rejected) state.rejected.fetch_add(1, std::memory_order_relaxed);
                        else state.accepted.fetch_add(1, std::memory_order_relaxed);
                    }
                }
            }

            const auto now = Clock::now();
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
                for (std::uint32_t worker = 0; worker < pool.threads; ++worker) {
                    workers.emplace_back(hash_worker, std::ref(state), worker,
                                         pool.threads, maximum_hashes);
                }
            }

            send_ready_shares();
            if (now - last_sample_time >= std::chrono::seconds(1)) {
                const auto total_hashes = state.hashes.load(std::memory_order_relaxed);
                samples.emplace_back(now, total_hashes);
                while (!samples.empty() && now - samples.front().first > std::chrono::seconds(900)) {
                    samples.pop_front();
                }
                const auto rate_for = [&](std::chrono::seconds window) {
                    if (samples.size() < 2) return 0.0;
                    const auto threshold = now - window;
                    auto start_sample = samples.front();
                    for (const auto& sample : samples) {
                        if (sample.first <= threshold) start_sample = sample;
                    }
                    const auto duration = std::chrono::duration<double>(now - start_sample.first).count();
                    return duration > 0 ? (total_hashes - start_sample.second) / duration : 0.0;
                };
                const auto uptime = std::chrono::duration_cast<std::chrono::seconds>(now - started).count();
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
                                 {"pending", pending.size()},
                                 {"total", state.accepted.load() + state.rejected.load()}}},
                    {"uptime_seconds", uptime},
                    {"pool", pool.host + ":" + pool.port}, {"connected", true},
                };
                std::cout << "UNIQUE_RX_STATUS " << status.dump() << '\n' << std::flush;
                last_sample_time = now;
            }
        }

        state.stop.store(true, std::memory_order_release);
        for (auto& worker : workers) if (worker.joinable()) worker.join();
        send_ready_shares();
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