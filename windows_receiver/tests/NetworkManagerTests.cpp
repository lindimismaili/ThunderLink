#include "Network/NetworkManager.h"
#include "Network/TcpTransport.h"
#include "App/Validation.h"
#include "Utils/Logger.h"
#include <nlohmann/json.hpp>
#include <cassert>
#include <thread>
using namespace thunderlink;
using json=nlohmann::json;
std::atomic<bool> running{true};
uint16_t FreePort(){
    auto s=tlnk_create_tcp_socket();auto addr=tlnk_make_addr("127.0.0.1",0);
    assert(bind(s,reinterpret_cast<sockaddr*>(&addr),sizeof(addr))==0);
    SocketLength length=sizeof(addr);assert(getsockname(s,reinterpret_cast<sockaddr*>(&addr),&length)==0);
    auto port=ntohs(addr.sin_port);tlnk_close_socket(s);return port;
}
tlnk_socket_t Connect(uint16_t port){
    auto s=tlnk_create_tcp_socket();assert(s!=TLNK_INVALID_SOCKET);
    auto addr=tlnk_make_addr("127.0.0.1",port);assert(ConnectWithDeadline(s,addr,running));return s;
}
json Receive(tlnk_socket_t s){
    FramedReader reader;auto end=NetClock::now()+std::chrono::seconds(2);
    while(NetClock::now()<end){std::string value;auto result=reader.Poll(s,value);
        if(result==ReadStatus::Message)return json::parse(value);
        assert(result==ReadStatus::Idle);
    }
    assert(false && "message timeout");return {};
}
void Await(const std::function<bool()>& predicate){
    auto end=NetClock::now()+std::chrono::seconds(2);
    while(!predicate()&&NetClock::now()<end)std::this_thread::sleep_for(std::chrono::milliseconds(2));
    assert(predicate());
}
json Handshake(tlnk_socket_t s){
    // Fragment both prefix and body to exercise the production incremental reader.
    auto hello=FrameJSON(json{{"type",TLNK_CTRL_HELLO},{"version",TLNK_VERSION}}.dump());
    for(auto byte:hello){assert(send(s,reinterpret_cast<char*>(&byte),1,0)==1);}
    auto config=Receive(s);assert(config["type"]==TLNK_CTRL_CONFIGURE);
    assert(ParseSession(config["session_id"].get<std::string>())!=0);
    config["type"]=TLNK_CTRL_START;config["width"]=1920;config["height"]=1080;
    assert(SendFramed(s,config.dump(),[]{return true;}));return config;
}
int main(){
    assert(tlnk_init_sockets()==0);Logger::SetLevel(LogLevel::Fatal);
    GetConfig().bindIP="127.0.0.1";GetConfig().controlPort=FreePort();
    std::atomic<int64_t> advanceMs{0};
    NetworkManager server([&]{return NetClock::now()+std::chrono::milliseconds(advanceMs.load());});
    std::atomic<int> connected{0},disconnected{0};
    server.SetOnConnected([&](const ConnectionInfo& info){assert(info.sessionId);++connected;});
    server.SetOnDisconnected([&]{++disconnected;});assert(server.Start());
    auto first=Connect(GetConfig().controlPort);auto config=Handshake(first);Await([&]{return server.IsConnected();});
    tlnk_close_socket(first);Await([&]{return disconnected==1 && !server.IsConnected();});
    auto second=Connect(GetConfig().controlPort);auto secondConfig=Handshake(second);
    Await([&]{return connected==2;});assert(config["session_id"]!=secondConfig["session_id"]);
    // Drain the requested IDR before testing runtime configuration.
    auto keyframe=Receive(second);assert(keyframe["type"]==TLNK_CTRL_REQUEST_KEYFRAME);
    assert(server.RequestConfigure(0,720,60));auto changed=Receive(second);
    assert(changed["type"]==TLNK_CTRL_CONFIGURE && changed["session_id"]!=secondConfig["session_id"]);
    changed["type"]=TLNK_CTRL_START;changed["width"]=1280;
    assert(SendFramed(second,changed.dump(),[]{return true;}));Await([&]{return connected==3;});
    tlnk_close_socket(second);Await([&]{return disconnected==3;});
    for (int oldVersion : {2, 3}) {
        auto incompatible=Connect(GetConfig().controlPort);
        assert(SendFramed(incompatible,json{{"type",1},{"version",oldVersion}}.dump(),[]{return true;}));
        assert(Receive(incompatible)["type"]==TLNK_CTRL_BYE);tlnk_close_socket(incompatible);
    }
    auto partial=Connect(GetConfig().controlPort);uint8_t byte=0;assert(send(partial,reinterpret_cast<char*>(&byte),1,0)==1);
    tlnk_close_socket(partial);
    auto finalClient=Connect(GetConfig().controlPort);Handshake(finalClient);Await([&]{return connected==4;});
    advanceMs=TLNK_PEER_TIMEOUT_MS+1;
    Await([&]{return disconnected==4 && !server.IsConnected();});
    assert(server.GetError()=="Peer stopped responding");
    tlnk_close_socket(finalClient);
    auto noStart=Connect(GetConfig().controlPort);
    assert(SendFramed(noStart,json{{"type",TLNK_CTRL_HELLO},{"version",TLNK_VERSION}}.dump(),[]{return true;}));
    assert(Receive(noStart)["type"]==TLNK_CTRL_CONFIGURE);
    advanceMs.fetch_add(TLNK_HANDSHAKE_TIMEOUT_MS+1);
    Await([&]{return server.GetError()=="Stream handshake timed out";});
    tlnk_close_socket(noStart);
    finalClient=Connect(GetConfig().controlPort);Handshake(finalClient);Await([&]{return connected==5;});
    auto start=NetClock::now();server.Stop();assert(NetClock::now()-start<std::chrono::milliseconds(500));
    tlnk_close_socket(finalClient);tlnk_cleanup_sockets();
}
