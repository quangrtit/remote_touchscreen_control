#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

namespace remote_touch::app {

struct HostOptions {
    std::string bind_address{"0.0.0.0"};
    std::uint16_t port{8080};
    std::filesystem::path web_root;
    std::string session_token;
    std::uint32_t video_fps{60};
    std::uint32_t video_bitrate{8'000'000};
    std::uint32_t max_video_width{1280};
    bool verbose_input{false};
    bool show_help{false};
};

[[nodiscard]] HostOptions parse_options(int argc, char* argv[]);
[[nodiscard]] std::filesystem::path executable_directory();
[[nodiscard]] const char* usage_text() noexcept;

}  // namespace remote_touch::app
