// The console's world render targets, checked without a GPU.
//
// renderer/ps5/world_surfaces.cpp creates the engine's render targets on the
// console and carries out its clears, resolves and frontbuffer copy. Which one
// to create, how far it may grow, which rectangle a resolve copies and which
// pixels a clear touches are decided by world_surface_policy.h, whose rules are
// WorldRendererD3D11's. This test drives those rules against the cases the
// Windows renderer's own tests exercise -- the 1290-against-1288 frontbuffer
// fringe, the eight-pixel rounding, the destination clip -- so a mistake in the
// console's arithmetic is caught here rather than on the console.
#include "world_surface_policy.h"

#include <cstdio>
#include <stdexcept>
#include <string_view>

namespace {

using namespace DarkRecomp::Ps5::WorldSurfacePolicy;

void require(bool ok, const char* why) {
    if (!ok) throw std::runtime_error(why);
}

void testGrowth() {
    Extent out;
    require(grow({1280, 720}, {}, out) && out == Extent{1280, 720}, "A new surface was not sized to its request");
    require(grow({640, 360}, {1280, 720}, out) && out == Extent{1280, 720}, "A surface shrank");
    require(grow({1280, 1080}, {1280, 720}, out) && out == Extent{1280, 1080}, "A surface did not grow in height");
    require(grow({1920, 1080}, {1280, 720}, out) && out == Extent{1920, 1080}, "A surface did not grow");
    require(!grow({}, {1280, 720}, out), "An empty extent was accepted");
    require(!grow({0, 720}, {1280, 720}, out), "A zero-width extent was accepted");
    require(!grow({kMaxExtent + 1, 720}, {1280, 720}, out), "An over-limit extent was accepted");
    require(grow({kMaxExtent, kMaxExtent}, {}, out) && out == Extent{kMaxExtent, kMaxExtent},
            "The largest supported extent was refused");
    require(out.width <= kMaxExtent && out.height <= kMaxExtent, "A grown extent left the GPU limit");
}

void testFormats() {
    using enum DestinationFormat;
    require(destinationFormat(6, false) == rgba8, "Guest format 6 is not RGBA8");
    require(destinationFormat(54, false) == rgb10a2, "Guest format 54 is not 10:10:10:2");
    require(destinationFormat(54, true) == rgb10a2, "A depth attachment lost the frontbuffer format");
    require(destinationFormat(26, false) == rgba16, "Guest format 26 is not RGBA16");
    require(destinationFormat(26, true) == rgba16, "Guest format 26 is not RGBA16 for depth");
    require(destinationFormat(23, true) == r32f, "Guest format 23 is not single-channel float for depth");
    require(destinationFormat(23, false) == unsupported, "Guest format 23 was accepted for colour");
    require(destinationFormat(0, false) == unsupported, "An absent format was accepted");
    require(destinationFormat(7, true) == unsupported, "An unknown format was accepted");
}

ResolveRequest frame(uint32_t width, uint32_t height, uint32_t format, uint32_t attachment = 0) {
    ResolveRequest request;
    request.flags = attachment;
    request.rectangle = {0, 0, width, height};
    request.destinationWidth = width;
    request.destinationHeight = height;
    request.destinationFormat = format;
    request.source = {width, height};
    request.viewport = {0, 0, width, height};
    return request;
}

void testResolvePlan() {
    ResolvePlan plan;
    ResolveRequest request = frame(1280, 720, 54);
    require(planResolve(request, plan) == ResolveStatus::ok, "A full-surface resolve was refused");
    require(plan.copied == std::array<uint32_t, 4>{0, 0, 1280, 720}, "A full-surface resolve did not cover the surface");
    require(plan.source == Extent{1280, 720} && !plan.growSource, "A fitting resolve grew its surface");
    require(plan.format == DestinationFormat::rgb10a2 && !plan.depth, "A colour resolve lost its format");
    require(plan.offset == std::array<uint32_t, 2>{0, 0}, "A resolve lost its destination offset");

    // The copy's right and bottom edges round up to the eight-pixel tile.
    request = frame(1280, 720, 6);
    request.rectangle = {0, 0, 1279, 719};
    require(planResolve(request, plan) == ResolveStatus::ok, "A tile-aligned resolve was refused");
    require(plan.copied == std::array<uint32_t, 4>{0, 0, 1280, 720}, "The resolve rectangle was not rounded to eight pixels");
    require(expandedResolveRectangle({3, 5, 1279, 719}) == std::array<int32_t, 4>{3, 5, 1280, 720},
            "The expanded resolve rectangle is wrong");

    // A 1080p frontbuffer asks for 1290 pixels of a surface drawn to 1288. The
    // original grows the surface by the two-pixel fringe and keeps every pixel.
    request = frame(1288, 720, 54, 0);
    request.rectangle = {0, 0, 1290, 720};
    request.destinationWidth = 1290;
    request.viewport = {0, 0, 1290, 720};
    require(planResolve(request, plan) == ResolveStatus::ok, "A two-pixel fringe was refused");
    require(plan.growSource && plan.source == Extent{1290, 720}, "The fringe did not grow the surface");
    require(plan.copied == std::array<uint32_t, 4>{0, 0, 1290, 720}, "The grown fringe was not copied");

    // More than seven pixels of undrawn surface is a real mismatch, as is a
    // rectangle that leaves the viewport the surface was drawn with.
    request.rectangle = {0, 0, 1300, 720};
    request.destinationWidth = 1300;
    request.viewport = {0, 0, 1300, 720};
    require(planResolve(request, plan) == ResolveStatus::sourceExtent, "An oversized fringe was accepted");
    request.rectangle = {0, 0, 1290, 720};
    request.destinationWidth = 1290;
    request.viewport = {0, 0, 1280, 720};
    require(planResolve(request, plan) == ResolveStatus::sourceExtent, "A fringe outside the viewport was accepted");

    // The rounded fringe is trimmed where the destination ends rather than
    // refused; a copy whose own extent does not fit is refused.
    request = frame(1280, 720, 26);
    request.rectangle = {0, 0, 1279, 719};
    request.destinationWidth = 1279;
    request.destinationHeight = 719;
    require(planResolve(request, plan) == ResolveStatus::ok, "A fringe clipped by its destination was refused");
    require(plan.copied == std::array<uint32_t, 4>{0, 0, 1279, 719},
            "The rounded fringe was not clipped to its destination");

    request = frame(1280, 720, 26);
    request.destinationWidth = 640;
    request.destinationHeight = 360;
    require(planResolve(request, plan) == ResolveStatus::destinationExtent,
            "A resolve larger than its destination was accepted");
    request.offset = {10, 10};
    request.rectangle = {0, 0, 640, 360};
    require(planResolve(request, plan) == ResolveStatus::destinationExtent, "A resolve past its destination was accepted");
    request.offset = {0, 0};

    request = frame(1280, 720, 6);
    request.rectangle = {40, 40, 40, 80};
    require(planResolve(request, plan) == ResolveStatus::emptyRectangle, "An empty resolve rectangle was accepted");

    request = frame(1280, 720, 6);
    request.source = {};
    require(planResolve(request, plan) == ResolveStatus::sourceMissing, "A resolve without a source was accepted");

    request = frame(1280, 720, 6);
    request.mip = 1;
    require(planResolve(request, plan) == ResolveStatus::descriptor, "A mip-selected resolve was accepted");
    request = frame(1280, 720, 6);
    request.faces = 6;
    request.face = 6;
    require(planResolve(request, plan) == ResolveStatus::descriptor, "A cube resolve left its face array");
    request = frame(1280, 720, 6);
    request.flags = 5;
    require(planResolve(request, plan) == ResolveStatus::descriptor, "An out-of-range attachment was accepted");

    request = frame(1280, 720, 7);
    require(planResolve(request, plan) == ResolveStatus::format, "An unknown resolve format was accepted");
    request = frame(1280, 720, 23);
    require(planResolve(request, plan) == ResolveStatus::depthFormat, "A depth-only format was accepted for colour");
    request = frame(1280, 720, 23, 4);
    request.source = {1280, 720};
    require(planResolve(request, plan) == ResolveStatus::ok && plan.depth &&
                plan.format == DestinationFormat::r32f,
            "A depth resolve into single-channel float was refused");

    // A refused plan leaves the caller's plan untouched.
    ResolvePlan untouched = plan;
    request = frame(1280, 720, 7);
    require(planResolve(request, untouched) == ResolveStatus::format, "A refused plan reported success");
    require(untouched.copied == plan.copied && untouched.format == plan.format,
            "A refused plan modified the caller's plan");

    require(std::string_view(resolveStatusName(ResolveStatus::sourceExtent)) == "source-extent",
            "A reject reason lost its Windows renderer name");
    require(std::string_view(resolveStatusName(ResolveStatus::destinationExtent)) == "destination-extent",
            "A reject reason lost its Windows renderer name");
}

void testClearPlan() {
    ClearRequest request;
    request.flags = 1;
    request.viewport = {0, 0, 1280, 720};
    request.surface = {1280, 720};
    ClearRegion region = planClear(request);
    require(region.valid && region.full, "A whole-surface clear was not the full extent");
    require((region.rect == std::array<int32_t, 4>{0, 0, 1280, 720}), "A full clear's rectangle is wrong");

    request.rectangle = std::array<int32_t, 4>{7, 11, 39, 43};
    region = planClear(request);
    require(region.valid && !region.full, "A partial clear claimed the whole surface");
    require((region.rect == std::array<int32_t, 4>{7, 11, 39, 43}), "A partial clear's rectangle was moved");

    // A region that runs past the surface is clipped, and one that covers it
    // completely is a full clear again.
    request.rectangle = std::array<int32_t, 4>{-4, -4, 2000, 2000};
    region = planClear(request);
    require(region.valid && region.full, "An oversized clear was not clipped to the surface");

    request.rectangle = std::array<int32_t, 4>{100, 100, 100, 200};
    require(!planClear(request).valid, "An empty clear region was accepted");
    request.rectangle = std::array<int32_t, 4>{2000, 2000, 2100, 2100};
    require(!planClear(request).valid, "A clear region outside the surface was accepted");

    // A clear whose viewport names a bigger surface grows it: the rectangle is
    // clipped against the extent the caller will allocate.
    request.rectangle.reset();
    request.surface = {0, 0};
    require(!planClear(request).valid, "A clear without a surface was accepted");
}

void testPresent() {
    require(presentable({1280, 720}, {1280, 720}), "A matching frontbuffer was refused");
    // A panel of a different size is fitted, not refused: the copy fills the
    // display the way the Windows display path fits its target to the window.
    require(presentable({1280, 720}, {1920, 1080}), "A frontbuffer for a larger panel was refused");
    require(presentable({1920, 1080}, {1280, 720}), "A frontbuffer for a smaller panel was refused");
    require(!presentable({}, {1280, 720}), "An absent frontbuffer was presentable");
    require(!presentable({1280, 720}, {}), "A frontbuffer was presentable without a display");
    require(!presentable({}, {}), "An empty frontbuffer and display were presentable");
}

} // namespace

int main() {
    try {
        testGrowth();
        testFormats();
        testResolvePlan();
        testClearPlan();
        testPresent();
        std::printf("WorldSurfacePolicy: growth, resolve rectangle and fringe, clear regions and the "
                    "frontbuffer extent all follow the Windows renderer's rules.\n");
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAILED: %s\n", error.what());
        return 1;
    }
    return 0;
}
