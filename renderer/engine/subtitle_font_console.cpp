// Subtitle font replacement on the console (subtitle_font.h).
//
// The Windows build re-rasterizes the game's subtitle and title fonts with
// DirectWrite at the render resolution. The console has no system font
// rasterizer, so no font is registered for replacement and every glyph keeps
// the game's own authored atlas, which is the header's documented fallback.
#include "subtitle_font.h"

namespace DarkRecomp::Native {

bool subtitleFontResourceAllowsReplacement(uint8_t*, uint32_t) noexcept { return false; }

std::optional<uint32_t> westernSubtitleCodepoint(uint16_t) noexcept { return std::nullopt; }

std::optional<uint32_t> russianSubtitleCodepoint(uint16_t) noexcept { return std::nullopt; }

void registerSubtitleFont(uint8_t*, uint32_t, bool) noexcept {}

void ensureSubtitleFontRegistered(uint8_t*, uint32_t) noexcept {}

unsigned subtitleFontRasterScale(unsigned, unsigned) noexcept { return 1; }

std::shared_ptr<const ColorImage> subtitleFontReplacementFor(uint32_t, const std::shared_ptr<const ColorImage>&,
                                                            unsigned) noexcept {
    return nullptr;
}

std::shared_ptr<const ColorImage> subtitleFontReplacementFor(uint32_t, const std::shared_ptr<const AlphaImage>&,
                                                            unsigned) noexcept {
    return nullptr;
}

} // namespace DarkRecomp::Native
