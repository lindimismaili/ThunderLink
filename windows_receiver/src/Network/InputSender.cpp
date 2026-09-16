#include "InputSender.h"
#include "TcpTransport.h"
#include "Utils/Logger.h"
#include "App/Validation.h"
#include "input_event.h"
#include <nlohmann/json.hpp>
using json=nlohmann::json;
namespace thunderlink {
InputSender::~InputSender(){Stop();}
bool InputSender::Start(){if(m_running.exchange(true))return true;m_sendThread=std::thread(&InputSender::SendThread,this);return true;}
void InputSender::Stop(){m_running=false;m_queueCV.notify_all();if(m_sendThread.joinable())m_sendThread.join();m_connected=false;}
void InputSender::SetSession(uint64_t sid){
    {std::lock_guard<std::mutex> lock(m_queueMutex);if(m_sessionId.exchange(sid)==sid)return;m_connected=false;m_eventQueue.clear();m_resetRequested=true;}
    m_queueCV.notify_all();
}
bool InputSender::SendEvent(const InputEvent& input){
    std::lock_guard<std::mutex> lock(m_queueMutex);
    if(!m_connected || !m_sessionId)return false;
    InputEvent event=input;event.sessionId=m_sessionId;
    if(event.type==TLNK_INPUT_MOUSE_MOVE && !m_eventQueue.empty() && m_eventQueue.back().type==TLNK_INPUT_MOUSE_MOVE)
        m_eventQueue.back()=event;
    else if(m_eventQueue.size()<1000)m_eventQueue.push_back(event);
    else {m_eventQueue.clear();m_resetRequested=true;m_queueCV.notify_one();return false;}
    m_queueCV.notify_one();
    return true;
}
void InputSender::SendThread(){
    auto socket=TLNK_INVALID_SOCKET;
    uint64_t connectedSession=0;
    auto closeConnection=[&]{
        m_connected=false;
        if(socket!=TLNK_INVALID_SOCKET)tlnk_close_socket(socket);
        socket=TLNK_INVALID_SOCKET;connectedSession=0;
        std::lock_guard<std::mutex> lock(m_queueMutex);m_eventQueue.clear();
    };
    while(m_running){
        bool reset=false;
        {std::lock_guard<std::mutex> lock(m_queueMutex);reset=m_resetRequested;m_resetRequested=false;}
        if(reset || connectedSession!=m_sessionId.load())closeConnection();
        uint64_t desired=m_sessionId.load();
        if(!desired){std::unique_lock<std::mutex> lock(m_queueMutex);m_queueCV.wait_for(lock,std::chrono::milliseconds(10));continue;}
        if(socket==TLNK_INVALID_SOCKET){
            socket=tlnk_create_tcp_socket();
            auto addr=tlnk_make_addr(GetConfig().peerIP.c_str(),GetConfig().inputPort);
            if(socket==TLNK_INVALID_SOCKET || !ConnectWithDeadline(socket,addr,m_running,TLNK_CONNECT_TIMEOUT_MS)){
                closeConnection();
                std::unique_lock<std::mutex> lock(m_queueMutex);
                m_queueCV.wait_for(lock,std::chrono::milliseconds(TLNK_RECONNECT_INTERVAL_MS),[&]{return !m_running || m_sessionId!=desired;});
                continue;
            }
            if(desired!=m_sessionId.load()){closeConnection();continue;}
            connectedSession=desired;
            InputEvent resetEvent;resetEvent.type=TLNK_INPUT_RESET;resetEvent.sessionId=desired;
            if(!SendFramed(socket,SerializeEvent(resetEvent),[&]{return m_running && m_sessionId==desired;},TLNK_WRITE_TIMEOUT_MS)){
                closeConnection();continue;
            }
            // Discard anything queued before this connection was established.
            {std::lock_guard<std::mutex> lock(m_queueMutex);
             m_eventQueue.clear();
             if (m_sessionId != desired || m_resetRequested) continue;
             m_connected=true;}
        }
        InputEvent event;bool haveEvent=false;
        {std::unique_lock<std::mutex> lock(m_queueMutex);
         m_queueCV.wait_for(lock,std::chrono::milliseconds(10),[&]{return !m_running||m_resetRequested||!m_eventQueue.empty();});
         if(!m_eventQueue.empty()){event=m_eventQueue.front();m_eventQueue.pop_front();haveEvent=true;}}
        if(haveEvent && event.sessionId==connectedSession &&
           !SendFramed(socket,SerializeEvent(event),[&]{return m_running && m_sessionId==connectedSession;},TLNK_WRITE_TIMEOUT_MS)){
            closeConnection();continue;
        }
        int readable = socket == TLNK_INVALID_SOCKET ? 0 : SocketReady(socket,false,0);
        if(readable < 0){closeConnection();continue;}
        if(readable > 0){
            char ignored;int n=static_cast<int>(recv(socket,&ignored,1,0));
            if(n>=0 || !tlnk_would_block())closeConnection();
        }
    }
    if(socket!=TLNK_INVALID_SOCKET){
        InputEvent reset;reset.type=TLNK_INPUT_RESET;reset.sessionId=connectedSession;
        SendFramed(socket,SerializeEvent(reset),[]{return true;},50);
    }
    closeConnection();
}
std::string InputSender::SerializeEvent(const InputEvent& event){
    json j; j["session_id"]=SessionString(event.sessionId);
    j["timestamp"]=std::chrono::duration_cast<std::chrono::milliseconds>(NetClock::now().time_since_epoch()).count();
    switch(event.type){
    case TLNK_INPUT_RESET:j["type"]="reset";break;
    case TLNK_INPUT_MOUSE_MOVE:j["type"]="mouse_move";j["x"]=event.x;j["y"]=event.y;break;
    case TLNK_INPUT_MOUSE_BUTTON:j["type"]="mouse_button";j["x"]=event.x;j["y"]=event.y;
        j["button"]=event.button;j["state"]=event.state==TLNK_STATE_DOWN?"down":"up";break;
    case TLNK_INPUT_KEY:{j["type"]="key";j["key_code"]=event.keyCode;j["state"]=event.state==TLNK_STATE_DOWN?"down":"up";
        auto mods=json::array();if(event.modifiers&TLNK_MOD_SHIFT)mods.push_back("shift");
        if(event.modifiers&TLNK_MOD_CTRL)mods.push_back("ctrl");if(event.modifiers&TLNK_MOD_ALT)mods.push_back("alt");
        if(event.modifiers&TLNK_MOD_META)mods.push_back("meta");j["modifiers"]=mods;break;}
    case TLNK_INPUT_SCROLL:j["type"]="scroll";j["delta_x"]=event.deltaX;j["delta_y"]=event.deltaY;break;
    default:j["type"]="unknown";
    }
    return j.dump();
}
}
