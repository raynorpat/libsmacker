#version 450
layout(binding = 0) uniform sampler2D u_frame;
layout(binding = 1) uniform sampler2D u_palette;
layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 frag_color;

void main()
{
    float index = texture(u_frame, v_uv).r;
    int slot = int(index * 255.0 + 0.5);
    vec3 rgb = texelFetch(u_palette, ivec2(slot, 0), 0).rgb;
    frag_color = vec4(rgb, 1.0);
}
