#include "input/touch_contact_tracker.h"

#include <algorithm>

namespace remote_touch::input {

TouchContactTracker::TouchContactTracker(const std::uint32_t max_contacts)
    : max_contacts_(std::clamp(max_contacts, 1U, 256U)) {}

std::uint32_t TouchContactTracker::allocate_injection_id() const noexcept {
    for (std::uint32_t candidate = 1; candidate <= max_contacts_; ++candidate) {
        const bool used = std::any_of(
            active_.begin(), active_.end(), [candidate](const auto& entry) {
                return entry.second.injection_id == candidate;
            });
        if (!used) {
            return candidate;
        }
    }
    return 0;
}

ContactFrame TouchContactTracker::prepare(
    const protocol::TouchEvent& event) const {
    ContactFrame frame{
        .status = ContactFrameStatus::ready,
        .action = event.action,
        .changed_pointer_id = event.pointer_id,
        .contacts = {},
    };
    frame.contacts.reserve(active_.size() + 1U);
    for (const auto& [pointer_id, active] : active_) {
        frame.contacts.push_back(TrackedContact{
            .pointer_id = pointer_id,
            .injection_id = active.injection_id,
            .event = active.last_event,
            .transition = ContactTransition::update,
        });
    }

    const auto existing = active_.find(event.pointer_id);
    if (event.action == protocol::TouchAction::down) {
        if (existing != active_.end()) {
            frame.status = ContactFrameStatus::duplicate;
            frame.contacts.clear();
            return frame;
        }
        const auto injection_id = allocate_injection_id();
        if (injection_id == 0) {
            frame.status = ContactFrameStatus::full;
            frame.contacts.clear();
            return frame;
        }
        frame.contacts.push_back(TrackedContact{
            .pointer_id = event.pointer_id,
            .injection_id = injection_id,
            .event = event,
            .transition = ContactTransition::down,
        });
        return frame;
    }

    if (existing == active_.end()) {
        frame.status = event.action == protocol::TouchAction::move
                           ? ContactFrameStatus::missing
                           : ContactFrameStatus::ignored;
        frame.contacts.clear();
        return frame;
    }

    auto changed = std::find_if(
        frame.contacts.begin(), frame.contacts.end(),
        [&event](const TrackedContact& contact) {
            return contact.pointer_id == event.pointer_id;
        });
    if (event.action == protocol::TouchAction::move) {
        changed->event = event;
        changed->transition = ContactTransition::update;
    } else {
        // Windows requires UP to use exactly the location from the preceding
        // successfully injected frame, not the possibly newer browser UP point.
        changed->event.pressure = 0.0F;
        changed->transition = event.action == protocol::TouchAction::cancel
                                  ? ContactTransition::cancel
                                  : ContactTransition::up;
    }
    return frame;
}

void TouchContactTracker::commit(const ContactFrame& frame) {
    if (frame.status != ContactFrameStatus::ready) {
        return;
    }
    const auto changed = std::find_if(
        frame.contacts.begin(), frame.contacts.end(),
        [&frame](const TrackedContact& contact) {
            return contact.pointer_id == frame.changed_pointer_id;
        });
    if (changed == frame.contacts.end()) {
        return;
    }

    switch (frame.action) {
        case protocol::TouchAction::down:
            active_.emplace(
                changed->pointer_id,
                ActiveContact{.injection_id = changed->injection_id,
                              .last_event = changed->event});
            break;
        case protocol::TouchAction::move:
            if (const auto active = active_.find(changed->pointer_id);
                active != active_.end()) {
                active->second.last_event = changed->event;
            }
            break;
        case protocol::TouchAction::up:
        case protocol::TouchAction::cancel:
            active_.erase(changed->pointer_id);
            break;
    }
}

std::vector<TrackedContact> TouchContactTracker::release_frame() const {
    std::vector<TrackedContact> contacts;
    contacts.reserve(active_.size());
    for (const auto& [pointer_id, active] : active_) {
        auto event = active.last_event;
        event.pressure = 0.0F;
        contacts.push_back(TrackedContact{
            .pointer_id = pointer_id,
            .injection_id = active.injection_id,
            .event = event,
            .transition = ContactTransition::cancel,
        });
    }
    return contacts;
}

void TouchContactTracker::clear() noexcept { active_.clear(); }

std::size_t TouchContactTracker::active_count() const noexcept {
    return active_.size();
}

}  // namespace remote_touch::input
