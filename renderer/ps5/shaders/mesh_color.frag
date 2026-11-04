#version 450
// Colour meshes: RGBA8 artwork from the guest's text/UI decoder, tinted by the
// vertex colour. Matches the D3D11 `colorMain`.
layout(binding = 0) uniform sampler2D colorImage;

layout(location = 0) in vec2 vUv;
layout(location = 1) in vec4 vColor;
layout(location = 0) out vec4 oColor;

void main() {
    oColor = texture(colorImage, vUv) * vColor;
}
