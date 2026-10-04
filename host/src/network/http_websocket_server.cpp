#include "network/http_websocket_server.h"

#include "network/network_addresses.h"

#include <WinSock2.h>
#include <WS2tcpip.h>
#include <Windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <fstream>
#include <iostream>
#include <iterator>
#include <mutex>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace remote_touch::network {
namespace {

constexpr std::size_t kMaxHttpHeaderSize = 16 * 1024;
constexpr std::uint64_t kMaxWebSocketPayload = 64 * 1024;
constexpr std::string_view kWebSocketGuid =
    "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

class SocketHandle {
public:
    explicit SocketHandle(const SOCKET socket = INVALID_SOCKET) noexcept
        : socket_(socket) {}
    ~SocketHandle() { reset(); }

    SocketHandle(const SocketHandle&) = delete;
    SocketHandle& operator=(const SocketHandle&) = delete;

    [[nodiscard]] SOCKET get() const noexcept { return socket_; }
    [[nodiscard]] SOCKET release() noexcept {
        return std::exchange(socket_, INVALID_SOCKET);
    }
    void reset(const SOCKET socket = INVALID_SOCKET) noexcept {
        if (socket_ != INVALID_SOCKET) {
            closesocket(socket_);
        }
        socket_ = socket;
    }

private:
    SOCKET socket_;
};

struct HttpRequest {
    std::string method;
    std::string target;
    std::unordered_map<std::string, std::string> headers;
};

[[nodiscard]] std::string trim(std::string_view value) {
    while (!value.empty() &&
           std::isspace(static_cast<unsigned char>(value.front()))) {
        value.remove_prefix(1);
    }
    while (!value.empty() &&
           std::isspace(static_cast<unsigned char>(value.back()))) {
        value.remove_suffix(1);
    }
    return std::string{value};
}

[[nodiscard]] std::string lowercase(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](const char ch) {
        return static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    });
    return value;
}

[[nodiscard]] bool send_all(const SOCKET socket, const char* data,
                            std::size_t size) noexcept {
    while (size > 0) {
        const auto chunk = static_cast<int>(std::min<std::size_t>(size, INT_MAX));
        const int sent = send(socket, data, chunk, 0);
        if (sent <= 0) {
            return false;
        }
        data += sent;
        size -= static_cast<std::size_t>(sent);
    }
    return true;
}

[[nodiscard]] bool recv_exact(const SOCKET socket, std::byte* destination,
                              std::size_t size) noexcept {
    while (size > 0) {
        const auto chunk = static_cast<int>(std::min<std::size_t>(size, INT_MAX));
        const int received =
            recv(socket, reinterpret_cast<char*>(destination), chunk, 0);
        if (received <= 0) {
            return false;
        }
        destination += received;
        size -= static_cast<std::size_t>(received);
    }
    return true;
}

[[nodiscard]] std::optional<HttpRequest> read_http_request(const SOCKET socket) {
    std::string buffer;
    buffer.reserve(2048);
    std::array<char, 2048> chunk{};

    while (buffer.find("\r\n\r\n") == std::string::npos) {
        const int received = recv(socket, chunk.data(), static_cast<int>(chunk.size()), 0);
        if (received <= 0) {
            return std::nullopt;
        }
        buffer.append(chunk.data(), static_cast<std::size_t>(received));
        if (buffer.size() > kMaxHttpHeaderSize) {
            return std::nullopt;
        }
    }

    std::istringstream stream{buffer};
    HttpRequest request;
    std::string version;
    if (!(stream >> request.method >> request.target >> version) ||
        version.rfind("HTTP/", 0) != 0) {
        return std::nullopt;
    }

    std::string line;
    std::getline(stream, line);
    while (std::getline(stream, line) && line != "\r") {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        const auto separator = line.find(':');
        if (separator == std::string::npos) {
            continue;
        }
        request.headers.emplace(lowercase(line.substr(0, separator)),
                                trim(std::string_view{line}.substr(separator + 1)));
    }
    return request;
}

[[nodiscard]] bool secure_equals(const std::string_view left,
                                 const std::string_view right) noexcept {
    if (left.size() != right.size()) {
        return false;
    }
    unsigned char difference = 0;
    for (std::size_t index = 0; index < left.size(); ++index) {
        difference |= static_cast<unsigned char>(left[index] ^ right[index]);
    }
    return difference == 0;
}

