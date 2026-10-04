#include "input/windows_touch_injector.h"

#include "input/touch_contact_tracker.h"

#include <Windows.h>

#include <algorithm>
#include <cmath>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <vector>

namespace remote_touch::input {
namespace {

struct DesktopBounds {
    LONG left{};
    LONG top{};
    LONG width{};
    LONG height{};
};

[[nodiscard]] DesktopBounds current_desktop_bounds() noexcept {
    return {
        .left = GetSystemMetrics(SM_XVIRTUALSCREEN),
        .top = GetSystemMetrics(SM_YVIRTUALSCREEN),
        .width = GetSystemMetrics(SM_CXVIRTUALSCREEN),
        .height = GetSystemMetrics(SM_CYVIRTUALSCREEN),
    };
}

[[nodiscard]] LONG map_coordinate(float normalized, LONG origin,
                                  LONG extent) noexcept {
    const auto max_offset = std::max<LONG>(extent - 1, 0);
    return origin + static_cast<LONG>(
                        std::lround(std::clamp(normalized, 0.0F, 1.0F) *
                                    static_cast<float>(max_offset)));
}

[[nodiscard]] POINTER_TOUCH_INFO make_contact(
    std::uint32_t windows_id, const protocol::TouchEvent& event,
    const DesktopBounds bounds, const DWORD pointer_flags) noexcept {
    POINTER_TOUCH_INFO contact{};
    contact.pointerInfo.pointerType = PT_TOUCH;
    contact.pointerInfo.pointerId = windows_id;
    contact.pointerInfo.pointerFlags = pointer_flags;
    contact.pointerInfo.ptPixelLocation = {
        map_coordinate(event.x, bounds.left, bounds.width),
        map_coordinate(event.y, bounds.top, bounds.height),
    };
    contact.touchFlags = TOUCH_FLAG_NONE;
    contact.touchMask = TOUCH_MASK_CONTACTAREA | TOUCH_MASK_ORIENTATION |
                        TOUCH_MASK_PRESSURE;

    constexpr LONG contact_radius = 2;
    const auto point = contact.pointerInfo.ptPixelLocation;
    contact.rcContact = {
        point.x - contact_radius,
        point.y - contact_radius,
        point.x + contact_radius,
        point.y + contact_radius,
    };
    contact.orientation = 90;
    contact.pressure = static_cast<UINT32>(
        std::lround(std::clamp(event.pressure, 0.0F, 1.0F) * 1024.0F));
    return contact;
}

[[nodiscard]] DWORD pointer_flags(
    const ContactTransition transition) noexcept {
    switch (transition) {
        case ContactTransition::down:
            return POINTER_FLAG_DOWN | POINTER_FLAG_INRANGE |
                   POINTER_FLAG_INCONTACT;
        case ContactTransition::update:
            return POINTER_FLAG_UPDATE | POINTER_FLAG_INRANGE |
                   POINTER_FLAG_INCONTACT;
        case ContactTransition::up:
            return POINTER_FLAG_UP;
        case ContactTransition::cancel:
            return POINTER_FLAG_UP | POINTER_FLAG_CANCELED;
    }
    return POINTER_FLAG_NONE;
}

}  // namespace

struct WindowsTouchInjector::Impl {
    explicit Impl(const std::uint32_t requested_max_contacts)
        : max_contacts(std::clamp(requested_max_contacts, 1U, 256U)),
          tracker(max_contacts) {
        if (!InitializeTouchInjection(max_contacts, TOUCH_FEEDBACK_NONE)) {
            throw std::runtime_error("InitializeTouchInjection failed with error " +
                                     std::to_string(GetLastError()));
        }
    }

    bool send(std::vector<POINTER_TOUCH_INFO>& contacts) noexcept {
        if (!contacts.empty() &&
            InjectTouchInput(static_cast<UINT32>(contacts.size()),
                             contacts.data())) {
            last_error = ERROR_SUCCESS;
            return true;
        }
        last_error = GetLastError();
        return false;
    }

    std::mutex mutex;
    std::optional<DesktopBounds> target_bounds;
    std::uint32_t max_contacts{};
    TouchContactTracker tracker;
    std::uint32_t last_error{ERROR_SUCCESS};
};

WindowsTouchInjector::WindowsTouchInjector(const std::uint32_t max_contacts)
    : impl_(std::make_unique<Impl>(max_contacts)) {}

WindowsTouchInjector::~WindowsTouchInjector() { release_all(); }

bool WindowsTouchInjector::inject(const protocol::TouchEvent& event) noexcept {
    std::scoped_lock lock(impl_->mutex);
    const auto bounds = impl_->target_bounds.value_or(current_desktop_bounds());
    if (bounds.width <= 0 || bounds.height <= 0) {
        impl_->last_error = ERROR_INVALID_DATA;
        return false;
    }

    const auto frame = impl_->tracker.prepare(event);
    switch (frame.status) {
        case ContactFrameStatus::ignored:
            impl_->last_error = ERROR_SUCCESS;
            return true;
        case ContactFrameStatus::duplicate:
            impl_->last_error = ERROR_ALREADY_EXISTS;
            return false;
        case ContactFrameStatus::missing:
            impl_->last_error = ERROR_NOT_FOUND;
            return false;
        case ContactFrameStatus::full:
            impl_->last_error = ERROR_TOO_MANY_OPEN_FILES;
            return false;
        case ContactFrameStatus::ready:
            break;
    }

    std::vector<POINTER_TOUCH_INFO> contacts;
    contacts.reserve(frame.contacts.size());
    for (const auto& tracked : frame.contacts) {
        contacts.push_back(make_contact(tracked.injection_id, tracked.event, bounds,
                                        pointer_flags(tracked.transition)));
    }
    if (!impl_->send(contacts)) {
        // ERROR_INVALID_PARAMETER cancels all active injected contacts according
        // to InjectTouchInput's contract, so mirror that reset locally.
        if (impl_->last_error == ERROR_INVALID_PARAMETER) {
            impl_->tracker.clear();
        }
        return false;
    }
    impl_->tracker.commit(frame);
    return true;
}

void WindowsTouchInjector::release_all() noexcept {
    if (!impl_) {
        return;
    }

    std::scoped_lock lock(impl_->mutex);
    if (impl_->tracker.active_count() == 0) {
        return;
    }

    const auto bounds = impl_->target_bounds.value_or(current_desktop_bounds());
    const auto tracked_contacts = impl_->tracker.release_frame();
    std::vector<POINTER_TOUCH_INFO> contacts;
    contacts.reserve(tracked_contacts.size());
    for (const auto& tracked : tracked_contacts) {
        contacts.push_back(make_contact(tracked.injection_id, tracked.event, bounds,
                                        pointer_flags(tracked.transition)));
    }

    (void)impl_->send(contacts);
    impl_->tracker.clear();
}

void WindowsTouchInjector::set_target(const std::int32_t left,
                                      const std::int32_t top,
                                      const std::uint32_t width,
                                      const std::uint32_t height) noexcept {
    if (width == 0 || height == 0) {
        return;
    }
    std::scoped_lock lock(impl_->mutex);
    impl_->target_bounds = DesktopBounds{
        .left = left,
        .top = top,
        .width = static_cast<LONG>(width),
        .height = static_cast<LONG>(height),
    };
}

std::uint32_t WindowsTouchInjector::last_error() const noexcept {
    std::scoped_lock lock(impl_->mutex);
    return impl_->last_error;
}

std::size_t WindowsTouchInjector::active_contact_count() const noexcept {
    std::scoped_lock lock(impl_->mutex);
    return impl_->tracker.active_count();
}

}  // namespace remote_touch::input
