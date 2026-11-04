// The console's world draw state, checked without a GPU.
//
// renderer/d3d11/world_renderer.cpp turns a WorldDraw's captured attributes into
// D3D11 states. renderer/ps5/world_draw.cpp builds Vulkan state from the same
// bytes through world_draw_policy.h. This test checks that the readings are the
// Windows renderer's: the reversed-Z depth table, the stencil faces, the blend
// factor table and its alpha counterpart, the write mask, the flipped console
// viewport, the clamped scissor and the depth-bias arithmetic (the same numbers
// tests/world_shadow_bias_tests.h exercises on the Windows state).
#include "world_draw_policy.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <stdexcept>

namespace {

using namespace DarkRecomp::Ps5::WorldDrawPolicy;

void require(bool ok, const char* why) {
    if (!ok) throw std::runtime_error(why);
}

void testDepth() {
    using enum CompareOp;
    const CompareOp table[] = {never, never, greater, equal, greaterOrEqual, less,
                               notEqual, lessOrEqual, always};
    for (unsigned code = 0; code <= 8; ++code)
        require(depthComparison(code) == table[code], "a depth comparison differs from the engine's");
    require(depthComparison(9) == never, "an unknown depth comparison was accepted");

    // 82248680 keeps writes independent of testing and selects ALWAYS when the
    // test is off; the console must not fold the two bits together.
    require(depthCompareFor(0, 0) == always, "a draw with no depth test compared");
    require(depthCompareFor(kDepthTest, 5) == less, "a depth-tested draw did not use the table");
    require(depthCompareFor(kDepthWrite, 3) == always, "a write-only draw compared");
    require(depthTestEnabled(kDepthWrite) == false, "depth testing was read from the write bit");
    require(depthWriteEnabled(kDepthWrite), "the depth write bit was dropped");

    // The lighting passes' EQUAL test tolerates the last bits a D32 float buffer
    // keeps and D24 rounded away, toward the camera only.
    const EqualDepth reversed = tolerantEqual(true);
    require(reversed.compare == greaterOrEqual && reversed.bias > 0,
            "a reversed-depth EQUAL did not become a nearer-or-equal test biased toward the camera");
    const EqualDepth forward = tolerantEqual(false);
    require(forward.compare == lessOrEqual && forward.bias < 0,
            "a forward-depth EQUAL did not become a nearer-or-equal test biased toward the camera");
    require(nearerIsGreaterFor(greaterOrEqual, false) && !nearerIsGreaterFor(less, true),
            "the frame's depth direction was misread from an ordered test");
    require(nearerIsGreaterFor(equal, true) && !nearerIsGreaterFor(always, false),
            "an unordered test changed the frame's depth direction");
}

void testStencil() {
    using enum CompareOp;
    using enum StencilOp;
    const uint8_t bytes[4] = {0x25, 0x31, 0, 0};
    const StencilFace face = stencilFace(bytes);
    require(face.compare == less, "the stencil comparison was misread");
    require(face.fail == invert, "the stencil fail operation was misread");
    require(face.pass == zero, "the stencil pass operation was misread");
    require(face.depthFail == incrementAndClamp, "the stencil depth-fail operation was misread");
    require(stencilFace(bytes + 2).compare == never, "an empty stencil face compared");
    require(stencilComparison(0) == never && stencilComparison(9) == never,
            "an out-of-table stencil comparison was accepted");
}

void testBlend() {
    using enum BlendFactor;
    const BlendFactor table[] = {zero, zero, one, sourceColor, oneMinusSourceColor, sourceAlpha,
                                 oneMinusSourceAlpha, destinationAlpha, oneMinusDestinationAlpha,
                                 destinationColor, oneMinusDestinationColor, sourceAlphaSaturate};
    for (unsigned index = 0; index < kBlendFactorCount; ++index)
        require(blendFactor(index) == table[index], "a blend factor differs from the engine's");
    // The Windows renderer derives the alpha factors from the colour ones.
    require(alphaBlendFactor(sourceColor) == sourceAlpha, "SRC_COLOR did not become SRC_ALPHA");
    require(alphaBlendFactor(oneMinusSourceColor) == oneMinusSourceAlpha,
            "INV_SRC_COLOR did not become INV_SRC_ALPHA");
    require(alphaBlendFactor(destinationColor) == destinationAlpha,
            "DEST_COLOR did not become DEST_ALPHA");
    require(alphaBlendFactor(oneMinusDestinationColor) == oneMinusDestinationAlpha,
            "INV_DEST_COLOR did not become INV_DEST_ALPHA");
    require(alphaBlendFactor(one) == one && alphaBlendFactor(sourceAlphaSaturate) == sourceAlphaSaturate,
            "a factor that needs no alpha form was changed");

    require(colorWriteMask(kWriteColor) == 7, "the RGB write mask was wrong");
    require(colorWriteMask(kWriteAlpha) == 8, "the alpha write mask was wrong");
    require(colorWriteMask(kWriteColor | kWriteAlpha) == 15, "both write masks were not combined");
    require(colorWriteMask(0) == 0, "a draw with no write mask wrote colour");
}

void testRaster() {
    using enum AddressMode;
    // The console's winding is D3D11's: the flipped viewport keeps D3D11's
    // y-down screen orientation, where Vulkan judges front faces.
    require(!cullEnabled(0) && frontFaceCounterClockwise(0),
            "an unculled draw did not keep D3D11's counter-clockwise default");
    require(cullEnabled(kCull) && !frontFaceCounterClockwise(kCull),
            "a clockwise-front draw lost the D3D11 winding");
    require(cullEnabled(kCull | kCounterClockwiseFront) && frontFaceCounterClockwise(kCull | kCounterClockwiseFront),
            "a counter-clockwise-front draw lost the D3D11 winding");

    // The original viewport is (x, y, w, h); the console flips y so NDC +1 is the
    // rectangle's top edge.
    const ViewportRect view = viewport({100, 200, 640, 480});
    require(view.valid, "a valid viewport was refused");
    require(view.x == 100.0f && view.y == 680.0f && view.width == 640.0f && view.height == -480.0f,
            "the console viewport did not flip y");
    require(!viewport({0, 0, 0, 480}).valid, "an empty viewport was accepted");

    // left, top, right, bottom as the original packs them: x low, y high.
    const ScissorRect rect = scissor((10u << 16) | 20u, (490u << 16) | 660u, 1280, 720);
    require(rect.x == 20 && rect.y == 10 && rect.width == 640 && rect.height == 480,
            "a scissor rectangle was misread");
    const ScissorRect clamped = scissor((0u << 16) | 0u, (5000u << 16) | 5000u, 1280, 720);
    require(clamped.width == 1280 && clamped.height == 720, "a scissor was not clamped to its target");
    const ScissorRect empty = scissor((0u << 16) | 1300u, (10u << 16) | 1400u, 1280, 720);
    require(empty.width == 0, "a scissor outside the target kept a width");
    const ScissorRect full = fullScissor(1280, 720);
    require(full.x == 0 && full.y == 0 && full.width == 1280 && full.height == 720,
            "a draw with no scissor bit did not cover its target");
}

void testBias() {
    int32_t constant = 0;
    float slope = 0.0f;
    require(depthBias(0, 0.0f, 1.0f, constant, slope) && constant == 0 && slope == 0.0f,
            "a draw without the bias bit was biased");

    // units * -2^-19 * 16777215 rounded away from zero: one unit is 32 steps.
    require(depthBias(kDepthBias, -1.0f, 1.0f, constant, slope), "a finite bias was refused");
    require(constant == -32, "the depth bias constant was not the original's");
    require(slope == 1.0f, "the slope was not negated");
    require(depthBias(kDepthBias, 0.0f, -1.0f, constant, slope) && constant == 32,
            "a negative unit was not biased the other way");
    require(depthBias(kDepthBias, 0.0f, 1.5f, constant, slope) && constant == -48,
            "a fractional unit was not rounded away from zero");
    require(!depthBias(kDepthBias, 0.0f, std::nanf(""), constant, slope),
            "a non-finite bias was accepted");
    require(!depthBias(kDepthBias, std::nanf(""), 1.0f, constant, slope),
            "a non-finite slope was accepted");

    // Reversed-Z tunnel surfaces are close to zero. A float-ULP bias there
    // cannot bridge the D24 rounding that the lighting pass relies on.
    constexpr float d24Step = 0x1p-24f;
    const float stored = 0x1p-12f;
    const float incoming = stored - d24Step;
    const float floatStep = std::nextafter(stored, 1.0f) - stored;
    require(incoming + 64 * floatStep < stored,
            "the regression case no longer distinguishes float and D24 bias");
    const float tolerance = float(tolerantEqual(true).bias) * d24Step;
    require(incoming + tolerance >= stored, "D24-rounded lighting was rejected");
    require(stored - 8 * d24Step + tolerance < stored,
            "an occluded surface passed the bounded lighting tolerance");
    require(-tolerantEqual(false).bias == tolerantEqual(true).bias,
            "forward and reversed depth tolerances differ");
}

void testSampling() {
    using enum AddressMode;
    require(addressMode(0) == repeat, "wrap addressing was misread");
    require(addressMode(1) == mirroredRepeat, "mirror addressing was misread");
    require(addressMode(2) == clampToEdge, "clamp addressing was misread");
    require(addressMode(3) == clampToEdge, "mirror-once did not degrade to clamp");
    require(addressMode(4) == clampToBorder, "border addressing was misread");
    require(addressMode(9) == clampToEdge, "an unknown addressing mode was accepted");

    SamplerState state;
    state.minLinear = state.magLinear = true;
    state.address = {0, 1, 4};
    state.anisotropy = 8;
    state.minLevel = 2;
    state.maxLevel = 7;
    state.bias = -0.5f;
    const Filter samplerFilter = filter(state);
    require(samplerFilter.minLinear && samplerFilter.magLinear && !samplerFilter.mipLinear,
            "a sampler filter was misread");
    require(minLod(state) == 2.0f && maxLod(state) == 7.0f, "the sampler LOD range was misread");
    require(anisotropy(state) == 8, "the sampler anisotropy was misread");
    require(anisotropy(SamplerState{}) == 1, "an absent anisotropy did not become one");
    require(lodBias(state) == -0.5f, "the sampler bias was misread");
    require(fetchScale(0) == 1.0f && fetchScale(2) == 4.0f && fetchScale(-1) == 0.5f,
            "the fetch exponent did not become a scale");
    for(int exponent=-150;exponent<=128;++exponent) {
        const float expected=std::exp2(float(exponent));
        require(std::bit_cast<uint32_t>(fetchScale(exponent))==std::bit_cast<uint32_t>(expected),
                "the fetch scale changed its exact bits at a normal, subnormal or overflow boundary");
    }
    for(const int exponent:{(std::numeric_limits<int>::min)(),(std::numeric_limits<int>::max)()})
        require(std::bit_cast<uint32_t>(fetchScale(exponent))==
                    std::bit_cast<uint32_t>(std::exp2(float(exponent))),
                "an extreme fetch exponent changed its fallback");
}

void testReasons() {
    // The numbering is WorldRendererD3D11::draw's, so a console line means the
    // same thing as a Windows one.
    require(unsigned(RejectReason::accepted) == 0, "accepted is not zero");
    for (unsigned reason = 0; reason <= unsigned(RejectReason::budget); ++reason)
        require(rejectName(RejectReason(reason)) != nullptr && rejectName(RejectReason(reason))[0],
                "a reject reason has no name");
    require(unsigned(RejectReason::texture) == 3 && unsigned(RejectReason::target) == 4 &&
                unsigned(RejectReason::state) == 11 && unsigned(RejectReason::budget) == 12,
            "the reject numbering left the Windows renderer's");
}

} // namespace

int main() {
    try {
        testDepth();
        testStencil();
        testBlend();
        testRaster();
        testBias();
        testSampling();
        testReasons();
        std::printf("WorldDrawPolicy: depth, stencil, blend, raster, bias and sampling rules match "
                    "the Windows renderer's.\n");
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAILED: %s\n", error.what());
        return 1;
    }
    return 0;
}
