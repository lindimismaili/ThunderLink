#include "NetworkManager.h"
#include "TcpTransport.h"
#include "Utils/Logger.h"
#include "App/Validation.h"
#include <nlohmann/json.hpp>
#include <random>
#ifdef _WIN32
#include <bcrypt.h>
#endif
using json=nlohmann::json;
namespace thunderlink {
namespace {
uint64_t NewSession() {
    uint64_t value=0;
#ifdef _WIN32
    do {
        if (BCryptGenRandom(nullptr,reinterpret_cast<PUCHAR>(&value),sizeof(value),BCRYPT_USE_SYSTEM_PREFERRED_RNG)!=0)
            throw std::runtime_error("Could not generate stream session ID");
    } while (!value);
#else
    std::random_device random;
    do { value=(uint64_t(random())<<32)|random(); } while (!value);
#endif
    return value;
}
uint32_t Number(const json& j,const char* field) {
    const auto& value=j.at(field);
    if (!value.is_number_integer()) throw std::invalid_argument("Invalid integer field");
    if (value.is_number_unsigned()) {
        auto n=value.get<uint64_t>();
        if (n>UINT32_MAX) throw std::invalid_argument("Integer out of range");
        return uint32_t(n);
    }
    auto n=value.get<int64_t>();
    if(n<0 || n>UINT32_MAX) throw std::invalid_argument("Integer out of range");
    return uint32_t(n);
}
}
NetworkManager::~NetworkManager(){ Stop(); }
bool NetworkManager::Start(){
    if(m_running.load()) return true;
    m_listenSocket=tlnk_create_tcp_socket();
    if(m_listenSocket==TLNK_INVALID_SOCKET) return false;
    const auto& cfg=GetConfig(); auto address=tlnk_make_addr(cfg.bindIP.c_str(),cfg.controlPort);
    if(bind(m_listenSocket,reinterpret_cast<sockaddr*>(&address),sizeof(address))!=0 ||
       listen(m_listenSocket,1)!=0 || tlnk_set_nonblocking(m_listenSocket)!=0){
        tlnk_close_socket(m_listenSocket); m_listenSocket=TLNK_INVALID_SOCKET; return false;
    }
    m_running=true;
    m_controlThread=std::thread(&NetworkManager::ControlThread,this);
    return true;
}
void NetworkManager::Stop(){
    m_running=false;
    if(m_controlThread.joinable()) m_controlThread.join();
}
ConnectionInfo NetworkManager::GetConnectionInfo()const{ std::lock_guard<std::mutex> l(m_mutex); return m_connInfo; }
std::string NetworkManager::GetError()const{ std::lock_guard<std::mutex> l(m_mutex); return m_error; }
void NetworkManager::SetError(const std::string& error){
    {std::lock_guard<std::mutex> l(m_mutex); m_error=error;}
    LOG_WARN("Control: %s",error.c_str());
}
bool NetworkManager::RequestConfigure(uint32_t w,uint32_t h,uint32_t fps){
    const auto& cfg=GetConfig();
    if(!m_connected || !ValidStreamConfig(w,h,fps,cfg.bitrateKbps)) return false;
    std::lock_guard<std::mutex> l(m_mutex);
    ConnectionInfo info; info.width=w;info.height=h;info.fps=fps;info.codec=cfg.codec;info.bitrateKbps=cfg.bitrateKbps;
    m_requested=info; return true;
}
void NetworkManager::RequestKeyframe(){
    if(!m_connected) return;
    std::lock_guard<std::mutex> l(m_mutex);m_keyframeRequested=true;
}
void NetworkManager::Disconnect(){
    bool wasConnected=m_connected.exchange(false);
    { std::lock_guard<std::mutex> l(m_mutex);m_connInfo={};m_requested.reset();m_keyframeRequested=false; }
    m_peerLatencyMs=0;
    if(wasConnected && m_onDisconnected) m_onDisconnected();
}
void NetworkManager::ControlThread(){
    while(m_running){
        int ready=SocketReady(m_listenSocket,false);
        if(ready<0){SetError("Control listener readiness failed");m_running=false;break;}
        if(!ready) continue;
        sockaddr_in addr{};SocketLength length=sizeof(addr);
        auto client=accept(m_listenSocket,reinterpret_cast<sockaddr*>(&addr),&length);
        if(client==TLNK_INVALID_SOCKET) continue;
        if(tlnk_set_nonblocking(client)==0){
            try {HandleClient(client);}catch(const std::exception& e){SetError(e.what());}
        }
        tlnk_close_socket(client);Disconnect();
    }
    tlnk_close_socket(m_listenSocket);m_listenSocket=TLNK_INVALID_SOCKET;
}
void NetworkManager::HandleClient(tlnk_socket_t socket){
    FramedReader reader;
    auto send=[&](const json& j){return SendFramed(socket,j.dump(),[&]{return m_running.load();},TLNK_WRITE_TIMEOUT_MS);};
    auto deadline=m_clock()+std::chrono::milliseconds(TLNK_HANDSHAKE_TIMEOUT_MS);
    auto lastReceived=m_clock(),lastPing=lastReceived,lastKeyframe=lastReceived-std::chrono::seconds(1);
    bool hello=false,waitingStart=false;
    ConnectionInfo pending;
    auto configure=[&](ConnectionInfo requested){
        Disconnect();
        requested.sessionId=NewSession(); pending=requested; waitingStart=true;
        deadline=m_clock()+std::chrono::milliseconds(TLNK_HANDSHAKE_TIMEOUT_MS);
        return send(json{{"type",TLNK_CTRL_CONFIGURE},{"session_id",SessionString(pending.sessionId)},
                         {"width",pending.width},{"height",pending.height},{"fps",pending.fps},
                         {"codec",int(pending.codec)},{"bitrate",pending.bitrateKbps}});
    };
    while(m_running){
        const auto now=m_clock();
        if((!hello||waitingStart) && now>=deadline){SetError("Stream handshake timed out");break;}
        if(hello && now-lastReceived>=std::chrono::milliseconds(TLNK_PEER_TIMEOUT_MS)){SetError("Peer stopped responding");break;}
        std::optional<ConnectionInfo> request;
        bool key=false;
        {std::lock_guard<std::mutex> l(m_mutex);request=m_requested;m_requested.reset();
         if(now-lastKeyframe>=std::chrono::milliseconds(75)){key=m_keyframeRequested;m_keyframeRequested=false;}}
        if(request && !configure(*request)) break;
        if(key && m_connected){
            if(!send(json{{"type",TLNK_CTRL_REQUEST_KEYFRAME},{"session_id",SessionString(pending.sessionId)}})) break;
            lastKeyframe=now;
        }
        if(hello && now-lastPing>=std::chrono::milliseconds(TLNK_PING_INTERVAL_MS)){
            auto stamp=std::chrono::duration_cast<std::chrono::microseconds>(now.time_since_epoch()).count();
            if(!send(json{{"type",TLNK_CTRL_PING},{"sent_time",stamp}})) break;
            lastPing=now;
        }
        std::string text;auto status=reader.Poll(socket,text);
        if(status==ReadStatus::Idle) continue;
        if(status!=ReadStatus::Message){SetError("Control connection closed or invalid framing");break;}
        lastReceived=m_clock();
        try{
            auto j=json::parse(text);auto type=Number(j,"type");
            if(type==TLNK_CTRL_BYE) break;
            if(!hello){
                if(type!=TLNK_CTRL_HELLO || Number(j,"version")!=TLNK_VERSION){
                    send(json{{"type",TLNK_CTRL_BYE},{"reason","Protocol v3 required; update both applications"}});
                    SetError("Protocol mismatch: update both applications to v3");break;
                }
                hello=true;
                const auto& c=GetConfig();ConnectionInfo requested;
                requested.width=c.width;requested.height=c.height;requested.fps=c.fps;requested.codec=c.codec;requested.bitrateKbps=c.bitrateKbps;
                if(!configure(requested))break;
            }else if(type==TLNK_CTRL_START){
                uint64_t sid=ParseSession(j.at("session_id").get<std::string>());
                if(!waitingStart || sid!=pending.sessionId) continue;
                auto w=Number(j,"width"),h=Number(j,"height"),f=Number(j,"fps"),b=Number(j,"bitrate"),c=Number(j,"codec");
                if(!ValidStreamConfig(w,h,f,b,true)||c!=uint32_t(pending.codec)||f!=pending.fps||b!=pending.bitrateKbps)
                    throw std::invalid_argument("Invalid START configuration");
                pending.width=w;pending.height=h;pending.connected=true;
                {std::lock_guard<std::mutex> l(m_mutex);m_connInfo=pending;m_error.clear();}
                waitingStart=false;m_connected=true;
                if(m_onConnected)m_onConnected(pending);
                RequestKeyframe();
            }else if(type==TLNK_CTRL_STOP){
                if(ParseSession(j.at("session_id").get<std::string>())!=pending.sessionId)continue;
                SetError(j.value("reason",std::string("Sender stopped streaming")));break;
            }else if(type==TLNK_CTRL_PONG){
                auto sent=j.at("sent_time").get<int64_t>();
                auto stamp=std::chrono::duration_cast<std::chrono::microseconds>(m_clock().time_since_epoch()).count();
                if(sent>0 && sent<=stamp && stamp-sent<=TLNK_PEER_TIMEOUT_MS*1000LL){
                    float latency=float(stamp-sent)/1000.f;m_peerLatencyMs=latency;
                    std::lock_guard<std::mutex> l(m_mutex);m_connInfo.peerLatencyMs=latency;
                }
            }else if(type==TLNK_CTRL_PING){
                if(!send(json{{"type",TLNK_CTRL_PONG},{"sent_time",j.at("sent_time")}}))break;
            }else throw std::invalid_argument("Unexpected control message");
        }catch(const std::exception& e){SetError(e.what());break;}
    }
    if(!m_running)SendFramed(socket,json{{"type",TLNK_CTRL_BYE}}.dump(),[]{return true;},50);
}
} // namespace thunderlink
