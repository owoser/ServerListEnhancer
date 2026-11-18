#include "ClientConfig.h"

#include <nlohmann/json.hpp>

#include <atomic>
#include <fstream>
#include <string_view>
#include <system_error>

namespace sle::client {
namespace {
std::atomic<bool> gDebugLogging{false};

// 默认配置保持最小面：只暴露当前真正可调的开关。
constexpr std::string_view kDefaultConfig = R"({
    "version": 1,
    "debugLogging": false
})";
} // namespace

void loadClientConfig(std::filesystem::path const& configPath) {
    bool debug = false;

    std::error_code ec;
    if (!std::filesystem::exists(configPath, ec)) {
        std::filesystem::create_directories(configPath.parent_path(), ec);
        std::ofstream create(configPath, std::ios::binary | std::ios::trunc);
        if (create) {
            create << kDefaultConfig << '\n';
        }
    } else {
        std::ifstream input(configPath, std::ios::binary);
        if (input) {
            // 解析失败不抛异常（本工程禁用 C++ 异常），回退到默认关闭。
            auto json = nlohmann::json::parse(input, nullptr, false);
            if (json.is_object()) {
                if (auto it = json.find("debugLogging"); it != json.end() && it->is_boolean()) {
                    debug = it->get<bool>();
                }
            }
        }
    }

    gDebugLogging.store(debug);
}

bool debugLogging() { return gDebugLogging.load(); }

} // namespace sle::client
