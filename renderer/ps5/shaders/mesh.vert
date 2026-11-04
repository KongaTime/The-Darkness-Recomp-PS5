#version 450
// The console's simple-mesh vertex stage.
//
// It is the D3D11 preview's `vertexMain` translated to GLSL: the projection is
// the engine's row-major matrix exactly as the guest handed it over, applied as
// a row vector (`mul(float4(position, 1), projection)` in HLSL), which GLSL
// spells as a row vector times a matrix whose 16 floats are in the same memory
// order. Deviating from that order would transpose the whole UI off screen.
layout(location = 0) in vec3 position;
layout(location = 1) in vec2 uv;
layout(location = 2) in vec4 color;

layout(push_constant, row_major) uniform Transform {
    mat4 projection;
} transform;

layout(location = 0) out vec2 vUv;
layout(location = 1) out vec4 vColor;

void main() {
    vec4 clip = vec4(position, 1.0) * transform.projection;
    // The guest's projection targets D3D's clip space, where +y is up; Vulkan's
    // framebuffer has +y down, so the y axis is flipped once here instead of
    // flipping every viewport the frame sets.
    clip.y = -clip.y;
    gl_Position = clip;
    vUv = uv;
    vColor = color;
}
