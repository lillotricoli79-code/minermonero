#include "logging/log.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <ctime>
#include <deque>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
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

enum class EventKind { Record, SetFile, Flush };

struct Event {
    EventKind kind = EventKind::Record;
    Entry entry;
    std::filesystem::path path;
    std::shared_ptr<std::promise<bool>> completion;
};

struct State {
    std::mutex mutex;
    std::condition_variable ready;
    std::condition_variable space;
    std::deque<Event> queue;
    std::thread writer;
    Config config;
    std::filesystem::path path;
    std::vector<Entry> history;
    std::atomic<std::uint64_t> dropped{0};
    std::atomic<std::uint64_t> errors{0};
    bool running = false;
    bool stopping = false;
    std::ofstream file;
};

State& state() {
    static State value;
    return value;
}

const char* level_name(Level level) {
    switch (level) {
    case Level::Trace: return "TRACE";
    case Level::Debug: return "DEBUG";
    case Level::Info: return "INFO";
    case Level::Warning: return "WARN";
    case Level::Error: return "ERROR";
    case Level::Fatal: return "FATAL";
    case Level::Off: return "OFF";
    }
    return "UNKNOWN";
}

std::string timestamp_now() {
    const auto now = std::chrono::system_clock::now();
    const auto value = std::chrono::system_clock::to_time_t(now);
    std::tm local{};
#ifdef _WIN32
    localtime_s(&local, &value);
#else
    localtime_r(&value, &local);
#endif
    std::ostringstream out;
    out << std::put_time(&local, "%Y-%m-%d %H:%M:%S");
    return out.str();
}

bool terminal_supports_color() {
#ifdef _WIN32
    static const bool enabled = [] {
        const HANDLE handle = GetStdHandle(STD_OUTPUT_HANDLE);
        if (handle == nullptr || handle == INVALID_HANDLE_VALUE) return false;
        DWORD mode = 0;
        if (!GetConsoleMode(handle, &mode)) return false;
        return SetConsoleMode(handle, mode | 0x0004 /* ENABLE_VIRTUAL_TERMINAL_PROCESSING */) != 0;
    }();
    return enabled;
#else
    return ::isatty(STDOUT_FILENO) != 0;
#endif
}

std::string format_entry(const Entry& entry) {
    std::ostringstream out;
    out << '[' << entry.timestamp << "] "
        << std::left << std::setw(5) << entry.level << ' '
        << std::left << std::setw(5) << entry.category << ' '
        << entry.message;
    return out.str();
}

void remember_locked(State& s, const Entry& entry) {
    constexpr std::size_t maximum_history = 2048;
    s.history.push_back(entry);
    if (s.history.size() > maximum_history) {
        s.history.erase(s.history.begin(),
                        s.history.begin() + static_cast<std::ptrdiff_t>(
                            s.history.size() - maximum_history));
    }
}

void write_console(const Entry& entry, const Config& config) {
    if (!config.console_output) return;
    const bool color = config.colored_output && terminal_supports_color();
    const char* code = entry.level == "ERROR" || entry.level == "FATAL"
        ? "\x1b[31;1m"
        : entry.level == "WARN" ? "\x1b[33;1m"
        : entry.category == "speed" ? "\x1b[32;1m"
        : entry.category == "net" ? "\x1b[36m" : "\x1b[0m";
    if (color) std::cout << code;
    std::cout << format_entry(entry);
    if (color) std::cout << "\x1b[0m";
    std::cout << '\n';
    if (entry.level == "WARN" || entry.level == "ERROR" || entry.level == "FATAL")
        std::cout.flush();
}

void note_io_error(State& s, const char* operation) {
    s.errors.fetch_add(1, std::memory_order_relaxed);
    std::cerr << "[ERROR] logger: " << operation << '\n';
}

void open_file(State& s, const std::filesystem::path& path) {
    if (s.file.is_open()) {
        s.file.flush();
        s.file.close();
    }
    s.file.clear();
    s.path = path;
    if (path.empty()) return;

    std::error_code ec;
    if (!path.parent_path().empty())
        std::filesystem::create_directories(path.parent_path(), ec);
    if (ec) {
        note_io_error(s, ec.message().c_str());
        return;
    }
    s.file.open(path, std::ios::binary | std::ios::app);
    if (!s.file) note_io_error(s, "cannot open log file");
}

