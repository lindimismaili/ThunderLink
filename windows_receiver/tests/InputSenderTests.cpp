#include "Network/InputSender.h"
#include "Network/TcpTransport.h"
#include "App/Config.h"
#include "input_event.h"
#include <nlohmann/json.hpp>
#include <cassert>
#include <thread>
using namespace thunderlink;
using json=nlohmann::json;
namespace {
void Await(const std::function<bool()>& test){
    auto end=NetClock::now()+std::chrono::seconds(3);
    bool passed=test();
    while(!passed&&NetClock::now()<end){std::this_thread::sleep_for(std::chrono::milliseconds(1));passed=test();}
    assert(passed);
}
tlnk_socket_t Accept(tlnk_socket_t listener){
    tlnk_socket_t client=TLNK_INVALID_SOCKET;
    Await([&]{if(SocketReady(listener,false,0)<=0)return false;client=accept(listener,nullptr,nullptr);return client!=TLNK_INVALID_SOCKET;});
    assert(tlnk_set_nonblocking(client)==0);return client;
}
json Receive(tlnk_socket_t socket){
    FramedReader reader;std::string message;
    Await([&]{auto status=reader.Poll(socket,message);assert(status==ReadStatus::Idle||status==ReadStatus::Message);return status==ReadStatus::Message;});
    return json::parse(message);
}
}
int main(){
    assert(tlnk_init_sockets()==0);
    auto listener=tlnk_create_tcp_socket();auto addr=tlnk_make_addr("127.0.0.1",0);
    assert(bind(listener,reinterpret_cast<sockaddr*>(&addr),sizeof(addr))==0);
    assert(listen(listener,1)==0 && tlnk_set_nonblocking(listener)==0);
    SocketLength length=sizeof(addr);assert(getsockname(listener,reinterpret_cast<sockaddr*>(&addr),&length)==0);
    GetConfig().peerIP="127.0.0.1";GetConfig().inputPort=ntohs(addr.sin_port);
    InputSender sender;InputEvent event;event.type=TLNK_INPUT_KEY;event.keyCode=65;event.state=TLNK_STATE_DOWN;
    assert(!sender.SendEvent(event));assert(sender.Start());sender.SetSession(1);
    auto first=Accept(listener);auto reset=Receive(first);assert(reset["type"]=="reset"&&reset["session_id"]=="0000000000000001");
    Await([&]{return sender.IsConnected();});assert(sender.SendEvent(event));auto key=Receive(first);
    assert(key["type"]=="key"&&key["session_id"]=="0000000000000001");
    sender.SetSession(2);
    auto second=Accept(listener);reset=Receive(second);assert(reset["type"]=="reset"&&reset["session_id"]=="0000000000000002");
    tlnk_close_socket(first);Await([&]{return sender.IsConnected();});
    event.state=TLNK_STATE_UP;assert(sender.SendEvent(event));key=Receive(second);assert(key["session_id"]=="0000000000000002");
    auto start=NetClock::now();sender.Stop();assert(NetClock::now()-start<std::chrono::milliseconds(500));
    reset=Receive(second);assert(reset["type"]=="reset");
    tlnk_close_socket(second);tlnk_close_socket(listener);tlnk_cleanup_sockets();
}
