// Surface parameters at a shaded point.
struct Surface {
  vec3 base_color;
  float alpha;
  vec3 normal;           // shading normal, on the viewed side
  vec3 geometric_normal; // interpolated vertex normal, on the viewed side
  float roughness;
  float metallic;
  float occlusion;
  vec3 emissive;
};
