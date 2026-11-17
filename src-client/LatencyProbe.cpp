#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <bcrypt.h>
#include "LatencyProbe.h"
#include "AddressParser.h"
#include "common/UdpProbeProtocol.h"
#include <algorithm>
#include <memory>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>
#include <cstring>
namespace sle::client {
namespace {
using Clock=std::chrono::steady_clock;
LatencyResult measure(std::string const& host,int port,bool echo,std::atomic_bool& stopping){
 LatencyResult result;result.error="UDP 未响应";
 ADDRINFOEXW hints{};hints.ai_socktype=SOCK_DGRAM;hints.ai_family=AF_UNSPEC;hints.ai_protocol=IPPROTO_UDP;
 std::wstring wideHost(host.begin(),host.end());auto service=std::to_wstring(port);
 ADDRINFOEXW* addresses{};OVERLAPPED lookup{};lookup.hEvent=CreateEventW(nullptr,TRUE,FALSE,nullptr);HANDLE cancel{};
 if(!lookup.hEvent){result.error="DNS 初始化失败";return result;}
 timeval timeout{1,500000};
 int status=GetAddrInfoExW(wideHost.c_str(),service.c_str(),NS_DNS,nullptr,&hints,&addresses,&timeout,&lookup,nullptr,&cancel);
 if(status==WSA_IO_PENDING){auto end=Clock::now()+std::chrono::milliseconds(1500);
  while(!stopping.load()&&Clock::now()<end&&WaitForSingleObject(lookup.hEvent,50)==WAIT_TIMEOUT){}
  if(WaitForSingleObject(lookup.hEvent,0)!=WAIT_OBJECT_0){GetAddrInfoExCancel(&cancel);WaitForSingleObject(lookup.hEvent,INFINITE);}
  status=GetAddrInfoExOverlappedResult(&lookup);
 }
 CloseHandle(lookup.hEvent);
 std::unique_ptr<ADDRINFOEXW,decltype(&FreeAddrInfoExW)> owned(addresses,FreeAddrInfoExW);
 if(status||stopping.load()){result.error="DNS 查询失败或超时";return result;}
 SOCKET socket=INVALID_SOCKET;
 for(auto* p=addresses;p;p=p->ai_next){socket=::socket(p->ai_family,SOCK_DGRAM,IPPROTO_UDP);if(socket!=INVALID_SOCKET&&connect(socket,p->ai_addr,static_cast<int>(p->ai_addrlen))==0)break;if(socket!=INVALID_SOCKET)closesocket(socket);socket=INVALID_SOCKET;}
 if(socket==INVALID_SOCKET)return result;
 struct Close{SOCKET s;~Close(){closesocket(s);}} close{socket};u_long nonblock=1;ioctlsocket(socket,FIONBIO,&nonblock);
 std::vector<int> samples;
 constexpr unsigned char magic[]{0,255,255,0,254,254,254,254,253,253,253,253,18,52,86,120};
 for(int attempt=0;attempt<3&&!stopping.load();++attempt){
  unsigned char packet[33]{};int size=echo?32:33;
  if(BCryptGenRandom(nullptr,packet,size,BCRYPT_USE_SYSTEM_PREFERRED_RNG)<0)continue;
  if(echo)std::copy(udp::Request.begin(),udp::Request.end(),packet);
  else{packet[0]=1;std::copy(std::begin(magic),std::end(magic),packet+9);}
  auto start=Clock::now();if(send(socket,reinterpret_cast<char*>(packet),size,0)!=size)continue;
  auto end=start+std::chrono::milliseconds(1500);
  while(!stopping.load()&&Clock::now()<end){WSAPOLLFD fd{socket,POLLRDNORM,0};if(WSAPoll(&fd,1,50)<=0)continue;unsigned char reply[2048]{};int n=recv(socket,reinterpret_cast<char*>(reply),sizeof(reply),0);if(n<0)continue;
   bool valid=echo?n==32&&std::equal(udp::Response.begin(),udp::Response.end(),reply)&&!std::memcmp(packet+8,reply+8,24):n>=35&&reply[0]==0x1c&&!std::memcmp(packet+1,reply+1,8)&&!std::memcmp(reply+17,magic,16);
   if(valid){samples.push_back(static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now()-start).count()));break;}
  }
 }
 if(!samples.empty()){std::sort(samples.begin(),samples.end());result.ms=samples[samples.size()/2];result.error.clear();}return result;
}
}
struct LatencyProbe::Impl {
 struct Entry{LatencyResult result;std::string host;int port{};bool echo{};Clock::time_point next{};std::uint64_t generation{};};
 struct Job{std::string key,host;int port{};bool echo{};std::uint64_t generation{};};
 mutable std::mutex mutex;std::mutex lifecycle;std::condition_variable cv;std::unordered_map<std::string,Entry> entries;std::deque<Job> queue;std::vector<std::thread> workers;std::atomic_bool stopping{true};std::uint64_t generation{};
 void run(){for(;;){Job job;{std::unique_lock lock(mutex);cv.wait(lock,[&]{return stopping.load()||!queue.empty();});if(stopping.load())return;job=std::move(queue.front());queue.pop_front();}
 auto result=measure(job.host,job.port,job.echo,stopping);
 std::lock_guard lock(mutex);auto it=entries.find(job.key);if(it!=entries.end()&&it->second.generation==job.generation){it->second.result=result;it->second.next=Clock::now()+std::chrono::seconds(15);}
 }}
};
LatencyProbe::LatencyProbe():impl(std::make_unique<Impl>()){}
LatencyProbe::~LatencyProbe(){stop();}LatencyProbe& LatencyProbe::getInstance(){static LatencyProbe value;return value;}
void LatencyProbe::start(){std::lock_guard life(impl->lifecycle);if(!impl->stopping.exchange(false))return;WSADATA data{};if(WSAStartup(MAKEWORD(2,2),&data)){impl->stopping=true;return;}for(int i=0;i<2;++i)impl->workers.emplace_back([this]{impl->run();});}
void LatencyProbe::stop(){std::lock_guard life(impl->lifecycle);if(impl->stopping.exchange(true))return;impl->cv.notify_all();for(auto& t:impl->workers)if(t.joinable())t.join();impl->workers.clear();{std::lock_guard lock(impl->mutex);impl->queue.clear();impl->entries.clear();}WSACleanup();}
void LatencyProbe::request(std::string const& key,std::string const& host,int gamePort,int echoPort,bool force){
 if(impl->stopping.load()||!validServerEndpoint(host,gamePort))return;int port=echoPort>0?echoPort:gamePort;if(port<1||port>65535)return;
 std::lock_guard lock(impl->mutex);if(!impl->entries.contains(key)&&impl->entries.size()>=512)return;
 auto& entry=impl->entries[key];bool changed=entry.host!=host||entry.port!=port||entry.echo!=(echoPort>0);
 if(!changed&&(entry.result.pending||(!force&&Clock::now()<entry.next)))return;
 entry.host=host;entry.port=port;entry.echo=echoPort>0;entry.generation=++impl->generation;entry.result.pending=true;
 impl->queue.push_back({key,host,port,echoPort>0,entry.generation});impl->cv.notify_one();
}
void LatencyProbe::pause(){std::lock_guard lock(impl->mutex);impl->queue.clear();for(auto& [key,e]:impl->entries)if(e.result.pending){e.result.pending=false;e.generation=++impl->generation;e.next={};}}
std::optional<LatencyResult> LatencyProbe::get(std::string const& key)const{std::lock_guard lock(impl->mutex);auto it=impl->entries.find(key);return it==impl->entries.end()?std::nullopt:std::optional(it->second.result);}
}
