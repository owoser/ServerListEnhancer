#pragma once
#include "AddressParser.h"
#include "common/DiscoveryVisual.h"
#include <chrono>
#include <nlohmann/json.hpp>
#include <string>
#include <optional>
namespace sle::android {
inline constexpr std::size_t MaxJsonBytes = 64 * 1024;
inline constexpr std::size_t MaxImageBytes = 2 * 1024 * 1024;
struct Status {
    std::string title, version, message, error, endpoint;
    int players{-1}, capacity{-1};
    std::optional<DiscoveryVisual> visual;
    bool pending{};
    std::chrono::steady_clock::time_point refreshedAt{};
};
inline void bindEndpoint(Status& status,std::string endpoint) {
    if(status.endpoint!=endpoint){status=Status{};status.endpoint=std::move(endpoint);}
}
inline bool parseDiscovery(std::string_view body, Status& result) {
    if(body.size() > MaxJsonBytes) return false;
    const auto j=nlohmann::json::parse(body, nullptr, false);
    if(!j.is_object()) return false;
    auto text=[](const nlohmann::json& v, const char* k, std::size_t n) {
        auto i=v.find(k); return i!=v.end() && i->is_string() && i->get_ref<const std::string&>().size()<=n ? i->get<std::string>() : std::string{};
    };
    auto number=[](const nlohmann::json& v, const char* k) {
        auto i=v.find(k); return i!=v.end() && i->is_number_integer() && *i>=0 && *i<=1000000 ? i->get<int>() : -1;
    };
    result.title=text(j,"name",256); result.version=text(j,"version",64);
    result.message=text(j,"level",1024); result.players=number(j,"players"); result.capacity=number(j,"maxPlayers");
    auto ext=j.find("server_list_enhancer");
    if(ext!=j.end() && ext->is_object() && ext->value("schemaVersion",nlohmann::json{})==1) {
        auto enabled=ext->find("enabled");
        if(enabled!=ext->end() && enabled->is_boolean() && !enabled->get<bool>()) result.visual.reset();
        else {
            DiscoveryVisual v; v.introText=text(*ext,"introText",1024); v.revision=text(*ext,"revision",128);
            auto image=[&](const char* key) {
                ImageReference ref; auto i=ext->find(key);
                if(i==ext->end() || !i->is_object()) return ref;
                auto type=text(*i,"type",16); auto value=text(*i,"value",33000);
                if(type=="url" && value.size()<=512 && (value.starts_with("https://")||value.starts_with("http://"))) return ImageReference{type,value};
                if(type=="base64" && value.starts_with("data:image/png;base64,")) return ImageReference{type,value};
                return ref;
            };
            v.logo=image("logo"); v.introImage=image("introImage"); result.visual=std::move(v);
        }
    }
    result.error.clear(); return true;
}
}
