#version 450

layout(push_constant) uniform PushConstants
{
    vec2 center;       // cursor centre, framebuffer pixels
    float outerRadius; // translucent dark disc, pixels
    float innerRadius; // brighter centre dot, pixels
    float opacity;     // 0..1 disc opacity
    float pressed;     // 1 while the stylus is down
    vec2 padding;
} pc;

layout(location = 0) out vec4 outColor;

void main()
{
    float dist = distance(gl_FragCoord.xy, pc.center);
    float aa = 1.25;
    float disc = 1.0 - smoothstep(pc.outerRadius - aa, pc.outerRadius, dist);
    float core = 1.0 - smoothstep(max(pc.innerRadius - aa, 0.0), pc.innerRadius, dist);

    // Dark, semi-transparent disc with a brighter, slightly more opaque centre.
    vec3 color = mix(vec3(0.02), vec3(1.0), core);
    float alpha = disc * mix(pc.opacity, min(pc.opacity + 0.30, 1.0), core);

    if (alpha <= 0.0)
        discard;
    outColor = vec4(color, alpha);
}
