#version 450
// Present pass of the Vulkan backend: the back buffer, opaque; with p.c.x the
// game's gamma ramp (SetGammaRamp) through a 256x1 table, interpolated as the
// Direct3D 9 layer's present does (android/host/d3d9_gl.c gamma_present).
layout(binding = 0) uniform sampler2D bb;
layout(binding = 1) uniform sampler2D lut;
layout(push_constant) uniform P { vec4 a; vec4 b; vec4 c; } p;
layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 color;
float ramp(float v, int c) {
	float x = clamp(v, 0.0, 1.0) * 255.0;
	float i = floor(x);
	float a = texelFetch(lut, ivec2(int(i), 0), 0)[c], b = texelFetch(lut, ivec2(min(int(i) + 1, 255), 0), 0)[c];
	return mix(a, b, x - i);
}
void main() {
	vec4 c = texture(bb, uv);
	color = p.c.x > 0.5 ? vec4(ramp(c.r, 0), ramp(c.g, 1), ramp(c.b, 2), 1.0) : vec4(c.rgb, 1.0);
}
