#ifndef CGF_ENGINE_RENDERER_INTERNAL_H
#define CGF_ENGINE_RENDERER_INTERNAL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <volk.h>

#include "engine.h"
#include "include/renderer.h"
#include "include/renderer_runtime.h"

/* Limits. Constants shared with shaders must match engine/shaders/common.glsl. */
#define FRAMES_IN_FLIGHT 2
#define MAX_RENDERS_PER_FRAME 8
#define MAX_SWAPCHAIN_IMAGES 8
#define MAX_INSTANCES (1u << 18)
#define MAX_MESHES (1u << 16)
#define MAX_MATERIALS (1u << 14)
#define MAX_TEXTURES (1u << 14)
#define MAX_LIGHTS (1u << 14)
#define MAX_SCENES 256
#define VERTEX_CAPACITY (1u << 23)
#define INDEX_CAPACITY (1u << 25)
/* Instanced draw kinds: pipeline = blend * 2 + double sided for OPAQUE and
   MASK. ALPHA materials (pipeline 4 and 5) are sorted and drawn one by one. */
#define DRAW_KIND_COUNT 4
#define PIPELINE_TRANSPARENT 4
#define SHADOW_CASCADE_COUNT 4
#define VIEW_COUNT (1 + SHADOW_CASCADE_COUNT) /* camera, then cascades */
#define TRANSPARENT_CAPACITY 2048 /* sorted transparent instances per render */
#define SHADOW_MAP_SIZE 2048
#define SHADOW_DISTANCE 120.0f
#define TILE_SIZE 16
#define MAX_LIGHTS_PER_TILE 255
#define GI_PROBE_SPACING 8
#define GI_DIRECTIONS 4
#define GI_CASCADE_COUNT 5
#define GI_INTERVAL 0.1f
#define GI_PYRAMID_LEVELS 4
#define MAX_ENVIRONMENTS 4
#define ENV_CUBE_SIZE 256
#define ENV_CUBE_MIPS 7 /* 256 .. 4; mip m is prefiltered for roughness m / 6 */
#define ENV_IRRADIANCE_SIZE 32
#define TEXTURE_NONE 0xFFFFFFFFu
#define ENVIRONMENT_NONE 0xFFFFFFFFu
/* Bytes of mesh and texture data copied to the GPU per frame. A single item
   larger than this still goes in one frame. */
#define UPLOAD_BUDGET (48u << 20)
#define PROFILE_QUERY_COUNT 10

/* cull_counters layout. */
#define COUNTER_TRANSPARENT_DRAWS (VIEW_COUNT * DRAW_KIND_COUNT)
#define COUNTER_TRANSPARENT_FOUND (COUNTER_TRANSPARENT_DRAWS + 1)
#define COUNTER_COUNT (COUNTER_TRANSPARENT_FOUND + 1)
/* First indirect draw and visible slot of the sorted transparent list. */
#define TRANSPARENT_DRAW_BASE (VIEW_COUNT * DRAW_KIND_COUNT * MAX_MESHES)
#define TRANSPARENT_VISIBLE_BASE (VIEW_COUNT * MAX_INSTANCES)

/* Diagnostic views, selected with the CGF_RENDERER_DEBUG environment
   variable ("direct", "indirect" or "lights"). */
typedef enum DebugMode {
  DEBUG_MODE_NONE,
  DEBUG_MODE_DIRECT,   /* no global illumination */
  DEBUG_MODE_INDIRECT, /* global illumination only */
  DEBUG_MODE_LIGHTS,   /* lights per Forward+ tile */
} DebugMode;

/* Descriptor set 0 bindings, matching engine/shaders/common.glsl. */
enum {
  BINDING_DEPTH,
  BINDING_NORMAL,
  BINDING_ALBEDO,
  BINDING_HDR,
  BINDING_CASCADES,
  BINDING_PYRAMID,
  BINDING_INSTANCES,
  BINDING_MESHES,
  BINDING_MATERIALS,
  BINDING_VISIBLE,
  BINDING_DRAWS,
  BINDING_DRAW_KEYS,
  BINDING_COUNTERS,
  BINDING_TRANSPARENT_KEYS,
  BINDING_TILE_LIGHTS,
  BINDING_PROBES,
  BINDING_SHADOW_MAP,
  BINDING_ENV_RADIANCE,
  BINDING_ENV_IRRADIANCE,
  BINDING_COUNT,
};

