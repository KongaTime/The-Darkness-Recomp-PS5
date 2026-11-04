#version 450
// Text and glyph meshes: one A8 atlas texel is the coverage, and the vertex
// colour carries the tint. Matches the D3D11 `pixelMain`.
layout(binding = 0) uniform sampler2D alphaImage;

layout(location = 0) in vec2 vUv;
layout(location = 1) in vec4 vColor;
layout(location = 0) out vec4 oColor;

void main() {
    oColor = vec4(vColor.rgb, vColor.a * texture(alphaImage, vUv).r);
}
