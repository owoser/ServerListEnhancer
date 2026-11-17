#pragma once
#include <jni.h>
#include <atomic>
#include <cstdint>
#include <span>
#include <string>
#include <vector>
namespace sle::android {
struct Bytes {std::vector<unsigned char> data; std::string error; explicit operator bool()const{return error.empty()&&!data.empty();}};
struct Pixels {int width{},height{}; std::vector<unsigned char> rgba; explicit operator bool()const{return width>0&&height>0&&!rgba.empty();}};
struct Surface {float width{},height{},density{1};};
void setJavaVm(JavaVM* vm);
Surface surface();
Bytes download(const std::string& url, std::size_t limit, int timeoutMs=4000, const std::atomic<bool>* running=nullptr);
Pixels decodeImage(std::span<const unsigned char> bytes, bool banner);
Bytes decodeInlinePng(std::string_view value);
}
