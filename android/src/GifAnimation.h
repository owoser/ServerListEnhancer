#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <vector>

namespace sle::android {

// 动态 GIF：解析全部帧、调色板合成与帧延迟，渲染时按经过的时间换帧。
// 限制与 Windows 端 src-client/GifAnimation.cpp 对齐：
//   源文件 <= 2 MiB、画布边长 <= 1024、帧数 <= 120、单次播放总时长 <= 30 s。
// 延迟字段单位是 1/100 秒；缺失或小于 20 ms 时按 100 ms 处理。
// 任何超限或解码失败都返回空，调用方回退为平台静态首帧（AndroidPlatform::decodeImage）。
// 该模块保持 header-only，因此不需要改动 android/CMakeLists.txt 的源文件列表。
struct GifLimits {
    std::size_t maxSourceBytes{2 * 1024 * 1024};
    int maxSide{1024};
    int maxFrames{120};
    std::size_t maxFrameBytes{64 * 1024 * 1024};
    int maxCycleMs{30000};
};

class GifAnimation {
public:
    int width{};
    int height{};
    // 0 表示无限循环；无 NETSCAPE 扩展时为 1（只播放一次）。
    int plays{1};
    std::vector<std::vector<unsigned char>> frames; // 每帧紧致 RGBA
    std::vector<int> delays;                        // 每帧毫秒

    bool valid() const { return !frames.empty() && delays.size() == frames.size(); }
    std::size_t frameCount() const { return frames.size(); }

