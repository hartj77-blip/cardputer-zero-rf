// Launcher list for the RF edition: RF apps first, then the base OS's apps.
// Replaces base/shell/src/apps/registry.cpp, which is left out of this build.
// When the base pin moves, compare with that file for added or removed apps.
#include "ui/app.h"

namespace zs {

// RF apps (this repository).
std::unique_ptr<App> make_spectrum_app();
std::unique_ptr<App> make_sensors_app();
std::unique_ptr<App> make_scanner_app();

// Base apps.
std::unique_ptr<App> make_settings_app();
std::unique_ptr<App> make_wifi_app();
std::unique_ptr<App> make_media_app();
std::unique_ptr<App> make_files_app();
std::unique_ptr<App> make_notes_app();
std::unique_ptr<App> make_ir_app();
std::unique_ptr<App> make_compass_app();
std::unique_ptr<App> make_battery_app();
std::unique_ptr<App> make_clock_app();
std::unique_ptr<App> make_sysmon_app();
std::unique_ptr<App> make_dock_app();
std::unique_ptr<App> make_terminal_app();
std::unique_ptr<App> make_remote_app();
std::unique_ptr<App> make_sync_app();
std::unique_ptr<App> make_gps_app();
std::unique_ptr<App> make_mesh_app();

const std::vector<AppInfo>& app_registry() {
    static const std::vector<AppInfo> apps = {
        {"spectrum", "Spectrum", LV_SYMBOL_BARS, 0x39c5cf, make_spectrum_app},
        {"scanner", "Scanner", LV_SYMBOL_REFRESH, 0x3fb950, make_scanner_app},
        {"sensors", "Sensors", LV_SYMBOL_TINT, 0x58a6ff, make_sensors_app},

        {"terminal", "Terminal", LV_SYMBOL_KEYBOARD, 0x3fb950, make_terminal_app},
        {"remote", "Remote", LV_SYMBOL_SHUFFLE, 0x39c5cf, make_remote_app},
        {"sync", "Sync", LV_SYMBOL_LOOP, 0xa371f7, make_sync_app},
        {"files", "Files", LV_SYMBOL_DIRECTORY, 0x58a6ff, make_files_app},
        {"notes", "Notes", LV_SYMBOL_EDIT, 0xe3b341, make_notes_app},
        {"media", "Media", LV_SYMBOL_AUDIO, 0xff7eb6, make_media_app},
        {"wifi", "Wi-Fi", LV_SYMBOL_WIFI, 0x3fb950, make_wifi_app},
        {"ir", "IR Remote", LV_SYMBOL_POWER, 0xf85149, make_ir_app},
        {"gps", "GPS", LV_SYMBOL_GPS, 0x3fb950, make_gps_app},
        {"mesh", "Mesh", LV_SYMBOL_ENVELOPE, 0xff8a1f, make_mesh_app},
        {"compass", "Compass", LV_SYMBOL_GPS, 0xff8a1f, make_compass_app},
        {"battery", "Battery", LV_SYMBOL_BATTERY_3, 0x3fb950, make_battery_app},
        {"clock", "Clock", LV_SYMBOL_BELL, 0xbc8cff, make_clock_app},
        {"dock", "Dock", LV_SYMBOL_IMAGE, 0x58a6ff, make_dock_app},
        {"sysmon", "System", LV_SYMBOL_LIST, 0x39c5cf, make_sysmon_app},
        {"settings", "Settings", LV_SYMBOL_SETTINGS, 0x8b97a5, make_settings_app},
    };
    return apps;
}

}  // namespace zs
