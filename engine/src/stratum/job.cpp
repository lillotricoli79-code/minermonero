#include "stratum.hpp"
#include "cpu/topology.hpp"

#include <algorithm>
#include <charconv>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <system_error>
#include <string>
#include <utility>
#include <vector>

namespace unique_rx {
namespace {

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
    if (value.size() != 8 && value.size() != 16) {
        throw std::invalid_argument("pool target must be 4 or 8 bytes");
    }
    const auto bytes = decode_hex(value);
    if (bytes.size() == 4) {
        const std::uint32_t compact = static_cast<std::uint32_t>(bytes[0]) |
            (static_cast<std::uint32_t>(bytes[1]) << 8) |
            (static_cast<std::uint32_t>(bytes[2]) << 16) |
            (static_cast<std::uint32_t>(bytes[3]) << 24);
        if (compact == 0) throw std::invalid_argument("zero pool target");
        // Match the established CryptoNote/RandomX pool compact-target
        // expansion used by XMRig: preserve the pool's integer division
        // semantics exactly for 4-byte targets.
        return UINT64_MAX /
            (UINT32_MAX / static_cast<std::uint64_t>(compact));
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
    if (job.id.empty() || job.id.size() > 256) {
        throw std::invalid_argument("pool job id length is invalid");
    }
    const auto blob_hex = value.at("blob").get<std::string>();
    if ((blob_hex.size() & 1U) != 0 ||
        blob_hex.size() < (kNonceOffset + sizeof(std::uint32_t)) * 2 ||
        blob_hex.size() > kMaximumBlobSize * 2) {
        throw std::invalid_argument("pool job blob length is invalid");
    }
    job.blob = decode_hex(blob_hex);
    const auto seed_hex = value.at("seed_hash").get<std::string>();
    if (seed_hex.size() != job.seed.size() * 2) {
        throw std::invalid_argument("seed hash must be 32 bytes");
    }
    const auto seed = decode_hex(seed_hex);
    std::copy(seed.begin(), seed.end(), job.seed.begin());
    job.target = parse_share_target(value.at("target").get<std::string>());
    job.height = value.value("height", std::uint64_t{0});
    job.nonce = decode_nonce_le(job.blob.data() + kNonceOffset);
    return job;
}

std::vector<PoolConfig> parse_pool_configs(const nlohmann::json& config) {
    if (!config.contains("pools") || !config["pools"].is_array() || config["pools"].empty()) {
        throw std::invalid_argument("config.json has no enabled pool");
    }

    const auto cpu_threads = select_cpu_threads(
        config.value("cpu", nlohmann::json::object()));
    if (cpu_threads == 0) {
        throw std::invalid_argument("CPU mining is disabled in the configuration");
    }

    std::vector<PoolConfig> result;
    for (const auto& pool : config["pools"]) {
        if (!pool.is_object()) {
            throw std::invalid_argument("pool configuration entry must be an object");
        }
        if (!pool.value("enabled", true)) continue;

        const auto address = pool.at("url").get<std::string>();
        PoolConfig entry;
        if (!address.empty() && address.front() == '[') {
            const auto end_bracket = address.find(']');
            if (end_bracket == std::string::npos || end_bracket + 2 >= address.size() ||
                address[end_bracket + 1] != ':') {
                throw std::invalid_argument("pool address must be [IPv6]:port");
            }
            entry.host = address.substr(1, end_bracket - 1);
            entry.port = address.substr(end_bracket + 2);
        } else {
            const auto separator = address.rfind(':');
            if (separator == std::string::npos || separator == 0 ||
                separator + 1 == address.size()) {
                throw std::invalid_argument("pool address must be host:port");
            }
            entry.host = address.substr(0, separator);
            entry.port = address.substr(separator + 1);
        }
        if (entry.host.empty() || entry.host.find_first_of(" \t\r\n") != std::string::npos) {
            throw std::invalid_argument("pool host is empty or contains whitespace");
        }
        unsigned int port_number = 0;
        const auto port_parse = std::from_chars(
            entry.port.data(), entry.port.data() + entry.port.size(), port_number);
        if (entry.port.empty() || port_parse.ec != std::errc{} ||
            port_parse.ptr != entry.port.data() + entry.port.size() ||
            port_number == 0 || port_number > 65535) {
            throw std::invalid_argument("pool port must be an integer between 1 and 65535");
        }
        entry.wallet = pool.at("user").get<std::string>();
        if (entry.wallet.empty() ||
            entry.wallet.find_first_of("\r\n") != std::string::npos) {
            throw std::invalid_argument("pool wallet/login cannot be empty or contain newlines");
        }
        entry.password = pool.value("pass", std::string("x"));
        if (entry.password.find_first_of("\r\n") != std::string::npos) {
            throw std::invalid_argument("pool password cannot contain newlines");
        }
        entry.worker = pool.value("rig-id", std::string("rig01"));
        entry.tls = pool.value("tls", false);
        entry.keepalive = pool.value("keepalive", false);
        entry.threads = cpu_threads;
        entry.tls_fingerprint = normalize_fingerprint(
            pool.value("tls-fingerprint", std::string()));
        if (!entry.tls_fingerprint.empty() && !entry.tls) {
            throw std::invalid_argument("pool fingerprint requires TLS");
        }
        result.push_back(std::move(entry));
    }

    if (result.empty()) {
        throw std::invalid_argument("config.json has no enabled pool");
    }
    return result;
}

PoolConfig parse_pool_config(const nlohmann::json& config) {
    auto pools = parse_pool_configs(config);
    return std::move(pools.front());
}

} // namespace unique_rx
