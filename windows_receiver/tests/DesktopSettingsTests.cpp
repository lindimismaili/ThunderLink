#include "App/DesktopSettings.h"
#include <cassert>
#include <chrono>
#include <iostream>

int main() {
    using namespace thunderlink;
    const auto directory = std::filesystem::temp_directory_path() /
        ("thunderlink-settings-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    const auto path = directory / "portable space" / "settings.json";
    try {
        assert(LoadSettings(path).peerIP == Config{}.peerIP);
        Config saved = ParseArguments({"--peer", "10.0.0.2", "--control-port", "54321", "--fps", "30", "--vsync"});
        SaveSettings(saved, path);
        const Config loaded = LoadSettings(path);
        const Config overridden = ParseArguments({"--fps", "60"}, loaded);
        assert(overridden.fps == 60 && overridden.peerIP == "10.0.0.2");
        assert(overridden.controlPort == 54321 && overridden.vsync);
        assert(LoadSettings(path).fps == 30);
        assert(!ParseArguments({"--no-vsync"}, loaded).vsync);
        Config bad = saved; bad.controlPort = 0;
        bool rejected = false;
        try { SaveSettings(bad, path); } catch (...) { rejected = true; }
        assert(rejected && LoadSettings(path).controlPort == 54321);
        { std::ofstream output(path); output << "broken json"; }
        rejected = false;
        try { (void)LoadSettings(path); } catch (...) { rejected = true; }
        assert(rejected);
        rejected = false;
        try { SaveSettings(saved, path / "child"); } catch (...) { rejected = true; }
        assert(rejected);
        std::filesystem::remove_all(directory);
    } catch (...) { std::filesystem::remove_all(directory); throw; }
    std::cout << "Desktop settings tests passed\n";
}
