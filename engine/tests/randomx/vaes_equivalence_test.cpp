// VAES fast-path equivalence checks against the portable RandomX AES path.
// The test invokes VAES only after RandomX's runtime CPU/OS capability check.
#include "aes_hash.hpp"
#include "cpu.hpp"

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace {
void require(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "VAES equivalence failure: %s\n", message);
        std::abort();
    }
}

struct AlignedBuffer {
    std::vector<std::uint8_t> storage;
    std::uint8_t* data;
    explicit AlignedBuffer(std::size_t bytes)
        : storage(bytes + 63),
          data(reinterpret_cast<std::uint8_t*>((reinterpret_cast<std::uintptr_t>(storage.data()) + 63) & ~std::uintptr_t(63))) {}
};

void fill_pattern(std::uint8_t* data, std::size_t size, std::uint8_t seed) {
    for (std::size_t i = 0; i < size; ++i)
        data[i] = static_cast<std::uint8_t>((i * 37u + (i >> 7) + seed * 19u) & 0xffu);
}

void check_fill_aes1(std::size_t size) {
    AlignedBuffer expected(size), actual(size);
    alignas(64) std::array<std::uint8_t, 64> expected_state{}, actual_state{};
    fill_pattern(expected_state.data(), expected_state.size(), 11);
    actual_state = expected_state;
    fillAes1Rx4<true>(expected_state.data(), size, expected.data);
    rxm_fillAes1Rx4_vaes(actual_state.data(), size, actual.data);
    require(expected_state == actual_state, "fillAes1 state");
    require(std::memcmp(expected.data, actual.data, size) == 0, "fillAes1 output");
}

void check_fill_aes4(std::size_t size) {
    AlignedBuffer expected(size), actual(size);
    alignas(64) std::array<std::uint8_t, 64> state{};
    fill_pattern(state.data(), state.size(), 29);
    fillAes4Rx4<true>(state.data(), size, expected.data);
    rxm_fillAes4Rx4_vaes(state.data(), size, actual.data);
    require(std::memcmp(expected.data, actual.data, size) == 0, "fillAes4 output");
}

void check_hash_and_fill(std::size_t size) {
    AlignedBuffer expected(size), actual(size);
    alignas(64) std::array<std::uint8_t, 64> expected_state{}, actual_state{};
    alignas(64) std::array<std::uint8_t, 64> expected_hash{}, actual_hash{};
    fill_pattern(expected.data, size, 41);
    std::memcpy(actual.data, expected.data, size);
    fill_pattern(expected_state.data(), expected_state.size(), 53);
    actual_state = expected_state;

    hashAndFillAes1Rx4<true>(expected.data, size, expected_hash.data(), expected_state.data());
    rxm_hashAndFillAes1Rx4_vaes(actual.data, size, actual_hash.data(), actual_state.data());

    require(expected_hash == actual_hash, "hashAndFill hash");
    require(expected_state == actual_state, "hashAndFill state");
    require(std::memcmp(expected.data, actual.data, size) == 0, "hashAndFill scratchpad");
}
} // namespace

int main() {
    if (!randomx::cpu.hasVaes()) {
        std::puts("SKIPPED VAES equivalence: CPU/OS VAES state is unavailable");
        return 0;
    }

    for (const std::size_t size : {4096u, 8192u, 65536u, 2097152u}) {
        check_fill_aes1(size);
        check_fill_aes4(size);
        check_hash_and_fill(size);
    }
    std::puts("PASSED VAES vs portable AES equivalence (all buffers, states and hashes)");
    return 0;
}
