#pragma once
// The decisions one world draw makes about pipeline state, without a GPU.
//
// WorldRendererD3D11::draw turns a WorldDraw into D3D11 objects: a depth/stencil
// state, a blend state, a rasterizer state, a scissor rectangle and a viewport,
// all read out of the engine's captured attributes. The console has RADV, so the
// objects differ, but every number in them comes from the same bytes and must
// mean the same thing. Those readings live here: free of Vulkan and of D3D11, so
// tests/world_draw_policy_tests.cpp can check them against the Windows renderer's
// behaviour, and renderer/ps5/world_draw.cpp only builds Vulkan state from the
// answers.
//
// Two console conventions are recorded here rather than in the caller:
//
//   * clip space. The console draws HLSL through glslang, which leaves NDC as
//     the original wrote it: +y up. Vulkan's framebuffer has +y down, so the
//     viewport is flipped (positive NDC y at the top of the rectangle). Both
//     APIs then judge winding in the same framebuffer coordinates, so the
//     original's front-face convention is retained.
//   * the scissor. D3D11 selects it per draw with 0x20 and otherwise leaves the
//     rectangle unread; Vulkan always clips, so a draw without the bit covers
//     the whole render area.
//
// The enum values are Vulkan's, so the renderer can cast them; the policy header
// stays independent of vulkan.h by naming them itself.
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <limits>

