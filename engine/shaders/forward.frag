#version 460
// Forward+ shading with the light list of the pixel's tile. Writes direct
// lighting and emission; indirect lighting is added later for opaque
// surfaces.
#include "common.glsl"

layout(constant_id = 0) const bool TRANSPARENT = false;

layout(location = 0) in vec3 in_world_position;
layout(location = 1) in vec3 in_normal;
layout(location = 2) flat in uint in_material;

layout(location = 0) out vec4 out_color;

void main() {
  Material material = pc.render.materials.data[in_material];
  vec3 normal = normalize(in_normal);
  if (!gl_FrontFacing)
    normal = -normal;
  vec3 albedo = material.base_color.rgb;
  vec3 view = normalize(pc.render.camera_position.xyz - in_world_position);

  uvec2 tile = uvec2(gl_FragCoord.xy) / TILE_SIZE;
  uint base = (tile.y * pc.render.tile_count.x + tile.x) *
              (MAX_LIGHTS_PER_TILE + 1u);
  uint count = pc.render.tile_lights.data[base];

  vec3 color = material.emissive.rgb;
  for (uint i = 0u; i < count; i++) {
    Light light = pc.render.lights.data[pc.render.tile_lights.data[base + 1u + i]];
    vec3 to_light = light.position_radius.xyz - in_world_position;
    float distance_squared = dot(to_light, to_light);
    float radius = light.position_radius.w;
    if (distance_squared >= radius * radius)
      continue;
    vec3 direction = to_light * inversesqrt(distance_squared);
    float n_dot_l = max(dot(normal, direction), 0.0);
    if (n_dot_l <= 0.0)
      continue;
    // Inverse-square falloff, windowed to reach zero at the radius.
    float ratio = distance_squared / (radius * radius);
    float window = clamp(1.0 - ratio * ratio, 0.0, 1.0);
    float attenuation = window * window / max(distance_squared, 0.01);
    vec3 halfway = normalize(direction + view);
    float specular = pow(max(dot(normal, halfway), 0.0), 64.0) * 0.25;
    color += light.color.rgb * attenuation * n_dot_l * (albedo / PI + specular);
  }

  if (TRANSPARENT) {
    // Transparent surfaces receive no global illumination; approximate it
    // with the environment.
    color += albedo * pc.render.sky.rgb;
    float alpha = material.base_color.a;
    out_color = vec4(color * alpha, alpha);
  } else {
    out_color = vec4(color, 1.0);
  }
}
