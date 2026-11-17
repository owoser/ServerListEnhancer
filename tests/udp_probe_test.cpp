#include "../src-client/LatencyProbe.cpp"
#include "../src-server/UdpEcho.h"
#include <cassert>
#include <iostream>
int main(){using namespace sle;using namespace sle::client;
 for(auto [ms,color]:std::vector<std::pair<int,int>>{{-1,0},{0,1},{60,1},{61,2},{100,2},{101,3},{999,3},{1000,3}})assert(udp::band(ms)==color);
 assert(server::startUdpEcho(29135));WSADATA w;WSAStartup(MAKEWORD(2,2),&w);std::atomic_bool stop{false};
 auto r=measure("127.0.0.1",29135,true,stop);assert(r.ms>=0);std::cout<<"Real UDP echo RTT: "<<r.ms<<" ms\n";
 auto miss=measure("127.0.0.1",29136,true,stop);assert(miss.ms<0);
 server::stopUdpEcho();assert(server::udpEchoPort()==0);
 // A real >1000ms reply must be measured, not clipped by the sample window.
 SOCKET slow=socket(AF_INET,SOCK_DGRAM,IPPROTO_UDP);assert(slow!=INVALID_SOCKET);
 sockaddr_in local{};local.sin_family=AF_INET;local.sin_addr.s_addr=htonl(INADDR_LOOPBACK);local.sin_port=htons(29137);
 assert(bind(slow,reinterpret_cast<sockaddr*>(&local),sizeof(local))==0);
 DWORD receiveTimeout=5000;setsockopt(slow,SOL_SOCKET,SO_RCVTIMEO,reinterpret_cast<char*>(&receiveTimeout),sizeof(receiveTimeout));
 std::thread delayed([slow]{for(int i=0;i<3;++i){unsigned char data[64]{};sockaddr_in from{};int len=sizeof(from);int n=recvfrom(slow,reinterpret_cast<char*>(data),sizeof(data),0,reinterpret_cast<sockaddr*>(&from),&len);assert(n==32);std::this_thread::sleep_for(std::chrono::milliseconds(1100));std::copy(udp::Response.begin(),udp::Response.end(),data);assert(sendto(slow,reinterpret_cast<char*>(data),n,0,reinterpret_cast<sockaddr*>(&from),len)==n);}closesocket(slow);});
 auto high=measure("127.0.0.1",29137,true,stop);delayed.join();assert(high.ms>=1000&&high.ms<1500);std::cout<<"Delayed UDP RTT: "<<high.ms<<" ms\n";
 WSACleanup();
 std::cout<<"UDP nonce echo, missing reply, color boundaries, lifecycle: PASS\n";
}
