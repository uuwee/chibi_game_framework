#version 460
// Shared by the shadow, depth prepass and forward passes. pc.view selects
// the camera (0) or a shadow cascade. gl_Position is invariant so the
// forward pass can test depth for equality.
#include "common.glsl"

layout(location = 0) in vec3 in_position;
layout(location = 1) in vec3 in_normal;
layout(location = 2) in vec2 in_uv;
layout(location = 3) in vec4 in_tangent;

layout(location = 0) out vec3 out_world_position;
layout(location = 1) out vec3 out_normal;
layout(location = 2) out vec4 out_tangent;
layout(location = 3) out vec2 out_uv;
layout(location = 4) flat out uint out_material;

invariant gl_Position;

void main() {
  uint instance_index = visible[gl_InstanceIndex];
  Instance instance = instances[instance_index];
  vec4 world = instance.transform * vec4(in_position, 1.0);
  mat3 linear = mat3(instance.transform);
  out_world_position = world.xyz;
  out_normal = mat3(instance.normal_transform) * in_normal;
  // A mirroring transform flips the handedness of the tangent frame.
  float handedness = determinant(linear) < 0.0 ? -1.0 : 1.0;
  out_tangent = vec4(linear * in_tangent.xyz, in_tangent.w * handedness);
  out_uv = in_uv;
  out_material = instance.material;
  mat4 view_projection =
      pc.view == 0u ? render_data.view_projection
                    : render_data.shadow_view_projection[pc.view - 1u];
  gl_Position = view_projection * world;
}
