#pragma once
#include <filesystem>
namespace DarkRecomp::Ps5 {
class Display;
class GraphicsBackend;
// Runs before allocating the guest. False means the player chose Exit.
bool runGameSetup(Display&, GraphicsBackend&, std::filesystem::path& gameDirectory);
}
