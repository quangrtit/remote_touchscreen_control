#include "app/host_options.h"

#include <Windows.h>

#include <algorithm>
#include <charconv>
#include <cctype>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace remote_touch::app {
namespace {

[[nodiscard]] std::string_view require_value(int& index, const int argc,
                                             char* argv[]) {
    if (++index >= argc) {
        throw std::invalid_argument(std::string{"Missing value for "} +
                                    argv[index - 1]);
    }
    return argv[index];
}

}  // namespace

HostOptions parse_options(const int argc, char* argv[]) {
    HostOptions options;
    options.web_root = executable_directory() / "web";

    for (int index = 1; index < argc; ++index) {
        const std::string_view argument{argv[index]};
        if (argument == "--help" || argument == "-h") {
            options.show_help = true;
        } else if (argument == "--verbose-input") {
            options.verbose_input = true;
        } else if (argument == "--bind") {
            options.bind_address = require_value(index, argc, argv);
        } else if (argument == "--web-root") {
            options.web_root = std::filesystem::path{
                require_value(index, argc, argv)};
        } else if (argument == "--port") {
            const auto value = require_value(index, argc, argv);
            unsigned int port = 0;
            const auto [end, error] =
                std::from_chars(value.data(), value.data() + value.size(), port);
            if (error != std::errc{} || end != value.data() + value.size() ||
                port == 0 || port > 65535) {
                throw std::invalid_argument("Port must be between 1 and 65535");
            }
            options.port = static_cast<std::uint16_t>(port);
        } else if (argument == "--fps") {
            const auto value = require_value(index, argc, argv);
            unsigned int fps = 0;
            const auto [end, error] =
                std::from_chars(value.data(), value.data() + value.size(), fps);
            if (error != std::errc{} || end != value.data() + value.size() ||
                fps < 10 || fps > 60) {
                throw std::invalid_argument("FPS must be between 10 and 60");
            }
            options.video_fps = fps;
        } else if (argument == "--bitrate") {
            const auto value = require_value(index, argc, argv);
            unsigned int bitrate = 0;
            const auto [end, error] = std::from_chars(
                value.data(), value.data() + value.size(), bitrate);
            if (error != std::errc{} || end != value.data() + value.size() ||
                bitrate < 500'000 || bitrate > 50'000'000) {
                throw std::invalid_argument(
                    "Bitrate must be between 500000 and 50000000");
            }
            options.video_bitrate = bitrate;
        } else if (argument == "--max-width") {
            const auto value = require_value(index, argc, argv);
            unsigned int width = 0;
            const auto [end, error] = std::from_chars(
                value.data(), value.data() + value.size(), width);
            if (error != std::errc{} || end != value.data() + value.size() ||
                (width != 0 && (width < 640 || width > 7680))) {
                throw std::invalid_argument(
                    "Max width must be 0 (native) or between 640 and 7680");
            }
            options.max_video_width = width;
        } else if (argument == "--session-token") {
            const auto value = require_value(index, argc, argv);
            const bool valid = value.size() >= 16 && value.size() <= 128 &&
                               std::all_of(value.begin(), value.end(), [](char character) {
                                   return std::isalnum(
                                              static_cast<unsigned char>(character)) != 0 ||
                                          character == '-' || character == '_';
                               });
            if (!valid) {
                throw std::invalid_argument(
                    "Session token must contain 16-128 URL-safe characters");
            }
            options.session_token = value;
        } else {
            throw std::invalid_argument("Unknown option: " + std::string{argument});
        }
    }
    return options;
}

std::filesystem::path executable_directory() {
    std::vector<wchar_t> buffer(512);
    for (;;) {
        const auto length = GetModuleFileNameW(
            nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (length == 0) {
            throw std::runtime_error("Unable to locate executable directory");
        }
        if (length < buffer.size() - 1) {
            return std::filesystem::path{
                       std::wstring_view{buffer.data(), length}}
                .parent_path();
        }
        buffer.resize(buffer.size() * 2);
    }
}

const char* usage_text() noexcept {
    return "RemoteTouchHost [--port 8080] [--bind 0.0.0.0] "
           "[--web-root PATH] [--fps 60] [--bitrate 8000000] "
           "[--max-width 1280] "
           "[--session-token TOKEN] [--verbose-input]\n";
}

}  // namespace remote_touch::app
