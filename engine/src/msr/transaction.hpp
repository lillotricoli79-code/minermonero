#pragma once

#include "presets.hpp"

#include <cstddef>
#include <cstdint>
#include <exception>
#include <stdexcept>
#include <vector>

namespace unique_rx::msr_detail {

struct RegisterSnapshot {
    std::size_t cpu_index = 0;
    std::uint64_t register_id = 0;
    std::uint64_t value = 0;
    std::uint64_t mask = 0;
    bool modified = false;
};

template <typename Read, typename Write, typename Log>
bool restore_registers(std::vector<RegisterSnapshot>& snapshots,
                       Read&& read, Write&& write, Log&& log) noexcept {
    bool pending = false;
    for (auto it = snapshots.rbegin(); it != snapshots.rend(); ++it) {
        if (!it->modified) continue;
        try {
            // Restore only the bits this transaction changed. Preserve unrelated
            // bits that firmware, the OS, or another owner may have changed since
            // the original snapshot was taken.
            const auto current = read(it->cpu_index, it->register_id);
            const auto restored = (current & ~it->mask) | (it->value & it->mask);
            write(it->cpu_index, it->register_id, restored);
            if ((read(it->cpu_index, it->register_id) & it->mask) !=
                (it->value & it->mask)) {
                throw std::runtime_error("MSR restore read-back verification failed");
            }
            it->modified = false;
        } catch (const std::exception& error) {
            pending = true;
            try { log(error.what()); } catch (...) {}
        } catch (...) {
            pending = true;
            try { log("unknown MSR restore error"); } catch (...) {}
        }
        pending = pending || it->modified;
    }
    return pending;
}

template <typename Read, typename Write, typename Log>
void apply_registers(const std::vector<std::size_t>& cpu_indices,
                     const std::vector<MsrWrite>& writes,
                     std::vector<RegisterSnapshot>& snapshots,
                     Read&& read, Write&& write, Log&& log) {
    if (writes.empty()) throw std::runtime_error("MSR transaction has no register writes");
    snapshots.clear();
    for (const std::size_t cpu : cpu_indices) {
        for (const auto& item : writes) {
            snapshots.push_back({cpu, item.register_id,
                                 read(cpu, item.register_id), item.mask, false});
        }
    }

    try {
        for (std::size_t index = 0; index < snapshots.size(); ++index) {
            auto& snapshot = snapshots[index];
            const auto& item = writes[index % writes.size()];
            const std::uint64_t value = (item.value & item.mask) |
                                        (snapshot.value & ~item.mask);
            snapshot.modified = true;
            write(snapshot.cpu_index, snapshot.register_id, value);
            if ((read(snapshot.cpu_index, snapshot.register_id) & item.mask) !=
                (value & item.mask)) {
                throw std::runtime_error("MSR read-back verification failed");
            }
        }
    } catch (...) {
        (void)restore_registers(snapshots, read, write, log);
        throw;
    }
}

} // namespace unique_rx::msr_detail
