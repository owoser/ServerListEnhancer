#include "ImageProvider.h"
#include "common/BuiltinImages.h"
#include "GifAnimation.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winhttp.h>
#include <wincodec.h>
#include <bcrypt.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <fstream>
#include <limits>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace sle::client {
namespace image_detail {
using Clock = std::chrono::steady_clock;
using Microsoft::WRL::ComPtr;
using Bytes = std::vector<unsigned char>;
static constexpr auto RequestTimeout = std::chrono::seconds(10);
static constexpr auto RetryDelay = std::chrono::minutes(5);
static constexpr std::size_t MaxEntries = 1024;
static constexpr std::string_view DataPrefix = "data:image/png;base64,";

struct ComScope {
    HRESULT result{CoInitializeEx(nullptr, COINIT_MULTITHREADED)};
    bool usable() const {return SUCCEEDED(result)||result==RPC_E_CHANGED_MODE;}
    ~ComScope() { if (SUCCEEDED(result)) CoUninitialize(); }
};

static bool isBuiltin(ImageReference const& ref) {
    return ref.type == "builtin" && ref.value == "default";
}
static bool validReference(ImageReference const& ref) {
    if (isBuiltin(ref)) return true;
    if (ref.type == "url")
        return ref.value.size() <= 4096 && !ref.value.empty()
            && (ref.value.starts_with("https://") || ref.value.starts_with("http://"))
            && ref.value.find_first_of("\r\n\t ") == std::string::npos
            && ref.value.find('\0') == std::string::npos;
    if (ref.type != "base64" && ref.type != "data") return false;
    return ref.value.starts_with(DataPrefix)
        && ref.value.size() <= DataPrefix.size() + ((ImageProvider::MaxInlineBytes + 2) / 3) * 4;
}
static std::string referenceKey(ImageReference const& ref, bool banner) {
    std::string result = banner ? "banner:" : "logo:";
    result += ref.type;
    result += ':';
    result += ref.value;
    return result;
}
static std::optional<Bytes> decodeInline(std::string_view value) {
    if (!value.starts_with(DataPrefix)) return std::nullopt;
    value.remove_prefix(DataPrefix.size());
    if (value.empty() || value.size() % 4 != 0
        || value.size() > ((ImageProvider::MaxInlineBytes + 2) / 3) * 4) return std::nullopt;
    auto digit = [](char c) -> int {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        if (c == '+') return 62;
        if (c == '/') return 63;
        return -1;
    };
    Bytes bytes;
    bytes.reserve(value.size() / 4 * 3);
    for (std::size_t i = 0; i < value.size(); i += 4) {
        int const a = digit(value[i]), b = digit(value[i + 1]);
        bool const p2 = value[i + 2] == '=', p3 = value[i + 3] == '=';
        int const c = p2 ? 0 : digit(value[i + 2]), d = p3 ? 0 : digit(value[i + 3]);
        if (a < 0 || b < 0 || c < 0 || d < 0 || (p2 && !p3)
            || ((p2 || p3) && i + 4 != value.size())
            || (p2 && (b & 15)) || (p3 && !p2 && (c & 3))) return std::nullopt;
        bytes.push_back(static_cast<unsigned char>((a << 2) | (b >> 4)));
        if (!p2) bytes.push_back(static_cast<unsigned char>((b << 4) | (c >> 2)));
        if (!p3) bytes.push_back(static_cast<unsigned char>((c << 6) | d));
        if (bytes.size() > ImageProvider::MaxInlineBytes) return std::nullopt;
    }
    return bytes;
}
static bool pngSignature(std::span<unsigned char const> bytes) {
    static constexpr std::array<unsigned char, 8> Png{137, 80, 78, 71, 13, 10, 26, 10};
    return bytes.size() >= Png.size() && std::equal(Png.begin(), Png.end(), bytes.begin());
}
static bool supportedSignature(std::span<unsigned char const> bytes) {
    return isGif(bytes) || pngSignature(bytes) || (bytes.size() >= 3 && bytes[0] == 0xff && bytes[1] == 0xd8 && bytes[2] == 0xff);
}
static std::string sha256(std::span<unsigned char const> bytes) {
    BCRYPT_ALG_HANDLE algorithm{};
    BCRYPT_HASH_HANDLE hash{};
    if (!BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0))) return {};
    std::array<unsigned char, 32> result{};
    bool const ok = BCRYPT_SUCCESS(BCryptCreateHash(algorithm, &hash, nullptr, 0, nullptr, 0, 0))
        && BCRYPT_SUCCESS(BCryptHashData(hash, const_cast<PUCHAR>(bytes.data()), static_cast<ULONG>(bytes.size()), 0))
        && BCRYPT_SUCCESS(BCryptFinishHash(hash, result.data(), static_cast<ULONG>(result.size()), 0));
    if (hash) BCryptDestroyHash(hash);
    BCryptCloseAlgorithmProvider(algorithm, 0);
    if (!ok) return {};
    static constexpr char Hex[] = "0123456789abcdef";
    std::string encoded;
    encoded.reserve(64);
    for (auto c : result) { encoded += Hex[c >> 4]; encoded += Hex[c & 15]; }
    return encoded;
}
static std::string sha256(std::string_view bytes) {
    return sha256(std::span{reinterpret_cast<unsigned char const*>(bytes.data()), bytes.size()});
}
static bool validHash(std::string_view value) {
    return value.size() == 64 && std::all_of(value.begin(), value.end(), [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    });
}

