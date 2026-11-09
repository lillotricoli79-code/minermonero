
#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace unique_rx::logging {

enum class Level : std::uint8_t {
    Trace = 0,
    Debug,
    Info,
    Warning,
    Error,
    Fatal,
    Off
};

struct Config {
    Level minimum_level = Level::Info;
    std::size_t queue_capacity = 8192;
    std::uintmax_t max_file_bytes = 8U * 1024U * 1024U;
    unsigned int retained_files = 3;
    bool console_output = true;
    bool colored_output = true;
    bool flush_on_warning = true;
};

struct Entry {
    std::string timestamp;
    std::string level;
    std::string category;
    std::string message;
};

bool initialize(const std::filesystem::path& path,
                const Config& config = {});

void set_file(const std::filesystem::path& path);
void configure(const Config& config);
void close_file();

void trace(std::string_view category, std::string_view message);
void debug(std::string_view category, std::string_view message);
void info(std::string_view category, std::string_view message);
void warning(std::string_view category, std::string_view message);
void error(std::string_view category, std::string_view message);
void fatal(std::string_view category, std::string_view message);

void log(Level level,
         std::string_view category,
         std::string_view message);

void flush();

std::vector<Entry> recent_entries(
    std::size_t limit = 200,
    Level minimum_level = Level::Trace,
    std::string_view category = {}
);

std::uint64_t dropped_messages();
std::uint64_t write_errors();
bool is_running();

} // namespace unique_rx::logging
