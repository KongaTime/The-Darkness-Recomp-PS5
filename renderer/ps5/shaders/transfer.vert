#version 450
// The console's resolve and frontbuffer copy, vertex stage.
//
// The engine's resolve is a pixel copy, not geometry: WorldRendererD3D11's
// `engine_resolve_copy` vertex shader emits a full-screen triangle and lets the
// viewport place it. This is that shader, so a resolve covers exactly the
// rectangle the viewport and scissor select and nothing is scaled.
//
// The triangle ignores Vulkan's y-down clip space because the fragment stage
// reads the source by framebuffer coordinate (gl_FragCoord), which is in the
// same space on both APIs.
void main() {
    const vec2 corner = vec2(float((gl_VertexIndex << 1) & 2), float(gl_VertexIndex & 2));
    gl_Position = vec4(corner * 2.0 - 1.0, 0.0, 1.0);
}
