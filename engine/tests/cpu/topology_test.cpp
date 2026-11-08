#include "cpu/topology.hpp"

#include <cstdlib>
#include <iostream>

namespace {
void expect(std::uint32_t actual, std::uint32_t expected, const char* name) {
    if (actual != expected) {
        std::cerr << name << ": expected " << expected << ", got " << actual << '\n';
        std::exit(1);
    }
}
}

int main() {
    using unique_rx::recommended_cpu_threads;
    expect(recommended_cpu_threads(8, 4, 0), 4, "SMT default");
    expect(recommended_cpu_threads(8, 8, 0), 8, "non-SMT default");
    expect(recommended_cpu_threads(128, 64, 0), 64, "large host default");
    expect(recommended_cpu_threads(8, 4, 50), 4, "50 percent hint");
    expect(recommended_cpu_threads(8, 4, 100), 8, "100 percent hint");
    expect(recommended_cpu_threads(8, 4, 1), 1, "minimum hint");
    expect(recommended_cpu_threads(8, 4, 200), 8, "clamped hint");
    expect(recommended_cpu_threads(0, 0, 0), 1, "unknown topology");
    expect(recommended_cpu_threads(4, 16, 0), 4, "physical count clamped");
    std::cout << "CPU topology thread-selection tests passed.\n";
    return 0;
}
