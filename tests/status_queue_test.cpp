#include "../src-client/ServerStatus.cpp"
#include <cassert>
#include <atomic>
#include <iostream>
namespace sle::client {
struct ServerStatusTestAccess {
 static void run(){
  ServerStatus service;
  std::mutex m;std::condition_variable cv;
  int entered=0,active=0,peak=0,done=0;bool release=false;
  service.mQuery=[&](auto const&,int,auto,std::stop_token stop){
   {std::unique_lock lock(m);++entered;++active;peak=std::max(peak,active);cv.notify_all();cv.wait(lock,[&]{return release||stop.stop_requested();});--active;++done;cv.notify_all();}
   return parseStatus(R"({"name":"Test","players":2,"server_list_enhancer":{"schemaVersion":1,"introText":"prejoin","logo":{"type":"builtin","value":"default"}}})");
  };
  service.start();
  for(int i=0;i<12;++i)service.request("key"+std::to_string(i),"127.0.0.1",19132+i);
  {std::unique_lock lock(m);assert(cv.wait_for(lock,std::chrono::seconds(3),[&]{return entered==4;}));assert(peak==4);release=true;cv.notify_all();assert(cv.wait_for(lock,std::chrono::seconds(3),[&]{return done==12;}));}
  auto allReady=[&]{for(int i=0;i<12;++i){auto r=service.get("key"+std::to_string(i));if(!r||r->state!=ServerStatusState::Ready)return false;}return true;};
  auto finish=Clock::now()+std::chrono::seconds(3);while(!allReady()&&Clock::now()<finish)std::this_thread::yield();assert(allReady());
  service.stop();
  for(int i=0;i<12;++i){auto result=service.get("key"+std::to_string(i));assert(result&&result->ok&&result->visual&&result->visual->introText=="prejoin");}
  // A completion from a deleted/re-edited endpoint must not resurrect its key.
  service.mQuery=[&](auto const&,int,auto,std::stop_token stop){while(!stop.stop_requested())std::this_thread::yield();return parseStatus(R"({"name":"late"})");};
  service.start();service.request("delete","127.0.0.1",19132);service.invalidate("delete");service.stop();assert(!service.get("delete"));
  // Force refresh bypasses freshness; a restart loads visuals before networking.
  auto cache=std::filesystem::path("build/status-persistence.json");
  std::error_code ec;std::filesystem::remove(cache,ec);
  std::atomic<int> calls{};
  auto ready=[](ServerStatus& s,std::string const& key){auto end=Clock::now()+std::chrono::seconds(3);do{auto r=s.get(key);if(r&&(r->state==ServerStatusState::Ready||r->state==ServerStatusState::Failed))return r;std::this_thread::sleep_for(std::chrono::milliseconds(2));}while(Clock::now()<end);return std::optional<ServerStatusResult>{};};
  {ServerStatus saved;saved.setCachePath(cache);saved.mQuery=[&](auto const&,int,auto,std::stop_token){++calls;return parseStatus(R"({"name":"cached","server_list_enhancer":{"schemaVersion":1,"introText":"persisted"}})");};
   saved.start();saved.request("persistent","host",19132);assert(ready(saved,"persistent"));
   saved.request("persistent","host",19132);assert(saved.get("persistent")->state==ServerStatusState::Ready&&calls==1);
   saved.request("persistent","host",19132,true);assert(ready(saved,"persistent")&&calls==2);saved.stop();}
  {ServerStatus restored;restored.setCachePath(cache);restored.mQuery=[](auto const&,int,auto,std::stop_token){ServerStatusResult r;r.error="offline";return r;};
   restored.start();auto cached=restored.get("persistent");assert(cached&&cached->visual&&cached->visual->introText=="persisted");
   restored.request("persistent","host",19132);auto offline=ready(restored,"persistent");assert(offline&&!offline->ok&&offline->visual&&offline->name=="cached");
   restored.stop();restored.mQuery=[](auto const&,int,auto,std::stop_token){return parseStatus(R"({"name":"ok","server_list_enhancer":{"schemaVersion":1,"enabled":false}})");};
   restored.start();restored.request("persistent","host",19132,true);auto revoked=ready(restored,"persistent");assert(revoked&&revoked->ok&&!revoked->visual);restored.stop();}
  ServerStatus live;live.start({.allowSelfSignedNetherNet=true});live.request("live","127.0.0.1",19132);
  auto deadline=Clock::now()+std::chrono::seconds(8);std::optional<ServerStatusResult> result;
  do{result=live.get("live");if(result&&(result->state==ServerStatusState::Ready||result->state==ServerStatusState::Failed))break;std::this_thread::sleep_for(std::chrono::milliseconds(10));}while(Clock::now()<deadline);
  live.stop();assert(result&&result->ok&&result->visual);std::cout<<"real HTTP: "<<result->visual->introText<<"\n";
 }
};
}
int main(){using namespace sle::client;
 std::string body(65535,'x');assert(appendBody(body,"x",1));assert(!appendBody(body,"x",1));assert(body.size()==65536);
 auto unknown=parseStatus(R"({"name":"ok","server_list_enhancer":{"schemaVersion":9}})");assert(unknown.ok&&unknown.visualFieldPresent&&!unknown.visual);
 auto disabled=parseStatus(R"({"name":"ok","server_list_enhancer":{"schemaVersion":1,"enabled":false}})");assert(disabled.ok&&disabled.visual&&!disabled.visual->enabled);
 ServerStatusTestAccess::run();
 std::cout<<"12-entry queue/4 workers, invalidation, strict response cap, schema handling, live prejoin: PASS\n";
}
