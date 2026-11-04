#pragma once
#include "runtime/native/graphics_settings.h"
#include <filesystem>

namespace DarkRecomp::Ps5 {
struct VideoPreferences {
    Native::GraphicsSettings graphics;
    unsigned fieldOfView = 0;
    unsigned language = 0;
};
VideoPreferences loadVideoPreferences(const std::filesystem::path& path);
bool saveVideoPreferences(const std::filesystem::path& path, const VideoPreferences& preferences);
constexpr unsigned renderScaleForHeight(unsigned height) { return (height + 719) / 720; }
}
