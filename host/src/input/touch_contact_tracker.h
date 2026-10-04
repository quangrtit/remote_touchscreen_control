#pragma once

#include "protocol/touch_packet.h"

#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace remote_touch::input {

enum class ContactTransition {
    down,
    update,
    up,
    cancel,
};

struct TrackedContact {
    std::uint32_t pointer_id{};
    std::uint32_t injection_id{};
    protocol::TouchEvent event{};
    ContactTransition transition{ContactTransition::update};
};

enum class ContactFrameStatus {
    ready,
    ignored,
    duplicate,
    missing,
    full,
};

struct ContactFrame {
    ContactFrameStatus status{ContactFrameStatus::ignored};
    protocol::TouchAction action{};
    std::uint32_t changed_pointer_id{};
    std::vector<TrackedContact> contacts;
};

// Builds atomic Windows injection frames. Every frame contains every active
// contact; only the changed pointer is DOWN/UP while the others are UPDATE.
class TouchContactTracker {
public:
    explicit TouchContactTracker(std::uint32_t max_contacts);

    [[nodiscard]] ContactFrame prepare(const protocol::TouchEvent& event) const;
    void commit(const ContactFrame& frame);
    [[nodiscard]] std::vector<TrackedContact> release_frame() const;
    void clear() noexcept;

    [[nodiscard]] std::size_t active_count() const noexcept;

private:
    struct ActiveContact {
        std::uint32_t injection_id{};
        protocol::TouchEvent last_event{};
    };

    [[nodiscard]] std::uint32_t allocate_injection_id() const noexcept;

    std::unordered_map<std::uint32_t, ActiveContact> active_;
    std::uint32_t max_contacts_{};
};

}  // namespace remote_touch::input
