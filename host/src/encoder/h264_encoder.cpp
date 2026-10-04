#include "encoder/h264_encoder.h"

#include <Windows.h>
#include <codecapi.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mftransform.h>
#include <wmcodecdsp.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <deque>
#include <stdexcept>
#include <string>

namespace remote_touch::encoder {
namespace {

using Microsoft::WRL::ComPtr;

[[noreturn]] void throw_hresult(const char* operation, const HRESULT result) {
    throw std::runtime_error(std::string{operation} + " failed (HRESULT " +
                             std::to_string(static_cast<unsigned long>(result)) +
                             ")");
}

void check_hresult(const char* operation, const HRESULT result) {
    if (FAILED(result)) {
        throw_hresult(operation, result);
    }
}

class MediaFoundationSession {
  public:
    MediaFoundationSession() {
        check_hresult("MFStartup", MFStartup(MF_VERSION, MFSTARTUP_FULL));
    }

    ~MediaFoundationSession() { (void)MFShutdown(); }

    MediaFoundationSession(const MediaFoundationSession&) = delete;
    MediaFoundationSession& operator=(const MediaFoundationSession&) = delete;
};

[[nodiscard]] eAVEncH264VLevel choose_h264_level(
    const std::uint32_t width, const std::uint32_t height,
    const std::uint32_t fps) noexcept {
    const std::uint64_t macroblocks_per_frame =
        static_cast<std::uint64_t>((width + 15U) / 16U) *
        ((height + 15U) / 16U);
    const std::uint64_t macroblocks_per_second =
        macroblocks_per_frame * std::max(fps, 1U);

    if (macroblocks_per_frame <= 3'600 && macroblocks_per_second <= 108'000) {
        return eAVEncH264VLevel3_1;
    }
    if (macroblocks_per_frame <= 5'120 && macroblocks_per_second <= 216'000) {
        return eAVEncH264VLevel3_2;
    }
    if (macroblocks_per_frame <= 8'192 && macroblocks_per_second <= 245'760) {
        return eAVEncH264VLevel4_1;
    }
    if (macroblocks_per_frame <= 8'704 && macroblocks_per_second <= 522'240) {
        return eAVEncH264VLevel4_2;
    }
    if (macroblocks_per_frame <= 22'080 && macroblocks_per_second <= 589'824) {
        return eAVEncH264VLevel5;
    }
    if (macroblocks_per_frame <= 36'864 && macroblocks_per_second <= 983'040) {
        return eAVEncH264VLevel5_1;
    }
    return eAVEncH264VLevel5_2;
}

[[nodiscard]] std::uint8_t clamp_byte(const int value) noexcept {
    return static_cast<std::uint8_t>(std::clamp(value, 0, 255));
}

void bgra_to_nv12(const capture::DesktopFrame& source, std::uint8_t* destination,
                  const std::uint32_t width, const std::uint32_t height) noexcept {
    auto* y_plane = destination;
    auto* uv_plane = destination + static_cast<std::size_t>(width) * height;

    const bool scaled = source.width != width || source.height != height;
    // Produce Y and UV together in 2x2 blocks. When the stream is capped below
    // desktop resolution, nearest-neighbour sampling performs scaling during
    // color conversion instead of allocating another full image.
    for (std::uint32_t row = 0; row < height; row += 2) {
        const auto source_row0 = scaled
                                     ? static_cast<std::uint32_t>(
                                           static_cast<std::uint64_t>(row) *
                                           source.height / height)
                                     : row;
        const auto source_row1 = scaled
                                     ? static_cast<std::uint32_t>(
                                           static_cast<std::uint64_t>(row + 1U) *
                                           source.height / height)
                                     : row + 1U;
        const auto* first = source.bgra.data() +
                            static_cast<std::size_t>(source_row0) * source.stride;
        const auto* second = source.bgra.data() +
                             static_cast<std::size_t>(source_row1) * source.stride;
        auto* first_y = y_plane + static_cast<std::size_t>(row) * width;
        auto* second_y = first_y + width;
        auto* uv_row = uv_plane + static_cast<std::size_t>(row / 2U) * width;
        for (std::uint32_t column = 0; column < width; column += 2) {
            const auto source_column0 = scaled
                                            ? static_cast<std::uint32_t>(
                                                  static_cast<std::uint64_t>(column) *
                                                  source.width / width)
                                            : column;
            const auto source_column1 = scaled
                                            ? static_cast<std::uint32_t>(
                                                  static_cast<std::uint64_t>(column + 1U) *
                                                  source.width / width)
                                            : column + 1U;
            const auto offset0 = source_column0 * 4U;
            const auto offset1 = source_column1 * 4U;
            const int blue00 = first[offset0];
            const int green00 = first[offset0 + 1U];
            const int red00 = first[offset0 + 2U];
            const int blue01 = first[offset1];
            const int green01 = first[offset1 + 1U];
            const int red01 = first[offset1 + 2U];
            const int blue10 = second[offset0];
            const int green10 = second[offset0 + 1U];
            const int red10 = second[offset0 + 2U];
            const int blue11 = second[offset1];
            const int green11 = second[offset1 + 1U];
            const int red11 = second[offset1 + 2U];

            first_y[column] = clamp_byte(
                ((66 * red00 + 129 * green00 + 25 * blue00 + 128) >> 8) + 16);
            first_y[column + 1U] = clamp_byte(
                ((66 * red01 + 129 * green01 + 25 * blue01 + 128) >> 8) + 16);
            second_y[column] = clamp_byte(
                ((66 * red10 + 129 * green10 + 25 * blue10 + 128) >> 8) + 16);
            second_y[column + 1U] = clamp_byte(
                ((66 * red11 + 129 * green11 + 25 * blue11 + 128) >> 8) + 16);

            const int blue = (blue00 + blue01 + blue10 + blue11) / 4;
            const int green = (green00 + green01 + green10 + green11) / 4;
            const int red = (red00 + red01 + red10 + red11) / 4;
            uv_row[column] = clamp_byte(
                ((-38 * red - 74 * green + 112 * blue + 128) >> 8) + 128);
            uv_row[column + 1U] = clamp_byte(
                ((112 * red - 94 * green - 18 * blue + 128) >> 8) + 128);
        }
    }
}

[[nodiscard]] bool has_annex_b_start_code(
    const std::vector<std::uint8_t>& bytes) noexcept {
    return bytes.size() >= 4 && bytes[0] == 0 && bytes[1] == 0 &&
           (bytes[2] == 1 || (bytes[2] == 0 && bytes[3] == 1));
}

void convert_length_prefixed_to_annex_b(std::vector<std::uint8_t>& bytes) {
    if (has_annex_b_start_code(bytes)) {
        return;
    }
    std::size_t offset = 0;
    while (offset + 4 <= bytes.size()) {
        const std::uint32_t length =
            (static_cast<std::uint32_t>(bytes[offset]) << 24U) |
            (static_cast<std::uint32_t>(bytes[offset + 1]) << 16U) |
            (static_cast<std::uint32_t>(bytes[offset + 2]) << 8U) |
            bytes[offset + 3];
        if (length == 0 || offset + 4U + length > bytes.size()) {
            return;
        }
        bytes[offset] = 0;
        bytes[offset + 1] = 0;
        bytes[offset + 2] = 0;
        bytes[offset + 3] = 1;
        offset += 4U + length;
    }
}

}  // namespace

struct H264Encoder::Impl {
    struct InputSlot {
        ComPtr<IMFSample> sample;
        ComPtr<IMFMediaBuffer> buffer;
        bool pending{false};
    };

