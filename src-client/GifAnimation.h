#pragma once
#include <chrono>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>
struct IWICImagingFactory;
namespace sle::client {
struct GifAnimation {
 unsigned width{},height{},sourceWidth{},sourceHeight{};
 unsigned repeats{1}; // total plays; zero means forever
 std::vector<unsigned> delaysMs;
 std::vector<std::vector<unsigned char>> frames; // fully composited RGBA
 std::size_t reservedBytes{};
 ~GifAnimation();
 std::size_t frameAt(std::chrono::milliseconds elapsed) const;
};
bool isGif(std::span<unsigned char const> bytes);
std::shared_ptr<GifAnimation const> decodeGif(IWICImagingFactory*,std::span<unsigned char const>,bool banner,std::chrono::steady_clock::time_point deadline,std::string& error);
std::size_t gifMemoryBytes();
}
