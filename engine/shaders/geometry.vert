#version 460
// Shared by the depth prepass and the forward passes. gl_Position is
// invariant so the forward pass can test depth for equality.
#include "common.glsl"

layout(location = 0) in vec3 in_position;
layout(location = 1) in vec3 in_normal;
layout(location = 2) in vec2 in_uv;

layout(location = 0) out vec3 out_world_position;
layout(location = 1) out vec3 out_normal;
layout(location = 2) flat out uint out_material;

invariant gl_Position;

void main() {
  uint instance_index = pc.render.visible.data[gl_InstanceIndex];
  Instance instance = pc.render.instances.data[instance_index];
  vec4 world = instance.transform * vec4(in_position, 1.0);
  out_world_position = world.xyz;
  out_normal = mat3(instance.normal_transform) * in_normal;
  out_material = instance.material;
  gl_Position = pc.render.view_projection * world;
}
