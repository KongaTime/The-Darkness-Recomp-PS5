#pragma once
// The decisions the console's world render targets make, without a GPU.
//
// renderer/d3d11/world_renderer.cpp owns the engine's render targets on the
// Windows build. WorldRendererD3D11::surface() allocates and grows them,
// clear() turns one engine clear into render-target operations, resolve()
// copies a surface into the guest's texture and present() hands the frontbuffer
// to the display. The console has RADV instead of D3D11, but the decisions --
// how a surface grows, which rectangle a resolve copies and how far its fringe
// may run past the surface, which guest format maps to which texture, which
// pixels a clear touches -- are the same arithmetic on both.
//
// They live here, free of Vulkan and of D3D11, so the host build can check them
// (tests/world_surface_policy_tests.cpp): renderer/ps5/world_surfaces.cpp asks
// these functions what to do and only carries out the answer, and its reject
// reasons keep the Windows renderer's numbering so a console log line means the
// same thing as a Windows one.
//
// The reference behaviour is WorldRendererD3D11::surface/clear/resolve/present;
// where a rule reads oddly it is because the original does that, not because
// the console needs it.
#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>

namespace DarkRecomp::Ps5::WorldSurfacePolicy {

// The same extent limit the Windows renderer enforces
// (D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION), and the Vulkan renderer's as well.
constexpr uint32_t kMaxExtent = 4096;

struct Extent {
    uint32_t width = 0, height = 0;
    bool operator==(const Extent&) const = default;
};

// A surface never shrinks. The guest reuses its render targets for a whole
// scene, and a reallocated smaller surface would drop pixels a later resolve
// still expects. A zero or over-limit request is refused; the caller reports it
// rather than allocating something the GPU cannot hold.
bool grow(Extent requested, Extent current, Extent& out) noexcept;

// The four guest formats the engine resolves into, and what the console draws
// them as. `depth` is the attachment's kind: the single-channel float the
// engine uses to read depth back only exists for a depth attachment.
enum class DestinationFormat { unsupported = 0, rgba8, rgb10a2, rgba16, r32f };

DestinationFormat destinationFormat(uint32_t guestFormat, bool depth) noexcept;

// The original resolve rounds its copy's right and bottom edges up to eight
// pixels, because the tile it reads is eight wide.
constexpr uint32_t kResolveAlignment = 8;
// The largest undrawn fringe the original tolerates before it refuses the copy
// (a 1290-pixel frontbuffer tile against a 1288-pixel drawn extent at 1080p).
constexpr uint32_t kMaxResolveFringe = 7;

struct ResolveRequest {
    uint32_t flags = 0; // bits 0..2 select the attachment: 0..3 colour, 4 depth.
    std::array<uint32_t, 4> rectangle{};
    std::array<uint32_t, 2> offset{};
    uint32_t destinationWidth = 0, destinationHeight = 0;
    uint32_t destinationFormat = 0;
    uint32_t faces = 1, face = 0, mip = 0;
    // The attachment's retained extent, zero when nothing has drawn or cleared
    // it yet. A resolve never allocates its source.
    Extent source{};
    // The viewport the source was drawn with; the fringe may not leave it.
    std::array<uint32_t, 4> viewport{};
};

// Numbered like WorldRendererD3D11::resolve's reject() reasons.
enum class ResolveStatus : uint32_t {
    ok = 0,
    descriptor = 1,   // attachment slot, destination identity/extent, mip or face
    sourceMissing = 2, // no surface retained for the attachment
    depthFormat = 3,  // a single-channel float asked of a colour attachment
    format = 4,       // a guest format the renderer cannot resolve into
    emptyRectangle = 5,
    sourceExtent = 6, // the copy leaves the surface by more than its fringe
    destinationExtent = 7,
};

const char* resolveStatusName(ResolveStatus) noexcept;

struct ResolvePlan {
    // The fringe extends past the retained extent, so the surface must grow
    // before the copy (the original grows it rather than dropping the tile).
    bool growSource = false;
    Extent source{}; // the extent the copy reads, after any growth
    // The rounded, clipped rectangle in surface pixels: x0, y0, x1, y1.
    std::array<uint32_t, 4> copied{};
    // Where the copy starts in the destination, in destination pixels.
    std::array<uint32_t, 2> offset{};
    DestinationFormat format = DestinationFormat::unsupported;
    bool depth = false;
    uint32_t face = 0;
};

// Fills `plan` on success. A failed plan leaves it untouched: the caller logs
// the reason and skips the command, as the Windows renderer does.
ResolveStatus planResolve(const ResolveRequest& request, ResolvePlan& plan) noexcept;

// The region the original clears *after* a resolve: its rounded rectangle,
// before that is clipped to the surface by planClear.
std::array<int32_t, 4> expandedResolveRectangle(const std::array<uint32_t, 4>& rectangle) noexcept;

struct ClearRequest {
    uint32_t flags = 0; // bit 0 colour, bit 4 depth, bit 5 stencil.
    // x, y, width, height. The extent a clear creates its surface at.
    std::array<uint32_t, 4> viewport{};
    // A completed partial clear, in surface pixels; absent means the whole
    // surface.
    std::optional<std::array<int32_t, 4>> rectangle;
    Extent surface{}; // the retained extent
};

struct ClearRegion {
    bool valid = false;
    // The whole surface: the original uses ClearRenderTargetView /
    // ClearDepthStencilView, which do not take a rectangle.
    bool full = false;
    std::array<int32_t, 4> rect{}; // left, top, right, bottom, in surface pixels
};

ClearRegion planClear(const ClearRequest&) noexcept;

// A resolved frontbuffer can be presented while it has pixels and there is a
// display to fill. The copy fills the display: at equal extents it is exact, and
// at a different one it is scaled, which is what the Windows display path does
// when it fits its presentation target to the window.
bool presentable(Extent resolved, Extent display) noexcept;

// ---------------------------------------------------------------------------
// Implementation
// ---------------------------------------------------------------------------

inline bool grow(Extent requested, Extent current, Extent& out) noexcept {
    if (!requested.width || !requested.height) return false;
    if (requested.width > kMaxExtent || requested.height > kMaxExtent) return false;
    out.width = (std::max)(requested.width, current.width);
    out.height = (std::max)(requested.height, current.height);
    return out.width <= kMaxExtent && out.height <= kMaxExtent;
}

inline DestinationFormat destinationFormat(uint32_t guestFormat, bool depth) noexcept {
    switch (guestFormat) {
        case 6: return DestinationFormat::rgba8;
        case 54: return DestinationFormat::rgb10a2;
        case 26: return DestinationFormat::rgba16;
        // The original display PWL ramp consumes all ten frontbuffer bits, so a
        // 10-bit guest surface is never narrowed here; the swapchain's own
        // format is a separate conversion at present time.
        case 23: return depth ? DestinationFormat::r32f : DestinationFormat::unsupported;
        default: return DestinationFormat::unsupported;
    }
}

inline const char* resolveStatusName(ResolveStatus status) noexcept {
    switch (status) {
        case ResolveStatus::ok: return "ok";
        case ResolveStatus::descriptor: return "descriptor";
        case ResolveStatus::sourceMissing: return "source-missing";
        case ResolveStatus::depthFormat: return "depth-format";
        case ResolveStatus::format: return "format";
        case ResolveStatus::emptyRectangle: return "empty-rectangle";
        case ResolveStatus::sourceExtent: return "source-extent";
        case ResolveStatus::destinationExtent: return "destination-extent";
    }
    return "unknown";
}

inline std::array<int32_t, 4> expandedResolveRectangle(const std::array<uint32_t, 4>& rectangle) noexcept {
    const uint32_t mask = kResolveAlignment - 1u;
    return {int32_t(rectangle[0]), int32_t(rectangle[1]),
            int32_t((rectangle[2] + mask) & ~mask), int32_t((rectangle[3] + mask) & ~mask)};
}

inline ResolveStatus planResolve(const ResolveRequest& request, ResolvePlan& plan) noexcept {
    const uint32_t attachment = request.flags & 7u;
    if (attachment > 4u || !request.destinationWidth || !request.destinationHeight ||
        request.destinationWidth > kMaxExtent || request.destinationHeight > kMaxExtent ||
        request.mip || (request.faces != 1u && request.faces != 6u) || request.face >= request.faces)
        return ResolveStatus::descriptor;
    if (!request.source.width || !request.source.height) return ResolveStatus::sourceMissing;
    if (request.rectangle[0] >= request.rectangle[2] || request.rectangle[1] >= request.rectangle[3])
        return ResolveStatus::emptyRectangle;

    const bool depth = attachment == 4u;
    Extent source = request.source;
    bool growSource = false;
    if (request.rectangle[2] > source.width || request.rectangle[3] > source.height) {
        // A guest frontbuffer can ask for a final partial tile past the drawn
        // extent. The original keeps the drawn pixels, clears the undrawn fringe
        // and copies the enlarged tile; anything further is a real mismatch.
        const uint32_t missingWidth =
            request.rectangle[2] > source.width ? request.rectangle[2] - source.width : 0u;
        const uint32_t missingHeight =
            request.rectangle[3] > source.height ? request.rectangle[3] - source.height : 0u;
        if (missingWidth > kMaxResolveFringe || missingHeight > kMaxResolveFringe ||
            uint64_t(request.rectangle[2]) >
                uint64_t(request.viewport[0]) + uint64_t(request.viewport[2]) ||
            uint64_t(request.rectangle[3]) >
                uint64_t(request.viewport[1]) + uint64_t(request.viewport[3]))
            return ResolveStatus::sourceExtent;
        Extent grown;
        if (!grow({request.rectangle[2], request.rectangle[3]}, source, grown))
            return ResolveStatus::sourceExtent;
        source = grown;
        growSource = true;
    }
    if (uint64_t(request.offset[0]) + uint64_t(request.rectangle[2] - request.rectangle[0]) >
            request.destinationWidth ||
        uint64_t(request.offset[1]) + uint64_t(request.rectangle[3] - request.rectangle[1]) >
            request.destinationHeight)
        return ResolveStatus::destinationExtent;

    const DestinationFormat format = destinationFormat(request.destinationFormat, depth);
    if (format == DestinationFormat::unsupported)
        return request.destinationFormat == 23u && !depth ? ResolveStatus::depthFormat
                                                          : ResolveStatus::format;

    // Round and clip in surface pixels before the copy. The expanded fringe can
    // extend into allocation padding, so it is clipped to the destination's
    // remaining extent as well as the surface's.
    const std::array<int32_t, 4> expanded = expandedResolveRectangle(request.rectangle);
    std::array<uint32_t, 4> copied{uint32_t(expanded[0]), uint32_t(expanded[1]),
                                   uint32_t(expanded[2]), uint32_t(expanded[3])};
    const uint64_t reachX = uint64_t(request.rectangle[0]) + request.destinationWidth - request.offset[0];
    const uint64_t reachY = uint64_t(request.rectangle[1]) + request.destinationHeight - request.offset[1];
    copied[2] = uint32_t((std::min)({uint64_t(copied[2]), uint64_t(source.width), reachX}));
    copied[3] = uint32_t((std::min)({uint64_t(copied[3]), uint64_t(source.height), reachY}));
    if (copied[2] <= copied[0] || copied[3] <= copied[1]) return ResolveStatus::emptyRectangle;

    plan.growSource = growSource;
    plan.source = source;
    plan.copied = copied;
    plan.offset = request.offset;
    plan.format = format;
    plan.depth = depth;
    plan.face = request.face;
    return ResolveStatus::ok;
}

inline ClearRegion planClear(const ClearRequest& request) noexcept {
    ClearRegion region;
    const int32_t width = int32_t(request.surface.width);
    const int32_t height = int32_t(request.surface.height);
    if (width <= 0 || height <= 0) return region;
    std::array<int32_t, 4> rect{0, 0, width, height};
    if (request.rectangle) {
        const auto& requested = *request.rectangle;
        // The guest can name a region that runs past the surface; the original
        // clips it and leaves the rest of the target alone.
        rect = {(std::max)(int32_t(0), requested[0]), (std::max)(int32_t(0), requested[1]),
                (std::min)(width, requested[2]), (std::min)(height, requested[3])};
    }
    if (rect[2] <= rect[0] || rect[3] <= rect[1]) return region;
    region.valid = true;
    region.rect = rect;
    region.full = rect[0] == 0 && rect[1] == 0 && rect[2] == width && rect[3] == height;
    return region;
}

inline bool presentable(Extent resolved, Extent display) noexcept {
    return resolved.width && resolved.height && display.width && display.height;
}

} // namespace DarkRecomp::Ps5::WorldSurfacePolicy
