#include "protocol/touch_packet.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <type_traits>

namespace remote_touch::protocol {
namespace {

template <typename T>
[[nodiscard]] T read_little_endian(const std::byte* data) noexcept {
    static_assert(std::is_trivially_copyable_v<T>);
    T value{};
    std::memcpy(&value, data, sizeof(value));
    if constexpr (std::endian::native == std::endian::big) {
        auto* first = reinterpret_cast<std::byte*>(&value);
        std::reverse(first, first + sizeof(value));
    }
    return value;
}

[[nodiscard]] bool valid_coordinate(float value) noexcept {
    return std::isfinite(value) && value >= 0.0F && value <= 1.0F;
}

}  // namespace

std::optional<TouchEvent> decode_touch_packet(
    const std::span<const std::byte> bytes) noexcept {
    if (bytes.size() != kTouchPacketSize ||
        std::to_integer<std::uint8_t>(bytes[0]) != kProtocolVersion) {
        return std::nullopt;
    }

    const auto action_value = std::to_integer<std::uint8_t>(bytes[1]);
    if (action_value > static_cast<std::uint8_t>(TouchAction::cancel)) {
        return std::nullopt;
    }

    TouchEvent event{
        .action = static_cast<TouchAction>(action_value),
        .pointer_id = read_little_endian<std::uint32_t>(bytes.data() + 4),
        .x = read_little_endian<float>(bytes.data() + 8),
        .y = read_little_endian<float>(bytes.data() + 12),
        .pressure = read_little_endian<float>(bytes.data() + 16),
    };

    if (event.pointer_id == 0 || !valid_coordinate(event.x) ||
        !valid_coordinate(event.y) || !valid_coordinate(event.pressure)) {
        return std::nullopt;
    }

    return event;
}

}  // namespace remote_touch::protocol
