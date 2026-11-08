#include "logging/log.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

int main() {
    const auto directory = std::filesystem::temp_directory_path() /
        "unique-rx-logging-rotation-test";
    std::error_code error;
    std::filesystem::remove_all(directory, error);
    std::filesystem::create_directories(directory, error);
    if (error) return 1;

    const auto path = directory / "miner.log";
    {
        std::ofstream file(path, std::ios::binary);
        if (!file) return 1;
        const std::string block(1024U * 1024U, 'x');
        for (int index = 0; index < 8; ++index) file.write(block.data(), block.size());
        if (!file) return 1;
    }

    unique_rx::logging::set_file(path);
    unique_rx::logging::info("test", "rotation verified");
    const auto rotated = std::filesystem::path(path.string() + ".1");
    const bool passed =
        std::filesystem::is_regular_file(rotated) &&
        std::filesystem::file_size(rotated) >= 8U * 1024U * 1024U &&
        std::filesystem::is_regular_file(path) &&
        std::filesystem::file_size(path) > 0;
    std::filesystem::remove_all(directory, error);
    if (!passed) {
        std::cerr << "bounded log rotation test failed\n";
        return 1;
    }
    std::cout << "Bounded rotating file logger passed.\n";
    return 0;
}
