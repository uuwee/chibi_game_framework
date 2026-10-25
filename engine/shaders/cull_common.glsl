// Visibility test shared by the counting and writing passes, so both passes
// accept exactly the same instances.
#include "common.glsl"

layout(local_size_x = 64) in;

// View 0 is the camera; views 1.. are the shadow cascades.
bool instance_visible(Instance instance, Mesh mesh, uint view) {
  if (mesh.index_count == 0u)
    return false;
  vec3 center = (instance.transform * vec4(mesh.sphere.xyz, 1.0)).xyz;
  float scale = max(length(instance.transform[0].xyz),
                    max(length(instance.transform[1].xyz),
                        length(instance.transform[2].xyz)));
  float radius = mesh.sphere.w * scale;
  for (uint i = 0u; i < 6u; i++) {
    vec4 plane = render_data.frustums[view * 6u + i];
    if (dot(plane.xyz, center) + plane.w < -radius)
      return false;
  }
  // Casters smaller than a shadow map texel leave no visible shadow.
  if (view > 0u && radius < 0.5 * render_data.shadow_texel[view - 1u])
    return false;
  return true;
}

uint draw_key(uint view, uint kind, uint mesh) {
  return (view * DRAW_KIND_COUNT + kind) * MAX_MESHES + mesh;
}
