#pragma once

#include <cstdint>
#include <memory>
#include <optional>

namespace unique_rx::telemetry {

struct SystemSample {
    std::optional<double> system_cpu_percent;
    std::optional<double> process_cpu_percent;
    std::optional<std::uint64_t> cpu_frequency_mhz;
    std::optional<std::uint64_t> memory_used_bytes;
    std::optional<std::uint64_t> process_memory_bytes;
    std::optional<double> temperature_c;
    std::optional<double> package_power_watts;
    std::optional<bool> on_battery;
};

class SystemTelemetry {
public:
    SystemTelemetry();
    ~SystemTelemetry();
    SystemTelemetry(SystemTelemetry&&) noexcept;
    SystemTelemetry& operator=(SystemTelemetry&&) noexcept;
    SystemTelemetry(const SystemTelemetry&) = delete;
    SystemTelemetry& operator=(const SystemTelemetry&) = delete;
    SystemSample sample();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace unique_rx::telemetry