void rotate_if_needed(State& s, const Config& config) {
    if (!s.file.is_open() || s.path.empty() || config.max_file_bytes == 0) return;
    s.file.flush();
    if (!s.file) {
        note_io_error(s, "flush before rotation failed");
        s.file.clear();
        return;
    }

    std::error_code ec;
    const auto size = std::filesystem::file_size(s.path, ec);
    if (ec || size < config.max_file_bytes) return;

    s.file.close();
    const auto rotated = [&s](unsigned int index) {
        return std::filesystem::path(s.path.string() + "." + std::to_string(index));
    };

    for (unsigned int index = config.retained_files; index > 0; --index) {
        const auto destination = rotated(index);
        if (index == config.retained_files) {
            std::filesystem::remove(destination, ec);
            ec.clear();
        }
        if (index > 1) {
            const auto source = rotated(index - 1);
            if (std::filesystem::exists(source, ec) && !ec) {
                std::filesystem::rename(source, destination, ec);
                if (ec) break;
            }
            ec.clear();
        }
    }
    if (!ec && config.retained_files > 0)
        std::filesystem::rename(s.path, rotated(1), ec);
    if (ec) {
        note_io_error(s, ec.message().c_str());
        ec.clear();
    }

    s.file.clear();
    s.file.open(s.path, std::ios::binary | std::ios::app);
    if (!s.file) note_io_error(s, "cannot reopen log file after rotation");
}

void write_record(State& s, const Entry& entry, const Config& config) {
    write_console(entry, config);
    if (!s.file.is_open()) return;
    rotate_if_needed(s, config);
    if (!s.file.is_open()) return;
    s.file << format_entry(entry) << '\n';
    if (!s.file) {
        note_io_error(s, "write failed");
        s.file.clear();
        return;
    }
    if (entry.level == "WARN" || entry.level == "ERROR" || entry.level == "FATAL")
        s.file.flush();
}

void writer_loop(State* state_ptr) {
    State& s = *state_ptr;
    for (;;) {
        Event event;
        Config config;
        {
            std::unique_lock lock(s.mutex);
            s.ready.wait(lock, [&] { return s.stopping || !s.queue.empty(); });
            if (s.queue.empty() && s.stopping) break;
            event = std::move(s.queue.front());
            s.queue.pop_front();
            config = s.config;
            s.space.notify_all();
        }

        if (event.kind == EventKind::Record) {
            write_record(s, event.entry, config);
        } else if (event.kind == EventKind::SetFile) {
            open_file(s, event.path);
            if (event.completion) {
                const bool ok = event.path.empty() || s.file.is_open();
                event.completion->set_value(ok);
            }
        } else if (event.kind == EventKind::Flush) {
            if (s.file.is_open()) s.file.flush();
            std::cout.flush();
            if (event.completion)
                event.completion->set_value(!s.file.is_open() || static_cast<bool>(s.file));
        }
    }
    if (s.file.is_open()) {
        s.file.flush();
        s.file.close();
    }
}

bool enqueue_control(State& s, Event event, bool wait_for_space) {
    std::unique_lock lock(s.mutex);
    if (!s.running || s.stopping) return false;
    const auto capacity = std::max<std::size_t>(64, s.config.queue_capacity);
    if (wait_for_space)
        s.space.wait(lock, [&] { return s.queue.size() < capacity || s.stopping; });
    if (s.stopping || s.queue.size() >= capacity) return false;
    s.queue.push_back(std::move(event));
    s.ready.notify_one();
    return true;
}

void start_locked(State& s) {
    if (s.running) return;
    s.stopping = false;
    s.running = true;
    s.writer = std::thread(writer_loop, &s);
}

void submit(Level level, std::string_view category, std::string_view message) {
    State& s = state();
    Entry entry{timestamp_now(), level_name(level),
                std::string(category), std::string(message)};
    Config config;
    bool synchronous = false;
    {
        std::lock_guard lock(s.mutex);
        config = s.config;
        if (level == Level::Off || level < config.minimum_level) return;
        remember_locked(s, entry);
        if (!s.running || s.stopping) {
            synchronous = true;
        } else {
            const auto capacity = std::max<std::size_t>(64, config.queue_capacity);
            if (s.queue.size() >= capacity) {
                s.dropped.fetch_add(1, std::memory_order_relaxed);
                // Preserve severe diagnostics without blocking mining workers.
                if (level >= Level::Error) {
                    write_console(entry, config);
                    std::cerr << "[WARN] logger queue full; message not persisted\n";
                }
                return;
            }
            Event event;
            event.kind = EventKind::Record;
            event.entry = std::move(entry);
            s.queue.push_back(std::move(event));
            s.ready.notify_one();
        }
    }
    if (synchronous) {
        std::lock_guard lock(s.mutex);
        write_console(entry, config);
        if (s.file.is_open()) {
            s.file << format_entry(entry) << '\n';
            if (level >= Level::Warning) s.file.flush();
            if (!s.file) {
                s.errors.fetch_add(1, std::memory_order_relaxed);
                s.file.clear();
            }
        }
    }
}

} // namespace

