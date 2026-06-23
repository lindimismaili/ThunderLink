#pragma once

#include <string>
#include <cstdint>

#include "protocol.h"

namespace thunderlink {

/* Runtime configuration for the receiver.
 *
 * Defaults match the direct USB-C->Ethernet link-local setup, so the common
 * "two adapters + a cable" case needs no command-line arguments. Overrides are
 * parsed from the command line in main.cpp (see ParseCommandLine).
 *
 * Accessed through the process-wide GetConfig() singleton so the networking
 * classes can read endpoints without threading a config object through every
 * constructor. It is populated once at startup before any subsystem starts. */
struct Config {
    /* Endpoints.
     * bindIP defaults to "0.0.0.0" (listen on every interface) so the receiver
     * starts whether or not the 169.254.x static link is configured, and works
     * over the direct link or a normal LAN. Override with --bind to restrict it. */
    std::string bindIP   = "0.0.0.0";     /* our local bind address                          */
    std::string peerIP   = TLNK_MAC_IP;   /* Mac address (InputSender connects here)         */
    uint16_t    videoPort   = TLNK_PORT_VIDEO;
    uint16_t    inputPort   = TLNK_PORT_INPUT;
    uint16_t    controlPort = TLNK_PORT_CONTROL;

    /* Desired stream parameters (sent to the Mac in CONFIGURE) */
    uint32_t  width        = 2560;
    uint32_t  height       = 1440;
    uint32_t  fps          = 60;
    TlnkCodec codec        = TLNK_CODEC_HEVC;  /* preferred; downgraded to H.264 if HEVC decode is unavailable */
    uint32_t  bitrateKbps  = 25000;            /* ~25 Mbps default for 1440p60 general use */

    /* Window (initial client size; independent of the stream resolution, which
     * is downscaled to fit). Defaults to 720p; override with --window WxH. */
    uint32_t windowWidth  = 1280;
    uint32_t windowHeight = 720;
    bool startFullscreen = false;
};

/* Process-wide config. Populated once in main(), read everywhere else. */
inline Config& GetConfig() {
    static Config g_config;
    return g_config;
}

} // namespace thunderlink
