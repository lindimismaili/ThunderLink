#include "App/UICommandGate.h"
#include <cassert>
#include <fstream>
#include <iostream>
#include <iterator>

using thunderlink::ui::UICommandGate;
using Kind = thunderlink::ui::CommandDecision::Kind;

static std::string Fixture(const char* name) {
    std::ifstream input(std::string(TLNK_TEST_SOURCE_DIR) + "/shared_ui/contract/fixtures/" + name);
    assert(input.good());
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}
int main() {
    UICommandGate gate;
    gate.StartInstance("fixture-instance", {"connection.connect", "settings.saveRestart", "cross.layoutApply", "cross.approvePairing"});
    gate.SetRevisions("1", "mac-1", "4", "attempt-1");
    const auto accepted = gate.Admit(Fixture("valid-command.json"));
    assert(accepted.kind == Kind::Accepted && accepted.action == "connection.connect");
    assert(gate.Admit(Fixture("valid-command.json")).kind == Kind::DuplicatePending);
    gate.Finish("req-1", {{"status", "ok"}});
    assert(gate.Admit(Fixture("valid-command.json")).kind == Kind::DuplicateResult);
    assert(gate.Admit(Fixture("invalid-command.json")).code == "malformed");
    assert(gate.Admit(std::string(65537, 'x')).code == "too_large");
    nlohmann::json command = nlohmann::json::parse(Fixture("valid-command.json"));
    command["requestId"] = "req-2"; command["instanceId"] = "old";
    assert(gate.Admit(command.dump()).code == "stale_instance");
    command["instanceId"] = "fixture-instance"; command["action"] = "unknown.action";
    assert(gate.Admit(command.dump()).code == "not_supported");
    command["action"] = "settings.saveRestart";
    command["payload"] = {{"revision", "0"}, {"values", {{"videoPort", 50001}}}};
    assert(gate.Admit(command.dump()).code == "stale_settings");
    command["payload"]["revision"] = "1";
    assert(gate.Admit(command.dump()).kind == Kind::Accepted);
    gate.Finish("req-2", {{"status", "ok"}});
    command["requestId"] = "req-invalid-revision";
    command["payload"]["revision"] = 1;
    assert(gate.Admit(command.dump()).code == "invalid_payload");
    command["payload"]["revision"] = "1";
    command["payload"]["values"]["videoPort"] = 1000000001;
    assert(gate.Admit(command.dump()).code == "invalid_payload");
    command["requestId"] = "req-3"; command["action"] = "cross.layoutApply";
    command["payload"] = {{"peerId", "mac-1"}, {"topologyRevision", "3"}, {"tiles", nlohmann::json::array()}};
    assert(gate.Admit(command.dump()).code == "stale_topology");
    command["payload"]["topologyRevision"] = "4";
    assert(gate.Admit(command.dump()).kind == Kind::Accepted);
    gate.Finish("req-3", {{"status", "ok"}});
    command["requestId"] = "req-invalid-tile";
    command["payload"]["tiles"] = {{{"id", "display"}, {"owner", "mac"},
                                       {"x", 0}, {"y", 0}, {"width", -1}, {"height", 100}}};
    assert(gate.Admit(command.dump()).code == "invalid_payload");
    command["requestId"] = "req-4"; command["action"] = "cross.approvePairing";
    command["payload"] = {{"peerId", "mac-1"}, {"attempt", "expired"}};
    assert(gate.Admit(command.dump()).code == "expired_pairing");
    gate.StartInstance("new-instance", {"connection.connect"});
    assert(gate.Admit(Fixture("valid-command.json")).code == "stale_instance");
    std::cout << "UI command admission tests passed\n";
}
