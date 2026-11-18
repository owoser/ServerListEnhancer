#pragma once

#include "ll/api/data/VersionRequirement.h"

#include <string>
#include <string_view>

namespace sle::client {

inline constexpr std::string_view kMinimumLoaderVersion = "26.60.8";

// LL itself is preload-native, so a required native-mod dependency cannot enforce this floor.
// Keep the load guard and its boundary test on the same real LL version parser.
[[nodiscard]] inline bool isSupportedLoaderVersion(ll::data::Version const& version) {
    static ll::data::VersionRequirement const requirement{">=" + std::string{kMinimumLoaderVersion}};
    return requirement.matches(version);
}

} // namespace sle::client
