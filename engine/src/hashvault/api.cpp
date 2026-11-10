#include "api.hpp"

#include "platform/socket.hpp"

#include <openssl/err.h>
#include <openssl/ssl.h>
#include <openssl/x509v3.h>

#ifdef _WIN32
#include <wincrypt.h>
#endif

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstring>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#ifdef _WIN32
#include <winsock2.h>
#else
#include <sys/time.h>
#endif

namespace unique_rx::hashvault {
namespace {

struct SocketCloser {
    void operator()(platform::Socket* socket) const noexcept {
        if (socket) {
            platform::close_socket(*socket);
            delete socket;
        }
    }
};
struct SslContextCloser {
    void operator()(SSL_CTX* value) const noexcept { SSL_CTX_free(value); }
};
struct SslCloser {
    void operator()(SSL* value) const noexcept { SSL_free(value); }
};

std::string lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

#ifdef _WIN32
bool import_windows_store(SSL_CTX* context, const char* store_name) {
    HCERTSTORE store = CertOpenSystemStoreA(nullptr, store_name);
    if (!store) return false;
    std::size_t imported = 0;
    PCCERT_CONTEXT certificate = nullptr;
    while ((certificate = CertEnumCertificatesInStore(store, certificate)) != nullptr) {
        const unsigned char* encoded = certificate->pbCertEncoded;
        X509* parsed = d2i_X509(nullptr, &encoded,
                                static_cast<long>(certificate->cbCertEncoded));
        if (parsed) {
            if (X509_STORE_add_cert(SSL_CTX_get_cert_store(context), parsed) == 1)
                ++imported;
            else
                ERR_clear_error(); // Certificato già presente nello store OpenSSL.
            X509_free(parsed);
        }
    }
    CertCloseStore(store, 0);
    return imported != 0;
}
#endif

bool configure_trust_store(SSL_CTX* context) {
#ifdef _WIN32
    // OpenSSL non legge automaticamente lo store radice di Windows.
    // Importiamo le CA attendibili dell'utente/macchina senza disabilitare la verifica TLS.
    const bool roots = import_windows_store(context, "ROOT");
    const bool intermediates = import_windows_store(context, "CA");
    if (roots || intermediates) return true;
#endif
    return SSL_CTX_set_default_verify_paths(context) == 1;
}

std::string decode_chunked(const std::string& input) {
    std::string output;
    std::size_t position = 0;
    while (position < input.size()) {
        const auto end = input.find("\r\n", position);
        if (end == std::string::npos) break;
        const auto size_text = input.substr(position, end - position);
        std::size_t chunk_size = 0;
        try { chunk_size = std::stoul(size_text, nullptr, 16); }
        catch (...) { break; }
        position = end + 2;
        if (chunk_size == 0 || position + chunk_size > input.size()) break;
        output.append(input, position, chunk_size);
        position += chunk_size + 2;
    }
    return output;
}

bool get_https(const std::string& path, std::string& body, std::string& error) {
    platform::NetworkRuntime network;
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* raw_addresses = nullptr;
    const int lookup = getaddrinfo("api.hashvault.pro", "443", &hints, &raw_addresses);
    if (lookup != 0) {
        error = "DNS HashVault non disponibile";
        return false;
    }
    std::unique_ptr<addrinfo, decltype(&freeaddrinfo)> addresses(raw_addresses, freeaddrinfo);
    std::unique_ptr<platform::Socket, SocketCloser> socket;
    for (auto* address = addresses.get(); address; address = address->ai_next) {
        const auto candidate = ::socket(address->ai_family, address->ai_socktype, address->ai_protocol);
        if (!platform::socket_valid(candidate)) continue;
        auto holder = std::unique_ptr<platform::Socket, SocketCloser>(
            new platform::Socket(candidate));
#ifdef _WIN32
        DWORD timeout = 8000;
        setsockopt(candidate, SOL_SOCKET, SO_RCVTIMEO,
                   reinterpret_cast<const char*>(&timeout), sizeof(timeout));
        setsockopt(candidate, SOL_SOCKET, SO_SNDTIMEO,
                   reinterpret_cast<const char*>(&timeout), sizeof(timeout));
#else
        timeval timeout{8, 0};
        setsockopt(candidate, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
        setsockopt(candidate, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
#endif
        if (platform::connect_socket(candidate, address->ai_addr,
#ifdef _WIN32
                                     static_cast<int>(address->ai_addrlen)
#else
                                     address->ai_addrlen
#endif
                                     ) == 0) {
            socket = std::move(holder);
            break;
        }
    }
    if (!socket) {
        error = "Connessione HTTPS HashVault non riuscita";
        return false;
    }

    std::unique_ptr<SSL_CTX, SslContextCloser> context(SSL_CTX_new(TLS_client_method()));
    if (!context || !configure_trust_store(context.get())) {
        error = "Store CA attendibili non disponibile per TLS HashVault";
        return false;
    }
    SSL_CTX_set_verify(context.get(), SSL_VERIFY_PEER, nullptr);
    std::unique_ptr<SSL, SslCloser> ssl(SSL_new(context.get()));
    if (!ssl) {
        error = "Creazione sessione TLS fallita";
        return false;
    }
    if (SSL_set_tlsext_host_name(ssl.get(), "api.hashvault.pro") != 1 ||
        SSL_set1_host(ssl.get(), "api.hashvault.pro") != 1 ||
        SSL_set_fd(ssl.get(), static_cast<int>(*socket)) != 1) {
        error = "Configurazione TLS/SNI HashVault fallita";
        return false;
    }
    if (SSL_connect(ssl.get()) != 1 ||
        SSL_get_verify_result(ssl.get()) != X509_V_OK) {
        const long verify = SSL_get_verify_result(ssl.get());
        error = "Verifica TLS HashVault fallita (codice " + std::to_string(verify) + ")";
        return false;
    }

    const std::string request =
        "GET " + path + " HTTP/1.1\r\n"
        "Host: api.hashvault.pro\r\n"
        "Accept: application/json\r\n"
        "User-Agent: UNIQUE-RX/0.3\r\n"
        "Connection: close\r\n\r\n";
    std::size_t sent = 0;
    while (sent < request.size()) {
        const int count = SSL_write(ssl.get(), request.data() + sent,
                                    static_cast<int>(request.size() - sent));
        if (count <= 0) {
            error = "Invio richiesta API HashVault fallito";
            return false;
        }
        sent += static_cast<std::size_t>(count);
    }

    std::string response;
    std::array<char, 8192> buffer{};
    for (;;) {
        const int count = SSL_read(ssl.get(), buffer.data(),
                                   static_cast<int>(buffer.size()));
        if (count > 0) {
            response.append(buffer.data(), static_cast<std::size_t>(count));
            if (response.size() > 4 * 1024 * 1024) {
                error = "Risposta API HashVault troppo grande";
                return false;
            }
            continue;
        }
        const int ssl_error = SSL_get_error(ssl.get(), count);
        if (ssl_error == SSL_ERROR_ZERO_RETURN) break;
        if (ssl_error == SSL_ERROR_SYSCALL && count == 0) break;
        if (ssl_error == SSL_ERROR_WANT_READ || ssl_error == SSL_ERROR_WANT_WRITE) continue;
        error = "Lettura risposta API HashVault fallita";
        return false;
    }

    const auto header_end = response.find("\r\n\r\n");
    if (header_end == std::string::npos) {
        error = "Risposta HTTP HashVault non valida";
        return false;
    }
    const auto headers = lower(response.substr(0, header_end));
    if (headers.find(" 200 ") == std::string::npos) {
        error = "HTTP HashVault: " + response.substr(0, response.find("\r\n"));
        return false;
    }
    body = response.substr(header_end + 4);
    if (headers.find("transfer-encoding: chunked") != std::string::npos)
        body = decode_chunked(body);
    return true;
}

std::string encode_path(const std::string& value) {
    static constexpr char hex[] = "0123456789ABCDEF";
    std::string output;
    for (const unsigned char c : value) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~')
            output.push_back(static_cast<char>(c));
        else {
            output.push_back('%');
            output.push_back(hex[c >> 4]);
            output.push_back(hex[c & 15]);
        }
    }
    return output;
}

} // namespace

bool fetch_json(const std::string& path, nlohmann::json& output,
                std::string& error) noexcept {
    try {
        std::string body;
        if (!get_https(path, body, error)) return false;
        output = nlohmann::json::parse(body);
        return true;
    } catch (const std::exception& exception) {
        error = exception.what();
        return false;
    } catch (...) {
        error = "Errore API HashVault sconosciuto";
        return false;
    }
}

void refresh(Snapshot& snapshot, const std::string& wallet, bool solo,
             const std::string& period, unsigned inactivity_minutes) noexcept {
    nlohmann::json pool;
    std::string pool_error;
    if (fetch_json("/v3/monero", pool, pool_error)) {
        snapshot.pool = std::move(pool);
        snapshot.pool_ready = true;
        snapshot.pool_error.clear();
    } else {
        snapshot.pool_error = std::move(pool_error);
    }

    if (wallet.empty()) {
        snapshot.wallet_ready = false;
        snapshot.wallet_error = "Wallet non presente nel pool configurato";
        return;
    }
    const auto safe_period = period == "weekly" || period == "monthly" ? period : "daily";
    const auto threshold = std::clamp(inactivity_minutes, 1U, 10080U);
    const auto path = "/v3/monero/wallet/" + encode_path(wallet) +
        "/stats?chart=true&poolType=" + std::string(solo ? "true" : "false") +
        "&workers=true&period=" + safe_period +
        "&inactivityThreshold=" + std::to_string(threshold);
    nlohmann::json wallet_data;
    std::string wallet_error;
    if (fetch_json(path, wallet_data, wallet_error)) {
        snapshot.wallet = std::move(wallet_data);
        snapshot.wallet_ready = true;
        snapshot.wallet_error.clear();
    } else {
        snapshot.wallet_error = std::move(wallet_error);
    }
}

} // namespace unique_rx::hashvault
