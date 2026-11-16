#include "ServerStatus.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <fstream>
#include <limits>
#include <utility>

#include <nlohmann/json.hpp>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winhttp.h>

#pragma comment(lib, "winhttp.lib")

namespace sle::client {
namespace {

using nlohmann::json;
using Clock = std::chrono::steady_clock;
constexpr int kTimeoutMs = 4000;
constexpr int kRefreshSecs = 60;
constexpr int kMaxConcurrent = 4;
constexpr std::size_t kMaxBodyBytes = 64 * 1024;
constexpr std::size_t kMaxCacheBytes = 4 * 1024 * 1024;
constexpr std::size_t kMaxCacheEntries = 4096;

std::int64_t nowSeconds() {
    return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch())
        .count();
}

std::wstring widen(std::string const& text) {
    if (text.empty() || text.size() > static_cast<std::size_t>((std::numeric_limits<int>::max)())) {
        return {};
    }
    auto const size = static_cast<int>(text.size());
    int const count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), size, nullptr, 0);
    if (count <= 0) {
        return {};
    }
    std::wstring result(static_cast<std::size_t>(count), L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), size, result.data(), count) != count) {
        return {};
    }
    return result;
}

bool validEndpoint(std::string const& address, int port) {
    return !address.empty() && address.size() <= 1024 && port > 0 && port <= 65535
        && std::none_of(address.begin(), address.end(), [](unsigned char c) {
               return c <= 32 || c == 127 || c == '/' || c == '\\' || c == '?' || c == '#' || c == '@';
           });
}

// Validate BEFORE appending, including chunked responses without Content-Length.
bool appendBody(std::string& body, char const* bytes, std::size_t count) {
    if (body.size() > kMaxBodyBytes || count > kMaxBodyBytes - body.size()) {
        return false;
    }
    body.append(bytes, count);
    return true;
}

struct InternetHandle {
    HINTERNET value{};
    explicit InternetHandle(HINTERNET handle = nullptr) : value(handle) {}
    ~InternetHandle() { if (value) WinHttpCloseHandle(value); }
    InternetHandle(InternetHandle const&) = delete;
    InternetHandle& operator=(InternetHandle const&) = delete;
};

// Asynchronous WinHTTP lets the worker enforce ONE deadline across DNS, connect,
// TLS, headers, chunks and the optional HTTP fallback (not four seconds per read).
// Closing waits for HANDLE_CLOSING, so callback context can never outlive its owner.
class HttpOperation {
public:
    enum class Stage { None, Sent, Headers, Available, Read };

    bool open(HINTERNET connection, bool https) {
        mRequest = WinHttpOpenRequest(
            connection, L"GET", L"/v1/join", nullptr, WINHTTP_NO_REFERER,
            WINHTTP_DEFAULT_ACCEPT_TYPES, https ? WINHTTP_FLAG_SECURE : 0
        );
        if (!mRequest) return false;
        DWORD_PTR context = reinterpret_cast<DWORD_PTR>(this);
        if (!WinHttpSetOption(mRequest, WINHTTP_OPTION_CONTEXT_VALUE, &context, sizeof(context))) return false;
        auto const previous = WinHttpSetStatusCallback(
            mRequest, callback, WINHTTP_CALLBACK_FLAG_ALL_COMPLETIONS | WINHTTP_CALLBACK_FLAG_HANDLES
                | WINHTTP_CALLBACK_FLAG_SECURE_FAILURE, 0
        );
        mObservingClose = previous != WINHTTP_INVALID_STATUS_CALLBACK;
        return mObservingClose;
    }

    ~HttpOperation() {
        if (!mRequest) return;
        if (WinHttpCloseHandle(mRequest) && mObservingClose) {
            std::unique_lock lock(mMutex);
            mChanged.wait(lock, [this] { return mClosed; });
        }
    }
    HttpOperation() = default;
    HttpOperation(HttpOperation const&) = delete;
    HttpOperation& operator=(HttpOperation const&) = delete;

