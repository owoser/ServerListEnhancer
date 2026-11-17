#pragma once
// Android 端的 UDP 时延探测：协议、重试、缓存语义与 Windows 端 src-client/LatencyProbe.cpp 一致，
// 但改用 POSIX 套接字；探测全部在后台线程执行，渲染线程只读取结果，不会阻塞界面。
#include "AddressParser.h"
#include "common/UdpProbeProtocol.h"
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <optional>
#include <random>
#include <string>
#include <thread>
#include <vector>
namespace sle::android {
struct LatencyResult {int ms{-1};bool pending{};std::string error;};
namespace detail {
using LatencyClock=std::chrono::steady_clock;
// 一次测量：最多 3 次尝试，每次 1500ms 超时，成功样本取中位数。
inline LatencyResult measureLatency(std::string const& host,int port,bool echo,std::atomic<bool>& stopping) {
    LatencyResult result;result.error="UDP 未响应";
    addrinfo hints{};hints.ai_socktype=SOCK_DGRAM;hints.ai_family=AF_UNSPEC;hints.ai_protocol=IPPROTO_UDP;
    addrinfo* addresses{};
    const auto service=std::to_string(port);
    if(getaddrinfo(host.c_str(),service.c_str(),&hints,&addresses)!=0||stopping.load()){result.error="DNS 查询失败或超时";return result;}
    int socket{-1};
    for(auto* address=addresses;address;address=address->ai_next) {
        socket=::socket(address->ai_family,SOCK_DGRAM,IPPROTO_UDP);
        if(socket<0)continue;
        if(::connect(socket,address->ai_addr,static_cast<socklen_t>(address->ai_addrlen))==0)break;
        ::close(socket);socket=-1;
    }
    freeaddrinfo(addresses);
    if(socket<0)return result;
    struct Close {int fd;~Close(){if(fd>=0)::close(fd);}} close{socket};
    std::vector<int> samples;
    const unsigned char magic[]{0,255,255,0,254,254,254,254,253,253,253,253,18,52,86,120};
    std::mt19937_64 random{std::random_device{}()};
    for(int attempt=0;attempt<3&&!stopping.load();++attempt) {
        unsigned char packet[33]{};const int size=echo?32:33;
        for(int i=0;i<size;++i)packet[i]=static_cast<unsigned char>(random()&255U);
        if(echo)std::copy(sle::udp::Request.begin(),sle::udp::Request.end(),packet);
        else{packet[0]=1;std::copy(std::begin(magic),std::end(magic),packet+9);}
        const auto start=LatencyClock::now();
        if(::send(socket,packet,static_cast<std::size_t>(size),0)!=size)continue;
        const auto end=start+std::chrono::milliseconds(1500);
        while(!stopping.load()&&LatencyClock::now()<end) {
            pollfd descriptor{socket,POLLIN,0};
            if(::poll(&descriptor,1,50)<=0)continue;
            unsigned char reply[2048]{};
            const auto count=::recv(socket,reply,sizeof(reply),0);
            if(count<0)continue;
            bool valid{};
            if(echo)valid=count==32&&std::equal(sle::udp::Response.begin(),sle::udp::Response.end(),reply)&&!std::memcmp(packet+8,reply+8,24);
            else valid=count>=35&&reply[0]==0x1c&&!std::memcmp(packet+1,reply+1,8)&&!std::memcmp(reply+17,magic,16);
            if(!valid)continue;
            samples.push_back(static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(LatencyClock::now()-start).count()));
            break;
        }
    }
    if(!samples.empty()){std::sort(samples.begin(),samples.end());result.ms=samples[samples.size()/2];result.error.clear();}
    return result;
}
}
class LatencyProbe {
public:
    LatencyProbe()=default;
    ~LatencyProbe(){stop();}
    LatencyProbe(const LatencyProbe&)=delete;
    LatencyProbe& operator=(const LatencyProbe&)=delete;
    void start() {
        std::lock_guard lifecycleLock(lifecycle);
        if(!stopping.exchange(false))return;
        for(int i=0;i<2;++i)workers.emplace_back([this]{run();});
    }
    void stop() {
        std::lock_guard lifecycleLock(lifecycle);
        if(stopping.exchange(true))return;
        cv.notify_all();
        for(auto& worker:workers)if(worker.joinable())worker.join();
        workers.clear();
        std::lock_guard lock(mutex);queue.clear();entries.clear();
    }
    // 列表关闭或翻页时调用：丢弃排队中的任务，未完成的结果不再显示为“查询中”。
    void pause() {
        std::lock_guard lock(mutex);queue.clear();
        for(auto& [key,entry]:entries)if(entry.result.pending){entry.result.pending=false;entry.generation=++generation;entry.next={};}
    }
    void request(std::string const& key,std::string const& host,int gamePort,int echoPort=0,bool force=false) {
        if(stopping.load()||key.empty()||!client::validServerEndpoint(host,gamePort))return;
        const int port=echoPort>0?echoPort:gamePort;
        if(port<1||port>65535)return;
        const bool echo=echoPort>0;
        std::lock_guard lock(mutex);
        if(!entries.contains(key)&&entries.size()>=512)return;
        auto& entry=entries[key];
        const bool changed=entry.host!=host||entry.port!=port||entry.echo!=echo;
        if(!changed&&(entry.result.pending||(!force&&detail::LatencyClock::now()<entry.next)))return;
        entry.host=host;entry.port=port;entry.echo=echo;entry.generation=++generation;entry.result.pending=true;
        queue.push_back({key,host,port,echo,entry.generation});
        cv.notify_one();
    }
    std::optional<LatencyResult> get(std::string const& key) const {
        std::lock_guard lock(mutex);
        auto it=entries.find(key);
        return it==entries.end()?std::nullopt:std::optional<LatencyResult>(it->second.result);
    }
private:
    struct Entry {LatencyResult result;std::string host;int port{};bool echo{};detail::LatencyClock::time_point next{};std::uint64_t generation{};};
    struct Job {std::string key,host;int port{};bool echo{};std::uint64_t generation{};};
    void run() {
        for(;;) {
            Job job;
            {std::unique_lock lock(mutex);cv.wait(lock,[&]{return stopping.load()||!queue.empty();});if(stopping.load())return;job=std::move(queue.front());queue.pop_front();}
            const auto result=detail::measureLatency(job.host,job.port,job.echo,stopping);
            std::lock_guard lock(mutex);
            auto it=entries.find(job.key);
            if(it!=entries.end()&&it->second.generation==job.generation){it->second.result=result;it->second.next=detail::LatencyClock::now()+std::chrono::seconds(15);}
        }
    }
    mutable std::mutex mutex;
    std::mutex lifecycle;
    std::condition_variable cv;
    std::map<std::string,Entry> entries;
    std::deque<Job> queue;
    std::vector<std::thread> workers;
    std::atomic<bool> stopping{true};
    std::uint64_t generation{};
};
}
