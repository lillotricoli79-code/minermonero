#include "logging/log.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>
#include <vector>
#include <system_error>

int main() {
    constexpr std::uintmax_t expected_bytes = 8U * 1024U * 1024U;
    const auto directory = std::filesystem::temp_directory_path() /
        "unique-rx-logging-rotation-test";
    std::error_code error;
    std::filesystem::remove_all(directory, error);
    error.clear();
    std::filesystem::create_directories(directory, error);
    if (error) {
        std::cerr << "cannot create test directory: " << error.message() << '\n';
        return 1;
    }

    const auto path = directory / "miner.log";
    {
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        if (!file) {
            std::cerr << "cannot create initial log file\n";
            std::filesystem::remove_all(directory, error);
            return 1;
        }
        const std::string block(1024U * 1024U, 'x');
        for (int index = 0; index < 8; ++index) {
            file.write(block.data(), static_cast<std::streamsize>(block.size()));
            if (!file) {
                std::cerr << "failed writing initial log data at block " << index << '\n';
                file.close();
                std::filesystem::remove_all(directory, error);
                return 1;
            }
        }
    }

    unique_rx::logging::set_file(path);
    unique_rx::logging::info("test", "rotation verified");
    // Close and flush the Windows CRT/MinGW stream before querying file sizes.
    // This makes the assertion measure persisted bytes, not an open-handle view.
    unique_rx::logging::close_file();

    const auto rotated = std::filesystem::path(path.string() + ".1");
    std::error_code inspect_error;
    const bool rotated_exists = std::filesystem::is_regular_file(rotated, inspect_error);
    const auto rotated_size = rotated_exists
        ? std::filesystem::file_size(rotated, inspect_error) : 0U;
    const bool rotated_inspect_error = static_cast<bool>(inspect_error);
    inspect_error.clear();
    const bool current_exists = std::filesystem::is_regular_file(path, inspect_error);
    const auto current_size = current_exists
        ? std::filesystem::file_size(path, inspect_error) : 0U;
    const bool current_inspect_error = static_cast<bool>(inspect_error);
    const bool passed = !rotated_inspect_error && !current_inspect_error &&
        rotated_exists && rotated_size >= expected_bytes &&
        current_exists && current_size > 0U;

    if (!passed) {
        std::cerr << "bounded log rotation test failed"
                  << " rotated_exists=" << rotated_exists
                  << " rotated_size=" << rotated_size
                  << " rotated_error=" << rotated_inspect_error
                  << " current_exists=" << current_exists
                  << " current_size=" << current_size
                  << " current_error=" << current_inspect_error
                  << " expected_rotated_min=" << expected_bytes
                  << '\n';
    }

    unique_rx::logging::close_file();
    std::filesystem::remove_all(directory, error);
    if (!passed) return 1;

    const auto async_path = directory / "async.log";
    unique_rx::logging::Config async_config;
    async_config.minimum_level = unique_rx::logging::Level::Trace;
    async_config.queue_capacity = 2048;
    async_config.max_file_bytes = 0;
    async_config.console_output = false;
    async_config.colored_output = false;
    const auto dropped_before = unique_rx::logging::dropped_messages();
    if (!unique_rx::logging::initialize(async_path, async_config)) {
        std::cerr << "cannot initialize asynchronous logger\\n";
        std::filesystem::remove_all(directory, error);
        return 1;
    }

    constexpr int producer_count = 4;
    constexpr int messages_per_producer = 250;
    std::vector<std::thread> producers;
    producers.reserve(producer_count);
    for (int producer = 0; producer < producer_count; ++producer) {
        producers.emplace_back([producer, messages_per_producer] {
            for (int index = 0; index < messages_per_producer; ++index) {
                unique_rx::logging::info("concurrent",
                    "producer=" + std::to_string(producer) +
                    " message=" + std::to_string(index));
            }
        });
    }
    for (auto& producer : producers) producer.join();
    unique_rx::logging::flush();
    const auto entries = unique_rx::logging::recent_entries(
        producer_count * messages_per_producer,
        unique_rx::logging::Level::Info, "concurrent");
    unique_rx::logging::close_file();

    std::size_t persisted_lines = 0;
    {
        std::ifstream file(async_path);
        std::string line;
        while (std::getline(file, line)) ++persisted_lines;
    }
    const bool async_passed =
        entries.size() == static_cast<std::size_t>(producer_count * messages_per_producer) &&
        persisted_lines == static_cast<std::size_t>(producer_count * messages_per_producer) &&
        unique_rx::logging::dropped_messages() == dropped_before;
    if (!async_passed) {
        std::cerr << "asynchronous multi-producer logger test failed"
                  << " history=" << entries.size()
                  << " persisted=" << persisted_lines
                  << " dropped_delta="
                  << (unique_rx::logging::dropped_messages() - dropped_before)
                  << '\\n';
    }
    std::filesystem::remove_all(directory, error);
    if (!async_passed) return 1;

    std::cout << "Bounded rotating and asynchronous multi-producer logger passed.\\n";
    return 0;
}
