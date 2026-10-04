#include "capture/desktop_capture.h"

#include <Windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include <chrono>
#include <cstring>
#include <stdexcept>
#include <string>

namespace remote_touch::capture {
namespace {

using Microsoft::WRL::ComPtr;

[[noreturn]] void throw_hresult(const char* operation, const HRESULT result) {
    throw std::runtime_error(std::string{operation} + " failed (HRESULT " +
                             std::to_string(static_cast<unsigned long>(result)) +
                             ")");
}

[[nodiscard]] bool contains_desktop_origin(const RECT& bounds) noexcept {
    return bounds.left <= 0 && bounds.right > 0 && bounds.top <= 0 &&
           bounds.bottom > 0;
}

}  // namespace

struct DesktopCapture::Impl {
    Impl() { initialize(); }

    void initialize() {
        duplication.Reset();
        staging.Reset();
        output.Reset();
        context.Reset();
        device.Reset();

        D3D_FEATURE_LEVEL feature_level{};
        const HRESULT create_result = D3D11CreateDevice(
            nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
            nullptr, 0, D3D11_SDK_VERSION, &device, &feature_level, &context);
        if (FAILED(create_result)) {
            throw_hresult("D3D11CreateDevice", create_result);
        }

        ComPtr<IDXGIDevice> dxgi_device;
        ComPtr<IDXGIAdapter> adapter;
        if (FAILED(device.As(&dxgi_device)) ||
            FAILED(dxgi_device->GetAdapter(&adapter))) {
            throw std::runtime_error("Unable to resolve the DXGI adapter");
        }

        ComPtr<IDXGIOutput> fallback;
        for (UINT index = 0;; ++index) {
            ComPtr<IDXGIOutput> candidate;
            const HRESULT result = adapter->EnumOutputs(index, &candidate);
            if (result == DXGI_ERROR_NOT_FOUND) {
                break;
            }
            if (FAILED(result)) {
                throw_hresult("IDXGIAdapter::EnumOutputs", result);
            }

            DXGI_OUTPUT_DESC description{};
            if (FAILED(candidate->GetDesc(&description)) ||
                !description.AttachedToDesktop) {
                continue;
            }
            if (!fallback) {
                fallback = candidate;
            }
            if (contains_desktop_origin(description.DesktopCoordinates)) {
                output = candidate;
                break;
            }
        }
        if (!output) {
            output = fallback;
        }
        if (!output) {
            throw std::runtime_error("No attached desktop output was found");
        }

        DXGI_OUTPUT_DESC output_description{};
        if (FAILED(output->GetDesc(&output_description))) {
            throw std::runtime_error("Unable to query desktop output geometry");
        }
        geometry = {
            .left = output_description.DesktopCoordinates.left,
            .top = output_description.DesktopCoordinates.top,
            .width = static_cast<std::uint32_t>(
                output_description.DesktopCoordinates.right -
                output_description.DesktopCoordinates.left),
            .height = static_cast<std::uint32_t>(
                output_description.DesktopCoordinates.bottom -
                output_description.DesktopCoordinates.top),
        };

        ComPtr<IDXGIOutput1> output1;
        if (FAILED(output.As(&output1))) {
            throw std::runtime_error("Desktop output does not support duplication");
        }
        const HRESULT duplicate_result = output1->DuplicateOutput(device.Get(), &duplication);
        if (FAILED(duplicate_result)) {
            throw_hresult("IDXGIOutput1::DuplicateOutput", duplicate_result);
        }

        DXGI_OUTDUPL_DESC duplication_description{};
        duplication->GetDesc(&duplication_description);
        texture_description = {};
        texture_description.Width = duplication_description.ModeDesc.Width;
        texture_description.Height = duplication_description.ModeDesc.Height;
        texture_description.MipLevels = 1;
        texture_description.ArraySize = 1;
        texture_description.Format = duplication_description.ModeDesc.Format;
        texture_description.SampleDesc.Count = 1;
        texture_description.Usage = D3D11_USAGE_STAGING;
        texture_description.CPUAccessFlags = D3D11_CPU_ACCESS_READ;

        const HRESULT texture_result =
            device->CreateTexture2D(&texture_description, nullptr, &staging);
        if (FAILED(texture_result)) {
            throw_hresult("ID3D11Device::CreateTexture2D", texture_result);
        }
    }

    [[nodiscard]] bool acquire(const std::uint32_t timeout_ms,
                               DesktopFrame& frame) {
        DXGI_OUTDUPL_FRAME_INFO frame_info{};
        ComPtr<IDXGIResource> resource;
        const HRESULT acquire_result = duplication->AcquireNextFrame(
            timeout_ms, &frame_info, &resource);
        if (acquire_result == DXGI_ERROR_WAIT_TIMEOUT) {
            return false;
        }
        if (acquire_result == DXGI_ERROR_ACCESS_LOST) {
            initialize();
            return false;
        }
        if (FAILED(acquire_result)) {
            throw_hresult("IDXGIOutputDuplication::AcquireNextFrame", acquire_result);
        }

        struct FrameRelease {
            IDXGIOutputDuplication* duplication;
            ~FrameRelease() { (void)duplication->ReleaseFrame(); }
        } release{duplication.Get()};

        ComPtr<ID3D11Texture2D> texture;
        const HRESULT texture_result = resource.As(&texture);
        if (FAILED(texture_result)) {
            throw_hresult("IDXGIResource::QueryInterface", texture_result);
        }
        context->CopyResource(staging.Get(), texture.Get());

        D3D11_MAPPED_SUBRESOURCE mapped{};
        const HRESULT map_result =
            context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped);
        if (FAILED(map_result)) {
            throw_hresult("ID3D11DeviceContext::Map", map_result);
        }

        struct TextureUnmap {
            ID3D11DeviceContext* context;
            ID3D11Texture2D* texture;
            ~TextureUnmap() { context->Unmap(texture, 0); }
        } unmap{context.Get(), staging.Get()};

        const std::uint32_t row_bytes = texture_description.Width * 4U;
        frame.width = texture_description.Width;
        frame.height = texture_description.Height;
        frame.stride = row_bytes;
        frame.timestamp_us = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now().time_since_epoch())
                .count());
        frame.bgra.resize(static_cast<std::size_t>(row_bytes) *
                          texture_description.Height);
        const auto* source = static_cast<const std::uint8_t*>(mapped.pData);
        for (std::uint32_t row = 0; row < frame.height; ++row) {
            std::memcpy(frame.bgra.data() + static_cast<std::size_t>(row) * row_bytes,
                        source + static_cast<std::size_t>(row) * mapped.RowPitch,
                        row_bytes);
        }
        return true;
    }

    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<IDXGIOutput> output;
    ComPtr<IDXGIOutputDuplication> duplication;
    ComPtr<ID3D11Texture2D> staging;
    D3D11_TEXTURE2D_DESC texture_description{};
    DesktopGeometry geometry{};
};

DesktopCapture::DesktopCapture() : impl_(std::make_unique<Impl>()) {}
DesktopCapture::~DesktopCapture() = default;

bool DesktopCapture::acquire_next(const std::uint32_t timeout_ms,
                                  DesktopFrame& destination) {
    return impl_->acquire(timeout_ms, destination);
}

DesktopGeometry DesktopCapture::geometry() const noexcept { return impl_->geometry; }

}  // namespace remote_touch::capture
