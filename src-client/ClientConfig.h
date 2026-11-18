#pragma once

#include <filesystem>

namespace sle::client {

// 读取 config/config.json 中的调试开关；文件缺失时写入默认配置（调试日志关闭）。
void loadClientConfig(std::filesystem::path const& configPath);

// 输入、命中与图片等高频诊断日志是否输出。默认 false，避免鼠标移动刷屏。
[[nodiscard]] bool debugLogging();

} // namespace sle::client
