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
    /* Endpoints */
    std::string bindIP   = TLNK_WIN_IP;   /* our local bind address ("0.0.0.0" = any link) */
    std::string peerIP   = TLNK_MAC_IP;   /* Mac address (InputSender connects here)        */
    uint16_t    videoPort   = TLNK_PORT_VIDEO;
    uint16_t    inputPort   = TLNK_PORT_INPUT;
    uint16_t    controlPort = TLNK_PORT_CONTROL;

    /* Desired stream parameters (sent to the Mac in CONFIGURE) */
    uint32_t  width        = 2560;
    uint32_t  height       = 1440;
    uint32_t  fps          = 60;
    TlnkCodec codec        = TLNK_CODEC_HEVC;  /* preferred; downgraded to H.264 if HEVC decode is unavailable */
    uint32_t  bitrateKbps  = 25000;            /* ~25 Mbps default for 1440p60 general use */

    /* Window */
    bool startFullscreen = false;
};

/* Process-wide config. Populated once in main(), read everywhere else. */
inline Config& GetConfig() {
    static Config g_config;
    return g_config;
}

} // namespace thunderlink
