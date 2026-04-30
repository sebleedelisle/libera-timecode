#pragma once

#include <filesystem>
#include <string>

namespace libera_timecode {

// Per-user settings directory. Created on first call.
const std::filesystem::path& settingsDirectory();

std::filesystem::path settingsFilePath();

} // namespace libera_timecode
