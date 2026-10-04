#include "input/touch_contact_tracker.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iostream>

namespace {

using remote_touch::input::ContactFrameStatus;
using remote_touch::input::ContactTransition;
using remote_touch::input::TouchContactTracker;
using remote_touch::input::TrackedContact;
using remote_touch::protocol::TouchAction;
using remote_touch::protocol::TouchEvent;

bool expect(const bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
    }
    return condition;
}

const TrackedContact* find_contact(
    const std::vector<TrackedContact>& contacts,
    const std::uint32_t pointer_id) {
    const auto found = std::find_if(
        contacts.begin(), contacts.end(), [pointer_id](const auto& contact) {
            return contact.pointer_id == pointer_id;
        });
    return found == contacts.end() ? nullptr : &*found;
}

TouchEvent touch(const TouchAction action, const std::uint32_t id,
                 const float x, const float y) {
    return TouchEvent{
        .action = action,
        .pointer_id = id,
        .x = x,
        .y = y,
        .pressure = action == TouchAction::up ? 0.0F : 1.0F,
    };
}

}  // namespace

int main() {
    bool passed = true;
    TouchContactTracker tracker{10};

    auto first_down = tracker.prepare(touch(TouchAction::down, 101, 0.2F, 0.7F));
    passed &= expect(first_down.status == ContactFrameStatus::ready,
                     "first finger down is accepted");
    passed &= expect(first_down.contacts.size() == 1,
                     "first frame contains one contact");
    passed &= expect(first_down.contacts[0].transition == ContactTransition::down,
                     "first finger is DOWN");
    tracker.commit(first_down);

    auto second_down = tracker.prepare(touch(TouchAction::down, 202, 0.8F, 0.6F));
    const auto* held_while_second_down = find_contact(second_down.contacts, 101);
    const auto* second = find_contact(second_down.contacts, 202);
    passed &= expect(second_down.contacts.size() == 2,
                     "second DOWN frame contains both fingers");
    passed &= expect(held_while_second_down &&
                         held_while_second_down->transition ==
                             ContactTransition::update,
                     "held joystick finger remains UPDATE");
    passed &= expect(second && second->transition == ContactTransition::down,
                     "skill finger is DOWN in the same frame");
    tracker.commit(second_down);
    passed &= expect(tracker.active_count() == 2,
                     "two contacts remain active");

    auto joystick_move =
        tracker.prepare(touch(TouchAction::move, 101, 0.3F, 0.65F));
    passed &= expect(joystick_move.contacts.size() == 2,
                     "move frame still contains both fingers");
    passed &= expect(find_contact(joystick_move.contacts, 101)->transition ==
                         ContactTransition::update &&
                         find_contact(joystick_move.contacts, 202)->transition ==
                             ContactTransition::update,
                     "both contacts are UPDATE while moving");
    tracker.commit(joystick_move);

    auto skill_up = tracker.prepare(touch(TouchAction::up, 202, 0.9F, 0.1F));
    const auto* held_while_skill_up = find_contact(skill_up.contacts, 101);
    const auto* released_skill = find_contact(skill_up.contacts, 202);
    passed &= expect(skill_up.contacts.size() == 2,
                     "UP frame contains held and released fingers");
    passed &= expect(held_while_skill_up &&
                         held_while_skill_up->transition ==
                             ContactTransition::update,
                     "joystick stays held while skill is released");
    passed &= expect(released_skill &&
                         released_skill->transition == ContactTransition::up,
                     "skill finger is UP");
    passed &= expect(released_skill &&
                         std::abs(released_skill->event.x - 0.8F) < 0.0001F &&
                         std::abs(released_skill->event.y - 0.6F) < 0.0001F,
                     "UP reuses the last injected location");
    tracker.commit(skill_up);
    passed &= expect(tracker.active_count() == 1,
                     "joystick remains active after skill release");

    const auto release = tracker.release_frame();
    passed &= expect(release.size() == 1 &&
                         release[0].transition == ContactTransition::cancel,
                     "disconnect cancels the remaining finger");

    return passed ? 0 : 1;
}
