#version 460
// Depth prepass for opaque and masked geometry. Also stores the surface
// parameters used by light culling and global illumination.
#include "common.glsl"
#include "material.glsl"

layout(constant_id = 0) const bool ALPHA_TEST = false;

layout(location = 0) in vec3 in_world_position;
layout(location = 1) in vec3 in_normal;
layout(location = 2) in vec4 in_tangent;
layout(location = 3) in vec2 in_uv;
layout(location = 4) flat in uint in_material;

layout(location = 0) out vec4 out_normal;
layout(location = 1) out vec4 out_albedo;

void main() {
  Material material = materials[in_material];
  Surface s = evaluate_surface(material, in_uv, in_normal, in_tangent,
                               gl_FrontFacing);
  if (ALPHA_TEST && s.alpha < material.alpha_cutoff)
    discard;
  out_normal = vec4(encode_normal(s.normal), s.roughness, s.metallic);
  out_albedo = vec4(s.base_color, s.occlusion);
}