struct HttpHandle {
    HINTERNET value{};
    ~HttpHandle() { if (value) WinHttpCloseHandle(value); }
    explicit operator bool() const { return value != nullptr; }
};
struct HttpCompletion {
    HANDLE event{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
    std::atomic<DWORD> status{};
    std::atomic<DWORD> count{};
    std::array<unsigned char, 16384> buffer{};
    std::mutex closeMutex;
    std::condition_variable closeChanged;
    bool closed{};
    ~HttpCompletion() { if (event) CloseHandle(event); }
};
struct HttpCallbackOwner { std::shared_ptr<HttpCompletion> completion; };
static void CALLBACK httpCallback(HINTERNET, DWORD_PTR context, DWORD status, void* data, DWORD length) {
    auto* owner = reinterpret_cast<HttpCallbackOwner*>(context);
    if (!owner) return;
    if (status == WINHTTP_CALLBACK_STATUS_HANDLE_CLOSING) {
        auto completion=owner->completion;
        delete owner;
        std::lock_guard lock(completion->closeMutex);
        completion->closed=true;
        completion->closeChanged.notify_all();
        return;
    }
    auto const& completion = owner->completion;
    switch (status) {
    case WINHTTP_CALLBACK_STATUS_SENDREQUEST_COMPLETE:
    case WINHTTP_CALLBACK_STATUS_HEADERS_AVAILABLE:
    case WINHTTP_CALLBACK_STATUS_DATA_AVAILABLE:
    case WINHTTP_CALLBACK_STATUS_READ_COMPLETE:
    case WINHTTP_CALLBACK_STATUS_REQUEST_ERROR:
        completion->count.store(status == WINHTTP_CALLBACK_STATUS_DATA_AVAILABLE && data
            ? *static_cast<DWORD*>(data) : (status == WINHTTP_CALLBACK_STATUS_READ_COMPLETE ? length : 0));
        completion->status.store(status);
        SetEvent(completion->event);
        break;
    default: break;
    }
}
static bool waitHttp(std::shared_ptr<HttpCompletion> const& completion, DWORD expected,
    Clock::time_point deadline, std::atomic_bool const& stopping) {
    while (!stopping.load()) {
        auto const remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count();
        if (remaining <= 0) return false;
        auto const wait = WaitForSingleObject(completion->event, static_cast<DWORD>(std::min<std::int64_t>(remaining, 100)));
        if (wait == WAIT_OBJECT_0) return completion->status.load() == expected;
        if (wait != WAIT_TIMEOUT) return false;
    }
    return false;
}
static void resetHttp(std::shared_ptr<HttpCompletion> const& completion) {
    completion->status.store(0);
    completion->count.store(0);
    ResetEvent(completion->event);
}
static std::optional<Bytes> download(std::string const& url, Clock::time_point deadline, std::atomic_bool const& stopping) {
    if (url.empty() || url.size() > 4096 || url.find('\0') != std::string::npos) return std::nullopt;
    int const count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, url.data(), static_cast<int>(url.size()), nullptr, 0);
    if (count <= 0) return std::nullopt;
    std::wstring wide(static_cast<std::size_t>(count), L'\0');
    if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, url.data(), static_cast<int>(url.size()), wide.data(), count)) return std::nullopt;
    URL_COMPONENTS parts{};
    parts.dwStructSize = sizeof(parts);
    parts.dwSchemeLength = parts.dwHostNameLength = parts.dwUrlPathLength = parts.dwExtraInfoLength = static_cast<DWORD>(-1);
    parts.dwUserNameLength = parts.dwPasswordLength = static_cast<DWORD>(-1);
    if (!WinHttpCrackUrl(wide.c_str(), static_cast<DWORD>(wide.size()), 0, &parts)
        || (parts.nScheme != INTERNET_SCHEME_HTTP && parts.nScheme != INTERNET_SCHEME_HTTPS)
        || !parts.dwHostNameLength || parts.dwUserNameLength || parts.dwPasswordLength
        || wide.find(L'#') != std::wstring::npos) return std::nullopt;
    std::wstring const host{parts.lpszHostName, parts.dwHostNameLength};
    std::wstring target{parts.lpszUrlPath, parts.dwUrlPathLength};
    if (target.empty()) target = L"/";
    if (parts.dwExtraInfoLength) target.append(parts.lpszExtraInfo, parts.dwExtraInfoLength);
    HttpHandle session{WinHttpOpen(L"ServerListEnhancer/1", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, WINHTTP_FLAG_ASYNC)};
    if (!session) return std::nullopt;
    WinHttpSetTimeouts(session.value, 4000, 4000, 4000, 4000);
    HttpHandle connection{WinHttpConnect(session.value, host.c_str(), parts.nPort, 0)};
    if (!connection) return std::nullopt;
    wchar_t const* accept[]{L"image/png", L"image/jpeg", L"image/gif", nullptr};
    HttpHandle request{WinHttpOpenRequest(connection.value, L"GET", target.c_str(), nullptr,
        WINHTTP_NO_REFERER, accept, parts.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0)};
    if (!request) return std::nullopt;
    DWORD disabled = WINHTTP_DISABLE_REDIRECTS | WINHTTP_DISABLE_COOKIES | WINHTTP_DISABLE_AUTHENTICATION;
    if (!WinHttpSetOption(request.value, WINHTTP_OPTION_DISABLE_FEATURE, &disabled, sizeof(disabled))) return std::nullopt;
    auto completion = std::make_shared<HttpCompletion>();
    if (!completion->event) return std::nullopt;
    if (WinHttpSetStatusCallback(request.value, httpCallback,
        WINHTTP_CALLBACK_FLAG_ALL_COMPLETIONS | WINHTTP_CALLBACK_FLAG_HANDLES, 0) == WINHTTP_INVALID_STATUS_CALLBACK) return std::nullopt;
    auto* owner = new HttpCallbackOwner{completion};
    DWORD_PTR const context = reinterpret_cast<DWORD_PTR>(owner);
    if (!WinHttpSetOption(request.value, WINHTTP_OPTION_CONTEXT_VALUE, const_cast<DWORD_PTR*>(&context), sizeof(context))) {
        delete owner;
        return std::nullopt;
    }
    // Join the request callback lifetime before a worker can stop/unload.
    struct RequestDrain {
        HttpHandle& request; std::shared_ptr<HttpCompletion> completion;
        ~RequestDrain(){
            if(request.value){WinHttpCloseHandle(request.value);request.value=nullptr;}
            std::unique_lock lock(completion->closeMutex);
            completion->closeChanged.wait(lock,[&]{return completion->closed;});
        }
    } drain{request,completion};
    resetHttp(completion);
    if (!WinHttpSendRequest(request.value, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, context)
        && GetLastError() != ERROR_IO_PENDING) return std::nullopt;
    if (!waitHttp(completion, WINHTTP_CALLBACK_STATUS_SENDREQUEST_COMPLETE, deadline, stopping)) return std::nullopt;
    resetHttp(completion);
    if (!WinHttpReceiveResponse(request.value, nullptr) && GetLastError() != ERROR_IO_PENDING) return std::nullopt;
    if (!waitHttp(completion, WINHTTP_CALLBACK_STATUS_HEADERS_AVAILABLE, deadline, stopping)) return std::nullopt;
    DWORD code{}, codeSize = sizeof(code);
    if (!WinHttpQueryHeaders(request.value, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
        WINHTTP_HEADER_NAME_BY_INDEX, &code, &codeSize, WINHTTP_NO_HEADER_INDEX) || code != 200) return std::nullopt;
    wchar_t contentType[128]{};
    DWORD typeSize = sizeof(contentType);
    if (WinHttpQueryHeaders(request.value, WINHTTP_QUERY_CONTENT_TYPE, WINHTTP_HEADER_NAME_BY_INDEX,
        contentType, &typeSize, WINHTTP_NO_HEADER_INDEX)) {
        std::wstring type{contentType};
        auto const semicolon = type.find(L';');
        if (semicolon != std::wstring::npos) type.resize(semicolon);
        if (_wcsicmp(type.c_str(), L"image/png") != 0 && _wcsicmp(type.c_str(), L"image/jpeg") != 0 && _wcsicmp(type.c_str(), L"image/gif") != 0) return std::nullopt;
    } else if (GetLastError() != ERROR_WINHTTP_HEADER_NOT_FOUND) return std::nullopt;
    wchar_t contentLength[64]{};
    DWORD lengthSize = sizeof(contentLength);
    if (WinHttpQueryHeaders(request.value, WINHTTP_QUERY_CONTENT_LENGTH, WINHTTP_HEADER_NAME_BY_INDEX,
        contentLength, &lengthSize, WINHTTP_NO_HEADER_INDEX)) {
        std::uint64_t length{};
        if (!contentLength[0]) return std::nullopt;
        for (wchar_t c : std::wstring_view{contentLength}) {
            if (c < L'0' || c > L'9' || length > ImageProvider::MaxDownloadBytes / 10) return std::nullopt;
            length = length * 10 + static_cast<unsigned>(c - L'0');
            if (length > ImageProvider::MaxDownloadBytes) return std::nullopt;
        }
    } else if (GetLastError() != ERROR_WINHTTP_HEADER_NOT_FOUND) return std::nullopt;
    Bytes bytes;
    while (Clock::now() < deadline && !stopping.load()) {
        resetHttp(completion);
        if (!WinHttpQueryDataAvailable(request.value, nullptr) && GetLastError() != ERROR_IO_PENDING) return std::nullopt;
        if (!waitHttp(completion, WINHTTP_CALLBACK_STATUS_DATA_AVAILABLE, deadline, stopping)) return std::nullopt;
        auto const available = completion->count.load();
        if (!available) return bytes.empty() ? std::nullopt : std::optional<Bytes>{std::move(bytes)};
        if (available > ImageProvider::MaxDownloadBytes - bytes.size()) return std::nullopt;
        DWORD const toRead = std::min<DWORD>(available, static_cast<DWORD>(completion->buffer.size()));
        resetHttp(completion);
        if (!WinHttpReadData(request.value, completion->buffer.data(), toRead, nullptr)
            && GetLastError() != ERROR_IO_PENDING) return std::nullopt;
        if (!waitHttp(completion, WINHTTP_CALLBACK_STATUS_READ_COMPLETE, deadline, stopping)) return std::nullopt;
        DWORD const read = completion->count.load();
        if (!read || read > toRead || read > ImageProvider::MaxDownloadBytes - bytes.size()) return std::nullopt;
        bytes.insert(bytes.end(), completion->buffer.begin(), completion->buffer.begin() + read);
    }
    return std::nullopt;
}

