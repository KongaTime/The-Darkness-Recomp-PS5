#version 450
// Video meshes: the guest's decoder leaves an 8-bit luma plane and a
// pair-reversed NV12 chroma plane (bytes V,U, sampled here as R=V, G=U, so
// `.gr` selects logical U,V). The matrix and the clamping are the D3D11
// `videoMain` unchanged; the alpha it writes is zero because the movie path
// blends nothing.
layout(binding = 0) uniform sampler2D lumaImage;
layout(binding = 1) uniform sampler2D chromaImage;

layout(location = 0) in vec2 vUv;
layout(location = 1) in vec4 vColor;
layout(location = 0) out vec4 oColor;

void main() {
    float y = texture(lumaImage, vUv).r;
    vec2 uv = texture(chromaImage, vUv).gr - 0.5;
    vec3 rgb = vec3(y * 1.164 - 0.073035294117647058823529411764044);
    rgb += vec3(1.596 * uv.y, -0.391 * uv.x - 0.813 * uv.y, 2.018 * uv.x);
    oColor = vec4(clamp(rgb * vColor.rgb, 0.0, 1.0), 0.0);
}
