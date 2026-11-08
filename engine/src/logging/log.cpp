#include "logging/log.hpp"

#include <chrono>
#include <cstdint>
#include <ctime>
#include <fstream>
#include <filesystem>
#include <system_error>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace unique_rx::logging {
namespace {
std::mutex output_mutex;
std::ofstream log_file;
std::filesystem::path log_path;

bool color_output() {
#ifdef _WIN32
    // Keep Windows console output plain: older MinGW/Windows SDK headers do
    // not consistently define ENABLE_VIRTUAL_TERMINAL_PROCESSING.
    return false;
#else
    return ::isatty(STDOUT_FILENO) != 0;
#endif
}

void rotate_log_if_needed() {
    constexpr std::uintmax_t maximum_log_bytes = 8U * 1024U * 1024U;
    constexpr int retained_rotations = 3;
    if (log_path.empty()) return;
    std::error_code error;
    if (!std::filesystem::exists(log_path, error) || error) return;
    const auto size = std::filesystem::file_size(log_path, error);
    if (error || size < maximum_log_bytes) return;

    if (log_file.is_open()) {
        log_file.flush();
        log_file.close();
    }
    const auto rotated = [&](int index) {
        return std::filesystem::path(log_path.string() + "." + std::to_string(index));
    };
    std::filesystem::remove(rotated(retained_rotations), error);
    error.clear();
    for (int index = retained_rotations - 1; index >= 1; --index) {
        const auto source = rotated(index);
        if (std::filesystem::exists(source, error) && !error) {
            std::filesystem::rename(source, rotated(index + 1), error);
            if (error) break;
        }
        error.clear();
    }
    if (!error) std::filesystem::rename(log_path, rotated(1), error);
    if (error) {
        std::cerr << "[WARN] log rotation skipped for " << log_path.string()
                  << ": " << error.message() << '\n';
    }
    log_file.clear();
    log_file.open(log_path, std::ios::app);
    if (!log_file) {
        std::cerr << "[ERROR] cannot reopen miner log file: " << log_path.string() << '\n';
    }
}

void write(std::string_view level, std::string_view category,
           std::string_view message) {
    const auto now = std::chrono::system_clock::now();
    const auto time = std::chrono::system_clock::to_time_t(now);
    std::tm local{};
#ifdef _WIN32
    localtime_s(&local, &time);
#else
    localtime_r(&time, &local);
#endif
    std::ostringstream line;
    line << '[' << std::put_time(&local, "%Y-%m-%d %H:%M:%S") << "] "
         << std::left << std::setw(5) << level << ' '
         << std::left << std::setw(5) << category << ' '
         << message;
    std::lock_guard lock(output_mutex);
    rotate_log_if_needed();
    const char* color = level == "ERROR" ? "\x1b[31;1m" :
        level == "WARN" ? "\x1b[33;1m" :
        category == "speed" ? "\x1b[32;1m" :
        category == "net" ? "\x1b[36m" : "\x1b[0m";
    if (color_output()) std::cout << color;
    std::cout << line.str();
    if (color_output()) std::cout << "\x1b[0m";
    std::cout << '\n' << std::flush;
    if (log_file) log_file << line.str() << '\n' << std::flush;
}
}

void set_file(const std::filesystem::path& path) {
    std::lock_guard lock(output_mutex);
    std::error_code error;
    if (!path.parent_path().empty()) {
        std::filesystem::create_directories(path.parent_path(), error);
        if (error) {
            std::cerr << "[ERROR] log cannot create directory "
                      << path.parent_path().string() << ": " << error.message() << '\n';
            return;
        }
    }
    log_file.close();
    log_file.clear();
    log_path = path;
    rotate_log_if_needed();
    if (!log_file.is_open()) log_file.open(path, std::ios::app);
    if (!log_file) {
        std::cerr << "[ERROR] cannot open miner log file: " << path.string() << '\n';
    }
}

void close_file() {
    std::lock_guard lock(output_mutex);
    if (log_file.is_open()) {
        log_file.flush();
        log_file.close();
    }
}

void info(std::string_view category, std::string_view message) {
    write("INFO", category, message);
}
void warning(std::string_view category, std::string_view message) {
    write("WARN", category, message);
}
void error(std::string_view category, std::string_view message) {
    write("ERROR", category, message);
}
}
