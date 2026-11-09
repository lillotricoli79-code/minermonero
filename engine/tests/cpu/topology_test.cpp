#include "cpu/topology.hpp"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <string>

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
    using unique_rx::limit_cpu_threads_by_quota;
    expect(recommended_cpu_threads(8, 4, 0), 4, "SMT default");
    expect(recommended_cpu_threads(8, 8, 0), 8, "non-SMT default");
    expect(recommended_cpu_threads(128, 64, 0), 64, "large host default");
    expect(recommended_cpu_threads(8, 4, 50), 2, "50 percent physical-core hint");
    expect(recommended_cpu_threads(8, 4, 75), 3, "75 percent physical-core hint");
    expect(recommended_cpu_threads(8, 4, 100), 4, "100 percent physical-core hint");
    expect(recommended_cpu_threads(8, 4, 1), 1, "minimum hint");
    expect(recommended_cpu_threads(8, 4, 200), 4, "clamped physical-core hint");
    expect(recommended_cpu_threads(0, 0, 0), 1, "unknown topology");
    expect(recommended_cpu_threads(4, 16, 0), 4, "physical count clamped");
    expect(limit_cpu_threads_by_quota(8, 150000, 100000), 2, "fractional cgroup quota");
    expect(limit_cpu_threads_by_quota(8, 50000, 100000), 1, "sub-core cgroup quota");
    expect(limit_cpu_threads_by_quota(8, 800000, 100000), 8, "quota above request");
    expect(limit_cpu_threads_by_quota(8, 0, 100000), 8, "unlimited quota");
    expect(limit_cpu_threads_by_quota(8, 100000, 0), 8, "invalid quota period");

    const auto logical = unique_rx::detected_logical_threads();
    const auto array_threads = unique_rx::select_cpu_threads({
        {"enabled", true}, {"threads", nlohmann::json::array({0, 1})}
    });
    expect(array_threads, std::min(2U, logical), "XMRig array thread profile");
    const auto object_threads = unique_rx::select_cpu_threads({
        {"enabled", true}, {"threads", {{"threads", 3}, {"affinity", "0x0f"}, {"intensity", 1}}}
    });
    expect(object_threads, std::min(3U, logical), "XMRig object thread profile");

    const auto cpu = unique_rx::cpu_summary_json();
    if (!cpu.is_object() || cpu.value("vendor", std::string()).empty() ||
        cpu.value("brand", std::string()).empty() ||
        cpu.value("logical_threads", 0U) == 0 ||
        cpu.value("physical_cores", 0U) == 0 ||
        cpu.value("physical_cores", 0U) > cpu.value("logical_threads", 0U) ||
        !cpu.contains("features") || !cpu["features"].is_object()) {
        std::cerr << "CPU identity/topology/feature summary is incomplete.\n";
        return 1;
    }
    std::cout << "CPU topology and hardware-summary tests passed.\n";
    return 0;
}