[[nodiscard]] std::string_view request_path(const std::string_view target) {
    return target.substr(0, target.find('?'));
}

[[nodiscard]] std::optional<std::string_view> query_token(
    const std::string_view target) {
    const auto question = target.find('?');
    if (question == std::string_view::npos) {
        return std::nullopt;
    }
    auto query = target.substr(question + 1);
    while (!query.empty()) {
        const auto ampersand = query.find('&');
        const auto item = query.substr(0, ampersand);
        if (item.rfind("token=", 0) == 0) {
            return item.substr(6);
        }
        if (ampersand == std::string_view::npos) {
            break;
        }
        query.remove_prefix(ampersand + 1);
    }
    return std::nullopt;
}

[[nodiscard]] std::optional<std::string_view> query_parameter(
    const std::string_view target, const std::string_view name) {
    const auto question = target.find('?');
    if (question == std::string_view::npos) {
        return std::nullopt;
    }
    const std::string prefix = std::string{name} + '=';
    auto query = target.substr(question + 1);
    while (!query.empty()) {
        const auto ampersand = query.find('&');
        const auto item = query.substr(0, ampersand);
        if (item.rfind(prefix, 0) == 0) {
            return item.substr(prefix.size());
        }
        if (ampersand == std::string_view::npos) {
            break;
        }
        query.remove_prefix(ampersand + 1);
    }
    return std::nullopt;
}

[[nodiscard]] std::string url_decode(const std::string_view encoded) {
    const auto hex_value = [](const char character) -> int {
        if (character >= '0' && character <= '9') return character - '0';
        if (character >= 'a' && character <= 'f') return character - 'a' + 10;
        if (character >= 'A' && character <= 'F') return character - 'A' + 10;
        return -1;
    };

    std::string decoded;
    decoded.reserve(encoded.size());
    for (std::size_t index = 0; index < encoded.size(); ++index) {
        if (encoded[index] == '+') {
            decoded.push_back(' ');
            continue;
        }
        if (encoded[index] == '%' && index + 2 < encoded.size()) {
            const int high = hex_value(encoded[index + 1]);
            const int low = hex_value(encoded[index + 2]);
            if (high >= 0 && low >= 0) {
                decoded.push_back(static_cast<char>((high << 4) | low));
                index += 2;
                continue;
            }
        }
        decoded.push_back(encoded[index]);
    }
    return decoded;
}

[[nodiscard]] bool cookie_has_token(const std::string_view cookie,
                                    const std::string_view token) {
    auto remaining = cookie;
    while (!remaining.empty()) {
        const auto separator = remaining.find(';');
        const auto item = trim(remaining.substr(0, separator));
        constexpr std::string_view prefix = "rt_session=";
        if (item.rfind(prefix, 0) == 0 &&
            secure_equals(std::string_view{item}.substr(prefix.size()), token)) {
            return true;
        }
        if (separator == std::string_view::npos) {
            break;
        }
        remaining.remove_prefix(separator + 1);
    }
    return false;
}

struct AuthenticationResult {
    bool accepted{false};
    bool set_cookie{false};
};

[[nodiscard]] AuthenticationResult authenticate(
    const HttpRequest& request, const std::string_view token) {
    if (const auto supplied = query_token(request.target);
        supplied && secure_equals(*supplied, token)) {
        return {.accepted = true, .set_cookie = true};
    }
    if (const auto cookie = request.headers.find("cookie");
        cookie != request.headers.end() && cookie_has_token(cookie->second, token)) {
        return {.accepted = true, .set_cookie = false};
    }
    return {};
}

void send_response(const SOCKET socket, const int status,
                   const std::string_view reason, const std::string_view content_type,
                   const std::string_view body, const std::string_view extra_headers = {}) {
    std::ostringstream header;
    header << "HTTP/1.1 " << status << ' ' << reason << "\r\n"
           << "Content-Type: " << content_type << "\r\n"
           << "Content-Length: " << body.size() << "\r\n"
           << "X-Content-Type-Options: nosniff\r\n"
           << "Cache-Control: no-store\r\n"
           << extra_headers << "Connection: close\r\n\r\n";
    const auto text = header.str();
    (void)send_all(socket, text.data(), text.size());
    (void)send_all(socket, body.data(), body.size());
}

