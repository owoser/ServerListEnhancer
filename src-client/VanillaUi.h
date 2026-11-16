#pragma once

#include <algorithm>
#include <array>
#include <string_view>

namespace sle::client::vanilla {

// JSON UI coordinates are already scaled by the game's GUI scale. Do not
// multiply these by the window height (that would apply a second scale).
struct Rect {
    float x0{}, y0{}, x1{}, y1{};
    bool contains(float x, float y) const { return x >= x0 && x < x1 && y >= y0 && y < y1; }
    float width() const { return x1 - x0; }
    float height() const { return y1 - y0; }
};

struct Rgb { int r, g, b; };
constexpr float channel(int byte) { return static_cast<float>(byte) / 255.0f; }
enum class ButtonState { Normal, Hover, Pressed };

inline constexpr std::array<std::string_view, 3> ButtonTextures{
    "textures/ui/button_borderless_light",
    "textures/ui/button_borderless_lighthover",
    "textures/ui/button_borderless_lightpressed"
};

// Exact 4x4 pixels from the vanilla textures, also used if texture loading is
// still pending. Each corner and each edge is a discrete patch, NOT a gradient.
inline constexpr std::array<std::array<Rgb, 9>, 3> ButtonPixels{{
    {{{254,254,254}, {247,247,247}, {192,191,192},
      {247,247,247}, {198,198,198}, {101,100,101},
      {192,191,192}, {101,100,101}, {89,87,89}}},
    {{{23,205,7}, {23,205,7}, {33,131,6},
      {23,205,7}, {33,131,6}, {0,78,0},
      {33,131,6}, {0,78,0}, {0,78,0}}},
    {{{0,78,0}, {0,78,0}, {33,131,6},
      {0,78,0}, {33,131,6}, {23,205,7},
      {33,131,6}, {23,205,7}, {23,205,7}}}
}};

struct Slice { Rect target; Rect uv; };
inline std::array<Slice, 9> nineSlice(Rect r, float sourceSize, float border = 1.0f) {
    float const bx = std::min(border, std::max(0.0f, r.width() * 0.5f));
    float const by = std::min(border, std::max(0.0f, r.height() * 0.5f));
    std::array<float,4> const x{r.x0, r.x0 + bx, r.x1 - bx, r.x1};
    std::array<float,4> const y{r.y0, r.y0 + by, r.y1 - by, r.y1};
    std::array<float,4> const uv{0.0f, 1.0f / sourceSize, 1.0f - 1.0f / sourceSize, 1.0f};
    std::array<Slice,9> result{};
    for (int j = 0; j < 3; ++j) for (int i = 0; i < 3; ++i)
        result[j * 3 + i] = {{x[i], y[j], x[i+1], y[j+1]}, {uv[i], uv[j], uv[i+1], uv[j+1]}};
    return result;
}
inline Rect inset(Rect r, float b) { return {r.x0+b, r.y0+b, r.x1-b, r.y1-b}; }
inline Rect menuEntry(Rect play, float rowStep) {
    return {play.x0, play.y0-rowStep, play.x1, play.y1-rowStep};
}
inline bool isInlineModal(std::string_view name) {
    return name == "modal_dialog_popup" || name == "modal_input";
}
inline bool allowOverlay(bool isStartScreen, bool playVisible, bool inlineModalVisible, bool focusModalActive) {
    return isStartScreen && playVisible && !inlineModalVisible && !focusModalActive;
}

} // namespace sle::client::vanilla
