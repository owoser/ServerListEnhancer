#pragma once

#include "ll/api/event/EventBus.h"
#include "ll/api/mod/NativeMod.h"

namespace sle::client {

class ClientEntry {
public:
    static ClientEntry& getInstance();

    ClientEntry();

    [[nodiscard]] ll::mod::NativeMod& getSelf() const { return mSelf; }

    bool load();
    bool enable();
    bool disable();
    bool unload();

private:
    ll::mod::NativeMod&    mSelf;
    ll::event::ListenerPtr mJoinListener{};
    ll::event::ListenerPtr mExitListener{};
};

} // namespace sle::client
