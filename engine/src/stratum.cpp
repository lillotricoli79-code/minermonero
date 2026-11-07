#include "stratum.hpp"

#include <randomx.h>

#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <deque>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_set>

namespace unique_rx {
namespace {

struct CacheRelease {
    void operator()(randomx_cache* cache) const {
        if (cache != nullptr) randomx_release_cache(cache);
    }
};

struct VmRelease {
    void operator()(randomx_vm* vm) const {
        if (vm != nullptr) randomx_destroy_vm(vm);
    }
};

using Cache = std::unique_ptr<randomx_cache, CacheRelease>;
using Vm = std::unique_ptr<randomx_vm, VmRelease>;
using Hash = std::array<std::uint8_t, RANDOMX_HASH_SIZE>;

struct MiningWork {
    Job job;
    std::shared_ptr<randomx_cache> cache;
    randomx_flags flags;
    std::uint64_t generation;
};

struct Share {
    std::uint64_t generation;
    std::string job_id;
    std::uint32_t nonce;
    Hash hash;
};

struct MiningState {
    std::mutex work_mutex;
    std::shared_ptr<const MiningWork> work;
    std::atomic<bool> stop{false};
    std::atomic<std::uint64_t> hashes{0};
    std::atomic<std::uint64_t> found{0};
    std::atomic<std::uint64_t> submitted{0};
    std::atomic<std::uint64_t> accepted{0};
    std::atomic<std::uint64_t> rejected{0};
    std::mutex share_mutex;
    std::deque<Share> shares;
    std::mutex error_mutex;
    std::string worker_error;

    std::shared_ptr<const MiningWork> current_work() {
        std::lock_guard lock(work_mutex);
        return work;
    }

    void set_work(std::shared_ptr<const MiningWork> next) {
        std::lock_guard lock(work_mutex);
        work = std::move(next);
    }

    void report_worker_error(const std::string& error) {
        {
            std::lock_guard lock(error_mutex);
            if (worker_error.empty()) worker_error = error;
        }
        stop.store(true, std::memory_order_release);
    }
};
std::string hex_encode(const std::uint8_t* bytes, std::size_t length) {
    static constexpr char digits[] = "0123456789abcdef";
    std::string result(length * 2, '0');
    for (std::size_t index = 0; index < length; ++index) {
        result[index * 2] = digits[bytes[index] >> 4];
        result[index * 2 + 1] = digits[bytes[index] & 0x0f];
    }
    return result;
}

std::uint32_t read_nonce(const std::vector<std::uint8_t>& blob) {
    std::uint32_t nonce = 0;
    std::memcpy(&nonce, blob.data() + kNonceOffset, sizeof(nonce));
    return nonce;
}

void write_nonce(std::vector<std::uint8_t>& blob, std::uint32_t nonce) {
    std::memcpy(blob.data() + kNonceOffset, &nonce, sizeof(nonce));
}

bool meets_target(const Hash& hash, std::uint64_t target) {
    std::uint64_t high = 0;
    std::memcpy(&high, hash.data() + 24, sizeof(high));
    return high < target;
}

std::string normalize_fingerprint(std::string value) {
    value.erase(std::remove(value.begin(), value.end(), ':'), value.end());
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    if (!value.empty() && (value.size() != 64 ||
        !std::all_of(value.begin(), value.end(), [](unsigned char ch) {
            return std::isxdigit(ch) != 0;
        }))) {
        throw std::invalid_argument("TLS fingerprint must be 64 hexadecimal characters");
    }
    return value;
}

void hash_worker(MiningState& state, std::uint32_t worker_id,
                 std::uint32_t worker_count, std::uint64_t maximum_hashes) {
    Vm vm;
    std::shared_ptr<const MiningWork> active_work;
    std::uint64_t cursor = 0;
    try {
        while (!state.stop.load(std::memory_order_acquire)) {
            const auto work = state.current_work();
            if (!work) {
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
                continue;
            }
            if (!active_work || active_work->generation != work->generation) {
                vm.reset(randomx_create_vm(work->flags, work->cache.get(), nullptr));
                if (!vm) throw std::runtime_error("worker VM creation failed");
                active_work = work;
                cursor = 0;
            }

            auto blob = work->job.blob;
            const auto nonce64 = static_cast<std::uint64_t>(work->job.nonce) +
                worker_id + cursor * worker_count;
            const auto nonce = static_cast<std::uint32_t>(nonce64);
            write_nonce(blob, nonce);
            Hash hash{};
            randomx_calculate_hash(vm.get(), blob.data(), blob.size(), hash.data());
            ++cursor;
            const auto count = state.hashes.fetch_add(1, std::memory_order_relaxed) + 1;

            if (state.current_work()->generation == work->generation &&
                meets_target(hash, work->job.target)) {
                {
                    std::lock_guard lock(state.share_mutex);
                    state.shares.push_back({work->generation, work->job.id, nonce, hash});
                }
                state.found.fetch_add(1, std::memory_order_relaxed);
            }
            if (maximum_hashes != 0 && count >= maximum_hashes) {
                state.stop.store(true, std::memory_order_release);
            }
        }
    } catch (const std::exception& error) {
        state.report_worker_error(error.what());
    }
}
class Transport {
public:
    ~Transport() {
        if (ssl_ != nullptr) SSL_free(ssl_);
        if (context_ != nullptr) SSL_CTX_free(context_);
        if (socket_ >= 0) close(socket_);
    }

