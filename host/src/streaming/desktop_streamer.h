#pragma once

#include "capture/desktop_capture.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string_view>

namespace remote_touch::streaming {

struct StreamConfig {
    std::uint32_t frames_per_second{60};
    std::uint32_t bitrate{8'000'000};
    std::uint32_t max_frame_width{1280};
};

class DesktopStreamer {
public:
    // Zero means disconnected. A new non-zero value identifies a new screen
    // WebSocket and requires a fresh H.264 keyframe.
    using ClientSession = std::function<std::uint64_t()>;
    using PacketSender = std::function<bool(std::span<const std::byte>)>;
    using GeometryHandler = std::function<void(capture::DesktopGeometry)>;
    using ErrorHandler = std::function<void(std::string_view)>;

    DesktopStreamer(StreamConfig config, ClientSession client_session,
                    PacketSender send_packet, GeometryHandler on_geometry,
                    ErrorHandler on_error);
    ~DesktopStreamer();

    DesktopStreamer(const DesktopStreamer&) = delete;
    DesktopStreamer& operator=(const DesktopStreamer&) = delete;

    void start();
    void stop() noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace remote_touch::streaming
