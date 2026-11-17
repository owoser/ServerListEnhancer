#include "AndroidPlatform.h"
#include "Config.h"
#include "Discovery.h"
#include "HomeUi.h"
#include "ServerStore.h"
#include "TextEdit.h"
#include "common/BuiltinImages.h"
#include "common/Endpoint.h"
#include <pl/Mod.hpp>
#include <pl/ModMenu.hpp>
#include <pl/Input.hpp>
#include <android/log.h>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <dlfcn.h>
#include <fstream>
#include <iomanip>
#include <map>
#include <mutex>
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
std::string codepoint(unsigned n) {
    std::string s;if(!n||n>0x10ffff||(n>=0xd800&&n<=0xdfff))return s;
    if(n<128)s+=static_cast<char>(n);
    else if(n<2048){s+=static_cast<char>(0xc0|(n>>6));s+=static_cast<char>(0x80|(n&63));}
    else if(n<65536){s+=static_cast<char>(0xe0|(n>>12));s+=static_cast<char>(0x80|((n>>6)&63));s+=static_cast<char>(0x80|(n&63));}
    else{s+=static_cast<char>(0xf0|(n>>18));s+=static_cast<char>(0x80|((n>>12)&63));s+=static_cast<char>(0x80|((n>>6)&63));s+=static_cast<char>(0x80|(n&63));}return s;
}
}
class AndroidMod {
    struct Hit {float x,y,w,h;int action;std::string id;bool contains(float a,float b)const{return a>=x&&b>=y&&a<x+w&&b<y+h;}};
    struct Task {SavedServer server;std::uint64_t generation;};
    std::mutex mutex;
    std::condition_variable changed;
    std::deque<Task> tasks;
    std::map<std::string,std::uint64_t> generations;
    std::map<std::string,Status> statuses;
    std::vector<std::thread> workers;
    std::thread renderer;
    std::atomic<bool> active{};
    bool inputRegistered{},moduleEnabled{true},open{},form{},confirm{},plainHttp{true};
    bool logoReady{},bannerReady{};
    int logoWidth{64},logoHeight{64},bannerWidth{960},bannerHeight{320};
    float scale{1},viewWidth{800},viewHeight{480};
    int page{},focus{},pressed{-1};
    std::vector<Hit> hits;
    std::string selected,editId,name,address,notice;
    client::TextEdit nameEdit,addressEdit;
    std::filesystem::path cache,modRoot;
    JavaVM* javaVm{};

    bool homeAllowed() const {
        auto state=homeUiState();return state.ready&&state.home&&!state.blocked;
    }
    void dismiss() {
        bool wasOpen=open;open=form=confirm=false;pressed=-1;hits.clear();
        if(wasOpen)pl::input::hideKeyboard();
    }
    void openFromHome() {
        if(!active)return;auto state=homeUiState();
        {std::lock_guard lock(mutex);
        if(!moduleEnabled||!state.ready||!state.home||state.blocked){submitHomeUiFrame(0,{},state.session);return;}
        open=true;form=confirm=false;pressed=-1;hits.clear();}
        refresh();
    }

