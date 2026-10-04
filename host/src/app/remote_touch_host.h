#pragma once

#include "app/host_options.h"
#include "input/windows_touch_injector.h"
#include "network/http_websocket_server.h"
#include "streaming/desktop_streamer.h"

#include <string>

namespace remote_touch::app {

class RemoteTouchHost {
public:
    explicit RemoteTouchHost(HostOptions options);
    ~RemoteTouchHost();

    RemoteTouchHost(const RemoteTouchHost&) = delete;
    RemoteTouchHost& operator=(const RemoteTouchHost&) = delete;

    void start();
    void stop() noexcept;

private:
    void handle_packet(std::span<const std::byte> bytes);

    HostOptions options_;
    std::string session_token_;
    input::WindowsTouchInjector touch_injector_;
    network::HttpWebSocketServer server_;
    streaming::DesktopStreamer desktop_streamer_;
};

}  // namespace remote_touch::app
