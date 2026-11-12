#include "msr/msr.hpp"
#include "msr/presets.hpp"
#include "msr/transaction.hpp"

#include <iostream>
#include <map>
#include <stdexcept>
#include <utility>
#include <vector>

namespace {
struct MockMsrBackend {
    std::map<std::pair<std::size_t, std::uint64_t>, std::uint64_t> registers;
    std::size_t writes = 0;
    std::size_t reads = 0;
    std::size_t fail_write_at = 0;
    std::size_t corrupt_read_at = 0;
    std::size_t mutate_unmasked_read_at = 0;
    bool mutate_unmasked_after_write = false;

    std::uint64_t read(std::size_t cpu, std::uint64_t reg) {
        ++reads;
        if (reads == mutate_unmasked_read_at) {
            registers[{cpu, reg}] ^= 0x40000000ULL;
        }
        const auto found = registers.find({cpu, reg});
        if (found == registers.end()) throw std::runtime_error("mock MSR is not initialized");
        return reads == corrupt_read_at ? found->second ^ 1ULL : found->second;
    }

    void write(std::size_t cpu, std::uint64_t reg, std::uint64_t value) {
        ++writes;
        if (fail_write_at != 0 && writes == fail_write_at) {
            throw std::runtime_error("injected mock MSR write failure");
        }
        registers[{cpu, reg}] = value;
        if (mutate_unmasked_after_write) {
            registers[{cpu, reg}] ^= 0x40000000ULL;
        }
    }
};

bool check(bool condition, const char* message) {
    if (!condition) std::cerr << message << '\n';
    return condition;
}

bool test_mock_msr_transactions() {
    using unique_rx::MsrWrite;
    using unique_rx::msr_detail::RegisterSnapshot;
    using unique_rx::msr_detail::apply_registers;
    using unique_rx::msr_detail::restore_registers;

    const std::vector<std::size_t> cpus{0, 1};
    const std::vector<MsrWrite> writes{
        {0x10, 0x05, 0x0f},
        {0x11, 0x123400, 0xffff00},
    };
    MockMsrBackend backend;
    const std::map<std::pair<std::size_t, std::uint64_t>, std::uint64_t> initial{
        {{0, 0x10}, 0xa5a50000}, {{0, 0x11}, 0x98765432},
        {{1, 0x10}, 0x5a5a0000}, {{1, 0x11}, 0xabcdef01},
    };
    backend.registers = initial;
    std::vector<RegisterSnapshot> snapshots;
    const auto read = [&](std::size_t cpu, std::uint64_t reg) {
        return backend.read(cpu, reg);
    };
    const auto write = [&](std::size_t cpu, std::uint64_t reg, std::uint64_t value) {
        backend.write(cpu, reg, value);
    };
    const auto log = [](const char*) {};

    apply_registers(cpus, writes, snapshots, read, write, log);
    bool ok = true;
    ok &= check(backend.read(0, 0x10) == 0xa5a50005, "masked MSR write failed on CPU 0");
    ok &= check(backend.read(1, 0x10) == 0x5a5a0005, "masked MSR write failed on CPU 1");
    ok &= check(backend.read(0, 0x11) == 0x98123432, "second masked MSR write failed on CPU 0");
    ok &= check(backend.read(1, 0x11) == 0xab123401, "second masked MSR write failed on CPU 1");

    // Simulate firmware/another process changing bits outside our masks
    // while the miner is active. Restore must not overwrite those unrelated bits.
    backend.registers[{0, 0x10}] ^= 0x40000000ULL;
    ok &= check(!restore_registers(snapshots, read, write, log),
                "successful mock MSR restore reported pending work");
    auto expected_after_restore = initial;
    expected_after_restore[{0, 0x10}] ^= 0x40000000ULL;
    ok &= check(backend.registers == expected_after_restore,
                "MSR restore overwrote bits outside the miner's write mask");

    // A successful backend WRMSR is authoritative, as in XMRig. Do not
    // require an additional RDMSR read-back after each write.
    backend.registers = initial;
    backend.writes = 0;
    backend.reads = 0;
    backend.mutate_unmasked_after_write = true;
    apply_registers(cpus, writes, snapshots, read, write, log);
    ok &= check(backend.reads == 8,
                "MSR apply performed unexpected post-write read-back operations");
    backend.mutate_unmasked_after_write = false;
    const std::map<std::pair<std::size_t, std::uint64_t>, std::uint64_t>
        expected_after_apply{
            {{0, 0x10}, 0xa5a50005ULL ^ 0x40000000ULL},
            {{0, 0x11}, 0x98123432ULL ^ 0x40000000ULL},
            {{1, 0x10}, 0x5a5a0005ULL ^ 0x40000000ULL},
            {{1, 0x11}, 0xab123401ULL ^ 0x40000000ULL},
        };
    ok &= check(backend.registers == expected_after_apply,
                "masked MSR write did not preserve unrelated concurrent register bits");
    ok &= check(!restore_registers(snapshots, read, write, log),
                "masked restore failed after unrelated register bits changed");
    auto expected_after_external_restore = initial;
    for (auto& entry : expected_after_external_restore) {
        entry.second ^= 0x40000000ULL;
    }
    ok &= check(backend.registers == expected_after_external_restore,
                "masked restore overwrote unrelated concurrent register bits");

    // Simulate an unrelated owner changing a register after the transaction
    // snapshot but before the miner applies its masked write.
    backend.registers = initial;
    backend.writes = 0;
    backend.reads = 0;
    backend.mutate_unmasked_read_at = 5; // four snapshots, then first apply read
    apply_registers(cpus, writes, snapshots, read, write, log);
    backend.mutate_unmasked_read_at = 0;
    const std::map<std::pair<std::size_t, std::uint64_t>, std::uint64_t>
        expected_after_concurrent_change{
            {{0, 0x10}, 0xa5a50005ULL ^ 0x40000000ULL},
            {{0, 0x11}, 0x98123432ULL},
            {{1, 0x10}, 0x5a5a0005ULL},
            {{1, 0x11}, 0xab123401ULL},
        };
    ok &= check(backend.registers == expected_after_concurrent_change,
                "MSR apply overwrote unrelated bits changed after the snapshot");
    ok &= check(!restore_registers(snapshots, read, write, log),
                "MSR restore failed after a concurrent pre-apply change");
    auto expected_after_concurrent_restore = initial;
    expected_after_concurrent_restore[{0, 0x10}] ^= 0x40000000ULL;
    ok &= check(backend.registers == expected_after_concurrent_restore,
                "MSR restore overwrote unrelated bits changed before apply");

    backend.registers = initial;
    backend.writes = 0;
    backend.reads = 0;
    backend.fail_write_at = 2;
    bool apply_failed = false;
    try {
        apply_registers(cpus, writes, snapshots, read, write, log);
    } catch (const std::exception&) {
        apply_failed = true;
    }
    ok &= check(apply_failed, "injected MSR write failure was not propagated");
    backend.fail_write_at = 0;
    ok &= check(backend.registers == initial,
                "partial mock MSR apply did not roll back all prior writes");

    backend.writes = 0;
    backend.reads = 0;
    apply_registers(cpus, writes, snapshots, read, write, log);
    backend.fail_write_at = backend.writes + 1;
    ok &= check(restore_registers(snapshots, read, write, log),
                "failed mock restore was not reported as pending");
    backend.fail_write_at = 0;
    ok &= check(!restore_registers(snapshots, read, write, log),
                "mock restore retry did not clear pending state");
    ok &= check(backend.registers == initial,
                "mock restore retry did not recover all original values");
    return ok;
}
} // namespace

