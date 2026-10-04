#include "streaming/desktop_streamer.h"

#include "capture/desktop_capture.h"
#include "encoder/h264_encoder.h"

#include <Windows.h>
#include <objbase.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <exception>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace remote_touch::streaming {
namespace {

struct EncoderSize {
    std::uint32_t width{};
    std::uint32_t height{};
};

[[nodiscard]] EncoderSize encoder_size(const std::uint32_t source_width,
                                       const std::uint32_t source_height,
                                       const std::uint32_t max_width) noexcept {
    auto width = source_width & ~1U;
    auto height = source_height & ~1U;
    if (max_width != 0 && width > max_width) {
        width = std::max(max_width & ~1U, 2U);
        height = static_cast<std::uint32_t>(
                     static_cast<std::uint64_t>(source_height) * width /
                     std::max(source_width, 1U)) &
                 ~1U;
        height = std::max(height, 2U);
    }
    return {.width = width, .height = height};
}

template <typename T>
void write_little_endian(std::byte* destination, const T value) noexcept {
    static_assert(std::is_integral_v<T>);
    for (std::size_t index = 0; index < sizeof(T); ++index) {
        destination[index] =
            static_cast<std::byte>((value >> (index * 8U)) & 0xFFU);
    }
}

void make_video_packet(const encoder::EncodedVideoFrame& frame,
                       std::vector<std::byte>& packet) {
    constexpr std::size_t header_size = 28;
    packet.resize(header_size + frame.annex_b.size());
    packet[0] = std::byte{'R'};
    packet[1] = std::byte{'T'};
    packet[2] = std::byte{'V'};
    packet[3] = std::byte{'1'};
    packet[4] = std::byte{1};
    packet[5] = frame.key_frame ? std::byte{1} : std::byte{0};
    write_little_endian<std::uint16_t>(packet.data() + 6,
                                       static_cast<std::uint16_t>(header_size));
    write_little_endian(packet.data() + 8, frame.width);
    write_little_endian(packet.data() + 12, frame.height);
    write_little_endian(packet.data() + 16, frame.timestamp_us);
    write_little_endian(packet.data() + 24, frame.duration_us);
    std::memcpy(packet.data() + header_size, frame.annex_b.data(),
                frame.annex_b.size());
}

}  // namespace

struct DesktopStreamer::Impl {
    Impl(StreamConfig stream_config, ClientSession session,
         PacketSender sender, GeometryHandler geometry_handler,
         ErrorHandler error_handler)
        : config(stream_config),
          client_session(std::move(session)),
          send_packet(std::move(sender)),
          on_geometry(std::move(geometry_handler)),
          on_error(std::move(error_handler)) {}

    void start() {
        if (worker.joinable()) {
            return;
        }
        worker = std::jthread([this](const std::stop_token stop_token) {
            run(stop_token);
        });
    }

    void stop() noexcept {
        if (worker.joinable()) {
            worker.request_stop();
            worker.join();
        }
    }

    void run(const std::stop_token stop_token) noexcept {
        const HRESULT com_result = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        const bool uninitialize_com = SUCCEEDED(com_result);
        if (FAILED(com_result) && com_result != RPC_E_CHANGED_MODE) {
            on_error("Unable to initialize COM for desktop streaming");
            return;
        }
        (void)SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);

        std::string previous_error;
        auto next_error_log = std::chrono::steady_clock::time_point{};
        auto retry_delay = std::chrono::seconds{1};

        while (!stop_token.stop_requested()) {
            try {
                run_capture_session(stop_token);
                previous_error.clear();
                retry_delay = std::chrono::seconds{1};
            } catch (const std::exception& error) {
                const auto now = std::chrono::steady_clock::now();
                if (previous_error != error.what() || now >= next_error_log) {
                    on_error(error.what());
                    previous_error = error.what();
                    next_error_log = now + std::chrono::seconds{30};
                }

                const auto retry_steps = std::chrono::duration_cast<
                    std::chrono::milliseconds>(retry_delay).count() / 50;
                for (std::int64_t step = 0;
                     step < retry_steps && !stop_token.stop_requested(); ++step) {
                    std::this_thread::sleep_for(std::chrono::milliseconds{50});
                }
                retry_delay = std::min(retry_delay * 2, std::chrono::seconds{10});
            }
        }

