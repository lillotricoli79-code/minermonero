#include "core/mining.hpp"

#include <iostream>
#include <string>

namespace {

bool check(bool condition, const char* message) {
    if (!condition) std::cerr << message << '\n';
    return condition;
}

bool has_error(unique_rx::core::MiningState& state, const std::string& expected) {
    std::lock_guard lock(state.error_mutex);
    return state.worker_error == expected;
}

} // namespace

int main() {
    using unique_rx::core::MiningState;
    constexpr const char* expected_error = "invalid CPU worker index or worker count";
    bool ok = true;
    unique_rx::core::Hash target_hash{};
    target_hash[31] = 0x01;
    ok &= check(unique_rx::core::hash_meets_target(target_hash, 0x0100000000000001ULL),
                "RandomX target comparison did not decode hash bytes as little-endian");
    ok &= check(!unique_rx::core::hash_meets_target(target_hash, 0x0100000000000000ULL),
                "RandomX target comparison accepted a hash equal to target");
    target_hash.fill(0);
    target_hash[24] = 0x02;
    ok &= check(unique_rx::core::hash_meets_target(target_hash, 3),
                "RandomX target comparison rejected a lower little-endian hash");

    MiningState metrics_state;
    metrics_state.record_accepted_share(250.0, 100.0);
    metrics_state.record_accepted_share(200.0, 100.0);
    const auto best = metrics_state.best_snapshot();
    ok &= check(best.estimated_hashes == 200.0,
                "accepted-share estimated work does not accumulate pool target difficulty");
    ok &= check(best.top_difficulties.size() == 2 &&
                best.top_difficulties[0].difficulty == 250.0 &&
                best.top_difficulties[1].difficulty == 200.0,
                "best-share telemetry does not retain descending actual difficulty");
    ok &= check(!best.top_difficulties.empty() &&
                best.top_difficulties[0].at_unix_seconds > 0,
                "best-share telemetry is missing its Unix timestamp");

    MiningState top_ten_state;
    for (std::uint64_t difficulty = 1; difficulty <= 12; ++difficulty) {
        top_ten_state.record_accepted_share(static_cast<double>(difficulty), 100.0);
    }
    const auto top_ten = top_ten_state.best_snapshot();
    ok &= check(top_ten.top_difficulties.size() == 10 &&
                top_ten.top_difficulties.front().difficulty == 12.0 &&
                top_ten.top_difficulties.back().difficulty == 3.0,
                "best-share telemetry does not retain the ten highest difficulties");

    MiningState state;
    std::atomic<bool> worker_stop{false};
    unique_rx::core::hash_worker(state, 0, 0, 0, worker_stop);
    ok &= check(state.stop.load(), "zero worker count did not stop the miner");
    ok &= check(has_error(state, expected_error), "zero worker count error was not reported");

    MiningState invalid_index_state;
    std::atomic<bool> invalid_worker_stop{false};
    unique_rx::core::hash_worker(invalid_index_state, 2, 2, 0, invalid_worker_stop);
    ok &= check(invalid_index_state.stop.load(), "out-of-range worker index did not stop the miner");
    ok &= check(has_error(invalid_index_state, expected_error),
                 "out-of-range worker index error was not reported");

    return ok ? 0 : 1;
}
