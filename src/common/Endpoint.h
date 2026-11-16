#pragma once
#include <algorithm>
#include <cctype>
#include <string>
#include <string_view>

namespace sle {
inline std::string endpointKey(std::string_view address, int port) {
    std::string host(address);
    while (!host.empty() && std::isspace(static_cast<unsigned char>(host.front()))) host.erase(host.begin());
    while (!host.empty() && std::isspace(static_cast<unsigned char>(host.back()))) host.pop_back();
    if (host.size()>1 && host.front()=='[' && host.back()==']') host=host.substr(1,host.size()-2);
    std::transform(host.begin(),host.end(),host.begin(),[](unsigned char c){return static_cast<char>(std::tolower(c));});
    if (host.find(':')!=std::string::npos) host="["+host+"]";
    return host+":"+std::to_string(port);
}
}
