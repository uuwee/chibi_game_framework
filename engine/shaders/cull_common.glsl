// Frustum test shared by the counting and writing passes, so both passes
// accept exactly the same instances.
#include "common.glsl"

layout(local_size_x = 64) in;

bool instance_visible(Instance instance, Mesh mesh) {
  if (mesh.index_count == 0u)
    return false;
  vec3 center = (instance.transform * vec4(mesh.sphere.xyz, 1.0)).xyz;
  float scale = max(length(instance.transform[0].xyz),
                    max(length(instance.transform[1].xyz),
                        length(instance.transform[2].xyz)));
  float radius = mesh.sphere.w * scale;
  for (int i = 0; i < 6; i++) {
    vec4 plane = pc.render.frustum[i];
    if (dot(plane.xyz, center) + plane.w < -radius)
      return false;
  }
  return true;
}

uint draw_index(uint pipeline, uint mesh) {
  return pipeline * pc.render.mesh_capacity + mesh;
}