/* GPU records, scalar layout. */
typedef struct GpuInstance {
  float transform[16];
  float normal_transform[16];
  uint32_t mesh;
  uint32_t material;
  uint32_t pad0;
  uint32_t pad1;
} GpuInstance;

/* index_count is 0 until the mesh data has reached the GPU. */
typedef struct GpuMesh {
  uint32_t first_index;
  uint32_t index_count;
  int32_t vertex_offset;
  uint32_t pad0;
  float sphere[4];
} GpuMesh;

enum {
  MATERIAL_TEXTURE_BASE_COLOR,
  MATERIAL_TEXTURE_NORMAL,
  MATERIAL_TEXTURE_ORM,
  MATERIAL_TEXTURE_EMISSIVE,
  MATERIAL_TEXTURE_COUNT,
};

typedef struct GpuMaterial {
  float base_color[4];
  float emissive[3];
  float alpha_cutoff;
  float metallic;
  float roughness;
  uint32_t pipeline; /* blend * 2 + double sided */
  float occlusion_strength;
  uint32_t textures[MATERIAL_TEXTURE_COUNT]; /* TEXTURE_NONE when absent */
} GpuMaterial;

/* Point light: position and radius. Directional light: unit vector toward
   the light, w = 0. color is color * intensity. */
typedef struct GpuLight {
  float position_radius[4];
  float color[4];
} GpuLight;

typedef struct GpuRenderData {
  float view[16];
  float projection[16];
  float view_projection[16];
  float inverse_view_projection[16];
  float inverse_projection[16];
  float shadow_view_projection[SHADOW_CASCADE_COUNT][16];
  float camera_position[4];
  float sky[4];
  float environment[4];   /* intensity, cos(rotation), sin(rotation), - */
  float shadow_splits[4]; /* far view depth of each cascade */
  float shadow_texel[4];  /* world size of a shadow map texel per cascade */
  float frustums[VIEW_COUNT][6][4];
  uint32_t screen_size[2];
  uint32_t tile_count[2];
  uint32_t entry_offset;
  uint32_t entry_count;
  uint32_t light_offset; /* directional lights, then point lights */
  uint32_t point_light_count;
  uint32_t directional_count;
  uint32_t shadow_light; /* index among the directional lights, or none */
  uint32_t view_count;
  uint32_t mesh_count;
  uint32_t cascade_count;
  float gi_interval;
  uint32_t debug_mode; /* DebugMode */
  uint32_t environment_slot;
  float exposure; /* linear multiplier */
  uint32_t pad[3];
} GpuRenderData;

/* Cascade 0 probe: world position (w = 1 on a surface, 0 on the sky) and
   L1 spherical harmonics of the radiance over its hemisphere (rgb). sh[0].w
   is the fraction of the hemisphere that sees the sky; sh[1..3].w hold the
   surface normal. */
typedef struct GpuProbe {
  float position[4];
  float sh[4][4];
} GpuProbe;

/* Meaning of the fields depends on the pass: render data index, view or
   texture index, and a cascade, level or mip. */
typedef struct GpuPushConstants {
  uint32_t render;
  uint32_t view;
  uint32_t value;
  uint32_t pad;
} GpuPushConstants;

#define GPU_DRAW_COMMAND_SIZE 20u

_Static_assert(sizeof(GpuInstance) == 144, "GpuInstance layout");
_Static_assert(sizeof(GpuMesh) == 32, "GpuMesh layout");
_Static_assert(sizeof(GpuMaterial) == 64, "GpuMaterial layout");
_Static_assert(sizeof(GpuLight) == 32, "GpuLight layout");
_Static_assert(offsetof(GpuRenderData, screen_size) == 1136,
               "GpuRenderData layout");
_Static_assert(sizeof(GpuRenderData) == 1216, "GpuRenderData layout");
_Static_assert(sizeof(GpuProbe) == 80, "GpuProbe layout");
_Static_assert(sizeof(CGF_Vertex) == 48, "CGF_Vertex layout");
_Static_assert(sizeof(VkDrawIndexedIndirectCommand) == GPU_DRAW_COMMAND_SIZE,
               "draw command layout");

typedef struct Buffer {
  VkBuffer buffer;
  VkDeviceMemory memory;
  VkDeviceSize size;
  void *mapped; /* host-visible buffers only */
} Buffer;

