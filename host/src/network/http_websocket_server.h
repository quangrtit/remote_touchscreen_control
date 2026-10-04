#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <span>
#include <string>

namespace remote_touch::network {

struct ServerConfig {
    std::string bind_address;
    std::uint16_t port{};
    std::filesystem::path web_root;
    std::string session_token;
};

class HttpWebSocketServer {
public:
    using BinaryHandler = std::function<void(std::span<const std::byte>)>;
    using DisconnectHandler = std::function<void()>;

    HttpWebSocketServer(ServerConfig config, BinaryHandler on_binary,
                        DisconnectHandler on_disconnect);
    ~HttpWebSocketServer();

    HttpWebSocketServer(const HttpWebSocketServer&) = delete;
    HttpWebSocketServer& operator=(const HttpWebSocketServer&) = delete;

    void start();
    void stop() noexcept;
    [[nodiscard]] bool has_screen_client() const noexcept;
    [[nodiscard]] std::uint64_t screen_session() const noexcept;
    [[nodiscard]] bool send_screen_packet(
        std::span<const std::byte> packet);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace remote_touch::network
