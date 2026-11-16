#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include "JoinInfoExtension.h"
#include "JoinInfoPayload.h"
#include "ServerConfig.h"
#include "ll/api/io/Logger.h"
#include "ll/api/memory/Hook.h"
#include "ll/api/mod/NativeMod.h"
#include "mc/deps/nether_net/signaling/http/HttpRequest.h"
#include "mc/deps/nether_net/signaling/http/HttpResponse.h"
#include "mc/deps/nether_net/signaling/http/HttpSignalingServer.h"
#include "mc/deps/core/threading/IAsyncResult.h"
#include <atomic>
#include <functional>
#include <memory>
#include <utility>

// SDK declares this non-exported diagnostic default. Our local derived adapter
// emits the base vtable during construction; provide a harmless local default.
// Runtime calls on VisualResponse dispatch to its forwarding override below.
Bedrock::Threading::AsyncBase::DiagnosticInfo
Bedrock::Threading::AsyncBase::getAsyncDiagnosticInfo() const { return {}; }

namespace sle::server {
namespace {
struct ExtensionState {
    join_info::SnapshotStore snapshots;
    std::atomic<bool> enabled{false};
    bool installed{};
    bool pinned{};
};
ExtensionState& state() { static auto* value=new ExtensionState; return *value; }
auto& logger(){return ll::mod::NativeMod::current()->getLogger();}
using Response = Bedrock::Result<NetherNet::HttpResponse>;
using AsyncResult = Bedrock::Threading::IAsyncResult<Response>;

// Do not read HttpSignalingServer members. Normal 26.60.7 generated member
// offsets differ from this game build (mGetServerInfo appeared empty in a real
// request). Transform the exported method's typed async response instead.
class VisualResponse final : public AsyncResult {
    std::shared_ptr<AsyncResult> mOriginal;
public:
    explicit VisualResponse(std::shared_ptr<AsyncResult> original):mOriginal(std::move(original)){}
    Bedrock::Threading::AsyncBase::DiagnosticInfo getAsyncDiagnosticInfo() const override {
        return mOriginal->getAsyncDiagnosticInfo();
    }
    Bedrock::Threading::AsyncStatus getStatus() const override {return mOriginal->getStatus();}
    std::error_code getError() const override {return mOriginal->getError();}
    void cancel() override {mOriginal->cancel();}
    Response getResult() const override {
        auto response=mOriginal->getResult();
        if(response && response->statusCode==200)
            response->body=state().snapshots.extend(response->body.get());
        return response;
    }
    void addOnComplete(CompletionHandler callback) override {
        auto weak=this->weak_from_this();
        mOriginal->addOnComplete([weak,callback=std::move(callback)](AsyncResult const&) {
            if(auto self=weak.lock()) callback(*std::static_pointer_cast<VisualResponse>(self));
        });
    }
};
LL_TYPE_INSTANCE_HOOK(
    JoinInfoOnRequestHook, ll::memory::HookPriority::Normal,
    NetherNet::HttpSignalingServer, &NetherNet::HttpSignalingServer::$onRequest,
    Bedrock::Threading::Async<Response>, NetherNet::HttpRequest request
) {
    bool const visual=request.method.get()=="GET" && request.path.get()=="/v1/join";
    auto result=origin(std::move(request));
    if(visual && result.mResult) result.mResult=std::make_shared<VisualResponse>(std::move(result.mResult));
    return result;
}
}
bool installJoinInfoExtension(){
    auto& s=state();
    if(s.installed)return true;
    HMODULE handle{};
    if(!s.pinned && !GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_PIN,
        reinterpret_cast<LPCWSTR>(&installJoinInfoExtension),&handle)) return false;
    s.pinned=true;
    auto const status=JoinInfoOnRequestHook::hook();
    if(status!=0){logger().error("[join-info] hook failed: {}",status);return false;}
    s.installed=true;
    logger().warn("[join-info] Native response extension installed; restart server to update/unload this DLL.");
    return true;
}
void refreshJoinInfoSnapshot(){
    auto payload=join_info::buildPayload(ConfigManager::getInstance().get(),state().enabled.load());
    auto revision=payload["revision"].get<std::string>();
    state().snapshots.publish(std::move(payload));
    logger().info("[join-info] Snapshot published: {}",revision);
}
void setJoinInfoEnabled(bool enabled){state().enabled.store(enabled);refreshJoinInfoSnapshot();}
bool isJoinInfoEnabled(){return state().enabled.load();}
void uninstallJoinInfoExtension(){setJoinInfoEnabled(false);}
}