static std::optional<Bytes> normalize(IWICImagingFactory* factory, std::span<unsigned char const> input,
    bool banner, Clock::time_point deadline) {
    if (!factory || input.empty() || input.size() > ImageProvider::MaxDownloadBytes || !supportedSignature(input)
        || Clock::now() >= deadline) return std::nullopt;
    ComPtr<IWICStream> stream;
    ComPtr<IWICBitmapDecoder> decoder;
    ComPtr<IWICBitmapFrameDecode> frame;
    GUID container{};
    UINT count{}, width{}, height{};
    if (FAILED(factory->CreateStream(&stream))
        || FAILED(stream->InitializeFromMemory(const_cast<BYTE*>(input.data()), static_cast<DWORD>(input.size())))
        || FAILED(factory->CreateDecoderFromStream(stream.Get(), nullptr, WICDecodeMetadataCacheOnDemand, &decoder))
        || FAILED(decoder->GetContainerFormat(&container))
        || (container != GUID_ContainerFormatPng && container != GUID_ContainerFormatJpeg && container != GUID_ContainerFormatGif)
        || FAILED(decoder->GetFrameCount(&count)) || !count || (container != GUID_ContainerFormatGif && count != 1)
        || FAILED(decoder->GetFrame(0, &frame)) || FAILED(frame->GetSize(&width, &height))
        || !width || !height || width > ImageProvider::MaxDimension || height > ImageProvider::MaxDimension
        || static_cast<std::uint64_t>(width) * height > ImageProvider::MaxPixels) return std::nullopt;
    unsigned const maxWidth = banner ? 1600 : 512;
    unsigned const maxHeight = banner ? 900 : 512;
    double const scale = std::min({1.0, static_cast<double>(maxWidth) / width, static_cast<double>(maxHeight) / height});
    UINT outWidth = std::max(1u, static_cast<UINT>(width * scale));
    UINT outHeight = std::max(1u, static_cast<UINT>(height * scale));
    for (int attempt = 0; attempt != 4 && Clock::now() < deadline; ++attempt) {
        ComPtr<IWICBitmapScaler> scaler;
        ComPtr<IWICFormatConverter> converter;
        IWICBitmapSource* source = frame.Get();
        if (width != outWidth || height != outHeight) {
            if (FAILED(factory->CreateBitmapScaler(&scaler))
                || FAILED(scaler->Initialize(source, outWidth, outHeight, WICBitmapInterpolationModeFant))) return std::nullopt;
            source = scaler.Get();
        }
        if (FAILED(factory->CreateFormatConverter(&converter))
            || FAILED(converter->Initialize(source, GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone,
                nullptr, 0.0, WICBitmapPaletteTypeCustom))) return std::nullopt;
        UINT const stride = outWidth * 4;
        Bytes pixels(static_cast<std::size_t>(stride) * outHeight);
        if (FAILED(converter->CopyPixels(nullptr, stride, static_cast<UINT>(pixels.size()), pixels.data()))
            || Clock::now() >= deadline) return std::nullopt;
        ComPtr<IStream> output;
        ComPtr<IWICBitmapEncoder> encoder;
        ComPtr<IWICBitmapFrameEncode> encodedFrame;
        WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
        if (FAILED(CreateStreamOnHGlobal(nullptr, TRUE, &output))
            || FAILED(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder))
            || FAILED(encoder->Initialize(output.Get(), WICBitmapEncoderNoCache))
            || FAILED(encoder->CreateNewFrame(&encodedFrame, nullptr))
            || FAILED(encodedFrame->Initialize(nullptr)) || FAILED(encodedFrame->SetSize(outWidth, outHeight))
            || FAILED(encodedFrame->SetPixelFormat(&format)) || format != GUID_WICPixelFormat32bppBGRA
            || FAILED(encodedFrame->WritePixels(outHeight, stride, static_cast<UINT>(pixels.size()), pixels.data()))
            || FAILED(encodedFrame->Commit()) || FAILED(encoder->Commit())) return std::nullopt;
        STATSTG stat{};
        HGLOBAL memory{};
        if (FAILED(output->Stat(&stat, STATFLAG_NONAME)) || stat.cbSize.HighPart || !stat.cbSize.LowPart
            || FAILED(GetHGlobalFromStream(output.Get(), &memory))) return std::nullopt;
        if (stat.cbSize.LowPart <= ImageProvider::MaxDownloadBytes) {
            auto const* data = static_cast<unsigned char const*>(GlobalLock(memory));
            if (!data) return std::nullopt;
            Bytes result(data, data + stat.cbSize.LowPart);
            GlobalUnlock(memory);
            return Clock::now() < deadline ? std::optional<Bytes>{std::move(result)} : std::nullopt;
        }
        outWidth = std::max(1u, outWidth / 2);
        outHeight = std::max(1u, outHeight / 2);
    }
    return std::nullopt;
}
static std::optional<Bytes> readBounded(std::filesystem::path const& path, std::size_t limit) {
    std::error_code ec;
    auto const size = std::filesystem::file_size(path, ec);
    if (ec || !size || size > limit) return std::nullopt;
    std::ifstream input(path, std::ios::binary);
    if (!input) return std::nullopt;
    Bytes bytes(static_cast<std::size_t>(size));
    if (!input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()))) return std::nullopt;
    char extra{};
    if (input.get(extra)) return std::nullopt;
    return bytes;
}
static bool atomicWrite(std::filesystem::path const& destination, std::span<unsigned char const> bytes) {
    auto temporary = destination;
    temporary += L".tmp";
    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        if (!output || !output.write(reinterpret_cast<char const*>(bytes.data()), static_cast<std::streamsize>(bytes.size()))) return false;
        output.flush();
        if (!output) return false;
    }
    if (MoveFileExW(temporary.c_str(), destination.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) return true;
    std::error_code ec;
    std::filesystem::remove(temporary, ec);
    return false;
}
} // namespace image_detail

