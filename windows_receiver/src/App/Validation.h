#pragma once
#include "Config.h"
#include "network_utils.h"
#include <charconv>
#include <cstdio>
#include <stdexcept>
#include <vector>

namespace thunderlink {
inline bool ValidStreamConfig(uint32_t w, uint32_t h, uint32_t fps, uint32_t bitrate,
                              bool resolved = false) {
    const bool dimensions = resolved ? (w >= 2 && h >= 2) :
        ((w == 0 && h == 0) || (w == 0 && h >= 2) || (w >= 2 && h >= 2));
    return dimensions && w <= TLNK_MAX_WIDTH && h <= TLNK_MAX_HEIGHT && !(w & 1) && !(h & 1) &&
           fps >= 1 && fps <= TLNK_MAX_FPS && bitrate >= TLNK_MIN_BITRATE_KBPS &&
           bitrate <= TLNK_MAX_BITRATE_KBPS;
}
inline std::string SessionString(uint64_t id) {
    char text[17]; std::snprintf(text, sizeof(text), "%016llx", static_cast<unsigned long long>(id));
    return text;
}
inline uint64_t ParseSession(const std::string& text) {
    if (text.size() != 16) return 0;
    uint64_t id = 0;
    for (char c : text) {
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return 0;
        id = (id << 4) | static_cast<uint64_t>(c <= '9' ? c - '0' : c - 'a' + 10);
    }
    return id;
}
inline uint32_t ParseNumber(const std::string& text, uint32_t lo, uint32_t hi) {
    uint32_t value{};
    auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    if (text.empty() || result.ec != std::errc() || result.ptr != text.data() + text.size() || value < lo || value > hi)
        throw std::invalid_argument("Invalid numeric value: " + text);
    return value;
}
inline void ParseDimensions(const std::string& text, uint32_t& w, uint32_t& h,
                            bool window = false) {
    auto x = text.find_first_of("xX");
    if (x == std::string::npos) throw std::invalid_argument("Expected WxH: " + text);
    w = ParseNumber(text.substr(0, x), window ? 16 : 0, TLNK_MAX_WIDTH);
    h = ParseNumber(text.substr(x + 1), window ? 16 : 0, TLNK_MAX_HEIGHT);
}
inline Config ParseArguments(const std::vector<std::string>& args, Config cfg = {}) {
    for (size_t i = 0; i < args.size(); ++i) {
        const auto& flag = args[i];
        if (flag == "--fullscreen") { cfg.startFullscreen = true; continue; }
        if (flag == "--no-vsync") { cfg.vsync = false; continue; }
        if (flag == "--vsync") { cfg.vsync = true; continue; }
        if (flag == "--console") { cfg.showConsole = true; continue; }
        if (i + 1 >= args.size()) throw std::invalid_argument("Missing value for " + flag);
        const auto& value = args[++i];
        if (flag == "--bind" || flag == "--peer") {
            in_addr address{};
            if (inet_pton(AF_INET, value.c_str(), &address) != 1 ||
                (flag == "--peer" && (address.s_addr == 0 || address.s_addr == INADDR_NONE)))
                throw std::invalid_argument("Invalid IPv4 address: " + value);
            (flag == "--bind" ? cfg.bindIP : cfg.peerIP) = value;
        } else if (flag == "--video-port") cfg.videoPort = static_cast<uint16_t>(ParseNumber(value, 1, 65535));
        else if (flag == "--input-port") cfg.inputPort = static_cast<uint16_t>(ParseNumber(value, 1, 65535));
        else if (flag == "--control-port") cfg.controlPort = static_cast<uint16_t>(ParseNumber(value, 1, 65535));
        else if (flag == "--fps") cfg.fps = ParseNumber(value, 1, TLNK_MAX_FPS);
        else if (flag == "--bitrate") cfg.bitrateKbps = ParseNumber(value, 1, 200) * 1000;
        else if (flag == "--res") ParseDimensions(value, cfg.width, cfg.height);
        else if (flag == "--window") ParseDimensions(value, cfg.windowWidth, cfg.windowHeight, true);
        else if (flag == "--codec") {
            if (value == "h264" || value == "H264") cfg.codec = TLNK_CODEC_H264;
            else if (value == "hevc" || value == "HEVC") cfg.codec = TLNK_CODEC_HEVC;
            else throw std::invalid_argument("Unsupported codec: " + value);
        } else throw std::invalid_argument("Unknown option: " + flag);
    }
    if (!ValidStreamConfig(cfg.width, cfg.height, cfg.fps, cfg.bitrateKbps))
        throw std::invalid_argument("Resolution must be native 0x0, aspect-derived 0xH, or even WxH within 3840x2160");
    return cfg;
}
} // namespace thunderlink
