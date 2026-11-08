#include <randomx.h>

#include <array>
#include <cstdint>
#include <iostream>
#include <memory>
#include <string_view>
#include <thread>

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
constexpr std::array<std::string_view, 4> inputs{
    "unique-rx-test-0", "unique-rx-test-1", "unique-rx-test-2", "unique-rx-test-3",
};

bool equal(const Hash& left, const Hash& right, const char* label) {
    if (left == right) return true;
    std::cerr << "Hash mismatch: " << label << '\n';
    return false;
}

} // namespace

bool run_mode(randomx_flags flags, const char* mode) {
    constexpr std::string_view key = "unique-rx-test-seed";
    Cache cache(randomx_alloc_cache(flags));
    if (!cache) {
        std::cerr << mode << ": RandomX cache allocation failed\n";
        return false;
    }
    randomx_init_cache(cache.get(), key.data(), key.size());

    Vm scalar_vm(randomx_create_vm(flags, cache.get(), nullptr));
    Vm batch_vm(randomx_create_vm(flags, cache.get(), nullptr));
    std::array<Vm, 2> lane_vms{
        Vm(randomx_create_vm(flags, cache.get(), nullptr)),
        Vm(randomx_create_vm(flags, cache.get(), nullptr)),
    };
    if (!scalar_vm || !batch_vm || !lane_vms[0] || !lane_vms[1]) {
        std::cerr << mode << ": RandomX VM creation failed\n";
        return false;
    }

    std::array<Hash, inputs.size()> scalar{};
    for (std::size_t index = 0; index < inputs.size(); ++index) {
        randomx_calculate_hash(scalar_vm.get(), inputs[index].data(),
                               inputs[index].size(), scalar[index].data());
    }

    std::array<Hash, inputs.size()> batch{};
    randomx_calculate_hash_first(batch_vm.get(), inputs[0].data(), inputs[0].size());
    for (std::size_t index = 1; index < inputs.size(); ++index) {
        randomx_calculate_hash_next(batch_vm.get(), inputs[index].data(),
                                    inputs[index].size(), batch[index - 1].data());
    }
    randomx_calculate_hash_last(batch_vm.get(), batch.back().data());
    for (std::size_t index = 0; index < inputs.size(); ++index) {
        if (!equal(scalar[index], batch[index], "scalar versus batch")) return false;
    }

    std::array<Hash, 2> lane_hashes{};
    std::array<std::jthread, 2> workers{
        std::thread([&] {
            randomx_calculate_hash(lane_vms[0].get(), inputs[0].data(),
                                   inputs[0].size(), lane_hashes[0].data());
        }),
        std::thread([&] {
            randomx_calculate_hash(lane_vms[1].get(), inputs[1].data(),
                                   inputs[1].size(), lane_hashes[1].data());
        }),
    };
    for (auto& worker : workers) worker.join();
    if (!equal(scalar[0], lane_hashes[0], "lane 0 versus scalar")) return false;
    if (!equal(scalar[1], lane_hashes[1], "lane 1 versus scalar")) return false;

    std::cout << mode << ": scalar, batch and two independent VM lanes match.\n";
    return true;
}

int main() {
    const auto available_flags = static_cast<randomx_flags>(
        static_cast<int>(randomx_get_flags()) &
        ~static_cast<int>(RANDOMX_FLAG_LARGE_PAGES));
    const auto interpreter_flags = static_cast<randomx_flags>(
        static_cast<int>(available_flags) & ~static_cast<int>(RANDOMX_FLAG_JIT));
    if (!run_mode(available_flags, "JIT") ||
        !run_mode(interpreter_flags, "interpreter")) {
        return 1;
    }
    return 0;
}