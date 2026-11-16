#include "ServerStatus.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <string>
#include <thread>

#include <nlohmann/json.hpp>
#include <windows.h>
#include <winhttp.h>

#pragma comment(lib, "winhttp.lib")

namespace sle::client {

namespace {

using nlohmann::json;

constexpr int kTimeoutMs     = 4000;
constexpr int kRefreshSecs   = 60;
constexpr int kMaxConcurrent = 4;

std::int64_t nowSeconds() {
    return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch())
        .count();
}

std::wstring widen(std::string const& s) {
    if (s.empty()) {
        return {};
    }
    int          n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<std::size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

// GET {scheme}://{host}:{port}{path}，返回响应体
std::string httpGet(std::string const& host, int port, std::string const& path, bool https, bool allowInvalidCert) {
    std::string body;
    HINTERNET   session = WinHttpOpen(
        L"ServerListEnhancer/1.0",
        WINHTTP_ACCESS_TYPE_NO_PROXY,
        WINHTTP_NO_PROXY_NAME,
        WINHTTP_NO_PROXY_BYPASS,
        0
    );
    if (!session) {
        return body;
    }
    WinHttpSetTimeouts(session, kTimeoutMs, kTimeoutMs, kTimeoutMs, kTimeoutMs);

    HINTERNET connect = WinHttpConnect(session, widen(host).c_str(), static_cast<INTERNET_PORT>(port), 0);
    if (!connect) {
        WinHttpCloseHandle(session);
        return body;
    }

    HINTERNET request = WinHttpOpenRequest(
        connect,
        L"GET",
        widen(path).c_str(),
        nullptr,
        WINHTTP_NO_REFERER,
        WINHTTP_DEFAULT_ACCEPT_TYPES,
        https ? WINHTTP_FLAG_SECURE : 0
    );
    if (!request) {
        WinHttpCloseHandle(connect);
        WinHttpCloseHandle(session);
        return body;
    }

    if (https && allowInvalidCert) {
        DWORD flags = SECURITY_FLAG_IGNORE_UNKNOWN_CA | SECURITY_FLAG_IGNORE_CERT_DATE_INVALID
                    | SECURITY_FLAG_IGNORE_CERT_CN_INVALID | SECURITY_FLAG_IGNORE_CERT_WRONG_USAGE;
        WinHttpSetOption(request, WINHTTP_OPTION_SECURITY_FLAGS, &flags, sizeof(flags));
    }

    if (WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0)
        && WinHttpReceiveResponse(request, nullptr)) {
        DWORD code = 0;
        DWORD size = sizeof(code);
        WinHttpQueryHeaders(
            request,
            WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
            WINHTTP_HEADER_NAME_BY_INDEX,
            &code,
            &size,
            WINHTTP_NO_HEADER_INDEX
        );
        if (code == 200) {
            for (;;) {
                DWORD avail = 0;
                if (!WinHttpQueryDataAvailable(request, &avail) || avail == 0) {
                    break;
                }
                std::string chunk(avail, '\0');
                DWORD       read = 0;
                if (!WinHttpReadData(request, chunk.data(), avail, &read) || read == 0) {
                    break;
                }
                chunk.resize(read);
                body += chunk;
                if (body.size() > 64 * 1024) {
                    break;
                }
            }
        }
    }

    WinHttpCloseHandle(request);
    WinHttpCloseHandle(connect);
    WinHttpCloseHandle(session);
    return body;
}

ServerStatusResult query(std::string const& address, int port) {
    ServerStatusResult result;
    result.at = nowSeconds();

    // NetherNet 状态端点；先 https 再 http（某些服务器只开明文）
    std::string body = httpGet(address, port, "/v1/join", true, true);
    if (body.empty()) {
        body = httpGet(address, port, "/v1/join", false, false);
    }
    if (body.empty()) {
        result.error = "no response";
        return result;
    }

    try {
        auto j = json::parse(body);
        result.name       = j.value("name", std::string{});
        result.version    = j.value("version", std::string{});
        result.level      = j.value("level", std::string{});
        result.protocol   = j.value("protocol", 0);
        result.players    = j.value("players", -1);
        result.maxPlayers = j.value("maxPlayers", -1);
        result.ok         = !result.name.empty();
        if (!result.ok) {
            result.error = "empty name";
        }
    } catch (...) {
        result.error = "bad json";
        result.at    = nowSeconds();
    }
    return result;
}

} // namespace

ServerStatus& ServerStatus::getInstance() {
    static ServerStatus instance;
    return instance;
}

void ServerStatus::request(std::string const& key, std::string const& address, int port) {
    if (address.empty() || key.empty()) {
        return;
    }
    auto now = nowSeconds();
    {
        std::lock_guard lock(mMutex);
        if (mInFlight.contains(key)) {
            return;
        }
        if (auto it = mResults.find(key); it != mResults.end() && now - it->second.at < kRefreshSecs) {
            return;
        }
        if (mConcurrent >= kMaxConcurrent) {
            return;
        }
        ++mConcurrent;
        mInFlight[key] = now;
    }

    std::thread([this, key, address, port]() {
        auto result = query(address, port);
        {
            std::lock_guard lock(mMutex);
            mResults[key] = result;
            mInFlight.erase(key);
            --mConcurrent;
        }
    }).detach();
}

std::optional<ServerStatusResult> ServerStatus::get(std::string const& key) const {
    std::lock_guard lock(mMutex);
    if (auto it = mResults.find(key); it != mResults.end()) {
        return it->second;
    }
    return std::nullopt;
}

} // namespace sle::client