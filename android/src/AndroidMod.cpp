#include "AndroidPlatform.h"
#include "Config.h"
#include "Discovery.h"
#include "GifAnimation.h"
#include "HomeUi.h"
#include "LatencyProbe.h"
#include "ServerStore.h"
#include "TextEdit.h"
#include "TouchInteraction.h"
#include "common/BuiltinImages.h"
#include "common/Endpoint.h"
#include <pl/Mod.hpp>
#include <pl/ModMenu.hpp>
#include <pl/Input.hpp>
#include <android/log.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <dlfcn.h>
#include <fstream>
#include <iomanip>
#include <map>
#include <mutex>
#include <optional>
#include <sstream>
#include <thread>

namespace sle::android {
namespace {
using namespace std::chrono_literals;
using sle::client::SavedServer;
using sle::client::ServerStore;
constexpr const char* Module="server_list_enhancer.list";
std::mutex cacheWriteMutex;
std::string cacheName(std::string_view s) {
    std::uint64_t hash=14695981039346656037ULL;
    for(unsigned char c:s){hash^=c;hash*=1099511628211ULL;}
    std::ostringstream out;out<<std::hex<<hash;return out.str();
}
std::vector<unsigned char> readBytes(const std::filesystem::path& path,std::size_t max) {
    std::error_code ec;auto n=std::filesystem::file_size(path,ec);if(ec||n>max)return {};
    std::ifstream file(path,std::ios::binary);std::vector<unsigned char> data(n);
    if(!file.read(reinterpret_cast<char*>(data.data()),n))return {};return data;
}
void saveBytes(const std::filesystem::path& path,std::span<const unsigned char> data) {
    std::lock_guard lock(cacheWriteMutex);
    auto temp=path;temp+=".tmp";
    {std::ofstream out(temp,std::ios::binary|std::ios::trunc);out.write(reinterpret_cast<const char*>(data.data()),data.size());if(!out)return;}
    std::error_code ec;std::filesystem::rename(temp,path,ec);
}
std::string fit(std::string value,std::size_t count) {
    std::size_t pos{};while(count--&&pos<value.size())pos=client::TextEdit::next(value,pos);
    if(pos<value.size())value=value.substr(0,pos)+"…";return value;
}
// 估算一段文本的像素宽度：ASCII 按 0.56 倍字号，其余（中文等）按 1 倍字号累加。
// Home 覆盖层的文本绘制只支持左对齐，右侧信息列靠这个宽度反推起点。
float textAdvance(std::string_view text,float size) {
    float width{};
    for(std::size_t i=0;i<text.size();) {
        auto c=static_cast<unsigned char>(text[i]);
        std::size_t len=c<0x80?1:(c>>5)==6?2:(c>>4)==14?3:(c>>3)==30?4:1;
        width+=len==1?size*.56f:size;
        i+=len;
    }
    return width;
}
// 文字宽度：优先用 Home 覆盖层绘制字体的实测宽度（measureText），失败时回退到上面的估算。
// 光标定位、按钮居中、右侧信息列都依赖这个值，实测可避免长文本误差累积成明显空隙。
float textWidth(std::string_view text,float size) {
    if(text.empty())return 0.f;
    static std::mutex cacheMutex;
    static std::map<std::string,float> cache;
    std::string key=std::to_string(static_cast<long long>(size*100.f+.5f));key.push_back('\x1f');key.append(text);
    {
        std::lock_guard lock(cacheMutex);
        auto it=cache.find(key);
        if(it!=cache.end())return it->second;
        if(cache.size()>=512)cache.clear();
    }
    const float measured=measureHomeText(text,size);
    if(measured<0.f)return textAdvance(text,size);
    {
        std::lock_guard lock(cacheMutex);
        if(cache.size()<512)cache.emplace(std::move(key),measured);
    }
    return measured;
}
// 行图标在真机上只占 48 个设计单位，统一按长边 ≤64 px 保存，避免大图挤爆帧预算。
constexpr int RowImageSide=64;
std::pair<int,int> rowImageSize(int width,int height) {
    if(width<=0||height<=0)return {0,0};
    if(width<=RowImageSide&&height<=RowImageSide)return {width,height};
    const float ratio=static_cast<float>(RowImageSide)/static_cast<float>(std::max(width,height));
    return {std::max(1,static_cast<int>(width*ratio+.5f)),std::max(1,static_cast<int>(height*ratio+.5f))};
}
// 最近邻降采样：输入 RGBA，输出按行紧密排列（每行 dw*4 字节，无填充）。
std::vector<unsigned char> scaleRgba(std::span<const unsigned char> source,int sw,int sh,int dw,int dh) {
    std::vector<unsigned char> out;
    if(sw<=0||sh<=0||dw<=0||dh<=0)return out;
    if(source.size()!=std::size_t(sw)*std::size_t(sh)*4)return out;
    out.resize(std::size_t(dw)*std::size_t(dh)*4);
    for(int y=0;y<dh;++y) {
        const int sy=std::min(sh-1,static_cast<int>(static_cast<std::int64_t>(y)*sh/dh));
        const unsigned char* srcRow=source.data()+std::size_t(sy)*std::size_t(sw)*4;
        unsigned char* dstRow=out.data()+std::size_t(y)*std::size_t(dw)*4;
        for(int x=0;x<dw;++x) {
            const int sx=std::min(sw-1,static_cast<int>(static_cast<std::int64_t>(x)*sw/dw));
            const unsigned char* src=srcRow+std::size_t(sx)*4;
            dstRow[std::size_t(x)*4+0]=src[0];dstRow[std::size_t(x)*4+1]=src[1];
            dstRow[std::size_t(x)*4+2]=src[2];dstRow[std::size_t(x)*4+3]=src[3];
        }
    }
    return out;
}
std::string codepoint(unsigned n) {
    std::string s;if(!n||n>0x10ffff||(n>=0xd800&&n<=0xdfff))return s;
    if(n<128)s+=static_cast<char>(n);
    else if(n<2048){s+=static_cast<char>(0xc0|(n>>6));s+=static_cast<char>(0x80|(n&63));}
    else if(n<65536){s+=static_cast<char>(0xe0|(n>>12));s+=static_cast<char>(0x80|((n>>6)&63));s+=static_cast<char>(0x80|(n&63));}
    else{s+=static_cast<char>(0xf0|(n>>18));s+=static_cast<char>(0x80|((n>>12)&63));s+=static_cast<char>(0x80|((n>>6)&63));s+=static_cast<char>(0x80|(n&63));}return s;
}
}
class AndroidMod {
    using Hit = TouchTarget;
    struct Task {SavedServer server;std::uint64_t generation;bool imagesOnly{false};};
    std::mutex mutex;
    std::condition_variable changed;
    std::deque<Task> tasks;
    std::map<std::string,std::uint64_t> generations;
    std::map<std::string,Status> statuses;
    std::vector<std::thread> workers;
    std::thread renderer;
    std::atomic<bool> active{};
    // 加入请求：joining 表示已有请求在途；结果经 joinMutex 交给渲染线程显示，
    // 避免 Java 同步回报时在 act() 已持有 mutex 的同一线程上再次加锁。
    std::atomic<bool> joining{};
    std::mutex joinMutex;
    std::string joinNotice;
    bool joinNoticePending{};
    bool inputRegistered{},touchRegistered{},moduleEnabled{true},open{},form{},confirm{},plainHttp{true},autoRefresh{true},autoRefreshPending{};
    bool logoReady{},bannerReady{};
    int logoWidth{64},logoHeight{64},bannerWidth{960},bannerHeight{320};
    float scale{1},viewWidth{800},viewHeight{480};
    int page{},focus{};
    TouchPress press;
    std::mutex renderMutex;
    std::condition_variable renderChanged;
    bool repaintRequested{};
    std::vector<Hit> hits;
    std::string selected,editId,name,address,notice;
    client::TextEdit nameEdit,addressEdit;
    std::filesystem::path cache,modRoot;
    JavaVM* javaVm{};
    struct Animation {
        std::vector<std::string> ids;
        std::vector<int> delays;
        int width{},height{},plays{};
        std::int64_t started{};
        bool ready{};
    };
    Animation logoAnimation,bannerAnimation;
    // 列表行图标：每个服务器一张静态首帧图，超过 32 张或 32 MiB 后按 LRU 回收。
    struct RowImage {int width{},height{};std::int64_t used{};};
    static constexpr std::size_t RowImageCount=32,RowImageBudget=32u*1024u*1024u;
    std::map<std::string,RowImage> rowImages;
    std::size_t rowImageBytes{};
    // 行内 GIF：每行各自循环播放自己的帧序列，最多同时 8 行动画，帧图与静态图标共用 32 MiB 预算。
    struct RowAnimation {
        std::vector<std::string> ids;
        std::vector<int> delays;
        int width{},height{},plays{};
        std::int64_t started{},used{};
        std::size_t bytes{};
    };
    static constexpr std::size_t RowAnimationCount=8,RowAnimationBytes=4u*1024u*1024u;
    std::map<std::string,RowAnimation> rowAnimations;
    int udpProbePort{};
    LatencyProbe latencyProbe;

