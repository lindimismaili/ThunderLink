#include "FrameReceiver.h"
#include "TcpTransport.h"
#include "Utils/FrameBuffer.h"
#include "Utils/PerformanceMonitor.h"
#include "Utils/Logger.h"
#include "App/Config.h"
#include "frame_metadata.h"
namespace thunderlink {
FrameReceiver::FrameReceiver(FrameBuffer* frames,PerformanceMonitor* perf):m_frameBuffer(frames),m_perfMon(perf){}
FrameReceiver::~FrameReceiver(){Stop();}
bool FrameReceiver::Start(){
    m_socket=tlnk_create_udp_socket();
    if(m_socket==TLNK_INVALID_SOCKET)return false;
    auto addr=tlnk_make_addr(GetConfig().bindIP.c_str(),GetConfig().videoPort);
    if(bind(m_socket,reinterpret_cast<sockaddr*>(&addr),sizeof(addr))!=0 || tlnk_set_nonblocking(m_socket)!=0){
        tlnk_close_socket(m_socket);m_socket=TLNK_INVALID_SOCKET;return false;
    }
    m_running=true;m_receiveThread=std::thread(&FrameReceiver::ReceiveThread,this);return true;
}
void FrameReceiver::Stop(){m_running=false;if(m_receiveThread.joinable())m_receiveThread.join();}
void FrameReceiver::ReceiveThread(){
    uint8_t bytes[65536];
    while(m_running){
        // Expire by elapsed time even if no packets arrive or every packet is invalid.
        m_frameBuffer->EvictStaleFrames(TLNK_FRAME_TIMEOUT_MS);
        int ready=SocketReady(m_socket,false,10);
        if(ready<0){LOG_ERROR("Video socket readiness failed");m_running=false;break;}
        if(!ready)continue;
        sockaddr_in peer{};SocketLength size=sizeof(peer);
        int count=static_cast<int>(recvfrom(m_socket,reinterpret_cast<char*>(bytes),sizeof(bytes),0,reinterpret_cast<sockaddr*>(&peer),&size));
        if(count<TLNK_HEADER_SIZE)continue;
        in_addr expected{};inet_pton(AF_INET,GetConfig().peerIP.c_str(),&expected);
        if(peer.sin_addr.s_addr!=expected.s_addr)continue;
        FrameMetadata header{};
        if(!DecodeFrameHeader(bytes,static_cast<size_t>(count),&header) ||
           static_cast<size_t>(count)!=TLNK_HEADER_SIZE+static_cast<size_t>(header.payloadSize))continue;
        if(m_frameBuffer->InsertPacket(header,bytes+TLNK_HEADER_SIZE) && m_perfMon)
            m_perfMon->PacketReceived(static_cast<uint32_t>(count));
    }
    tlnk_close_socket(m_socket);m_socket=TLNK_INVALID_SOCKET;
}
}
