#pragma once
#include <algorithm>
#include <charconv>
#include <cctype>
#include <string>
#include <string_view>
namespace sle::client {
struct ParsedAddress { std::string host,error; int port{19132}; explicit operator bool()const{return error.empty()&&!host.empty();} };
inline bool validIpv4(std::string_view host){
 int parts=0;while(!host.empty()){auto dot=host.find('.');auto part=host.substr(0,dot);unsigned n{};auto [end,ec]=std::from_chars(part.data(),part.data()+part.size(),n);if(part.empty()||part.size()>3||(part.size()>1&&part.front()=='0')||ec!=std::errc{}||end!=part.data()+part.size()||n>255)return false;++parts;if(dot==host.npos)break;host.remove_prefix(dot+1);if(host.empty())return false;}return parts==4;
}
inline bool validIpv6(std::string_view host){
 if(host.empty()||host.find(":::")!=host.npos||(host.front()==':'&&!host.starts_with("::"))||(host.back()==':'&&!host.ends_with("::")))return false;
 auto compressed=host.find("::");if(compressed!=host.npos&&host.find("::",compressed+2)!=host.npos)return false;
 if(compressed==host.npos&&(host.front()==':'||host.back()==':'))return false;
 unsigned groups{};std::size_t pos{};
 while(pos<host.size()){if(host[pos]==':'){++pos;continue;}auto end=host.find(':',pos);if(end==host.npos)end=host.size();auto group=host.substr(pos,end-pos);
  if(group.find('.')!=group.npos){if(end!=host.size()||!validIpv4(group))return false;groups+=2;}
  else{if(group.empty()||group.size()>4||group.find_first_not_of("0123456789abcdefABCDEF")!=group.npos)return false;++groups;}
  pos=end;
 }
 return compressed==host.npos?groups==8:groups<8;
}
inline ParsedAddress parseServerAddress(std::string_view raw){
 ParsedAddress r;while(!raw.empty()&&std::isspace(static_cast<unsigned char>(raw.front())))raw.remove_prefix(1);while(!raw.empty()&&std::isspace(static_cast<unsigned char>(raw.back())))raw.remove_suffix(1);
 auto fail=[&](char const* e){r.error=e;return r;};
 if(raw.empty())return fail("请输入服务器地址");
 if(raw.size()>255||raw.find_first_of("/\\?#@")!=raw.npos||std::any_of(raw.begin(),raw.end(),[](unsigned char c){return c<=32||c>=127;}))return fail("地址不能包含协议、路径、空格或非ASCII字符");
 std::string_view host=raw,port;bool bracket=raw.front()=='[';
 if(bracket){auto close=raw.find(']');if(close==raw.npos)return fail("IPv6 缺少右括号");host=raw.substr(1,close-1);if(close+1<raw.size()){if(raw[close+1]!=':')return fail("IPv6 端口格式错误");port=raw.substr(close+2);if(port.empty())return fail("请输入端口");}}
 else{auto colon=raw.find(':');if(colon!=raw.npos){if(raw.find(':',colon+1)!=raw.npos)return fail("IPv6 请使用 [地址]:端口");host=raw.substr(0,colon);port=raw.substr(colon+1);if(port.empty())return fail("请输入端口");}}
 if(host.empty())return fail("主机名不能为空");
 if(!port.empty()){int n{};auto [end,ec]=std::from_chars(port.data(),port.data()+port.size(),n);if(ec!=std::errc{}||end!=port.data()+port.size())return fail("端口只能包含数字");if(n<1||n>65535)return fail("端口范围为 1–65535");r.port=n;}
 if(bracket){if(!validIpv6(host))return fail("IPv6 地址格式错误");}
 else{
  if(host.find_first_not_of("0123456789.")==host.npos){if(!validIpv4(host))return fail("IPv4 地址格式错误");}
  else{
   std::string lower(host);std::transform(lower.begin(),lower.end(),lower.begin(),[](unsigned char c){return static_cast<char>(std::tolower(c));});
   if(lower!="localhost"){
    if(host.size()>253||host.find('.')==host.npos)return fail("域名需为完整形式，如 play.example.com；本机可用 localhost");
    auto tld=host.substr(host.rfind('.')+1);
    bool letters=tld.size()>=2&&std::all_of(tld.begin(),tld.end(),[](unsigned char c){return (c>='a'&&c<='z')||(c>='A'&&c<='Z');});
    if(!letters&&!std::string_view(lower).substr(lower.rfind('.')+1).starts_with("xn--"))return fail("域名最后一级需为字母或规范 Punycode");
   }
   std::size_t pos{};while(pos<host.size()){auto end=host.find('.',pos);if(end==host.npos)end=host.size();auto label=host.substr(pos,end-pos);if(label.empty()||label.size()>63||label.front()=='-'||label.back()=='-'||label.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-")!=label.npos)return fail("主机名格式错误");pos=end+1;}if(host.back()=='.')return fail("主机名格式错误");}
 }
 r.host=std::string(host);return r;
}
inline std::string displayAddress(std::string const& host,int port){return (host.find(':')!=std::string::npos?"["+host+"]":host)+":"+std::to_string(port);}
inline bool validServerEndpoint(std::string const& host,int port){auto parsed=parseServerAddress(displayAddress(host,port));return parsed&&parsed.host==host&&parsed.port==port;}
}
