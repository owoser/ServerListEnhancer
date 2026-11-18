#include "HomeUi.h"

#include <android/log.h>
#include <nlohmann/json.hpp>
#include <pl/memory/Hook.hpp>

#include <atomic>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <limits>
#include <memory>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <utility>
#include <vector>

namespace sle::android {
namespace {
using Json = nlohmann::json;
constexpr const char* LogTag = "ServerListEnhancer";
constexpr std::string_view kOpenHomeUri = "https://server-list-enhancer.invalid/open";

class Jni {
    JavaVM* vm_{};
    bool attached_{};
    bool frame_{};
public:
    JNIEnv* env{};

    explicit Jni(JavaVM* vm) : vm_(vm) {
        if (!vm_) throw std::runtime_error("HomeUi Java VM unavailable");
        const auto status = vm_->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6);
        if (status == JNI_EDETACHED) {
            if (vm_->AttachCurrentThread(&env, nullptr) != JNI_OK)
                throw std::runtime_error("HomeUi cannot attach JNI thread");
            attached_ = true;
        } else if (status != JNI_OK || !env) {
            throw std::runtime_error("HomeUi cannot obtain JNI environment");
        }
        if (env->PushLocalFrame(64) < 0) {
            if (env->ExceptionCheck()) env->ExceptionClear();
            if (attached_) vm_->DetachCurrentThread();
            throw std::runtime_error("HomeUi cannot allocate JNI local frame");
        }
        frame_ = true;
    }

    ~Jni() {
        if (env && env->ExceptionCheck()) env->ExceptionClear();
        if (frame_) env->PopLocalFrame(nullptr);
        if (attached_) vm_->DetachCurrentThread();
    }
    Jni(const Jni&) = delete;
    Jni& operator=(const Jni&) = delete;

    void check(const char* operation) {
        if (!env->ExceptionCheck()) return;
        auto exception = env->ExceptionOccurred();
        env->ExceptionClear();
        std::string detail;
        if (exception) {
            auto type = env->GetObjectClass(exception);
            auto method = type ? env->GetMethodID(type, "toString", "()Ljava/lang/String;") : nullptr;
            if (env->ExceptionCheck()) env->ExceptionClear();
            auto text = method ? static_cast<jstring>(env->CallObjectMethod(exception, method)) : nullptr;
            if (env->ExceptionCheck()) {
                env->ExceptionClear();
                text = nullptr;
            }
            if (text) {
                const char* chars = env->GetStringUTFChars(text, nullptr);
                if (chars) {
                    detail = chars;
                    env->ReleaseStringUTFChars(text, chars);
                }
                if (env->ExceptionCheck()) env->ExceptionClear();
                env->DeleteLocalRef(text);
            }
            if (type) env->DeleteLocalRef(type);
            env->DeleteLocalRef(exception);
        }
        throw std::runtime_error(std::string(operation) + (detail.empty() ? " raised a Java exception" : ": " + detail));
    }