typedef struct Image {
  VkImage image;
  VkDeviceMemory memory;
  VkImageView view;
  uint32_t width;
  uint32_t height;
} Image;

/* Handle slots: id is the slot index, gen the slot's generation (0 when the
   slot is free; never 0 for a live handle). */
typedef struct SlotPool {
  uint32_t *generations;
  uint32_t *free_slots;
  uint32_t free_count;
  uint32_t used; /* slots ever handed out */
  uint32_t capacity;
  uint32_t next_generation;
} SlotPool;

typedef struct Range {
  uint32_t first;
  uint32_t count;
} Range;

/* First-fit allocator over [0, capacity) for the shared vertex and index
   buffers. */
typedef struct RangeAllocator {
  Range *free_ranges;
  uint32_t free_count;
  uint32_t free_capacity;
} RangeAllocator;

typedef struct MeshRecord {
  Range vertices;
  Range indices;
  GpuMesh gpu; /* published to gpu_meshes once uploaded */
} MeshRecord;

/* Mesh data waiting to be copied to the GPU. */
typedef struct MeshUpload {
  uint32_t slot;
  uint32_t first_vertex;
  uint32_t vertex_count;
  uint32_t first_index;
  uint32_t index_count;
  CGF_Vertex *vertices;
  uint32_t *indices;
} MeshUpload;

typedef struct TextureRecord {
  VkImage image;
  VkDeviceMemory memory;
  VkImageView view;
  VkFormat format;
  CGF_TextureFormat source_format;
  uint32_t width;
  uint32_t height;
  uint32_t mip_count;
  bool resident; /* uploaded and written to the texture descriptor array */
} TextureRecord;

/* Texture data waiting to be copied to the GPU, mip by mip. */
typedef struct TextureUpload {
  uint32_t slot;
  uint32_t next_mip;
  uint32_t planned_mip_end; /* set by frame_begin's upload plan */
  uint8_t *data;
  size_t size;
} TextureUpload;

/* A destroyed texture, released once no submitted frame can use it. */
typedef struct RetiredTexture {
  TextureRecord record;
  uint32_t slot;
  uint64_t frame; /* frames up to this one may still use it */
} RetiredTexture;

typedef enum EnvironmentState {
  ENVIRONMENT_EMPTY,
  ENVIRONMENT_WAITING, /* texture not uploaded yet */
  ENVIRONMENT_READY,
} EnvironmentState;

/* Prefiltered radiance and irradiance cube maps made from an environment
   texture. */
typedef struct EnvironmentSlot {
  EnvironmentState state;
  CGF_Texture texture;
  uint64_t last_used; /* frame number */
  bool process;       /* set by the snapshot when the texture is ready */
  VkImage radiance;
  VkDeviceMemory radiance_memory;
  VkImageView radiance_cube;
  VkImageView radiance_mips[ENV_CUBE_MIPS]; /* storage, 2D array */
  VkImage irradiance;
  VkDeviceMemory irradiance_memory;
  VkImageView irradiance_cube;
  VkImageView irradiance_target; /* storage, 2D array */
} EnvironmentSlot;

typedef struct SceneRecord {
  CGF_Instance *instances;
  uint32_t instance_count;
  uint32_t instance_capacity;
  CGF_Light *lights;
  uint32_t light_count;
  uint32_t light_capacity;
  bool has_environment;
  CGF_EnvironmentData environment;

  /* Snapshot taken by frame_begin; read by render. */
  uint32_t frame_generation;
  uint32_t entry_offset;
  uint32_t entry_count;
  uint32_t light_offset;
  uint32_t frame_point_count;
  uint32_t frame_directional_count;
  uint32_t frame_shadow_light;
  float frame_shadow_direction[3];
  uint32_t frame_environment; /* slot, or ENVIRONMENT_NONE */
  float frame_environment_intensity;
  float frame_environment_rotation;
} SceneRecord;

typedef struct Frame {
  VkCommandPool command_pool;
  VkCommandBuffer command_buffer;
  VkFence fence;
  VkSemaphore image_available;
  /* Host-visible: render data, scene entries, lights and staging data. */
  Buffer upload;
  VkDescriptorSet descriptor_set; /* set 2, over upload */
} Frame;

