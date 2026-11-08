#include "stratum/stratum.hpp"
#include "cpu/topology.hpp"

#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <algorithm>

namespace {

bool check(bool value, const char* message) {
    if (!value) std::cerr << message << '\n';
    return value;
}

template <typename Function>
bool rejects(Function&& function, const char* message) {
    try {
        function();
    } catch (const std::exception&) {
        return true;
    }
    std::cerr << message << '\n';
    return false;
}

nlohmann::json base_pool_config() {
    return {
        {"pools", nlohmann::json::array({nlohmann::json{
            {"url", "pool.example.net:3333"},
            {"user", "test-wallet"},
        }})},
    };
}

} // namespace

int main() {
    using namespace unique_rx;
    bool ok = true;

    // Pool target and hexadecimal decoding.
    ok &= check(parse_share_target("ffffffff") == UINT64_MAX,
                "4-byte little-endian target normalization failed");
    ok &= check(parse_share_target("00000080") ==
                    UINT64_MAX / (UINT32_MAX / 0x80000000ULL),
                "4-byte target normalization diverged from pool target semantics");
    ok &= check(parse_share_target("01000000") ==
                    UINT64_MAX / (UINT32_MAX / 1ULL),
                "4-byte target normalization failed for the minimum threshold");
    ok &= check(parse_share_target("0100000000000000") == 1,
                "8-byte little-endian target parsing failed");
    ok &= check(decode_hex("00aF10") == std::vector<std::uint8_t>{0x00, 0xaf, 0x10},
                "hexadecimal decoding failed");
    ok &= rejects([] { decode_hex("abc"); },
                  "odd-length hexadecimal input was accepted");
    ok &= rejects([] { decode_hex("0g"); },
                  "non-hexadecimal input was accepted");
    ok &= rejects([] { parse_share_target("00000000"); },
                  "zero pool target was accepted");
    ok &= rejects([] { parse_share_target("1234"); },
                  "unsupported target length was accepted");

    // Job shape, algorithm and seed validation.
    nlohmann::json wire_job{
        {"job_id", "job-1"},
        {"blob", std::string(76 * 2, '0')},
        {"seed_hash", std::string(64, '0')},
        {"target", "ffffffff"},
        {"height", 2345678},
        {"algo", "rx/0"},
        {"nicehash", false},
    };
    wire_job["blob"] = std::string(kNonceOffset * 2, '0') +
        "78563412" + std::string((76 - kNonceOffset - 4) * 2, '0');
    const auto job = parse_job(wire_job);
    ok &= check(job.id == "job-1" && job.blob.size() == 76 &&
                    job.target == UINT64_MAX && job.nonce == 0x12345678U &&
                    job.height == std::optional<std::uint64_t>{2345678},
                "valid rx/0 job or little-endian nonce did not parse");
    auto nicehash_job = wire_job;
    nicehash_job["nicehash"] = true;
    nicehash_job["blob"] = std::string(kNonceOffset * 2, '0') +
        "000000ab" + std::string((76 - kNonceOffset - 4) * 2, '0');
    const auto parsed_nicehash = parse_job(nicehash_job);
    ok &= check(parsed_nicehash.nonce == 0 &&
                    parsed_nicehash.nonce_fixed == 0xab000000U &&
                    parsed_nicehash.nonce_mask == 0x00ffffffU &&
                    nonce_at_offset(parsed_nicehash, 0x00ffffffULL) == 0xabffffffU &&
                    nonce_at_offset(parsed_nicehash, 0x01000000ULL) == 0xab000000U,
                "NiceHash nonce allocation did not preserve the pool prefix or wrap its 24-bit counter");
    auto inferred_nicehash = nicehash_job;
    inferred_nicehash.erase("nicehash");
    ok &= check(parse_job(inferred_nicehash).nonce_fixed == 0xab000000U,
                "non-zero pool nonce prefix was not detected when NiceHash metadata was absent");
    auto ordinary_nonce_job = wire_job;
    ordinary_nonce_job.erase("nicehash");
    ok &= check(parse_job(ordinary_nonce_job).nonce == 0x12345678U &&
                    parse_job(ordinary_nonce_job).nonce_mask == UINT32_MAX,
                "ordinary explicit 32-bit nonce range changed unexpectedly");

    auto job_without_height = wire_job;
    job_without_height.erase("height");
    ok &= check(!parse_job(job_without_height).height.has_value(),
                "missing pool height was reported as a real block height");
    auto invalid_job = wire_job;
    invalid_job["height"] = -1;
    ok &= rejects([&] { parse_job(invalid_job); },
                  "negative block height was accepted");
    invalid_job = wire_job;
    invalid_job["height"] = 1.5;
    ok &= rejects([&] { parse_job(invalid_job); },
                  "fractional block height was accepted");
    ok &= check(encode_nonce_le(0x12345678U) ==
                    std::array<std::uint8_t, 4>{0x78, 0x56, 0x34, 0x12},
                "little-endian nonce encoding failed");
    ok &= check(nonce_at_offset(0xfffffffeU, 0) == 0xfffffffeU &&
                    nonce_at_offset(0xfffffffeU, 1) == 0xffffffffU &&
                    nonce_at_offset(0xfffffffeU, 2) == 0U &&
                    nonce_at_offset(0xfffffffeU, 3) == 1U,
                "nonce allocation failed to wrap across the full 32-bit pool nonce space");

    auto target_update = job;
    target_update.target /= 2;
    ok &= check(job.same_work(target_update),
                "target-only update incorrectly changed job identity");
    auto changed_job_id = job;
    changed_job_id.id = "job-2";
    ok &= check(!job.same_work(changed_job_id),
                "different job identifier was treated as the same work");
    auto changed_blob = job;
    changed_blob.blob[0] = 1;
    ok &= check(!job.same_work(changed_blob),
                "different job blob was treated as the same work");
    auto changed_seed = job;
    changed_seed.seed[0] = 1;
    ok &= check(!job.same_work(changed_seed),
                "different RandomX seed was treated as the same work");
    invalid_job = wire_job;
    invalid_job["blob"] = "00";
    ok &= rejects([&] { parse_job(invalid_job); },
                  "short job blob was accepted");
    invalid_job = wire_job;
    invalid_job["algo"] = "cn/r";
    ok &= rejects([&] { parse_job(invalid_job); },
                  "unsupported job algorithm was accepted");
    invalid_job = wire_job;
    invalid_job["job_id"] = "";
    ok &= rejects([&] { parse_job(invalid_job); },
                  "empty job identifier was accepted");
    invalid_job = wire_job;
    invalid_job["job_id"] = std::string(257, 'j');
    ok &= rejects([&] { parse_job(invalid_job); },
                  "oversized job identifier was accepted");
    invalid_job = wire_job;
    invalid_job["target"] = std::string(1024 * 1024, 'f');
    ok &= rejects([&] { parse_job(invalid_job); },
                  "oversized share target was accepted");
    invalid_job = wire_job;
    invalid_job["seed_hash"] = "00";
    ok &= rejects([&] { parse_job(invalid_job); },
                  "incorrect seed hash length was accepted");
    invalid_job = wire_job;
    invalid_job["blob"] = std::string((kMaximumBlobSize + 1) * 2, '0');
    ok &= rejects([&] { parse_job(invalid_job); },
                  "oversized job blob was accepted");

    // CPU topology selection and safety bounds.
    ok &= check(select_cpu_threads(nlohmann::json{{"enabled", false}}) == 0,
                "disabled CPU configuration still selected mining threads");
    const auto full_cpu_threads = select_cpu_threads(
        nlohmann::json{{"enabled", true}, {"max-threads-hint", 100}});
    ok &= check(full_cpu_threads >= 1 && full_cpu_threads <= std::max(1U, std::thread::hardware_concurrency()),
                "CPU topology selection exceeded supported thread bounds");
    const auto minimum_cpu_threads = select_cpu_threads(
        nlohmann::json{{"enabled", true}, {"max-threads-hint", 1}});
    ok &= check(minimum_cpu_threads >= 1 && minimum_cpu_threads <= full_cpu_threads,
                "CPU low-thread hint did not stay within selected topology bounds");

    // Pool URL parsing, defaults, IPv6 and TLS fingerprint validation.
    auto config = base_pool_config();
    config["cpu"] = {{"enabled", false}};
    ok &= rejects([&] { parse_pool_configs(config); },
                  "disabled CPU mining configuration was accepted");
    config = base_pool_config();
    const auto pools = parse_pool_configs(config);
    const auto& pool = pools.front();
    ok &= check(pools.size() == 1 && pool.host == "pool.example.net" &&
                    pool.port == "3333" && pool.wallet == "test-wallet" &&
                    pool.password == "x" && pool.worker == "rig01" &&
                    !pool.tls && pool.threads > 0,
                "pool configuration defaults or host parsing failed");

    config["pools"][0]["keepalive"] = true;
    const auto keepalive_pools = parse_pool_configs(config);
    ok &= check(keepalive_pools.front().keepalive,
                "per-pool keepalive setting was not parsed");
    config = base_pool_config();

    config["pools"][0]["url"] = "[::1]:4444";
    const auto ipv6_pools = parse_pool_configs(config);
    const auto& ipv6_pool = ipv6_pools.front();
    ok &= check(ipv6_pool.host == "::1" && ipv6_pool.port == "4444",
                "bracketed IPv6 pool URL parsing failed");

    config["pools"][0]["tls"] = true;
    config["pools"][0]["tls-fingerprint"] =
        "AA:BB:CC:DD:EE:FF:00:11:22:33:44:55:66:77:88:99:"
        "AA:BB:CC:DD:EE:FF:00:11:22:33:44:55:66:77:88:99";
    const auto pinned_pools = parse_pool_configs(config);
    const auto& pinned_pool = pinned_pools.front();
    ok &= check(pinned_pool.tls &&
                    pinned_pool.tls_fingerprint ==
                        "aabbccddeeff00112233445566778899"
                        "aabbccddeeff00112233445566778899",
                "TLS fingerprint normalization failed");

    config["pools"][0]["tls"] = false;
    ok &= rejects([&] { parse_pool_configs(config); },
                  "TLS fingerprint without TLS was accepted");
    config = base_pool_config();
    config["pools"][0]["url"] = "missing-port";
    ok &= rejects([&] { parse_pool_configs(config); },
                  "pool URL without port was accepted");
    config = base_pool_config();
    config["pools"][0]["enabled"] = false;
    config["pools"].push_back({
        {"url", "backup.example.net:4444"}, {"user", "backup-wallet"},
        {"enabled", true},
    });
    const auto failover_pools = parse_pool_configs(config);
    ok &= check(failover_pools.size() == 1 &&
                    failover_pools.front().host == "backup.example.net" &&
                    failover_pools.front().wallet == "backup-wallet",
                "disabled pool was not skipped in configured failover order");

    config["pools"][1]["enabled"] = false;
    ok &= rejects([&] { parse_pool_configs(config); },
                  "configuration with no enabled pools was accepted");

    config = base_pool_config();
    config["pools"][0]["user"] = "";
    ok &= rejects([&] { parse_pool_configs(config); },
                  "empty pool wallet was accepted");

    config = base_pool_config();
    config["pools"] = nlohmann::json::array();
    ok &= rejects([&] { parse_pool_configs(config); },
                  "empty pool list was accepted");

    if (!ok) return 1;
    std::cout << "Stratum target, job and pool configuration tests passed.\n";
    return 0;
}
