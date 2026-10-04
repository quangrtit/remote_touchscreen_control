#pragma once

#include "protocol/touch_packet.h"

namespace remote_touch::input {

class TouchSink {
public:
    virtual ~TouchSink() = default;

    virtual bool inject(const protocol::TouchEvent& event) noexcept = 0;
    virtual void release_all() noexcept = 0;
};

}  // namespace remote_touch::input
