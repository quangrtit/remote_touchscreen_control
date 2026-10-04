#include "network/network_addresses.h"

#include <WinSock2.h>
#include <WS2tcpip.h>
#include <Windows.h>
#include <bcrypt.h>
#include <iphlpapi.h>

#include <array>
#include <iomanip>
#include <set>
#include <sstream>
#include <stdexcept>
#include <vector>

namespace remote_touch::network {

bool is_private_or_loopback_ipv4(
    const std::uint32_t host_order_address) noexcept {
    const auto first = (host_order_address >> 24U) & 0xFFU;
    const auto second = (host_order_address >> 16U) & 0xFFU;

    return first == 10U || first == 127U ||
           (first == 172U && second >= 16U && second <= 31U) ||
           (first == 192U && second == 168U) ||
           (first == 169U && second == 254U) ||
           (first == 100U && second >= 64U && second <= 127U);
}

std::vector<std::string> private_ipv4_addresses() {
    WSADATA data{};
    if (WSAStartup(MAKEWORD(2, 2), &data) != 0) {
        return {"127.0.0.1"};
    }

    constexpr ULONG flags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST |
                            GAA_FLAG_SKIP_DNS_SERVER | GAA_FLAG_INCLUDE_GATEWAYS;
    ULONG buffer_size = 15'000;
    std::vector<std::byte> buffer(buffer_size);
    auto* adapters = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data());
    ULONG result = GetAdaptersAddresses(AF_INET, flags, nullptr, adapters,
                                        &buffer_size);
    if (result == ERROR_BUFFER_OVERFLOW) {
        buffer.resize(buffer_size);
        adapters = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data());
        result = GetAdaptersAddresses(AF_INET, flags, nullptr, adapters,
                                      &buffer_size);
    }

    std::set<std::string> routed_addresses;
    std::set<std::string> fallback_addresses;
    if (result == NO_ERROR) {
        for (auto* adapter = adapters; adapter != nullptr; adapter = adapter->Next) {
            const bool usable_type =
                adapter->IfType == IF_TYPE_ETHERNET_CSMACD ||
                adapter->IfType == IF_TYPE_IEEE80211;
            if (adapter->OperStatus != IfOperStatusUp || !usable_type) {
                continue;
            }
            for (auto* item = adapter->FirstUnicastAddress; item != nullptr;
                 item = item->Next) {
                if (!item->Address.lpSockaddr ||
                    item->Address.lpSockaddr->sa_family != AF_INET) {
                    continue;
                }
                const auto* address = reinterpret_cast<const sockaddr_in*>(
                    item->Address.lpSockaddr);
                const auto host_order = ntohl(address->sin_addr.s_addr);
                if (!is_private_or_loopback_ipv4(host_order) ||
                    (host_order >> 24U) == 127U ||
                    ((host_order >> 24U) == 169U &&
                     ((host_order >> 16U) & 0xFFU) == 254U)) {
                    continue;
                }
                std::array<char, INET_ADDRSTRLEN> text{};
                if (!InetNtopA(AF_INET, &address->sin_addr, text.data(),
                               static_cast<DWORD>(text.size()))) {
                    continue;
                }
                fallback_addresses.emplace(text.data());
                if (adapter->FirstGatewayAddress != nullptr) {
                    routed_addresses.emplace(text.data());
                }
            }
        }
    }
    WSACleanup();

    auto& addresses = routed_addresses.empty() ? fallback_addresses
                                                : routed_addresses;
    if (addresses.empty()) {
        addresses.emplace("127.0.0.1");
    }
    return {addresses.begin(), addresses.end()};
}

std::string make_session_token() {
    std::array<unsigned char, 16> random_bytes{};
    const auto status = BCryptGenRandom(
        nullptr, random_bytes.data(), static_cast<ULONG>(random_bytes.size()),
        BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    if (status < 0) {
        throw std::runtime_error("Unable to create a secure session token");
    }

    std::ostringstream token;
    token << std::hex << std::setfill('0');
    for (const auto value : random_bytes) {
        token << std::setw(2) << static_cast<unsigned int>(value);
    }
    return token.str();
}

}  // namespace remote_touch::network
