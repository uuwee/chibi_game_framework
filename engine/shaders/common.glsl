// Shared declarations. Layouts must match the GPU structs and bindings in
// engine/renderer_internal.h.
#extension GL_EXT_scalar_block_layout : require
#extension GL_EXT_nonuniform_qualifier : require

#define MAX_INSTANCES (1u << 18)
#define MAX_MESHES (1u << 16)
#define DRAW_KIND_COUNT 4u
#define PIPELINE_TRANSPARENT 4u
#define SHADOW_CASCADE_COUNT 4u
#define VIEW_COUNT 5u
#define TRANSPARENT_CAPACITY 2048u
#define SHADOW_MAP_SIZE 2048u
#define TILE_SIZE 16u
#define MAX_LIGHTS_PER_TILE 255u
#define GI_PROBE_SPACING 8u
#define GI_DIRECTIONS 4u
#define GI_CASCADE_COUNT 5u
#define GI_PYRAMID_LEVELS 4u
#define MAX_ENVIRONMENTS 4u
#define ENV_CUBE_SIZE 256u
#define ENV_CUBE_MIPS 7u
#define TEXTURE_NONE 0xFFFFFFFFu
#define ENVIRONMENT_NONE 0xFFFFFFFFu
#define COUNTER_TRANSPARENT_DRAWS (VIEW_COUNT * DRAW_KIND_COUNT)
#define COUNTER_TRANSPARENT_FOUND (COUNTER_TRANSPARENT_DRAWS + 1u)
#define COUNTER_COUNT (COUNTER_TRANSPARENT_FOUND + 1u)
#define TRANSPARENT_DRAW_BASE (VIEW_COUNT * DRAW_KIND_COUNT * MAX_MESHES)
#define TRANSPARENT_VISIBLE_BASE (VIEW_COUNT * MAX_INSTANCES)
// View depth stored in the pyramid for sky texels (fits a 16-bit float).
#define SKY_DEPTH 60000.0
#define PI 3.14159265358979
// Largest value written to 16-bit float targets; larger ones would become
// infinite.
#define HALF_MAX 60000.0

// DebugMode in engine/renderer_internal.h.
#define DEBUG_MODE_DIRECT 1u
#define DEBUG_MODE_INDIRECT 2u
#define DEBUG_MODE_LIGHTS 3u

// GpuMaterial texture order.
#define MATERIAL_TEXTURE_BASE_COLOR 0
#define MATERIAL_TEXTURE_NORMAL 1
#define MATERIAL_TEXTURE_ORM 2
#define MATERIAL_TEXTURE_EMISSIVE 3

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
  uint index_count; // 0 until the mesh data is on the GPU
  int vertex_offset;
  uint pad0;
  vec4 sphere; // xyz: center, w: radius (object space)
};

struct Material {
  vec4 base_color;
  vec3 emissive;
  float alpha_cutoff;
  float metallic;
  float roughness;
  uint pipeline; // blend * 2 + double sided
  float occlusion_strength;
  uint textures[4];
};

// Point light: position and radius. Directional light: unit vector toward
// the light, w = 0.
struct Light {
  vec4 position_radius;
  vec4 color; // rgb: color * intensity
};

// Cascade 0 probe: world position (w = 1 on a surface, 0 on the sky) and L1
// spherical harmonics of the radiance over its hemisphere. sh[0].w is the
// fraction of the hemisphere that sees the sky; sh[1..3].w hold the normal.
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

struct RenderData {
  mat4 view;
  mat4 projection;
  mat4 view_projection;
  mat4 inverse_view_projection;
  mat4 inverse_projection;
  mat4 shadow_view_projection[SHADOW_CASCADE_COUNT];
  vec4 camera_position;
  vec4 sky;
  vec4 environment; // intensity, cos(rotation), sin(rotation), -
  vec4 shadow_splits;
  vec4 shadow_texel;
  vec4 frustums[VIEW_COUNT * 6u];
  uvec2 screen_size;
  uvec2 tile_count;
  uint entry_offset;
  uint entry_count;
  uint light_offset; // directional lights, then point lights
  uint point_light_count;
  uint directional_count;
  uint shadow_light;
  uint view_count;
  uint mesh_count;
  uint cascade_count;
  float gi_interval;
  uint debug_mode;
  uint environment_slot;
  float exposure;
  uint pad0;
  uint pad1;
  uint pad2;
};