    HINTERNET handle() const { return mRequest; }
    void begin() {
        std::lock_guard lock(mMutex);
        mCompleted = Stage::None;
        mCount = 0;
    }
    bool initiated(BOOL ok) {
        if (ok || GetLastError() == ERROR_IO_PENDING) return true;
        std::lock_guard lock(mMutex);
        mError = GetLastError();
        return false;
    }
    bool wait(Stage stage, Clock::time_point deadline, std::stop_token cancellation) {
        std::stop_callback wake(cancellation, [this] {
            std::lock_guard lock(mMutex);
            mChanged.notify_all();
        });
        std::unique_lock lock(mMutex);
        bool const signalled = mChanged.wait_until(lock, deadline, [&] {
            return mCompleted == stage || mError || mClosed || cancellation.stop_requested();
        });
        if (cancellation.stop_requested()) { mError = ERROR_WINHTTP_OPERATION_CANCELLED; return false; }
        if (!signalled || Clock::now() >= deadline) { mError = ERROR_WINHTTP_TIMEOUT; return false; }
        return !mError && !mClosed && mCompleted == stage;
    }
    DWORD count() const { std::lock_guard lock(mMutex); return mCount; }
    DWORD error() const { std::lock_guard lock(mMutex); return mError; }
    DWORD secureFailure() const { std::lock_guard lock(mMutex); return mSecureFailure; }

private:
    static void CALLBACK callback(HINTERNET, DWORD_PTR context, DWORD status, void* info, DWORD length) {
        auto* self = reinterpret_cast<HttpOperation*>(context);
        if (!self) return;
        std::lock_guard lock(self->mMutex);
        switch (status) {
        case WINHTTP_CALLBACK_STATUS_SENDREQUEST_COMPLETE: self->mCompleted = Stage::Sent; break;
        case WINHTTP_CALLBACK_STATUS_HEADERS_AVAILABLE: self->mCompleted = Stage::Headers; break;
        case WINHTTP_CALLBACK_STATUS_DATA_AVAILABLE:
            self->mCompleted = Stage::Available;
            if (info && length >= sizeof(DWORD)) std::memcpy(&self->mCount, info, sizeof(DWORD));
            break;
        case WINHTTP_CALLBACK_STATUS_READ_COMPLETE:
            self->mCompleted = Stage::Read;
            self->mCount = length;
            break;
        case WINHTTP_CALLBACK_STATUS_REQUEST_ERROR:
            self->mError = info && length >= sizeof(WINHTTP_ASYNC_RESULT)
                ? static_cast<WINHTTP_ASYNC_RESULT*>(info)->dwError : ERROR_WINHTTP_INTERNAL_ERROR;
            break;
        case WINHTTP_CALLBACK_STATUS_SECURE_FAILURE:
            if (info && length >= sizeof(DWORD)) std::memcpy(&self->mSecureFailure, info, sizeof(DWORD));
            break;
        case WINHTTP_CALLBACK_STATUS_HANDLE_CLOSING: self->mClosed = true; break;
        default: break;
        }
        // Notify while holding the mutex: the final callback releases it only
        // after its last access, before the destructor can destroy this context.
        self->mChanged.notify_all();
    }

    HINTERNET mRequest{};
    mutable std::mutex mMutex;
    std::condition_variable mChanged;
    Stage mCompleted{Stage::None};
    DWORD mCount{};
    DWORD mError{};
    DWORD mSecureFailure{};
    bool mClosed{false};
    bool mObservingClose{false};
};

struct HttpReply {
    std::string body;
    std::string error;
    bool allowFallback{false};
};

bool fallbackForTransport(DWORD error, DWORD secureFailure) {
    // Do not turn a certificate rejection into a silent HTTP downgrade. A plain
    // NetherNet listener normally reports only SECURITY_CHANNEL_ERROR.
    if (secureFailure & ~WINHTTP_CALLBACK_STATUS_FLAG_SECURITY_CHANNEL_ERROR) return false;
    return error == ERROR_WINHTTP_CANNOT_CONNECT || error == ERROR_WINHTTP_CONNECTION_ERROR
        || (error == ERROR_WINHTTP_SECURE_FAILURE
            && secureFailure == WINHTTP_CALLBACK_STATUS_FLAG_SECURITY_CHANNEL_ERROR);
}

