#include "Network/TcpTransport.h"
#include "App/Validation.h"
#include <cassert>
#include <thread>
using namespace thunderlink;
int main(){
    FramedReader reader;auto bytes=FrameJSON("{\"ok\":true}");
    auto now=NetClock::time_point{};
    for(size_t i=0;i<bytes.size();++i){
        auto status=reader.Feed(&bytes[i],1,now);
        assert(status==(i+1==bytes.size()?ReadStatus::Message:ReadStatus::Idle));
    }
    assert(reader.TakeMessage()=="{\"ok\":true}");
    uint8_t zero[4]{};assert(reader.Feed(zero,4,now)==ReadStatus::Malformed);
    FramedReader partial;assert(partial.Feed(bytes.data(),2,now)==ReadStatus::Idle);
    assert(partial.CheckDeadline(now+std::chrono::milliseconds(1999))==ReadStatus::Idle);
    assert(partial.CheckDeadline(now+std::chrono::milliseconds(2000))==ReadStatus::TimedOut);
    for(const auto& args:std::vector<std::vector<std::string>>{{"--fps","-1"},{"--fps","4294967295"},{"--bitrate","201"},{"--video-port","65536"},{"--res","101x100"},{"--res","2x0"},{"--codec","bad"},{"--bind","x"},{"--fps"},{"--what","1"}}){
        bool threw=false;try{(void)ParseArguments(args);}catch(const std::exception&){threw=true;}assert(threw);
    }
    auto config=ParseArguments({"--res","0x1080","--fps","240","--bitrate","200","--fullscreen"});
    assert(config.width==0 && config.height==1080 && config.startFullscreen);
    assert(ParseSession(SessionString(UINT64_MAX))==UINT64_MAX);
    assert(ParseSession("+000000000000001")==0);
    assert(ParseSession("0000000000000000")==0);
    return 0;
}