    void enqueue(const SavedServer& s) {
        if(!client::validServerEndpoint(s.address,s.port))return;
        std::erase_if(tasks,[&](const auto& task){return task.server.id==s.id;});
        bindEndpoint(statuses[s.id],endpointKey(s.address,s.port));
        tasks.push_back({s,++generations[s.id]});statuses[s.id].pending=true;changed.notify_one();
    }
    void refresh() {for(const auto& s:ServerStore::getInstance().list())enqueue(s);}
    bool current(const Task& task) {
        auto server=ServerStore::getInstance().get(task.server.id);
        return active&&generations[task.server.id]==task.generation&&server&&server->address==task.server.address&&server->port==task.server.port;
    }
    Pixels image(const ImageReference& ref,bool banner) {
        if(ref.type=="builtin")return decodeImage(banner?client::builtin::banner():client::builtin::logo(),banner);
        if(ref.type=="base64"&&!banner){auto bytes=decodeInlinePng(ref.value);return bytes?decodeImage(bytes.data,false):Pixels{};}
        if(ref.type!="url")return {};
        auto path=cache/(cacheName(ref.value)+".image");auto bytes=readBytes(path,MaxImageBytes);
        if(!bytes.empty()){auto result=decodeImage(bytes,banner);if(result)return result;}
        auto downloaded=download(ref.value,MaxImageBytes,4000,&active);
        if(!downloaded)return {};
        auto result=decodeImage(downloaded.data,banner);
        if(result){saveBytes(path,downloaded.data);pruneCache();}return result;
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
            const auto authority=client::displayAddress(server.address,server.port);auto response=download("https://"+authority+"/v1/join",MaxJsonBytes,2000,&active);
            if(!response&&plainHttp&&active.load())response=download("http://"+authority+"/v1/join",MaxJsonBytes,2000,&active);
            if(!active.load())break;
            if(response&&parseDiscovery(std::string_view(reinterpret_cast<const char*>(response.data.data()),response.data.size()),status)){saveBytes(cache/(cacheName(key)+".json"),response.data);pruneCache();}
            else status.error=response.error.empty()?"服务器资料格式无效":response.error;
            status.pending=false;
            bool selectedNow{};
            {std::lock_guard lock(mutex);if(!current(task))continue;statuses[server.id]=status;selectedNow=selected==server.id;}
            if(selectedNow) {
                ImageReference logo,banner;if(status.visual){logo=status.visual->logo;banner=status.visual->introImage;}
                if(!server.logoUrl.empty())logo={"url",server.logoUrl};if(!server.introImageUrl.empty())banner={"url",server.introImageUrl};
                auto lp=image(logo,false);if(!active.load())break;auto bp=image(banner,true);if(!active.load())break;
                std::lock_guard lock(mutex);
                if(current(task)&&selected==server.id){
                    logoReady=lp&&registerHomeUiImage("sle.selected.logo",lp.rgba,lp.width,lp.height);
                    bannerReady=bp&&registerHomeUiImage("sle.selected.banner",bp.rgba,bp.width,bp.height);
                    if(logoReady){logoWidth=lp.width;logoHeight=lp.height;}if(bannerReady){bannerWidth=bp.width;bannerHeight=bp.height;}
                }
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
        s.name=name.empty()?parsed.host:name;s.address=parsed.host;s.port=parsed.port;auto id=store.upsert(s);
        if(id.empty()||!store.save()){notice="保存失败，请检查存储空间";return;}
        selected=id;logoReady=bannerReady=false;form=false;notice="已保存";pl::input::hideKeyboard();enqueue(*store.get(id));
    }
    void act(const Hit& hit) {
        switch(hit.action) {
        case 1:refresh();notice="已排队查询全部服务器";break;
        case 2:newForm({});break;
        case 3:dismiss();break;
        case 4:if(page>0)--page;break;
        case 5:++page;break;
        case 6:selected=hit.id;logoReady=bannerReady=false;if(auto s=ServerStore::getInstance().get(selected))enqueue(*s);break;
        case 7:save();break;
        case 8:form=false;confirm=false;pl::input::hideKeyboard();break;
        case 9:confirm=true;pl::input::hideKeyboard();break;
        case 10:focus=0;nameEdit.end(name);pl::input::showKeyboard();break;
        case 11:focus=1;addressEdit.end(address);pl::input::showKeyboard();break;
        case 12:if(!selected.empty())newForm(selected);break;
        case 13:++generations[editId];std::erase_if(tasks,[&](const auto& t){return t.server.id==editId;});ServerStore::getInstance().remove(editId);ServerStore::getInstance().save();statuses.erase(editId);selected.clear();logoReady=bannerReady=false;form=confirm=false;break;
        }
    }
    bool touch(const pl::input::TouchEvent& event) {
        if(!active||!homeAllowed())return false;std::lock_guard lock(mutex);if(!moduleEnabled||!open)return false;
        auto x=event.x/scale,y=event.y/scale;int action=event.action&255;
        if(action==0){pressed=-1;for(std::size_t i=0;i<hits.size();++i)if(hits[i].contains(x,y)){pressed=static_cast<int>(i);break;}}
        if(action==1){int index=pressed;pressed=-1;if(index>=0&&static_cast<std::size_t>(index)<hits.size()&&hits[index].contains(x,y)){auto hit=hits[index];act(hit);}}
        if(action==3)pressed=-1;return true;
    }
    bool key(const pl::input::KeyEvent& event) {
        if(!active||!homeAllowed())return false;std::lock_guard lock(mutex);if(!moduleEnabled||!open)return false;
        if(!event.isKeyDown)return true;
        if(event.keyCode==4){if(form){form=confirm=false;pressed=-1;pl::input::hideKeyboard();}else dismiss();return true;}
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
            if(mode==2){
                scale=std::max(.5f,std::min(display.width/800.f,display.height/480.f));viewWidth=display.width/scale;viewHeight=display.height/scale;
                auto rect=[&](float x,float y,float w,float h,std::uint32_t color){commands.push_back({.type=pl::modmenu::DrawCommandType::RectFilled,.x=x*scale,.y=y*scale,.w=w*scale,.h=h*scale,.color=color});};
                auto text=[&](float x,float y,std::string value,float size=16,std::uint32_t color=0xffffffff){commands.push_back({.type=pl::modmenu::DrawCommandType::Text,.x=x*scale,.y=(y+size)*scale,.color=color,.size=size*scale,.text=std::move(value)});};
                auto button=[&](float x,float y,float w,std::string label,int action,std::string id=std::string{}){rect(x,y,w,36,0xff424242);text(x+10,y+8,std::move(label),16);hits.push_back({x,y,w,36,action,std::move(id)});};
                auto image=[&](const char* id,float x,float y,float w,float h,int iw,int ih){float r=std::min(w/iw,h/ih);commands.push_back({.type=pl::modmenu::DrawCommandType::Image,.x=(x+(w-iw*r)/2)*scale,.y=(y+(h-ih*r)/2)*scale,.w=iw*r*scale,.h=ih*r*scale,.color=0xffffffff,.imageId=id});};
                rect(0,0,viewWidth,viewHeight,0xff1c201e);float left=20,right=viewWidth-20;
                text(left,16,"服务器列表",24);button(right-280,16,80,"刷新",1);button(right-190,16,90,"添加",2);button(right-90,16,90,"关闭",3);
                if(form){
                    float x=std::max(20.f,(viewWidth-560)/2);rect(x,65,560,310,0xff303632);text(x+20,75,confirm?"确认删除此服务器？":editId.empty()?"添加服务器":"编辑服务器",22);
                    if(confirm){text(x+20,125,fit(name,28));button(x+20,230,180,"确认删除",13);button(x+220,230,180,"取消",8);}
                    else {
                        text(x+20,112,"名称（可留空）",14);rect(x+20,135,520,38,focus==0?0xff486045:0xff454545);text(x+30,142,fit(name.empty()?"点击输入名称":name,28));hits.push_back({x+20,135,520,38,10,{}});
                        text(x+20,185,"地址：域名/IP:端口，默认 19132",14);rect(x+20,209,520,38,focus==1?0xff486045:0xff454545);text(x+30,216,fit(address.empty()?"点击输入 play.example.com:19132":address,40));hits.push_back({x+20,209,520,38,11,{}});
                        button(x+20,275,120,"保存",7);button(x+150,275,120,"取消",8);if(!editId.empty())button(x+400,275,140,"删除",9);
                        text(x+20,325,fit(notice,42),14,0xffffcb80);
                    }
                } else {
                    auto servers=ServerStore::getInstance().list();int rows=std::max(1,static_cast<int>((viewHeight-245)/64));int pages=std::max(1,(static_cast<int>(servers.size())+rows-1)/rows);page=std::clamp(page,0,pages-1);
                    if(servers.empty())text(left+20,100,"还没有服务器，点击右上角“添加”开始。",19);
                    for(int row=0;row<rows;++row){auto index=page*rows+row;if(index>=static_cast<int>(servers.size()))break;const auto& s=servers[index];float y=65+row*64.f;
                        rect(left,y,right-left,58,s.id==selected?0xff44633b:0xe03a403c);image(s.id==selected&&logoReady?"sle.selected.logo":"sle.default.logo",left+6,y+5,48,48,s.id==selected&&logoReady?logoWidth:64,s.id==selected&&logoReady?logoHeight:64);
                        auto status=statuses.find(s.id);text(left+66,y+4,fit(s.name.empty()?s.address:s.name,26),18);
                        text(left+66,y+32,fit(client::displayAddress(s.address,s.port),48),13,0xffcccccc);
                        std::string detail="未查询";if(!client::validServerEndpoint(s.address,s.port))detail="地址无效，请编辑";
                        else if(status!=statuses.end()){const auto& v=status->second;detail=v.pending?"查询中…":!v.error.empty()?"查询失败（可重试）":v.version+"  "+std::to_string(v.players)+"/"+std::to_string(v.capacity);}
                        text(right-230,y+18,fit(detail,22),14);hits.push_back({left,y,right-left,58,6,s.id});
                    }
                    float dy=viewHeight-170;rect(left,dy,right-left,110,0xd0252c27);
                    if(auto s=ServerStore::getInstance().get(selected)){
                        image(bannerReady?"sle.selected.banner":"sle.default.banner",left+8,dy+5,220,100,bannerReady?bannerWidth:960,bannerReady?bannerHeight:320);
                        std::string intro=s->introText;auto state=statuses.find(selected);if(intro.empty()&&state!=statuses.end()){if(state->second.visual)intro=state->second.visual->introText;else intro=state->second.message;}
                        std::istringstream lines(intro);std::string line;int row=0;while(row<4&&std::getline(lines,line)){text(left+245,dy+8+row*22,fit(line,34),15);++row;}
                    }else text(left+20,dy+35,"点选服务器查看图文资料；编辑可以修改或删除。",16);
                    button(left,viewHeight-48,90,"上一页",4);button(left+100,viewHeight-48,90,"下一页",5);text(left+210,viewHeight-39,std::to_string(page+1)+"/"+std::to_string(pages),16);button(right-130,viewHeight-48,130,"编辑所选",12);
                    if(!notice.empty())text(left+300,viewHeight-38,fit(notice,22),13,0xffcbdfb0);
                }
            }}
            if(mode==2||lastMode!=0||display.session!=0)submitHomeUiFrame(mode,commands,display.session);
            lastMode=mode;std::this_thread::sleep_for(50ms);
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
        moduleEnabled=config.enabled;plainHttp=config.allowPlainHttpFallback;pruneCache();
        Dl_info info{};if(!dladdr(reinterpret_cast<void*>(&setJavaVm),&info)||!dlopen(info.dli_fname,RTLD_NOW|RTLD_NODELETE))return false;
        __android_log_print(ANDROID_LOG_INFO,"ServerListEnhancer","Loaded Android ARM64 port; %zu servers; launcher 1.6.0 SDK 5d108da; restart game after updates",store.list().size());
        if(config.autoRefresh)refresh();return true;
    }
    bool enable(){
        active=true;
        pl::modmenu::ModuleInfo module;module.moduleId=Module;module.displayName="服务器列表增强";module.description="独立列表、触控编辑与服务器图文资料";module.defaultEnabled=moduleEnabled;module.hideInHudEditor=true;
        module.onToggle=[this](std::string_view,bool enabled){std::lock_guard lock(mutex);moduleEnabled=enabled;if(!enabled)dismiss();};
        if(!pl::modmenu::registerModule(module)){active=false;return false;}
        if(!initializeHomeUi(javaVm,modRoot,[this]{openFromHome();},
            [this](float x,float y,int action){pl::input::TouchEvent event{};event.x=x;event.y=y;event.action=action;return touch(event);},
            [this]{pl::input::KeyEvent event{};event.keyCode=4;event.isKeyDown=true;return key(event);})){
            pl::modmenu::unregisterModule(Module);active=false;return false;
        }
        if(!inputRegistered){pl::input::registerKeyCallback([this](const auto& e){return key(e);});inputRegistered=true;}
        for(int i=0;i<2;++i)workers.emplace_back([this]{worker();});renderer=std::thread([this]{render();});
        __android_log_print(ANDROID_LOG_INFO,"ServerListEnhancer","Enabled home-screen server entry and two workers");return true;
    }
    bool disable(){
        active=false;changed.notify_all();if(renderer.joinable())renderer.join();for(auto& t:workers)if(t.joinable())t.join();workers.clear();
        {std::lock_guard lock(mutex);open=form=false;hits.clear();tasks.clear();}
        pl::input::hideKeyboard();closeHomeUi();pl::modmenu::unregisterModule(Module);return ServerStore::getInstance().save();
    }
    bool unload(){return true;}
};
}
PL_REGISTER_MOD(sle::android::AndroidMod, sle::android::AndroidMod::instance());
