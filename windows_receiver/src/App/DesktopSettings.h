#pragma once
#include "Validation.h"
#include <nlohmann/json.hpp>
#include <filesystem>
#include <fstream>
#ifdef _WIN32
#include <shlobj.h>
#include <shellapi.h>
#endif

namespace thunderlink {
inline std::vector<std::string> SettingsArguments(const Config& cfg) {
    std::vector<std::string> args = {
        "--bind", cfg.bindIP, "--peer", cfg.peerIP,
        "--video-port", std::to_string(cfg.videoPort), "--input-port", std::to_string(cfg.inputPort),
        "--control-port", std::to_string(cfg.controlPort), "--codec", cfg.codec == TLNK_CODEC_HEVC ? "hevc" : "h264",
        "--res", std::to_string(cfg.width) + "x" + std::to_string(cfg.height),
        "--fps", std::to_string(cfg.fps), "--bitrate", std::to_string(cfg.bitrateKbps / 1000)};
    if (cfg.vsync) args.push_back("--vsync");
    return args;
}
inline Config LoadSettings(const std::filesystem::path& path) {
    if (!std::filesystem::exists(path)) return Config{};
    std::ifstream input(path);
    if (!input) throw std::runtime_error("Cannot read saved settings");
    const auto args = nlohmann::json::parse(input).get<std::vector<std::string>>();
    return ParseArguments(args);
}
inline void SaveSettings(const Config& cfg, const std::filesystem::path& path) {
    auto args = SettingsArguments(cfg);
    (void)ParseArguments(args);
    std::filesystem::create_directories(path.parent_path());
    auto temporary = path; temporary += ".tmp";
    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        if (!output) throw std::runtime_error("Cannot write settings");
        output << nlohmann::json(args).dump(2) << '\n';
        output.flush();
        if (!output) throw std::runtime_error("Cannot finish writing settings");
    }
#ifdef _WIN32
    if (!MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        throw std::runtime_error("Cannot replace saved settings");
#else
    std::filesystem::rename(temporary, path);
#endif
}
#ifdef _WIN32
inline std::filesystem::path UserDataDirectory() {
    PWSTR value = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_CREATE, nullptr, &value)))
        throw std::runtime_error("Windows Local AppData is unavailable");
    std::filesystem::path result = std::filesystem::path(value) / L"ThunderLink";
    CoTaskMemFree(value);
    std::filesystem::create_directories(result);
    return result;
}
inline std::filesystem::path SettingsPath() { return UserDataDirectory() / L"settings.json"; }
inline std::filesystem::path CrossSettingsPath() { return UserDataDirectory() / L"cross.json"; }
inline std::filesystem::path LogPath() {
#ifdef TLNK_PORTABLE
    auto directory = UserDataDirectory() / L"Logs";
    std::filesystem::create_directories(directory);
    return directory / L"windows.log";
#elif defined(TLNK_LOG_FILE)
    return std::filesystem::path(TLNK_LOG_FILE);
#else
    return UserDataDirectory() / L"windows.log";
#endif
}
inline void OpenLogDirectory(HWND owner) {
    ShellExecuteW(owner, L"open", LogPath().parent_path().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}
#endif
inline std::string& StartupSettingsError() { static std::string value; return value; }
inline Config& SavedConfig() { static Config value; return value; }
} // namespace thunderlink
