#include "logging/log.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
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

    std::cout << "Bounded rotating file logger passed.\n";
    return 0;
}
