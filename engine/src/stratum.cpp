#include "stratum.hpp"
#include "core/mining.hpp"
#include "randomx/cache.hpp"
#include "platform/socket.hpp"

#include <randomx.h>

#include <openssl/evp.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>

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

class Transport {
public:
    ~Transport() {
        if (ssl_ != nullptr) SSL_free(ssl_);
        if (context_ != nullptr) SSL_CTX_free(context_);
        platform::close_socket(socket_);
    }

    void connect_to(const PoolConfig& config) {
        addrinfo hints{};
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;
        addrinfo* addresses = nullptr;
        const int lookup = getaddrinfo(config.host.c_str(), config.port.c_str(),
                                       &hints, &addresses);
        if (lookup != 0) throw std::runtime_error(platform::address_error(lookup));

        for (auto* address = addresses; address != nullptr; address = address->ai_next) {
            socket_ = ::socket(address->ai_family, address->ai_socktype, address->ai_protocol);
            if (!platform::socket_valid(socket_)) continue;
            if (platform::connect_socket(socket_, address->ai_addr,
                                         static_cast<int>(address->ai_addrlen)) == 0) break;
            platform::close_socket(socket_);
            socket_ = platform::kInvalidSocket;
        }
        freeaddrinfo(addresses);
        if (!platform::socket_valid(socket_)) throw std::runtime_error("unable to connect to pool");

        if (config.tls) connect_tls(config);
        if (!platform::set_nonblocking(socket_)) {
            throw std::runtime_error("unable to set nonblocking pool socket");
        }
    }

    void send_json(const nlohmann::json& value) {
        const auto wire = value.dump() + "\n";
        std::size_t sent = 0;
        while (sent < wire.size()) {
            int count = 0;
            if (ssl_ != nullptr) {
                count = SSL_write(ssl_, wire.data() + sent,
                                  static_cast<int>(wire.size() - sent));
                if (count <= 0) {
                    const int error = SSL_get_error(ssl_, count);
                    if (error == SSL_ERROR_WANT_READ || error == SSL_ERROR_WANT_WRITE) {
                        wait_for(error == SSL_ERROR_WANT_READ ? platform::kReadEvent : platform::kWriteEvent, 1000);
                        continue;
                    }
                    throw std::runtime_error("TLS write to pool failed");
                }
            } else {
                count = platform::send_bytes(socket_, wire.data() + sent, wire.size() - sent);
                if (count < 0) {
                    const int error = platform::socket_error();
                    if (platform::socket_would_block(error) || platform::socket_interrupted(error)) {
                        wait_for(platform::kWriteEvent, 1000);
                        continue;
                    }
                }
                if (count <= 0) throw std::runtime_error("socket write to pool failed");
            }
            sent += static_cast<std::size_t>(count);
        }
    }

    std::vector<std::string> read_lines(int timeout_ms) {
        std::vector<std::string> lines;
        const bool tls_buffered = ssl_ != nullptr && SSL_pending(ssl_) > 0;
        if (!tls_buffered && !wait_for(platform::kReadEvent, timeout_ms)) return lines;

        std::array<char, 8192> chunk{};
        int count = 0;
        if (ssl_ != nullptr) {
            count = SSL_read(ssl_, chunk.data(), static_cast<int>(chunk.size()));
            if (count <= 0) {
                const int error = SSL_get_error(ssl_, count);
                if (error == SSL_ERROR_WANT_READ || error == SSL_ERROR_WANT_WRITE) return lines;
                if (error == SSL_ERROR_ZERO_RETURN) throw std::runtime_error("pool closed TLS connection");
                throw std::runtime_error("TLS read from pool failed");
            }
        } else {
            count = platform::receive_bytes(socket_, chunk.data(), chunk.size());
            if (count == 0) throw std::runtime_error("pool closed connection");
            if (count < 0) {
                const int error = platform::socket_error();
                if (platform::socket_would_block(error) || platform::socket_interrupted(error)) return lines;
                throw std::runtime_error("socket read from pool failed");
            }
        }

        buffer_.append(chunk.data(), static_cast<std::size_t>(count));
        if (buffer_.size() > 1024 * 1024) throw std::runtime_error("oversized Stratum response");
        std::size_t consumed = 0;
        while (true) {
            const auto newline = buffer_.find('\n', consumed);
            if (newline == std::string::npos) break;
            auto line = buffer_.substr(consumed, newline - consumed);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (!line.empty()) lines.push_back(std::move(line));
            consumed = newline + 1;
        }
        buffer_.erase(0, consumed);
        return lines;
    }

private:
    platform::Socket socket_ = platform::kInvalidSocket;
    SSL_CTX* context_ = nullptr;
    SSL* ssl_ = nullptr;
    std::string buffer_;

    bool wait_for(short events, int timeout_ms) const {
        return platform::wait_socket(socket_, events, timeout_ms);
    }

    void connect_tls(const PoolConfig& config) {
        context_ = SSL_CTX_new(TLS_client_method());
        if (context_ == nullptr) throw std::runtime_error("TLS context creation failed");
        SSL_CTX_set_min_proto_version(context_, TLS1_2_VERSION);
        if (config.tls_fingerprint.empty()) {
            SSL_CTX_set_verify(context_, SSL_VERIFY_PEER, nullptr);
            if (SSL_CTX_set_default_verify_paths(context_) != 1) {
                throw std::runtime_error("cannot load default TLS trust store");
            }
        } else {
            SSL_CTX_set_verify(context_, SSL_VERIFY_NONE, nullptr);
        }
        ssl_ = SSL_new(context_);
        if (ssl_ == nullptr) throw std::runtime_error("TLS session creation failed");
        SSL_set_tlsext_host_name(ssl_, config.host.c_str());
        if (config.tls_fingerprint.empty()) SSL_set1_host(ssl_, config.host.c_str());
        SSL_set_fd(ssl_, static_cast<int>(socket_));
        if (SSL_connect(ssl_) != 1) throw std::runtime_error("TLS handshake with pool failed");
        if (!config.tls_fingerprint.empty()) verify_fingerprint(config.tls_fingerprint);
    }

    void verify_fingerprint(const std::string& expected) const {
        X509* certificate = SSL_get1_peer_certificate(ssl_);
        if (certificate == nullptr) throw std::runtime_error("pool did not provide a TLS certificate");
        std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
        unsigned int digest_size = 0;
        const bool computed = X509_digest(certificate, EVP_sha256(), digest.data(),
                                          &digest_size) == 1;
        X509_free(certificate);
        if (!computed || hex_encode(digest.data(), digest_size) != expected) {
            throw std::runtime_error("pool TLS certificate fingerprint mismatch");
        }
    }
};

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
        Transport transport;
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