[[nodiscard]] std::optional<std::pair<std::filesystem::path, std::string_view>>
resolve_asset(const std::filesystem::path& root, const std::string_view path) {
    static constexpr std::array assets{
        std::pair{std::string_view{"/index.html"}, std::string_view{"text/html; charset=utf-8"}},
        std::pair{std::string_view{"/manifest.webmanifest"}, std::string_view{"application/manifest+json"}},
        std::pair{std::string_view{"/styles/main.css"}, std::string_view{"text/css; charset=utf-8"}},
        std::pair{std::string_view{"/js/app.js"}, std::string_view{"text/javascript; charset=utf-8"}},
        std::pair{std::string_view{"/js/diagnostics.js"}, std::string_view{"text/javascript; charset=utf-8"}},
        std::pair{std::string_view{"/js/input/touch-controller.js"}, std::string_view{"text/javascript; charset=utf-8"}},
        std::pair{std::string_view{"/js/input/viewport-zoom-controller.js"}, std::string_view{"text/javascript; charset=utf-8"}},
        std::pair{std::string_view{"/js/network/websocket-transport.js"}, std::string_view{"text/javascript; charset=utf-8"}},
        std::pair{std::string_view{"/js/video/fmp4-muxer.js"}, std::string_view{"text/javascript; charset=utf-8"}},
        std::pair{std::string_view{"/js/video/screen-stream.js"}, std::string_view{"text/javascript; charset=utf-8"}},
        std::pair{std::string_view{"/js/protocol/touch-packet.js"}, std::string_view{"text/javascript; charset=utf-8"}},
    };

    const auto normalized = path == "/" ? std::string_view{"/index.html"} : path;
    const auto asset = std::find_if(assets.begin(), assets.end(),
                                    [normalized](const auto& candidate) {
                                        return candidate.first == normalized;
                                    });
    if (asset == assets.end()) {
        return std::nullopt;
    }
    return std::pair{root / normalized.substr(1), asset->second};
}

[[nodiscard]] std::optional<std::string> read_file(
    const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return std::nullopt;
    }
    return std::string{std::istreambuf_iterator<char>{file},
                       std::istreambuf_iterator<char>{}};
}

[[nodiscard]] std::string base64_encode(const std::span<const unsigned char> bytes) {
    static constexpr std::string_view alphabet =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string output;
    output.reserve(((bytes.size() + 2) / 3) * 4);
    for (std::size_t index = 0; index < bytes.size(); index += 3) {
        const auto remaining = bytes.size() - index;
        const std::uint32_t value =
            (static_cast<std::uint32_t>(bytes[index]) << 16U) |
            (remaining > 1 ? static_cast<std::uint32_t>(bytes[index + 1]) << 8U : 0U) |
            (remaining > 2 ? static_cast<std::uint32_t>(bytes[index + 2]) : 0U);
        output.push_back(alphabet[(value >> 18U) & 0x3FU]);
        output.push_back(alphabet[(value >> 12U) & 0x3FU]);
        output.push_back(remaining > 1 ? alphabet[(value >> 6U) & 0x3FU] : '=');
        output.push_back(remaining > 2 ? alphabet[value & 0x3FU] : '=');
    }
    return output;
}

[[nodiscard]] std::optional<std::string> websocket_accept_key(
    const std::string_view client_key) {
    const std::string source = std::string{client_key} + std::string{kWebSocketGuid};
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    DWORD object_size = 0;
    DWORD hash_size = 0;
    DWORD result_size = 0;

    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA1_ALGORITHM, nullptr, 0) < 0 ||
        BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH,
                          reinterpret_cast<PUCHAR>(&object_size), sizeof(object_size),
                          &result_size, 0) < 0 ||
        BCryptGetProperty(algorithm, BCRYPT_HASH_LENGTH,
                          reinterpret_cast<PUCHAR>(&hash_size), sizeof(hash_size),
                          &result_size, 0) < 0) {
        if (algorithm) {
            BCryptCloseAlgorithmProvider(algorithm, 0);
        }
        return std::nullopt;
    }

    std::vector<unsigned char> object(object_size);
    std::vector<unsigned char> digest(hash_size);
    const bool ok =
        BCryptCreateHash(algorithm, &hash, object.data(), object_size, nullptr, 0, 0) >= 0 &&
        BCryptHashData(hash, reinterpret_cast<PUCHAR>(const_cast<char*>(source.data())),
                       static_cast<ULONG>(source.size()), 0) >= 0 &&
        BCryptFinishHash(hash, digest.data(), hash_size, 0) >= 0;

    if (hash) {
        BCryptDestroyHash(hash);
    }
    BCryptCloseAlgorithmProvider(algorithm, 0);
    return ok ? std::optional{base64_encode(digest)} : std::nullopt;
}