struct ImageProvider::Impl {
    using Clock = image_detail::Clock;
    struct Entry {
        std::optional<std::filesystem::path> path;
        std::shared_ptr<GifAnimation const> animation;
        Clock::time_point retryAt{};
        Clock::time_point lastUse{Clock::now()};
        bool pending{};
    };
    struct Job { ImageReference reference; bool banner{}; std::string key; };
    struct DiskResult { std::optional<std::filesystem::path> path; bool fresh{}; };
    std::mutex lifecycle;
    mutable std::mutex mutex;
    std::mutex diskMutex;
    std::condition_variable condition;
    std::atomic_bool stopping{true};
    bool started{};
    bool visible{};
    std::filesystem::path directory;
    std::array<std::filesystem::path, 2> defaults;
    std::unordered_map<std::string, Entry> entries;
    std::unordered_map<std::filesystem::path, Clock::time_point> access;
    std::deque<Job> queue;
    std::array<std::thread, 2> workers;

    void stopWorkers() {
        stopping.store(true);
        condition.notify_all();
        for (auto& worker : workers) if (worker.joinable()) worker.join();
        std::lock_guard lock(mutex);
        started = false;
        queue.clear();
        for(auto& [key,entry]:entries)entry.animation.reset();
        for (auto& [key, entry] : entries) { entry.pending = false; entry.retryAt = {}; }
    }
    std::optional<std::filesystem::path> save(image_detail::Bytes const& bytes) {
        auto const hash = image_detail::sha256(bytes);
        if (hash.empty()) return std::nullopt;
        auto const path = directory / (hash + ".png");
        auto const existing = image_detail::readBounded(path, MaxDownloadBytes);
        if ((!existing || *existing != bytes) && !image_detail::atomicWrite(path, bytes)) return std::nullopt;
        {
            std::lock_guard lock(mutex);
            access[path] = Clock::now();
        }
        std::error_code ec;
        std::filesystem::last_write_time(path, std::filesystem::file_time_type::clock::now(), ec);
        return path;
    }
    DiskResult cached(std::string const& key) {
        auto const keyHash = image_detail::sha256(key);
        if (keyHash.empty()) return {};
        auto const recordPath = directory / (keyHash + ".ref");
        auto const record = image_detail::readBounded(recordPath, 64);
        if (!record) return {};
        std::string const hash{reinterpret_cast<char const*>(record->data()), record->size()};
        if (!image_detail::validHash(hash)) return {};
        auto const path = directory / (hash + ".png");
        auto const bytes = image_detail::readBounded(path, MaxDownloadBytes);
        if (!bytes || !image_detail::pngSignature(*bytes) || image_detail::sha256(*bytes) != hash) return {};
        std::error_code ec;
        auto const time = std::filesystem::last_write_time(recordPath, ec);
        auto const now = std::filesystem::file_time_type::clock::now();
        bool const fresh = !ec && time <= now && now - time < image_detail::RetryDelay;
        {
            std::lock_guard lock(mutex);
            access[path] = Clock::now();
            access[recordPath] = Clock::now();
        }
        std::filesystem::last_write_time(path, now, ec);
        return {path, fresh};
    }
    bool saveReference(std::string const& key, std::filesystem::path const& path) {
        auto const hash = image_detail::sha256(key);
        if (hash.empty()) return false;
        auto const value = path.stem().string();
        auto const record = directory / (hash + ".ref");
        bool const ok = image_detail::atomicWrite(record,
            std::span{reinterpret_cast<unsigned char const*>(value.data()), value.size()});
        if (ok) { std::lock_guard lock(mutex); access[record] = Clock::now(); }
        return ok;
    }
    void prune(std::filesystem::path const& protectedPath = {}) {
        struct File { std::filesystem::path path; std::uintmax_t bytes; std::filesystem::file_time_type time; Clock::time_point use; };
        std::vector<File> files;
        std::uintmax_t total{};
        std::error_code ec;
        std::filesystem::directory_iterator iterator(directory, ec), end;
        while (!ec && iterator != end) {
            auto const& item = *iterator;
            auto const extension = item.path().extension();
            if ((extension == ".png" || extension == ".ref" || extension == ".gif" || extension == ".gref") && image_detail::validHash(item.path().stem().string())
                && item.is_regular_file(ec)) {
                auto const size = item.file_size(ec);
                auto const time = item.last_write_time(ec);
                if (!ec) {
                    total += size;
                    Clock::time_point use{};
                    { std::lock_guard lock(mutex); auto const found = access.find(item.path()); if (found != access.end()) use = found->second; }
                    files.push_back({item.path(), size, time, use});
                }
            }
            iterator.increment(ec);
        }
        std::sort(files.begin(), files.end(), [](File const& a, File const& b) {
            if (a.use != b.use) return a.use < b.use;
            return a.time < b.time;
        });
        for (auto const& file : files) {
            if (total <= MaxCacheBytes) break;
            if (file.path == protectedPath || file.path == defaults[0] || file.path == defaults[1]) continue;
            {
                std::lock_guard lock(mutex);
                for (auto& [key, entry] : entries) if (entry.path == file.path) { entry.path.reset(); entry.retryAt = {}; }
                access.erase(file.path);
            }
            if (std::filesystem::remove(file.path, ec)) total -= file.bytes;
            ec.clear();
        }
    }
    void work() {
        image_detail::ComScope apartment;
        image_detail::ComPtr<IWICImagingFactory> factory;
        if (apartment.usable()) CoCreateInstance(CLSID_WICImagingFactory, nullptr,
            CLSCTX_INPROC_SERVER, IID_PPV_ARGS(factory.GetAddressOf()));
        while (!stopping.load()) {
            Job job;
            {
                std::unique_lock lock(mutex);
                condition.wait(lock, [&] { return stopping.load() || !queue.empty(); });
                if (stopping.load()) return;
                job = std::move(queue.front());
                queue.pop_front();
            }
            auto const deadline = Clock::now() + image_detail::RequestTimeout;
            std::optional<std::filesystem::path> result;
            bool success{};
            std::shared_ptr<GifAnimation const> animated;
            std::optional<image_detail::Bytes> originalGif;
            {
                std::lock_guard diskLock(diskMutex);
                auto ref=image_detail::readBounded(directory/(image_detail::sha256(job.key)+".gref"),64);
                if(ref){std::string hash(ref->begin(),ref->end());if(image_detail::validHash(hash)){
                    originalGif=image_detail::readBounded(directory/(hash+".gif"),MaxDownloadBytes);
                    if(originalGif&&image_detail::sha256(*originalGif)!=hash)originalGif.reset();
                }}
            }
            if(originalGif){std::string error;animated=decodeGif(factory.Get(),*originalGif,job.banner,deadline,error);}
            {
                std::lock_guard diskLock(diskMutex);
                auto const previous = cached(job.key);
                result = previous.path;
                success = previous.fresh && (!originalGif || bool(animated));
            }
            // Make a disk hit visible before a possibly slow network refresh.
            if (result) {
                std::lock_guard lock(mutex);
                auto found=entries.find(job.key);
                if(found!=entries.end()){found->second.path=result;if(animated&&visible)found->second.animation=animated;}
            }
            if (!success && !stopping.load()) {
                std::optional<image_detail::Bytes> bytes = job.reference.type == "url"
                    ? image_detail::download(job.reference.value, deadline, stopping)
                    : image_detail::decodeInline(job.reference.value);
                if (bytes && (job.reference.type == "url" || image_detail::pngSignature(*bytes))) {
                    if(isGif(*bytes)){
                        if(!originalGif||*originalGif!=*bytes||!animated){std::string error;auto candidate=decodeGif(factory.Get(),*bytes,job.banner,deadline,error);if(candidate)animated=std::move(candidate);}
                        std::lock_guard diskLock(diskMutex);
                        auto hash=image_detail::sha256(*bytes);
                        if(!hash.empty()&&image_detail::atomicWrite(directory/(hash+".gif"),*bytes)){
                            auto refHash=image_detail::sha256(job.key);
                            image_detail::atomicWrite(directory/(refHash+".gref"),std::span{reinterpret_cast<unsigned char const*>(hash.data()),hash.size()});
                        }
                    }else animated.reset();
                    auto normalized = image_detail::normalize(factory.Get(), *bytes, job.banner, deadline);
                    if (normalized && !stopping.load() && Clock::now() < deadline) {
                        std::lock_guard diskLock(diskMutex);
                        if (auto path = save(*normalized)) {
                            result = std::move(path);
                            success = saveReference(job.key, *result);
                            // A URL can change from GIF to PNG/JPEG. Invalidate the old
                            // animation mapping only after the new static image is cached.
                            if (success && !isGif(*bytes)) {
                                auto const keyHash = image_detail::sha256(job.key);
                                image_detail::atomicWrite(directory / (keyHash + ".gref"), std::span<unsigned char const>{});
                            }
                        }
                    }
                }
            }
            {
                std::lock_guard lock(mutex);
                auto const found = entries.find(job.key);
                if (found != entries.end()) {
                    if (result) found->second.path = result;
                    if (visible && (success || animated)) found->second.animation = std::move(animated);
                    found->second.pending = false;
                    found->second.retryAt = Clock::now() + image_detail::RetryDelay;
                }
            }
            {
                std::lock_guard diskLock(diskMutex);
                prune(result.value_or(std::filesystem::path{}));
            }
        }
    }
};

