#pragma once
#include <memory>
#include <optional>
#include <string>
namespace sle::client {
struct LatencyResult {int ms{-1};bool pending{};std::string error;};
class LatencyProbe {
public:
 static LatencyProbe& getInstance();
 void start();void stop();void pause();
 void request(std::string const& key,std::string const& host,int gamePort,int echoPort=0,bool force=false);
 std::optional<LatencyResult> get(std::string const& key) const;
 ~LatencyProbe();
private:
 LatencyProbe();struct Impl;std::unique_ptr<Impl> impl;
};
}
