#include "app/ps5/video_preferences.h"
#include <cassert>
#include <fstream>
#include <iostream>

int main(int argc, char** argv) {
    using namespace DarkRecomp::Ps5;
    assert(argc == 2);
    std::filesystem::path file = argv[1];
    std::filesystem::remove(file);
    assert(loadVideoPreferences(file).graphics.renderHeight == 720);
    VideoPreferences chosen;
    chosen.graphics.renderHeight = 1440;
    chosen.graphics.bloom = false;
    chosen.graphics.motionBlur = true;
    chosen.graphics.antialiasing = DarkRecomp::Native::AntialiasingMode::FXAA;
    chosen.graphics.brightnessPercent = 115;
    chosen.graphics.anisotropyLevels = 8;
    chosen.graphics.subtitleScalePercent = 150;
    chosen.graphics.frameRateLimit = 30;
    chosen.graphics.verticalSync = true;
    chosen.fieldOfView = 95;
    chosen.language = 4;
    assert(saveVideoPreferences(file, chosen));
    auto loaded = loadVideoPreferences(file);
    assert(loaded.graphics == chosen.graphics && loaded.fieldOfView == 95 && loaded.language == 4);
    chosen.graphics.renderHeight = 1080;
    assert(saveVideoPreferences(file, chosen));
    assert(loadVideoPreferences(file).graphics.renderHeight == 1080);
    { std::ofstream out(file); out << "RenderHeight=bogus\nFrameRateLimit=-1\nBrightnessPercent=999999999999999\nAntialiasing=5\nAnisotropyLevels=3\nFieldOfView=0\n"; }
    loaded = loadVideoPreferences(file);
    assert(loaded.graphics.renderHeight == 720 && loaded.graphics.frameRateLimit == 60);
    assert(loaded.graphics.brightnessPercent == 100 && loaded.graphics.anisotropyLevels == 16);
    assert(loaded.graphics.antialiasing == DarkRecomp::Native::AntialiasingMode::Off);
    assert(renderScaleForHeight(720) == 1 && renderScaleForHeight(1080) == 2 && renderScaleForHeight(1440) == 2 && renderScaleForHeight(2160) == 3);
    std::filesystem::remove(file);
    std::cout << "PS5 video: persistence, replacement, invalid values, and render scales pass.\n";
}
