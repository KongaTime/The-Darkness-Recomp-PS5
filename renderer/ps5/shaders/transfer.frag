#version 450
// The console's resolve and frontbuffer copy, fragment stage.
//
// It is the engine's own `Transfer` program translated to GLSL:
//
//     float4 pixelMain(float4 p : SV_Position) : SV_Target {
//         return sourceImage.Load(int3(int2(p.xy - originScale.xy + area.xy), 0))
//             * originScale.z;
//     }
//
// A render target is read by integer texel, never filtered, so a resolve
// reproduces the surface exactly: the original's surface holds the scene in
// half-float and its resolve aliases that into the guest's format, which
// filtering would blur. `originScale.z` is the engine's fetch exponent
// (`exp2(exponent)`) and is 1 for a plain copy, which is what the frontbuffer
// present uses.
layout(binding = 0) uniform sampler2D sourceImage;

layout(push_constant) uniform Transfer {
    // The source rectangle's origin in surface pixels (x, y); z, w are the
    // destination end in the original's constants and are not read here.
    vec4 area;
    // The destination origin in the target's pixels (x, y), and the fetch
    // exponent's scale (z).
    vec4 originScale;
} transfer;

layout(location = 0) out vec4 oColor;

void main() {
    const ivec2 texel = ivec2(gl_FragCoord.xy - transfer.originScale.xy + transfer.area.xy);
    oColor = texelFetch(sourceImage, texel, 0) * transfer.originScale.z;
}