[[nodiscard]] bool header_contains(const HttpRequest& request,
                                   const std::string& name,
                                   const std::string_view value) {
    const auto item = request.headers.find(name);
    return item != request.headers.end() &&
           lowercase(item->second).find(lowercase(std::string{value})) !=
               std::string::npos;
}

[[nodiscard]] bool send_websocket_frame(
    const SOCKET socket, const std::uint8_t opcode,
    const std::span<const std::byte> payload) noexcept {
    std::array<std::byte, 10> header{};
    header[0] = static_cast<std::byte>(0x80U | opcode);
    std::size_t header_size = 2;
    if (payload.size() <= 125) {
        header[1] = static_cast<std::byte>(payload.size());
    } else if (payload.size() <= 0xFFFFU) {
        header[1] = std::byte{126};
        header[2] = static_cast<std::byte>((payload.size() >> 8U) & 0xFFU);
        header[3] = static_cast<std::byte>(payload.size() & 0xFFU);
        header_size = 4;
    } else {
        header[1] = std::byte{127};
        const auto size = static_cast<std::uint64_t>(payload.size());
        for (std::size_t index = 0; index < 8; ++index) {
            header[2 + index] = static_cast<std::byte>(
                (size >> ((7U - index) * 8U)) & 0xFFU);
        }
        header_size = 10;
    }
    return send_all(socket, reinterpret_cast<const char*>(header.data()), header_size) &&
           send_all(socket, reinterpret_cast<const char*>(payload.data()), payload.size());
}

void websocket_loop(const SOCKET socket,
                    const HttpWebSocketServer::BinaryHandler& on_binary,
                    std::mutex& send_mutex) {
    std::vector<std::byte> payload;
    payload.reserve(128);
    for (;;) {
        std::array<std::byte, 2> prefix{};
        if (!recv_exact(socket, prefix.data(), prefix.size())) {
            return;
        }

        const auto first = std::to_integer<std::uint8_t>(prefix[0]);
        const auto second = std::to_integer<std::uint8_t>(prefix[1]);
        const bool final = (first & 0x80U) != 0;
        const auto opcode = static_cast<std::uint8_t>(first & 0x0FU);
        const bool masked = (second & 0x80U) != 0;
        std::uint64_t payload_size = second & 0x7FU;
        if (!final || !masked) {
            return;
        }

        if (payload_size == 126) {
            std::array<std::byte, 2> extended{};
            if (!recv_exact(socket, extended.data(), extended.size())) {
                return;
            }
            payload_size =
                (static_cast<std::uint64_t>(std::to_integer<std::uint8_t>(extended[0])) << 8U) |
                std::to_integer<std::uint8_t>(extended[1]);
        } else if (payload_size == 127) {
            std::array<std::byte, 8> extended{};
            if (!recv_exact(socket, extended.data(), extended.size())) {
                return;
            }
            payload_size = 0;
            for (const auto value : extended) {
                payload_size = (payload_size << 8U) |
                               std::to_integer<std::uint8_t>(value);
            }
        }
        if (payload_size > kMaxWebSocketPayload ||
            ((opcode & 0x08U) != 0 && payload_size > 125)) {
            return;
        }

        std::array<std::byte, 4> mask{};
        if (!recv_exact(socket, mask.data(), mask.size())) {
            return;
        }
        payload.resize(static_cast<std::size_t>(payload_size));
        if (!payload.empty() &&
            !recv_exact(socket, payload.data(), payload.size())) {
            return;
        }
        for (std::size_t index = 0; index < payload.size(); ++index) {
            payload[index] ^= mask[index % mask.size()];
        }

        switch (opcode) {
            case 0x2:
                on_binary(payload);
                break;
            case 0x8:
                {
                    std::scoped_lock lock(send_mutex);
                    (void)send_websocket_frame(socket, 0x8, payload);
                }
                return;
            case 0x9:
                {
                    std::scoped_lock lock(send_mutex);
                    if (!send_websocket_frame(socket, 0xA, payload)) {
                        return;
                    }
                }
                break;
            case 0xA:
                break;
            default:
                return;
        }
    }
}

}  // namespace

