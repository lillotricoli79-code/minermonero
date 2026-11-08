#include "msr/msr.hpp"
#include "msr/presets.hpp"

#include <array>
#include <iostream>

int main() {
    // Intel mainstream Core/Xeon generations: the preset must not be
    // restricted to the developer's i7-6700K model.
    constexpr std::array<std::uint32_t, 12> intel_models{
        0x1a, 0x2a, 0x3a, 0x3c, 0x4e, 0x5e,
        0x8e, 0x9e, 0x97, 0x9a, 0xb7, 0xc5
    };
    for (const auto model : intel_models) {
        const auto intel = unique_rx::msr_preset_for_cpu("GenuineIntel", 6, model);
        if (!intel || intel->name != "intel" || intel->writes.size() != 1 ||
            intel->writes[0].register_id != 0x1a4 ||
            intel->writes[0].value != 0x0f || intel->writes[0].mask != 0x0f) {
            std::cerr << "Intel family 6 MSR preset selection failed for model "
                      << std::hex << model << "\n";
            return 1;
        }
    }

    if (unique_rx::msr_preset_for_cpu("GenuineIntel", 5, 0x04) != std::nullopt ||
        unique_rx::msr_preset_for_cpu("GenuineIntel", 0x0f, 0x03) != std::nullopt ||
        unique_rx::msr_preset_for_cpu("AuthenticAMD", 6, 0x5e) != std::nullopt) {
        std::cerr << "Unsupported Intel/AMD family received Intel MSR preset\n";
        return 1;
    }

    const auto zen1 = unique_rx::msr_preset_for_cpu("AuthenticAMD", 0x17, 0x31);
    const auto zen3 = unique_rx::msr_preset_for_cpu("AuthenticAMD", 0x19, 0x21);
    const auto zen4 = unique_rx::msr_preset_for_cpu("AuthenticAMD", 0x19, 0x61);
    const auto zen5 = unique_rx::msr_preset_for_cpu("AuthenticAMD", 0x1a, 0x44);
    if (!zen1 || zen1->name != "ryzen_17h" || zen1->writes.size() != 4 ||
        !zen3 || zen3->name != "ryzen_19h" || zen3->writes.size() != 4 ||
        !zen4 || zen4->name != "ryzen_19h_zen4" || zen4->writes.size() != 4 ||
        !zen5 || zen5->name != "ryzen_1ah_zen5" || zen5->writes.size() != 4) {
        std::cerr << "AMD RandomX MSR preset selection failed\n";
        return 1;
    }

    if (unique_rx::msr_preset_for_cpu("AuthenticAMD", 0x15, 0x02) != std::nullopt ||
        unique_rx::msr_preset_for_cpu("UnknownVendor", 6, 0x5e) != std::nullopt) {
        std::cerr << "Unsupported CPU MSR preset selection failed\n";
        return 1;
    }

    unique_rx::MsrManager manager;
    if (manager.active()) {
        std::cerr << "MSR manager must start inactive\n";
        return 1;
    }
    manager.restore();
    if (manager.active()) {
        std::cerr << "MSR manager became active after an empty restore\n";
        return 1;
    }
    manager.restore();
    if (manager.active()) {
        std::cerr << "MSR manager restore is not idempotent\n";
        return 1;
    }
    return 0;
}
