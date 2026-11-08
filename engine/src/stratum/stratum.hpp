#pragma once

#include <nlohmann/json.hpp>

#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
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

struct Job {
    std::string id;
    std::vector<std::uint8_t> blob;
    std::array<std::uint8_t, 32> seed{};
    std::uint64_t target = 0;
    std::uint32_t nonce = 0;

    // Target-only updates keep the same nonce work; adapted from RxMiner (MIT).
    bool same_work(const Job& other) const {
        return id == other.id && blob == other.blob && seed == other.seed;
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
    bool tls = false;
    bool keepalive = false;
};

std::vector<std::uint8_t> decode_hex(std::string_view value);
std::uint64_t parse_share_target(std::string_view value);
Job parse_job(const nlohmann::json& value);
std::vector<PoolConfig> parse_pool_configs(const nlohmann::json& config);
PoolConfig parse_pool_config(const nlohmann::json& config);
int run_miner(const std::filesystem::path& config_path,
              std::uint64_t maximum_hashes = 0);

} // namespace unique_rx