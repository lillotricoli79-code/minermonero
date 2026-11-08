#pragma once

#include "stratum.hpp"

#include <memory>
#include <string>
#include <vector>

namespace unique_rx {

class StratumTransport {
public:
    StratumTransport();
    ~StratumTransport();

    StratumTransport(const StratumTransport&) = delete;
    StratumTransport& operator=(const StratumTransport&) = delete;
    StratumTransport(StratumTransport&&) noexcept;
    StratumTransport& operator=(StratumTransport&&) noexcept;

    void connect_to(const PoolConfig& config);
    void send_json(const nlohmann::json& value);
    std::vector<std::string> read_lines(int timeout_ms);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace unique_rx
