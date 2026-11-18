#pragma once

#include "ll/api/mod/NativeMod.h"

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
    ll::mod::NativeMod& mSelf;
};

} // namespace sle::server
