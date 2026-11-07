#include "stratum.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>

namespace {

bool send_all(int socket, const std::string& value) {
    std::size_t offset = 0;
    while (offset < value.size()) {
        const auto sent = send(socket, value.data() + offset, value.size() - offset, 0);
        if (sent <= 0) return false;
        offset += static_cast<std::size_t>(sent);
    }
    return true;
}

bool read_line(int socket, std::string& line) {
    char character = 0;
    while (line.size() < 1024 * 1024) {
        const auto count = recv(socket, &character, 1, 0);
        if (count != 1) return false;
        if (character == '\n') return true;
        if (character != '\r') line.push_back(character);
    }
    return false;
}

} // namespace

int main() {
    const int listener = socket(AF_INET, SOCK_STREAM, 0);
    if (listener < 0) return 1;
    int reuse = 1;
    setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
    if (bind(listener, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0 ||
        listen(listener, 1) != 0) {
        close(listener);
        return 1;
    }
    socklen_t address_size = sizeof(address);
    if (getsockname(listener, reinterpret_cast<sockaddr*>(&address), &address_size) != 0) {
        close(listener);
        return 1;
    }

    std::atomic<bool> server_ok{false};
    std::thread pool([&] {
        try {
            const int client = accept(listener, nullptr, nullptr);
            if (client < 0) return;
            timeval timeout{15, 0};
            setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
            std::string line;
            if (!read_line(client, line)) {
                close(client);
                return;
            }
            const auto login = nlohmann::json::parse(line);
            if (login.value("method", std::string()) != "login") {
                close(client);
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
                close(client);
                return;
            }
            const auto submit = nlohmann::json::parse(line);
            server_ok.store(
                submit.value("method", std::string()) == "submit" &&
                submit["params"].value("id", std::string()) == "local-session" &&
                submit["params"].value("job_id", std::string()) == "local-job" &&
                submit["params"].value("nonce", std::string()).size() == 8 &&
                submit["params"].value("result", std::string()).size() == 64,
                std::memory_order_release);
            close(client);
        } catch (const std::exception& error) {
            std::cerr << "mock pool error: " << error.what() << '\n';
        }
    });

    const auto config_path = std::filesystem::temp_directory_path() /
        ("unique-rx-stratum-" + std::to_string(getpid()) + ".json");
    const nlohmann::json config{
        {"pools", nlohmann::json::array({nlohmann::json{
            {"url", "127.0.0.1:" + std::to_string(ntohs(address.sin_port))},
            {"user", "loopback-test-wallet"}, {"pass", "x"},
            {"rig-id", "test"}, {"tls", false},
        }})},
    };
    {
        std::ofstream file(config_path);
        file << config.dump(2);
    }
    const int result = unique_rx::run_miner(config_path, 1);
    pool.join();
    close(listener);
    std::filesystem::remove(config_path);
    if (result != 0 || !server_ok.load(std::memory_order_acquire)) {
        std::cerr << "local Stratum login/job/share test failed\n";
        return 1;
    }
    std::cout << "Local Stratum login, RandomX job and share submit passed.\n";
    return 0;
}