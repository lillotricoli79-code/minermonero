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
                          std::uint64_t mask = UINT64_MAX) {
        return MsrWrite{reg, value, mask};
    };

    // Match XMRig v6.26.0 MSR_MOD_INTEL: all Intel family-6 CPUs use
    // the 0x1a4 RandomX prefetcher preset.
    if (vendor == "GenuineIntel") {
        if (family != 0x06) return std::nullopt;
        return MsrPreset{"intel", {write(0x1a4, 0x0f, all_bits)}};
    }

    if (vendor != "AuthenticAMD") return std::nullopt;

    // XMRig v6.26.0 MSR_MOD_RYZEN_17H.
    if (family == 0x17) {
        return MsrPreset{"ryzen_17h", {
            write(0xc0011020, 0x0000000000000000, all_bits),
            write(0xc0011021, 0x0000000000000040, amd_mask),
            write(0xc0011022, 0x0000000001510000, all_bits),
            write(0xc001102b, 0x000000002000cc16, all_bits),
        }};
    }

    // XMRig v6.26.0 distinguishes the Zen 4 models in family 19h.
    if (family == 0x19 && (model == 0x61 || model == 0x75)) {
        return MsrPreset{"ryzen_19h_zen4", {
            write(0xc0011020, 0x0004400000000000, all_bits),
            write(0xc0011021, 0x0004000000000040, amd_mask),
            write(0xc0011022, 0x8680000401570000, all_bits),
            write(0xc001102b, 0x000000002040cc10, all_bits),
        }};
    }

    // Remaining family-19h processors use the XMRig Zen 3 preset.
    if (family == 0x19) {
        return MsrPreset{"ryzen_19h", {
            write(0xc0011020, 0x0004480000000000, all_bits),
            write(0xc0011021, 0x001c000200000040, amd_mask),
            write(0xc0011022, 0xc000000401570000, all_bits),
            write(0xc001102b, 0x000000002000cc10, all_bits),
        }};
    }

    // XMRig v6.26.0 MSR_MOD_RYZEN_1AH_ZEN5.
    if (family == 0x1a) {
        return MsrPreset{"ryzen_1ah_zen5", {
            write(0xc0011020, 0x0004400000000000, all_bits),
            write(0xc0011021, 0x0004000000000040, amd_mask),
            write(0xc0011022, 0x8680000401570000, all_bits),
            write(0xc001102b, 0x000000002040cc10, all_bits),
        }};
    }

    return std::nullopt;
}

} // namespace unique_rx
