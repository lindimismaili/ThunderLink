#include "Cross/CrossSettings.h"
#include "protocol.h"
#include <cassert>
#include <chrono>
#include <iostream>
using namespace thunderlink;
using nlohmann::json;
template<class F> void rejects(F f) { bool rejected=false;try{f();}catch(const std::exception&){rejected=true;}assert(rejected); }
int main() {
    const auto root=std::filesystem::temp_directory_path()/
        ("ThunderLink Cross settings "+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    const auto path=root/"cross.json";
    auto defaults=LoadCrossSettings(path);
    assert(defaults.mode==CrossMode::ScreenShare && !defaults.enabled && defaults.swapCtrlCmd);
    defaults.mode=CrossMode::Cross; defaults.enabled=true; defaults.peerId=std::string(64,'a');
    defaults.manualHost="fe80::1234%7"; defaults.interfaceId="7";
    defaults.layout={{"win","Windows",{0,0,1920,1080},false},{"mac","Mac",{1920,-100,1000,1500},true}};
    defaults.localTopology={{"windows:stable",0,0,1920,1080,1,0}};
    defaults.remoteTopology={{"mac:stable",-1920,0,2560,1440,2,90}};
    SaveCrossSettings(defaults,path);
    auto restored=LoadCrossSettings(path);
    assert(restored.mode==CrossMode::Cross && restored.enabled && restored.peerId==defaults.peerId);
    assert(restored.manualHost==defaults.manualHost && restored.interfaceId=="7");
    assert(restored.layout.size()==2 && restored.layout[1].bounds.y==-100);
    assert(restored.localTopology.size()==1 && restored.remoteTopology.size()==1);
    assert(restored.remoteTopology[0].x==-1920 && restored.remoteTopology[0].rotation==90);
    json value=restored;
    assert(value.dump().find("private_key")==std::string::npos);
    assert(value.dump().find("password")==std::string::npos);
    auto bad=value; bad["version"]=2; rejects([&]{bad.get<CrossSettings>();});
    bad=value; bad["mode"]="unknown"; rejects([&]{bad.get<CrossSettings>();});
    bad=value; bad["displays"][1]["x"]=2000; rejects([&]{bad.get<CrossSettings>();});
    bad=value; bad["displays"][1]["x"]=1000; rejects([&]{bad.get<CrossSettings>();});
    bad=value; bad["displays"][1]["id"]="win"; rejects([&]{bad.get<CrossSettings>();});
    bad=value; bad["displays"][1]["width"]=-1; rejects([&]{bad.get<CrossSettings>();});
    bad=value; bad["displays"][1]["height"]="1080"; rejects([&]{bad.get<CrossSettings>();});
    bad=value; bad["enabled"]="true"; rejects([&]{bad.get<CrossSettings>();});
    bad=value; bad["emergency_shortcut"]="F12"; rejects([&]{bad.get<CrossSettings>();});
    bad=value; bad["remote_topology"][0]["scale"]=0; rejects([&]{bad.get<CrossSettings>();});
    bad=value; bad["remote_topology"][0]["rotation"]=45; rejects([&]{bad.get<CrossSettings>();});
    bad=value; bad["local_topology"][0]["id"]=""; rejects([&]{bad.get<CrossSettings>();});
    auto invalidTopology=restored; invalidTopology.localTopology[0].scale=0;
    rejects([&]{SaveCrossSettings(invalidTopology,path);});
    invalidTopology=restored; invalidTopology.emergencyShortcut="nope";
    rejects([&]{SaveCrossSettings(invalidTopology,path);});
    auto invalid=restored; invalid.layout[1].bounds.x=3000;
    rejects([&]{SaveCrossSettings(invalid,path);});
    assert(LoadCrossSettings(path).layout[1].bounds.x==1920); // failed validation preserves prior save
    auto previous=json{{"version",1}}.get<CrossSettings>();
    assert(previous.mode==CrossMode::ScreenShare && !previous.enabled);
    CrossSettings draft; draft.mode=CrossMode::Cross;
    draft.layout={{"win","Windows",{0,0,1920,1080},false}};
    SaveCrossSettings(draft,path);
    assert(LoadCrossSettings(path).layout.size()==1);
    draft.enabled=true; rejects([&]{SaveCrossSettings(draft,path);});
    draft.enabled=false; draft.layout[0].bounds.width=-1;
    rejects([&]{SaveCrossSettings(draft,path);});
    draft.layout.clear(); draft.enabled=true;
    rejects([&]{SaveCrossSettings(draft,path);});
    std::ifstream fixture(std::filesystem::path(TLNK_TEST_SOURCE_DIR)/"common/fixtures/cross_v4.json");
    auto golden=json::parse(fixture);
    assert(golden["version"]==TLNK_VERSION && golden["port"]==TLNK_PORT_CROSS);
    assert(golden["service"]==TLNK_CROSS_SERVICE);
    assert(golden["lease_ms"]==TLNK_CROSS_LEASE_MS && golden["ping_ms"]==TLNK_CROSS_PING_MS);
    assert(golden["queue_limit"]==TLNK_CROSS_QUEUE_LIMIT);
    std::filesystem::remove_all(root); // only this test's uniquely-created directory
    std::cout<<"Cross settings persistence, invalid layouts, defaults and shared v4 fixture passed\n";
}
