#include "msr/msr.hpp"

#include <iostream>

int main() {
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
