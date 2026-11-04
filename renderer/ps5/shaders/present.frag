#version 450
// The console's frontbuffer copy, fragment stage.
//
// The engine finishes a scene by resolving into its frontbuffer texture and
// presenting it. On Windows the preview copies that frontbuffer into its
// presentation target and its display pass then fits the target to the window;
// the console has one step instead, so this shader fills the swapchain image
// from the frontbuffer whatever the panel's size is. At equal extents the
// filter is exact -- a sample at a texel centre returns that texel -- and at
// different extents it is the linear fit the Windows display path performs.
//
// Once the engine has set its display ramp (VdSetDisplayGamma's table, the
// Windows preview's displayGammaPixel pass), the colour is mapped through it:
// the original's frontbuffer holds values the Xbox scan-out converts with that
// table, and the game's grade lives there.
//
// The resolve itself (transfer.frag) is an integer texel copy and stays that
// way: only this, the step that reaches the screen, is allowed to scale.
layout(binding = 0) uniform sampler2D sourceImage;

// 768 interleaved RGB entries: (base, delta) for the piecewise-linear table's
// 128 segments, or (value, unused) for the 256-entry table. Values are 10-bit
// output scaled by 64.
layout(std430, binding = 1) readonly buffer DisplayRamp {
    uvec2 entries[768];
} ramp;

layout(push_constant) uniform Present {
    // x, y: the destination image's extent in pixels. z: the display ramp, 0 for
    // none, 1 for the 256-entry table, 2 for the piecewise-linear one.
    vec4 extent;
    // x: brightness multiplier; y: FXAA enabled.
    vec4 controls;
} present;

layout(location = 0) out vec4 oColor;

float rampChannel(float value, uint channel) {
    if (present.extent.z > 1.5) {
        const uint q = uint(clamp(value, 0.0, 1.0) * 1023.0 + 0.5);
        const uvec2 entry = ramp.entries[(q >> 3) * 3u + channel];
        return clamp((float(entry.x) + float((q & 7u) * entry.y) / 8.0) / (64.0 * 1023.0), 0.0, 1.0);
    }
    const uint q = uint(clamp(value, 0.0, 1.0) * 255.0 + 0.5);
    return clamp(float(ramp.entries[q * 3u + channel].x) / (64.0 * 1023.0), 0.0, 1.0);
}

vec3 filteredColor(vec2 uv) {
    vec3 center = texture(sourceImage, uv).rgb;
    if (present.controls.y < 0.5) return center;
    vec2 pixel = 1.0 / vec2(textureSize(sourceImage, 0));
    vec3 nw = texture(sourceImage, uv + vec2(-1, -1) * pixel).rgb;
    vec3 ne = texture(sourceImage, uv + vec2( 1, -1) * pixel).rgb;
    vec3 sw = texture(sourceImage, uv + vec2(-1,  1) * pixel).rgb;
    vec3 se = texture(sourceImage, uv + vec2( 1,  1) * pixel).rgb;
    const vec3 luma = vec3(0.299, 0.587, 0.114);
    float m = dot(center, luma), a = dot(nw, luma), b = dot(ne, luma);
    float c = dot(sw, luma), d = dot(se, luma);
    float low = min(m, min(min(a, b), min(c, d)));
    float high = max(m, max(max(a, b), max(c, d)));
    if (high - low < max(0.0312, high * 0.125)) return center;
    vec2 direction = vec2(-((a + b) - (c + d)), (a + c) - (b + d));
    float reduce = max((a + b + c + d) * (0.25 * 0.125), 1.0 / 128.0);
    direction = clamp(direction / (min(abs(direction.x), abs(direction.y)) + reduce), -8.0, 8.0) * pixel;
    vec3 first = 0.5 * (texture(sourceImage, uv + direction * (-1.0 / 6.0)).rgb +
                        texture(sourceImage, uv + direction * ( 1.0 / 6.0)).rgb);
    vec3 second = first * 0.5 + 0.25 * (texture(sourceImage, uv - direction * 0.5).rgb +
                                      texture(sourceImage, uv + direction * 0.5).rgb);
    float secondLuma = dot(second, luma);
    return secondLuma < low || secondLuma > high ? first : second;
}

void main() {
    // gl_FragCoord is in the destination's pixels with the origin at the top
    // left, which is also the source's first row.
    vec4 color = vec4(filteredColor(gl_FragCoord.xy / present.extent.xy), 1.0);
    if (present.extent.z > 0.5)
        color.rgb = vec3(rampChannel(color.r, 0u), rampChannel(color.g, 1u), rampChannel(color.b, 2u));
    oColor = vec4(clamp(color.rgb * present.controls.x, 0.0, 1.0), 1.0);
}
