#include "stratum/stratum.hpp"
#include "platform/socket.hpp"

#include <nlohmann/json.hpp>

#include <atomic>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/types.h>
#include <sys/time.h>
#include <unistd.h>
#endif

namespace {

using Socket = unique_rx::platform::Socket;

#ifdef _WIN32
using SocketLength = int;
#else
using SocketLength = socklen_t;
#endif

bool set_reuse_address(Socket socket) {
    int enabled = 1;
#ifdef _WIN32
    return setsockopt(socket, SOL_SOCKET, SO_REUSEADDR,
                      reinterpret_cast<const char*>(&enabled), sizeof(enabled)) == 0;
#else
    return setsockopt(socket, SOL_SOCKET, SO_REUSEADDR,
                      &enabled, sizeof(enabled)) == 0;
#endif
}

bool set_receive_timeout(Socket socket) {
#ifdef _WIN32
    const DWORD timeout_ms = 15000;
    return setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO,
                      reinterpret_cast<const char*>(&timeout_ms), sizeof(timeout_ms)) == 0;
#else
    const timeval timeout{15, 0};
    return setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO,
                      &timeout, sizeof(timeout)) == 0;
#endif
}

int bind_loopback(Socket socket, sockaddr_in& address) {
    return ::bind(socket, reinterpret_cast<const sockaddr*>(&address),
                  static_cast<SocketLength>(sizeof(address)));
}

int get_local_address(Socket socket, sockaddr_in& address, SocketLength& length) {
    return getsockname(socket, reinterpret_cast<sockaddr*>(&address), &length);
}

unsigned short reserve_closed_port() {
    const Socket probe = ::socket(AF_INET, SOCK_STREAM, 0);
    if (!unique_rx::platform::socket_valid(probe)) return 0;
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
    SocketLength length = static_cast<SocketLength>(sizeof(address));
    if (bind_loopback(probe, address) != 0 ||
        get_local_address(probe, address, length) != 0) {
        unique_rx::platform::close_socket(probe);
        return 0;
    }
    const auto port = ntohs(address.sin_port);
    unique_rx::platform::close_socket(probe);
    return port;
}

unsigned long long process_id() {
#ifdef _WIN32
    return static_cast<unsigned long long>(GetCurrentProcessId());
#else
    return static_cast<unsigned long long>(getpid());
#endif
}

bool send_all(Socket socket, const std::string& value) {
    std::size_t offset = 0;
    while (offset < value.size()) {
        const int sent = unique_rx::platform::send_bytes(
            socket, value.data() + offset, value.size() - offset);
        if (sent <= 0) return false;
        offset += static_cast<std::size_t>(sent);
    }
    return true;
}

bool is_hex_string(const std::string& value, std::size_t expected_size) {
    if (value.size() != expected_size) return false;
    for (const unsigned char ch : value) {
        if (!((ch >= '0' && ch <= '9') ||
              (ch >= 'a' && ch <= 'f') ||
              (ch >= 'A' && ch <= 'F'))) {
            return false;
        }
    }
    return true;
}

bool read_line(Socket socket, std::string& line) {
    char character = 0;
    while (line.size() < 1024 * 1024) {
        const int count = unique_rx::platform::receive_bytes(socket, &character, 1);
        if (count != 1) return false;
        if (character == '\n') return true;
        if (character != '\r') line.push_back(character);
    }
    return false;
}

} // namespace

