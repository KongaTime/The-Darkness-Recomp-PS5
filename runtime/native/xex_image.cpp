#include "xex_image.h"
#include "xex_crypto_portable.h"
#include <algorithm>
#include <array>
#include <cstring>
#include <stdexcept>
#include <string>

namespace DarkRecomp::Native {
namespace {
constexpr std::array<uint8_t, 16> retailKey = {
    0x20, 0xb1, 0x85, 0xa5, 0x9d, 0x28, 0xfd, 0xc3,
    0x40, 0x58, 0x3f, 0xbb, 0x08, 0x96, 0xbf, 0x91};
constexpr size_t maxImage = 64 * 1024 * 1024;
void require(bool ok, const char* message) {
    if (!ok) throw std::runtime_error(message);
}
uint32_t be32(std::span<const uint8_t> b, size_t p) {
    require(p <= b.size() && b.size() - p >= 4, "Truncated XEX field");
    return uint32_t(b[p]) << 24 | uint32_t(b[p+1]) << 16 | uint32_t(b[p+2]) << 8 | b[p+3];
}
// The loader's two cryptographic steps, shared with the console build: bcrypt
// is not on the console, and a second implementation would have to be proven
// against the same dumps a second time.
std::string digest(std::span<const uint8_t> bytes) {
    require(!bytes.empty(), "Cannot verify an empty game image hash");
    return XexCrypto::Sha256Hex(bytes);
}
void decrypt(std::vector<uint8_t>& bytes, std::span<const uint8_t, 16> key) {
    require(!bytes.empty() && bytes.size() % 16 == 0, "Invalid encrypted XEX length");
    XexCrypto::DecryptCbc(bytes, key);
}
}

XexImage decodeXex(std::span<const uint8_t> bytes) {
    require(bytes.size() >= 24 && bytes.size() <= maxImage && be32(bytes, 0) == 0x58455832,
            "Invalid XEX2 file");
    const size_t headerSize = be32(bytes, 8), security = be32(bytes, 16);
    require(headerSize >= 24 && headerSize <= bytes.size() && headerSize <= 0x100000,
            "Invalid XEX header size");
    auto header = bytes.first(headerSize);
    require(security >= 24 && security <= headerSize && headerSize - security >= 0x180,
            "Invalid XEX security header");
    const size_t imageSize = be32(header, security + 4);
    require(imageSize > 0 && imageSize <= maxImage && be32(header, security + 0x110) == 0x82000000,
            "Unsupported XEX image layout");
    const size_t count = be32(header, 20);
    require(count <= (headerSize - 24) / 8, "Invalid XEX optional header count");
    size_t format = 0;
    for (size_t i = 0; i < count; ++i) {
        if (be32(header, 24 + i * 8) == 0x3ff) {
            require(!format, "Duplicate XEX format header");
            format = be32(header, 28 + i * 8);
        }
    }
    require(format >= 24 && format <= headerSize && headerSize - format >= 8,
            "Missing XEX format header");
    const size_t formatSize = be32(header, format);
    require(formatSize >= 8 && formatSize <= headerSize - format, "Invalid XEX format size");
    const uint32_t types = be32(header, format + 4);
    const uint32_t encryption = types >> 16, compression = types & 0xffff;
    require(encryption <= 1 && compression <= 1, "Unsupported XEX encryption/compression for this port");
    std::vector<uint8_t> payload(bytes.begin() + headerSize, bytes.end());
    if (encryption) {
        std::vector<uint8_t> session(header.begin() + security + 0x150, header.begin() + security + 0x160);
        decrypt(session, retailKey);
        decrypt(payload, std::span<const uint8_t, 16>(session.data(), 16));
        // The decrypted session key is key material: clear it without letting
        // the compiler drop the write.
        volatile uint8_t* scratch = session.data();
        for (size_t i = 0; i < session.size(); ++i) scratch[i] = 0;
    }
    XexImage result{{header.begin(), header.end()}, std::vector<uint8_t>(imageSize)};
    if (!compression) {
        require(payload.size() >= imageSize, "Truncated XEX image");
        std::copy_n(payload.begin(), imageSize, result.image.begin());
    } else {
        require(formatSize >= 16 && (formatSize - 8) % 8 == 0, "Invalid XEX block table");
        size_t input = 0, output = 0;
        for (size_t block = format + 8; block < format + formatSize; block += 8) {
            const size_t data = be32(header, block), zeros = be32(header, block + 4);
            require(data <= payload.size() - input && data <= imageSize - output &&
                    zeros <= imageSize - output - data, "Invalid XEX block range");
            std::copy_n(payload.begin() + input, data, result.image.begin() + output);
            input += data;
            output += data + zeros;
        }
        // The security image size includes a final zero-filled region that
        // need not have a block-table entry (0x8000 bytes in this revision).
        require(payload.size() - input < 16, "Unexpected trailing XEX payload");
    }
    require(result.image.size() >= 2 && result.image[0] == 'M' && result.image[1] == 'Z',
            "Decoded XEX image is invalid");
    return result;
}
XexImage decodeVerifiedXex(std::span<const uint8_t> bytes, std::span<const GameRevision> revisions) {
    require(bytes.size() >= 24 && bytes.size() <= maxImage, "Invalid default.xex size");
    const auto fileHash = digest(bytes);
    const auto revision = std::find_if(revisions.begin(), revisions.end(), [&](const GameRevision& item) {
        return fileHash == item.xexSha256;
    });
    require(revision != revisions.end(), "default.xex differs from the supported AOT revision");
    auto decoded = decodeXex(bytes);
    require(digest(decoded.image) == revision->imageSha256,
            "Game image differs from the verified AOT revision");
    decoded.revisionName = revision->name;
    return decoded;
}
}
