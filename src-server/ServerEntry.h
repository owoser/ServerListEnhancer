#pragma once

#include "ll/api/event/EventBus.h"
#include "ll/api/mod/NativeMod.h"

#include <memory>

namespace sle::server {

class ServerEntry {
public:
    static ServerEntry& getInstance();

    ServerEntry();

    [[nodiscard]] ll::mod::NativeMod& getSelf() const { return mSelf; }

    bool load();
    bool enable();
    bool disable();
    bool unload();

private:
    ll::mod::NativeMod&   mSelf;
    ll::event::ListenerPtr mListener{};
};

} // namespace sle::server
