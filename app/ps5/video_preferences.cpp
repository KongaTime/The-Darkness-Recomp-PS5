#include "video_preferences.h"
#include <charconv>
#include <fstream>
#include <string>

namespace DarkRecomp::Ps5 {
VideoPreferences loadVideoPreferences(const std::filesystem::path& path) {
    VideoPreferences result;
    auto& g = result.graphics;
    g.verticalSync = true;
    std::ifstream input(path);
    std::string line;
    while (std::getline(input, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const auto split = line.find('=');
        if (split == std::string::npos) continue;
        const auto key = line.substr(0, split);
        unsigned value = 0;
        const auto [end, error] = std::from_chars(line.data() + split + 1, line.data() + line.size(), value);
        if (error != std::errc{} || end != line.data() + line.size()) continue;
        if (key == "FrameRateLimit" && (value == 0 || value == 30 || value == 60)) g.frameRateLimit = value;
        if (key == "RenderHeight" && (value == 360 || value == 480 || value == 720 || value == 1080 || value == 1440 || value == 2160)) g.renderHeight = value;
        if (key == "BrightnessPercent" && value >= 50 && value <= 200) g.brightnessPercent = value;
        if (key == "Bloom" && value <= 1) g.bloom = value;
        if (key == "MotionBlur" && value <= 1) g.motionBlur = value;
        if (key == "Antialiasing" && value <= 1) g.antialiasing = Native::AntialiasingMode(value);
        if (key == "AnisotropyLevels" && Native::validAnisotropyLevels(value)) g.anisotropyLevels = value;
        if (key == "SubtitleScalePercent" && value >= 50 && value <= 200) g.subtitleScalePercent = value;
        if (key == "FieldOfView" && (value == 0 || (value >= 60 && value <= 120))) result.fieldOfView = value;
        if (key == "Language" && (value <= 1 || (value >= 3 && value <= 6))) result.language = value;
    }
    return result;
}

bool saveVideoPreferences(const std::filesystem::path& path, const VideoPreferences& p) {
    if (!Native::validGraphicsSettings(p.graphics)) return false;
    const auto temporary = std::filesystem::path(path.string() + ".tmp");
    const auto& g = p.graphics;
    std::ofstream output(temporary, std::ios::trunc);
    output << "FrameRateLimit=" << g.frameRateLimit << "\nRenderHeight=" << g.renderHeight
           << "\nBrightnessPercent=" << g.brightnessPercent << "\nBloom=" << g.bloom
           << "\nMotionBlur=" << g.motionBlur << "\nAntialiasing=" << unsigned(g.antialiasing)
           << "\nAnisotropyLevels=" << g.anisotropyLevels << "\nSubtitleScalePercent=" << g.subtitleScalePercent
           << "\nFieldOfView=" << p.fieldOfView << "\nLanguage=" << p.language << '\n';
    output.flush();
    const bool written = bool(output);
    output.close();
    if (!written || output.fail()) return false;
    std::error_code error;
    std::filesystem::rename(temporary, path, error);
    return !error;
}
}