        if (uninitialize_com) {
            CoUninitialize();
        }
    }

    void run_capture_session(const std::stop_token stop_token) {
        capture::DesktopCapture capture;
        on_geometry(capture.geometry());
        std::unique_ptr<encoder::H264Encoder> encoder;
        std::uint64_t previous_session = 0;
        const auto frame_interval = std::chrono::microseconds{
            1'000'000 / std::max(config.frames_per_second, 1U)};
        auto next_frame_time = std::chrono::steady_clock::now();
        capture::DesktopFrame frame;
        std::vector<std::byte> packet;
        std::uint32_t source_width = 0;
        std::uint32_t source_height = 0;

        while (!stop_token.stop_requested()) {
            const std::uint64_t current_session = client_session();
            if (current_session == 0) {
                encoder.reset();
                previous_session = 0;
                std::this_thread::sleep_for(std::chrono::milliseconds{5});
                continue;
            }
            if (current_session != previous_session) {
                const auto geometry = capture.geometry();
                const auto encoded_size = encoder_size(
                    geometry.width, geometry.height, config.max_frame_width);
                encoder = std::make_unique<encoder::H264Encoder>(
                    encoded_size.width, encoded_size.height,
                    config.frames_per_second,
                    config.bitrate);
                source_width = geometry.width & ~1U;
                source_height = geometry.height & ~1U;
                next_frame_time = std::chrono::steady_clock::now();
                previous_session = current_session;

                // Desktop Duplication only reports changed frames. Re-encode
                // the most recent complete frame so a reconnecting client gets
                // an IDR immediately even when the desktop is motionless.
                if (!frame.bgra.empty() && (frame.width & ~1U) == source_width &&
                    (frame.height & ~1U) == source_height) {
                    auto cached = encoder->encode(frame);
                    if (cached && !cached->annex_b.empty()) {
                        make_video_packet(*cached, packet);
                        if (!send_packet(packet)) {
                            encoder.reset();
                            previous_session = 0;
                            continue;
                        }
                    }
                }
            }

            if (const auto now = std::chrono::steady_clock::now(); now < next_frame_time) {
                std::this_thread::sleep_until(next_frame_time);
            }
            next_frame_time = std::chrono::steady_clock::now() + frame_interval;

            if (!capture.acquire_next(20, frame)) {
                continue;
            }
            if ((frame.width & ~1U) != source_width ||
                (frame.height & ~1U) != source_height) {
                const auto geometry = capture.geometry();
                on_geometry(geometry);
                const auto encoded_size = encoder_size(
                    frame.width, frame.height, config.max_frame_width);
                encoder = std::make_unique<encoder::H264Encoder>(
                    encoded_size.width, encoded_size.height,
                    config.frames_per_second,
                    config.bitrate);
                source_width = frame.width & ~1U;
                source_height = frame.height & ~1U;
            }
            auto encoded = encoder->encode(frame);
            if (!encoded || encoded->annex_b.empty()) {
                continue;
            }
            make_video_packet(*encoded, packet);
            if (!send_packet(packet)) {
                encoder.reset();
                previous_session = 0;
            }
        }
    }

    StreamConfig config;
    ClientSession client_session;
    PacketSender send_packet;
    GeometryHandler on_geometry;
    ErrorHandler on_error;
    std::jthread worker;
};

DesktopStreamer::DesktopStreamer(StreamConfig config,
                                 ClientSession client_session,
                                 PacketSender send_packet,
                                 GeometryHandler on_geometry,
                                 ErrorHandler on_error)
    : impl_(std::make_unique<Impl>(config, std::move(client_session),
                                   std::move(send_packet), std::move(on_geometry),
                                   std::move(on_error))) {}

DesktopStreamer::~DesktopStreamer() { stop(); }

void DesktopStreamer::start() { impl_->start(); }

void DesktopStreamer::stop() noexcept { impl_->stop(); }

}  // namespace remote_touch::streaming
