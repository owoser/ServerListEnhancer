#pragma once

#include "common/DiscoveryVisual.h"

#include <cstddef>
#include <filesystem>
#include <memory>
#include <optional>

namespace sle::client {

class ImageProvider {
public:
    static ImageProvider& getInstance();

    // initialize/stop are lifecycle operations, never render callbacks.
    void initialize(std::filesystem::path const& cacheDir);
    void start();
    void stop();
    void request(ImageReference const& reference, bool banner = false);
    [[nodiscard]] std::optional<std::filesystem::path> get(ImageReference const& reference, bool banner = false) const;
    [[nodiscard]] std::filesystem::path builtinPath(bool banner = false) const;

    static constexpr std::size_t MaxDownloadBytes = 2 * 1024 * 1024;
    static constexpr std::size_t MaxInlineBytes = 24 * 1024;
    static constexpr unsigned MaxDimension = 4096;
    static constexpr unsigned MaxPixels = 4'000'000;
    static constexpr std::size_t MaxCacheBytes = 128 * 1024 * 1024;

    ~ImageProvider();
    ImageProvider(ImageProvider const&) = delete;
    ImageProvider& operator=(ImageProvider const&) = delete;

private:
    ImageProvider();
    struct Impl;
    std::unique_ptr<Impl> mImpl;
};

} // namespace sle::client