layout(push_constant, scalar) uniform PushConstants {
  uint render;
  uint view;  // view, or a texture index
  uint value; // cascade, pyramid level or mip
  uint pad;
} pc;

// Set 0: screen targets, scene buffers, shadow map and environments.
layout(set = 0, binding = 0) uniform sampler2D depth_texture;
// Octahedral normal (xy, -1..1), roughness, metallic.
layout(set = 0, binding = 1, rgba16f) uniform image2D normal_image;
// Base color, occlusion.
layout(set = 0, binding = 2, rgba8) uniform image2D albedo_image;
layout(set = 0, binding = 3, rgba16f) uniform image2D hdr_image;
layout(set = 0, binding = 4, rgba16f) uniform image2D cascade_images[GI_CASCADE_COUNT];
layout(set = 0, binding = 5, rgba16f) uniform image2D pyramid_levels[GI_PYRAMID_LEVELS];
layout(set = 0, binding = 6, scalar) readonly buffer InstanceBuffer { Instance instances[]; };
layout(set = 0, binding = 7, scalar) readonly buffer MeshBuffer { Mesh meshes[]; };
layout(set = 0, binding = 8, scalar) readonly buffer MaterialBuffer { Material materials[]; };
layout(set = 0, binding = 9, scalar) buffer VisibleBuffer { uint visible[]; };
layout(set = 0, binding = 10, scalar) buffer DrawBuffer { DrawCommand draws[]; };
// Per (view, kind, mesh) key k: [2k] = visible instances, [2k + 1] = slot
// of its draw.
layout(set = 0, binding = 11, scalar) buffer DrawKeyBuffer { uint draw_keys[]; };
layout(set = 0, binding = 12, scalar) buffer CounterBuffer { uint counters[]; };
// Sort key (x: inverted distance bits, y: scene entry) of each transparent
// instance in view.
layout(set = 0, binding = 13, scalar) buffer TransparentKeyBuffer { uvec2 transparent_keys[]; };
layout(set = 0, binding = 14, scalar) buffer TileLightBuffer { uint tile_lights[]; };
layout(set = 0, binding = 15, scalar) buffer ProbeBuffer { Probe probes[]; };
layout(set = 0, binding = 16) uniform sampler2DArrayShadow shadow_map;
layout(set = 0, binding = 17) uniform samplerCube env_radiance[MAX_ENVIRONMENTS];
layout(set = 0, binding = 18) uniform samplerCube env_irradiance[MAX_ENVIRONMENTS];

// Set 1: material textures.
layout(set = 1, binding = 0) uniform sampler2D textures[];

// Set 2: this frame's render data, scene entries and lights.
layout(set = 2, binding = 0, scalar) readonly buffer RenderBuffer { RenderData renders[]; };
layout(set = 2, binding = 1, scalar) readonly buffer EntryBuffer { uint entries[]; };
layout(set = 2, binding = 2, scalar) readonly buffer LightBuffer { Light lights[]; };

#define render_data renders[pc.render]

// Rows of matrices that turn positions into view depth: the distance along
// the view axis, positive in front of the camera whatever the handedness.
struct DepthRows {
  vec4 view;         // world position -> view z
  vec4 projection_z; // NDC -> view z (numerator)
  vec4 projection_w; // NDC -> view w (denominator)
  float sign;        // makes view z positive in front of the camera
};

DepthRows depth_rows() {
  mat4 v = render_data.view;
  mat4 ip = render_data.inverse_projection;
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
  vec2 ndc = pixel / vec2(render_data.screen_size) * 2.0 - 1.0;
  vec4 world = render_data.inverse_view_projection * vec4(ndc, depth, 1.0);
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

// Signed octahedral encoding of a unit vector into [-1, 1]^2.
vec2 encode_normal(vec3 n) {
  n /= abs(n.x) + abs(n.y) + abs(n.z);
  vec2 e = n.xy;
  if (n.z < 0.0)
    e = (1.0 - abs(n.yx)) * sign_not_zero(n.xy);
  return e;
}

vec3 decode_normal(vec2 e) {
  vec3 n = vec3(e, 1.0 - abs(e.x) - abs(e.y));
  if (n.z < 0.0)
    n.xy = (1.0 - abs(n.yx)) * sign_not_zero(n.xy);
  return normalize(n);
}

vec3 load_normal(ivec2 pixel) {
  return decode_normal(imageLoad(normal_image, pixel).xy);
}
