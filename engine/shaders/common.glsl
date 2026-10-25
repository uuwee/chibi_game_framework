// Shared declarations. Layouts must match the GPU structs in
// engine/renderer_internal.h.
#extension GL_EXT_buffer_reference : require
#extension GL_EXT_scalar_block_layout : require

#define PIPELINE_KIND_COUNT 4u
#define TILE_SIZE 16u
#define MAX_LIGHTS_PER_TILE 255u
#define GI_PROBE_SPACING 8u
#define GI_DIRECTIONS 4u
#define GI_CASCADE_COUNT 5u
#define GI_PYRAMID_LEVELS 4u
// View depth stored in the pyramid for sky texels (fits a 16-bit float).
#define SKY_DEPTH 60000.0
#define PI 3.14159265358979

// DebugMode in engine/renderer_internal.h.
#define DEBUG_MODE_DIRECT 1u
#define DEBUG_MODE_INDIRECT 2u
#define DEBUG_MODE_LIGHTS 3u

struct Instance {
  mat4 transform;
  mat4 normal_transform;
  uint mesh;
  uint material;
  uint pad0;
  uint pad1;
};

struct Mesh {
  uint first_index;
  uint index_count;
  int vertex_offset;
  uint pad0;
  vec4 sphere; // xyz: center, w: radius (object space)
};

struct Material {
  vec4 base_color;
  vec4 emissive;
  uint pipeline;
  uint pad0;
  uint pad1;
  uint pad2;
};

struct Light {
  vec4 position_radius;
  vec4 color; // rgb: color * intensity
};

// Cascade 0 probe: world position (w = 1 on a surface, 0 on the sky) and L1
// spherical harmonics of the radiance over its hemisphere.
struct Probe {
  vec4 position;
  vec4 sh[4];
};

struct DrawCommand {
  uint index_count;
  uint instance_count;
  uint first_index;
  int vertex_offset;
  uint first_instance;
};

layout(buffer_reference, scalar) readonly buffer InstanceBuffer { Instance data[]; };
layout(buffer_reference, scalar) readonly buffer MeshBuffer { Mesh data[]; };
layout(buffer_reference, scalar) readonly buffer MaterialBuffer { Material data[]; };
layout(buffer_reference, scalar) readonly buffer LightBuffer { Light data[]; };
layout(buffer_reference, scalar) buffer UintBuffer { uint data[]; };
layout(buffer_reference, scalar) buffer DrawCommandBuffer { DrawCommand data[]; };
layout(buffer_reference, scalar) buffer ProbeBuffer { Probe data[]; };

layout(buffer_reference, scalar) readonly buffer RenderData {
  mat4 view;
  mat4 projection;
  mat4 view_projection;
  mat4 inverse_view_projection;
  mat4 inverse_projection;
  vec4 camera_position;
  vec4 sky;
  vec4 frustum[6];
  uvec2 screen_size;
  uvec2 tile_count;
  uint entry_count;
  uint light_count;
  uint mesh_count;
  uint mesh_capacity;
  uint cascade_count;
  float gi_interval;
  uint debug_mode;
  uint pad0;
  InstanceBuffer instances;
  MeshBuffer meshes;
  MaterialBuffer materials;
  UintBuffer entries;
  UintBuffer visible;
  DrawCommandBuffer draws;
  LightBuffer lights;
  UintBuffer tile_lights;
  ProbeBuffer probes;
};

layout(push_constant, scalar) uniform PushConstants {
  RenderData render;
  uint cascade;
  uint pad;
} pc;

layout(set = 0, binding = 0) uniform sampler2D depth_texture;
layout(set = 0, binding = 1, rgba16f) uniform image2D normal_image;
layout(set = 0, binding = 2, rgba8) uniform image2D albedo_image;
layout(set = 0, binding = 3, rgba16f) uniform image2D hdr_image;
layout(set = 0, binding = 4, rgba16f) uniform image2D cascade_images[GI_CASCADE_COUNT];
layout(set = 0, binding = 5, rgba16f) uniform image2D pyramid_levels[GI_PYRAMID_LEVELS];

// Rows of matrices that turn positions into view depth: the distance along
// the view axis, positive in front of the camera whatever the handedness.
struct DepthRows {
  vec4 view;         // world position -> view z
  vec4 projection_z; // NDC -> view z (numerator)
  vec4 projection_w; // NDC -> view w (denominator)
  float sign;        // makes view z positive in front of the camera
};

DepthRows depth_rows() {
  mat4 v = pc.render.view;
  mat4 ip = pc.render.inverse_projection;
  DepthRows rows;
  rows.view = vec4(v[0][2], v[1][2], v[2][2], v[3][2]);
  rows.projection_z = vec4(ip[0][2], ip[1][2], ip[2][2], ip[3][2]);
  rows.projection_w = vec4(ip[0][3], ip[1][3], ip[2][3], ip[3][3]);
  vec4 center = vec4(0.0, 0.0, 0.5, 1.0);
  float z = dot(rows.projection_z, center) / dot(rows.projection_w, center);
  rows.sign = z < 0.0 ? -1.0 : 1.0;
  return rows;
}

float ndc_view_depth(DepthRows rows, vec2 ndc, float depth) {
  vec4 p = vec4(ndc, depth, 1.0);
  return dot(rows.projection_z, p) / dot(rows.projection_w, p) * rows.sign;
}

float world_view_depth(DepthRows rows, vec3 position) {
  return dot(rows.view, vec4(position, 1.0)) * rows.sign;
}

// World position of a pixel center at the given depth.
vec3 reconstruct_position(vec2 pixel, float depth) {
  vec2 ndc = pixel / vec2(pc.render.screen_size) * 2.0 - 1.0;
  vec4 world = pc.render.inverse_view_projection * vec4(ndc, depth, 1.0);
  return world.xyz / world.w;
}

vec2 sign_not_zero(vec2 v) {
  return vec2(v.x >= 0.0 ? 1.0 : -1.0, v.y >= 0.0 ? 1.0 : -1.0);
}

// Octahedral mapping between the unit sphere and [0, 1]^2.
vec3 octahedral_decode(vec2 uv) {
  vec2 f = uv * 2.0 - 1.0;
  vec3 n = vec3(f, 1.0 - abs(f.x) - abs(f.y));
  if (n.z < 0.0)
    n.xy = (1.0 - abs(n.yx)) * sign_not_zero(n.xy);
  return normalize(n);
}
