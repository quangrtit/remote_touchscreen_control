#pragma once

#include "input/touch_sink.h"

#include <cstddef>
#include <cstdint>
#include <memory>

namespace remote_touch::input {

class WindowsTouchInjector final : public TouchSink {
public:
    explicit WindowsTouchInjector(std::uint32_t max_contacts = 10);
    ~WindowsTouchInjector() override;

    WindowsTouchInjector(const WindowsTouchInjector&) = delete;
    WindowsTouchInjector& operator=(const WindowsTouchInjector&) = delete;

    bool inject(const protocol::TouchEvent& event) noexcept override;
    void release_all() noexcept override;
    void set_target(std::int32_t left, std::int32_t top, std::uint32_t width,
                    std::uint32_t height) noexcept;

    [[nodiscard]] std::uint32_t last_error() const noexcept;
    [[nodiscard]] std::size_t active_contact_count() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace remote_touch::input