    static std::int64_t steadyMillis() {
        return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    }
    // 动态 GIF：按已播放时间返回当前应提交的帧图片 id；没有动画时返回空串。
    std::string animationId(const Animation& state) const {
        if(!state.ready||state.ids.empty())return {};
        if(state.ids.size()==1||state.delays.size()!=state.ids.size())return state.ids.front();
        std::int64_t total{};for(int delay:state.delays)total+=delay;if(total<=0)return state.ids.front();
        const auto elapsed=steadyMillis()-state.started;const std::int64_t time=elapsed>0?elapsed:0;
        if(state.plays>0&&time>=total*state.plays)return state.ids.back();
        std::int64_t remaining=time%total;
        for(std::size_t i=0;i<state.delays.size();++i){remaining-=state.delays[i];if(remaining<0)return state.ids[i];}
        return state.ids.back();
    }
    // 逐帧注册 GIF 图片（id 固定为 base+"#fN"），home 覆盖只会替换同名 Bitmap。
    bool registerAnimation(const char* base,const GifAnimation& animation,Animation& state) {
        std::vector<std::string> ids;ids.reserve(animation.frameCount());
        bool ok=true;
        for(std::size_t i=0;ok&&i<animation.frameCount();++i){
            auto id=std::string(base)+"#f"+std::to_string(i);
            if(!registerHomeUiImage(id,animation.frame(i),animation.width,animation.height))ok=false;
            ids.push_back(std::move(id));
        }
        if(!ok){
            static const unsigned char transparent[4]{};
            for(const auto& id:ids)registerHomeUiImage(id,transparent,1,1);
            return false;
        }
        state.ids=std::move(ids);state.delays=animation.delays;state.width=animation.width;state.height=animation.height;
        state.plays=animation.plays;state.started=steadyMillis();state.ready=true;return true;
    }
    // 释放上一组帧图：用 1x1 透明图覆盖同名 id，HomeOverlay 会回收旧 Bitmap。
    void clearAnimation(Animation& state) {
        static const unsigned char transparent[4]{};
        for(const auto& id:state.ids)registerHomeUiImage(id,transparent,1,1);
        state=Animation{};
    }
    // 用 1x1 透明图覆盖行图标 id：HomeOverlay 会回收旧 Bitmap，同时释放预算。
    void clearRowImage(const std::string& id) {
        static const unsigned char transparent[4]{};
        registerHomeUiImage(id,transparent,1,1);
        auto it=rowImages.find(id);
        if(it==rowImages.end())return;
        rowImageBytes-=std::size_t(it->second.width)*std::size_t(it->second.height)*4;
        rowImages.erase(it);
    }
    void clearRowImages() {
        while(!rowImages.empty())clearRowImage(rowImages.begin()->first);
        rowImageBytes=0;
    }
    // 行动画的当前帧 id：与选中项动画同一套算法，按累计帧延时取模循环。
    static std::string rowFrameId(const RowAnimation& state) {
        if(state.ids.empty())return {};
        if(state.ids.size()==1||state.delays.size()!=state.ids.size())return state.ids.front();
        std::int64_t total{};for(int delay:state.delays)total+=delay;if(total<=0)return state.ids.front();
        const auto elapsed=steadyMillis()-state.started;const std::int64_t time=elapsed>0?elapsed:0;
        if(state.plays>0&&time>=total*state.plays)return state.ids.back();
        std::int64_t remaining=time%total;
        for(std::size_t i=0;i<state.delays.size();++i){remaining-=state.delays[i];if(remaining<0)return state.ids[i];}
        return state.ids.back();
    }
    // 行图标（静态首帧或动画帧）统一降采样到长边 ≤64 px：真机只显示 48 单位，原图尺寸没有意义。
    Pixels rowPixels(std::span<const unsigned char> rgba,int width,int height) {
        const auto size=rowImageSize(width,height);
        const int dw=size.first,dh=size.second;
        if(dw<=0||dh<=0)return {};
        if(rgba.size()!=std::size_t(width)*std::size_t(height)*4)return {};
        if(dw==width&&dh==height)return Pixels{width,height,std::vector<unsigned char>(rgba.begin(),rgba.end())};
        auto scaled=scaleRgba(rgba,width,height,dw,dh);
        if(scaled.size()!=std::size_t(dw)*std::size_t(dh)*4)return {};
        return Pixels{dw,dh,std::move(scaled)};
    }
    // 用一个 1x1 透明图覆盖整组帧 id 并释放预算；HomeOverlay 会随即回收旧 Bitmap。
    void clearRowAnimation(const std::string& key) {
        auto it=rowAnimations.find(key);
        if(it==rowAnimations.end())return;
        static const unsigned char transparent[4]{};
        for(const auto& id:it->second.ids)registerHomeUiImage(id,transparent,1,1);
        rowImageBytes-=it->second.bytes;
        rowAnimations.erase(it);
    }
    void clearRowAnimations() {
        while(!rowAnimations.empty())clearRowAnimation(rowAnimations.begin()->first);
    }
    // 注册一行的 GIF：帧 id 为 sle.row.<serverId>#fN；超过 8 行或 32 MiB 时按 LRU 淘汰最旧的一行动画。
    bool registerRowAnimation(const std::string& key,const GifAnimation& gif) {
        if(gif.frameCount()<2||gif.width<=0||gif.height<=0)return false;
        // 帧按长边 ≤64 px 降采样后再计入预算，延迟数组保持原样。
        const auto size=rowImageSize(gif.width,gif.height);
        const int width=size.first,height=size.second;
        if(width<=0||height<=0)return false;
        const std::size_t frameBytes=std::size_t(width)*std::size_t(height)*4;
        const std::size_t bytes=frameBytes*gif.frameCount();
        if(bytes>RowAnimationBytes)return false;
        clearRowAnimation(key);
        while(!rowAnimations.empty()&&(rowAnimations.size()>=RowAnimationCount||rowImageBytes+bytes>RowImageBudget)) {
            auto victim=rowAnimations.begin();
            for(auto it=rowAnimations.begin();it!=rowAnimations.end();++it)if(it->second.used<victim->second.used)victim=it;
            clearRowAnimation(victim->first);
        }
        if(rowImageBytes+bytes>RowImageBudget)return false;
        std::vector<std::vector<unsigned char>> frames;
        frames.reserve(gif.frameCount());
        for(std::size_t i=0;i<gif.frameCount();++i) {
            auto frame=scaleRgba(gif.frame(i),gif.width,gif.height,width,height);
            if(frame.size()!=frameBytes)return false;
            frames.push_back(std::move(frame));
        }
        RowAnimation state;state.width=width;state.height=height;state.delays=gif.delays;state.plays=gif.plays;
        state.started=state.used=steadyMillis();state.bytes=bytes;
        static const unsigned char transparent[4]{};
        state.ids.reserve(frames.size());
        for(std::size_t i=0;i<frames.size();++i) {
            auto id="sle.row."+key+"#f"+std::to_string(i);
            if(!registerHomeUiImage(id,frames[i],width,height)) {
                for(const auto& made:state.ids)registerHomeUiImage(made,transparent,1,1);
                return false;
            }
            state.ids.push_back(std::move(id));
        }
        rowImageBytes+=bytes;rowAnimations[key]=std::move(state);return true;
    }
    // 翻页后回收不在当前页的行动画，把预算留给真正看得见的行。
    void releaseOffPageRowAnimations(int rows) {
        std::vector<std::string> visible;
        auto servers=ServerStore::getInstance().list();
        for(int i=0;i<rows;++i) {
            auto index=page*rows+i;
            if(index<0||index>=static_cast<int>(servers.size()))break;
            visible.push_back(servers[index].id);
        }
        for(auto it=rowAnimations.begin();it!=rowAnimations.end();) {
            auto current=it++;
            if(std::find(visible.begin(),visible.end(),current->first)==visible.end())clearRowAnimation(current->first);
        }
    }
    void evictRowImages(std::size_t need) {
        while(!rowImages.empty()&&(rowImages.size()>=RowImageCount||rowImageBytes+need>RowImageBudget)) {
            auto victim=rowImages.begin();
            for(auto it=rowImages.begin();it!=rowImages.end();++it)if(it->second.used<victim->second.used)victim=it;
            clearRowImage(victim->first);
        }
    }
    // 注册列表行图标（只存静态首帧）；同 id 同尺寸时直接复用，避免重复上传。
    bool registerRowImage(const std::string& id,std::span<const unsigned char> rgba,int width,int height) {
        if(id.empty()||width<=0||height<=0)return false;
        const std::size_t bytes=std::size_t(width)*std::size_t(height)*4;
        if(bytes*2>RowImageBudget)return false;
        auto it=rowImages.find(id);
        if(it!=rowImages.end()) {
            it->second.used=steadyMillis();
            if(it->second.width==width&&it->second.height==height)return true;
            if(!registerHomeUiImage(id,rgba,width,height))return false;
            rowImageBytes-=std::size_t(it->second.width)*std::size_t(it->second.height)*4;
            it->second.width=width;it->second.height=height;rowImageBytes+=bytes;
            return true;
        }
        evictRowImages(bytes);
        while(!rowAnimations.empty()&&rowImageBytes+bytes>RowImageBudget) {
            auto victim=rowAnimations.begin();
            for(auto it=rowAnimations.begin();it!=rowAnimations.end();++it)if(it->second.used<victim->second.used)victim=it;
            clearRowAnimation(victim->first);
        }
        if(rowImageBytes+bytes>RowImageBudget)return false;
        if(!registerHomeUiImage(id,rgba,width,height))return false;
        rowImages[id]={width,height,steadyMillis()};rowImageBytes+=bytes;
        return true;
    }
    // 列表标题与 Windows 端一致：自定义名称 → /v1/join 返回的名称 → host:port。
    // 旧版本会把地址写进名称，遇到与地址或端点相同的名称时当作未命名处理，避免历史数据看不了探测名称。
    std::string displayName(const SavedServer& server,std::map<std::string,Status>::const_iterator status) const {
        auto endpoint=endpointKey(server.address,server.port);
        if(!server.name.empty()&&server.name!=server.address&&server.name!=endpoint)return server.name;
        if(status!=statuses.end()&&!status->second.title.empty())return status->second.title;
        return endpoint;
    }