typedef struct Pipelines {
  VkPipeline cull_reset;
  VkPipeline cull_count;
  VkPipeline cull_prefix;
  VkPipeline cull_write;
  VkPipeline transparent_sort;
  VkPipeline light_cull;
  VkPipeline gi_pyramid;
  VkPipeline gi_cascade;
  VkPipeline gi_probe;
  VkPipeline gi_probe_filter;
  VkPipeline gi_apply;
  VkPipeline env_prefilter;
  VkPipeline env_irradiance;
  VkPipeline shadow[2]; /* indexed by alpha test */
  VkPipeline prepass[DRAW_KIND_COUNT];
  VkPipeline forward[DRAW_KIND_COUNT];
  VkPipeline transparent;
  VkPipeline tonemap;
} Pipelines;

/* Persistent renderer state, kept in the registry across plugin reloads. It
   holds no pointers into the plugin DLL. */
typedef struct RendererState {
  bool initialized;
  DebugMode debug_mode;
  SRWLOCK lock;
  const CGF_EngineAPI *engine;

  VkInstance instance;
  VkDebugUtilsMessengerEXT messenger;
  VkSurfaceKHR surface;
  VkPhysicalDevice physical_device;
  VkDevice device;
  uint32_t queue_family;
  VkQueue queue;
  VkPhysicalDeviceMemoryProperties memory_properties;

  VkSwapchainKHR swapchain;
  VkFormat swapchain_format;
  bool swapchain_srgb;
  VkExtent2D extent;
  uint32_t image_count;
  VkImage images[MAX_SWAPCHAIN_IMAGES];
  VkImageView image_views[MAX_SWAPCHAIN_IMAGES];
  VkSemaphore render_finished[MAX_SWAPCHAIN_IMAGES];
  bool swapchain_dirty;

  Image depth;
  Image normal; /* octahedral normal (xy), roughness, metallic */
  Image albedo; /* base color, occlusion */
  Image hdr;
  Image cascades[GI_CASCADE_COUNT];
  /* Direct radiance (rgb) and view depth (a) of the scene, with
     GI_PYRAMID_LEVELS mips that halve the resolution each level. */
  VkImage pyramid;
  VkDeviceMemory pyramid_memory;
  VkImageView pyramid_views[GI_PYRAMID_LEVELS];
  uint32_t pyramid_width[GI_PYRAMID_LEVELS];
  uint32_t pyramid_height[GI_PYRAMID_LEVELS];
  Buffer probes; /* GpuProbe per cascade 0 probe: raw, then filtered */
  uint32_t probe_count;
  Buffer tile_lights;
  bool targets_need_init;

  /* Directional shadow cascades, one array layer each. */
  VkImage shadow_map;
  VkDeviceMemory shadow_memory;
  VkImageView shadow_array;
  VkImageView shadow_layers[SHADOW_CASCADE_COUNT];
  EnvironmentSlot environments[MAX_ENVIRONMENTS];
  bool static_images_need_init;

  Buffer vertices;
  Buffer indices;
  Buffer instance_buffer;
  Buffer mesh_buffer;
  Buffer material_buffer;
  Buffer visible;
  Buffer draws;
  Buffer draw_keys; /* per (view, kind, mesh): instance count, draw slot */
  Buffer cull_counters;
  Buffer transparent_keys;

  VkSampler sampler;         /* nearest, clamp */
  VkSampler texture_sampler; /* anisotropic, repeat */
  VkSampler shadow_sampler;  /* depth comparison */
  VkSampler env_sampler;     /* trilinear, clamp */
  VkDescriptorSetLayout set_layout;         /* set 0: targets and buffers */
  VkDescriptorSetLayout texture_set_layout; /* set 1: material textures */
  VkDescriptorSetLayout frame_set_layout;   /* set 2: per-frame data */
  VkDescriptorPool descriptor_pool;
  VkDescriptorPool texture_descriptor_pool;
  VkDescriptorSet descriptor_set;
  VkDescriptorSet texture_set;
  VkPipelineLayout pipeline_layout;
  /* Environment preprocessing: one storage image per set, then textures. */
  VkDescriptorSetLayout env_set_layout;
  VkPipelineLayout env_pipeline_layout;
  VkDescriptorSet env_sets[MAX_ENVIRONMENTS][ENV_CUBE_MIPS + 1];
  Pipelines pipelines;

  /* GPU pass timings, enabled with CGF_RENDERER_PROFILE=1. Each frame slot
     owns PROFILE_QUERY_COUNT timestamps for its first render. */
  bool profile;
  VkQueryPool query_pool;
  float timestamp_period;
  bool queries_written[FRAMES_IN_FLIGHT];
  double profile_sums[PROFILE_QUERY_COUNT];
  uint32_t profile_frames;

  Frame frames[FRAMES_IN_FLIGHT];
  uint64_t frame_number;
  uint32_t frame_index;
  bool in_frame;
  uint32_t image_index;
  uint32_t render_count;
  float sky[4];
  /* Layout of the current frame's upload buffer, set by frame_begin. */
  VkDeviceSize entries_offset;
  VkDeviceSize lights_offset;

  /* CPU copies of everything the GPU draws. Guarded by lock. */
  SlotPool instance_pool;
  CGF_InstanceData *instances;
  uint8_t *instance_dirty;
  uint32_t *instance_dirty_list;
  uint32_t instance_dirty_count;

  SlotPool mesh_pool;
  MeshRecord *mesh_records;
  GpuMesh *gpu_meshes;
  bool meshes_dirty;
  MeshUpload *mesh_uploads;
  uint32_t mesh_upload_count;
  uint32_t mesh_upload_capacity;
  RangeAllocator vertex_allocator;
  RangeAllocator index_allocator;

  SlotPool texture_pool;
  TextureRecord *textures;
  TextureUpload *texture_uploads;
  uint32_t texture_upload_count;
  uint32_t texture_upload_capacity;
  RetiredTexture *retired_textures;
  uint32_t retired_count;
  uint32_t retired_capacity;

  SlotPool material_pool;
  CGF_MaterialData *materials;
  GpuMaterial *gpu_materials;
  bool materials_dirty;

  SlotPool light_pool;
  CGF_LightData *lights;

  SlotPool scene_pool;
  SceneRecord *scenes;

  /* Mesh count of the current frame's snapshot. */
  uint32_t frame_mesh_count;
} RendererState;