bool initialize(const std::filesystem::path& path, const Config& config) {
    State& s = state();
    {
        std::lock_guard lock(s.mutex);
        s.config = config;
        s.config.queue_capacity = std::max<std::size_t>(64, s.config.queue_capacity);
        if (s.running && !s.stopping) {
            // The writer owns the stream: enqueue a serialized file switch.
        } else {
            open_file(s, path);
            start_locked(s);
            return s.path.empty() || s.file.is_open();
        }
    }
    set_file(path);
    return true;
}

void set_file(const std::filesystem::path& path) {
    State& s = state();
    std::shared_ptr<std::promise<bool>> completion =
        std::make_shared<std::promise<bool>>();
    auto result = completion->get_future();
    bool running = false;
    {
        std::lock_guard lock(s.mutex);
        running = s.running && !s.stopping;
        if (!running) {
            open_file(s, path);
            return;
        }
    }
    Event event;
    event.kind = EventKind::SetFile;
    event.path = path;
    event.completion = completion;
    if (!enqueue_control(s, std::move(event), true)) return;
    (void)result.get();
}

void configure(const Config& config) {
    State& s = state();
    std::lock_guard lock(s.mutex);
    s.config = config;
    s.config.queue_capacity = std::max<std::size_t>(64, s.config.queue_capacity);
}

void close_file() {
    State& s = state();
    std::thread writer;
    {
        std::lock_guard lock(s.mutex);
        if (!s.running) {
            if (s.file.is_open()) {
                s.file.flush();
                s.file.close();
            }
            return;
        }
        s.stopping = true;
        s.ready.notify_all();
        s.space.notify_all();
        writer = std::move(s.writer);
    }
    if (writer.joinable() && writer.get_id() != std::this_thread::get_id())
        writer.join();
    {
        std::lock_guard lock(s.mutex);
        s.running = false;
        s.stopping = false;
        s.queue.clear();
    }
}

void trace(std::string_view category, std::string_view message) {
    submit(Level::Trace, category, message);
}
void debug(std::string_view category, std::string_view message) {
    submit(Level::Debug, category, message);
}
void info(std::string_view category, std::string_view message) {
    submit(Level::Info, category, message);
}
void warning(std::string_view category, std::string_view message) {
    submit(Level::Warning, category, message);
}
void error(std::string_view category, std::string_view message) {
    submit(Level::Error, category, message);
}
void fatal(std::string_view category, std::string_view message) {
    submit(Level::Fatal, category, message);
    flush();
}
void log(Level level, std::string_view category, std::string_view message) {
    submit(level, category, message);
}

void flush() {
    State& s = state();
    auto completion = std::make_shared<std::promise<bool>>();
    auto result = completion->get_future();
    {
        std::lock_guard lock(s.mutex);
        if (!s.running || s.stopping) {
            if (s.file.is_open()) s.file.flush();
            std::cout.flush();
            return;
        }
    }
    Event event;
    event.kind = EventKind::Flush;
    event.completion = completion;
    if (enqueue_control(s, std::move(event), true))
        (void)result.get();
}

std::vector<Entry> recent_entries(std::size_t limit, Level minimum_level,
                                  std::string_view category) {
    State& s = state();
    std::lock_guard lock(s.mutex);
    std::vector<Entry> result;
    if (limit == 0) return result;
    result.reserve(std::min(limit, s.history.size()));
    for (auto it = s.history.rbegin(); it != s.history.rend() && result.size() < limit; ++it) {
        const auto found = std::find_if(
            std::begin(std::array{Level::Trace, Level::Debug, Level::Info,
                                  Level::Warning, Level::Error, Level::Fatal}),
            std::end(std::array{Level::Trace, Level::Debug, Level::Info,
                                Level::Warning, Level::Error, Level::Fatal}),
            [&](Level value) { return it->level == level_name(value); });
        if (found == std::end(std::array{Level::Trace, Level::Debug, Level::Info,
                                         Level::Warning, Level::Error, Level::Fatal}))
            continue;
        if (*found < minimum_level) continue;
        if (!category.empty() && it->category != category) continue;
        result.push_back(*it);
    }
    std::reverse(result.begin(), result.end());
    return result;
}

std::uint64_t dropped_messages() {
    return state().dropped.load(std::memory_order_relaxed);
}
std::uint64_t write_errors() {
    return state().errors.load(std::memory_order_relaxed);
}
bool is_running() {
    State& s = state();
    std::lock_guard lock(s.mutex);
    return s.running && !s.stopping;
}

} // namespace unique_rx::logging
