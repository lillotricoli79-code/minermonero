#include "msr/msr.hpp"
#include "msr/presets.hpp"

#include <iostream>

int main() {
    const auto intel = unique_rx::msr_preset_for_cpu("GenuineIntel", 6, 0x5e);
    if (!intel || intel->name != "intel_family_6" ||
        intel->writes.size() != 1 ||
        intel->writes[0].register_id != 0x1a4 ||
        intel->writes[0].value != 0x0f ||
        intel->writes[0].mask != 0x0f) {
        std::cerr << "Intel family-6 MSR preset selection failed\n";
        return 1;
    }
    if (!unique_rx::msr_preset_for_cpu("GenuineIntel", 6, 0x8e) ||
        !unique_rx::msr_preset_for_cpu("GenuineIntel", 6, 0x9e) ||
        unique_rx::msr_preset_for_cpu("GenuineIntel", 5, 0x04) ||
        unique_rx::msr_preset_for_cpu("GenuineIntel", 6, 0x97) ||
        unique_rx::msr_preset_for_cpu("AuthenticAMD", 6, 0x5e)) {
        std::cerr << "Intel family/model MSR selection failed\n";
        return 1;
    }

    for (const auto model : {0x31U, 0x60U, 0x68U, 0x71U}) {
        const auto zen = unique_rx::msr_preset_for_cpu("AuthenticAMD", 0x17, model);
        if (!zen || zen->name != "amd_zen_17h" || zen->writes.size() != 4) {
            std::cerr << "AMD Zen family-17 preset selection failed\n";
            return 1;
        }
    }
    for (const auto model : {0x00U, 0x0fU, 0x20U, 0x2fU, 0x50U, 0x5fU}) {
        const auto zen = unique_rx::msr_preset_for_cpu("AuthenticAMD", 0x19, model);
        if (!zen || zen->name != "amd_zen_19h" || zen->writes.size() != 4) {
            std::cerr << "AMD Zen family-19 preset selection failed\n";
            return 1;
        }
    }
    for (const auto model : {0x61U, 0x75U}) {
        const auto zen4 = unique_rx::msr_preset_for_cpu("AuthenticAMD", 0x19, model);
        if (!zen4 || zen4->name != "amd_zen4_19h" || zen4->writes.size() != 4) {
            std::cerr << "AMD Zen 4 preset selection failed\n";
            return 1;
        }
    }

    if (unique_rx::msr_preset_for_cpu("AuthenticAMD", 0x17, 0x01) ||
        unique_rx::msr_preset_for_cpu("AuthenticAMD", 0x19, 0x40) ||
        unique_rx::msr_preset_for_cpu("AuthenticAMD", 0x15, 0x02) ||
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
