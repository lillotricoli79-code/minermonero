#pragma once

#include "stratum.hpp"

#include <openssl/evp.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

namespace unique_rx {

// Owns the TCP socket and optional TLS session used by the Stratum client.
// This class is intentionally isolated from job parsing and mining workers.
class StratumTransport {
public:
    StratumTransport() = default;
    StratumTransport(const StratumTransport&) = delete;
    StratumTransport& operator=(const StratumTransport&) = delete;

    ~StratumTransport() {
        if (ssl_ != nullptr) SSL_free(ssl_);
        if (context_ != nullptr) SSL_CTX_free(context_);
        if (socket_ >= 0) close(socket_);
    }

    void connect_to(const PoolConfig& config) {
        addrinfo hints{};
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;
        addrinfo* addresses = nullptr;
        const int lookup = getaddrinfo(config.host.c_str(), config.port.c_str(),
                                       &hints, &addresses);
        if (lookup != 0) throw std::runtime_error(gai_strerror(lookup));

        for (auto* address = addresses; address != nullptr; address = address->ai_next) {
            socket_ = ::socket(address->ai_family, address->ai_socktype, address->ai_protocol);
            if (socket_ < 0) continue;
            if (::connect(socket_, address->ai_addr, address->ai_addrlen) == 0) break;
            close(socket_);
            socket_ = -1;
        }
        freeaddrinfo(addresses);
        if (socket_ < 0) throw std::runtime_error("unable to connect to pool");

        if (config.tls) connect_tls(config);
        const auto flags = fcntl(socket_, F_GETFL, 0);
        if (flags < 0 || fcntl(socket_, F_SETFL, flags | O_NONBLOCK) < 0) {
            throw std::runtime_error("unable to set nonblocking pool socket");
        }
    }

    void send_json(const nlohmann::json& value) {
        const auto wire = value.dump() + "\n";
        std::size_t sent = 0;
        while (sent < wire.size()) {
            int count = 0;
            if (ssl_ != nullptr) {
                count = SSL_write(ssl_, wire.data() + sent,
                                  static_cast<int>(wire.size() - sent));
                if (count <= 0) {
                    const int error = SSL_get_error(ssl_, count);
                    if (error == SSL_ERROR_WANT_READ || error == SSL_ERROR_WANT_WRITE) {
                        wait_for(error == SSL_ERROR_WANT_READ ? POLLIN : POLLOUT, 1000);
                        continue;
                    }
                    throw std::runtime_error("TLS write to pool failed");
                }
            } else {
                count = static_cast<int>(send(socket_, wire.data() + sent,
                                               wire.size() - sent, MSG_NOSIGNAL));
                if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) {
                    wait_for(POLLOUT, 1000);
                    continue;
                }
                if (count <= 0) throw std::runtime_error("socket write to pool failed");
            }
            sent += static_cast<std::size_t>(count);
        }
    }

    std::vector<std::string> read_lines(int timeout_ms) {
        std::vector<std::string> lines;
        const bool tls_buffered = ssl_ != nullptr && SSL_pending(ssl_) > 0;
        if (!tls_buffered && !wait_for(POLLIN, timeout_ms)) return lines;

        std::array<char, 8192> chunk{};
        int count = 0;
        if (ssl_ != nullptr) {
            count = SSL_read(ssl_, chunk.data(), static_cast<int>(chunk.size()));
            if (count <= 0) {
                const int error = SSL_get_error(ssl_, count);
                if (error == SSL_ERROR_WANT_READ || error == SSL_ERROR_WANT_WRITE) return lines;
                if (error == SSL_ERROR_ZERO_RETURN) {
                    throw std::runtime_error("pool closed TLS connection");
                }
                throw std::runtime_error("TLS read from pool failed");
            }
        } else {
            count = static_cast<int>(recv(socket_, chunk.data(), chunk.size(), 0));
            if (count == 0) throw std::runtime_error("pool closed connection");
            if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) return lines;
            if (count < 0) throw std::runtime_error("socket read from pool failed");
        }

        buffer_.append(chunk.data(), static_cast<std::size_t>(count));
        if (buffer_.size() > 1024 * 1024) {
            throw std::runtime_error("oversized Stratum response");
        }
        std::size_t consumed = 0;
        while (true) {
            const auto newline = buffer_.find('\n', consumed);
            if (newline == std::string::npos) break;
            auto line = buffer_.substr(consumed, newline - consumed);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (!line.empty()) lines.push_back(std::move(line));
            consumed = newline + 1;
        }
        buffer_.erase(0, consumed);
        return lines;
    }

private:
    int socket_ = -1;
    SSL_CTX* context_ = nullptr;
    SSL* ssl_ = nullptr;
    std::string buffer_;

    bool wait_for(short events, int timeout_ms) const {
        pollfd descriptor{socket_, events, 0};
        const int result = poll(&descriptor, 1, timeout_ms);
        if (result < 0 && errno != EINTR) {
            throw std::runtime_error("pool socket poll failed");
        }
        if (result > 0 && (descriptor.revents & (POLLERR | POLLHUP | POLLNVAL))) {
            throw std::runtime_error("pool socket disconnected");
        }
        return result > 0 && (descriptor.revents & events) != 0;
    }

    void connect_tls(const PoolConfig& config) {
        context_ = SSL_CTX_new(TLS_client_method());
        if (context_ == nullptr) throw std::runtime_error("TLS context creation failed");
        SSL_CTX_set_min_proto_version(context_, TLS1_2_VERSION);
        if (config.tls_fingerprint.empty()) {
            SSL_CTX_set_verify(context_, SSL_VERIFY_PEER, nullptr);
            if (SSL_CTX_set_default_verify_paths(context_) != 1) {
                throw std::runtime_error("cannot load default TLS trust store");
            }
        } else {
            SSL_CTX_set_verify(context_, SSL_VERIFY_NONE, nullptr);
        }
        ssl_ = SSL_new(context_);
        if (ssl_ == nullptr) throw std::runtime_error("TLS session creation failed");
        SSL_set_tlsext_host_name(ssl_, config.host.c_str());
        if (config.tls_fingerprint.empty()) SSL_set1_host(ssl_, config.host.c_str());
        SSL_set_fd(ssl_, socket_);
        if (SSL_connect(ssl_) != 1) {
            throw std::runtime_error("TLS handshake with pool failed");
        }
        if (!config.tls_fingerprint.empty()) verify_fingerprint(config.tls_fingerprint);
    }

    void verify_fingerprint(const std::string& expected) const {
        X509* certificate = SSL_get1_peer_certificate(ssl_);
        if (certificate == nullptr) {
            throw std::runtime_error("pool did not provide a TLS certificate");
        }
        std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
        unsigned int digest_size = 0;
        const bool computed = X509_digest(certificate, EVP_sha256(), digest.data(),
                                          &digest_size) == 1;
        X509_free(certificate);
        if (!computed || hex_encode(digest.data(), digest_size) != expected) {
            throw std::runtime_error("pool TLS certificate fingerprint mismatch");
        }
    }

    static std::string hex_encode(const std::uint8_t* bytes, std::size_t length) {
        static constexpr char digits[] = "0123456789abcdef";
        std::string result(length * 2, '0');
        for (std::size_t index = 0; index < length; ++index) {
            result[index * 2] = digits[bytes[index] >> 4];
            result[index * 2 + 1] = digits[bytes[index] & 0x0f];
        }
        return result;
    }
};

} // namespace unique_rx