    void connect_to(const PoolConfig& config) {
        addrinfo hints{};
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;
        addrinfo* addresses = nullptr;
        const int lookup = getaddrinfo(config.host.c_str(), config.port.c_str(),
                                       &hints, &addresses);
        if (lookup != 0) throw std::runtime_error(gai_strerror(lookup));

        for (auto* address = addresses; address != nullptr; address = address->ai_next) {
            socket_ = ::socket(address->ai_family, address->ai_socktype, address->ai_protocol);
            if (socket_ < 0) continue;
            if (::connect(socket_, address->ai_addr, address->ai_addrlen) == 0) break;
            close(socket_);
            socket_ = -1;
        }
        freeaddrinfo(addresses);
        if (socket_ < 0) throw std::runtime_error("unable to connect to pool");

        if (config.tls) connect_tls(config);
        const auto flags = fcntl(socket_, F_GETFL, 0);
        if (flags < 0 || fcntl(socket_, F_SETFL, flags | O_NONBLOCK) < 0) {
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
                        wait_for(POLLOUT, 1000);
                        continue;
                    }
                    throw std::runtime_error("TLS write to pool failed");
                }
            } else {
                count = static_cast<int>(send(socket_, wire.data() + sent,
                                               wire.size() - sent, MSG_NOSIGNAL));
                if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) {
                    wait_for(POLLOUT, 1000);
                    continue;
                }
                if (count <= 0) throw std::runtime_error("socket write to pool failed");
            }
            sent += static_cast<std::size_t>(count);
        }
    }

    std::vector<std::string> read_lines(int timeout_ms) {
        std::vector<std::string> lines;
        const bool tls_buffered = ssl_ != nullptr && SSL_pending(ssl_) > 0;
        if (!tls_buffered && !wait_for(POLLIN, timeout_ms)) return lines;

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
            count = static_cast<int>(recv(socket_, chunk.data(), chunk.size(), 0));
            if (count == 0) throw std::runtime_error("pool closed connection");
            if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) return lines;
            if (count < 0) throw std::runtime_error("socket read from pool failed");
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
    int socket_ = -1;
    SSL_CTX* context_ = nullptr;
    SSL* ssl_ = nullptr;
    std::string buffer_;

    bool wait_for(short events, int timeout_ms) const {
        pollfd descriptor{socket_, events, 0};
        const int result = poll(&descriptor, 1, timeout_ms);
        if (result < 0 && errno != EINTR) throw std::runtime_error("pool socket poll failed");
        if (result > 0 && (descriptor.revents & (POLLERR | POLLHUP | POLLNVAL))) {
            throw std::runtime_error("pool socket disconnected");
        }
        return result > 0 && (descriptor.revents & events) != 0;
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
        SSL_set_fd(ssl_, socket_);
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

std::uint32_t nonce_from_hash(const Hash& hash) {
    return static_cast<std::uint32_t>(hash[0]) |
           (static_cast<std::uint32_t>(hash[1]) << 8) |
           (static_cast<std::uint32_t>(hash[2]) << 16) |
           (static_cast<std::uint32_t>(hash[3]) << 24);
}

} // namespace

