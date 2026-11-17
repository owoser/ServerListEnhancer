#pragma once
#include <string>

namespace sle {
struct ImageReference {
    std::string type{"builtin"};
    std::string value{"default"};
    bool operator==(ImageReference const&) const = default;
};
struct DiscoveryVisual {
    std::string revision;
    std::string serverId;
    std::string introText;
    ImageReference logo;
    ImageReference introImage;
    int udpProbePort{};
    int cacheTtlSeconds{300};
    bool enabled{true};
    bool operator==(DiscoveryVisual const&) const = default;
};
}