HttpReply httpGet(
    std::string const& address, int port, bool https, ServerStatusTransportOptions options,
    Clock::time_point deadline, std::stop_token cancellation
) {
    HttpReply reply;
    if (cancellation.stop_requested() || Clock::now() >= deadline) {
        reply.error = "request cancelled or deadline exceeded";
        return reply;
    }
    auto host = address;
    if (host.size() > 2 && host.front() == '[' && host.back() == ']') host = host.substr(1, host.size() - 2);
    auto const wideHost = widen(host);
    if (wideHost.empty()) { reply.error = "invalid UTF-8 host"; return reply; }
    InternetHandle session(WinHttpOpen(
        L"ServerListEnhancer/1.0", WINHTTP_ACCESS_TYPE_NO_PROXY, WINHTTP_NO_PROXY_NAME,
        WINHTTP_NO_PROXY_BYPASS, WINHTTP_FLAG_ASYNC
    ));
    if (!session.value) { reply.error = "WinHttpOpen failed"; return reply; }
    if (!WinHttpSetTimeouts(session.value, kTimeoutMs, kTimeoutMs, kTimeoutMs, kTimeoutMs)) {
        reply.error = "WinHttpSetTimeouts failed"; return reply;
    }
    InternetHandle connection(WinHttpConnect(session.value, wideHost.c_str(), static_cast<INTERNET_PORT>(port), 0));
    if (!connection.value) { reply.error = "WinHttpConnect failed"; return reply; }
    // Declared before operation: the read buffer must survive cancellation and HANDLE_CLOSING.
    std::array<char, 8192> buffer{};
    HttpOperation operation;
    if (!operation.open(connection.value, https)) { reply.error = "WinHttpOpenRequest failed"; return reply; }
    DWORD const redirectPolicy = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
    if (!WinHttpSetOption(operation.handle(), WINHTTP_OPTION_REDIRECT_POLICY,
            const_cast<DWORD*>(&redirectPolicy), sizeof(redirectPolicy))) {
        reply.error = "cannot disable redirects"; return reply;
    }
    if (https && options.allowSelfSignedNetherNet) {
        DWORD flags = SECURITY_FLAG_IGNORE_UNKNOWN_CA | SECURITY_FLAG_IGNORE_CERT_DATE_INVALID
            | SECURITY_FLAG_IGNORE_CERT_CN_INVALID | SECURITY_FLAG_IGNORE_CERT_WRONG_USAGE;
        if (!WinHttpSetOption(operation.handle(), WINHTTP_OPTION_SECURITY_FLAGS, &flags, sizeof(flags))) {
            reply.error = "cannot apply explicit NetherNet TLS policy"; return reply;
        }
    }
    auto failed = [&]() {
        reply.body.clear();
        reply.error = "WinHTTP error " + std::to_string(operation.error());
        reply.allowFallback = !cancellation.stop_requested() && Clock::now() < deadline
            && fallbackForTransport(operation.error(), operation.secureFailure());
        return reply;
    };
    operation.begin();
    if (!operation.initiated(WinHttpSendRequest(
            operation.handle(), WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0,
            reinterpret_cast<DWORD_PTR>(&operation)))
        || !operation.wait(HttpOperation::Stage::Sent, deadline, cancellation)) return failed();
    operation.begin();
    if (!operation.initiated(WinHttpReceiveResponse(operation.handle(), nullptr))
        || !operation.wait(HttpOperation::Stage::Headers, deadline, cancellation)) return failed();
    DWORD code = 0;
    DWORD size = sizeof(code);
    if (!WinHttpQueryHeaders(operation.handle(), WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
            WINHTTP_HEADER_NAME_BY_INDEX, &code, &size, WINHTTP_NO_HEADER_INDEX)) {
        reply.error = "missing HTTP status"; return reply;
    }
    if (code != 200) { reply.error = "HTTP " + std::to_string(code); return reply; }
    // Preflight a numeric Content-Length when present; chunk limits below remain authoritative.
    DWORD contentLength = 0;
    size = sizeof(contentLength);
    if (WinHttpQueryHeaders(operation.handle(), WINHTTP_QUERY_CONTENT_LENGTH | WINHTTP_QUERY_FLAG_NUMBER,
            WINHTTP_HEADER_NAME_BY_INDEX, &contentLength, &size, WINHTTP_NO_HEADER_INDEX)
        && contentLength > kMaxBodyBytes) {
        reply.error = "response exceeds 64 KiB"; return reply;
    }
    for (;;) {
        operation.begin();
        if (!operation.initiated(WinHttpQueryDataAvailable(operation.handle(), nullptr))
            || !operation.wait(HttpOperation::Stage::Available, deadline, cancellation)) return failed();
        DWORD const available = operation.count();
        if (!available) return reply;
        if (reply.body.size() > kMaxBodyBytes || available > kMaxBodyBytes - reply.body.size()) {
            reply.body.clear(); reply.error = "response exceeds 64 KiB"; return reply;
        }
        DWORD const wanted = (std::min)(available, static_cast<DWORD>(buffer.size()));
        operation.begin();
        if (!operation.initiated(WinHttpReadData(operation.handle(), buffer.data(), wanted, nullptr))
            || !operation.wait(HttpOperation::Stage::Read, deadline, cancellation)) return failed();
        DWORD const read = operation.count();
        if (!read) return reply;
        if (read > wanted || !appendBody(reply.body, buffer.data(), read)) {
            reply.body.clear(); reply.error = "response exceeds 64 KiB"; return reply;
        }
    }
}