struct HttpWebSocketServer::Impl {
    struct Worker {
        std::jthread thread;
        std::shared_ptr<std::atomic_bool> completed;
    };

    Impl(ServerConfig server_config, BinaryHandler binary_handler,
         DisconnectHandler disconnect_handler)
        : config(std::move(server_config)),
          on_binary(std::move(binary_handler)),
          on_disconnect(std::move(disconnect_handler)) {
        WSADATA data{};
        if (WSAStartup(MAKEWORD(2, 2), &data) != 0) {
            throw std::runtime_error("WSAStartup failed");
        }
        local_addresses = private_ipv4_addresses();
        local_addresses.emplace_back("127.0.0.1");
    }

    ~Impl() {
        stop();
        WSACleanup();
    }

    void log(const std::string_view message) {
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - started_at);
        std::scoped_lock lock(log_mutex);
        std::cout << "[net +" << elapsed.count() << "ms] " << message << '\n'
                  << std::flush;
    }

    [[nodiscard]] bool is_local_peer(const std::string_view peer) const {
        return std::find(local_addresses.begin(), local_addresses.end(), peer) !=
               local_addresses.end();
    }

    void start() {
        if (running.exchange(true)) {
            return;
        }

        SocketHandle candidate{socket(AF_INET, SOCK_STREAM, IPPROTO_TCP)};
        if (candidate.get() == INVALID_SOCKET) {
            running = false;
            throw std::runtime_error("Unable to create listening socket");
        }

        BOOL exclusive = TRUE;
        (void)setsockopt(candidate.get(), SOL_SOCKET, SO_EXCLUSIVEADDRUSE,
                         reinterpret_cast<const char*>(&exclusive), sizeof(exclusive));

        sockaddr_in endpoint{};
        endpoint.sin_family = AF_INET;
        endpoint.sin_port = htons(config.port);
        if (InetPtonA(AF_INET, config.bind_address.c_str(), &endpoint.sin_addr) != 1 ||
            bind(candidate.get(), reinterpret_cast<const sockaddr*>(&endpoint),
                 sizeof(endpoint)) == SOCKET_ERROR ||
            listen(candidate.get(), SOMAXCONN) == SOCKET_ERROR) {
            const auto error = WSAGetLastError();
            running = false;
            throw std::runtime_error("Unable to listen on " + config.bind_address + ":" +
                                     std::to_string(config.port) + " (error " +
                                     std::to_string(error) + ")");
        }

        listener = candidate.release();
        const SOCKET listening_socket = listener;
        accept_thread =
            std::jthread([this, listening_socket] { accept_loop(listening_socket); });
    }

    void stop() noexcept {
        if (!running.exchange(false)) {
            return;
        }
        if (listener != INVALID_SOCKET) {
            shutdown(listener, SD_BOTH);
            closesocket(listener);
            listener = INVALID_SOCKET;
        }
        if (accept_thread.joinable()) {
            accept_thread.join();
        }

        std::vector<SOCKET> sockets;
        {
            std::scoped_lock lock(clients_mutex);
            sockets = active_sockets;
        }
        for (const auto client : sockets) {
            shutdown(client, SD_BOTH);
        }

        std::vector<Worker> threads;
        {
            std::scoped_lock lock(clients_mutex);
            threads.swap(workers);
        }
    }

    [[nodiscard]] bool has_screen_client() const noexcept {
        return screen_ready.load(std::memory_order_acquire);
    }

    [[nodiscard]] std::uint64_t screen_session() const noexcept {
        return screen_generation.load(std::memory_order_acquire);
    }

    [[nodiscard]] bool send_screen_packet(
        const std::span<const std::byte> packet) {
        std::string diagnostic;
        bool sent = false;
        {
            std::scoped_lock lock(screen_send_mutex);
            if (!screen_ready.load(std::memory_order_relaxed) ||
                screen_socket == INVALID_SOCKET) {
                return false;
            }
            sent = send_websocket_frame(screen_socket, 0x2, packet);
            if (sent) {
                ++screen_frames_sent;
                screen_bytes_sent += packet.size();
                if (screen_frames_sent == 1 || screen_frames_sent % 150 == 0) {
                    const bool key_frame =
                        packet.size() > 5 &&
                        (std::to_integer<std::uint8_t>(packet[5]) & 1U) != 0;
                    diagnostic = "screen #" + std::to_string(screen_session_id) +
                                 " sent frames=" +
                                 std::to_string(screen_frames_sent) + " bytes=" +
                                 std::to_string(screen_bytes_sent) +
                                 " packet=" + std::to_string(packet.size()) +
                                 " key=" + (key_frame ? "1" : "0");
                }
            } else {
                const int error = WSAGetLastError();
                diagnostic = "screen #" + std::to_string(screen_session_id) +
                             " send failed WSA=" + std::to_string(error) +
                             " frames=" + std::to_string(screen_frames_sent) +
                             " bytes=" + std::to_string(screen_bytes_sent);
                screen_generation.store(0, std::memory_order_release);
                screen_ready.store(false, std::memory_order_release);
                shutdown(screen_socket, SD_BOTH);
            }
        }
        if (!diagnostic.empty()) {
            log(diagnostic);
        }
        return sent;
    }

    void accept_loop(const SOCKET listening_socket) {
        while (running) {
            sockaddr_in peer{};
            int peer_size = sizeof(peer);
            const SOCKET client = accept(
                listening_socket, reinterpret_cast<sockaddr*>(&peer), &peer_size);
            if (client == INVALID_SOCKET) {
                if (running) {
                    std::this_thread::yield();
                }
                continue;
            }
            if (!is_private_or_loopback_ipv4(ntohl(peer.sin_addr.s_addr))) {
                closesocket(client);
                continue;
            }

            std::array<char, INET_ADDRSTRLEN> peer_text{};
            const char* converted = InetNtopA(AF_INET, &peer.sin_addr,
                                               peer_text.data(),
                                               static_cast<DWORD>(peer_text.size()));
            const std::string peer_address = converted ? converted : "unknown";

            std::scoped_lock lock(clients_mutex);
            std::erase_if(workers, [](const Worker& worker) {
                return worker.completed->load(std::memory_order_acquire);
            });
            active_sockets.push_back(client);
            auto completed = std::make_shared<std::atomic_bool>(false);
            workers.push_back(Worker{
                .thread = std::jthread([this, client, completed, peer_address] {
                    SocketHandle owned{client};
                    handle_connection(client, peer_address);
                    {
                        std::scoped_lock client_lock(clients_mutex);
                        std::erase(active_sockets, client);
                    }
                    completed->store(true, std::memory_order_release);
                }),
                .completed = std::move(completed),
            });
        }
    }

    void handle_connection(const SOCKET socket, const std::string_view peer) {
        const auto request = read_http_request(socket);
        if (!request || request->method != "GET") {
            send_response(socket, 400, "Bad Request", "text/plain; charset=utf-8",
                          "Bad request\n");
            return;
        }

        const auto authentication = authenticate(*request, config.session_token);
        if (!authentication.accepted) {
            log("HTTP 403 peer=" + std::string{peer} + " target=" +
                std::string{request_path(request->target)});
            send_response(socket, 403, "Forbidden", "text/plain; charset=utf-8",
                          "Invalid or missing session token\n");
            return;
        }

        const auto path = request_path(request->target);
        const auto user_agent_item = request->headers.find("user-agent");
        const std::string_view user_agent =
            user_agent_item == request->headers.end()
                ? std::string_view{"unknown"}
                : std::string_view{user_agent_item->second};
        if (path == "/client-log") {
            const auto encoded = query_parameter(request->target, "message");
            std::string message = encoded ? url_decode(*encoded) : "missing message";
            if (message.size() > 4'096) {
                message.resize(4'096);
            }
            log("client peer=" + std::string{peer} + " " + message);
            send_response(socket, 204, "No Content", "text/plain", "");
            return;
        }
        if (path == "/input" && header_contains(*request, "upgrade", "websocket")) {
            handle_input_websocket(socket, *request, peer);
            return;
        }
        if (path == "/screen" && header_contains(*request, "upgrade", "websocket")) {
            handle_screen_websocket(socket, *request, peer, user_agent);
            return;
        }

        if (path == "/" || path == "/index.html") {
            log("page peer=" + std::string{peer} + " ua=" +
                std::string{user_agent});
        }

        const auto asset = resolve_asset(config.web_root, path);
        if (!asset) {
            send_response(socket, 404, "Not Found", "text/plain; charset=utf-8",
                          "Not found\n");
            return;
        }
        const auto body = read_file(asset->first);
        if (!body) {
            send_response(socket, 500, "Internal Server Error",
                          "text/plain; charset=utf-8",
                          "Web client asset is missing\n");
            return;
        }
        const std::string cookie_header = authentication.set_cookie
                                              ? "Set-Cookie: rt_session=" +
                                                    config.session_token +
                                                    "; HttpOnly; SameSite=Strict; Path=/\r\n"
                                              : std::string{};
        send_response(socket, 200, "OK", asset->second, *body, cookie_header);
    }

    [[nodiscard]] bool complete_websocket_handshake(
        const SOCKET socket, const HttpRequest& request) {
        const auto key = request.headers.find("sec-websocket-key");
        if (key == request.headers.end() ||
            !header_contains(request, "connection", "upgrade")) {
            send_response(socket, 400, "Bad Request", "text/plain; charset=utf-8",
                          "Invalid WebSocket upgrade\n");
            return false;
        }
        const auto accept_key = websocket_accept_key(key->second);
        if (!accept_key) {
            return false;
        }

        BOOL no_delay = TRUE;
        (void)setsockopt(socket, IPPROTO_TCP, TCP_NODELAY,
                         reinterpret_cast<const char*>(&no_delay), sizeof(no_delay));
        const std::string response =
            "HTTP/1.1 101 Switching Protocols\r\n"
            "Upgrade: websocket\r\n"
            "Connection: Upgrade\r\n"
            "Sec-WebSocket-Accept: " +
            *accept_key + "\r\n\r\n";
        return send_all(socket, response.data(), response.size());
    }

    void handle_input_websocket(const SOCKET socket, const HttpRequest& request,
                                const std::string_view peer) {
        if (is_local_peer(peer)) {
            log("input rejected peer=" + std::string{peer} +
                " reason=local-client");
            send_response(socket, 409, "Conflict", "text/plain; charset=utf-8",
                          "Open the controller on a phone or tablet\n");
            return;
        }
        bool expected = false;
        if (!websocket_claimed.compare_exchange_strong(expected, true)) {
            log("input rejected peer=" + std::string{peer} +
                " reason=already-connected");
            send_response(socket, 409, "Conflict", "text/plain; charset=utf-8",
                          "Another controller is already connected\n");
            return;
        }

        const auto release_claim = [this] {
            on_disconnect();
            websocket_claimed = false;
        };
        if (!complete_websocket_handshake(socket, request)) {
            log("input handshake failed peer=" + std::string{peer});
            release_claim();
            return;
        }
        log("input open peer=" + std::string{peer});
        std::mutex input_send_mutex;
        websocket_loop(socket, on_binary, input_send_mutex);
        log("input close peer=" + std::string{peer});
        release_claim();
    }

    void handle_screen_websocket(const SOCKET socket, const HttpRequest& request,
                                 const std::string_view peer,
                                 const std::string_view user_agent) {
        const bool local_peer = is_local_peer(peer);
        {
            std::scoped_lock lock(screen_send_mutex);
            if (screen_socket != INVALID_SOCKET && local_peer &&
                !screen_peer_is_local) {
                log("screen rejected peer=" + std::string{peer} +
                    " reason=remote-client-active");
                send_response(socket, 409, "Conflict",
                              "text/plain; charset=utf-8",
                              "A phone or tablet is already receiving the screen\n");
                return;
            }
        }
        if (!complete_websocket_handshake(socket, request)) {
            log("screen handshake failed peer=" + std::string{peer});
            return;
        }

        // Keep the kernel queue short so old video cannot accumulate behind a
        // congested Wi-Fi link. A keyframe can exceed this size; send() simply
        // blocks while the peer drains it, bounded by the timeout below.
        int send_buffer_bytes = 128 * 1024;
        (void)setsockopt(socket, SOL_SOCKET, SO_SNDBUF,
                         reinterpret_cast<const char*>(&send_buffer_bytes),
                         sizeof(send_buffer_bytes));
        DWORD send_timeout_ms = 750;
        (void)setsockopt(socket, SOL_SOCKET, SO_SNDTIMEO,
                         reinterpret_cast<const char*>(&send_timeout_ms),
                         sizeof(send_timeout_ms));
        SOCKET previous_screen = INVALID_SOCKET;
        std::uint64_t session_id = 0;
        std::uint64_t replaced_session_id = 0;
        {
            std::scoped_lock lock(screen_send_mutex);
            previous_screen = screen_socket;
            replaced_session_id = screen_session_id;
            screen_socket = socket;
            screen_session_id = ++next_screen_session_id;
            session_id = screen_session_id;
            screen_frames_sent = 0;
            screen_bytes_sent = 0;
            screen_peer_is_local = local_peer;
            screen_ready.store(true, std::memory_order_release);
            screen_generation.store(session_id, std::memory_order_release);
        }
        log("screen #" + std::to_string(session_id) + " open peer=" +
            std::string{peer} + " ua=" + std::string{user_agent});
        if (previous_screen != INVALID_SOCKET && previous_screen != socket) {
            log("screen #" + std::to_string(session_id) + " replaces #" +
                std::to_string(replaced_session_id));
            shutdown(previous_screen, SD_BOTH);
        }

        static const BinaryHandler ignore_incoming =
            [](const std::span<const std::byte>) {};
        websocket_loop(socket, ignore_incoming, screen_send_mutex);

        std::uint64_t frames_sent = 0;
        std::uint64_t bytes_sent = 0;
        bool was_current = false;
        {
            std::scoped_lock lock(screen_send_mutex);
            if (screen_socket == socket) {
                was_current = true;
                frames_sent = screen_frames_sent;
                bytes_sent = screen_bytes_sent;
                screen_generation.store(0, std::memory_order_release);
                screen_ready.store(false, std::memory_order_release);
                screen_socket = INVALID_SOCKET;
                screen_peer_is_local = false;
            }
        }
        log("screen #" + std::to_string(session_id) + " close peer=" +
            std::string{peer} + " current=" + (was_current ? "1" : "0") +
            " frames=" + std::to_string(frames_sent) + " bytes=" +
            std::to_string(bytes_sent));
    }

    ServerConfig config;
    BinaryHandler on_binary;
    DisconnectHandler on_disconnect;
    std::atomic_bool running{false};
    std::atomic_bool websocket_claimed{false};
    std::atomic_bool screen_ready{false};
    std::atomic_uint64_t screen_generation{0};
    SOCKET screen_socket{INVALID_SOCKET};
    mutable std::mutex screen_send_mutex;
    std::uint64_t next_screen_session_id{};
    std::uint64_t screen_session_id{};
    std::uint64_t screen_frames_sent{};
    std::uint64_t screen_bytes_sent{};
    bool screen_peer_is_local{false};
    SOCKET listener{INVALID_SOCKET};
    std::jthread accept_thread;
    std::mutex clients_mutex;
    std::vector<SOCKET> active_sockets;
    std::vector<Worker> workers;
    std::vector<std::string> local_addresses;
    std::chrono::steady_clock::time_point started_at{
        std::chrono::steady_clock::now()};
    std::mutex log_mutex;
};

HttpWebSocketServer::HttpWebSocketServer(ServerConfig config,
                                         BinaryHandler on_binary,
                                         DisconnectHandler on_disconnect)
    : impl_(std::make_unique<Impl>(std::move(config), std::move(on_binary),
                                   std::move(on_disconnect))) {}

HttpWebSocketServer::~HttpWebSocketServer() = default;

void HttpWebSocketServer::start() { impl_->start(); }

void HttpWebSocketServer::stop() noexcept { impl_->stop(); }

bool HttpWebSocketServer::has_screen_client() const noexcept {
    return impl_->has_screen_client();
}

std::uint64_t HttpWebSocketServer::screen_session() const noexcept {
    return impl_->screen_session();
}

bool HttpWebSocketServer::send_screen_packet(
    const std::span<const std::byte> packet) {
    return impl_->send_screen_packet(packet);
}

}  // namespace remote_touch::network
