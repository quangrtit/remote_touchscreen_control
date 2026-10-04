#pragma once

#include "capture/desktop_capture.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

namespace remote_touch::encoder {

struct EncodedVideoFrame {
    std::uint32_t width{};
    std::uint32_t height{};
    std::uint64_t timestamp_us{};
    std::uint32_t duration_us{};
    bool key_frame{false};
    std::vector<std::uint8_t> annex_b;
};

class H264Encoder {
public:
    H264Encoder(std::uint32_t width, std::uint32_t height,
                std::uint32_t frames_per_second = 30,
                std::uint32_t bitrate = 6'000'000);
    ~H264Encoder();

    H264Encoder(const H264Encoder&) = delete;
    H264Encoder& operator=(const H264Encoder&) = delete;

    [[nodiscard]] std::optional<EncodedVideoFrame> encode(
        const capture::DesktopFrame& frame);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace remote_touch::encoder
