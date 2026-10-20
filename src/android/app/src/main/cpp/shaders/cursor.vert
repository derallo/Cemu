#version 450

void main()
{
    // Full-screen triangle. The fragment shader gets the circle position from
    // gl_FragCoord, so no vertex buffer or attributes are needed.
    vec2 p = vec2(float((gl_VertexIndex << 1) & 2), float(gl_VertexIndex & 2));
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
