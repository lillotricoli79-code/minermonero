#pragma once

#include "presets.hpp"

#include <filesystem>
#include <memory>
#include <optional>
#include <vector>

namespace unique_rx {

class MsrManager {
public:
    explicit MsrManager(std::filesystem::path module_path = {});
    ~MsrManager();

    MsrManager(const MsrManager&) = delete;
    MsrManager& operator=(const MsrManager&) = delete;
    MsrManager(MsrManager&&) noexcept;
    MsrManager& operator=(MsrManager&&) noexcept;

    void apply_randomx_preset(std::optional<std::uint64_t> intel_prefetch_value = std::nullopt);
    void apply_custom_writes(const std::vector<MsrWrite>& writes);
    void set_restore_on_exit(bool enabled) noexcept;
    void restore() noexcept;
    bool active() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace unique_rx