    jclass type(const char* name) {
        auto result = env->FindClass(name);
        check(name);
        if (!result) throw std::runtime_error(std::string("HomeUi class unavailable: ") + name);
        return result;
    }
    jmethodID method(jclass type, const char* name, const char* signature, bool isStatic = false) {
        auto result = isStatic ? env->GetStaticMethodID(type, name, signature)
                               : env->GetMethodID(type, name, signature);
        check(name);
        if (!result) throw std::runtime_error(std::string("HomeUi method unavailable: ") + name);
        return result;
    }
    jstring ascii(const char* text) {
        auto result = env->NewStringUTF(text);
        check("NewStringUTF");
        if (!result) throw std::runtime_error("HomeUi cannot allocate Java string");
        return result;
    }
    // Use standard UTF-8 for paths and IDs; NewStringUTF expects modified UTF-8.
    jstring string(std::string_view text) {
        if (text.size() > static_cast<std::size_t>(std::numeric_limits<jsize>::max()))
            throw std::runtime_error("HomeUi Java string is too large");
        auto bytes = env->NewByteArray(static_cast<jsize>(text.size()));
        check("NewByteArray(string)");
        if (!bytes) throw std::runtime_error("HomeUi cannot allocate string bytes");
        if (!text.empty()) env->SetByteArrayRegion(bytes, 0, static_cast<jsize>(text.size()),
                                                  reinterpret_cast<const jbyte*>(text.data()));
        check("SetByteArrayRegion(string)");
        auto cls = type("java/lang/String");
        auto result = static_cast<jstring>(env->NewObject(cls, method(cls, "<init>", "([BLjava/lang/String;)V"),
                                                        bytes, ascii("UTF-8")));
        check("String UTF-8 constructor");
        if (!result) throw std::runtime_error("HomeUi cannot construct Java string");
        env->DeleteLocalRef(bytes);
        env->DeleteLocalRef(cls);
        return result;
    }
    std::string string(jstring text) {
        if (!text) throw std::runtime_error("HomeUi queryState returned null");
        auto cls = type("java/lang/String");
        auto bytes = static_cast<jbyteArray>(env->CallObjectMethod(text,
            method(cls, "getBytes", "(Ljava/lang/String;)[B"), ascii("UTF-8")));
        check("String.getBytes(UTF-8)");
        if (!bytes) throw std::runtime_error("HomeUi queryState has no UTF-8 bytes");
        const auto size = env->GetArrayLength(bytes);
        check("GetArrayLength(state)");
        if (size > 65536) throw std::runtime_error("HomeUi queryState exceeds 64 KiB");
        std::string result(static_cast<std::size_t>(size), '\0');
        if (size) env->GetByteArrayRegion(bytes, 0, size, reinterpret_cast<jbyte*>(result.data()));
        check("GetByteArrayRegion(state)");
        return result;
    }
};

struct Bridge {
    JavaVM* vm{};
    jclass type{}; // Global ref retains its DexClassLoader as well.
    jmethodID install{}, query{}, frame{}, image{}, close{}, request{}, observeTouch{}, measure{};
    jmethodID join{}, cancelJoin{};
    std::function<void()> open;
    std::function<bool(float, float, int)> touch;
    std::function<bool()> back;
    std::function<void(int, std::string)> joinStatus;
    std::atomic<bool> installed{}, failed{}, closing{}, reported{};