namespace DarkRecomp::Ps5::WorldDrawPolicy {

// The engine's captured attributes, as WorldRendererD3D11::draw reads them.
// `attributes` is the draw's 160-byte completed state block.
enum : uint32_t {
    kDepthTest = 0x2u,
    kDepthWrite = 0x4u,
    kBlend = 0x8u,
    kScissor = 0x20u,
    kStencil = 0x4000u,
    kCull = 0x800u,
    kCounterClockwiseFront = 0x1000u,
    kDepthBias = 0x40000u,
    kWriteColor = 0x100000u,
    kWriteAlpha = 0x1000000u,
};

constexpr uint32_t word(const uint8_t* bytes) noexcept {
    return uint32_t(bytes[0]) << 24 | uint32_t(bytes[1]) << 16 | uint32_t(bytes[2]) << 8 | bytes[3];
}

// VkCompareOp, spelled out so this header needs no Vulkan.
enum class CompareOp : uint32_t {
    never = 0, less = 1, equal = 2, lessOrEqual = 3,
    greater = 4, notEqual = 5, greaterOrEqual = 6, always = 7,
};

// VkBlendFactor.
enum class BlendFactor : uint32_t {
    zero = 0, one = 1, sourceColor = 2, oneMinusSourceColor = 3,
    destinationColor = 4, oneMinusDestinationColor = 5, sourceAlpha = 6,
    oneMinusSourceAlpha = 7, destinationAlpha = 8, oneMinusDestinationAlpha = 9,
    sourceAlphaSaturate = 10,
};

// VkStencilOp.
enum class StencilOp : uint32_t {
    keep = 0, zero = 1, replace = 2, incrementAndClamp = 3,
    decrementAndClamp = 4, invert = 5, incrementAndWrap = 6, decrementAndWrap = 7,
};

struct StencilFace {
    CompareOp compare = CompareOp::never;
    StencilOp fail = StencilOp::keep;
    StencilOp pass = StencilOp::keep;
    StencilOp depthFail = StencilOp::keep;
    bool operator==(const StencilFace&) const = default;
};

// Original 82066B20 reverses comparison ordering for the engine's reversed Z.
// The table is the Windows renderer's (world_render_state.h), converted from
// D3D11_COMPARISON_* to VkCompareOp: D3D11 numbers comparisons from 1 and Vulkan
// from 0, so every entry is the Windows value minus one.
constexpr CompareOp depthComparison(unsigned code) noexcept {
    constexpr CompareOp table[] = {
        CompareOp::never,        // 0: never
        CompareOp::never,        // 1: never (the original's reserved entry)
        CompareOp::greater,      // 2: greater
        CompareOp::equal,        // 3: equal
        CompareOp::greaterOrEqual, // 4: greater equal
        CompareOp::less,         // 5: less
        CompareOp::notEqual,     // 6: not equal
        CompareOp::lessOrEqual,  // 7: less equal
        CompareOp::always,       // 8: always
    };
    return table[code <= 8u ? code : 0u];
}

// 82247FE8 keeps table 82066AFC for the stencil comparisons; unlike depth they
// keep their ordinary order, and the guest's 1..8 is D3D11's numbering.
constexpr CompareOp stencilComparison(unsigned code) noexcept {
    return code >= 1u && code <= 8u ? static_cast<CompareOp>(code - 1u) : CompareOp::never;
}

// The same three bytes the rasterizer state reads: comparison in the high nibble
// of byte0, fail op in its low nibble, pass op in the low nibble of byte1 and
// depth-fail in its high nibble. D3D11's stencil operations also number from 1,
// with the same order, so the console's value is the guest's minus one.
constexpr StencilFace stencilFace(const uint8_t* bytes) noexcept {
    StencilFace face;
    face.compare = stencilComparison(bytes[0] >> 4);
    face.fail = static_cast<StencilOp>((bytes[0] & 7u));
    face.pass = static_cast<StencilOp>((bytes[1] & 7u));
    face.depthFail = static_cast<StencilOp>(((bytes[1] >> 4) & 7u));
    return face;
}

// The original's blend-factor table (world_renderer.cpp's factors[]), in the
// same index order, as VkBlendFactor. An index outside it is a reject (11).
constexpr unsigned kBlendFactorCount = 12;

constexpr BlendFactor blendFactor(unsigned index) noexcept {
    constexpr BlendFactor table[kBlendFactorCount] = {
        BlendFactor::zero,               // 0: D3D11_BLEND_ZERO
        BlendFactor::zero,               // 1: D3D11_BLEND_ZERO
        BlendFactor::one,                // 2: D3D11_BLEND_ONE
        BlendFactor::sourceColor,        // 3: D3D11_BLEND_SRC_COLOR
        BlendFactor::oneMinusSourceColor, // 4: D3D11_BLEND_INV_SRC_COLOR
        BlendFactor::sourceAlpha,        // 5: D3D11_BLEND_SRC_ALPHA
        BlendFactor::oneMinusSourceAlpha, // 6: D3D11_BLEND_INV_SRC_ALPHA
        BlendFactor::destinationAlpha,   // 7: D3D11_BLEND_DEST_ALPHA
        BlendFactor::oneMinusDestinationAlpha, // 8: D3D11_BLEND_INV_DEST_ALPHA
        BlendFactor::destinationColor,   // 9: D3D11_BLEND_DEST_COLOR
        BlendFactor::oneMinusDestinationColor, // 10: D3D11_BLEND_INV_DEST_COLOR
        BlendFactor::sourceAlphaSaturate, // 11: D3D11_BLEND_SRC_ALPHA_SAT
    };
    return table[index < kBlendFactorCount ? index : 0u];
}

// The alpha channel uses the colour factor's alpha counterpart, as the Windows
// renderer translates it: SRC_COLOR becomes SRC_ALPHA, DEST_COLOR DEST_ALPHA.
constexpr BlendFactor alphaBlendFactor(BlendFactor color) noexcept {
    switch (color) {
        case BlendFactor::sourceColor: return BlendFactor::sourceAlpha;
        case BlendFactor::oneMinusSourceColor: return BlendFactor::oneMinusSourceAlpha;
        case BlendFactor::destinationColor: return BlendFactor::destinationAlpha;
        case BlendFactor::oneMinusDestinationColor: return BlendFactor::oneMinusDestinationAlpha;
        default: return color;
    }
}

// 0x100000 writes RGB, 0x1000000 alpha; VkColorComponentFlags numbers R=1, G=2,
// B=4, A=8, so the RGB bits are the same three.
constexpr uint32_t colorWriteMask(uint32_t flags) noexcept {
    return ((flags & kWriteColor) ? 7u : 0u) | ((flags & kWriteAlpha) ? 8u : 0u);
}

constexpr bool depthTestEnabled(uint32_t flags) noexcept { return (flags & kDepthTest) != 0; }
constexpr bool depthWriteEnabled(uint32_t flags) noexcept { return (flags & kDepthWrite) != 0; }
constexpr bool stencilEnabled(uint32_t flags) noexcept { return (flags & kStencil) != 0; }
constexpr bool blendEnabled(uint32_t flags) noexcept { return (flags & kBlend) != 0; }
constexpr bool cullEnabled(uint32_t flags) noexcept { return (flags & kCull) != 0; }

// The original selects ALWAYS while testing is off and keeps writes independent;
// Vulkan has the same split, so a disabled test still writes when the guest asks.
constexpr CompareOp depthCompareFor(uint32_t flags, uint8_t function) noexcept {
    return depthTestEnabled(flags) ? depthComparison(function) : CompareOp::always;
}

// D3D11 raster.FrontCounterClockwise = !(flags&0x800) || (flags&0x1000), and the
// console uses the same value. Vulkan judges winding in framebuffer space, y
// down; the flipped viewport puts every vertex where D3D11's y-down screen has
// it, so a triangle that is clockwise for D3D11 is clockwise for Vulkan. An
// inverted enumeration culled the visible faces on the console (2026-10-25):
// walls showed their far sides and bodies lost necks and arms, and the picture
// came right with culling switched off.
constexpr bool frontFaceCounterClockwise(uint32_t flags) noexcept {
    return !cullEnabled(flags) || (flags & kCounterClockwiseFront) != 0;
}

// Original 822487D0 negates the slope and multiplies units by the fixed
// 2^-19 value 8209F114 initialises; 82248680's factor 16 is cancelled by Xenos's
// 1/16-pixel slope unit. D24 needs an integer count of representable depth steps
// (the console uses depth_bias_control's FORCE_UNORM representation on D32),
// rounded away from zero. The Windows renderer clamps into int's range; the
// console converts a float to int32 the same way, which also saturates there.
constexpr bool depthBias(uint32_t flags, float slope, float units, int32_t& constant,
                         float& slopeScaled) noexcept {
    constant = 0;
    slopeScaled = 0.0f;
    if (!(flags & kDepthBias)) return true;
    const float offset = units * -0x1p-19f;
    if (!std::isfinite(offset) || !std::isfinite(slope)) return false;
    const double steps = std::copysign(std::ceil(std::abs(double(offset)) * 16777215.0), offset);
    constant = int32_t(std::clamp(steps, double((std::numeric_limits<int32_t>::min)()),
                                  double((std::numeric_limits<int32_t>::max)())));
    slopeScaled = -slope;
    return true;
}

// The engine draws a depth-only pass and then lights each surface with depth
// compare EQUAL (XRShader_FP20_NDSP and its kin), relying on both passes
// producing the same stored depth. They come from different vertex programs, so
// the values differ in the last bits of the float; D3D11's D24 buffer rounds
// that away, the console's D32 float buffer (RADV has no D24) does not, and the
// lights then touch nothing. An EQUAL test therefore becomes "no farther than
// the stored surface, within a few steps": the ordering comparison the frame's
// depth writes use, a small bias toward the camera, and no depth write (writing
// an equal value changed nothing in the original).
// Two fixed D24 steps cover conversion/interpolation rounding. In the default
// D32 representation even thousands of bias units can be less than one D24
// step at distance, which rejected the tunnel's lighting. The GPU call must
// use FORCE_UNORM; increasing float-ULP counts cannot reproduce this rule.
inline constexpr int32_t kEqualDepthTolerance = 2;

struct EqualDepth {
    CompareOp compare;
    int32_t bias;
};

// nearerIsGreater: whether the frame's depth writes keep the greater value
// (reversed depth, as The Darkness uses with GREATER_OR_EQUAL).
constexpr EqualDepth tolerantEqual(bool nearerIsGreater) noexcept {
    return nearerIsGreater ? EqualDepth{CompareOp::greaterOrEqual, kEqualDepthTolerance}
                           : EqualDepth{CompareOp::lessOrEqual, -kEqualDepthTolerance};
}

// Which way the frame's depth runs, read from a depth-writing ordered test.
// Equal, always and never say nothing and keep the previous answer.
constexpr bool nearerIsGreaterFor(CompareOp compare, bool previous) noexcept {
    switch (compare) {
        case CompareOp::greater:
        case CompareOp::greaterOrEqual: return true;
        case CompareOp::less:
        case CompareOp::lessOrEqual: return false;
        default: return previous;
    }
}

// The viewport the original sets is (x, y, w, h) in surface pixels. The console
// keeps x and w and flips y, so NDC +1 lands on the rectangle's top edge. A draw
// with no area is refused by the caller, not clipped here.
struct ViewportRect {
    float x = 0.0f, y = 0.0f, width = 0.0f, height = 0.0f;
    bool valid = false;
};

inline ViewportRect viewport(const std::array<uint32_t, 4>& guest) noexcept {
    ViewportRect result;
    if (!guest[2] || !guest[3]) return result;
    result.x = float(guest[0]);
    result.y = float(guest[1]) + float(guest[3]);
    result.width = float(guest[2]);
    result.height = -float(guest[3]);
    result.valid = true;
    return result;
}

// The scissor's two corners are captured as packed x/y words.
struct ScissorRect {
    int32_t x = 0, y = 0, width = 0, height = 0;
    bool operator==(const ScissorRect&) const = default;
};

inline ScissorRect scissor(uint32_t leftTop, uint32_t rightBottom, uint32_t targetWidth,
                           uint32_t targetHeight) noexcept {
    // left, top, right, bottom as the original packs them: x low, y high.
    const int32_t left = int32_t(leftTop & 0xFFFFu), top = int32_t(leftTop >> 16);
    const int32_t right = int32_t(rightBottom & 0xFFFFu), bottom = int32_t(rightBottom >> 16);
    ScissorRect rect;
    rect.x = (std::max)(left, 0);
    rect.y = (std::max)(top, 0);
    const int64_t clampedRight = (std::min)(int64_t(right), int64_t(targetWidth));
    const int64_t clampedBottom = (std::min)(int64_t(bottom), int64_t(targetHeight));
    rect.width = int32_t((std::max)(int64_t(0), clampedRight - rect.x));
    rect.height = int32_t((std::max)(int64_t(0), clampedBottom - rect.y));
    return rect;
}

// A draw without the scissor bit covers the whole render area, which is what
// D3D11 does when ScissorEnable is off.
inline ScissorRect fullScissor(uint32_t targetWidth, uint32_t targetHeight) noexcept {
    return {0, 0, int32_t(targetWidth), int32_t(targetHeight)};
}

// The engine's sampler addressing, in Xenos order: 0 wrap, 1 mirror, 2 clamp,
// 3 mirror-once, 4 border. Vulkan has no mirror-once, so it degrades to clamp,
// which differs only for coordinates outside [-1, 1].
enum class AddressMode : uint32_t {
    repeat = 0, mirroredRepeat = 1, clampToEdge = 2, clampToBorder = 3,
};

constexpr AddressMode addressMode(uint8_t guest) noexcept {
    switch (guest) {
        case 0: return AddressMode::repeat;
        case 1: return AddressMode::mirroredRepeat;
        case 4: return AddressMode::clampToBorder;
        default: return AddressMode::clampToEdge; // clamp and mirror-once
    }
}

// The original's sampler filter bits select each axis independently; a complete
// linear filter is the common case and the only one the engine states need.
struct Filter {
    bool minLinear = false, magLinear = false, mipLinear = false;
    bool operator==(const Filter&) const = default;
};

// The engine's sampler state (world_mesh.h's WorldSampler), mirrored so this
// header needs no engine include.
struct SamplerState {
    bool valid = false, minLinear = false, magLinear = false, mipLinear = false;
    std::array<uint8_t, 3> address{};
    uint8_t anisotropy = 1;
    uint8_t minLevel = 0, maxLevel = 0;
    float bias = 0.0f;
};

constexpr Filter filter(const SamplerState& state) noexcept {
    return {state.minLinear, state.magLinear, state.mipLinear};
}

// A sampler's mip range is the guest's resident levels; the console uploads the
// levels a ColorImage owns, so an upper bound past the chain is clamped by the
// image's own level count (Vulkan ignores an out-of-range maxLod).
constexpr float minLod(const SamplerState& state) noexcept { return float(state.minLevel); }
constexpr float maxLod(const SamplerState& state) noexcept { return float(state.maxLevel); }
constexpr float lodBias(const SamplerState& state) noexcept { return state.bias; }
constexpr uint32_t anisotropy(const SamplerState& state) noexcept {
    return state.anisotropy ? state.anisotropy : 1u;
}

// The engine's fetch scale: a surface's exponent makes its half-float contents
// readable as the format the guest asked for, and texture fetches apply it.
inline float fetchScale(int exponent) noexcept {
    // Integral powers in the normal range have exact IEEE-754 encodings.
    if(exponent>=-126 && exponent<=127)
        return std::bit_cast<float>(uint32_t(exponent+127)<<23);
    return std::exp2(float(exponent));
}

// Reject reasons, numbered like WorldRendererD3D11::draw's steps so a console
// log line means the same thing as a Windows one.
enum class RejectReason : uint32_t {
    accepted = 0,
    fragment = 1,      // no fragment program for the name and flags
    resolved = 2,      // a resolved texture of another size or shape
    texture = 3,       // a sampled texture with no pixels
    target = 4,        // no colour or depth surface where the draw needs one
    indices = 5,       // an index range outside its stream
    attributes = 6,    // a vertex stream the program needs is absent
    coordinates = 7,   // a texture stage naming a coordinate bank past the end
    decode = 8,        // the vertex stream did not decode
    indexOverflow = 9, // an index past the decoded vertex count
    constants = 10,    // the vertex constants could not be prepared
    state = 11,        // a comparison, blend factor or bias the GPU refuses
    budget = 12,       // this frame's upload budget is spent; the draw returns
};

constexpr const char* rejectName(RejectReason reason) noexcept {
    switch (reason) {
        case RejectReason::accepted: return "ok";
        case RejectReason::fragment: return "fragment";
        case RejectReason::resolved: return "resolved";
        case RejectReason::texture: return "texture";
        case RejectReason::target: return "target";
        case RejectReason::indices: return "indices";
        case RejectReason::attributes: return "attributes";
        case RejectReason::coordinates: return "coordinates";
        case RejectReason::decode: return "decode";
        case RejectReason::indexOverflow: return "index-overflow";
        case RejectReason::constants: return "constants";
        case RejectReason::state: return "state";
        case RejectReason::budget: return "budget";
    }
    return "unknown";
}

} // namespace DarkRecomp::Ps5::WorldDrawPolicy
