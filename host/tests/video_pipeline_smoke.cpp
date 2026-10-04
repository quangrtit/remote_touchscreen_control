#include "capture/desktop_capture.h"
#include "encoder/h264_encoder.h"

#include <Windows.h>
#include <objbase.h>

#include <chrono>
#include <cstdint>
#include <exception>
#include <iostream>
#include <string>

namespace {

bool contains_nal_type(const std::vector<std::uint8_t>& bytes,
                       const std::uint8_t expected_type) {
    for (std::size_t index = 0; index + 5 < bytes.size(); ++index) {
        std::size_t nal = 0;
        if (bytes[index] == 0 && bytes[index + 1] == 0 && bytes[index + 2] == 1) {
            nal = index + 3;
        } else if (bytes[index] == 0 && bytes[index + 1] == 0 &&
                   bytes[index + 2] == 0 && bytes[index + 3] == 1) {
            nal = index + 4;
        }
        if (nal != 0 && (bytes[nal] & 0x1FU) == expected_type) {
            return true;
        }
    }
    return false;
}

}  // namespace

int main(const int argc, char* argv[]) {
    const auto fps = argc > 1 ? static_cast<std::uint32_t>(std::stoul(argv[1])) : 30U;
    const auto bitrate =
        argc > 2 ? static_cast<std::uint32_t>(std::stoul(argv[2])) : 6'000'000U;
    const auto max_width =
        argc > 3 ? static_cast<std::uint32_t>(std::stoul(argv[3])) : 1280U;
    const auto frame_count =
        argc > 4 ? static_cast<std::uint32_t>(std::stoul(argv[4])) : 1U;
    const HRESULT com_result = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(com_result)) {
        std::cerr << "COM initialization failed\n";
        return 1;
    }

    int exit_code = 1;
    try {
        remote_touch::capture::DesktopCapture capture;
        const auto geometry = capture.geometry();
        auto encoded_width = geometry.width & ~1U;
        auto encoded_height = geometry.height & ~1U;
        if (max_width != 0 && encoded_width > max_width) {
            encoded_width = max_width & ~1U;
            encoded_height = static_cast<std::uint32_t>(
                                 static_cast<std::uint64_t>(geometry.height) *
                                 encoded_width / geometry.width) &
                             ~1U;
        }
        remote_touch::encoder::H264Encoder encoder{encoded_width, encoded_height,
                                                    fps, bitrate};
        remote_touch::capture::DesktopFrame frame;
        for (int attempt = 0; attempt < 120; ++attempt) {
            if (!capture.acquire_next(100, frame)) {
                continue;
            }
            const auto started = std::chrono::steady_clock::now();
            std::uint32_t produced = 0;
            std::size_t encoded_bytes = 0;
            bool has_sps = false;
            bool first_key_frame = false;
            for (std::uint32_t index = 0; index < frame_count; ++index) {
                auto encoded = encoder.encode(frame);
                if (!encoded || encoded->annex_b.empty()) {
                    continue;
                }
                if (produced == 0) {
                    first_key_frame = encoded->key_frame;
                }
                has_sps = has_sps || contains_nal_type(encoded->annex_b, 7);
                encoded_bytes += encoded->annex_b.size();
                ++produced;
            }
            if (produced == 0) {
                continue;
            }
            const auto elapsed = std::chrono::duration<double>(
                std::chrono::steady_clock::now() - started);
            std::cout << "Captured " << frame.width << 'x' << frame.height
                      << " -> " << encoded_width << 'x' << encoded_height
                      << ", frames=" << produced
                      << ", bytes=" << encoded_bytes
                      << ", throughput=" << produced / elapsed.count() << " FPS"
                      << ", first-key=" << first_key_frame
                      << ", sps=" << has_sps << '\n';
            exit_code = has_sps ? 0 : 2;
            break;
        }
        if (exit_code == 1) {
            std::cerr << "No encoded frame was produced\n";
        }
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
    }
    CoUninitialize();
    return exit_code;
}
