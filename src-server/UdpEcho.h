#pragma once
namespace sle::server {
bool startUdpEcho(int port);
void stopUdpEcho();
int udpEchoPort();
}