std::string stringField(json const& object, char const* name, std::size_t limit = kMaxBodyBytes) {
    auto const it = object.find(name);
    if (it == object.end() || !it->is_string()) return {};
    auto const& value = it->get_ref<std::string const&>();
    return value.size() <= limit ? value : std::string{};
}

int intField(json const& object, char const* name, int fallback) {
    auto const it = object.find(name);
    if (it == object.end() || !it->is_number_integer()) return fallback;
    if (it->is_number_unsigned()) {
        auto const value = it->get<std::uint64_t>();
        return value <= static_cast<std::uint64_t>((std::numeric_limits<int>::max)())
            ? static_cast<int>(value) : fallback;
    }
    auto const value = it->get<std::int64_t>();
    return value >= (std::numeric_limits<int>::min)() && value <= (std::numeric_limits<int>::max)()
        ? static_cast<int>(value) : fallback;
}

ImageReference parseImage(json const& object, char const* name) {
    auto const it = object.find(name);
    if (it == object.end() || !it->is_object()) return {};
    auto type = stringField(*it, "type", 16);
    auto value = stringField(*it, "value", type=="data" ? 32790 : type=="url" ? 512 : 64);
    if (type != "builtin" && type != "url" && type != "data") return {};
    if (value.empty()) return {};
    return {std::move(type), std::move(value)};
}

std::optional<DiscoveryVisual> parseVisual(json const& extension) {
    if (!extension.is_object() || intField(extension, "schemaVersion", 0) != 1) return std::nullopt;
    DiscoveryVisual visual;
    auto enabled = extension.find("enabled");
    if (enabled != extension.end()) {
        if (!enabled->is_boolean()) return std::nullopt;
        visual.enabled = enabled->get<bool>();
    }
    visual.revision = stringField(extension, "revision", 64);
    visual.serverId = stringField(extension, "serverId", 64);
    visual.introText = stringField(extension, "introText", 1024);
    visual.logo = parseImage(extension, "logo");
    visual.introImage = parseImage(extension, "introImage");
    visual.cacheTtlSeconds = std::clamp(intField(extension, "cacheTtlSeconds", 300), 1, 86400);
    return visual;
}

json visualJson(DiscoveryVisual const& visual) {
    return {{"schemaVersion", 1}, {"enabled", visual.enabled}, {"revision", visual.revision},
        {"serverId", visual.serverId}, {"introText", visual.introText},
        {"cacheTtlSeconds", visual.cacheTtlSeconds},
        {"logo", {{"type", visual.logo.type}, {"value", visual.logo.value}}},
        {"introImage", {{"type", visual.introImage.type}, {"value", visual.introImage.value}}}};
}

