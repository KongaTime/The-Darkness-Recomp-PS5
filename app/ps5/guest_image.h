#pragma once
// Loading The Darkness into a guest address space, on the console.
//
// The Windows build loads the image inside Memory (runtime.h), which also owns
// the D3D11 display, the XMA bridge and the native menus. The console title
// needs only the part the CPU emulation depends on: the verified image, the XEX
// header, the static TLS template, the AOT dispatch table and the main guest
// thread's stack and PCR. This header is that part, kept free of the renderer
// so it can be linked before a graphics backend exists.
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

struct PPCContext;

namespace DarkRecomp::Ps5 {

// Where the guest stack, its PCR and the thread object live. These are the
// addresses the Windows build's initThread commits, unchanged: engine code
// reads them as absolute guest addresses.
inline constexpr uint32_t kStackLow = 0x70000000;
inline constexpr uint32_t kStackSize = 0x01000000;
inline constexpr uint32_t kPcr = 0x7ff00000;
inline constexpr uint32_t kThreadObjectOffset = 0xd00;

class GuestImage {
public:
    // Reads and verifies `default.xex` in `gameDirectory`, copies the decoded
    // image into `base` at its guest address, publishes the AOT dispatch table
    // and reads the static TLS template. Returns false and sets `error` on any
    // failure; a rejected dump never reaches the guest.
    bool Load(uint8_t* base, const std::filesystem::path& gameDirectory, std::string& error);

    // Resets `ctx` for the main guest thread and points it at the committed
    // stack and PCR, filling the thread object the way initThread does.
    void InitMainThread(uint8_t* base, PPCContext& ctx, uint32_t threadId) const;

    const char* revision() const { return revision_; }
    size_t nativeFunctions() const { return nativeFunctions_; }

    // The static TLS template, for the other guest threads the title creates
    // (the audio worker); InitThread copies it into that thread's PCR.
    const std::vector<uint8_t>& tlsTemplate() const { return tlsTemplate_; }

private:
    std::vector<uint8_t> tlsTemplate_;
    uint32_t headerSize_ = 0;
    const char* revision_ = nullptr;
    size_t nativeFunctions_ = 0;
};

} // namespace DarkRecomp::Ps5
