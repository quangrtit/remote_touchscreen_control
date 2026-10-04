#include "app/remote_touch_host.h"

#include "network/network_addresses.h"
#include "protocol/touch_packet.h"

#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <utility>

namespace remote_touch::app {
namespace {

[[nodiscard]] const char* action_name(
    const protocol::TouchAction action) noexcept {
    switch (action) {
        case protocol::TouchAction::down:
            return "DOWN";
        case protocol::TouchAction::move:
            return "MOVE";
        case protocol::TouchAction::up:
            return "UP";
        case protocol::TouchAction::cancel:
            return "CANCEL";
    }
    return "UNKNOWN";
}

}  // namespace

RemoteTouchHost::RemoteTouchHost(HostOptions options)
    : options_(std::move(options)),
      session_token_(options_.session_token.empty() ? network::make_session_token()
                                                    : options_.session_token),
      touch_injector_(10),
      server_(network::ServerConfig{
                  .bind_address = options_.bind_address,
                  .port = options_.port,
                  .web_root = options_.web_root,
                  .session_token = session_token_,
              },
              [this](const std::span<const std::byte> bytes) {
                  handle_packet(bytes);
              },
              [this] { touch_injector_.release_all(); }),
      desktop_streamer_(
          streaming::StreamConfig{
              .frames_per_second = options_.video_fps,
              .bitrate = options_.video_bitrate,
              .max_frame_width = options_.max_video_width,
          },
          [this] { return server_.screen_session(); },
          [this](const std::span<const std::byte> packet) {
              return server_.send_screen_packet(packet);
          },
          [this](const capture::DesktopGeometry geometry) {
              touch_injector_.set_target(geometry.left, geometry.top, geometry.width,
                                         geometry.height);
          },
          [](const std::string_view message) {
              std::cerr << "Desktop stream: " << message << '\n';
          }) {
    if (!std::filesystem::is_directory(options_.web_root)) {
        throw std::runtime_error("Web root does not exist: " +
                                 options_.web_root.string());
    }
}

RemoteTouchHost::~RemoteTouchHost() { stop(); }

void RemoteTouchHost::start() {
    server_.start();
    desktop_streamer_.start();

    std::cout << "Remote Touch is running. Open one of these URLs:\n";
    if (options_.bind_address != "0.0.0.0") {
        std::cout << "  http://" << options_.bind_address << ':' << options_.port
                  << "/?token=" << session_token_ << '\n';
    } else {
        for (const auto& address : network::private_ipv4_addresses()) {
            std::cout << "  http://" << address << ':' << options_.port
                      << "/?token=" << session_token_ << '\n';
        }
    }
    std::cout << "Video: H.264 " << options_.video_fps << " FPS at "
              << options_.video_bitrate / 1'000'000.0 << " Mbps, max width "
              << (options_.max_video_width == 0
                      ? std::string{"native"}
                      : std::to_string(options_.max_video_width))
              << "\n"
              << "\nPress Ctrl+C to stop.\n"
              << std::flush;
}

void RemoteTouchHost::stop() noexcept {
    desktop_streamer_.stop();
    server_.stop();
    touch_injector_.release_all();
}

void RemoteTouchHost::handle_packet(const std::span<const std::byte> bytes) {
    const auto event = protocol::decode_touch_packet(bytes);
    if (!event) {
        return;
    }

    const bool injected = touch_injector_.inject(*event);
    if (options_.verbose_input || event->action != protocol::TouchAction::move) {
        std::cout << action_name(event->action) << " id=" << event->pointer_id
                  << " x=" << event->x << " y=" << event->y
                  << " pressure=" << event->pressure
                  << " active=" << touch_injector_.active_contact_count() << '\n';
    }

    if (!injected && event->action != protocol::TouchAction::move) {
        std::cerr << "Touch injection failed for " << action_name(event->action)
                  << " id=" << event->pointer_id
                  << " (Windows error " << touch_injector_.last_error() << ")\n";
    }
}

}  // namespace remote_touch::app
