#version 460
// Forward+ shading with the light list of the pixel's tile. Opaque surfaces
// get direct lighting and emission; indirect lighting is added later from
// the depth prepass's surface parameters. Transparent surfaces also get
// indirect lighting here and are blended with premultiplied alpha.
#include "common.glsl"
#include "material.glsl"
#include "lighting.glsl"

layout(constant_id = 0) const bool TRANSPARENT = false;

layout(location = 0) in vec3 in_world_position;
layout(location = 1) in vec3 in_normal;
layout(location = 2) in vec4 in_tangent;
layout(location = 3) in vec2 in_uv;
layout(location = 4) flat in uint in_material;

layout(location = 0) out vec4 out_color;

void main() {
  Material material = materials[in_material];
  // Transparent geometry is drawn without face culling.
  if (TRANSPARENT && !gl_FrontFacing && (material.pipeline & 1u) == 0u)
    discard;
  Surface s = evaluate_surface(material, in_uv, in_normal, in_tangent,
                               gl_FrontFacing);
  vec3 v = normalize(render_data.camera_position.xyz - in_world_position);
  vec3 color = direct_lighting(s, in_world_position, v, uvec2(gl_FragCoord.xy));

  if (TRANSPARENT) {
    vec3 diffuse, specular;
    indirect_lighting(s, gl_FragCoord.xy, in_world_position, v, false, diffuse,
                      specular);
    if (render_data.debug_mode == DEBUG_MODE_DIRECT)
      diffuse = specular = vec3(0.0);
    float alpha = clamp(s.alpha, 0.0, 1.0);
    // Reflections stay at full strength on clear surfaces.
    out_color =
        vec4(min((color + diffuse + s.emissive) * alpha + specular,
                 vec3(HALF_MAX)),
             alpha);
  } else {
    out_color = vec4(min(color + s.emissive, vec3(HALF_MAX)), 1.0);
  }
}