    ~Bridge() {
        if (!type) return;
        try { Jni j(vm); j.env->DeleteGlobalRef(type); } catch (...) {}
    }
};
std::mutex bridgeMutex;
std::mutex lifecycleMutex;
std::shared_ptr<Bridge> current;

using CallVoidMethodVFn = void (JNICALL*)(JNIEnv*, jobject, jmethodID, va_list);
using CallVoidMethodAFn = void (JNICALL*)(JNIEnv*, jobject, jmethodID, const jvalue*);
using CallVoidMethodFn = void (JNICALL*)(JNIEnv*, jobject, jmethodID, ...);

std::mutex jniHookMutex;
bool jniHooksInstalled = false;
std::atomic<jmethodID> gLaunchUriMethod{};
std::atomic<jmethodID> gShareMethod{};
std::atomic<long long> gJniVoidCalls{};
CallVoidMethodVFn gOrigCallVoidMethodV = nullptr;
CallVoidMethodAFn gOrigCallVoidMethodA = nullptr;
CallVoidMethodFn gOrigCallVoidMethod = nullptr;

std::shared_ptr<Bridge> bridge() {
    std::lock_guard lock(bridgeMutex);
    return current;
}
bool usable(const std::shared_ptr<Bridge>& state) {
    return state && state->installed.load() && !state->failed.load() && !state->closing.load();
}

void reportFailure(const std::shared_ptr<Bridge>& state, const char* operation, const char* reason) noexcept {
    if (!state->reported.exchange(true))
        __android_log_print(ANDROID_LOG_ERROR, LogTag, "Home UI disabled (%s): %s", operation, reason);
}
void hideAndClose(const std::shared_ptr<Bridge>& state) noexcept {
    if (!state || !state->type) return;
    try {
        Jni j(state->vm);
        if (state->frame) {
            try {
                auto hidden = j.ascii("{\"mode\":0,\"session\":0,\"commands\":[]}");
                j.env->CallStaticVoidMethod(state->type, state->frame, hidden);
                j.check("HomeOverlay.frame(hide)");
            } catch (const std::exception& e) { reportFailure(state, "hide", e.what()); }
        }
        if (state->close) {
            try {
                j.env->CallStaticVoidMethod(state->type, state->close);
                j.check("HomeOverlay.close");
            } catch (const std::exception& e) { reportFailure(state, "close", e.what()); }
        }
    } catch (const std::exception& e) { reportFailure(state, "close JNI", e.what()); }
    catch (...) { reportFailure(state, "close JNI", "unknown C++ exception"); }
}
void fail(const std::shared_ptr<Bridge>& state, const char* operation, const char* reason) noexcept {
    if (!state) return;
    const bool first = !state->failed.exchange(true);
    reportFailure(state, operation, reason);
    if (first) hideAndClose(state);
}

bool interceptLaunchUri(JNIEnv* env, jstring uri) noexcept {
    if (!env || !uri || env->ExceptionCheck()) return false;
    try {
        if (env->GetStringLength(uri) != static_cast<jsize>(kOpenHomeUri.size())) return false;
        jchar chars[kOpenHomeUri.size()]{};
        env->GetStringRegion(uri, 0, static_cast<jsize>(kOpenHomeUri.size()), chars);
        if (env->ExceptionCheck()) {
            env->ExceptionClear();
            return false;
        }
        for (std::size_t i = 0; i < kOpenHomeUri.size(); ++i) {
            if (chars[i] != static_cast<jchar>(kOpenHomeUri[i])) return false;
        }
        __android_log_print(ANDROID_LOG_INFO, LogTag, "Received native home server button request");
        auto state = bridge();
        if (usable(state) && state->request) {
            env->CallStaticVoidMethod(state->type, state->request);
            if (env->ExceptionCheck()) {
                env->ExceptionClear();
                __android_log_print(ANDROID_LOG_ERROR, LogTag, "HomeOverlay.openFromHomeControl raised a Java exception");
            }
        }
        return true;
    } catch (...) {
        if (env->ExceptionCheck()) env->ExceptionClear();
        return false;
    }
}

void logLaunchUriCall() noexcept {
    const long long count = gJniVoidCalls.fetch_add(1, std::memory_order_relaxed) + 1;
    if (count <= 32 || count % 32 == 0) {
        __android_log_print(ANDROID_LOG_INFO, LogTag, "launchUri call #%lld", count);
    }
}

void JNICALL hookCallVoidMethodV(JNIEnv* env, jobject obj, jmethodID method, va_list args) {
    if (method != nullptr && method == gLaunchUriMethod.load(std::memory_order_acquire) && obj != nullptr) {
        logLaunchUriCall();
        va_list copy;
        va_copy(copy, args);
        jobject arg0 = va_arg(copy, jobject);
        va_end(copy);
        if (interceptLaunchUri(env, static_cast<jstring>(arg0))) return;
    }
    if (gOrigCallVoidMethodV) gOrigCallVoidMethodV(env, obj, method, args);
}

void JNICALL hookCallVoidMethodA(JNIEnv* env, jobject obj, jmethodID method, const jvalue* args) {
    if (method != nullptr && method == gLaunchUriMethod.load(std::memory_order_acquire) && obj != nullptr && args != nullptr) {
        logLaunchUriCall();
        if (interceptLaunchUri(env, static_cast<jstring>(args[0].l))) return;
    }
    if (gOrigCallVoidMethodA) gOrigCallVoidMethodA(env, obj, method, args);
}

void JNICALL hookCallVoidMethod(JNIEnv* env, jobject obj, jmethodID method, ...) {
    va_list args;
    va_start(args, method);
    if (method != nullptr && method == gLaunchUriMethod.load(std::memory_order_acquire) && obj != nullptr) {
        logLaunchUriCall();
        va_list copy;
        va_copy(copy, args);
        jobject arg0 = va_arg(copy, jobject);
        va_end(copy);
        if (interceptLaunchUri(env, static_cast<jstring>(arg0))) {
            va_end(args);
            return;
        }
    }
    if (gOrigCallVoidMethodV) gOrigCallVoidMethodV(env, obj, method, args);
    va_end(args);
}

bool installLaunchUriHooks(JNIEnv* env) noexcept {
    std::lock_guard lock(jniHookMutex);
    if (jniHooksInstalled) return true;
    if (!env || !env->functions || !env->functions->CallVoidMethodV || !env->functions->CallVoidMethodA) {
        return false;
    }
    if (pl::memory::hook(reinterpret_cast<pl::memory::FuncPtr>(env->functions->CallVoidMethodV),
                         reinterpret_cast<pl::memory::FuncPtr>(&hookCallVoidMethodV),
                         reinterpret_cast<pl::memory::FuncPtr*>(&gOrigCallVoidMethodV)) != 0 ||
        !gOrigCallVoidMethodV) {
        __android_log_print(ANDROID_LOG_ERROR, LogTag, "Failed to hook JNI CallVoidMethodV");
        return false;
    }
    if (pl::memory::hook(reinterpret_cast<pl::memory::FuncPtr>(env->functions->CallVoidMethodA),
                         reinterpret_cast<pl::memory::FuncPtr>(&hookCallVoidMethodA),
                         reinterpret_cast<pl::memory::FuncPtr*>(&gOrigCallVoidMethodA)) != 0 ||
        !gOrigCallVoidMethodA) {
        __android_log_print(ANDROID_LOG_ERROR, LogTag, "Failed to hook JNI CallVoidMethodA");
        return false;
    }
    if (env->functions->CallVoidMethod) {
        if (pl::memory::hook(reinterpret_cast<pl::memory::FuncPtr>(env->functions->CallVoidMethod),
                             reinterpret_cast<pl::memory::FuncPtr>(&hookCallVoidMethod),
                             reinterpret_cast<pl::memory::FuncPtr*>(&gOrigCallVoidMethod)) != 0) {
            __android_log_print(ANDROID_LOG_WARN, LogTag, "Optional hook on JNI CallVoidMethod skipped; CallVoidMethodV/A active");
        }
    }
    jniHooksInstalled = true;
    return true;
}

jboolean JNICALL nativeBindMainActivity(JNIEnv* env, jclass owner, jclass mainActivityClass) {
    auto state = bridge();
    if (!state || state->failed.load() || state->closing.load() || !env || !mainActivityClass ||
        !env->IsSameObject(owner, state->type)) {
        return JNI_FALSE;
    }
    try {
        jmethodID method = env->GetMethodID(mainActivityClass, "launchUri", "(Ljava/lang/String;)V");
        if (env->ExceptionCheck()) {
            env->ExceptionClear();
            return JNI_FALSE;
        }
        if (!method) return JNI_FALSE;
        gLaunchUriMethod.store(method, std::memory_order_release);
        jmethodID share = env->GetMethodID(mainActivityClass, "share", "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)V");
        if (env->ExceptionCheck()) {
            env->ExceptionClear();
            share = nullptr;
        }
        gShareMethod.store(share, std::memory_order_release);
        if (!installLaunchUriHooks(env)) {
            gLaunchUriMethod.store(nullptr, std::memory_order_release);
            return JNI_FALSE;
        }
        __android_log_print(ANDROID_LOG_INFO, LogTag, "Bound MainActivity.launchUri JNI bridge");
        return JNI_TRUE;
    } catch (...) {
        if (env->ExceptionCheck()) env->ExceptionClear();
        return JNI_FALSE;
    }
}

void JNICALL nativeOpen(JNIEnv* env, jclass owner) {
    auto state = bridge();
    if (!usable(state) || !env->IsSameObject(owner, state->type)) return;
    try { state->open(); }
    catch (const std::exception& e) { fail(state, "nativeOpen", e.what()); }
    catch (...) { fail(state, "nativeOpen", "C++ callback threw an unknown exception"); }
}
jboolean JNICALL nativeTouch(JNIEnv* env, jclass owner, jfloat x, jfloat y, jint action) {
    auto state = bridge();
    if (!usable(state) || !env->IsSameObject(owner, state->type) ||
        !std::isfinite(x) || !std::isfinite(y)) return JNI_FALSE;
    try { return state->touch(x, y, action) ? JNI_TRUE : JNI_FALSE; }
    catch (const std::exception& e) { fail(state, "nativeTouch", e.what()); }
    catch (...) { fail(state, "nativeTouch", "C++ callback threw an unknown exception"); }
    return JNI_FALSE;
}
jboolean JNICALL nativeBack(JNIEnv* env, jclass owner) {
    auto state = bridge();
    if (!usable(state) || !env->IsSameObject(owner, state->type)) return JNI_FALSE;
    try { return state->back() ? JNI_TRUE : JNI_FALSE; }
    catch (const std::exception& e) { fail(state, "nativeBack", e.what()); }
    catch (...) { fail(state, "nativeBack", "C++ callback threw an unknown exception"); }
    return JNI_FALSE;
}
// Java 侧把加入请求的结果回报给宿主：status>0 已发起深链，<=0 失败并带提示文案。
void JNICALL nativeJoinStatus(JNIEnv* env, jclass owner, jint status, jstring message) {
    auto state = bridge();
    if (!usable(state) || !env->IsSameObject(owner, state->type)) return;
    try {
        Jni j(state->vm);
        std::string text;
        if (message != nullptr) {
            // 复用 Jni::string(jstring)：内部走 String.getBytes(UTF-8) 并限制 64 KiB。
            try { text = j.string(message); } catch (...) { text.clear(); }
        }
        if (state->joinStatus) state->joinStatus(static_cast<int>(status), std::move(text));
    } catch (const std::exception& e) { fail(state, "nativeJoinStatus", e.what()); }
    catch (...) { fail(state, "nativeJoinStatus", "C++ callback threw an unknown exception"); }
}

void closeCurrent() {
    std::shared_ptr<Bridge> previous;
    {
        std::lock_guard lock(bridgeMutex);
        previous = std::exchange(current, {});
    }
    if (!previous) return;
    previous->closing.store(true);
    previous->installed.store(false);
    hideAndClose(previous);
}

std::string utf8Path(const std::filesystem::path& path) {
    const auto text = path.u8string();
    return {reinterpret_cast<const char*>(text.data()), text.size()};
}

std::filesystem::path cacheDex(Jni& j, const std::filesystem::path& source,
                               const std::filesystem::path& cacheRoot) {
    const auto size = std::filesystem::file_size(source);
    if (!size || size > 16 * 1024 * 1024) throw std::runtime_error("HomeUi ui.dex size is invalid");
    std::vector<char> contents(static_cast<std::size_t>(size));
    std::ifstream input(source, std::ios::binary);
    if (!input.read(contents.data(), static_cast<std::streamsize>(contents.size())))
        throw std::runtime_error("HomeUi cannot read packaged ui.dex");
    input.close();

    auto bytes = j.env->NewByteArray(static_cast<jsize>(contents.size()));
    j.check("NewByteArray(DEX)");
    if (!bytes) throw std::runtime_error("HomeUi cannot allocate DEX digest input");
    j.env->SetByteArrayRegion(bytes, 0, static_cast<jsize>(contents.size()),
                              reinterpret_cast<const jbyte*>(contents.data()));
    j.check("SetByteArrayRegion(DEX)");
    auto digestClass = j.type("java/security/MessageDigest");
    auto digest = j.env->CallStaticObjectMethod(digestClass,
        j.method(digestClass, "getInstance", "(Ljava/lang/String;)Ljava/security/MessageDigest;", true),
        j.ascii("SHA-256"));
    j.check("MessageDigest.getInstance(SHA-256)");
    if (!digest) throw std::runtime_error("HomeUi SHA-256 digest unavailable");
    auto digestBytes = static_cast<jbyteArray>(j.env->CallObjectMethod(digest,
        j.method(digestClass, "digest", "([B)[B"), bytes));
    j.check("MessageDigest.digest(DEX)");
    if (!digestBytes || j.env->GetArrayLength(digestBytes) != 32)
        throw std::runtime_error("HomeUi SHA-256 digest has invalid size");
    unsigned char hash[32];
    j.env->GetByteArrayRegion(digestBytes, 0, 32, reinterpret_cast<jbyte*>(hash));
    j.check("GetByteArrayRegion(DEX digest)");
    std::ostringstream name;
    name << "sle-home-" << std::hex << std::setfill('0');
    for (auto byte : hash) name << std::setw(2) << static_cast<unsigned>(byte);
    name << ".dex";
    const auto target = cacheRoot / name.str();
    if (std::filesystem::exists(target)) {
        if (std::filesystem::is_symlink(target) || !std::filesystem::is_regular_file(target) ||
            std::filesystem::file_size(target) != size)
            throw std::runtime_error("HomeUi cached DEX identity mismatch");
        std::vector<char> existing(contents.size());
        std::ifstream cached(target, std::ios::binary);
        if (!cached.read(existing.data(), static_cast<std::streamsize>(existing.size())) || existing != contents)
            throw std::runtime_error("HomeUi cached DEX contents mismatch");
    } else {
        std::ofstream output(target, std::ios::binary | std::ios::trunc);
        output.write(contents.data(), static_cast<std::streamsize>(contents.size()));
        output.close();
        if (!output) throw std::runtime_error("HomeUi cannot write private cached DEX");
    }
    // Keep the installer's source writable; only our private immutable copy is loaded.
    std::error_code error;
    std::filesystem::permissions(target,
        std::filesystem::perms::owner_write | std::filesystem::perms::group_write |
        std::filesystem::perms::others_write, std::filesystem::perm_options::remove, error);
    if (error) throw std::runtime_error("HomeUi cannot mark cached DEX read-only: " + error.message());
    return target;
}
} // namespace

