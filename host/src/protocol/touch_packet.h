#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace remote_touch::protocol {

enum class TouchAction : std::uint8_t {
    down = 0,
    move = 1,
    up = 2,
    cancel = 3,
};

struct TouchEvent {
    TouchAction action{};
    std::uint32_t pointer_id{};
    float x{};
    float y{};
    float pressure{};
};

inline constexpr std::uint8_t kProtocolVersion = 1;
inline constexpr std::size_t kTouchPacketSize = 20;

[[nodiscard]] std::optional<TouchEvent> decode_touch_packet(
    std::span<const std::byte> bytes) noexcept;

}  // namespace remote_touch::protocol
