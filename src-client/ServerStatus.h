#pragma once

#include "../src/common/DiscoveryVisual.h"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace sle::client {

enum class ServerStatusState { Idle, Queued, Loading, Ready, Failed };

struct ServerStatusTransportOptions {
    // Only the NetherNet metadata endpoint may opt into its legacy self-signed TLS policy.
    // Image downloads must not reuse this option. Certificate validation is strict by default.
    bool allowSelfSignedNetherNet{false};
    bool allowPlainHttpFallback{true};
    bool operator==(ServerStatusTransportOptions const&) const = default;
};

// NetherNet /v1/join snapshot. Health is independent from the last usable visual.
struct ServerStatusResult {
    bool         ok{false};
    std::string  name;
    std::string  version;
    std::string  level;
    int          protocol{0};
    int          players{-1};
    int          maxPlayers{-1};
    std::string  error;
    std::int64_t at{0};

    ServerStatusState              state{ServerStatusState::Idle};
    bool                           visualFieldPresent{false};
    std::optional<DiscoveryVisual> visual;
};

// Four managed workers own all network/disk work. get() only copies an in-memory snapshot.
class ServerStatus {
public:
    static ServerStatus& getInstance();
    ~ServerStatus();
    ServerStatus(ServerStatus const&)            = delete;
    ServerStatus& operator=(ServerStatus const&) = delete;

    // Full discovery.json path; configure only while stopped, before start().
    void setCachePath(std::filesystem::path const& file);
    void start(ServerStatusTransportOptions options = {});
    void stop();

    // force bypasses freshness, but coalesces a matching queued/in-flight request.
    // Requests while stopped are ignored. Cache identity is key/host/port, never serverId.
    void request(std::string const& key, std::string const& address, int port, bool force = false);
    void request(
        std::string const& key,
        std::string const& address,
        int port,
        bool force,
        ServerStatusTransportOptions options
    );
    // Use before removing/changing a configured endpoint. Late completions cannot recreate it.
    void invalidate(std::string const& key);
    [[nodiscard]] std::optional<ServerStatusResult> get(std::string const& key) const;

private:
    ServerStatus() = default;
    friend struct ServerStatusTestAccess;

    using Query = std::function<
        ServerStatusResult(std::string const&, int, ServerStatusTransportOptions, std::stop_token)>;
    struct Entry {
        ServerStatusResult result;
        std::string address;
        int port{0};
        ServerStatusTransportOptions options;
        std::uint64_t generation{0};
        bool pending{false};
        std::chrono::steady_clock::time_point nextRefresh{};
        std::shared_ptr<std::stop_source> cancellation;
    };
    struct Task {
        std::string key;
        std::string address;
        int port;
        ServerStatusTransportOptions options;
        std::uint64_t generation;
        std::shared_ptr<std::stop_source> cancellation;
    };

    void requestImpl(
        std::string const& key, std::string const& address, int port, bool force,
        std::optional<ServerStatusTransportOptions> options
    );
    void worker();
    void loadCache();
    void saveCache();

    std::mutex mLifecycleMutex;
    mutable std::mutex mMutex;
    std::mutex mCacheMutex;
    std::condition_variable mWork;
    std::unordered_map<std::string, Entry> mEntries;
    std::deque<Task> mQueue;
    std::vector<std::thread> mWorkers;
    std::filesystem::path mCachePath;
    ServerStatusTransportOptions mOptions;
    std::uint64_t mGeneration{0};
    bool mRunning{false};
    bool mCacheDirty{false};
    Query mQuery; // Private transport seam for deterministic offline tests.
};

} // namespace sle::client