bool initializeHomeUi(JavaVM* vm, std::filesystem::path modRoot,
                      std::function<void()> open,
                      std::function<bool(float, float, int)> touch,
                      std::function<bool()> back,
                      std::function<void(int, std::string)> joinStatus) {
    std::lock_guard lifecycleLock(lifecycleMutex);
    closeCurrent();
    auto state = std::make_shared<Bridge>();
    state->vm = vm;
    state->open = std::move(open);
    state->touch = std::move(touch);
    state->back = std::move(back);
    state->joinStatus = std::move(joinStatus);
    try {
        if (!state->open || !state->touch || !state->back || !state->joinStatus)
            throw std::runtime_error("HomeUi requires open, touch, back, and join status callbacks");
        modRoot = std::filesystem::absolute(modRoot);
        const auto dexPath = modRoot / "ui.dex";
        std::error_code error;
        if (!std::filesystem::is_regular_file(dexPath, error) || error)
            throw std::runtime_error("HomeUi ui.dex is missing or not a regular file");

        Jni j(vm);
        auto activityThread = j.type("android/app/ActivityThread");
        auto app = j.env->CallStaticObjectMethod(activityThread,
            j.method(activityThread, "currentApplication", "()Landroid/app/Application;", true));
        j.check("ActivityThread.currentApplication");
        if (!app) throw std::runtime_error("HomeUi application is not ready");
        auto appClass = j.env->GetObjectClass(app);
        j.check("GetObjectClass(application)");
        auto parent = j.env->CallObjectMethod(app,
            j.method(appClass, "getClassLoader", "()Ljava/lang/ClassLoader;"));
        j.check("Application.getClassLoader");
        if (!parent) throw std::runtime_error("HomeUi application ClassLoader unavailable");
        auto cache = j.env->CallObjectMethod(app,
            j.method(appClass, "getCodeCacheDir", "()Ljava/io/File;"));
        j.check("Application.getCodeCacheDir");
        if (!cache) throw std::runtime_error("HomeUi application code cache unavailable");
        auto fileClass = j.type("java/io/File");
        auto cachePath = static_cast<jstring>(j.env->CallObjectMethod(cache,
            j.method(fileClass, "getAbsolutePath", "()Ljava/lang/String;")));
        j.check("File.getAbsolutePath");
        if (!cachePath) throw std::runtime_error("HomeUi code cache path unavailable");
        const auto immutableDex = cacheDex(j, dexPath, std::filesystem::path(j.string(cachePath)));
        auto loaderClass = j.type("dalvik/system/DexClassLoader");
        auto loader = j.env->NewObject(loaderClass,
            j.method(loaderClass, "<init>", "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/ClassLoader;)V"),
            j.string(utf8Path(immutableDex)), cachePath, nullptr, parent);
        j.check("DexClassLoader constructor");
        if (!loader) throw std::runtime_error("HomeUi DexClassLoader unavailable");
        auto loaded = j.env->CallObjectMethod(loader,
            j.method(loaderClass, "loadClass", "(Ljava/lang/String;)Ljava/lang/Class;"),
            j.ascii("cn.owoser.sle.HomeOverlay"));
        j.check("DexClassLoader.loadClass(HomeOverlay)");
        if (!loaded) throw std::runtime_error("HomeUi HomeOverlay class unavailable");
        state->type = static_cast<jclass>(j.env->NewGlobalRef(loaded));
        j.check("NewGlobalRef(HomeOverlay)");
        if (!state->type) throw std::runtime_error("HomeUi cannot retain HomeOverlay class");
        state->install = j.method(state->type, "install", "(Ljava/lang/String;)V", true);
        state->query = j.method(state->type, "queryState", "()Ljava/lang/String;", true);
        state->frame = j.method(state->type, "frame", "(Ljava/lang/String;)V", true);
        state->image = j.method(state->type, "image", "(Ljava/lang/String;[BII)V", true);
        state->close = j.method(state->type, "close", "()V", true);
        state->request = j.method(state->type, "openFromHomeControl", "()V", true);
        state->observeTouch = j.method(state->type, "observeHomeTouch", "(FFII)V", true);
        state->join = j.method(state->type, "joinServer", "(Ljava/lang/String;I)V", true);
        state->cancelJoin = j.method(state->type, "cancelJoin", "()V", true);
        // 可选能力：旧版 DEX 没有 measureText，解析失败时保持为空并退回 C++ 估算，不影响安装。
        state->measure = j.env->GetStaticMethodID(state->type, "measureText", "(Ljava/lang/String;F)F");
        if (j.env->ExceptionCheck()) {
            j.env->ExceptionClear();
            state->measure = nullptr;
        }
        const JNINativeMethod natives[] = {
            {const_cast<char*>("nativeBindMainActivity"), const_cast<char*>("(Ljava/lang/Class;)Z"), reinterpret_cast<void*>(nativeBindMainActivity)},
            {const_cast<char*>("nativeOpen"), const_cast<char*>("()V"), reinterpret_cast<void*>(nativeOpen)},
            {const_cast<char*>("nativeTouch"), const_cast<char*>("(FFI)Z"), reinterpret_cast<void*>(nativeTouch)},
            {const_cast<char*>("nativeBack"), const_cast<char*>("()Z"), reinterpret_cast<void*>(nativeBack)},
            {const_cast<char*>("nativeJoinStatus"), const_cast<char*>("(ILjava/lang/String;)V"), reinterpret_cast<void*>(nativeJoinStatus)},
        };
        const auto registration = j.env->RegisterNatives(state->type, natives, 5);
        j.check("RegisterNatives(HomeOverlay)");
        if (registration != JNI_OK) throw std::runtime_error("HomeUi native callback registration failed");
        {
            std::lock_guard lock(bridgeMutex);
            current = state;
        }
        j.env->CallStaticVoidMethod(state->type, state->install, j.string(utf8Path(modRoot)));
        j.check("HomeOverlay.install");
        state->installed.store(true);
        return true;
    } catch (const std::exception& e) {
        fail(state, "initialize", e.what());
    } catch (...) {
        fail(state, "initialize", "unknown C++ exception");
    }
    {
        std::lock_guard lock(bridgeMutex);
        if (current == state) current.reset();
    }
    return false;
}

