#pragma once

#include <filesystem>
#include <string_view>

namespace unique_rx::logging {
void set_file(const std::filesystem::path& path);
void info(std::string_view category, std::string_view message);
void warning(std::string_view category, std::string_view message);
void error(std::string_view category, std::string_view message);
}