/* The state of the loaded plugin; set by cgf_renderer_load. */
extern RendererState *renderer;

/* renderer_vulkan.c */
bool vk_initialize(RendererState *s);
void vk_shutdown(RendererState *s);
bool vk_create_pipelines(RendererState *s, Pipelines *out);
void vk_destroy_pipelines(RendererState *s, Pipelines *pipelines);
bool vk_create_messenger(RendererState *s, VkDebugUtilsMessengerEXT *out);
/* Recreates the swapchain and screen-sized targets for the window size. */
bool vk_resize(RendererState *s, uint32_t width, uint32_t height);
bool vk_create_buffer(RendererState *s, VkDeviceSize size,
                      VkBufferUsageFlags usage, bool host_visible, Buffer *out);
void vk_destroy_buffer(RendererState *s, Buffer *buffer);
/* Points the frame's descriptor set at its upload buffer. */
void vk_write_frame_set(RendererState *s, Frame *frame,
                        VkDeviceSize entries_offset,
                        VkDeviceSize lights_offset);
/* Creates the image of a texture record whose format and size are set. Safe
   to call without the renderer lock. */
bool vk_create_texture(RendererState *s, TextureRecord *texture);
void vk_destroy_texture(RendererState *s, TextureRecord *texture);
void vk_write_texture_descriptor(RendererState *s, uint32_t slot,
                                 const TextureRecord *texture);

/* renderer_frame.c */
CGF_RenderResult renderer_frame_begin(const float clear_color[4]);
CGF_RenderResult renderer_render(CGF_Scene scene, const CGF_Camera *camera);
CGF_RenderResult renderer_frame_end(void);

/* renderer.c */
bool slot_valid(const SlotPool *pool, CGF_ID id);
bool renderer_instance_drawable(const RendererState *s, uint32_t slot);
/* Byte size and Vulkan format of texture data. */
VkFormat texture_vk_format(CGF_TextureFormat format);
size_t texture_mip_size(CGF_TextureFormat format, uint32_t width,
                        uint32_t height, uint32_t mip);
/* Returns retired texture slots to the pool once frames up to
   completed_frame have finished. Caller holds the lock. */
void renderer_release_retired(RendererState *s, uint64_t completed_frame);
/* Fills the GPU materials from the CPU copies, resolving textures that are
   resident. Caller holds the lock. */
void renderer_build_materials(RendererState *s);

#endif
