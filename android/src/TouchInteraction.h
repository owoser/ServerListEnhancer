#pragma once

#include <optional>
#include <span>
#include <string>

namespace sle::android {

struct TouchTarget {
    float x{}, y{}, w{}, h{};
    int action{};
    std::string id;

    bool contains(float px, float py) const {
        return px >= x && py >= y && px < x + w && py < y + h;
    }
    bool same(const TouchTarget& other) const {
        return action == other.action && id == other.id && x == other.x && y == other.y
            && w == other.w && h == other.h;
    }
};

// Retain the control identity and geometry, never an index into a frame's rebuilt hit list.
class TouchPress {
    std::optional<TouchTarget> target;
    bool inside{};
public:
    void cancel() { target.reset(); inside = false; }
    void begin(const TouchTarget& hit) { target = hit; inside = true; }
    void move(float x, float y) { inside = inside && target && target->contains(x, y); }
    bool isPressed(const TouchTarget& hit) const { return inside && target && target->same(hit); }
    std::optional<TouchTarget> release(std::span<const TouchTarget> current, float x, float y) {
        const bool wasInside = inside;
        auto previous = std::move(target);
        cancel();
        if (wasInside && previous && previous->contains(x, y))
            for (const auto& hit : current)
                if (previous->same(hit)) return hit;
        return std::nullopt;
    }
};

} // namespace sle::android
