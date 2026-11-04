#pragma once
// The console's frame dump: a PNG writer with no dependency behind it.
//
// The console has neither zlib nor libpng, and the SDK ships both absent, so a
// frame dump writes PNG's required pieces itself: the signature, IHDR, an IDAT
// stream of stored (uncompressed) deflate blocks inside the zlib wrapper, and
// IEND. Stored blocks keep the encoder exact and small; the cost is size, about
// 25 MB for a 4K frame, which is the right trade for a handful of diagnostic
// frames and wrong for a recording.
#include <cstddef>
#include <cstdint>

namespace DarkRecomp::Ps5 {

// Writes an 8-bit RGB PNG. `pixels` holds `height` rows of `rowStride` bytes,
// three bytes per pixel in red, green, blue order. Returns false and sets
// `error` (when non-null) if the file cannot be written.
bool writePngRgb(const char* path, uint32_t width, uint32_t height, const uint8_t* pixels,
                 size_t rowStride, const char** error);

// Repacks a 32-bit surface into the tight RGB rows writePngRgb takes. The
// console's swapchain reports either B8G8R8A8 (the usual display format) or
// R8G8B8A8; both name the same four bytes per pixel in a different order.
// `texels` is `height` rows of `width` 4-byte pixels, and `rgb` receives
// `width * height * 3` bytes.
void packRgbRows(const uint8_t* texels, uint32_t width, uint32_t height, bool bgr, uint8_t* rgb);

} // namespace DarkRecomp::Ps5
