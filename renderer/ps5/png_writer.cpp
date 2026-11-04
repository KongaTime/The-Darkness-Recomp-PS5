#include "png_writer.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <vector>

namespace DarkRecomp::Ps5 {
namespace {

constexpr uint32_t kAdlerMod = 65521;
// A stored deflate block carries at most 65535 bytes, and its five-byte header
// travels with the data it describes.
constexpr size_t kBlockData = 65535;
constexpr uint8_t kSignature[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};

uint32_t crcTable[256];
bool crcTableReady = false;

void buildCrcTable() {
    for (uint32_t n = 0; n < 256; ++n) {
        uint32_t c = n;
        for (unsigned k = 0; k < 8; ++k) c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        crcTable[n] = c;
    }
    crcTableReady = true;
}

// Rolling CRC-32; the caller starts at 0xFFFFFFFF and applies the final xor.
uint32_t crc32(const uint8_t* data, size_t size, uint32_t crc) {
    if (!crcTableReady) buildCrcTable();
    for (size_t i = 0; i < size; ++i) crc = crcTable[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
    return crc;
}

bool writeWord(std::FILE* file, uint32_t value) {
    const uint8_t bytes[4] = {uint8_t(value >> 24), uint8_t(value >> 16), uint8_t(value >> 8), uint8_t(value)};
    return std::fwrite(bytes, 1, sizeof(bytes), file) == sizeof(bytes);
}

bool writeChunk(std::FILE* file, const char* type, const uint8_t* data, size_t size) {
    uint32_t crc = crc32(reinterpret_cast<const uint8_t*>(type), 4, 0xFFFFFFFFu);
    if (size) crc = crc32(data, size, crc);
    crc ^= 0xFFFFFFFFu;
    return writeWord(file, uint32_t(size)) && std::fwrite(type, 1, 4, file) == 4 &&
           (!size || std::fwrite(data, 1, size, file) == size) && writeWord(file, crc);
}

// The zlib stream's Adler-32, accumulated over the filter bytes and rows as
// they are handed to the encoder.
struct Adler {
    uint64_t first = 1, second = 0;
    void update(const uint8_t* data, size_t size) {
        size_t i = 0;
        while (i < size) {
            const size_t run = (std::min)(size - i, size_t(5552));
            for (size_t k = 0; k < run; ++k) {
                first += data[i + k];
                second += first;
            }
            first %= kAdlerMod;
            second %= kAdlerMod;
            i += run;
        }
    }
    uint32_t value() const { return uint32_t((second << 16) | first); }
};

} // namespace

void packRgbRows(const uint8_t* texels, uint32_t width, uint32_t height, bool bgr, uint8_t* rgb) {
    if (!texels || !rgb) return;
    for (uint32_t y = 0; y < height; ++y) {
        const uint8_t* row = texels + size_t(y) * width * 4;
        uint8_t* target = rgb + size_t(y) * width * 3;
        for (uint32_t x = 0; x < width; ++x) {
            const uint8_t* texel = row + size_t(x) * 4;
            target[x * 3] = bgr ? texel[2] : texel[0];
            target[x * 3 + 1] = texel[1];
            target[x * 3 + 2] = bgr ? texel[0] : texel[2];
        }
    }
}

bool writePngRgb(const char* path, uint32_t width, uint32_t height, const uint8_t* pixels,
                 size_t rowStride, const char** error) {
    auto fail = [&](const char* reason) {
        if (error) *error = reason;
        return false;
    };
    if (!path || !pixels || !width || !height) return fail("the frame has no image or no destination");
    if (rowStride < size_t(width) * 3) return fail("the frame's rows are shorter than its width");

    std::FILE* file = std::fopen(path, "wb");
    if (!file) return fail("the frame file could not be opened");

    bool ok = std::fwrite(kSignature, 1, sizeof(kSignature), file) == sizeof(kSignature);

    uint8_t header[13];
    auto store = [&](unsigned at, uint32_t value) {
        header[at] = uint8_t(value >> 24);
        header[at + 1] = uint8_t(value >> 16);
        header[at + 2] = uint8_t(value >> 8);
        header[at + 3] = uint8_t(value);
    };
    store(0, width);
    store(4, height);
    header[8] = 8;  // bit depth
    header[9] = 2;  // colour type: truecolour
    header[10] = 0; // deflate
    header[11] = 0; // adaptive filtering
    header[12] = 0; // no interlace
    ok = ok && writeChunk(file, "IHDR", header, sizeof(header));

    std::vector<uint8_t> block;
    block.reserve(kBlockData + 8);
    Adler adler;
    bool first = true;

    // One IDAT chunk per stored block. The chunks' payloads concatenate into a
    // single zlib stream, so the two-byte zlib header leads the first and the
    // Adler-32 follows the final, empty block.
    auto flushBlock = [&]() {
        if (!ok) return;
        std::vector<uint8_t> chunk;
        chunk.reserve(block.size() + 8);
        if (first) {
            chunk.push_back(0x78); // deflate, 32K window
            chunk.push_back(0x01); // no preset dictionary, fastest
            first = false;
        }
        const uint32_t size = uint32_t(block.size());
        chunk.push_back(0x00); // BFINAL=0, BTYPE=00 (stored)
        chunk.push_back(uint8_t(size));
        chunk.push_back(uint8_t(size >> 8));
        chunk.push_back(uint8_t(~size));
        chunk.push_back(uint8_t((~size) >> 8));
        chunk.insert(chunk.end(), block.begin(), block.end());
        block.clear();
        ok = writeChunk(file, "IDAT", chunk.data(), chunk.size());
    };
    auto push = [&](const uint8_t* data, size_t size) {
        while (ok && size) {
            const size_t take = (std::min)(size, kBlockData - block.size());
            block.insert(block.end(), data, data + take);
            adler.update(data, take);
            data += take;
            size -= take;
            if (block.size() == kBlockData) flushBlock();
        }
    };

    for (uint32_t row = 0; ok && row < height; ++row) {
        const uint8_t filter = 0; // none: the dump stores pixels exactly
        push(&filter, 1);
        push(pixels + size_t(row) * rowStride, size_t(width) * 3);
    }
    if (ok && !block.empty()) flushBlock();

    if (ok) {
        const uint32_t checksum = adler.value();
        uint8_t tail[9];
        tail[0] = 0x01; // BFINAL=1, BTYPE=00
        tail[1] = 0x00;
        tail[2] = 0x00;
        tail[3] = 0xFF;
        tail[4] = 0xFF;
        tail[5] = uint8_t(checksum >> 24);
        tail[6] = uint8_t(checksum >> 16);
        tail[7] = uint8_t(checksum >> 8);
        tail[8] = uint8_t(checksum);
        ok = writeChunk(file, "IDAT", tail, sizeof(tail));
    }
    if (ok) ok = writeChunk(file, "IEND", nullptr, 0);
    if (std::fclose(file) != 0) ok = false;
    return ok ? true : fail("the frame file could not be written");
}

} // namespace DarkRecomp::Ps5
