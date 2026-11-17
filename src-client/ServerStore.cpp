#include "ServerStore.h"
#include "AddressParser.h"

#include <algorithm>
#include <chrono>
#include <fstream>
#include <random>
#include <system_error>

#include <nlohmann/json.hpp>

namespace sle::client {

namespace {

using nlohmann::json;

constexpr int kSchemaVersion = 1;

std::string newId() {
    static std::mt19937_64 rng{std::random_device{}()};
    auto              now = std::chrono::system_clock::now().time_since_epoch().count();
    return std::to_string(now) + "-" + std::to_string(rng() & 0xFFFFFFULL);
}

json toJson(SavedServer const& s) {
    json j;
    j["id"]           = s.id;
    j["name"]         = s.name;
    j["address"]      = s.address;
    j["port"]         = s.port;
    j["logoUrl"]      = s.logoUrl;
    j["logoId"]       = s.logoId;
    j["introText"]    = s.introText;
    j["introImageUrl"] = s.introImageUrl;
    j["lastJoinedAt"] = s.lastJoinedAt;
    j["statusName"]   = s.statusName;
    j["players"]      = s.players;
    j["maxPlayers"]   = s.maxPlayers;
    j["version"]      = s.version;
    j["protocol"]     = s.protocol;
    j["statusAt"]     = s.statusAt;
    return j;
}

SavedServer fromJson(json const& j) {
    SavedServer s;
    auto        str = [&j](char const* key, std::string& out) {
        if (auto it = j.find(key); it != j.end() && it->is_string()) {
            out = it->get<std::string>();
        }
    };
    auto num = [&j](char const* key, auto& out) {
        if (auto it = j.find(key); it != j.end() && (it->is_number_integer() || it->is_number())) {
            out = it->get<std::decay_t<decltype(out)>>();
        }
    };
    str("id", s.id);
    str("name", s.name);
    str("address", s.address);
    num("port", s.port);
    str("logoUrl", s.logoUrl);
    str("logoId", s.logoId);
    str("introText", s.introText);
    str("introImageUrl", s.introImageUrl);
    num("lastJoinedAt", s.lastJoinedAt);
    str("statusName", s.statusName);
    num("players", s.players);
    num("maxPlayers", s.maxPlayers);
    str("version", s.version);
    num("protocol", s.protocol);
    num("statusAt", s.statusAt);
    return s;
}

} // namespace

ServerStore& ServerStore::getInstance() {
    static ServerStore instance;
    return instance;
}

void ServerStore::setPath(std::filesystem::path path) {
    std::lock_guard lock(mMutex);
    mPath = std::move(path);
    mPath.make_preferred();
}

bool ServerStore::load() {
    std::lock_guard lock(mMutex);
    mServers.clear();
    if (mPath.empty()) {
        return false;
    }
    std::error_code ec;
    if (!std::filesystem::exists(mPath, ec)) {
        return true; // 空列表
    }
    std::ifstream ifs(mPath);
    if (!ifs) {
        return false;
    }
    json j;
    try {
        ifs >> j;
    } catch (...) {
        return false;
    }
    if (!j.is_object()) {
        return false;
    }
    if (auto it = j.find("servers"); it != j.end() && it->is_array()) {
        for (auto const& item : *it) {
            if (item.is_object()) {
                mServers.push_back(fromJson(item));
            }
        }
    }
    return true;
}

bool ServerStore::save() const {
    std::lock_guard lock(mMutex);
    if (mPath.empty()) {
        return false;
    }

    json j;
    j["schemaVersion"] = kSchemaVersion;
    j["servers"]       = json::array();
    for (auto const& s : mServers) {
        j["servers"].push_back(toJson(s));
    }

    std::error_code ec;
    if (auto parent = mPath.parent_path(); !parent.empty()) {
        std::filesystem::create_directories(parent, ec);
    }

    auto tmp = mPath;
    tmp += ".tmp";
    {
        std::ofstream ofs(tmp, std::ios::trunc);
        if (!ofs) {
            return false;
        }
        ofs << j.dump(2);
        if (!ofs) {
            return false;
        }
    }
    std::filesystem::rename(tmp, mPath, ec);
    if (ec) {
        std::filesystem::remove(mPath, ec);
        std::filesystem::rename(tmp, mPath, ec);
    }
    return !ec;
}

std::vector<SavedServer> ServerStore::list() const {
    std::lock_guard lock(mMutex);
    return mServers;
}

std::optional<SavedServer> ServerStore::get(std::string const& id) const {
    std::lock_guard lock(mMutex);
    for (auto const& s : mServers) {
        if (s.id == id) {
            return s;
        }
    }
    return std::nullopt;
}

std::string ServerStore::upsert(SavedServer server) {
    if(!validServerEndpoint(server.address,server.port))return {};
    std::lock_guard lock(mMutex);
    if (server.id.empty()) {
        server.id = newId();
        mServers.push_back(std::move(server));
        return mServers.back().id;
    }
    for (auto& s : mServers) {
        if (s.id == server.id) {
            s = std::move(server);
            return s.id;
        }
    }
    mServers.push_back(std::move(server));
    return mServers.back().id;
}

bool ServerStore::remove(std::string const& id) {
    std::lock_guard lock(mMutex);
    auto        it = std::remove_if(mServers.begin(), mServers.end(), [&id](SavedServer const& s) { return s.id == id; });
    if (it == mServers.end()) {
        return false;
    }
    mServers.erase(it, mServers.end());
    return true;
}

} // namespace sle::client