ServerStatusResult parseStatus(std::string const& body) {
    ServerStatusResult result;
    result.at = nowSeconds();
    if (body.size() > kMaxBodyBytes) { result.error = "response exceeds 64 KiB"; return result; }
    try {
        auto const object = json::parse(body);
        if (!object.is_object()) { result.error = "expected JSON object"; return result; }
        result.name = stringField(object, "name");
        result.version = stringField(object, "version");
        result.level = stringField(object, "level");
        result.protocol = intField(object, "protocol", 0);
        result.players = intField(object, "players", -1);
        result.maxPlayers = intField(object, "maxPlayers", -1);
        result.ok = !result.name.empty();
        if (!result.ok) result.error = "empty name";
        auto const extension = object.find("server_list_enhancer");
        result.visualFieldPresent = extension != object.end();
        if (result.visualFieldPresent) result.visual = parseVisual(*extension);
    } catch (...) { result.error = "bad json"; }
    return result;
}

ServerStatusResult query(
    std::string const& address, int port, ServerStatusTransportOptions options, std::stop_token cancellation
) {
    auto const deadline = Clock::now() + std::chrono::milliseconds(kTimeoutMs);
    auto response = httpGet(address, port, true, options, deadline, cancellation);
    if (!response.error.empty() && response.allowFallback && options.allowPlainHttpFallback) {
        response = httpGet(address, port, false, options, deadline, cancellation);
    }
    if (!response.error.empty()) {
        ServerStatusResult result; result.at = nowSeconds(); result.error = std::move(response.error); return result;
    }
    if (cancellation.stop_requested() || Clock::now() >= deadline) {
        ServerStatusResult result; result.at = nowSeconds(); result.error = "request cancelled or deadline exceeded";
        return result;
    }
    return parseStatus(response.body);
}

} // namespace

ServerStatus& ServerStatus::getInstance() {
    static ServerStatus instance;
    return instance;
}
ServerStatus::~ServerStatus() { stop(); }

void ServerStatus::setCachePath(std::filesystem::path const& file) {
    std::lock_guard lifecycle(mLifecycleMutex);
    std::lock_guard lock(mMutex);
    if (!mRunning) mCachePath = file;
}

void ServerStatus::start(ServerStatusTransportOptions options) {
    std::lock_guard lifecycle(mLifecycleMutex);
    {
        std::lock_guard lock(mMutex);
        if (mRunning) return;
        mOptions = options;
    }
    loadCache();
    {
        std::lock_guard lock(mMutex);
        mRunning = true;
    }
    try {
        mWorkers.reserve(kMaxConcurrent);
        for (int i = 0; i < kMaxConcurrent; ++i) mWorkers.emplace_back([this] { worker(); });
    } catch (...) {
        {
            std::lock_guard lock(mMutex);
            mRunning = false;
            for (auto& [key, entry] : mEntries) {
                if (entry.cancellation) entry.cancellation->request_stop();
                entry.pending = false;
                entry.generation = ++mGeneration;
            }
            mQueue.clear();
        }
        mWork.notify_all();
        for (auto& thread : mWorkers) if (thread.joinable()) thread.join();
        mWorkers.clear();
        throw;
    }
}

void ServerStatus::stop() {
    std::lock_guard lifecycle(mLifecycleMutex);
    {
        std::lock_guard lock(mMutex);
        mRunning = false;
        mQueue.clear();
        for (auto& [key, entry] : mEntries) {
            if (entry.cancellation) entry.cancellation->request_stop();
            entry.pending = false;
            entry.generation = ++mGeneration;
            entry.nextRefresh = {};
            if (entry.result.state == ServerStatusState::Queued || entry.result.state == ServerStatusState::Loading) {
                entry.result.state = entry.result.ok ? ServerStatusState::Ready : ServerStatusState::Idle;
            }
        }
    }
    mWork.notify_all();
    for (auto& thread : mWorkers) if (thread.joinable()) thread.join();
    mWorkers.clear();
    saveCache();
}

void ServerStatus::request(std::string const& key, std::string const& address, int port, bool force) {
    requestImpl(key, address, port, force, std::nullopt);
}
void ServerStatus::request(
    std::string const& key, std::string const& address, int port, bool force, ServerStatusTransportOptions options
) {
    requestImpl(key, address, port, force, options);
}