HomeUiState homeUiState() {
    auto state = bridge();
    if (!usable(state)) return {};
    try {
        Jni j(state->vm);
        auto text = static_cast<jstring>(j.env->CallStaticObjectMethod(state->type, state->query));
        j.check("HomeOverlay.queryState");
        const auto data = Json::parse(j.string(text));
        HomeUiState result;
        result.ready = data.at("ready").get<bool>();
        result.home = data.at("home").get<bool>();
        result.blocked = data.at("blocked").get<bool>();
        result.session = data.value("session", std::uint64_t{0});
        result.screen = data.at("screen").get<std::string>();
        result.width = data.at("width").get<float>();
        result.height = data.at("height").get<float>();
        if (!std::isfinite(result.width) || !std::isfinite(result.height) ||
            (result.ready && (result.width <= 0 || result.height <= 0)))
            throw std::runtime_error("HomeUi snapshot has invalid surface dimensions");
        if (!usable(state)) return {};
        result.blocked = result.blocked || !result.ready;
        result.home = result.home && result.ready && result.screen == "native_home_button" && result.session != 0;
        return result;
    } catch (const std::exception& e) {
        fail(state, "queryState", e.what());
    } catch (...) {
        fail(state, "queryState", "unknown C++ exception");
    }
    return {};
}

