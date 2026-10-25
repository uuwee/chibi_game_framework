// Surface parameters of a material at a fragment. Shared by the depth
// prepass and the forward passes so both see the same surface. Fragment
// shaders only: uses screen-space derivatives.
#include "surface.glsl"

vec4 sample_material_texture(uint index, vec2 uv) {
  return texture(textures[nonuniformEXT(index)], uv);
}

float material_alpha(Material material, vec2 uv) {
  float alpha = material.base_color.a;
  uint texture_index = material.textures[MATERIAL_TEXTURE_BASE_COLOR];
  if (texture_index != TEXTURE_NONE)
    alpha *= sample_material_texture(texture_index, uv).a;
  return alpha;
}

Surface evaluate_surface(Material material, vec2 uv, vec3 vertex_normal,
                         vec4 tangent, bool front_facing) {
  Surface s;
  vec4 base = material.base_color;
  uint base_texture = material.textures[MATERIAL_TEXTURE_BASE_COLOR];
  if (base_texture != TEXTURE_NONE)
    base *= sample_material_texture(base_texture, uv);
  s.base_color = base.rgb;
  s.alpha = base.a;

  vec3 n = normalize(vertex_normal);
  vec3 side = front_facing ? n : -n;
  s.geometric_normal = side;
  s.normal = side;
  uint normal_texture = material.textures[MATERIAL_TEXTURE_NORMAL];
  if (normal_texture != TEXTURE_NONE) {
    vec3 t = tangent.xyz - n * dot(n, tangent.xyz);
    float length_t = length(t);
    if (length_t > 1e-6) {
      t /= length_t;
      // The tangent frame follows the texture, whichever side is viewed.
      vec3 b = cross(n, t) * (tangent.w < 0.0 ? -1.0 : 1.0);
      vec2 xy = sample_material_texture(normal_texture, uv).rg * 2.0 - 1.0;
      float z = sqrt(max(1.0 - dot(xy, xy), 0.0));
      s.normal = normalize(t * xy.x + b * xy.y + side * z);
    }
  }

  s.roughness = material.roughness;
  s.metallic = material.metallic;
  s.occlusion = 1.0;
  uint orm_texture = material.textures[MATERIAL_TEXTURE_ORM];
  if (orm_texture != TEXTURE_NONE) {
    vec3 orm = sample_material_texture(orm_texture, uv).rgb;
    s.occlusion = mix(1.0, orm.r, material.occlusion_strength);
    s.roughness *= orm.g;
    s.metallic *= orm.b;
  }

  // Widen the highlight where the normal varies within a pixel, so small
  // bright reflections do not alias.
  vec3 dndx = dFdx(s.normal);
  vec3 dndy = dFdy(s.normal);
  float variance = 0.25 * (dot(dndx, dndx) + dot(dndy, dndy));
  float alpha = s.roughness * s.roughness;
  float alpha2 = clamp(alpha * alpha + min(2.0 * variance, 0.18), 0.0, 1.0);
  s.roughness = sqrt(sqrt(alpha2));

  s.emissive = material.emissive;
  uint emissive_texture = material.textures[MATERIAL_TEXTURE_EMISSIVE];
  if (emissive_texture != TEXTURE_NONE)
    s.emissive *= sample_material_texture(emissive_texture, uv).rgb;
  return s;
}
