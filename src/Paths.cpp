#include "Paths.hpp"

#include <cstdlib>
#include <system_error>

#ifdef _WIN32
#include <windows.h>
#include <shlobj.h>
#endif

namespace libera_timecode {

namespace {

std::filesystem::path computeSettingsDirectory() {
#ifdef __APPLE__
    const char* home = std::getenv("HOME");
    if (home && *home) {
        return std::filesystem::path(home) / "Library" / "Application Support" / "Libera Timecode";
    }
    return std::filesystem::current_path() / "Libera Timecode";
#elif defined(_WIN32)
    PWSTR path = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &path))) {
        std::filesystem::path result = std::filesystem::path(path) / L"Libera Timecode";
        CoTaskMemFree(path);
        return result;
    }
    return std::filesystem::current_path() / "Libera Timecode";
#else
    if (const char* xdg = std::getenv("XDG_CONFIG_HOME"); xdg && *xdg) {
        return std::filesystem::path(xdg) / "libera-timecode";
    }
    if (const char* home = std::getenv("HOME"); home && *home) {
        return std::filesystem::path(home) / ".config" / "libera-timecode";
    }
    return std::filesystem::current_path() / "libera-timecode";
#endif
}

} // namespace

const std::filesystem::path& settingsDirectory() {
    static const std::filesystem::path dir = []() {
        auto path = computeSettingsDirectory();
        std::error_code ec;
        std::filesystem::create_directories(path, ec);
        return path;
    }();
    return dir;
}

std::filesystem::path settingsFilePath() {
    return settingsDirectory() / "settings.json";
}

} // namespace libera_timecode
