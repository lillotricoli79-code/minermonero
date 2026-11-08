#pragma once

#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

namespace unique_rx {

struct MsrWrite {
    std::uint64_t register_id;
    std::uint64_t value;
    std::uint64_t mask;
};

struct MsrPreset {
    std::string_view name;
    std::vector<MsrWrite> writes;
};

inline std::optional<MsrPreset> msr_preset_for_cpu(
    std::string_view vendor, std::uint32_t family, std::uint32_t model) {
    constexpr std::uint64_t all_bits = UINT64_MAX;
    constexpr std::uint64_t amd_mask = ~std::uint64_t{0x20};
    const auto write = [](std::uint64_t reg, std::uint64_t value,
                          std::uint64_t mask) {
        return MsrWrite{reg, value, mask};
    };

    // Intel's RandomX prefetcher control is the 0x1a4 register.
    // Only touch the four documented prefetcher-disable bits.
    if (vendor == "GenuineIntel") {
        if (family != 0x06 ||
            (model != 0x5e && model != 0x8e && model != 0x9e)) {
            return std::nullopt;
        }
        return MsrPreset{"intel_family_6", {write(0x1a4, 0x0f, 0x0f)}};
    }

    if (vendor != "AuthenticAMD") return std::nullopt;

    // AMD Zen/Zen+ models with a verified RandomX MSR profile.
    if (family == 0x17 &&
        (model == 0x31 || model == 0x60 || model == 0x68 || model == 0x71)) {
        return MsrPreset{"amd_zen_17h", {
            write(0xc0011020, 0x0000000000000000, all_bits),
            write(0xc0011021, 0x0000000000000040, amd_mask),
            write(0xc0011022, 0x0000000001510000, all_bits),
            write(0xc001102b, 0x000000002000cc16, all_bits),
        }};
    }

    // AMD Zen 3 family 19h profile ported from RxMiner's MIT-licensed
    // src/msr.rs at 5fe52dde2650c623fd0472804152a94711889dce.
    // Keep the exact allow-list and register values from that implementation.
    if (family == 0x19 &&
        (model == 0x00 || model == 0x0f || model == 0x20 ||
         model == 0x2f || model == 0x50 || model == 0x5f)) {
        return MsrPreset{"amd_zen_19h", {
            write(0xc0011020, 0x0004480000000000, all_bits),
            write(0xc0011021, 0x001c000200000040, amd_mask),
            write(0xc0011022, 0xc000000401500000, all_bits),
            write(0xc001102b, 0x000000002000cc14, all_bits),
        }};
    }
    return std::nullopt;
}

} // namespace unique_rx
