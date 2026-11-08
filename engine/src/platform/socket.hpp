#pragma once

#include <cstddef>
#include <string>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace unique_rx::platform {

#ifdef _WIN32
using Socket = SOCKET;
inline constexpr Socket kInvalidSocket = INVALID_SOCKET;
inline constexpr short kReadEvent = POLLRDNORM;
inline constexpr short kWriteEvent = POLLWRNORM;

class NetworkRuntime {
public:
    NetworkRuntime() {
        WSADATA data{};
        const int result = WSAStartup(MAKEWORD(2, 2), &data);
        if (result != 0) throw std::runtime_error("Winsock initialization failed");
        active_ = true;
    }
    ~NetworkRuntime() {
        if (active_) WSACleanup();
    }
    NetworkRuntime(const NetworkRuntime&) = delete;
    NetworkRuntime& operator=(const NetworkRuntime&) = delete;
private:
    bool active_ = false;
};

inline void close_socket(Socket socket) {
    if (socket != kInvalidSocket) closesocket(socket);
}
inline bool socket_valid(Socket socket) { return socket != kInvalidSocket; }
inline int socket_error() { return WSAGetLastError(); }
inline bool socket_interrupted(int error) { return error == WSAEINTR; }
inline bool socket_would_block(int error) {
    return error == WSAEWOULDBLOCK || error == WSAEINPROGRESS;
}
inline int connect_socket(Socket socket, const sockaddr* address, int length) {
    return ::connect(socket, address, length);
}
inline int send_bytes(Socket socket, const char* data, std::size_t length) {
    return ::send(socket, data, static_cast<int>(length), 0);
}
inline int receive_bytes(Socket socket, char* data, std::size_t length) {
    return ::recv(socket, data, static_cast<int>(length), 0);
}
inline bool set_nonblocking(Socket socket) {
    u_long enabled = 1;
    return ioctlsocket(socket, FIONBIO, &enabled) == 0;
}
inline bool wait_socket(Socket socket, short events, int timeout_ms) {
    WSAPOLLFD descriptor{};
    descriptor.fd = socket;
    descriptor.events = events;
    const int result = WSAPoll(&descriptor, 1, timeout_ms);
    if (result == SOCKET_ERROR) {
        const int error = socket_error();
        if (socket_interrupted(error)) return false;
        throw std::runtime_error("pool socket poll failed");
    }
    if (result > 0 && (descriptor.revents & (POLLERR | POLLHUP | POLLNVAL))) {
        throw std::runtime_error("pool socket disconnected");
    }
    return result > 0 && (descriptor.revents & events) != 0;
}
inline std::string address_error(int error) {
    const char* message = gai_strerrorA(error);
    return message != nullptr ? message : "address lookup failed";
}
#else
using Socket = int;
inline constexpr Socket kInvalidSocket = -1;
inline constexpr short kReadEvent = POLLIN;
inline constexpr short kWriteEvent = POLLOUT;

class NetworkRuntime {
public:
    NetworkRuntime() = default;
};

inline void close_socket(Socket socket) {
    if (socket != kInvalidSocket) ::close(socket);
}
inline bool socket_valid(Socket socket) { return socket >= 0; }
inline int socket_error() { return errno; }
inline bool socket_interrupted(int error) { return error == EINTR; }
inline bool socket_would_block(int error) {
    return error == EAGAIN || error == EWOULDBLOCK || error == EINPROGRESS;
}
inline int connect_socket(Socket socket, const sockaddr* address, socklen_t length) {
    return ::connect(socket, address, length);
}
inline int send_bytes(Socket socket, const char* data, std::size_t length) {
#ifdef MSG_NOSIGNAL
    return static_cast<int>(::send(socket, data, length, MSG_NOSIGNAL));
#else
    return static_cast<int>(::send(socket, data, length, 0));
#endif
}
inline int receive_bytes(Socket socket, char* data, std::size_t length) {
    return static_cast<int>(::recv(socket, data, length, 0));
}
inline bool set_nonblocking(Socket socket) {
    const int flags = fcntl(socket, F_GETFL, 0);
    return flags >= 0 && fcntl(socket, F_SETFL, flags | O_NONBLOCK) == 0;
}
inline bool wait_socket(Socket socket, short events, int timeout_ms) {
    pollfd descriptor{socket, events, 0};
    const int result = ::poll(&descriptor, 1, timeout_ms);
    if (result < 0) {
        if (socket_interrupted(socket_error())) return false;
        throw std::runtime_error("pool socket poll failed");
    }
    if (result > 0 && (descriptor.revents & (POLLERR | POLLHUP | POLLNVAL))) {
        throw std::runtime_error("pool socket disconnected");
    }
    return result > 0 && (descriptor.revents & events) != 0;
}
inline std::string address_error(int error) {
    const char* message = gai_strerror(error);
    return message != nullptr ? message : "address lookup failed";
}
#endif

} // namespace unique_rx::platform
