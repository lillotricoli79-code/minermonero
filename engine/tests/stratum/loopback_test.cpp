#include "stratum/stratum.hpp"
#include "platform/socket.hpp"

#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <csignal>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>
#ifdef _WIN32
#else
#include <cerrno>
#include <sys/wait.h>
#endif

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

int run_miner_process(const std::filesystem::path& executable,
                       const std::filesystem::path& config_path) {
#ifdef _WIN32
    std::wstring command = L"\"" + executable.wstring() + L"\" --mine \"" +
                           config_path.wstring() + L"\"";
    std::vector<wchar_t> mutable_command(command.begin(), command.end());
    mutable_command.push_back(L'\0');
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    // Do not inherit the harness' listening/client sockets or CRT handles.
    // The miner writes diagnostics to its own log and telemetry file.
    if (!CreateProcessW(executable.c_str(), mutable_command.data(), nullptr, nullptr,
                        FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process)) {
        std::cerr << "CreateProcessW failed: " << GetLastError() << '\n';
        return -1;
    }
    const DWORD wait_result = WaitForSingleObject(process.hProcess, 30000);
    if (wait_result != WAIT_OBJECT_0) {
        TerminateProcess(process.hProcess, 124);
        WaitForSingleObject(process.hProcess, 5000);
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
        return 124;
    }
    DWORD exit_code = 1;
    GetExitCodeProcess(process.hProcess, &exit_code);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return static_cast<int>(exit_code);
#else
    const pid_t child = fork();
    if (child < 0) return -1;
    if (child == 0) {
        const auto exe = executable.string();
        const auto config = config_path.string();
        execl(exe.c_str(), exe.c_str(), "--mine", config.c_str(),
              static_cast<char*>(nullptr));
        _exit(127);
    }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    int status = 0;
    while (std::chrono::steady_clock::now() < deadline) {
        const pid_t waited = waitpid(child, &status, WNOHANG);
        if (waited == child) return WIFEXITED(status) ? WEXITSTATUS(status) : 128;
        if (waited < 0 && errno != EINTR) return -1;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    kill(child, SIGKILL);
    waitpid(child, &status, 0);
    return 124;
#endif
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

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "usage: loopback_test <unique-rx executable>\n";
        return 2;
    }
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
    if (bind_loopback(listener, address) != 0 || listen(listener, 2) != 0) {
        unique_rx::platform::close_socket(listener);
        return 1;
    }
    SocketLength address_size = static_cast<SocketLength>(sizeof(address));
    if (get_local_address(listener, address, address_size) != 0) {
        unique_rx::platform::close_socket(listener);
        return 1;
    }

    const auto closed_port = reserve_closed_port();
    if (closed_port == 0) {
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
        // This test exercises failover deterministically; production defaults
        // retain XMRig-style retries and exponential backoff.
        {"retries", 0},
        {"retry-pause", 0},
    };
    {
        std::ofstream file(config_path);
        if (!file) {
            unique_rx::platform::close_socket(listener);
            return 1;
        }
        file << config.dump(2);
        if (!file) {
            file.close();
            std::filesystem::remove(config_path);
            unique_rx::platform::close_socket(listener);
            return 1;
        }
    }
    std::atomic<bool> server_ok{false};
    std::jthread pool([&] {
        try {
            bool first_session_ok = false;
            bool second_session_ok = false;
            const auto valid_login_message = [](const nlohmann::json& login) {
                return login.value("jsonrpc", std::string()) == "2.0" &&
                    login.value("id", 0) == 1 &&
                    login.value("method", std::string()) == "login" &&
                    login.contains("params") && login["params"].is_object() &&
                    login["params"].value("login", std::string()) == "loopback-test-wallet" &&
                    login["params"].value("pass", std::string()) == "x" &&
                    login["params"].value("rigid", std::string()) == "test" &&
                    login["params"].value("agent", std::string()) ==
                        "UniqueRX/1.0 (C++ RandomX; XMRig-compatible Stratum)" &&
                    login["params"].contains("algo") &&
                    login["params"]["algo"] == nlohmann::json::array({"rx/0"});
            };
            const auto valid_submit_message = [](const nlohmann::json& submit) {
                return submit.value("jsonrpc", std::string()) == "2.0" &&
                    submit.value("id", 0ULL) > 1 &&
                    submit.value("method", std::string()) == "submit" &&
                    submit.contains("params") && submit["params"].is_object() &&
                    submit["params"].value("id", std::string()) == "local-session" &&
                    submit["params"].value("job_id", std::string()) == "local-job" &&
                    is_hex_string(submit["params"].value("nonce", std::string()), 8) &&
                    is_hex_string(submit["params"].value("result", std::string()), 64);
            };
            const nlohmann::json job{
                {"job_id", "local-job"},
                {"blob", std::string(76 * 2, '0')},
                {"seed_hash", std::string(64, '0')},
                {"target", "ffffffff"},
                {"height", 1},
                {"algo", "rx/0"},
            };
            const nlohmann::json login_response{
                {"id", 1}, {"jsonrpc", "2.0"},
                // Standard XMRig-compatible pools may omit the optional status field.
                {"result", {{"id", "local-session"}, {"job", job}}},
            };

            // First authenticated session: accept one share, then deliberately
            // drop the established TCP connection to exercise reconnect handling.
            for (int connection_index = 0; connection_index < 2; ++connection_index) {
                if (!unique_rx::platform::wait_socket(
                        listener, unique_rx::platform::kReadEvent, 20000)) {
                    return;
                }
                const Socket client = accept(listener, nullptr, nullptr);
                if (!unique_rx::platform::socket_valid(client)) return;
                if (!set_receive_timeout(client)) {
                    unique_rx::platform::close_socket(client);
                    return;
                }
                std::string line;
                if (!read_line(client, line)) {
                    unique_rx::platform::close_socket(client);
                    return;
                }
                const auto login = nlohmann::json::parse(line);
                if (!valid_login_message(login) ||
                    !send_all(client, login_response.dump() + "\n")) {
                    unique_rx::platform::close_socket(client);
                    return;
                }

                if (connection_index == 0) {
                    line.clear();
                    if (!read_line(client, line)) {
                        unique_rx::platform::close_socket(client);
                        return;
                    }
                    const auto submit = nlohmann::json::parse(line);
                    if (!valid_submit_message(submit)) {
                        unique_rx::platform::close_socket(client);
                        return;
                    }
                    // Match the explicit successful acknowledgement required by the
                    // native Stratum share-response validator.
                    const nlohmann::json accepted{
                        {"id", submit.value("id", 0ULL)}, {"jsonrpc", "2.0"},
                        {"result", {{"status", "OK"}}},
                    };
                    first_session_ok = send_all(client, accepted.dump() + "\n");
                    unique_rx::platform::close_socket(client);
                    if (!first_session_ok) return;
                    continue;
                }

                bool valid_submits = true;
                for (int index = 0; index < 3; ++index) {
                    line.clear();
                    if (!read_line(client, line)) {
                        valid_submits = false;
                        break;
                    }
                    const auto submit = nlohmann::json::parse(line);
                    if (!valid_submit_message(submit)) {
                        valid_submits = false;
                        break;
                    }
                    const nlohmann::json submit_response = index == 0
                        ? nlohmann::json{{"id", submit.value("id", 0ULL)}, {"jsonrpc", "2.0"},
                                         {"error", nlohmann::json::array({-1, "Stale share", nullptr})}}
                        : index == 1
                            ? nlohmann::json{{"id", submit.value("id", 0ULL)}, {"jsonrpc", "2.0"},
                                             {"error", nlohmann::json::array({1, "Low difficulty share", nullptr})}}
                            : nlohmann::json{{"id", submit.value("id", 0ULL)}, {"jsonrpc", "2.0"},
                                             {"result", {{"status", "OK"}}}};
                    if (!send_all(client, submit_response.dump() + "\n")) {
                        valid_submits = false;
                        break;
                    }
                }
                if (!valid_submits) {
                    unique_rx::platform::close_socket(client);
                    return;
                }
                second_session_ok = true;

                // Ask the miner to stop through the same atomic command-file
                // channel used by the local dashboard. This exercises graceful
                // shutdown without raising a process-wide CRT signal from the
                // mock-pool thread (which can fast-fail on Windows runners).
                const auto command_path = config_path.parent_path() /
                    "unique-rx-command.json";
                const auto issued_at = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::system_clock::now().time_since_epoch()).count();
                {
                    std::ofstream command_file(command_path, std::ios::binary | std::ios::trunc);
                    if (!command_file) {
                        unique_rx::platform::close_socket(client);
                        return;
                    }
                    command_file << nlohmann::json{
                        {"command", "stop"}, {"timestamp_unix_ms", issued_at}
                    }.dump() << '\n';
                    command_file.flush();
                    if (!command_file) {
                        command_file.close();
                        unique_rx::platform::close_socket(client);
                        return;
                    }
                }

                // Continue servicing the miner's orderly two-second queue drain
                // until it closes the socket.
                line.clear();
                while (read_line(client, line)) {
                    const auto submit = nlohmann::json::parse(line);
                    if (!valid_submit_message(submit)) {
                        second_session_ok = false;
                        break;
                    }
                    const nlohmann::json accepted{
                        {"id", submit.value("id", 0ULL)}, {"jsonrpc", "2.0"},
                        {"result", {{"status", "OK"}}},
                    };
                    if (!send_all(client, accepted.dump() + "\n")) {
                        second_session_ok = false;
                        break;
                    }
                    line.clear();
                }
                unique_rx::platform::close_socket(client);
            }
            server_ok.store(first_session_ok && second_session_ok,
                            std::memory_order_release);
        } catch (const std::exception& error) {
            std::cerr << "mock pool error: " << error.what() << '\n';
        } catch (...) {
            std::cerr << "mock pool error: unknown exception\n";
        }
    });

    // Run the real miner executable in a child process.
    std::cerr << "loopback: child start\n";
    const int result = run_miner_process(std::filesystem::path(argv[1]), config_path);
    std::cerr << "loopback: child exit=" << result << '\n';
    pool.join();
    std::cerr << "loopback: server joined\n";
    unique_rx::platform::close_socket(listener);
    std::cerr << "loopback: listener closed\n";

    const auto log_path = config_path.parent_path() / "unique-rx-stratum.log";
    const auto status_path = config_path.parent_path() / "unique-rx-status.json";
    std::ifstream log_file(log_path, std::ios::binary);
    std::ostringstream log_contents;
    if (log_file) log_contents << log_file.rdbuf();
    const std::string miner_log = log_contents.str();

    std::ifstream status_file(status_path, std::ios::binary);
    std::ostringstream status_contents;
    if (status_file) status_contents << status_file.rdbuf();
    const std::string miner_status = status_contents.str();
    std::cerr << "loopback: files read\n";

    std::cerr << "loopback: cleanup begin\\n";
    std::error_code cleanup_error;
    std::filesystem::remove(config_path, cleanup_error);
    std::cerr << "loopback: config removed\\n";
    cleanup_error.clear();
    std::filesystem::remove(log_path, cleanup_error);
    std::cerr << "loopback: log removed\\n";
    cleanup_error.clear();
    std::filesystem::remove(config_path.parent_path() / "unique-rx-command.json", cleanup_error);
    std::cerr << "loopback: command removed\\n";
    cleanup_error.clear();
    std::filesystem::remove(status_path, cleanup_error);
    std::cerr << "loopback: status removed\\n";
    const bool server_ok_value = server_ok.load(std::memory_order_acquire);
    std::cerr << "loopback: validate begin result=" << result
              << " server_ok=" << server_ok_value << "\\n";
    if (result != 0 || !server_ok_value ||
        miner_log.find("switching pool") == std::string::npos ||
        miner_log.find("disconnected") == std::string::npos ||
        miner_log.find("pool login accepted") == std::string::npos ||
        miner_log.find("new job local-job") == std::string::npos ||
        miner_log.find("share rejected") == std::string::npos ||
        miner_log.find("stale share") == std::string::npos ||
        miner_status.find(R"("accepted":1)") == std::string::npos) {
        std::cerr << "local Stratum reconnect/login/job/share acceptance test failed"
                  << " result=" << result
                  << " server_ok=" << server_ok.load(std::memory_order_acquire)
                  << "\n--- miner log ---\n" << miner_log
                  << "\n--- miner status stdout ---\n" << miner_status
                  << "\n";
        return 1;
    }
    std::cout << "Local Stratum established-connection reconnect, login/job, accepted, rejected and stale share handling passed.\n";
    return 0;
}
