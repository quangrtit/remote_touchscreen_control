#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace remote_touch::network {

[[nodiscard]] bool is_private_or_loopback_ipv4(
    std::uint32_t host_order_address) noexcept;
[[nodiscard]] std::vector<std::string> private_ipv4_addresses();
[[nodiscard]] std::string make_session_token();

}  // namespace remote_touch::network
