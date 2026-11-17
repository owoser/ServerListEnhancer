#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <cstring>
#include "UdpEcho.h"
#include "common/UdpProbeProtocol.h"
namespace sle::server {
namespace {
std::atomic_bool stopping{true};std::atomic_int activePort{};std::thread worker;std::mutex lifecycle;bool winsock{};
}
bool startUdpEcho(int port){
 stopUdpEcho();if(port==0)return true;if(port<1024||port>65535||port==19132||port==19133)return false;
 std::lock_guard lock(lifecycle);WSADATA data{};if(WSAStartup(MAKEWORD(2,2),&data))return false;
 SOCKET s=socket(AF_INET6,SOCK_DGRAM,IPPROTO_UDP);if(s==INVALID_SOCKET){WSACleanup();return false;}
 DWORD no=0;setsockopt(s,IPPROTO_IPV6,IPV6_V6ONLY,reinterpret_cast<char*>(&no),sizeof(no));
 sockaddr_in6 address{};address.sin6_family=AF_INET6;address.sin6_port=htons(static_cast<u_short>(port));address.sin6_addr=in6addr_any;
 if(bind(s,reinterpret_cast<sockaddr*>(&address),sizeof(address))){closesocket(s);WSACleanup();return false;}
 u_long nb=1;ioctlsocket(s,FIONBIO,&nb);stopping=false;activePort=port;winsock=true;
 worker=std::thread([s]{
  using Clock=std::chrono::steady_clock;auto window=Clock::now();unsigned total{};std::unordered_map<std::string,unsigned> counts;
  while(!stopping.load()){
   WSAPOLLFD fd{s,POLLRDNORM,0};if(WSAPoll(&fd,1,100)<=0)continue;
   unsigned char packet[64]{};sockaddr_in6 source{};int length=sizeof(source);int n=recvfrom(s,reinterpret_cast<char*>(packet),sizeof(packet),0,reinterpret_cast<sockaddr*>(&source),&length);
   if(n!=udp::PacketSize||!std::equal(udp::Request.begin(),udp::Request.end(),packet))continue;
   auto now=Clock::now();if(now-window>=std::chrono::seconds(1)){counts.clear();total=0;window=now;}
   if(total>=200)continue;std::string key(reinterpret_cast<char*>(&source.sin6_addr),sizeof(source.sin6_addr));
   if(!counts.contains(key)&&counts.size()>=256)continue;if(++counts[key]>10)continue;++total;
   std::copy(udp::Response.begin(),udp::Response.end(),packet);sendto(s,reinterpret_cast<char*>(packet),udp::PacketSize,0,reinterpret_cast<sockaddr*>(&source),length);
  }
  closesocket(s);
 });return true;
}
void stopUdpEcho(){std::lock_guard lock(lifecycle);stopping=true;if(worker.joinable())worker.join();activePort=0;if(winsock){WSACleanup();winsock=false;}}
int udpEchoPort(){return activePort.load();}
}