    // 当前真正可以加入的服务器：必须已选中、仍在列表里、地址端口有效。
    std::optional<SavedServer> joinTarget() const {
        if(selected.empty())return std::nullopt;
        auto server=ServerStore::getInstance().get(selected);
        if(!server||!client::validServerEndpoint(server->address,server->port))return std::nullopt;
        return server;
    }
    // 清空选中及其详情资源：翻页、重新打开、新增服务器后都必须回到"未选中"，
    // 否则「加入游戏」会连到屏幕上看不到的旧条目。
    void clearSelection() {
        selected.clear();
        logoReady=bannerReady=false;
        clearAnimation(logoAnimation);clearAnimation(bannerAnimation);
    }
    // 渲染线程取出最近一次加入结果；只在持有 mutex 时调用。
    void drainJoinNotice() {
        bool pending{};
        {std::lock_guard guard(joinMutex);pending=joinNoticePending;if(pending){notice=std::move(joinNotice);joinNoticePending=false;}}
        (void)pending;
    }
    // Launcher 1.6.0 在 callback=true 时跳过游戏分发。首页必须放行完整手势，
    // 原版 light_text_button 才能显示按压绿色；Java 在 UP 后用实际渲染边界确认入口。
    bool homeTouch(const pl::input::TouchEvent& event) {
        if(!active)return false;
        {std::lock_guard lock(mutex);if(!moduleEnabled||open)return false;}
        observeHomeUiTouch(event.x,event.y,event.action,event.pointerId);
        return false;
    }
    void requestRepaint() {
        {std::lock_guard lock(renderMutex);repaintRequested=true;}
        renderChanged.notify_one();
    }