void submitHomeUiFrame(int mode, std::span<const pl::modmenu::DrawCommand> commands, std::uint64_t session) {
    auto state = bridge();
    if (!usable(state)) return;
    try {
        if (mode != 0 && mode != 2) throw std::runtime_error("HomeUi frame mode must be 0 or 2");
        Json data = {{"mode", mode}, {"session", session}, {"commands", Json::array()}};
        if (mode == 2) {
            for (const auto& command : commands) {
                if (!std::isfinite(command.x) || !std::isfinite(command.y) || !std::isfinite(command.w) ||
                    !std::isfinite(command.h) || !std::isfinite(command.x3) || !std::isfinite(command.y3) ||
                    !std::isfinite(command.size))
                    throw std::runtime_error("HomeUi draw command has nonfinite coordinates");
                data["commands"].push_back({{"type", static_cast<int>(command.type)},
                    {"x", command.x}, {"y", command.y}, {"w", command.w}, {"h", command.h},
                    {"x3", command.x3}, {"y3", command.y3}, {"color", command.color},
                    {"size", command.size}, {"text", command.text}, {"imageId", command.imageId}});
            }
        }
        // ASCII JSON preserves supplementary Unicode through standard JSON escapes.
        const auto encoded = data.dump(-1, ' ', true);
        Jni j(state->vm);
        j.env->CallStaticVoidMethod(state->type, state->frame, j.ascii(encoded.c_str()));
        j.check("HomeOverlay.frame");
    } catch (const std::exception& e) {
        fail(state, "frame", e.what());
    } catch (...) {
        fail(state, "frame", "unknown C++ exception");
    }
}