int main() {
    unique_rx::platform::NetworkRuntime network;
    const Socket listener = ::socket(AF_INET, SOCK_STREAM, 0);
    if (!unique_rx::platform::socket_valid(listener)) return 1;
    if (!set_reuse_address(listener)) {
        unique_rx::platform::close_socket(listener);
        return 1;
    }

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
    if (bind_loopback(listener, address) != 0 || listen(listener, 1) != 0) {
        unique_rx::platform::close_socket(listener);
        return 1;
    }
    SocketLength address_size = static_cast<SocketLength>(sizeof(address));
    if (get_local_address(listener, address, address_size) != 0) {
        unique_rx::platform::close_socket(listener);
        return 1;
    }

    std::atomic<bool> server_ok{false};
    std::thread pool([&] {
        try {
            if (!unique_rx::platform::wait_socket(
                    listener, unique_rx::platform::kReadEvent, 20000)) {
                return;
            }
            const Socket client = accept(listener, nullptr, nullptr);
            if (!unique_rx::platform::socket_valid(client)) return;
            set_receive_timeout(client);
            std::string line;
            if (!read_line(client, line)) {
                unique_rx::platform::close_socket(client);
                return;
            }
            const auto login = nlohmann::json::parse(line);
            const bool valid_login =
                login.value("jsonrpc", std::string()) == "2.0" &&
                login.value("id", 0) == 1 &&
                login.value("method", std::string()) == "login" &&
                login.contains("params") && login["params"].is_object() &&
                login["params"].value("login", std::string()) == "loopback-test-wallet" &&
                login["params"].value("pass", std::string()) == "x" &&
                login["params"].value("rigid", std::string()) == "test" &&
                login["params"].contains("algo") &&
                login["params"]["algo"].is_array() &&
                login["params"]["algo"] == nlohmann::json::array({"rx/0"});
            if (!valid_login) {
                unique_rx::platform::close_socket(client);
                return;
            }

            const nlohmann::json job{
                {"job_id", "local-job"},
                {"blob", std::string(76 * 2, '0')},
                {"seed_hash", std::string(64, '0')},
                {"target", "ffffffff"},
                {"height", 1},
                {"algo", "rx/0"},
            };
            const nlohmann::json response{
                {"id", 1}, {"jsonrpc", "2.0"},
                {"result", {{"id", "local-session"}, {"status", "OK"}, {"job", job}}},
            };
            line.clear();
            if (!send_all(client, response.dump() + "\n") || !read_line(client, line)) {
                unique_rx::platform::close_socket(client);
                return;
            }
            const auto submit = nlohmann::json::parse(line);
            const bool valid_submit =
                submit.value("jsonrpc", std::string()) == "2.0" &&
                submit.value("id", 0ULL) > 1 &&
                submit.value("method", std::string()) == "submit" &&
                submit.contains("params") && submit["params"].is_object() &&
                submit["params"].value("id", std::string()) == "local-session" &&
                submit["params"].value("job_id", std::string()) == "local-job" &&
                is_hex_string(submit["params"].value("nonce", std::string()), 8) &&
                is_hex_string(submit["params"].value("result", std::string()), 64);
            const nlohmann::json submit_response{
                {"id", submit.value("id", 0ULL)}, {"jsonrpc", "2.0"},
                {"result", {{"status", "OK"}}},
            };
            const bool response_sent = valid_submit &&
                send_all(client, submit_response.dump() + "\n");
            server_ok.store(response_sent, std::memory_order_release);
            unique_rx::platform::close_socket(client);
        } catch (const std::exception& error) {
            std::cerr << "mock pool error: " << error.what() << '\n';
        }
    });

    const auto closed_port = reserve_closed_port();
    if (closed_port == 0) {
        pool.join();
        unique_rx::platform::close_socket(listener);
        return 1;
    }
    const auto config_path = std::filesystem::temp_directory_path() /
        ("unique-rx-stratum-" + std::to_string(process_id()) + ".json");
    const nlohmann::json config{
        {"pools", nlohmann::json::array({
            nlohmann::json{
                {"url", "127.0.0.1:" + std::to_string(closed_port)},
                {"user", "unavailable-pool-wallet"}, {"pass", "x"},
                {"enabled", true}, {"tls", false},
            },
            nlohmann::json{
                {"url", "127.0.0.1:" + std::to_string(ntohs(address.sin_port))},
                {"user", "loopback-test-wallet"}, {"pass", "x"},
                {"rig-id", "test"}, {"enabled", true}, {"tls", false},
            },
        })},
        {"cpu", {{"enabled", true}, {"max-threads-hint", 1}}},
        {"randomx", {{"wrmsr", false}, {"full-memory", false}}},
    };
    {
        std::ofstream file(config_path);
        file << config.dump(2);
    }
    std::ostringstream miner_output;
    std::ostringstream miner_errors;
    auto* previous_output = std::cout.rdbuf(miner_output.rdbuf());
    auto* previous_error = std::cerr.rdbuf(miner_errors.rdbuf());
    const int result = unique_rx::run_miner(config_path, 1);
    std::cout.rdbuf(previous_output);
    std::cerr.rdbuf(previous_error);
    pool.join();
    unique_rx::platform::close_socket(listener);
    std::filesystem::remove(config_path);
    if (result != 0 || !server_ok.load(std::memory_order_acquire) ||
        miner_errors.str().find("switching pool") == std::string::npos ||
        miner_output.str().find("Share accepted") == std::string::npos) {
        std::cerr << "local Stratum failover/login/job/share acceptance test failed\n";
        return 1;
    }
    std::cout << "Local Stratum login, RandomX job and share submit passed.\n";
    return 0;
}