    Impl(const std::uint32_t source_width, const std::uint32_t source_height,
         const std::uint32_t requested_fps, const std::uint32_t requested_bitrate)
        : width(source_width & ~1U),
          height(source_height & ~1U),
          fps(std::max(requested_fps, 1U)),
          frame_duration_100ns(10'000'000ULL / fps) {
        if (width == 0 || height == 0) {
            throw std::invalid_argument("H.264 frame dimensions are invalid");
        }
        check_hresult("CoCreateInstance(CMSH264EncoderMFT)",
                      CoCreateInstance(CLSID_CMSH264EncoderMFT, nullptr,
                                       CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&transform)));
        // Several encoder properties are static and must be supplied before
        // SetOutputType. Configure the MFT before negotiating media types.
        configure_low_latency();

        ComPtr<IMFMediaType> output_type;
        check_hresult("MFCreateMediaType(output)", MFCreateMediaType(&output_type));
        check_hresult("Set output major type",
                      output_type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video));
        check_hresult("Set H.264 output subtype",
                      output_type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264));
        check_hresult("Set H.264 bitrate",
                      output_type->SetUINT32(MF_MT_AVG_BITRATE, requested_bitrate));
        check_hresult("Set H.264 frame size",
                      MFSetAttributeSize(output_type.Get(), MF_MT_FRAME_SIZE, width,
                                         height));
        check_hresult("Set H.264 frame rate",
                      MFSetAttributeRatio(output_type.Get(), MF_MT_FRAME_RATE, fps, 1));
        check_hresult("Set H.264 pixel aspect ratio",
                      MFSetAttributeRatio(output_type.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1,
                                          1));
        check_hresult("Set progressive output",
                      output_type->SetUINT32(MF_MT_INTERLACE_MODE,
                                             MFVideoInterlace_Progressive));
        check_hresult("Set H.264 profile",
                      output_type->SetUINT32(MF_MT_MPEG2_PROFILE,
                                             eAVEncH264VProfile_Base));
        check_hresult("Set H.264 level",
                      output_type->SetUINT32(
                          MF_MT_MPEG2_LEVEL,
                          choose_h264_level(width, height, fps)));
        check_hresult("IMFTransform::SetOutputType",
                      transform->SetOutputType(0, output_type.Get(), 0));

        ComPtr<IMFMediaType> input_type;
        check_hresult("MFCreateMediaType(input)", MFCreateMediaType(&input_type));
        check_hresult("Set input major type",
                      input_type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video));
        check_hresult("Set NV12 input subtype",
                      input_type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12));
        check_hresult("Set NV12 frame size",
                      MFSetAttributeSize(input_type.Get(), MF_MT_FRAME_SIZE, width,
                                         height));
        check_hresult("Set NV12 frame rate",
                      MFSetAttributeRatio(input_type.Get(), MF_MT_FRAME_RATE, fps, 1));
        check_hresult("Set NV12 pixel aspect ratio",
                      MFSetAttributeRatio(input_type.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1,
                                          1));
        check_hresult("Set progressive input",
                      input_type->SetUINT32(MF_MT_INTERLACE_MODE,
                                            MFVideoInterlace_Progressive));
        check_hresult("IMFTransform::SetInputType",
                      transform->SetInputType(0, input_type.Get(), 0));

        input_buffer_size = width * height * 3U / 2U;
        for (auto& slot : input_slots) {
            check_hresult("MFCreateSample(input)", MFCreateSample(&slot.sample));
            check_hresult("MFCreateMemoryBuffer(input)",
                          MFCreateMemoryBuffer(input_buffer_size, &slot.buffer));
            check_hresult("IMFSample::AddBuffer(input)",
                          slot.sample->AddBuffer(slot.buffer.Get()));
        }

        MFT_OUTPUT_STREAM_INFO output_info{};
        check_hresult("IMFTransform::GetOutputStreamInfo",
                      transform->GetOutputStreamInfo(0, &output_info));
        output_buffer_size = std::max<DWORD>(
            output_info.cbSize,
            std::max<DWORD>(requested_bitrate / 4U, width * height * 2U));
        check_hresult("MFCreateSample(output)", MFCreateSample(&output_sample));
        check_hresult("MFCreateMemoryBuffer(output)",
                      MFCreateMemoryBuffer(output_buffer_size, &output_buffer));
        check_hresult("IMFSample::AddBuffer(output)",
                      output_sample->AddBuffer(output_buffer.Get()));
        check_hresult("MFT_MESSAGE_NOTIFY_BEGIN_STREAMING",
                      transform->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0));
        check_hresult("MFT_MESSAGE_NOTIFY_START_OF_STREAM",
                      transform->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0));
    }

    ~Impl() {
        if (transform) {
            (void)transform->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
            (void)transform->ProcessMessage(MFT_MESSAGE_NOTIFY_END_STREAMING, 0);
        }
        transform.Reset();
    }

    void configure_low_latency() noexcept {
        ComPtr<ICodecAPI> codec;
        if (FAILED(transform->QueryInterface(
                IID_ICodecAPI, reinterpret_cast<void**>(codec.GetAddressOf())))) {
            return;
        }
        VARIANT value;
        VariantInit(&value);

        value.vt = VT_BOOL;
        value.boolVal = VARIANT_TRUE;
        (void)codec->SetValue(&CODECAPI_AVLowLatencyMode, &value);
        (void)codec->SetValue(&CODECAPI_AVEncCommonRealTime, &value);

        VariantClear(&value);
        value.vt = VT_UI4;
        value.ulVal = eAVEncCommonRateControlMode_LowDelayVBR;
        (void)codec->SetValue(&CODECAPI_AVEncCommonRateControlMode, &value);
        value.ulVal = fps;
        (void)codec->SetValue(&CODECAPI_AVEncMPVGOPSize, &value);
        value.ulVal = 0;
        (void)codec->SetValue(&CODECAPI_AVEncMPVDefaultBPictureCount, &value);
        // The documented 0-33 band is the encoder's lowest-complexity path.
        value.ulVal = 0;
        (void)codec->SetValue(&CODECAPI_AVEncCommonQualityVsSpeed, &value);
        // Zero lets the MFT choose the optimal number of slice worker threads.
        value.ulVal = 0;
        (void)codec->SetValue(&CODECAPI_AVEncNumWorkerThreads, &value);
        value.ulVal = 1;
        (void)codec->SetValue(&CODECAPI_AVEncVideoMaxNumRefFrame, &value);
        VariantClear(&value);
    }

    [[nodiscard]] std::optional<EncodedVideoFrame> encode(
        const capture::DesktopFrame& frame) {
        if (frame.width < width || frame.height < height ||
            frame.bgra.size() < static_cast<std::size_t>(frame.stride) * frame.height) {
            throw std::invalid_argument("Captured frame does not match encoder geometry");
        }

        std::optional<EncodedVideoFrame> deferred_output;
        auto available = std::find_if(
            input_slots.begin(), input_slots.end(),
            [](const InputSlot& slot) { return !slot.pending; });
        if (available == input_slots.end()) {
            deferred_output = drain_one();
            available = std::find_if(
                input_slots.begin(), input_slots.end(),
                [](const InputSlot& slot) { return !slot.pending; });
            if (available == input_slots.end()) {
                throw std::runtime_error("H.264 encoder retained every input buffer");
            }
        }
        const auto slot_index = static_cast<std::size_t>(
            std::distance(input_slots.begin(), available));
        auto& input_slot = *available;
        check_hresult("IMFSample::DeleteAllItems(input)",
                      input_slot.sample->DeleteAllItems());
        check_hresult("IMFMediaBuffer::SetCurrentLength(input)",
                      input_slot.buffer->SetCurrentLength(0));
        BYTE* input_data = nullptr;
        DWORD capacity = 0;
        check_hresult("IMFMediaBuffer::Lock(input)",
                      input_slot.buffer->Lock(&input_data, &capacity, nullptr));
        bgra_to_nv12(frame, input_data, width, height);
        (void)input_slot.buffer->Unlock();
        check_hresult("IMFMediaBuffer::SetCurrentLength(input)",
                      input_slot.buffer->SetCurrentLength(input_buffer_size));
        const LONGLONG sample_time =
            static_cast<LONGLONG>(frame_index * frame_duration_100ns);
        check_hresult("IMFSample::SetSampleTime",
                      input_slot.sample->SetSampleTime(sample_time));
        check_hresult("IMFSample::SetSampleDuration",
                      input_slot.sample->SetSampleDuration(
                          static_cast<LONGLONG>(frame_duration_100ns)));

        const HRESULT input_result =
            transform->ProcessInput(0, input_slot.sample.Get(), 0);
        if (input_result == MF_E_NOTACCEPTING) {
            auto pending_output = drain_one();
            check_hresult("IMFTransform::ProcessInput(retry)",
                          transform->ProcessInput(0, input_slot.sample.Get(), 0));
            input_slot.pending = true;
            pending_input_slots.push_back(slot_index);
            ++frame_index;
            if (deferred_output) {
                return deferred_output;
            }
            return pending_output ? std::move(pending_output) : drain_one();
        }
        check_hresult("IMFTransform::ProcessInput", input_result);
        input_slot.pending = true;
        pending_input_slots.push_back(slot_index);
        ++frame_index;
        if (deferred_output) {
            return deferred_output;
        }
        return drain_one();
    }

    [[nodiscard]] std::optional<EncodedVideoFrame> drain_one() {
        // Reuse the large output allocation. ProcessOutput completes before it
        // returns, and the encoded bytes are copied out below, so no frame can
        // retain this application-owned buffer.
        check_hresult("IMFSample::DeleteAllItems(output)",
                      output_sample->DeleteAllItems());
        check_hresult("IMFMediaBuffer::SetCurrentLength(output)",
                      output_buffer->SetCurrentLength(0));

        MFT_OUTPUT_DATA_BUFFER output{};
        output.dwStreamID = 0;
        output.pSample = output_sample.Get();
        DWORD status = 0;
        const HRESULT result = transform->ProcessOutput(0, 1, &output, &status);
        if (output.pEvents) {
            output.pEvents->Release();
        }
        if (result == MF_E_TRANSFORM_NEED_MORE_INPUT) {
            return std::nullopt;
        }
        check_hresult("IMFTransform::ProcessOutput", result);
        if (!pending_input_slots.empty()) {
            input_slots[pending_input_slots.front()].pending = false;
            pending_input_slots.pop_front();
        }

        ComPtr<IMFMediaBuffer> contiguous;
        check_hresult("IMFSample::ConvertToContiguousBuffer",
                      output_sample->ConvertToContiguousBuffer(&contiguous));
        BYTE* encoded_data = nullptr;
        DWORD encoded_size = 0;
        check_hresult("IMFMediaBuffer::Lock(output)",
                      contiguous->Lock(&encoded_data, nullptr, &encoded_size));
        std::vector<std::uint8_t> encoded(encoded_data, encoded_data + encoded_size);
        (void)contiguous->Unlock();
        convert_length_prefixed_to_annex_b(encoded);

        UINT32 clean_point = FALSE;
        (void)output_sample->GetUINT32(MFSampleExtension_CleanPoint, &clean_point);
        LONGLONG sample_time = 0;
        (void)output_sample->GetSampleTime(&sample_time);
        return EncodedVideoFrame{
            .width = width,
            .height = height,
            .timestamp_us = static_cast<std::uint64_t>(sample_time / 10),
            .duration_us = static_cast<std::uint32_t>(frame_duration_100ns / 10),
            .key_frame = clean_point != FALSE,
            .annex_b = std::move(encoded),
        };
    }

    MediaFoundationSession media_foundation;
    ComPtr<IMFTransform> transform;
    std::array<InputSlot, 3> input_slots;
    std::deque<std::size_t> pending_input_slots;
    ComPtr<IMFSample> output_sample;
    ComPtr<IMFMediaBuffer> output_buffer;
    std::uint32_t width{};
    std::uint32_t height{};
    std::uint32_t fps{};
    std::uint64_t frame_duration_100ns{};
    std::uint64_t frame_index{};
    DWORD input_buffer_size{};
    DWORD output_buffer_size{};
};

H264Encoder::H264Encoder(const std::uint32_t width, const std::uint32_t height,
                         const std::uint32_t frames_per_second,
                         const std::uint32_t bitrate)
    : impl_(std::make_unique<Impl>(width, height, frames_per_second, bitrate)) {}

H264Encoder::~H264Encoder() = default;

std::optional<EncodedVideoFrame> H264Encoder::encode(
    const capture::DesktopFrame& frame) {
    return impl_->encode(frame);
}

}  // namespace remote_touch::encoder