void ServerStatus::requestImpl(
    std::string const& key, std::string const& address, int port, bool force,
    std::optional<ServerStatusTransportOptions> suppliedOptions
) {
    if (key.empty() || key.size() > 4096 || !validEndpoint(address, port)) return;
    {
        std::lock_guard lock(mMutex);
        if (!mRunning) return;
        auto const options = suppliedOptions.value_or(mOptions);
        auto [it, inserted] = mEntries.try_emplace(key);
        auto& entry = it->second;
        bool const endpointChanged = !inserted && (entry.address != address || entry.port != port);
        bool const policyChanged = !inserted && entry.options != options;
        if (endpointChanged || policyChanged) {
            if (entry.cancellation) entry.cancellation->request_stop();
            std::erase_if(mQueue, [&](Task const& queued) { return queued.key == key; });
            entry.pending = false;
            entry.nextRefresh = {};
            if (endpointChanged) entry.result = {};
            mCacheDirty = true;
        }
        if (entry.pending || (!force && Clock::now() < entry.nextRefresh)) return;
        entry.address = address;
        entry.port = port;
        entry.options = options;
        entry.generation = ++mGeneration;
        entry.cancellation = std::make_shared<std::stop_source>();
        // Queue insertion precedes marking pending, so allocation failure cannot
        // strand an entry permanently in Queued without a task.
        mQueue.push_back({key, address, port, options, entry.generation, entry.cancellation});
        entry.pending = true;
        entry.result.state = ServerStatusState::Queued;
    }
    mWork.notify_one();
}

void ServerStatus::invalidate(std::string const& key) {
    {
        std::lock_guard lock(mMutex);
        auto const it = mEntries.find(key);
        if (it != mEntries.end()) {
            if (it->second.cancellation) it->second.cancellation->request_stop();
            mEntries.erase(it);
            mCacheDirty = true;
        }
        std::erase_if(mQueue, [&](Task const& queued) { return queued.key == key; });
        ++mGeneration;
    }
    mWork.notify_one();
}

std::optional<ServerStatusResult> ServerStatus::get(std::string const& key) const {
    std::lock_guard lock(mMutex);
    auto const it = mEntries.find(key);
    return it == mEntries.end() ? std::nullopt : std::optional{it->second.result};
}

void ServerStatus::worker() {
    for (;;) {
        Task task;
        {
            std::unique_lock lock(mMutex);
            mWork.wait(lock, [this] { return !mRunning || !mQueue.empty() || mCacheDirty; });
            if (!mRunning) return;
            if (mQueue.empty()) { lock.unlock(); saveCache(); continue; }
            task = std::move(mQueue.front());
            mQueue.pop_front();
            auto const it = mEntries.find(task.key);
            if (it == mEntries.end() || it->second.generation != task.generation) continue;
            it->second.result.state = ServerStatusState::Loading;
        }
        ServerStatusResult result;
        try {
            result = mQuery ? mQuery(task.address, task.port, task.options, task.cancellation->get_token())
                            : query(task.address, task.port, task.options, task.cancellation->get_token());
        } catch (...) { result.error = "status query failed"; }
        if (!result.at) result.at = nowSeconds();
        {
            std::lock_guard lock(mMutex);
            auto const it = mEntries.find(task.key);
            if (!mRunning || it == mEntries.end() || it->second.generation != task.generation
                || task.cancellation->stop_requested()) continue;
            auto& entry = it->second;
            int const refresh = result.ok && result.visual ? result.visual->cacheTtlSeconds : kRefreshSecs;
            auto visual = entry.result.visual;
            if (result.visual) {
                if (result.visual->enabled) visual = result.visual;
                else visual.reset(); // Known schema enabled:false explicitly revokes cached visuals.
            }
            if (!result.ok) {
                // Health failure must not erase a previous successful name or visual.
                auto previous = entry.result;
                previous.ok = false;
                previous.error = std::move(result.error);
                previous.at = result.at;
                previous.visualFieldPresent = result.visualFieldPresent;
                result = std::move(previous);
            }
            result.visual = std::move(visual);
            result.state = result.ok ? ServerStatusState::Ready : ServerStatusState::Failed;
            entry.result = std::move(result);
            entry.pending = false;
            entry.cancellation.reset();
            entry.nextRefresh = Clock::now() + std::chrono::seconds(std::clamp(refresh, 1, 86400));
            mCacheDirty = true;
        }
        saveCache();
    }
}