    bool homeAllowed() const {
        auto state=homeUiState();return state.ready&&state.home&&!state.blocked;
    }
    void dismiss() {
        // 关闭列表即作废在途的加入请求，迟到的 DNS 结果不会再拉起游戏。
        if(joining.exchange(false))cancelServerJoin();
        bool wasOpen=open;open=form=confirm=false;press.cancel();hits.clear();
        // 关闭时释放逐帧 Bitmap，重新打开后由探测流程重新注册。
        if(wasOpen){clearAnimation(logoAnimation);clearAnimation(bannerAnimation);clearRowAnimations();clearRowImages();latencyProbe.pause();pl::input::hideKeyboard();}
    }
    void openFromHome() {
        if(!active)return;auto state=homeUiState();
        {std::lock_guard lock(mutex);
        if(!moduleEnabled||!state.ready||!state.home||state.blocked){submitHomeUiFrame(0,{},state.session);return;}
        open=true;form=confirm=false;press.cancel();hits.clear();clearSelection();autoRefreshPending=autoRefresh;}
        // 首次打开或缓存过期时自动补齐资料与图片；行点击仍然只做选中，不发查询。
    }

    // imagesOnly 只做“选中即出图”：不查 /v1/join、不写 statuses、不置 pending。
    void enqueue(const SavedServer& s,bool imagesOnly=false) {
        if(!client::validServerEndpoint(s.address,s.port))return;
        std::erase_if(tasks,[&](const auto& task){return task.server.id==s.id;});
        if(!imagesOnly)bindEndpoint(statuses[s.id],endpointKey(s.address,s.port));
        tasks.push_back({s,++generations[s.id],imagesOnly});
        if(!imagesOnly)statuses[s.id].pending=true;
        changed.notify_one();
    }
    void refresh() {for(const auto& s:ServerStore::getInstance().list())enqueue(s);}
    // 打开列表即刷新全部服务器：正在查询的（pending）跳过，避免重复入队。
    void refreshOnOpen() {
        int queued=0;
        for(const auto& s:ServerStore::getInstance().list()){
            auto it=statuses.find(s.id);
            if(it!=statuses.end()&&it->second.pending)continue;
            enqueue(s);++queued;
        }
        if(queued)__android_log_print(ANDROID_LOG_INFO,"ServerListEnhancer","Opening list: queued %d server(s) for refresh",queued);
    }
    bool current(const Task& task) {
        auto server=ServerStore::getInstance().get(task.server.id);
        return active&&generations[task.server.id]==task.generation&&server&&server->address==task.server.address&&server->port==task.server.port;
    }
    // 返回静态像素；如果内容是能播放的多帧 GIF，就把逐帧动画填进 animation 并返回空。
    // 解码失败或超出预算时仍然走 decodeImage，即平台静态首帧。
    Pixels image(const ImageReference& ref,bool banner,GifAnimation& animation) {
        if(ref.type=="builtin")return decodeImage(banner?client::builtin::banner():client::builtin::logo(),banner);
        if(ref.type!="base64"&&ref.type!="url")return {};
        std::vector<unsigned char> bytes;std::filesystem::path path;bool store{};
        if(ref.type=="base64"){
            if(banner)return {};
            auto decoded=decodeInlinePng(ref.value);if(!decoded)return {};
            bytes=std::move(decoded.data);
        }else{
            path=cache/(cacheName(ref.value)+".image");bytes=readBytes(path,MaxImageBytes);
            if(!bytes.empty()){
                if(auto frames=decodeGifAnimation(bytes,banner);frames&&frames->frameCount()>1){animation=std::move(*frames);return {};}
                if(auto result=decodeImage(bytes,banner))return result;
                bytes.clear();
            }
            auto downloaded=download(ref.value,MaxImageBytes,4000,&active);
            if(!downloaded)return {};bytes=std::move(downloaded.data);store=true;
        }
        if(auto frames=decodeGifAnimation(bytes,banner);frames&&frames->frameCount()>1){
            if(store){saveBytes(path,bytes);pruneCache();}
            animation=std::move(*frames);return {};
        }
        auto result=decodeImage(bytes,banner);
        if(result&&store){saveBytes(path,bytes);pruneCache();}
        return result;
    }
    void pruneCache() {
        std::lock_guard lock(cacheWriteMutex);
        std::error_code ec;std::uintmax_t total{};std::vector<std::filesystem::directory_entry> files;
        for(std::filesystem::directory_iterator it(cache,ec),end;!ec&&it!=end;it.increment(ec))if(it->is_regular_file(ec)){total+=it->file_size(ec);files.push_back(*it);}
        if(total<=128*1024*1024)return;
        std::sort(files.begin(),files.end(),[](const auto& a,const auto& b){std::error_code e1,e2;return a.last_write_time(e1)<b.last_write_time(e2);});
        for(const auto& file:files){if(total<=128*1024*1024)break;auto n=file.file_size(ec);if(std::filesystem::remove(file.path(),ec))total-=n;}
    }
    void worker() {
        while(active.load()) {
            Task task;Status status;
            {std::unique_lock lock(mutex);changed.wait(lock,[&]{return !active.load()||!tasks.empty();});if(!active.load())break;task=tasks.front();tasks.pop_front();status=statuses[task.server.id];}
            const auto& server=task.server;auto key=endpointKey(server.address,server.port);auto cached=readBytes(cache/(cacheName(key)+".json"),MaxJsonBytes);
            if(!cached.empty())parseDiscovery(std::string_view(reinterpret_cast<const char*>(cached.data()),cached.size()),status);
            if(!task.imagesOnly) {
                const auto authority=client::displayAddress(server.address,server.port);auto response=download("https://"+authority+"/v1/join",MaxJsonBytes,2000,&active);
                if(!response&&plainHttp&&active.load())response=download("http://"+authority+"/v1/join",MaxJsonBytes,2000,&active);
                if(!active.load())break;
                if(response&&parseDiscovery(std::string_view(reinterpret_cast<const char*>(response.data.data()),response.data.size()),status)){saveBytes(cache/(cacheName(key)+".json"),response.data);pruneCache();}
                else status.error=response.error.empty()?"服务器资料格式无效":response.error;
                status.pending=false;status.refreshedAt=std::chrono::steady_clock::now();
            }
            bool selectedNow{},visible{};
            {std::lock_guard lock(mutex);if(!current(task))continue;if(!task.imagesOnly)statuses[server.id]=status;selectedNow=selected==server.id;visible=open;}
            ImageReference logo,banner;if(status.visual){logo=status.visual->logo;banner=status.visual->introImage;}
            if(!server.logoUrl.empty())logo={"url",server.logoUrl};if(!server.introImageUrl.empty())banner={"url",server.introImageUrl};
            // 行图标只认这台服务器自己的图；选中项的详情横幅才需要单独解码。
            const bool rowLogo=(logo.type=="url"||logo.type=="base64")&&!logo.value.empty();
            GifAnimation logoFrames,bannerFrames;Pixels lp,bp;
            if(rowLogo||selectedNow){lp=image(logo,false,logoFrames);if(!active.load())break;}
            if(selectedNow){bp=image(banner,true,bannerFrames);if(!active.load())break;}
            {
                std::lock_guard lock(mutex);
                if(!current(task))continue;
                // 行内只保留静态首帧：整页列表不会同时动画，只有选中项的详情可以播 GIF。
                // 列表关闭时不注册行图标，避免 dismiss() 清空后又被后台线程补回来。
                if(rowLogo&&visible){
                    // 静态首帧与动画帧共用同一套降采样结果，切换时不会跳尺寸。
                    if(logoFrames.valid()&&logoFrames.frameCount()>0) {
                        auto scaled=rowPixels(logoFrames.frame(0),logoFrames.width,logoFrames.height);
                        if(scaled)registerRowImage("sle.row."+server.id,scaled.rgba,scaled.width,scaled.height);
                    }
                    else if(lp) {
                        auto scaled=rowPixels(lp.rgba,lp.width,lp.height);
                        if(scaled)registerRowImage("sle.row."+server.id,scaled.rgba,scaled.width,scaled.height);
                    }
                    // 预算允许时把整组帧注册进来：未选中的行也按自己的延时间隔播放。
                    if(logoFrames.valid()&&logoFrames.frameCount()>1)registerRowAnimation(server.id,logoFrames);
                }
                if(selectedNow&&selected==server.id){
                    clearAnimation(logoAnimation);clearAnimation(bannerAnimation);
                    logoReady=lp&&registerHomeUiImage("sle.selected.logo",lp.rgba,lp.width,lp.height);
                    bannerReady=bp&&registerHomeUiImage("sle.selected.banner",bp.rgba,bp.width,bp.height);
                    if(logoFrames.valid())logoReady=registerAnimation("sle.selected.logo",logoFrames,logoAnimation);
                    if(bannerFrames.valid())bannerReady=registerAnimation("sle.selected.banner",bannerFrames,bannerAnimation);
                    if(logoAnimation.ready){logoWidth=logoAnimation.width;logoHeight=logoAnimation.height;}
                    else if(logoReady){logoWidth=lp.width;logoHeight=lp.height;}
                    if(bannerAnimation.ready){bannerWidth=bannerAnimation.width;bannerHeight=bannerAnimation.height;}
                    else if(bannerReady){bannerWidth=bp.width;bannerHeight=bp.height;}
                }
            }
            // 探测结束后为这一行触发一次 UDP 时延查询；结果缓存 15 秒，翻页或关闭列表时 pause。
            if(!task.imagesOnly&&visible&&active.load()&&client::validServerEndpoint(server.address,server.port)){
                // 端口优先级与 Windows 一致：服务器 /v1/join 通告的探测端口优先，其次配置里的默认值。
                int echoPort=udpProbePort;if(status.visual&&status.visual->udpProbePort>0)echoPort=status.visual->udpProbePort;
                latencyProbe.request(endpointKey(server.address,server.port),server.address,server.port,echoPort);
            }
        }
    }
    void newForm(const std::string& id) {
        editId=id;name.clear();address.clear();if(auto s=ServerStore::getInstance().get(id)){name=s->name;address=client::displayAddress(s->address,s->port);}
        nameEdit.end(name);addressEdit.end(address);form=true;confirm=false;focus=1;notice.clear();pl::input::showKeyboard();
    }
    void save() {
        auto parsed=client::parseServerAddress(address);if(!parsed){notice=parsed.error;return;}
        auto& store=ServerStore::getInstance();if(editId.empty()&&store.list().size()>=512){notice="服务器列表已达 512 条";return;}
        SavedServer s;if(auto old=store.get(editId))s=*old;
        // 名称留空就保持空（与 Windows 一致）：显示时再回退到探测名称或 host:port，
        // 探测结果不会写回 servers.json，因此失败时已有名称不会被改掉。
        s.name=name;s.address=parsed.host;s.port=parsed.port;
        // 只有编辑"本来就是选中项"的服务器时把选中 id 换成新 id；新增一律回到未选中。
        const bool wasSelected=!editId.empty()&&selected==editId;
        auto id=store.upsert(s);
        if(id.empty()||!store.save()){notice="保存失败，请检查存储空间";return;}
        if(wasSelected)selected=id;else if(editId.empty())clearSelection();
        logoReady=bannerReady=false;clearAnimation(logoAnimation);clearAnimation(bannerAnimation);clearRowAnimation(id);clearRowImage("sle.row."+id);
        form=false;notice="已保存";pl::input::hideKeyboard();enqueue(*store.get(id));
    }
    void act(const Hit& hit) {
        switch(hit.action) {
        case 1:refresh();notice="已排队查询全部服务器";break;
        case 2:newForm({});break;
        case 3:dismiss();break;
        case 4:if(page>0)--page;clearSelection();latencyProbe.pause();releaseOffPageRowAnimations(std::max(1,static_cast<int>((viewHeight-245)/64)));break;
        case 5:++page;clearSelection();latencyProbe.pause();releaseOffPageRowAnimations(std::max(1,static_cast<int>((viewHeight-245)/64)));break;
        case 6:selected=hit.id;logoReady=bannerReady=false;clearAnimation(logoAnimation);clearAnimation(bannerAnimation);if(auto s=ServerStore::getInstance().get(selected))enqueue(*s,true);break;
        case 7:save();break;
        case 8:form=false;confirm=false;pl::input::hideKeyboard();break;
        case 9:confirm=true;pl::input::hideKeyboard();break;
        case 10:focus=0;nameEdit.end(name);pl::input::showKeyboard();break;
        case 11:focus=1;addressEdit.end(address);pl::input::showKeyboard();break;
        case 15:{
            // 加入选中的服务器：仅在选中项仍有效且没有在途请求时发起，重复点击直接忽略。
            if(joining.load()){notice="正在加入，请稍候";break;}
            auto server=joinTarget();
            if(!server){notice="请先选择一台可用的服务器";break;}
            joining.store(true);notice="正在解析地址并加入…";
            if(!requestServerJoin(server->address,server->port)){joining.store(false);notice="加入功能不可用（界面尚未就绪）";}
            break;}
        case 14:if(!hit.id.empty())newForm(hit.id);break;
        case 13:++generations[editId];std::erase_if(tasks,[&](const auto& t){return t.server.id==editId;});ServerStore::getInstance().remove(editId);ServerStore::getInstance().save();statuses.erase(editId);clearRowAnimation(editId);clearRowImage("sle.row."+editId);selected.clear();logoReady=bannerReady=false;clearAnimation(logoAnimation);clearAnimation(bannerAnimation);form=confirm=false;break;
        }
    }
    bool touch(const pl::input::TouchEvent& event) {
        if(!active||!homeAllowed())return false;std::lock_guard lock(mutex);if(!moduleEnabled||!open)return false;
        auto x=event.x/scale,y=event.y/scale;int action=event.action&255;
        if(action==0){press.cancel();for(const auto& hit:hits)if(hit.contains(x,y)){press.begin(hit);break;}}
        else if(action==2)press.move(x,y);
        else if(action==1){if(auto hit=press.release(hits,x,y))act(*hit);}
        else if(action==3||action==5||action==6)press.cancel();
        requestRepaint();return true;
    }
    bool key(const pl::input::KeyEvent& event) {
        if(!active||!homeAllowed())return false;std::lock_guard lock(mutex);if(!moduleEnabled||!open)return false;
        if(!event.isKeyDown)return true;
        if(event.keyCode==4){if(form){form=confirm=false;press.cancel();pl::input::hideKeyboard();}else dismiss();return true;}
        if(!form||confirm)return true;
        auto& text=focus?address:name;auto& editor=focus?addressEdit:nameEdit;
        if(event.keyCode==67)editor.backspace(text);
        else if(event.keyCode==66)save();
        else if(event.keyCode==61){focus=1-focus;}
        else if(event.keyCode==21)editor.left(text,false);
        else if(event.keyCode==22)editor.right(text,false);
        else if(event.unicodeChar>=32&&event.unicodeChar<=0x10ffff)editor.insert(text,codepoint(event.unicodeChar),focus?255:128);
        return true;
    }
    void render() {
        auto logo=decodeImage(client::builtin::logo(),false),banner=decodeImage(client::builtin::banner(),true);
        if(logo)registerHomeUiImage("sle.default.logo",logo.rgba,logo.width,logo.height);
        if(banner)registerHomeUiImage("sle.default.banner",banner.rgba,banner.width,banner.height);
        int lastMode=0;
        while(active.load()) {
            auto display=homeUiState();std::vector<pl::modmenu::DrawCommand> commands;int mode=0;
            {std::lock_guard lock(mutex);hits.clear();
            const bool allowed=display.ready&&display.home&&!display.blocked&&moduleEnabled&&display.width>0&&display.height>0;
            if(!allowed)dismiss();
            else if(open)mode=2;
            if(mode==2&&autoRefreshPending){autoRefreshPending=false;refreshOnOpen();}
            if(mode==2){
                drainJoinNotice();
                scale=std::max(.5f,std::min(display.width/800.f,display.height/480.f));viewWidth=display.width/scale;viewHeight=display.height/scale;
                auto rect=[&](float x,float y,float w,float h,std::uint32_t color){commands.push_back({.type=pl::modmenu::DrawCommandType::RectFilled,.x=x*scale,.y=y*scale,.w=w*scale,.h=h*scale,.color=color});};
                auto text=[&](float x,float y,std::string value,float size=16,std::uint32_t color=0xffffffff){commands.push_back({.type=pl::modmenu::DrawCommandType::Text,.x=x*scale,.y=(y+size)*scale,.color=color,.size=size*scale,.text=std::move(value)});};
                auto button=[&](float x,float y,float w,std::string label,int action,std::string id=std::string{}){
                    Hit hit{x,y,w,36,action,std::move(id)};const bool down=press.isPressed(hit);const float offset=down?1.f:0.f;
                    rect(x,y,w,36,down?0xff3c8527:0xff424242);rect(x,y,w,down?1:2,down?0xff285c1b:0xff686868);
                    rect(x,y+34,w,2,down?0xff285c1b:0xff252525);
                    const float labelWidth=textWidth(label,16);text(x+(w-labelWidth)/2,y+8+offset,std::move(label),16);hits.push_back(std::move(hit));};
                auto image=[&](const char* id,float x,float y,float w,float h,int iw,int ih){float r=std::min(w/iw,h/ih);commands.push_back({.type=pl::modmenu::DrawCommandType::Image,.x=(x+(w-iw*r)/2)*scale,.y=(y+(h-ih*r)/2)*scale,.w=iw*r*scale,.h=ih*r*scale,.color=0xffffffff,.imageId=id});};
                // 右侧信息列（玩家数/UDP 时延/版本）右对齐：Home 覆盖层只有左对齐，先估算宽度再反推起点。
                auto rightText=[&](float rightEdge,float y,std::string value,float size,std::uint32_t color=0xffffffff){commands.push_back({.type=pl::modmenu::DrawCommandType::Text,.x=(rightEdge-textWidth(value,size))*scale,.y=(y+size)*scale,.color=color,.size=size*scale,.text=std::move(value)});};
                rect(0,0,viewWidth,viewHeight,0xff1c201e);float left=20,right=viewWidth-20;
                text(left,16,"服务器列表",24);button(right-280,16,80,"刷新",1);button(right-190,16,90,"添加",2);button(right-90,16,90,"关闭",3);
                if(form){
                    float x=std::max(20.f,(viewWidth-560)/2);rect(x,65,560,310,0xff303632);text(x+20,75,confirm?"确认删除此服务器？":editId.empty()?"添加服务器":"编辑服务器",22);
                    if(confirm){text(x+20,125,fit(name,28));button(x+20,230,180,"确认删除",13);button(x+220,230,180,"取消",8);}
                    else {
                        // 光标约 500ms 亮灭一次；只按已绘制文字的像素宽度定位，不参与布局。
                        const bool caretOn=(steadyMillis()/500)%2==0;
                        text(x+20,112,"名称（可留空）",14);rect(x+20,135,520,38,focus==0?0xff486045:0xff454545);
                        const std::string nameText=fit(name.empty()?"点击输入名称":name,28);text(x+30,142,nameText);
                        if(focus==0&&caretOn)rect(std::min(x+30+(name.empty()?0.f:textWidth(nameText,28)),x+534),142,2,28,0xffffffff);
                        hits.push_back({x+20,135,520,38,10,{}});
                        text(x+20,185,"地址：域名/IP:端口，默认 19132",14);rect(x+20,209,520,38,focus==1?0xff486045:0xff454545);
                        const std::string addressText=fit(address.empty()?"点击输入 play.example.com:19132":address,40);text(x+30,216,addressText,16);
                        if(focus==1&&caretOn)rect(std::min(x+30+(address.empty()?0.f:textWidth(addressText,16)),x+534),216,2,16,0xffffffff);
                        hits.push_back({x+20,209,520,38,11,{}});
                        button(x+20,275,120,"保存",7);button(x+150,275,120,"取消",8);if(!editId.empty())button(x+400,275,140,"删除",9);
                        text(x+20,325,fit(notice,42),14,0xffffcb80);
                    }
                } else {
                    auto servers=ServerStore::getInstance().list();int rows=std::max(1,static_cast<int>((viewHeight-245)/64));int pages=std::max(1,(static_cast<int>(servers.size())+rows-1)/rows);page=std::clamp(page,0,pages-1);
                    if(servers.empty())text(left+20,100,"还没有服务器，点击右上角“添加”开始。",19);
                    for(int row=0;row<rows;++row){auto index=page*rows+row;if(index>=static_cast<int>(servers.size()))break;const auto& s=servers[index];float y=65+row*64.f;
                        const float editX=right-35,columnRight=editX-8;
                        const Hit rowHit{left,y,editX-left,58,6,s.id};
                        rect(left,y,editX-left,58,press.isPressed(rowHit)?0xff3c8527:s.id==selected?0xff44633b:0xe03a403c);
                        // 行图标：该行 GIF 当前帧 → 该行静态首帧 → 选中项动画 → 选中项静图 → 默认图标。
                        const std::int64_t rowNow=steadyMillis();
                        std::string rowFrame;int rowFrameW=0,rowFrameH=0;
                        if(auto animation=rowAnimations.find(s.id);animation!=rowAnimations.end()){rowFrame=rowFrameId(animation->second);animation->second.used=rowNow;rowFrameW=animation->second.width;rowFrameH=animation->second.height;}
                        std::string frameId=selected==s.id?animationId(logoAnimation):std::string{};
                        std::string rowId="sle.row."+s.id;auto ownImage=rowImages.find(rowId);
                        std::string logoId="sle.default.logo";int logoW=64,logoH=64;
                        if(!rowFrame.empty()){logoId=rowFrame;logoW=rowFrameW;logoH=rowFrameH;}
                        else if(ownImage!=rowImages.end()){logoId=rowId;logoW=ownImage->second.width;logoH=ownImage->second.height;ownImage->second.used=rowNow;}
                        else if(!frameId.empty()){logoId=frameId;logoW=logoAnimation.width;logoH=logoAnimation.height;}
                        else if(selected==s.id&&logoReady){logoId="sle.selected.logo";logoW=logoWidth;logoH=logoHeight;}
                        image(logoId.c_str(),left+6,y+5,48,48,logoW,logoH);
                        auto status=statuses.find(s.id);text(left+66,y+4,fit(displayName(s,status),26),18);
                        text(left+66,y+32,fit(client::displayAddress(s.address,s.port),44),13,0xffcccccc);
                        const bool addressValid=client::validServerEndpoint(s.address,s.port);
                        std::string population="未查询";std::uint32_t populationColor=0xffebebeb;
                        if(!addressValid){population="请编辑地址";populationColor=0xffffc0c0;}
                        else if(status!=statuses.end()){const auto& v=status->second;population=v.pending?"查询中…":!v.error.empty()?"查询失败":std::to_string(v.players)+"/"+std::to_string(v.capacity);}
                        rightText(columnRight,y+4,fit(population,12),13,populationColor);
                        // UDP 时延与 Windows 同文案同配色：<ms> ms / UDP 查询中 / UDP 未响应 / 地址无效。
                        int ms=-1;bool latencyPending{};
                        if(addressValid)if(auto latency=latencyProbe.get(endpointKey(s.address,s.port))){ms=latency->ms;latencyPending=latency->pending;}
                        const std::string ping=!addressValid?"地址无效":ms>=0?(ms>=1000?"1000+ ms":std::to_string(ms)+" ms"):latencyPending?"UDP 查询中":"UDP 未响应";
                        const int latencyBand=addressValid?sle::udp::band(ms):0;
                        rightText(columnRight,y+22,fit(ping,12),13,latencyBand==1?0xff81e65e:latencyBand==2?0xffffde56:latencyBand==3?0xffff7f73:0xffcacaca);
                        if(status!=statuses.end()&&!status->second.version.empty())rightText(columnRight,y+40,fit(status->second.version,16),13,0xffe1e1e1);
                        // 行尾铅笔按钮：76 灰底 + 1px 分隔线 + 8 个 3x3 像素斜线；点击直接编辑这一行。
                        const Hit editHit{editX,y,35,58,14,s.id};const bool editDown=press.isPressed(editHit);
                        rect(editX,y,35,58,editDown?0xff3c8527:0xff4c4c4c);rect(editX,y,1,58,editDown?0xff285c1b:0xff787878);
                        for(int pen=0;pen<8;++pen)rect(editX+12+pen,y+34-pen+(editDown?1:0),3,3,0xffeeeeee);
                        hits.push_back(editHit);hits.push_back(rowHit);
                    }
                    // 加入按钮：列表中部、详情图上方居中；没有可用选中项时变暗且不注册点击区。
                    const bool joinable=joinTarget().has_value();
                    const bool joinBusy=joining.load();
                    const bool joinActive=joinable&&!joinBusy;
                    const float joinWidth=200,joinHeight=36;
                    const float joinX=(viewWidth-joinWidth)/2,joinY=viewHeight-170-42;
                    const Hit joinHit{joinX,joinY,joinWidth,joinHeight,15,{}};const bool joinDown=joinActive&&press.isPressed(joinHit);
                    rect(joinX,joinY,joinWidth,joinHeight,joinDown?0xff2a6b22:joinActive?0xff3fa34a:0xff2c322c);
                    rect(joinX,joinY,joinWidth,1,joinDown?0xff204b19:joinActive?0xff7fd486:0xff3a423a);
                    rect(joinX,joinY+34,joinWidth,2,joinActive?0xff204b19:0xff252a25);
                    const std::string joinLabel=joinBusy?"正在加入…":"加入游戏";
                    text(joinX+(joinWidth-textWidth(joinLabel,16))/2,joinY+10+(joinDown?1:0),joinLabel,16,joinActive?0xffffffff:0xff8f968f);
                    if(joinActive)hits.push_back(joinHit);
                    float dy=viewHeight-170;rect(left,dy,right-left,110,0xd0252c27);
                    if(auto s=ServerStore::getInstance().get(selected)){
                        std::string bannerId=animationId(bannerAnimation);
                        image(bannerId.empty()?(bannerReady?"sle.selected.banner":"sle.default.banner"):bannerId.c_str(),left+8,dy+5,220,100,
                            bannerId.empty()?(bannerReady?bannerWidth:960):bannerAnimation.width,
                            bannerId.empty()?(bannerReady?bannerHeight:320):bannerAnimation.height);
                        std::string intro=s->introText;auto state=statuses.find(selected);if(intro.empty()&&state!=statuses.end()){if(state->second.visual)intro=state->second.visual->introText;else intro=state->second.message;}
                        std::istringstream lines(intro);std::string line;int row=0;while(row<4&&std::getline(lines,line)){text(left+245,dy+8+row*22,fit(line,34),15);++row;}
                    }else text(left+20,dy+35,"点选服务器查看图文资料；编辑可以修改或删除。",16);
                    button(left,viewHeight-48,90,"上一页",4);button(left+100,viewHeight-48,90,"下一页",5);text(left+210,viewHeight-39,std::to_string(page+1)+"/"+std::to_string(pages),16);
                    if(!notice.empty())text(left+300,viewHeight-38,fit(notice,22),13,0xffcbdfb0);
                }
            }}
            if(mode==2||lastMode!=0||display.session!=0)submitHomeUiFrame(mode,commands,display.session);
            lastMode=mode;
            std::unique_lock renderLock(renderMutex);
            renderChanged.wait_for(renderLock,50ms,[&]{return repaintRequested||!active.load();});repaintRequested=false;
        }
        submitHomeUiFrame(0,{},0);
    }
public:
    static AndroidMod& instance(){static AndroidMod mod;return mod;}
    bool load(pl::mod::ModContext& context){
        javaVm=context.javaVm();modRoot=context.modRootPath();setJavaVm(javaVm);cache=context.dataDir()/"cache";std::error_code ec;std::filesystem::create_directories(cache,ec);if(ec)return false;
        auto& store=ServerStore::getInstance();store.setPath(context.modRootPath()/"servers.json");if(!store.load())return false;
        auto configPath=context.configDir()/"config.json";Config config;
        if(std::filesystem::exists(configPath)){std::ifstream file(configPath);auto j=nlohmann::json::parse(file,nullptr,false);if(!j.is_object())return false;config=pl::reflection::deserialize<Config>(j);}
        else if(!pl::config::writeConfig(configPath,pl::config::defaultJson(config)))return false;
        moduleEnabled=config.enabled;plainHttp=config.allowPlainHttpFallback;udpProbePort=config.udpProbePort;pruneCache();
        Dl_info info{};if(!dladdr(reinterpret_cast<void*>(&setJavaVm),&info)||!dlopen(info.dli_fname,RTLD_NOW|RTLD_NODELETE))return false;
        __android_log_print(ANDROID_LOG_INFO,"ServerListEnhancer","Loaded Android ARM64 port; %zu servers; launcher 1.6.0 SDK 5d108da; restart game after updates",store.list().size());
        autoRefresh=config.autoRefresh;if(autoRefresh)refresh();return true;
    }
    bool enable(){
        active=true;
        pl::modmenu::ModuleInfo module;module.moduleId=Module;module.displayName="服务器列表增强";module.description="独立列表、触控编辑与服务器图文资料";module.defaultEnabled=moduleEnabled;module.hideInHudEditor=true;
        module.onToggle=[this](std::string_view,bool enabled){std::lock_guard lock(mutex);moduleEnabled=enabled;if(!enabled)dismiss();};
        if(!pl::modmenu::registerModule(module)){active=false;return false;}
        if(!initializeHomeUi(javaVm,modRoot,[this]{openFromHome();},
            [this](float x,float y,int action){pl::input::TouchEvent event{};event.x=x;event.y=y;event.action=action;return touch(event);},
            [this]{pl::input::KeyEvent event{};event.keyCode=4;event.isKeyDown=true;return key(event);},
            [this](int status,std::string message){
                // Java 可能从调用 requestServerJoin 的同一线程同步回报，这里只写 joinMutex 保护的结果区。
                std::lock_guard guard(joinMutex);
                joining.store(false);
                if(!message.empty())joinNotice=std::move(message);
                else joinNotice=status>0?"已发起加入":status==-2?"正在连接中，请稍候":"加入失败";
                joinNoticePending=true;
            })){
            pl::modmenu::unregisterModule(Module);active=false;return false;
        }
        if(!inputRegistered){pl::input::registerKeyCallback([this](const auto& e){return key(e);});inputRegistered=true;}
        if(!touchRegistered){pl::input::registerTouchCallback([this](const auto& e){return homeTouch(e);});touchRegistered=true;}
        latencyProbe.start();
        for(int i=0;i<2;++i)workers.emplace_back([this]{worker();});renderer=std::thread([this]{render();});
        __android_log_print(ANDROID_LOG_INFO,"ServerListEnhancer","Enabled home-screen server entry and two workers");return true;
    }
    bool disable(){
        active=false;if(joining.exchange(false))cancelServerJoin();changed.notify_all();requestRepaint();if(renderer.joinable())renderer.join();for(auto& t:workers)if(t.joinable())t.join();workers.clear();
        {std::lock_guard lock(mutex);open=form=false;press.cancel();hits.clear();tasks.clear();rowAnimations.clear();rowImages.clear();rowImageBytes=0;}
        latencyProbe.stop();
        pl::input::hideKeyboard();closeHomeUi();pl::modmenu::unregisterModule(Module);return ServerStore::getInstance().save();
    }
    bool unload(){return true;}
};
}
PL_REGISTER_MOD(sle::android::AndroidMod, sle::android::AndroidMod::instance());
