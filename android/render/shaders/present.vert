#version 450
// Present pass of the Vulkan backend: the back buffer's quad, corners in the
// push constants (aspect fit and pre-rotation are applied by the caller);
// c.x: 1 when the game's gamma ramp applies (present.frag).
layout(push_constant) uniform P { vec4 a; vec4 b; vec4 c; } p;
layout(location = 0) out vec2 uv;
void main() {
	int i = gl_VertexIndex;
	vec2 pos = i == 0 ? p.a.xy : i == 1 ? p.a.zw : i == 2 ? p.b.xy : p.b.zw;
	uv = vec2(float(i & 1), float(i >> 1));
	gl_Position = vec4(pos, 0.0, 1.0);
}
