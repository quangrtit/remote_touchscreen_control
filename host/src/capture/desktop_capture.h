#pragma once

#include <cstdint>
#include <memory>
#include <vector>

namespace remote_touch::capture {

struct DesktopGeometry {
    std::int32_t left{};
    std::int32_t top{};
    std::uint32_t width{};
    std::uint32_t height{};
};

struct DesktopFrame {
    std::uint32_t width{};
    std::uint32_t height{};
    std::uint32_t stride{};
    std::uint64_t timestamp_us{};
    std::vector<std::uint8_t> bgra;
};

class DesktopCapture {
public:
    DesktopCapture();
    ~DesktopCapture();

    DesktopCapture(const DesktopCapture&) = delete;
    DesktopCapture& operator=(const DesktopCapture&) = delete;

    [[nodiscard]] bool acquire_next(std::uint32_t timeout_ms,
                                    DesktopFrame& destination);
    [[nodiscard]] DesktopGeometry geometry() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace remote_touch::capture