bool registerHomeUiImage(std::string_view id, std::span<const unsigned char> rgba, int width, int height) {
    auto state = bridge();
    if (!usable(state) || id.empty() || width <= 0 || height <= 0 || width > 4096 || height > 4096 ||
        rgba.size() != static_cast<std::uint64_t>(width) * height * 4 ||
        rgba.size() > static_cast<std::size_t>(std::numeric_limits<jsize>::max())) return false;
    try {
        Jni j(state->vm);
        auto bytes = j.env->NewByteArray(static_cast<jsize>(rgba.size()));
        j.check("NewByteArray(image)");
        if (!bytes) throw std::runtime_error("HomeUi cannot allocate image bytes");
        j.env->SetByteArrayRegion(bytes, 0, static_cast<jsize>(rgba.size()),
                                 reinterpret_cast<const jbyte*>(rgba.data()));
        j.check("SetByteArrayRegion(image)");
        j.env->CallStaticVoidMethod(state->type, state->image, j.string(id), bytes,
                                   static_cast<jint>(width), static_cast<jint>(height));
        j.check("HomeOverlay.image");
        return true;
    } catch (const std::exception& e) {
        fail(state, "image", e.what());
    } catch (...) {
        fail(state, "image", "unknown C++ exception");
    }
    return false;
}

