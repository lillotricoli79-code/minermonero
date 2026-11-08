#pragma once

#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

namespace unique_rx {

// Adapted from NyxRX MSR preset selection (MIT; see LICENSES/NYXRX-MIT.txt).
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

    if (vendor == "GenuineIntel") {
        return MsrPreset{"intel", {write(0x1a4, 0x0f, all_bits)}};
    }
    if (vendor != "AuthenticAMD") return std::nullopt;

    if (family == 0x17) {
        return MsrPreset{"ryzen_17h", {
            write(0xc0011020, 0, all_bits),
            write(0xc0011021, 0x40, amd_mask),
            write(0xc0011022, 0x01510000, all_bits),
            write(0xc001102b, 0x2000cc16, all_bits),
        }};
    }
    if (family == 0x19 && (model == 0x61 || model == 0x75)) {
        return MsrPreset{"ryzen_19h_zen4", {
            write(0xc0011020, 0x0004400000000000, all_bits),
            write(0xc0011021, 0x0004000000000040, amd_mask),
            write(0xc0011022, 0x8680000401570000, all_bits),
            write(0xc001102b, 0x2040cc10, all_bits),
        }};
    }
    if (family == 0x19) {
        return MsrPreset{"ryzen_19h", {
            write(0xc0011020, 0x0004480000000000, all_bits),
            write(0xc0011021, 0x001c000200000040, amd_mask),
            write(0xc0011022, 0xc000000401570000, all_bits),
            write(0xc001102b, 0x2000cc10, all_bits),
        }};
    }
    if (family == 0x1a) {
        return MsrPreset{"ryzen_1ah_zen5", {
            write(0xc0011020, 0x0004400000000000, all_bits),
            write(0xc0011021, 0x0004000000000040, amd_mask),
            write(0xc0011022, 0x8680000401570000, all_bits),
            write(0xc001102b, 0x2040cc10, all_bits),
        }};
    }
    return std::nullopt;
}

} // namespace unique_rx
