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

// Adapted from XMRig src/base/net/stratum/Job.cpp, Job::setTarget()
// and Job::toDiff(): Copyright (c) 2016-2025 XMRig and contributors.
// Distributed under GPL-3.0-or-later; see LICENSES/XMRIG-GPL-3.0.txt.
// The port keeps XMRig's 4-byte compact-target integer semantics while
// validating hexadecimal input and rejecting zero/unsupported target sizes.
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
    if (!value.is_object()) {
        throw std::invalid_argument("pool job must be a JSON object");
    }
    // NyxRX RandomXVersion::parse_pool_name port: accept the V1 aliases
    // while refusing rx/2 because this engine currently implements rx/0 only.
    std::string algorithm = value.value("algo", std::string("rx/0"));
    std::transform(algorithm.begin(), algorithm.end(), algorithm.begin(),
        [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    if (algorithm != "rx/0" && algorithm != "rx" &&
        algorithm != "randomx" && algorithm != "randomx/0") {
        throw std::invalid_argument("unsupported pool algorithm: " + algorithm +
                                    " (this engine supports RandomX V1 / rx/0)");
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
    if (value.contains("height") && !value["height"].is_null()) {
        const auto& height = value["height"];
        if (height.is_number_unsigned()) {
            job.height = height.get<std::uint64_t>();
        } else if (height.is_number_integer()) {
            const auto signed_height = height.get<std::int64_t>();
            if (signed_height < 0) {
                throw std::invalid_argument("job height cannot be negative");
            }
            job.height = static_cast<std::uint64_t>(signed_height);
        } else {
            throw std::invalid_argument("job height must be a non-negative integer");
        }
    }
    const auto initial_nonce = decode_nonce_le(job.blob.data() + kNonceOffset);
    // Match XMRig's Job::setBlob and NyxRX's pool-job handling: an
    // advertised/explicit true flag enables NiceHash, and a non-zero nonce
    // prefix is also detected automatically. An explicit false must not
    // erase a prefix that XMRig would recognize from the blob itself.
    const bool nicehash = value.contains("nicehash") && !value.at("nicehash").is_null()
        ? value.at("nicehash").get<bool>() : initial_nonce != 0;
    job.nonce_mask = nicehash ? 0x00ffffffU : UINT32_MAX;
    job.nonce_fixed = initial_nonce & ~job.nonce_mask;
    job.nonce = initial_nonce & job.nonce_mask;
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

        auto address = pool.at("url").get<std::string>();
        PoolConfig entry;
        // XMRig accepts bare host:port and explicit Stratum URL schemes.
        // Preserve the URL's TLS intent while passing a normalized endpoint
        // to the native socket/TLS transport.
        bool scheme_tls = false;
        const auto scheme = address.find("://");
        if (scheme != std::string::npos) {
            const auto protocol = address.substr(0, scheme);
            if (protocol == "stratum+ssl" || protocol == "stratum+tls" ||
                protocol == "ssl" || protocol == "tls") {
                scheme_tls = true;
            } else if (protocol != "stratum+tcp" && protocol != "tcp") {
                throw std::invalid_argument("unsupported pool URL scheme: " + protocol);
            }
            address.erase(0, scheme + 3);
        }
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
        entry.tls = pool.value("tls", false) || scheme_tls;
        if (pool.contains("keepalive") && !pool["keepalive"].is_null()) {
            const auto& keepalive = pool["keepalive"];
            if (keepalive.is_boolean()) {
                entry.keepalive = keepalive.get<bool>();
            } else if (keepalive.is_number_integer()) {
                entry.keepalive = keepalive.get<std::int64_t>() != 0;
            } else {
                throw std::invalid_argument("pool.keepalive must be boolean or integer");
            }
        }
        std::string normalized_host = entry.host;
        std::transform(normalized_host.begin(), normalized_host.end(), normalized_host.begin(),
            [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
        // XMRig Pool::Pool detects NiceHash from either the explicit option
        // or the nicehash.com host suffix; preserve that behavior in C++.
        entry.nicehash = pool.value("nicehash", false) ||
            normalized_host.find("nicehash.com") != std::string::npos;
        entry.threads = cpu_threads;
        entry.cpu_affinities = select_cpu_affinities(
            config.value("cpu", nlohmann::json::object()));
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



// C++ adaptations of upstream Stratum protocol behavior:
// - RxMiner src/stratum.rs at 5fe52dde2650c623fd0472804152a94711889dce (MIT)
// - NyxRX crates/own-randomx-miner/src/stratum/protocol.rs at
//   d543ed59585596b917ac53cc3f60b91da1e9bb88 (MIT)
// The wire protocol is implemented natively in this single C++ engine;
// no Rust runtime or second miner process is involved.
nlohmann::json make_login_request(std::uint64_t id, const PoolConfig& pool,
                                  std::string_view agent) {
    nlohmann::json params = {
        {"login", pool.wallet},
        {"pass", pool.password},
        {"agent", std::string(agent)},
        {"algo", nlohmann::json::array({"rx/0"})}
    };
    if (!pool.worker.empty()) params["rigid"] = pool.worker;
    return {{"id", id}, {"jsonrpc", "2.0"}, {"method", "login"},
            {"params", std::move(params)}};
}

nlohmann::json make_submit_request(std::uint64_t id, std::string_view session_id,
                                   std::string_view job_id, std::string_view nonce_hex,
                                   std::string_view result_hex) {
    return {{"id", id}, {"jsonrpc", "2.0"}, {"method", "submit"},
            {"params", {{"id", std::string(session_id)},
                        {"job_id", std::string(job_id)},
                        {"nonce", std::string(nonce_hex)},
                        {"result", std::string(result_hex)}}}};
}

nlohmann::json make_keepalive_request(std::uint64_t id, std::string_view session_id) {
    return {{"id", id}, {"jsonrpc", "2.0"}, {"method", "keepalived"},
            {"params", {{"id", std::string(session_id)}}}};
}

bool is_job_notification(const nlohmann::json& message) noexcept {
    return message.is_object() && message.value("method", std::string()) == "job";
}

// NyxRX accepts both numeric and decimal-string JSON-RPC response IDs.
std::optional<std::uint64_t> parse_rpc_response_id(const nlohmann::json& message) noexcept {
    if (!message.is_object() || !message.contains("id")) return std::nullopt;
    const auto& id = message["id"];
    if (id.is_number_unsigned()) return id.get<std::uint64_t>();
    if (id.is_number_integer()) {
        const auto value = id.get<std::int64_t>();
        if (value >= 0) return static_cast<std::uint64_t>(value);
        return std::nullopt;
    }
    if (id.is_string()) {
        const auto& value = id.get_ref<const std::string&>();
        std::uint64_t parsed = 0;
        const auto result = std::from_chars(value.data(), value.data() + value.size(), parsed);
        if (!value.empty() && result.ec == std::errc{} &&
            result.ptr == value.data() + value.size()) return parsed;
    }
    return std::nullopt;
}

bool rpc_response_failed(const nlohmann::json& message) noexcept {
    if (!message.is_object()) return true;
    if (message.contains("error") && !message["error"].is_null()) return true;
    if (message.contains("result") && message["result"].is_object()) {
        const auto& result = message["result"];
        if (result.contains("status") && result["status"].is_string()) {
            const auto& status = result["status"].get_ref<const std::string&>();
            if (status.size() != 2 ||
                std::tolower(static_cast<unsigned char>(status[0])) != 'o' ||
                std::tolower(static_cast<unsigned char>(status[1])) != 'k') return true;
        }
    }
    return false;
}

// NyxRX protocol.rs format_error port: preserve pool-provided human-readable
// messages instead of logging opaque JSON objects.
std::string format_rpc_error(const nlohmann::json& error) {
    if (error.is_string()) return error.get<std::string>();
    if (error.is_array()) {
        std::string result;
        for (const auto& part : error) {
            if (!part.is_string()) continue;
            if (!result.empty()) result += ": ";
            result += part.get_ref<const std::string&>();
        }
        return result;
    }
    if (error.is_object()) {
        if (error.contains("message") && error["message"].is_string()) {
            return error["message"].get<std::string>();
        }
        return "unspecified error";
    }
    return error.dump();
}

bool login_advertises_nicehash(const nlohmann::json& result) noexcept {
    if (!result.is_object() || !result.contains("extensions") ||
        !result["extensions"].is_array()) return false;
    for (const auto& extension : result["extensions"]) {
        if (extension.is_string() && extension.get_ref<const std::string&>() == "nicehash")
            return true;
    }
    return false;
}

void apply_nicehash_extension(Job& job, bool enabled) noexcept {
    if (!enabled) return;
    const auto initial_nonce = decode_nonce_le(job.blob.data() + kNonceOffset);
    job.nonce_mask = 0x00ffffffU;
    job.nonce_fixed = initial_nonce & ~job.nonce_mask;
    job.nonce = initial_nonce & job.nonce_mask;
}

std::string validate_login_response(const nlohmann::json& message) {
    if (!message.is_object()) {
        throw std::invalid_argument("pool login response is not an object");
    }
    if (message.contains("error") && !message["error"].is_null()) {
        throw std::runtime_error("pool rejected login: " + format_rpc_error(message["error"]));
    }
    if (!message.contains("result") || !message["result"].is_object()) {
        throw std::runtime_error("pool login response has no result object");
    }
    const auto& result = message["result"];
    if (result.contains("status") && result["status"].is_string()) {
        std::string status = result["status"].get<std::string>();
        std::transform(status.begin(), status.end(), status.begin(),
            [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
        if (status != "ok") {
            throw std::runtime_error("pool returned unsuccessful login status: " + status);
        }
    }
    if (!result.contains("id") || !result["id"].is_string() ||
        result["id"].get_ref<const std::string&>().empty() ||
        !result.contains("job") || !result["job"].is_object()) {
        throw std::runtime_error("pool login response is missing session id or initial job");
    }
    return result["id"].get<std::string>();
}

} // namespace unique_rx
