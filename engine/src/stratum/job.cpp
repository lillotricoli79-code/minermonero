#include "stratum.hpp"
#include "cpu/topology.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
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
    result.threads = select_cpu_threads(
        config.value("cpu", nlohmann::json::object()));
    result.tls_fingerprint = normalize_fingerprint(
        pool.value("tls-fingerprint", std::string()));
    if (!result.tls_fingerprint.empty() && !result.tls) {
        throw std::invalid_argument("pool fingerprint requires TLS");
    }
    return result;
}

} // namespace unique_rx