ImageProvider::ImageProvider() : mImpl(std::make_unique<Impl>()) {}
ImageProvider::~ImageProvider() { stop(); }
void ImageProvider::tick(bool visible){
    std::lock_guard lock(mImpl->mutex);mImpl->visible=visible;auto now=image_detail::Clock::now();
    for(auto& [key,e]:mImpl->entries)if(!visible||(!e.pending&&now-e.lastUse>std::chrono::seconds(3))){if(e.animation){e.animation.reset();e.retryAt={};}}
}
ImageProvider& ImageProvider::getInstance() { static ImageProvider provider; return provider; }

void ImageProvider::initialize(std::filesystem::path const& cacheDir) {
    std::lock_guard lifecycleLock(mImpl->lifecycle);
    mImpl->stopWorkers();
    {
        std::lock_guard lock(mImpl->mutex);
        mImpl->entries.clear();
        mImpl->access.clear();
        mImpl->defaults = {};
        mImpl->directory.clear();
    }
    if (cacheDir.empty()) return;
    std::error_code ec;
    auto const absolute = std::filesystem::absolute(cacheDir, ec).lexically_normal();
    if (ec) return;
    std::filesystem::create_directories(absolute, ec);
    if (ec) return;
    {
        std::lock_guard lock(mImpl->mutex);
        mImpl->directory = absolute;
    }
    image_detail::ComScope apartment;
    image_detail::ComPtr<IWICImagingFactory> factory;
    if (!apartment.usable() || FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr,
        CLSCTX_INPROC_SERVER, IID_PPV_ARGS(factory.GetAddressOf())))) return;
    std::lock_guard diskLock(mImpl->diskMutex);
    for (int i = 0; i < 2; ++i) {
        auto bytes = image_detail::normalize(factory.Get(), i ? builtin::banner() : builtin::logo(), i != 0,
            image_detail::Clock::now() + image_detail::RequestTimeout);
        if (bytes) if (auto path = mImpl->save(*bytes)) {
            std::lock_guard lock(mImpl->mutex);
            mImpl->defaults[static_cast<std::size_t>(i)] = std::move(*path);
        }
    }
    mImpl->prune();
}
void ImageProvider::start() {
    std::lock_guard lifecycleLock(mImpl->lifecycle);
    std::lock_guard lock(mImpl->mutex);
    if (mImpl->started || mImpl->directory.empty()) return;
    mImpl->stopping.store(false);
    mImpl->started = true;
    for (auto& worker : mImpl->workers) worker = std::thread([this] { mImpl->work(); });
    mImpl->condition.notify_all();
}
void ImageProvider::stop() {
    std::lock_guard lifecycleLock(mImpl->lifecycle);
    mImpl->stopWorkers();
}
void ImageProvider::request(ImageReference const& reference, bool banner) {
    if (!image_detail::validReference(reference) || image_detail::isBuiltin(reference)) return;
    auto const key = image_detail::referenceKey(reference, banner);
    std::lock_guard lock(mImpl->mutex);
    if (!mImpl->started || mImpl->stopping.load()) return;
    auto found = mImpl->entries.find(key);
    auto const now = image_detail::Clock::now();
    for(auto& [k,e]:mImpl->entries)if(!e.pending && now-e.lastUse>std::chrono::seconds(15))e.animation.reset();
    if (found == mImpl->entries.end()) {
        if (mImpl->entries.size() >= image_detail::MaxEntries) {
            auto victim = mImpl->entries.end();
            for (auto it = mImpl->entries.begin(); it != mImpl->entries.end(); ++it)
                if (!it->second.pending && (victim == mImpl->entries.end() || it->second.lastUse < victim->second.lastUse)) victim = it;
            if (victim == mImpl->entries.end()) return;
            mImpl->entries.erase(victim);
        }
        found = mImpl->entries.emplace(key, Impl::Entry{}).first;
    }
    found->second.lastUse = now;
    if (found->second.pending || now < found->second.retryAt) return;
    found->second.pending = true;
    mImpl->queue.push_back({reference, banner, key});
    mImpl->condition.notify_one();
}
std::optional<std::filesystem::path> ImageProvider::get(ImageReference const& reference, bool banner) const {
    if (!image_detail::validReference(reference)) return std::nullopt;
    std::lock_guard lock(mImpl->mutex);
    if (image_detail::isBuiltin(reference)) {
        auto const& path = mImpl->defaults[banner ? 1 : 0];
        return path.empty() ? std::nullopt : std::optional<std::filesystem::path>{path};
    }
    auto const found = mImpl->entries.find(image_detail::referenceKey(reference, banner));
    if (found == mImpl->entries.end() || !found->second.path) return std::nullopt;
    found->second.lastUse = image_detail::Clock::now();
    mImpl->access[*found->second.path] = found->second.lastUse;
    return found->second.path;
}
std::shared_ptr<GifAnimation const> ImageProvider::animation(ImageReference const& ref,bool banner) const {
    std::lock_guard lock(mImpl->mutex);
    auto found=mImpl->entries.find(image_detail::referenceKey(ref,banner));
    return found==mImpl->entries.end()?nullptr:found->second.animation;
}
std::filesystem::path ImageProvider::builtinPath(bool banner) const {
    std::lock_guard lock(mImpl->mutex);
    return mImpl->defaults[banner ? 1 : 0];
}

} // namespace sle::client