    std::span<const unsigned char> frame(std::size_t index) const {
        if (index >= frames.size()) return {};
        return std::span<const unsigned char>(frames[index].data(), frames[index].size());
    }
    // 播放到 elapsedMs 时应显示的帧；有限次播放结束后停在最后一帧。
    std::size_t frameAt(std::int64_t elapsedMs) const {
        if (frames.empty() || delays.size() != frames.size()) return 0;
        std::int64_t total{};
        for (int delay : delays) total += delay;
        if (total <= 0) return 0;
        std::int64_t time = elapsedMs > 0 ? elapsedMs : 0;
        if (plays > 0 && time >= total * plays) return frames.size() - 1;
        time %= total;
        std::int64_t acc{};
        for (std::size_t i = 0; i < delays.size(); ++i) {
            acc += delays[i];
            if (time < acc) return i;
        }
        return frames.size() - 1;
    }
};

namespace detail {
using GifByte = unsigned char;
constexpr int GifMaxCodes = 4096;

struct GifPalette {
    GifByte rgb[256][3]{};
    int size{};
};

inline bool readGifPalette(std::span<const GifByte> bytes, std::size_t& pos, int count, GifPalette& palette) {
    if (count <= 0 || count > 256) return false;
    if (pos + static_cast<std::size_t>(count) * 3 > bytes.size()) return false;
    for (int i = 0; i < count; ++i) {
        palette.rgb[i][0] = bytes[pos++];
        palette.rgb[i][1] = bytes[pos++];
        palette.rgb[i][2] = bytes[pos++];
    }
    palette.size = count;
    return true;
}

inline bool skipGifSubBlocks(std::span<const GifByte> bytes, std::size_t& pos) {
    while (pos < bytes.size()) {
        const std::size_t size = bytes[pos++];
        if (size == 0) return true;
        if (pos + size > bytes.size()) return false;
        pos += size;
    }
    return false;
}

inline bool readGifSubBlocks(std::span<const GifByte> bytes, std::size_t& pos, std::size_t limit,
                             std::vector<GifByte>& out) {
    out.clear();
    while (pos < bytes.size()) {
        const std::size_t size = bytes[pos++];
        if (size == 0) return true;
        if (pos + size > bytes.size()) return false;
        if (out.size() + size > limit) return false;
        out.insert(out.end(), bytes.begin() + static_cast<std::ptrdiff_t>(pos),
                   bytes.begin() + static_cast<std::ptrdiff_t>(pos + size));
        pos += size;
    }
    return false;
}

inline bool decodeGifLzw(std::span<const GifByte> data, int minCodeSize, std::size_t expected,
                         std::vector<GifByte>& out) {
    if (minCodeSize < 1 || minCodeSize > 8) return false;
    const int clearCode = 1 << minCodeSize;
    const int endCode = clearCode + 1;
    int codeSize = minCodeSize + 1;
    int next = endCode + 1;
    int prefix[GifMaxCodes];
    GifByte suffix[GifMaxCodes];
    for (int i = 0; i < clearCode; ++i) {
        prefix[i] = -1;
        suffix[i] = static_cast<GifByte>(i);
    }
    out.clear();
    out.reserve(std::min<std::size_t>(expected, data.size() * 256));
    std::uint32_t bits{};
    int bitCount{};
    std::size_t pos{};
    int previous{-1};
    int chain[GifMaxCodes];
    while (true) {
        while (bitCount < codeSize) {
            if (pos >= data.size()) return out.size() >= expected;
            bits |= static_cast<std::uint32_t>(data[pos++]) << bitCount;
            bitCount += 8;
        }
        const int code = static_cast<int>(bits & ((1u << codeSize) - 1));
        bits >>= codeSize;
        bitCount -= codeSize;
        if (code == clearCode) {
            codeSize = minCodeSize + 1;
            next = endCode + 1;
            previous = -1;
            continue;
        }
        if (code == endCode) break;
        if (previous < 0) {
            if (code >= clearCode) return false;
            out.push_back(suffix[code]);
            if (out.size() > expected) return false;
            previous = code;
            continue;
        }
        int first{};
        if (code < next) {
            int c = code;
            int length{};
            while (c >= 0 && length < GifMaxCodes) {
                chain[length++] = c;
                c = prefix[c];
            }
            if (length >= GifMaxCodes) return false;
            first = suffix[chain[length - 1]];
            for (int i = length; i-- > 0;) out.push_back(suffix[chain[i]]);
        } else if (code == next) {
            int c = previous;
            int length{};
            while (c >= 0 && length < GifMaxCodes) {
                chain[length++] = c;
                c = prefix[c];
            }
            if (length >= GifMaxCodes) return false;
            first = suffix[chain[length - 1]];
            for (int i = length; i-- > 0;) out.push_back(suffix[chain[i]]);
            out.push_back(static_cast<GifByte>(first));
        } else {
            return false;
        }
        if (out.size() > expected) return false;
        if (next < GifMaxCodes) {
            prefix[next] = previous;
            suffix[next] = static_cast<GifByte>(first);
            ++next;
        }
        if (next == (1 << codeSize) && codeSize < 12) ++codeSize;
        previous = code;
    }
    return out.size() >= expected;
}

inline void clearGifRect(std::vector<GifByte>& canvas, int width, int height, int x, int y, int w, int h) {
    for (int row = 0; row < h; ++row) {
        const int py = y + row;
        if (py < 0 || py >= height) continue;
        for (int col = 0; col < w; ++col) {
            const int px = x + col;
            if (px < 0 || px >= width) continue;
            GifByte* pixel = &canvas[(static_cast<std::size_t>(py) * width + px) * 4];
            pixel[0] = pixel[1] = pixel[2] = pixel[3] = 0;
        }
    }
}

inline std::vector<GifByte> scaleGifFrame(std::span<const GifByte> source, int sw, int sh, int tw, int th) {
    std::vector<GifByte> out(static_cast<std::size_t>(tw) * th * 4);
    for (int y = 0; y < th; ++y) {
        const int sy = std::min(sh - 1, static_cast<int>(static_cast<std::int64_t>(y) * sh / th));
        for (int x = 0; x < tw; ++x) {
            const int sx = std::min(sw - 1, static_cast<int>(static_cast<std::int64_t>(x) * sw / tw));
            std::memcpy(&out[(static_cast<std::size_t>(y) * tw + x) * 4],
                        &source[(static_cast<std::size_t>(sy) * sw + sx) * 4], 4);
        }
    }
    return out;
}
} // namespace detail

inline std::optional<GifAnimation> decodeGifAnimation(std::span<const unsigned char> bytes, bool banner,
                                                      GifLimits const& limits = GifLimits{}) {
    using detail::GifByte;
    if (bytes.size() < 14 || bytes.size() > limits.maxSourceBytes) return std::nullopt;
    if (bytes[0] != 'G' || bytes[1] != 'I' || bytes[2] != 'F' || bytes[3] != '8') return std::nullopt;
    const int screenWidth = bytes[6] | (bytes[7] << 8);
    const int screenHeight = bytes[8] | (bytes[9] << 8);
    if (screenWidth <= 0 || screenHeight <= 0 || screenWidth > limits.maxSide ||
        screenHeight > limits.maxSide) {
        return std::nullopt;
    }
    const GifByte flags = bytes[10];
    detail::GifPalette global;
    std::size_t pos{13};
    if (flags & 0x80) {
        if (!detail::readGifPalette(bytes, pos, 2 << (flags & 7), global)) return std::nullopt;
    }
    GifAnimation animation;
    const std::size_t canvasBytes = static_cast<std::size_t>(screenWidth) * screenHeight * 4;
    std::vector<GifByte> canvas(canvasBytes, 0);
    std::vector<GifByte> saved;
    std::vector<GifByte> compressed;
    std::vector<GifByte> indices;
    int delay{100};
    int disposal{};
    int previousDisposal{};
    int transparentIndex{-1};
    int previousRect[4]{-1, -1, 0, 0};
    while (pos < bytes.size()) {
        const GifByte block = bytes[pos++];
        if (block == 0x3b) break; // trailer
        if (block == 0x21) {      // extension
            if (pos >= bytes.size()) return std::nullopt;
            const GifByte label = bytes[pos++];
            if (label == 0xf9) {
                // 图形控制扩展：[0x21 0xF9][size=4][flags, delayLo, delayHi, transparentIndex][0x00]
                if (pos + 6 > bytes.size()) return std::nullopt;
                if (bytes[pos] != 4) return std::nullopt;
                const GifByte control = bytes[pos + 1];
                delay = (bytes[pos + 2] | (bytes[pos + 3] << 8)) * 10;
                if (delay < 20) delay = 100;
                disposal = (control >> 2) & 7;
                transparentIndex = (control & 1) ? bytes[pos + 4] : -1;
                pos += 6;
                if (bytes[pos - 1] != 0) return std::nullopt;
                continue;
            }
            if (label == 0xff) {
                // 应用扩展：NETSCAPE2.0 / ANIMEXTS1.0 携带循环次数
                if (pos >= bytes.size()) return std::nullopt;
                const std::size_t blockSize = bytes[pos];
                if (pos + 1 + blockSize > bytes.size()) return std::nullopt;
                const bool netscape = blockSize == 11 &&
                    (std::memcmp(&bytes[pos + 1], "NETSCAPE2.0", 11) == 0 ||
                     std::memcmp(&bytes[pos + 1], "ANIMEXTS1.0", 11) == 0);
                pos += 1 + blockSize;
                if (netscape && pos < bytes.size() && bytes[pos] >= 3 && pos + 1 + bytes[pos] <= bytes.size() &&
                    bytes[pos + 1] == 1) {
                    const int loops = bytes[pos + 2] | (bytes[pos + 3] << 8);
                    animation.plays = loops ? loops + 1 : 0;
                }
            }
            if (!detail::skipGifSubBlocks(bytes, pos)) return std::nullopt;
            continue;
        }
        if (block != 0x2c) return std::nullopt; // image descriptor
        if (pos + 9 > bytes.size()) return std::nullopt;
        const int left = bytes[pos] | (bytes[pos + 1] << 8);
        const int top = bytes[pos + 2] | (bytes[pos + 3] << 8);
        const int width = bytes[pos + 4] | (bytes[pos + 5] << 8);
        const int height = bytes[pos + 6] | (bytes[pos + 7] << 8);
        const GifByte packed = bytes[pos + 8];
        pos += 9;
        if (width <= 0 || height <= 0 || left + width > screenWidth || top + height > screenHeight) {
            return std::nullopt;
        }
        detail::GifPalette local;
        if (packed & 0x80) {
            if (!detail::readGifPalette(bytes, pos, 2 << (packed & 7), local)) return std::nullopt;
        }
        const detail::GifPalette& palette = (packed & 0x80) ? local : global;
        if (palette.size <= 0) return std::nullopt;
        if (static_cast<int>(animation.frames.size()) >= limits.maxFrames) return std::nullopt;
        if ((animation.frames.size() + 1) * canvasBytes > limits.maxFrameBytes * 4) return std::nullopt;
        if (pos >= bytes.size()) return std::nullopt;
        const int minCodeSize = bytes[pos++];
        if (!detail::readGifSubBlocks(bytes, pos, canvasBytes + 1024, compressed)) return std::nullopt;
        if (!detail::decodeGifLzw(compressed, minCodeSize, static_cast<std::size_t>(width) * height, indices)) {
            return std::nullopt;
        }
        // 先按上一帧的处置方式还原画布，再决定本帧是否需要保存画布。
        if (previousDisposal == 2) {
            detail::clearGifRect(canvas, screenWidth, screenHeight, previousRect[0], previousRect[1],
                                 previousRect[2], previousRect[3]);
        } else if (previousDisposal == 3 && !saved.empty()) {
            canvas = saved;
        }
        if (disposal == 3) saved = canvas;
        // 隔行 GIF：按 0/8、4/8、2/4、1/2 四趟重建行顺序。
        std::vector<int> rowOrder;
        if (packed & 0x40) {
            static constexpr int starts[4]{0, 4, 2, 1};
            static constexpr int steps[4]{8, 8, 4, 2};
            rowOrder.reserve(static_cast<std::size_t>(height));
            for (int pass = 0; pass < 4; ++pass)
                for (int y = starts[pass]; y < height; y += steps[pass]) rowOrder.push_back(y);
            if (static_cast<int>(rowOrder.size()) != height) return std::nullopt;
        } else {
            rowOrder.resize(static_cast<std::size_t>(height));
            for (int y = 0; y < height; ++y) rowOrder[static_cast<std::size_t>(y)] = y;
        }
        for (int row = 0; row < height; ++row) {
            for (int col = 0; col < width; ++col) {
                const int index = indices[static_cast<std::size_t>(row) * width + col];
                if (index == transparentIndex) continue;
                const int px = left + col;
                const int py = top + rowOrder[static_cast<std::size_t>(row)];
                GifByte* pixel = &canvas[(static_cast<std::size_t>(py) * screenWidth + px) * 4];
                pixel[0] = palette.rgb[index][0];
                pixel[1] = palette.rgb[index][1];
                pixel[2] = palette.rgb[index][2];
                pixel[3] = 255;
            }
        }
        animation.frames.push_back(canvas);
        animation.delays.push_back(delay);
        previousDisposal = disposal;
        previousRect[0] = left;
        previousRect[1] = top;
        previousRect[2] = width;
        previousRect[3] = height;
        delay = 100;
        disposal = 0;
        transparentIndex = -1;
    }
    if (animation.frames.empty()) return std::nullopt;
    int cycle{};
    for (int value : animation.delays) cycle += value;
    if (cycle <= 0 || cycle > limits.maxCycleMs) return std::nullopt;
    // 目标尺寸贴合渲染区域（列表图 48px 逻辑宽、资料横幅 220px），再按总像素预算缩小。
    const int capWidth = banner ? 640 : 256;
    const int capHeight = banner ? 360 : 256;
    double ratio = std::min({1.0, static_cast<double>(capWidth) / screenWidth,
                             static_cast<double>(capHeight) / screenHeight});
    int targetWidth = std::max(1, static_cast<int>(screenWidth * ratio));
    int targetHeight = std::max(1, static_cast<int>(screenHeight * ratio));
    while (static_cast<std::size_t>(targetWidth) * targetHeight * 4 * animation.frames.size() >
               limits.maxFrameBytes &&
           (targetWidth > 16 && targetHeight > 16)) {
        ratio *= 0.85;
        targetWidth = std::max(16, static_cast<int>(screenWidth * ratio));
        targetHeight = std::max(16, static_cast<int>(screenHeight * ratio));
    }
    if (static_cast<std::size_t>(targetWidth) * targetHeight * 4 * animation.frames.size() >
        limits.maxFrameBytes) {
        return std::nullopt;
    }
    if (targetWidth != screenWidth || targetHeight != screenHeight) {
        for (auto& frame : animation.frames) {
            frame = detail::scaleGifFrame(frame, screenWidth, screenHeight, targetWidth, targetHeight);
        }
    }
    animation.width = targetWidth;
    animation.height = targetHeight;
    return animation;
}

} // namespace sle::android
