#include "logging/log.hpp"

#include <chrono>
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

bool color_output() {
#ifdef _WIN32
    static const bool enabled = [] {
        const HANDLE handle = GetStdHandle(STD_OUTPUT_HANDLE);
        DWORD mode = 0;
        return handle != INVALID_HANDLE_VALUE && GetConsoleMode(handle, &mode) &&
            SetConsoleMode(handle, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
    }();
    return enabled;
#else
    return ::isatty(STDOUT_FILENO) != 0;
#endif
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
    constexpr std::uintmax_t maximum_log_bytes = 8U * 1024U * 1024U;
    constexpr int retained_rotations = 3;
    std::lock_guard lock(output_mutex);
    std::error_code error;
    if (!path.parent_path().empty()) {
        std::filesystem::create_directories(path.parent_path(), error);
        if (error) {
            std::cerr << "[ERROR] log cannot create directory "
                      << path.parent_path().string() << ": " << error.message() << '\\n';
            return;
        }
    }
    if (std::filesystem::exists(path, error) && !error) {
        const auto size = std::filesystem::file_size(path, error);
        if (!error && size >= maximum_log_bytes) {
            const auto rotated = [&](int index) {
                return std::filesystem::path(path.string() + "." + std::to_string(index));
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
            if (!error) std::filesystem::rename(path, rotated(1), error);
            if (error) {
                std::cerr << "[WARN] log rotation skipped for " << path.string()
                          << ": " << error.message() << '\\n';
            }
        }
    }
    log_file.close();
    log_file.clear();
    log_file.open(path, std::ios::app);
    if (!log_file) {
        std::cerr << "[ERROR] cannot open miner log file: " << path.string() << '\\n';
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
