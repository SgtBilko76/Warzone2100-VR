#version 450

// xy scales full-texture UVs down to the rendered sub-rect of the source,
// zw clamps just inside its edge so bilinear filtering cannot bleed in
// texels from the unrendered region (both are identity-like at full size)
layout(std140, set = 0, binding = 0) uniform cbuffer {
	vec4 uvScaleClamp;
};

layout(set = 1, binding = 0) uniform sampler2D Texture;

layout(location = 0) in vec2 texCoords;
layout(location = 0) out vec4 FragColor;

// Mixed reality (VR): where nothing was drawn (black background) the image is transparent, so the
// headset's passthrough shows the real room around the battlefield. Ignored by opaque presentation.
vec4 wzMixedRealityKey(vec3 c)
{
	return vec4(c, clamp(max(c.r, max(c.g, c.b)) * 48.0, 0.0, 1.0));
}

void main()
{
	vec2 uv = min(texCoords * uvScaleClamp.xy, uvScaleClamp.zw);
	vec3 texColour = texture(Texture, uv).rgb;

	FragColor = wzMixedRealityKey(texColour);
}
