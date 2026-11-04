#pragma once
#include <cstdint>
#include <filesystem>
#include <functional>
#include <span>
#include <string>
#include <vector>

namespace DarkRecomp::Ps5 {
struct IsoEntry {
    std::filesystem::path path;
    uint64_t offset = 0, size = 0;
    bool directory = false;
};
struct IsoPlan {
    std::filesystem::path source;
    uint64_t sourceSize = 0, bytes = 0;
    std::string executableHash;
    std::vector<IsoEntry> entries;
};
using ImportProgress = std::function<void(uint64_t, uint64_t, const std::string&)>;
using VerifyExecutable = std::function<void(std::span<const uint8_t>)>;
// Inspection is read-only, bounded, and verifies default.xex before any extraction.
IsoPlan inspectGameIso(const std::filesystem::path&, const VerifyExecutable&);
// Extract beside destination, then rename only a complete tree into place.
// Never merges with or overwrites an existing nonempty destination.
void extractGameIso(const IsoPlan&, const std::filesystem::path&, const ImportProgress& = {});
std::vector<std::filesystem::path> findGameIsos(const std::vector<std::filesystem::path>& roots);
}
