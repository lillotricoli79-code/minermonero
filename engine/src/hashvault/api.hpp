#pragma once

#include <nlohmann/json.hpp>
#include <string>

namespace unique_rx::hashvault {

struct Snapshot {
    nlohmann::json pool = nlohmann::json::object();
    nlohmann::json wallet = nlohmann::json::object();
    std::string pool_error;
    std::string wallet_error;
    bool pool_ready = false;
    bool wallet_ready = false;
};

bool fetch_json(const std::string& path, const std::string& tls_fingerprint,
                nlohmann::json& output, std::string& error) noexcept;
void refresh(Snapshot& snapshot, const std::string& wallet,
             const std::string& tls_fingerprint, bool solo,
             const std::string& period = "daily",
             unsigned inactivity_minutes = 10) noexcept;

} // namespace unique_rx::hashvault
