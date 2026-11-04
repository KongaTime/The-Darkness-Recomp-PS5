#include "guest_image.h"

#include "ppc_config.h"
#include "ppc_context.h"
#include "ppc_image_metadata.h"
#include "runtime/native/ps5_thread.h"
#include "runtime/native/xex_image.h"

#include <cstring>
#include <fstream>
#include <iterator>
#include <stdexcept>

namespace DarkRecomp::Ps5 {
namespace {

// The XEX header lives at a fixed guest address; engine code resolves the
// module handle through it, so the loader must publish it there.
constexpr uint32_t kXexHeader = 0x81000000;
constexpr uint32_t kXexHeaderCapacity = 0x100000;
// The main thread's stack, PCR and thread object (guest_image.h).
constexpr uint32_t kTlsOffset = Native::kGuestTlsOffset;

// Guest words are big-endian, exactly as the engine reads them.
uint32_t read32(const uint8_t* base, uint32_t address) {
    uint32_t value = 0;
    std::memcpy(&value, base + address, sizeof(value));
    return __builtin_bswap32(value);
}

void write32(uint8_t* base, uint32_t address, uint32_t value) {
    value = __builtin_bswap32(value);
    std::memcpy(base + address, &value, sizeof(value));
}

// A field of the published XEX header, or 0 when absent. Single-word fields
// store the value in place; every other key stores an offset from the header.
uint32_t headerField(const uint8_t* base, uint32_t headerSize, uint32_t key) {
    const uint32_t count = read32(base, kXexHeader + 20);
    if (uint64_t(count) * 8 + 24 > headerSize) throw std::runtime_error("Invalid XEX optional header count");
    for (uint32_t i = 0; i < count; ++i) {
        const uint32_t entry = kXexHeader + 24 + i * 8;
        if (read32(base, entry) != key) continue;
        if ((key & 0xff) <= 1) return entry + 4;
        const uint32_t offset = read32(base, entry + 4);
        if (offset >= headerSize) throw std::runtime_error("XEX field lies outside its header");
        return kXexHeader + offset;
    }
    return 0;
}

} // namespace

bool GuestImage::Load(uint8_t* base, const std::filesystem::path& gameDirectory, std::string& error) {
    try {
        if (base == nullptr) throw std::runtime_error("No guest address space");
        const auto xexPath = gameDirectory / "default.xex";
        std::ifstream file(xexPath, std::ios::binary);
        if (!file) throw std::runtime_error("No default.xex in " + gameDirectory.string());
        const std::vector<uint8_t> original((std::istreambuf_iterator<char>(file)),
                                            std::istreambuf_iterator<char>());
        if (original.size() < 24 || original.size() > 64 * 1024 * 1024)
            throw std::runtime_error("Invalid default.xex size");
        // An unapproved dump stops here: the AOT code is only valid for the
        // revisions in the catalog.
        const auto decoded = Native::decodeVerifiedXex(original, kGameRevisions);
        revision_ = decoded.revisionName;
        if (decoded.image.size() > PPC_IMAGE_SIZE) throw std::runtime_error("Game image exceeds its region");
        std::memcpy(base + PPC_IMAGE_BASE, decoded.image.data(), decoded.image.size());

        headerSize_ = uint32_t(decoded.header.size());
        if (headerSize_ > kXexHeaderCapacity) throw std::runtime_error("Invalid XEX header size");
        std::memcpy(base + kXexHeader, decoded.header.data(), headerSize_);

        // Static TLS: the template every guest thread copies from.
        const uint32_t tlsHeader = headerField(base, headerSize_, 0x20104);
        if (!tlsHeader || read32(base, tlsHeader) != 64)
            throw std::runtime_error("Unsupported XEX TLS slot count");
        const uint32_t rawAddress = read32(base, tlsHeader + 4);
        const uint32_t size = read32(base, tlsHeader + 8);
        const uint32_t rawSize = read32(base, tlsHeader + 12);
        if (size > kThreadObjectOffset - kTlsOffset - 256 || rawSize > size || rawAddress < PPC_IMAGE_BASE ||
            uint64_t(rawAddress) + rawSize > uint64_t(PPC_IMAGE_BASE) + PPC_IMAGE_SIZE)
            throw std::runtime_error("Invalid XEX static TLS template");
        tlsTemplate_.resize(size);
        std::memcpy(tlsTemplate_.data(), base + rawAddress, rawSize);

        // The dispatch table sits directly above the image. Clear it so an
        // address whose function was never translated is found to be null
        // instead of pointing at whatever the mapping left behind.
        uint8_t* const table = base + PPC_IMAGE_BASE + PPC_IMAGE_SIZE;
        std::memset(table, 0, size_t(PPC_CODE_SIZE) * 2);
        nativeFunctions_ = 0;
        for (auto* entry = PPCFuncMappings; entry->host; ++entry) {
            if ((entry->guest & 3) || entry->guest < PPC_CODE_BASE ||
                entry->guest >= PPC_CODE_BASE + PPC_CODE_SIZE)
                throw std::runtime_error("Invalid address in the AOT function manifest");
            PPC_LOOKUP_FUNC(base, entry->guest) = entry->host;
            ++nativeFunctions_;
        }
        return true;
    } catch (const std::exception& failure) {
        error = failure.what();
        return false;
    }
}

void GuestImage::InitMainThread(uint8_t* base, PPCContext& ctx, uint32_t threadId) const {
    Native::initGuestThread(base, ctx, tlsTemplate_.data(), tlsTemplate_.size(), threadId,
                            kStackLow, kStackSize, kPcr);
}

} // namespace DarkRecomp::Ps5