int main() {
    if (!test_mock_msr_transactions()) return 1;
    const auto intel = unique_rx::msr_preset_for_cpu("GenuineIntel", 6, 0x5e);
    if (!intel || intel->name != "intel" ||
        intel->writes.size() != 1 ||
        intel->writes[0].register_id != 0x1a4 ||
        intel->writes[0].value != 0x0f ||
        intel->writes[0].mask != UINT64_MAX) {
        std::cerr << "Intel family-6 XMRig MSR preset selection failed\n";
        return 1;
    }
    if (!unique_rx::msr_preset_for_cpu("GenuineIntel", 6, 0x8e) ||
        !unique_rx::msr_preset_for_cpu("GenuineIntel", 6, 0x9e) ||
        !unique_rx::msr_preset_for_cpu("GenuineIntel", 6, 0x97) ||
        unique_rx::msr_preset_for_cpu("GenuineIntel", 5, 0x04) ||
        unique_rx::msr_preset_for_cpu("AuthenticAMD", 6, 0x5e)) {
        std::cerr << "Intel family/model MSR selection diverged from XMRig\n";
        return 1;
    }

    for (const auto model : {0x01U, 0x31U, 0x60U, 0x68U, 0x71U}) {
        const auto zen = unique_rx::msr_preset_for_cpu("AuthenticAMD", 0x17, model);
        if (!zen || zen->name != "ryzen_17h" || zen->writes.size() != 4 ||
            zen->writes[0].register_id != 0xc0011020 ||
            zen->writes[0].value != 0 ||
            zen->writes[1].register_id != 0xc0011021 ||
            zen->writes[1].value != 0x40 ||
            zen->writes[1].mask != ~std::uint64_t{0x20} ||
            zen->writes[2].value != 0x1510000 ||
            zen->writes[3].value != 0x2000cc16) {
            std::cerr << "AMD family-17 XMRig MSR preset mismatch\n";
            return 1;
        }
    }

    for (const auto model : {0x00U, 0x0fU, 0x20U, 0x2fU, 0x40U, 0x50U, 0x5fU, 0x80U}) {
        const auto zen = unique_rx::msr_preset_for_cpu("AuthenticAMD", 0x19, model);
        if (!zen || zen->name != "ryzen_19h" || zen->writes.size() != 4 ||
            zen->writes[0].register_id != 0xc0011020 ||
            zen->writes[0].value != 0x0004480000000000ULL ||
            zen->writes[1].register_id != 0xc0011021 ||
            zen->writes[1].value != 0x001c000200000040ULL ||
            zen->writes[1].mask != ~std::uint64_t{0x20} ||
            zen->writes[2].register_id != 0xc0011022 ||
            zen->writes[2].value != 0xc000000401570000ULL ||
            zen->writes[3].register_id != 0xc001102b ||
            zen->writes[3].value != 0x2000cc10ULL) {
            std::cerr << "AMD family-19 Zen 3 XMRig MSR preset mismatch\n";
            return 1;
        }
    }

    for (const auto model : {0x61U, 0x75U}) {
        const auto zen4 = unique_rx::msr_preset_for_cpu("AuthenticAMD", 0x19, model);
        if (!zen4 || zen4->name != "ryzen_19h_zen4" ||
            zen4->writes.size() != 4 ||
            zen4->writes[0].value != 0x0004400000000000ULL ||
            zen4->writes[1].value != 0x0004000000000040ULL ||
            zen4->writes[1].mask != ~std::uint64_t{0x20} ||
            zen4->writes[2].value != 0x8680000401570000ULL ||
            zen4->writes[3].value != 0x2040cc10ULL) {
            std::cerr << "AMD family-19 Zen 4 XMRig MSR preset mismatch\n";
            return 1;
        }
    }

    const auto zen5 = unique_rx::msr_preset_for_cpu("AuthenticAMD", 0x1a, 0x01);
    if (!zen5 || zen5->name != "ryzen_1ah_zen5" ||
        zen5->writes.size() != 4 ||
        zen5->writes[0].value != 0x0004400000000000ULL ||
        zen5->writes[1].value != 0x0004000000000040ULL ||
        zen5->writes[1].mask != ~std::uint64_t{0x20} ||
        zen5->writes[2].value != 0x8680000401570000ULL ||
        zen5->writes[3].value != 0x2040cc10ULL) {
        std::cerr << "AMD family-1Ah Zen 5 XMRig MSR preset mismatch\n";
        return 1;
    }

    if (unique_rx::msr_preset_for_cpu("AuthenticAMD", 0x15, 0x02) ||
        unique_rx::msr_preset_for_cpu("UnknownVendor", 6, 0x5e)) {
        std::cerr << "Unsupported CPU received an MSR preset\n";
        return 1;
    }

    unique_rx::MsrManager manager;
    if (manager.active()) return 1;
    manager.restore();
    if (manager.active()) return 1;
    manager.restore();
    if (manager.active()) return 1;
    return 0;
}
