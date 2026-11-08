#include "transport.hpp"

#include "platform/socket.hpp"

#include <openssl/evp.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace unique_rx {
namespace {

std::string hex_encode(const unsigned char* bytes, std::size_t length) {
    static constexpr char digits[] = "0123456789abcdef";
    std::string result(length * 2, '0');
    for (std::size_t index = 0; index < length; ++index) {
        result[index * 2] = digits[bytes[index] >> 4];
        result[index * 2 + 1] = digits[bytes[index] & 0x0f];
    }
    return result;
}

} // namespace

struct StratumTransport::Impl {
public:
    ~Impl() {
        if (ssl_ != nullptr) SSL_free(ssl_);
        if (context_ != nullptr) SSL_CTX_free(context_);
        platform::close_socket(socket_);
    }

    void connect_to(const PoolConfig& config) {
        addrinfo hints{};
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;
        addrinfo* addresses = nullptr;
        const int lookup = getaddrinfo(config.host.c_str(), config.port.c_str(),
                                       &hints, &addresses);
        if (lookup != 0) throw std::runtime_error(platform::address_error(lookup));

        for (auto* address = addresses; address != nullptr; address = address->ai_next) {
            socket_ = ::socket(address->ai_family, address->ai_socktype, address->ai_protocol);
            if (!platform::socket_valid(socket_)) continue;
            if (platform::connect_socket(socket_, address->ai_addr,
                                         static_cast<int>(address->ai_addrlen)) == 0) break;
            platform::close_socket(socket_);
            socket_ = platform::kInvalidSocket;
        }
        freeaddrinfo(addresses);
        if (!platform::socket_valid(socket_)) throw std::runtime_error("unable to connect to pool");

        if (config.tls) connect_tls(config);
        if (!platform::set_nonblocking(socket_)) {
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
                        wait_for(error == SSL_ERROR_WANT_READ ? platform::kReadEvent : platform::kWriteEvent, 1000);
                        continue;
                    }
                    throw std::runtime_error("TLS write to pool failed");
                }
            } else {
                count = platform::send_bytes(socket_, wire.data() + sent, wire.size() - sent);
                if (count < 0) {
                    const int error = platform::socket_error();
                    if (platform::socket_would_block(error) || platform::socket_interrupted(error)) {
                        wait_for(platform::kWriteEvent, 1000);
                        continue;
                    }
                }
                if (count <= 0) throw std::runtime_error("socket write to pool failed");
            }
            sent += static_cast<std::size_t>(count);
        }
    }

    std::vector<std::string> read_lines(int timeout_ms) {
        std::vector<std::string> lines;
        const bool tls_buffered = ssl_ != nullptr && SSL_pending(ssl_) > 0;
        if (!tls_buffered && !wait_for(platform::kReadEvent, timeout_ms)) return lines;

        std::array<char, 8192> chunk{};
        int count = 0;
        if (ssl_ != nullptr) {
            count = SSL_read(ssl_, chunk.data(), static_cast<int>(chunk.size()));
            if (count <= 0) {
                const int error = SSL_get_error(ssl_, count);
                if (error == SSL_ERROR_WANT_READ || error == SSL_ERROR_WANT_WRITE) return lines;
                if (error == SSL_ERROR_ZERO_RETURN) throw std::runtime_error("pool closed TLS connection");
                throw std::runtime_error("TLS read from pool failed");
            }
        } else {
            count = platform::receive_bytes(socket_, chunk.data(), chunk.size());
            if (count == 0) throw std::runtime_error("pool closed connection");
            if (count < 0) {
                const int error = platform::socket_error();
                if (platform::socket_would_block(error) || platform::socket_interrupted(error)) return lines;
                throw std::runtime_error("socket read from pool failed");
            }
        }

        buffer_.append(chunk.data(), static_cast<std::size_t>(count));
        if (buffer_.size() > 1024 * 1024) throw std::runtime_error("oversized Stratum response");
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
    platform::Socket socket_ = platform::kInvalidSocket;
    SSL_CTX* context_ = nullptr;
    SSL* ssl_ = nullptr;
    std::string buffer_;

    bool wait_for(short events, int timeout_ms) const {
        return platform::wait_socket(socket_, events, timeout_ms);
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
        SSL_set_fd(ssl_, static_cast<int>(socket_));
        if (SSL_connect(ssl_) != 1) throw std::runtime_error("TLS handshake with pool failed");
        if (!config.tls_fingerprint.empty()) verify_fingerprint(config.tls_fingerprint);
    }

    void verify_fingerprint(const std::string& expected) const {
        X509* certificate = SSL_get1_peer_certificate(ssl_);
        if (certificate == nullptr) throw std::runtime_error("pool did not provide a TLS certificate");
        std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
        unsigned int digest_size = 0;
        const bool computed = X509_digest(certificate, EVP_sha256(), digest.data(),
                                          &digest_size) == 1;
        X509_free(certificate);
        if (!computed || hex_encode(digest.data(), digest_size) != expected) {
            throw std::runtime_error("pool TLS certificate fingerprint mismatch");
        }
    }
};

StratumTransport::StratumTransport() : impl_(std::make_unique<Impl>()) {}
StratumTransport::~StratumTransport() = default;
StratumTransport::StratumTransport(StratumTransport&&) noexcept = default;
StratumTransport& StratumTransport::operator=(StratumTransport&&) noexcept = default;

void StratumTransport::connect_to(const PoolConfig& config) {
    impl_->connect_to(config);
}

void StratumTransport::send_json(const nlohmann::json& value) {
    impl_->send_json(value);
}

std::vector<std::string> StratumTransport::read_lines(int timeout_ms) {
    return impl_->read_lines(timeout_ms);
}

} // namespace unique_rx