// 绘制字体的真实文字宽度（设计单位）。任何 JNI 失败都返回负数表示不可用，调用方回退到估算。
float measureHomeText(std::string_view text, float size) {
    if (!std::isfinite(size) || size <= 0.f) return -1.f;
    if (text.empty()) return 0.f;
    auto state = bridge();
    if (!usable(state) || !state->measure) return -1.f;
    try {
        Jni j(state->vm);
        auto payload = j.string(text);
        const auto width = static_cast<float>(j.env->CallStaticFloatMethod(
            state->type, state->measure, payload, static_cast<jfloat>(size)));
        if (j.env->ExceptionCheck()) {
            j.env->ExceptionClear();
            return -1.f;
        }
        return std::isfinite(width) && width >= 0.f ? width : -1.f;
    } catch (...) {
        return -1.f;
    }
}

// 发起加入：域名解析与 Intent 组装都在伴生 DEX 里完成，这里只投递参数。
bool requestServerJoin(std::string address, int port) {
    auto state = bridge();
    if (!usable(state) || !state->join || address.empty() || port <= 0 || port > 65535) return false;
    try {
        Jni j(state->vm);
        j.env->CallStaticVoidMethod(state->type, state->join, j.string(address), static_cast<jint>(port));
        j.check("HomeOverlay.joinServer");
        return !j.env->ExceptionCheck();
    } catch (...) {
        return false;
    }
}

void observeHomeUiTouch(float x, float y, int action, int pointerId) {
    auto state = bridge();
    if (!usable(state) || !state->observeTouch) return;
    try {
        Jni j(state->vm);
        j.env->CallStaticVoidMethod(state->type, state->observeTouch, x, y, action, pointerId);
        if (j.env->ExceptionCheck()) j.env->ExceptionClear();
    } catch (...) {}
}

// 取消未落地的加入：列表关闭、模块禁用后旧的 DNS 结果不会再触发 startActivity。
void cancelServerJoin() {
    auto state = bridge();
    if (!usable(state) || !state->cancelJoin) return;
    try {
        Jni j(state->vm);
        j.env->CallStaticVoidMethod(state->type, state->cancelJoin);
        if (j.env->ExceptionCheck()) j.env->ExceptionClear();
    } catch (...) {}
}

void closeHomeUi() {
    std::lock_guard lifecycleLock(lifecycleMutex);
    closeCurrent();
}

} // namespace sle::android
