#include "msr/msr.hpp"
#include "msr/presets.hpp"

#include <iostream>

int main() {
    const auto intel = unique_rx::msr_preset_for_cpu("GenuineIntel", 6, 0x5e);
    if (!intel || intel->name != "intel_skylake_s" || intel->writes.size() != 1 ||
        intel->writes[0].register_id != 0x1a4 ||
        intel->writes[0].value != 0x0f) {
        std::cerr << "Intel Skylake-S RandomX MSR preset selection failed\n";
        return 1;
    }

    if (unique_rx::msr_preset_for_cpu("GenuineIntel", 6, 0x9e) != std::nullopt ||
        unique_rx::msr_preset_for_cpu("GenuineIntel", 6, 0x8e) != std::nullopt ||
        unique_rx::msr_preset_for_cpu("GenuineIntel", 0x17, 0x31) != std::nullopt) {
        std::cerr << "Unverified Intel CPU received an MSR preset\n";
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