std::vector<std::uint8_t> decode_hex(std::string_view value) {
    if ((value.size() & 1U) != 0) throw std::invalid_argument("hex string has odd length");
    const auto nibble = [](char ch) -> int {
        if (ch >= '0' && ch <= '9') return ch - '0';
        if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
        if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
        return -1;
    };
    std::vector<std::uint8_t> bytes(value.size() / 2);
    for (std::size_t index = 0; index < bytes.size(); ++index) {
        const int high = nibble(value[index * 2]);
        const int low = nibble(value[index * 2 + 1]);
        if (high < 0 || low < 0) throw std::invalid_argument("invalid hex string");
        bytes[index] = static_cast<std::uint8_t>((high << 4) | low);
    }
    return bytes;
}

std::uint64_t parse_share_target(std::string_view value) {
    const auto bytes = decode_hex(value);
    if (bytes.size() == 4) {
        const std::uint32_t compact = static_cast<std::uint32_t>(bytes[0]) |
            (static_cast<std::uint32_t>(bytes[1]) << 8) |
            (static_cast<std::uint32_t>(bytes[2]) << 16) |
            (static_cast<std::uint32_t>(bytes[3]) << 24);
        if (compact == 0) throw std::invalid_argument("zero pool target");
        return UINT64_MAX / (UINT32_MAX / static_cast<std::uint64_t>(compact));
    }
    if (bytes.size() == 8) {
        std::uint64_t target = 0;
        for (std::size_t index = 0; index < bytes.size(); ++index) {
            target |= static_cast<std::uint64_t>(bytes[index]) << (index * 8);
        }
        if (target == 0) throw std::invalid_argument("zero pool target");
        return target;
    }
    throw std::invalid_argument("pool target must be 4 or 8 bytes");
}

Job parse_job(const nlohmann::json& value) {
    if (!value.is_object() || value.value("algo", std::string("rx/0")) != "rx/0") {
        throw std::invalid_argument("pool job algorithm is not rx/0");
    }
    Job job;
    job.id = value.at("job_id").get<std::string>();
    if (job.id.empty()) throw std::invalid_argument("pool job id is empty");
    job.blob = decode_hex(value.at("blob").get<std::string>());
    if (job.blob.size() < kNonceOffset + sizeof(std::uint32_t) ||
        job.blob.size() > kMaximumBlobSize) {
        throw std::invalid_argument("pool job blob length is invalid");
    }
    const auto seed = decode_hex(value.at("seed_hash").get<std::string>());
    if (seed.size() != job.seed.size()) throw std::invalid_argument("seed hash must be 32 bytes");
    std::copy(seed.begin(), seed.end(), job.seed.begin());
    job.target = parse_share_target(value.at("target").get<std::string>());
    std::memcpy(&job.nonce, job.blob.data() + kNonceOffset, sizeof(job.nonce));
    return job;
}

