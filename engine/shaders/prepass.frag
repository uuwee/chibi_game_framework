#version 460
// Depth prepass for opaque geometry. Also stores the surface normal and
// albedo used by light culling and global illumination.
#include "common.glsl"

layout(location = 0) in vec3 in_world_position;
layout(location = 1) in vec3 in_normal;
layout(location = 2) flat in uint in_material;

layout(location = 0) out vec4 out_normal;
layout(location = 1) out vec4 out_albedo;

void main() {
  vec3 normal = normalize(in_normal);
  if (!gl_FrontFacing)
    normal = -normal;
  Material material = pc.render.materials.data[in_material];
  out_normal = vec4(normal, 1.0);
  out_albedo = vec4(material.base_color.rgb, 1.0);
}
