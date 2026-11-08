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

    MiningState state;
    unique_rx::core::hash_worker(state, 0, 0, 0);
    ok &= check(state.stop.load(), "zero worker count did not stop the miner");
    ok &= check(has_error(state, expected_error), "zero worker count error was not reported");

    MiningState invalid_index_state;
    unique_rx::core::hash_worker(invalid_index_state, 2, 2, 0);
    ok &= check(invalid_index_state.stop.load(), "out-of-range worker index did not stop the miner");
    ok &= check(has_error(invalid_index_state, expected_error),
                 "out-of-range worker index error was not reported");

    return ok ? 0 : 1;
}
