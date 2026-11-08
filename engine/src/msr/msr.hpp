#pragma once

#include <filesystem>
#include <memory>

namespace unique_rx {

class MsrManager {
public:
    explicit MsrManager(std::filesystem::path module_path = {});
    ~MsrManager();

    MsrManager(const MsrManager&) = delete;
    MsrManager& operator=(const MsrManager&) = delete;
    MsrManager(MsrManager&&) noexcept;
    MsrManager& operator=(MsrManager&&) noexcept;

    void apply_randomx_preset();
    void restore() noexcept;
    bool active() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace unique_rx
