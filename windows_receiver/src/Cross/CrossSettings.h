#pragma once

#include <filesystem>
#include <fstream>
#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <vector>

#include <nlohmann/json.hpp>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

#include "cross_layout.h"
#include "cross_input_state.h"

namespace thunderlink {

enum class CrossMode { ScreenShare, Cross };

struct CrossSettings {
    int version = 1;
    CrossMode mode = CrossMode::ScreenShare;
    bool enabled = false;
    bool swapCtrlCmd = true;
    std::string interfaceId;
    std::string manualHost;
    std::string peerId;
    std::string emergencyShortcut = "Ctrl+Alt+Shift+F12";
    cross::Layout layout;
    /* Native Mac topology observed when this arrangement was applied. It is
     * deliberately separate from virtual tile geometry, which users may scale. */
    struct TopologyDisplay { std::string id; double x = 0, y = 0, width = 0, height = 0, scale = 1; int rotation = 0; };
    using RemoteTopologyDisplay = TopologyDisplay;
    std::vector<TopologyDisplay> remoteTopology;
    std::vector<TopologyDisplay> localTopology;
};

inline void ValidateCrossTopology(const std::vector<CrossSettings::TopologyDisplay>& topology) {
    std::unordered_set<std::string> ids;
    for (const auto& display : topology) {
        if (display.id.empty() || display.id.size() > 256 || !ids.insert(display.id).second ||
            display.width <= 0 || display.height <= 0 || display.scale <= 0 || display.scale > 16 ||
            !std::isfinite(display.x) || !std::isfinite(display.y) || !std::isfinite(display.width) ||
            !std::isfinite(display.height) || !std::isfinite(display.scale) ||
            std::abs(display.x) > 1e7 || std::abs(display.y) > 1e7 || display.rotation % 90 != 0)
            throw std::invalid_argument("Saved display topology is invalid");
    }
}

inline void ValidateEmergencyShortcut(const std::string& shortcut) {
    crossinput::EmergencyShortcut parsed{};
    if (!crossinput::ParseEmergencyShortcut(shortcut, parsed))
        throw std::invalid_argument("Cross emergency shortcut is invalid");
}

inline void to_json(nlohmann::json& json, const CrossSettings& value) {
    json = nlohmann::json{{"version", value.version},
        {"mode", value.mode == CrossMode::Cross ? "cross" : "screen_share"}, {"enabled", value.enabled},
        {"swap_ctrl_cmd", value.swapCtrlCmd}, {"interface_id", value.interfaceId},
        {"manual_host", value.manualHost}, {"peer_id", value.peerId},
        {"emergency_shortcut", value.emergencyShortcut}, {"displays", nlohmann::json::array()}, {"remote_topology", nlohmann::json::array()}, {"local_topology", nlohmann::json::array()}};
    for (const auto& display : value.layout) {
        json["displays"].push_back({{"id", display.id}, {"name", display.name},
            {"remote", display.remote}, {"x", display.bounds.x}, {"y", display.bounds.y},
            {"width", display.bounds.width}, {"height", display.bounds.height}});
    }
    auto writeTopology = [&](const char* field, const auto& topology) { for (const auto& display : topology) json[field].push_back({{"id", display.id}, {"x", display.x}, {"y", display.y}, {"width", display.width}, {"height", display.height}, {"scale", display.scale}, {"rotation", display.rotation}}); };
    writeTopology("remote_topology", value.remoteTopology); writeTopology("local_topology", value.localTopology);
}

inline void from_json(const nlohmann::json& json, CrossSettings& value) {
    if (json.value("version", 0) != 1) throw std::invalid_argument("Unsupported Cross settings version");
    value.version = 1;
    const auto mode = json.value("mode", std::string{"screen_share"});
    if (mode == "screen_share") value.mode = CrossMode::ScreenShare;
    else if (mode == "cross") value.mode = CrossMode::Cross;
    else throw std::invalid_argument("Unsupported Cross mode");
    value.enabled = json.value("enabled", false);
    value.swapCtrlCmd = json.value("swap_ctrl_cmd", true);
    value.interfaceId = json.value("interface_id", std::string{});
    value.manualHost = json.value("manual_host", std::string{});
    value.peerId = json.value("peer_id", std::string{});
    value.emergencyShortcut = json.value("emergency_shortcut", std::string{"Ctrl+Alt+Shift+F12"});
    ValidateEmergencyShortcut(value.emergencyShortcut);
    value.layout.clear();
    for (const auto& item : json.value("displays", nlohmann::json::array())) {
        cross::Display display{};
        display.id = item.at("id").get<std::string>(); display.name = item.value("name", display.id);
        display.remote = item.at("remote").get<bool>();
        display.bounds = {item.at("x").get<double>(), item.at("y").get<double>(),
                          item.at("width").get<double>(), item.at("height").get<double>()};
        value.layout.push_back(std::move(display));
    }
    auto readTopology = [&](const char* field, auto& destination) {
      destination.clear();
      for (const auto& item : json.value(field, nlohmann::json::array())) {
        CrossSettings::TopologyDisplay display{};
        display.id = item.at("id").get<std::string>(); display.x = item.value("x", 0.0); display.y = item.value("y", 0.0); display.width = item.at("width").get<double>();
        display.height = item.at("height").get<double>(); display.scale = item.value("scale", 1.0); display.rotation = item.value("rotation", 0);
        destination.push_back(std::move(display));
      }
      ValidateCrossTopology(destination);
    };
    readTopology("remote_topology", value.remoteTopology); readTopology("local_topology", value.localTopology);
    const bool hasRemote = std::any_of(value.layout.begin(), value.layout.end(), [](const auto& display) { return display.remote; });
    if (!value.layout.empty() || value.enabled) {
        const auto error = cross::ValidateLayout(value.layout, value.enabled || hasRemote);
        if (!error.empty()) throw std::invalid_argument("Saved Cross layout is invalid: " + error);
    }
}

inline CrossSettings LoadCrossSettings(const std::filesystem::path& path) {
    if (!std::filesystem::exists(path)) return {};
    std::ifstream input(path);
    if (!input) throw std::runtime_error("Cannot read Cross settings");
    return nlohmann::json::parse(input).get<CrossSettings>();
}

inline void SaveCrossSettings(const CrossSettings& settings, const std::filesystem::path& path) {
    ValidateEmergencyShortcut(settings.emergencyShortcut);
    ValidateCrossTopology(settings.remoteTopology);
    ValidateCrossTopology(settings.localTopology);
    const bool hasRemote = std::any_of(settings.layout.begin(), settings.layout.end(), [](const auto& display) { return display.remote; });
    if (!settings.layout.empty() || settings.enabled) {
        const auto error = cross::ValidateLayout(settings.layout, settings.enabled || hasRemote);
        if (!error.empty()) throw std::invalid_argument(error);
    }
    std::filesystem::create_directories(path.parent_path());
    auto temporary = path; temporary += ".tmp";
    { std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
      if (!output) throw std::runtime_error("Cannot write Cross settings");
      output << nlohmann::json(settings).dump(2) << '\n'; output.flush();
      if (!output) throw std::runtime_error("Cannot finish Cross settings"); }
#ifdef _WIN32
    if (!MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        throw std::runtime_error("Cannot replace Cross settings");
#else
    std::filesystem::rename(temporary, path);
#endif
}

} // namespace thunderlink
