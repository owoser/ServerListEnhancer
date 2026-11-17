#include "Discovery.h"
#include "ServerStore.h"
#include "TextEdit.h"
#include <cstdlib>
#include <iostream>
static void check(bool value,const char* label){if(!value){std::cerr<<label<<" failed\n";std::exit(1);}}
int main(){
    using namespace sle::android;
    Status s;s.visual=sle::DiscoveryVisual{};s.visual->introText="cached";
    check(!parseDiscovery(std::string(MaxJsonBytes+1,' '),s),"JSON limit");
    check(!parseDiscovery("[]",s),"object required");
    check(parseDiscovery(R"({"name":"测试服务器","version":"1.26.60","players":2,"maxPlayers":10})",s),"base discovery");
    check(s.title=="测试服务器"&&s.players==2&&s.capacity==10,"native values");
    check(s.visual&&s.visual->introText=="cached","missing extension keeps old metadata");
    check(parseDiscovery(R"({"server_list_enhancer":{"schemaVersion":8,"introText":"bad"}})",s)&&s.visual->introText=="cached","unknown schema");
    check(parseDiscovery(R"({"server_list_enhancer":{"schemaVersion":1,"enabled":false}})",s)&&!s.visual,"explicit disable");
    check(parseDiscovery(R"({"server_list_enhancer":{"schemaVersion":1,"logo":{"type":"url","value":"file:///etc/passwd"},"introText":"简介"}})",s)&&s.visual&&s.visual->logo.type=="builtin","image URL scheme");
    auto tooLong=nlohmann::json{{"server_list_enhancer",{{"schemaVersion",1},{"introText",std::string(1025,'x')}}}};
    check(parseDiscovery(tooLong.dump(),s)&&s.visual->introText.empty(),"intro byte bound");
    check(sle::client::parseServerAddress("[2001:db8::1]:19132").port==19132,"IPv6");
    check(!sle::client::parseServerAddress("999.0.0.1:19132"),"IPv4 rejection");
    check(!sle::client::parseServerAddress("host:999999"),"bad endpoint");
    std::string text="中文";sle::client::TextEdit edit;edit.end(text);edit.backspace(text);check(text=="中","UTF8 deletion");
    s.endpoint="first.example:19132";s.visual=sle::DiscoveryVisual{};s.visual->introText="first";
    bindEndpoint(s,"first.example:19132");check(s.visual.has_value(),"same endpoint keeps stale cache");
    bindEndpoint(s,"second.example:19132");check(!s.visual&&s.title.empty()&&s.error.empty(),"edited endpoint clears old metadata");
    std::cout<<"Android discovery bounds, schema fallback, endpoint isolation and UTF8: PASS\n";
}
