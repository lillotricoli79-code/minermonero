#pragma once

#include <nlohmann/json.hpp>

#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <optional>
#include <vector>

namespace unique_rx {

inline constexpr std::size_t kNonceOffset = 39;
inline constexpr std::size_t kMaximumBlobSize = 1024;

inline std::uint32_t decode_nonce_le(const std::uint8_t* bytes) noexcept {
    return static_cast<std::uint32_t>(bytes[0]) |
        (static_cast<std::uint32_t>(bytes[1]) << 8) |
        (static_cast<std::uint32_t>(bytes[2]) << 16) |
        (static_cast<std::uint32_t>(bytes[3]) << 24);
}

inline std::array<std::uint8_t, 4> encode_nonce_le(std::uint32_t nonce) noexcept {
    return {
        static_cast<std::uint8_t>(nonce),
        static_cast<std::uint8_t>(nonce >> 8),
        static_cast<std::uint8_t>(nonce >> 16),
        static_cast<std::uint8_t>(nonce >> 24),
    };
}

// A Stratum job's 32-bit nonce space is a ring. The offset is bounded by
// the worker partition (0..2^32-1); adding it to a non-zero pool nonce base
// must wrap modulo 2^32 rather than prematurely exhausting the job.
inline constexpr std::uint64_t kNonceSpace = (std::uint64_t{1} << 32);
inline std::uint32_t nonce_at_offset(std::uint32_t base,
                                     std::uint64_t offset) noexcept {
    return base + static_cast<std::uint32_t>(offset);
}

struct Job {
    std::string id;
    std::vector<std::uint8_t> blob;
    std::array<std::uint8_t, 32> seed{};
    std::uint64_t target = 0;
    std::optional<std::uint64_t> height;
    std::uint32_t nonce = 0;
    // NiceHash reserves the high nonce byte for the pool connection prefix.
    std::uint32_t nonce_mask = UINT32_MAX;
    std::uint32_t nonce_fixed = 0;

    // Target-only updates keep the same nonce work; adapted from RxMiner (MIT).
    bool same_work(const Job& other) const {
        return id == other.id && blob == other.blob && seed == other.seed &&
            nonce_mask == other.nonce_mask && nonce_fixed == other.nonce_fixed;
    }
};

struct PoolConfig {
    std::string host;
    std::string port;
    std::string wallet;
    std::string password;
    std::string worker;
    std::string tls_fingerprint;
    std::uint32_t threads = 1;
    std::vector<std::int64_t> cpu_affinities;
    bool tls = false;
    bool keepalive = false;
    std::uint32_t keepalive_seconds = 0;
    bool nicehash = false;
    bool sni = false;
};

// Allocate a nonce within the pool-provided nonce range. NiceHash jobs keep
// the server-assigned high byte fixed and wrap only the low 24-bit counter.
inline std::uint64_t nonce_space(const Job& job) noexcept {
    return static_cast<std::uint64_t>(job.nonce_mask) + 1;
}

inline std::uint32_t nonce_at_offset(const Job& job,
                                     std::uint64_t offset) noexcept {
    return job.nonce_fixed |
        ((job.nonce + static_cast<std::uint32_t>(offset)) & job.nonce_mask);
}

std::vector<std::uint8_t> decode_hex(std::string_view value);
std::uint64_t parse_share_target(std::string_view value);
Job parse_job(const nlohmann::json& value);

// C++ port of the RxMiner/NyxRX JSON-RPC Stratum protocol helpers.
nlohmann::json make_login_request(std::uint64_t id, const PoolConfig& pool,
                                  std::string_view agent);
nlohmann::json make_submit_request(std::uint64_t id, std::string_view session_id,
                                   std::string_view job_id, std::string_view nonce_hex,
                                   std::string_view result_hex);
nlohmann::json make_keepalive_request(std::uint64_t id, std::string_view session_id);
std::string validate_login_response(const nlohmann::json& message);
bool is_job_notification(const nlohmann::json& message) noexcept;
std::optional<std::uint64_t> parse_rpc_response_id(const nlohmann::json& message) noexcept;
bool rpc_response_failed(const nlohmann::json& message) noexcept;
std::string format_rpc_error(const nlohmann::json& error);
bool login_advertises_nicehash(const nlohmann::json& result) noexcept;
void apply_nicehash_extension(Job& job, bool enabled) noexcept;
std::vector<PoolConfig> parse_pool_configs(const nlohmann::json& config);
PoolConfig parse_pool_config(const nlohmann::json& config);
int run_miner(const std::filesystem::path& config_path,
              std::uint64_t maximum_hashes = 0,
              std::uint32_t thread_override = 0);

} // namespace unique_rx