void ServerStatus::loadCache() {
    std::lock_guard disk(mCacheMutex);
    std::filesystem::path file;
    { std::lock_guard lock(mMutex); file = mCachePath; }
    if (file.empty()) return;
    try {
        std::ifstream stream(file, std::ios::binary | std::ios::ate);
        if (!stream) return;
        auto const size = stream.tellg();
        if (size <= 0 || size > static_cast<std::streamoff>(kMaxCacheBytes)) return;
        std::string bytes(static_cast<std::size_t>(size), '\0');
        stream.seekg(0);
        if (!stream.read(bytes.data(), static_cast<std::streamsize>(bytes.size()))) return;
        auto const document = json::parse(bytes);
        if (!document.is_object() || intField(document, "schemaVersion", 0) != 1) return;
        auto const items = document.find("entries");
        if (items == document.end() || !items->is_array() || items->size() > kMaxCacheEntries) return;
        std::lock_guard lock(mMutex);
        // Do not resurrect entries removed in memory when restarting this same instance.
        if (!mEntries.empty() || mCacheDirty) return;
        for (auto const& item : *items) {
            if (!item.is_object()) continue;
            auto key = stringField(item, "key", 4096);
            auto address = stringField(item, "address", 1024);
            int const port = intField(item, "port", 0);
            auto const snapshot = item.find("result");
            if (key.empty() || !validEndpoint(address, port) || snapshot == item.end() || !snapshot->is_object()) continue;
            auto const serialized = snapshot->dump();
            auto result = parseStatus(serialized);
            if (auto ok = snapshot->find("ok"); ok != snapshot->end() && ok->is_boolean()) result.ok = ok->get<bool>();
            if (auto at = snapshot->find("at"); at != snapshot->end() && at->is_number_integer()) {
                result.at = at->get<std::int64_t>();
            }
            result.error = stringField(*snapshot, "error", 4096);
            result.state = result.ok ? ServerStatusState::Ready : ServerStatusState::Failed;
            if (result.visual && !result.visual->enabled) result.visual.reset();
            if (!result.ok && !result.visual) continue;
            Entry entry;
            entry.result = std::move(result);
            entry.address = std::move(address);
            entry.port = port;
            entry.options = mOptions;
            entry.generation = ++mGeneration;
            // A disk hit is usable immediately, but always eligible for background refresh.
            mEntries.emplace(std::move(key), std::move(entry));
        }
    } catch (...) { /* Corrupt/unsupported caches never prevent startup. */ }
}

void ServerStatus::saveCache() {
    std::lock_guard disk(mCacheMutex);
    std::filesystem::path file;
    json items = json::array();
    {
        std::lock_guard lock(mMutex);
        if (!mCacheDirty) return;
        file = mCachePath;
        mCacheDirty = false;
        if (file.empty()) return;
        for (auto const& [key, entry] : mEntries) {
            auto const& result = entry.result;
            if (!result.ok && !result.visual) continue;
            json snapshot = {{"ok", result.ok}, {"at", result.at}, {"name", result.name},
                {"version", result.version}, {"level", result.level}, {"protocol", result.protocol},
                {"players", result.players}, {"maxPlayers", result.maxPlayers}, {"error", result.error}};
            if (result.visual) snapshot["server_list_enhancer"] = visualJson(*result.visual);
            items.push_back({{"key", key}, {"address", entry.address}, {"port", entry.port}, {"result", std::move(snapshot)}});
            if (items.size() >= kMaxCacheEntries) break;
        }
    }
    try {
        auto const bytes = json{{"schemaVersion", 1}, {"entries", std::move(items)}}.dump();
        if (bytes.size() > kMaxCacheBytes) return;
        std::error_code error;
        auto const parent = file.parent_path();
        if (!parent.empty()) std::filesystem::create_directories(parent, error);
        if (error) return;
        auto temporary = file;
        temporary += L".tmp";
        {
            std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
            if (!stream) return;
            stream.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
            stream.flush();
            if (!stream) return;
        }
        // Same-directory replace: interrupted writes cannot leave partial discovery.json.
        MoveFileExW(temporary.c_str(), file.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
    } catch (...) { /* A read-only/full cache disk must not kill a worker. */ }
}

} // namespace sle::client