PoolConfig parse_pool_config(const nlohmann::json& config) {
    if (!config.contains("pools") || !config["pools"].is_array() || config["pools"].empty()) {
        throw std::invalid_argument("config.json has no enabled pool");
    }
    const auto& pool = config["pools"].front();
    const auto address = pool.at("url").get<std::string>();
    PoolConfig result;
    if (!address.empty() && address.front() == '[') {
        const auto end = address.find(']');
        if (end == std::string::npos || end + 2 >= address.size() || address[end + 1] != ':') {
            throw std::invalid_argument("pool address must be [IPv6]:port");
        }
        result.host = address.substr(1, end - 1);
        result.port = address.substr(end + 2);
    } else {
        const auto separator = address.rfind(':');
        if (separator == std::string::npos || separator == 0 || separator + 1 == address.size()) {
            throw std::invalid_argument("pool address must be host:port");
        }
        result.host = address.substr(0, separator);
        result.port = address.substr(separator + 1);
    }
    result.wallet = pool.at("user").get<std::string>();
    result.password = pool.value("pass", std::string("x"));
    result.worker = pool.value("rig-id", std::string("rig01"));
    result.tls = pool.value("tls", false);
    const auto cpu = config.value("cpu", nlohmann::json::object());
    if (cpu.value("enabled", true)) {
        const auto logical_threads = std::max(1U, std::thread::hardware_concurrency());
        const auto hint = std::min(cpu.value("max-threads-hint", 0U), 100U);
        result.threads = hint == 0
            ? std::max(1U, logical_threads / 2)
            : std::clamp(static_cast<std::uint32_t>(
                  std::lround(logical_threads * (hint / 100.0))), 1U, logical_threads);
    }
    result.threads = std::min(result.threads, 64U);
    result.tls_fingerprint = normalize_fingerprint(
        pool.value("tls-fingerprint", std::string()));
    if (!result.tls_fingerprint.empty() && !result.tls) {
        throw std::invalid_argument("pool fingerprint requires TLS");
    }
    return result;
}

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
        const bool msr_requested = wrmsr.is_boolean() ? wrmsr.get<bool>() :
            (wrmsr.is_number_integer() ? wrmsr.get<int>() >= 0 :
             (wrmsr.is_array() && !wrmsr.empty()));
        if (msr_requested) {
            throw std::runtime_error(
                "MSR preset is not yet supported by the native engine; use the XMRig controller");
        }

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
        std::unordered_set<std::uint64_t> pending;
        std::uint64_t next_id = 2;
        std::uint64_t last_sample_hashes = 0;
        auto started = std::chrono::steady_clock::now();
        auto last_sample_time = started;
        std::deque<std::pair<std::chrono::steady_clock::time_point, std::uint64_t>> samples;

        const auto install_job = [&](Job next_job) {
            if (!have_seed || next_job.seed != active_seed) {
                auto* raw_cache = randomx_alloc_cache(flags);
                if (raw_cache == nullptr) throw std::runtime_error("RandomX cache allocation failed");
                cache = std::shared_ptr<randomx_cache>(raw_cache,
                    [](randomx_cache* value) { randomx_release_cache(value); });
                randomx_init_cache(cache.get(), next_job.seed.data(), next_job.seed.size());
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
            {
                std::lock_guard lock(state.share_mutex);
                ready.swap(state.shares);
            }
            for (const auto& share : ready) {
                const auto work = state.current_work();
                if (!work || work->generation != share.generation || session_id.empty()) {
                    state.rejected.fetch_add(1, std::memory_order_relaxed);
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
                pending.insert(submit_id);
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

            const auto work = state.current_work();
            if (!work || session_id.empty()) continue;
            if (workers.empty()) {
                for (std::uint32_t worker = 0; worker < pool.threads; ++worker) {
                    workers.emplace_back(hash_worker, std::ref(state), worker,
                                         pool.threads, maximum_hashes);
                }
            }

            send_ready_shares();
            const auto now = std::chrono::steady_clock::now();
            if (now - last_sample_time >= std::chrono::seconds(1)) {
                const auto total_hashes = state.hashes.load(std::memory_order_relaxed);
                const auto elapsed = std::chrono::duration<double>(now - last_sample_time).count();
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
                    {"shares", {{"accepted", state.accepted.load()},
                                 {"rejected", state.rejected.load()},
                                 {"total", state.accepted.load() + state.rejected.load()}}},
                    {"uptime_seconds", uptime},
                    {"pool", pool.host + ":" + pool.port}, {"connected", true},
                };
                std::cout << "UNIQUE_RX_STATUS " << status.dump() << '\n' << std::flush;
                last_sample_hashes = total_hashes;
                last_sample_time = now;
                static_cast<void>(elapsed);
            }
        }

        state.stop.store(true, std::memory_order_release);
        for (auto& worker : workers) if (worker.joinable()) worker.join();
        send_ready_shares();
        {
            std::lock_guard lock(state.error_mutex);
            if (!state.worker_error.empty()) throw std::runtime_error(state.worker_error);
        }
        static_cast<void>(last_sample_hashes);
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "UNIQUE RX mining error: " << error.what() << '\n';
        return 1;
    }
}

} // namespace unique_rx