#version 460
// Alpha test for masked shadow casters. Opaque casters need no fragment
// shader.
#include "common.glsl"
#include "material.glsl"

layout(location = 3) in vec2 in_uv;
layout(location = 4) flat in uint in_material;

void main() {
  Material material = materials[in_material];
  if (material_alpha(material, in_uv) < material.alpha_cutoff)
    discard;
}
