#pragma once

#include <functional>
#include <string>

namespace sle::client {

using JoinFeedback = std::function<void(bool submitted, std::string const& message)>;

void startJoinService();
void stopJoinService();
// Keep the DLL loaded while queued work or the game owns a callback.
bool canUnloadJoinService();

// Copies the endpoint and queues work on the client thread. No vanilla-list entry
// is created. Feedback reports dispatch or a connection error, never success in-world.
bool joinServer(std::string address, unsigned short port, std::string name, JoinFeedback feedback);

} // namespace sle::client
