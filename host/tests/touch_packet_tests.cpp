#include "protocol/touch_packet.h"

#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>

namespace {

template <typename T>
void write_little_endian(std::byte* destination, T value) {
    std::memcpy(destination, &value, sizeof(value));
}

std::array<std::byte, remote_touch::protocol::kTouchPacketSize> valid_packet() {
    std::array<std::byte, remote_touch::protocol::kTouchPacketSize> bytes{};
    bytes[0] = std::byte{remote_touch::protocol::kProtocolVersion};
    bytes[1] = std::byte{static_cast<std::uint8_t>(
        remote_touch::protocol::TouchAction::move)};
    write_little_endian(bytes.data() + 4, std::uint32_t{42});
    write_little_endian(bytes.data() + 8, 0.25F);
    write_little_endian(bytes.data() + 12, 0.75F);
    write_little_endian(bytes.data() + 16, 0.5F);
    return bytes;
}

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
    }
    return condition;
}

}  // namespace

int main() {
    bool passed = true;

    const auto bytes = valid_packet();
    const auto event = remote_touch::protocol::decode_touch_packet(bytes);
    passed &= expect(event.has_value(), "valid packet decodes");
    if (event) {
        passed &= expect(event->pointer_id == 42, "pointer id is preserved");
        passed &= expect(std::abs(event->x - 0.25F) < 0.0001F, "x is preserved");
        passed &= expect(std::abs(event->y - 0.75F) < 0.0001F, "y is preserved");
    }

    auto bad_version = bytes;
    bad_version[0] = std::byte{99};
    passed &= expect(!remote_touch::protocol::decode_touch_packet(bad_version),
                     "unknown version is rejected");

    auto bad_coordinate = bytes;
    write_little_endian(bad_coordinate.data() + 8, 1.5F);
    passed &= expect(!remote_touch::protocol::decode_touch_packet(bad_coordinate),
                     "out-of-range coordinates are rejected");

    passed &= expect(!remote_touch::protocol::decode_touch_packet(
                         std::span{bytes}.first(bytes.size() - 1)),
                     "incorrect packet size is rejected");

    return passed ? 0 : 1;
}
