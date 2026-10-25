#include "include/loader.h"
#include "include/loader_runtime.h"
#include "loader_internal.h"

#include "engine.h"
#include "ufbx.h"

#include <ctype.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#define MAX_ASSETS 64
#define PATH_SIZE 1024
#define MESH_BATCH 64

typedef enum AssetType {
  ASSET_MODEL,
  ASSET_IMAGE,
} AssetType;

/* A texture file shared by the assets that use it. */
typedef struct CachedTexture {
  char path[PATH_SIZE];
  bool srgb;
  bool has_alpha;
  CGF_Texture texture;
  uint32_t references;
} CachedTexture;

/* Written by the asset's thread until it publishes the status; read by
   others only after the status is READY or FAILED. */
typedef struct Asset {
  uint32_t generation; /* 0 when the slot is free */
  AssetType type;
  volatile LONG status; /* CGF_AssetStatus */
  volatile LONG progress; /* thousandths */
  volatile LONG cancel;
  HANDLE thread;
  char path[PATH_SIZE];
  CGF_ModelLoadDesc desc;

  uint32_t *textures; /* indices into the texture cache */
  uint32_t texture_count;
  uint32_t texture_capacity;
  CGF_Material *materials;
  uint32_t material_count;
  CGF_Mesh *meshes;
  uint32_t mesh_count;
  CGF_Instance *instances;
  uint32_t instance_count;
  CGF_Light *lights;
  uint32_t light_count;
  CGF_ModelInfo info;
  CGF_Texture image;
} Asset;

static const CGF_APIRegistry *registry;
static const CGF_EngineAPI *engine;
static Asset assets[MAX_ASSETS];
static uint32_t next_generation = 1;
static SRWLOCK assets_lock = SRWLOCK_INIT;
/* Held shared while a loading thread calls the renderer; held exclusively
   while the renderer reloads. */
static SRWLOCK renderer_lock = SRWLOCK_INIT;
static CachedTexture *texture_cache;
static uint32_t texture_cache_count;
static uint32_t texture_cache_capacity;
static SRWLOCK cache_lock = SRWLOCK_INIT;

static void log_message(const char *format, ...) {
  char message[1024];
  va_list args;
  va_start(args, format);
  vsnprintf(message, sizeof(message), format, args);
  va_end(args);
  if (engine)
    engine->log(message);
}

static const CGF_EngineRendererAPI *renderer_api(void) {
  return registry->get(CGF_ENGINE_RENDERER_API_NAME,
                       sizeof(CGF_EngineRendererAPI));
}

/* Calls into the renderer happen between these. */
static const CGF_EngineRendererAPI *begin_renderer(void) {
  AcquireSRWLockShared(&renderer_lock);
  const CGF_EngineRendererAPI *r = renderer_api();
  if (!r)
    ReleaseSRWLockShared(&renderer_lock);
  return r;
}

static void end_renderer(void) { ReleaseSRWLockShared(&renderer_lock); }

static bool grow(void **array, uint32_t *capacity, uint32_t needed,
                 size_t element_size) {
  if (needed <= *capacity)
    return true;
  uint32_t grown = *capacity ? *capacity : 16;
  while (grown < needed)
    grown *= 2;
  void *data = realloc(*array, element_size * grown);
  if (!data)
    return false;
  *array = data;
  *capacity = grown;
  return true;
}

static void set_progress(Asset *asset, float progress) {
  InterlockedExchange(&asset->progress, (LONG)(progress * 1000.0f));
}

static bool cancelled(Asset *asset) {
  return InterlockedCompareExchange(&asset->cancel, 0, 0) != 0;
}

///////////////////////////////////////////////////////////////////////////////
// Texture cache
///////////////////////////////////////////////////////////////////////////////

static bool has_extension(const char *path, const char *extension) {
  size_t length = strlen(path);
  size_t extension_length = strlen(extension);
  return length >= extension_length &&
         _stricmp(path + length - extension_length, extension) == 0;
}

/* Returns the cache index of the texture for a file, loading it on first
   use, or UINT32_MAX. Takes a reference. */
static uint32_t acquire_texture(const char *path, bool srgb) {
  AcquireSRWLockExclusive(&cache_lock);
  for (uint32_t i = 0; i < texture_cache_count; i++) {
    CachedTexture *entry = &texture_cache[i];
    if (entry->references > 0 && entry->srgb == srgb &&
        _stricmp(entry->path, path) == 0) {
      entry->references++;
      ReleaseSRWLockExclusive(&cache_lock);
      return i;
    }
  }
  ReleaseSRWLockExclusive(&cache_lock);

  LoadedImage image;
  char error[512];
  bool loaded = has_extension(path, ".hdr")
                    ? image_load_hdr(path, &image, error, sizeof(error))
                    : image_load_dds(path, srgb, &image, error, sizeof(error));
  if (!loaded) {
    log_message("[LOADER] %s", error);
    return UINT32_MAX;
  }
  CGF_TextureData data = {
      .format = image.format,
      .width = image.width,
      .height = image.height,
      .mip_count = image.mip_count,
      .data = image.data,
      .size = image.size,
  };
  CGF_Texture texture = {0};
  const CGF_EngineRendererAPI *r = begin_renderer();
  CGF_RenderResult result = r ? r->texture_create(&data, 1, &texture)
                              : CGF_RENDER_ERROR_INVALID_STATE;
  if (r)
    end_renderer();
  bool has_alpha = image.has_alpha;
  image_free(&image);
  if (result != CGF_RENDER_OK) {
    log_message("[LOADER] cannot create the texture of %s (%d)", path,
                (int)result);
    return UINT32_MAX;
  }

  AcquireSRWLockExclusive(&cache_lock);
  uint32_t index = UINT32_MAX;
  /* Another thread may have loaded the same file meanwhile. */
  for (uint32_t i = 0; i < texture_cache_count && index == UINT32_MAX; i++) {
    CachedTexture *entry = &texture_cache[i];
    if (entry->references > 0 && entry->srgb == srgb &&
        _stricmp(entry->path, path) == 0) {
      entry->references++;
      index = i;
    }
  }
  bool duplicate = index != UINT32_MAX;
  for (uint32_t i = 0; i < texture_cache_count && index == UINT32_MAX; i++) {
    if (texture_cache[i].references == 0)
      index = i;
  }
  if (index == UINT32_MAX &&
      grow((void **)&texture_cache, &texture_cache_capacity,
           texture_cache_count + 1, sizeof(CachedTexture)))
    index = texture_cache_count++;
  if (index != UINT32_MAX && !duplicate) {
    CachedTexture *entry = &texture_cache[index];
    *entry = (CachedTexture){
        .srgb = srgb,
        .has_alpha = has_alpha,
        .texture = texture,
        .references = 1,
    };
    snprintf(entry->path, sizeof(entry->path), "%s", path);
  }
  ReleaseSRWLockExclusive(&cache_lock);
  if (duplicate || index == UINT32_MAX) {
    r = begin_renderer();
    if (r) {
      r->texture_destroy(texture);
      end_renderer();
    }
  }
  return index;
}

static void release_texture(uint32_t index) {
  AcquireSRWLockExclusive(&cache_lock);
  CachedTexture *entry = &texture_cache[index];
  CGF_Texture texture = {0};
  if (entry->references > 0 && --entry->references == 0)
    texture = entry->texture;
  ReleaseSRWLockExclusive(&cache_lock);
  if (texture.gen) {
    const CGF_EngineRendererAPI *r = begin_renderer();
    if (r) {
      r->texture_destroy(texture);
      end_renderer();
    }
  }
}

static CachedTexture cached_texture(uint32_t index) {
  AcquireSRWLockShared(&cache_lock);
  CachedTexture entry = texture_cache[index];
  ReleaseSRWLockShared(&cache_lock);
  return entry;
}

///////////////////////////////////////////////////////////////////////////////
// Model: materials
///////////////////////////////////////////////////////////////////////////////

/* Texture lookups not yet attempted. */
#define UNTRIED (UINT32_MAX - 1)

typedef struct ModelLoad {
  Asset *asset;
  ufbx_scene *scene;
  char directory[PATH_SIZE];
  /* Per ufbx texture: cache index, UINT32_MAX when missing, or UNTRIED; for
     sRGB and linear use. */
  uint32_t *srgb_textures;
  uint32_t *linear_textures;
  /* Per ufbx mesh, two meshes per part: as stored, then mirrored. */
  CGF_Mesh *part_meshes;
  uint32_t *part_offsets;
  float *part_bounds; /* local min and max of each part */
  float bounds_min[3];
  float bounds_max[3];
} ModelLoad;

static bool contains_ignore_case(const char *text, const char *word) {
  size_t length = strlen(word);
  for (; *text; text++) {
    if (_strnicmp(text, word, length) == 0)
      return true;
  }
  return false;
}

static void texture_path(const ModelLoad *load, const ufbx_texture *texture,
                         char *out, size_t size) {
  const ufbx_string *name = texture->relative_filename.length
                                ? &texture->relative_filename
                                : &texture->filename;
  if (name->length >= 2 && (name->data[1] == ':' || name->data[0] == '\\' ||
                            name->data[0] == '/'))
    snprintf(out, size, "%s", name->data);
  else
    snprintf(out, size, "%s%s", load->directory, name->data);
}

/* Cache index of a material property's file texture, or UINT32_MAX. */
static uint32_t material_texture(ModelLoad *load, const ufbx_material *material,
                                 const char *property, bool srgb) {
  for (size_t i = 0; i < material->textures.count; i++) {
    const ufbx_material_texture *entry = &material->textures.data[i];
    if (strcmp(entry->material_prop.data, property) != 0)
      continue;
    const ufbx_texture *texture = entry->texture;
    if (texture->type != UFBX_TEXTURE_FILE)
      continue;
    uint32_t *slot = srgb ? &load->srgb_textures[texture->typed_id]
                          : &load->linear_textures[texture->typed_id];
    if (*slot == UNTRIED) {
      char path[PATH_SIZE];
      texture_path(load, texture, path, sizeof(path));
      *slot = acquire_texture(path, srgb);
      Asset *asset = load->asset;
      if (*slot != UINT32_MAX) {
        if (grow((void **)&asset->textures, &asset->texture_capacity,
                 asset->texture_count + 1, sizeof(uint32_t))) {
          asset->textures[asset->texture_count++] = *slot;
        } else {
          release_texture(*slot);
          *slot = UINT32_MAX;
        }
      }
    }
    return *slot;
  }
  return UINT32_MAX;
}

static CGF_Texture cache_handle(uint32_t index) {
  return index == UINT32_MAX ? (CGF_Texture){0}
                             : cached_texture(index).texture;
}

/* Bistro-style FBX materials: Phong materials whose textures follow the
   metal/roughness convention (base color with opacity in alpha; occlusion,
   roughness and metallic in the specular map; DirectX normal maps). */
static CGF_MaterialData convert_material(ModelLoad *load,
                                         const ufbx_material *material) {
  float emissive_scale =
      load->asset->desc.emissive_scale ? load->asset->desc.emissive_scale : 1.0f;
  uint32_t base = material_texture(load, material, "DiffuseColor", true);
  uint32_t normal = material_texture(load, material, "NormalMap", false);
  if (normal == UINT32_MAX)
    normal = material_texture(load, material, "Bump", false);
  uint32_t orm = material_texture(load, material, "SpecularColor", false);
  uint32_t emissive = material_texture(load, material, "EmissiveColor", true);

  CGF_MaterialData data = {
      .blend = CGF_MATERIAL_BLEND_OPAQUE,
      .alpha_cutoff = 0.5f,
      .base_color = {1.0f, 1.0f, 1.0f, 1.0f},
      .metallic = orm != UINT32_MAX ? 1.0f : 0.0f,
      .roughness = orm != UINT32_MAX ? 1.0f : 0.6f,
      /* The occlusion channel of these specular maps is left empty. */
      .occlusion_strength = 0.0f,
      .base_color_texture = cache_handle(base),
      .normal_texture = cache_handle(normal),
      .occlusion_roughness_metallic_texture = cache_handle(orm),
      .emissive_texture = cache_handle(emissive),
  };
  if (base == UINT32_MAX && material->fbx.diffuse_color.has_value) {
    float factor = material->fbx.diffuse_factor.has_value
                       ? (float)material->fbx.diffuse_factor.value_real
                       : 1.0f;
    for (int k = 0; k < 3; k++)
      data.base_color[k] =
          (float)material->fbx.diffuse_color.value_vec3.v[k] * factor;
  }
  float emission = material->fbx.emission_factor.has_value
                       ? (float)material->fbx.emission_factor.value_real
                       : 1.0f;
  for (int k = 0; k < 3; k++) {
    float color = emissive != UINT32_MAX
                      ? 1.0f
                      : (float)material->fbx.emission_color.value_vec3.v[k];
    data.emissive[k] = fmaxf(color * emission * emissive_scale, 0.0f);
  }

  const char *name = material->name.data;
  /* Transmission is the factor times the color (black when opaque). */
  const ufbx_material_map *factor = &material->pbr.transmission_factor;
  const ufbx_material_map *color = &material->pbr.transmission_color;
  float transmission =
      (factor->has_value ? (float)factor->value_real : 1.0f) *
      (color->has_value ? (float)(color->value_vec3.x + color->value_vec3.y +
                                  color->value_vec3.z) /
                              3.0f
                        : 0.0f);
  bool alpha_texture = base != UINT32_MAX && cached_texture(base).has_alpha;
  if (transmission > 0.0f ||
      (alpha_texture && contains_ignore_case(name, "glass"))) {
    data.blend = CGF_MATERIAL_BLEND_ALPHA;
    data.base_color[3] = transmission > 0.0f ? 1.0f - transmission : 1.0f;
  } else if (alpha_texture) {
    data.blend = CGF_MATERIAL_BLEND_MASK;
  }
  data.double_sided = contains_ignore_case(name, ".DoubleSided") ||
                      data.blend != CGF_MATERIAL_BLEND_OPAQUE;
  return data;
}

///////////////////////////////////////////////////////////////////////////////
// Model: meshes
///////////////////////////////////////////////////////////////////////////////

static void normalize3(float v[3]) {
  float length = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
  if (length > 1e-20f) {
    for (int i = 0; i < 3; i++)
      v[i] /= length;
  }
}

/* Per-vertex tangents from the triangles' UV derivatives, orthogonalized
   against the normal, with the handedness of the v direction in w. */
static void compute_tangents(CGF_Vertex *vertices, uint32_t vertex_count,
                             const uint32_t *indices, uint32_t index_count,
                             float *scratch) {
  float *tangents = scratch;               /* 3 per vertex */
  float *bitangents = scratch + vertex_count * 3; /* 3 per vertex */
  memset(scratch, 0, sizeof(float) * vertex_count * 6);
  for (uint32_t t = 0; t + 2 < index_count; t += 3) {
    const CGF_Vertex *v0 = &vertices[indices[t]];
    const CGF_Vertex *v1 = &vertices[indices[t + 1]];
    const CGF_Vertex *v2 = &vertices[indices[t + 2]];
    float e1[3], e2[3];
    for (int k = 0; k < 3; k++) {
      e1[k] = v1->position[k] - v0->position[k];
      e2[k] = v2->position[k] - v0->position[k];
    }
    float du1 = v1->uv[0] - v0->uv[0], dv1 = v1->uv[1] - v0->uv[1];
    float du2 = v2->uv[0] - v0->uv[0], dv2 = v2->uv[1] - v0->uv[1];
    float det = du1 * dv2 - du2 * dv1;
    if (fabsf(det) < 1e-20f)
      continue;
    float r = 1.0f / det;
    for (int corner = 0; corner < 3; corner++) {
      uint32_t i = indices[t + corner];
      for (int k = 0; k < 3; k++) {
        tangents[i * 3 + k] += (e1[k] * dv2 - e2[k] * dv1) * r;
        bitangents[i * 3 + k] += (e2[k] * du1 - e1[k] * du2) * r;
      }
    }
  }
  for (uint32_t i = 0; i < vertex_count; i++) {
    CGF_Vertex *v = &vertices[i];
    const float *n = v->normal;
    float *t = &tangents[i * 3];
    float d = n[0] * t[0] + n[1] * t[1] + n[2] * t[2];
    float o[3] = {t[0] - n[0] * d, t[1] - n[1] * d, t[2] - n[2] * d};
    if (o[0] * o[0] + o[1] * o[1] + o[2] * o[2] < 1e-20f) {
      /* No UV gradient: any direction across the normal. */
      float axis[3] = {fabsf(n[0]) < 0.9f ? 1.0f : 0.0f,
                       fabsf(n[0]) < 0.9f ? 0.0f : 1.0f, 0.0f};
      d = n[0] * axis[0] + n[1] * axis[1];
      for (int k = 0; k < 3; k++)
        o[k] = axis[k] - n[k] * d;
    }
    normalize3(o);
    const float *b = &bitangents[i * 3];
    float c[3] = {n[1] * o[2] - n[2] * o[1], n[2] * o[0] - n[0] * o[2],
                  n[0] * o[1] - n[1] * o[0]};
    float handedness = c[0] * b[0] + c[1] * b[1] + c[2] * b[2] < 0.0f ? -1.0f
                                                                       : 1.0f;
    v->tangent[0] = o[0];
    v->tangent[1] = o[1];
    v->tangent[2] = o[2];
    v->tangent[3] = handedness;
  }
}

typedef struct PartGeometry {
  CGF_Vertex *vertices;
  uint32_t vertex_count;
  uint32_t *indices;
  uint32_t index_count;
} PartGeometry;

static void free_geometry(PartGeometry *geometry) {
  free(geometry->vertices);
  free(geometry->indices);
  *geometry = (PartGeometry){0};
}

/* Triangulated, deduplicated vertices of one material part, with v flipped
   so that v grows down the image. */
static bool build_part(const ufbx_mesh *mesh, const ufbx_mesh_part *part,
                       PartGeometry *out) {
  *out = (PartGeometry){0};
  size_t corner_count = part->num_triangles * 3;
  if (corner_count == 0 || corner_count > UINT32_MAX)
    return false;
  uint32_t *triangle = malloc(sizeof(uint32_t) * mesh->max_face_triangles * 3);
  out->vertices = malloc(sizeof(CGF_Vertex) * corner_count);
  out->indices = malloc(sizeof(uint32_t) * corner_count);
  if (!triangle || !out->vertices || !out->indices) {
    free(triangle);
    free_geometry(out);
    return false;
  }
  size_t count = 0;
  for (size_t f = 0; f < part->face_indices.count; f++) {
    ufbx_face face = mesh->faces.data[part->face_indices.data[f]];
    uint32_t triangles = ufbx_triangulate_face(
        triangle, mesh->max_face_triangles * 3, mesh, face);
    for (uint32_t k = 0; k < triangles * 3 && count < corner_count; k++) {
      uint32_t index = triangle[k];
      ufbx_vec3 p = ufbx_get_vertex_vec3(&mesh->vertex_position, index);
      ufbx_vec3 n = mesh->vertex_normal.exists
                        ? ufbx_get_vertex_vec3(&mesh->vertex_normal, index)
                        : (ufbx_vec3){0.0, 1.0, 0.0};
      ufbx_vec2 uv = mesh->vertex_uv.exists
                         ? ufbx_get_vertex_vec2(&mesh->vertex_uv, index)
                         : (ufbx_vec2){0.0, 0.0};
      CGF_Vertex *v = &out->vertices[count++];
      *v = (CGF_Vertex){
          .position = {(float)p.x, (float)p.y, (float)p.z},
          .normal = {(float)n.x, (float)n.y, (float)n.z},
          .uv = {(float)uv.x, 1.0f - (float)uv.y},
      };
      normalize3(v->normal);
    }
  }
  free(triangle);
  ufbx_vertex_stream stream = {out->vertices, count, sizeof(CGF_Vertex)};
  ufbx_error error;
  size_t unique = ufbx_generate_indices(&stream, 1, out->indices, count, NULL,
                                        &error);
  if (unique == 0) {
    free_geometry(out);
    return false;
  }
  out->vertex_count = (uint32_t)unique;
  out->index_count = (uint32_t)count;
  float *scratch = malloc(sizeof(float) * unique * 6);
  if (!scratch) {
    free_geometry(out);
    return false;
  }
  compute_tangents(out->vertices, out->vertex_count, out->indices,
                   out->index_count, scratch);
  free(scratch);
  return true;
}

static double determinant(const ufbx_matrix *m) {
  const ufbx_vec3 *c = m->cols;
  return c[0].x * (c[1].y * c[2].z - c[2].y * c[1].z) -
         c[1].x * (c[0].y * c[2].z - c[2].y * c[0].z) +
         c[2].x * (c[0].y * c[1].z - c[1].y * c[0].z);
}

static void to_matrix4(const ufbx_matrix *m, float out[16]) {
  for (int c = 0; c < 4; c++) {
    out[c * 4 + 0] = (float)m->cols[c].x;
    out[c * 4 + 1] = (float)m->cols[c].y;
    out[c * 4 + 2] = (float)m->cols[c].z;
    out[c * 4 + 3] = c == 3 ? 1.0f : 0.0f;
  }
}


typedef struct MeshBatch {
  CGF_MeshData data[MESH_BATCH];
  PartGeometry geometry[MESH_BATCH];
  uint32_t slots[MESH_BATCH];
  uint32_t count;
} MeshBatch;

/* Creates the batched meshes and frees their geometry. */
static bool flush_meshes(ModelLoad *load, MeshBatch *batch) {
  bool ok = true;
  if (batch->count > 0) {
    CGF_Mesh meshes[MESH_BATCH];
    const CGF_EngineRendererAPI *r = begin_renderer();
    CGF_RenderResult result =
        r ? r->mesh_create(batch->data, batch->count, meshes)
          : CGF_RENDER_ERROR_INVALID_STATE;
    if (r)
      end_renderer();
    ok = result == CGF_RENDER_OK;
    if (!ok)
      log_message("[LOADER] cannot create meshes (%d)", (int)result);
    for (uint32_t k = 0; k < batch->count; k++) {
      if (ok)
        load->part_meshes[batch->slots[k]] = meshes[k];
      free_geometry(&batch->geometry[k]);
    }
  }
  batch->count = 0;
  return ok;
}

/* Takes ownership of the geometry. */
static bool add_mesh(ModelLoad *load, MeshBatch *batch, PartGeometry geometry,
                     uint32_t slot) {
  uint32_t k = batch->count++;
  batch->geometry[k] = geometry;
  batch->data[k] = (CGF_MeshData){geometry.vertices, geometry.vertex_count,
                                  geometry.indices, geometry.index_count};
  batch->slots[k] = slot;
  return batch->count < MESH_BATCH || flush_meshes(load, batch);
}

static bool copy_geometry(const PartGeometry *source, PartGeometry *out) {
  out->vertices = malloc(sizeof(CGF_Vertex) * source->vertex_count);
  out->indices = malloc(sizeof(uint32_t) * source->index_count);
  if (!out->vertices || !out->indices) {
    free_geometry(out);
    return false;
  }
  memcpy(out->vertices, source->vertices,
         sizeof(CGF_Vertex) * source->vertex_count);
  memcpy(out->indices, source->indices, sizeof(uint32_t) * source->index_count);
  out->vertex_count = source->vertex_count;
  out->index_count = source->index_count;
  return true;
}

static void reverse_winding(PartGeometry *geometry) {
  for (uint32_t t = 0; t + 2 < geometry->index_count; t += 3) {
    uint32_t swap = geometry->indices[t + 1];
    geometry->indices[t + 1] = geometry->indices[t + 2];
    geometry->indices[t + 2] = swap;
  }
}

static void part_bounds(const PartGeometry *geometry, float bounds[6]) {
  for (int k = 0; k < 3; k++) {
    bounds[k] = INFINITY;
    bounds[3 + k] = -INFINITY;
  }
  for (uint32_t i = 0; i < geometry->vertex_count; i++) {
    for (int k = 0; k < 3; k++) {
      float v = geometry->vertices[i].position[k];
      bounds[k] = fminf(bounds[k], v);
      bounds[3 + k] = fmaxf(bounds[3 + k], v);
    }
  }
}

/* Creates the meshes of every part that some node uses, in the winding each
   node needs: mirroring transforms get a copy with reversed triangles. Mesh
   slot (part * 2 + mirrored) of part_meshes. */
static bool create_meshes(ModelLoad *load) {
  ufbx_scene *scene = load->scene;
  Asset *asset = load->asset;
  uint32_t part_total = 0;
  load->part_offsets = malloc(sizeof(uint32_t) * (scene->meshes.count + 1));
  if (!load->part_offsets)
    return false;
  for (size_t i = 0; i < scene->meshes.count; i++) {
    load->part_offsets[i] = part_total;
    part_total += (uint32_t)scene->meshes.data[i]->material_parts.count;
  }
  load->part_offsets[scene->meshes.count] = part_total;
  load->part_meshes = calloc((size_t)part_total * 2 + 1, sizeof(CGF_Mesh));
  load->part_bounds = calloc((size_t)part_total + 1, sizeof(float) * 6);
  bool *needed = calloc((size_t)part_total * 2 + 1, sizeof(bool));
  MeshBatch *batch = calloc(1, sizeof(MeshBatch));
  if (!load->part_meshes || !load->part_bounds || !needed || !batch) {
    free(needed);
    free(batch);
    return false;
  }
  for (size_t i = 0; i < scene->nodes.count; i++) {
    const ufbx_node *node = scene->nodes.data[i];
    if (!node->mesh)
      continue;
    bool mirrored = determinant(&node->geometry_to_world) < 0.0;
    uint32_t first = load->part_offsets[node->mesh->typed_id];
    for (size_t p = 0; p < node->mesh->material_parts.count; p++)
      needed[(first + p) * 2 + (mirrored ? 1 : 0)] = true;
  }

  bool ok = true;
  for (size_t m = 0; m < scene->meshes.count && ok; m++) {
    const ufbx_mesh *mesh = scene->meshes.data[m];
    for (size_t p = 0; p < mesh->material_parts.count && ok; p++) {
      uint32_t part = load->part_offsets[m] + (uint32_t)p;
      if (!needed[part * 2] && !needed[part * 2 + 1])
        continue;
      PartGeometry base;
      if (!build_part(mesh, &mesh->material_parts.data[p], &base))
        continue; /* no triangles */
      part_bounds(&base, &load->part_bounds[part * 6]);
      PartGeometry mirrored = {0};
      if (needed[part * 2 + 1]) {
        if (!needed[part * 2]) {
          mirrored = base;
          base = (PartGeometry){0};
        } else if (!copy_geometry(&base, &mirrored)) {
          free_geometry(&base);
          ok = false;
          break;
        }
        reverse_winding(&mirrored);
      }
      if (base.vertices)
        ok = add_mesh(load, batch, base, part * 2);
      if (mirrored.vertices) {
        if (ok)
          ok = add_mesh(load, batch, mirrored, part * 2 + 1);
        else
          free_geometry(&mirrored);
      }
    }
    if (cancelled(asset))
      ok = false;
    set_progress(asset, 0.6f + 0.35f * (float)(m + 1) /
                                 (float)scene->meshes.count);
  }
  if (ok) {
    ok = flush_meshes(load, batch);
  } else {
    for (uint32_t k = 0; k < batch->count; k++)
      free_geometry(&batch->geometry[k]);
  }
  free(batch);
  free(needed);

  /* Remember the meshes for destruction. */
  asset->meshes = malloc(sizeof(CGF_Mesh) * ((size_t)part_total * 2 + 1));
  if (!asset->meshes)
    return false;
  for (uint32_t i = 0; i < part_total * 2; i++) {
    if (load->part_meshes[i].gen)
      asset->meshes[asset->mesh_count++] = load->part_meshes[i];
  }
  return ok;
}

///////////////////////////////////////////////////////////////////////////////
// Model: instances, lights and camera
///////////////////////////////////////////////////////////////////////////////

static void include_point(ModelLoad *load, const float p[3]) {
  for (int k = 0; k < 3; k++) {
    load->bounds_min[k] = fminf(load->bounds_min[k], p[k]);
    load->bounds_max[k] = fmaxf(load->bounds_max[k], p[k]);
  }
}

static bool create_instances(ModelLoad *load, uint32_t default_material) {
  ufbx_scene *scene = load->scene;
  Asset *asset = load->asset;
  size_t capacity = 0;
  for (size_t i = 0; i < scene->nodes.count; i++) {
    if (scene->nodes.data[i]->mesh)
      capacity += scene->nodes.data[i]->mesh->material_parts.count;
  }
  CGF_InstanceData *data = malloc(sizeof(CGF_InstanceData) * (capacity + 1));
  asset->instances = malloc(sizeof(CGF_Instance) * (capacity + 1));
  if (!data || !asset->instances) {
    free(data);
    return false;
  }
  uint32_t count = 0;
  for (size_t i = 0; i < scene->nodes.count; i++) {
    const ufbx_node *node = scene->nodes.data[i];
    const ufbx_mesh *mesh = node->mesh;
    if (!mesh)
      continue;
    bool mirrored = determinant(&node->geometry_to_world) < 0.0;
    uint32_t first = load->part_offsets[mesh->typed_id];
    for (size_t p = 0; p < mesh->material_parts.count; p++) {
      uint32_t part = first + (uint32_t)p;
      CGF_Mesh part_mesh = load->part_meshes[part * 2 + (mirrored ? 1 : 0)];
      if (!part_mesh.gen)
        continue;
      uint32_t index = mesh->material_parts.data[p].index;
      const ufbx_material *material =
          index < node->materials.count   ? node->materials.data[index]
          : index < mesh->materials.count ? mesh->materials.data[index]
                                          : NULL;
      uint32_t material_index =
          material ? material->typed_id : default_material;
      CGF_InstanceData *instance = &data[count++];
      *instance = (CGF_InstanceData){
          .mesh = part_mesh,
          .material = asset->materials[material_index],
      };
      to_matrix4(&node->geometry_to_world, instance->transform);
      const float *b = &load->part_bounds[part * 6];
      for (int corner = 0; corner < 8; corner++) {
        ufbx_vec3 local = {b[(corner & 1) ? 3 : 0], b[(corner & 2) ? 4 : 1],
                           b[(corner & 4) ? 5 : 2]};
        ufbx_vec3 world =
            ufbx_transform_position(&node->geometry_to_world, local);
        float point[3] = {(float)world.x, (float)world.y, (float)world.z};
        include_point(load, point);
      }
    }
  }
  bool ok = true;
  if (count > 0) {
    const CGF_EngineRendererAPI *r = begin_renderer();
    CGF_RenderResult result =
        r ? r->instance_create(data, count, asset->instances)
          : CGF_RENDER_ERROR_INVALID_STATE;
    if (r)
      end_renderer();
    ok = result == CGF_RENDER_OK;
    if (ok)
      asset->instance_count = count;
    else
      log_message("[LOADER] cannot create instances (%d)", (int)result);
  }
  free(data);
  return ok;
}

static bool create_lights(ModelLoad *load) {
  ufbx_scene *scene = load->scene;
  Asset *asset = load->asset;
  float scale = asset->desc.light_scale ? asset->desc.light_scale : 1.0f;
  float cutoff = asset->desc.light_cutoff ? asset->desc.light_cutoff : 0.1f;
  CGF_LightData *data =
      malloc(sizeof(CGF_LightData) * (scene->lights.count + 1));
  asset->lights = malloc(sizeof(CGF_Light) * (scene->lights.count + 1));
  if (!data || !asset->lights) {
    free(data);
    return false;
  }
  uint32_t count = 0;
  for (size_t i = 0; i < scene->nodes.count; i++) {
    const ufbx_node *node = scene->nodes.data[i];
    const ufbx_light *light = node->light;
    if (!light || count >= scene->lights.count)
      continue;
    CGF_LightData *out = &data[count++];
    *out = (CGF_LightData){
        .color = {(float)light->color.x, (float)light->color.y,
                  (float)light->color.z},
        .intensity = (float)light->intensity * scale,
    };
    if (light->type == UFBX_LIGHT_DIRECTIONAL) {
      ufbx_vec3 d = ufbx_transform_direction(&node->node_to_world,
                                             light->local_direction);
      out->type = CGF_LIGHT_TYPE_DIRECTIONAL;
      out->direction[0] = (float)d.x;
      out->direction[1] = (float)d.y;
      out->direction[2] = (float)d.z;
      normalize3(out->direction);
      out->cast_shadow = true;
    } else {
      /* Spot, area and volume lights are approximated as point lights. */
      ufbx_vec3 p = node->node_to_world.cols[3];
      float brightest =
          fmaxf(out->color[0], fmaxf(out->color[1], out->color[2]));
      out->type = CGF_LIGHT_TYPE_POINT;
      out->position[0] = (float)p.x;
      out->position[1] = (float)p.y;
      out->position[2] = (float)p.z;
      out->radius = fmaxf(sqrtf(brightest * out->intensity / cutoff), 0.1f);
    }
  }
  bool ok = true;
  if (count > 0) {
    const CGF_EngineRendererAPI *r = begin_renderer();
    CGF_RenderResult result =
        r ? r->light_create(data, count, asset->lights)
          : CGF_RENDER_ERROR_INVALID_STATE;
    if (r)
      end_renderer();
    ok = result == CGF_RENDER_OK;
    if (ok)
      asset->light_count = count;
    else
      log_message("[LOADER] cannot create lights (%d)", (int)result);
  }
  free(data);
  return ok;
}

static void read_camera(ModelLoad *load) {
  CGF_ModelInfo *info = &load->asset->info;
  for (size_t i = 0; i < load->scene->nodes.count; i++) {
    const ufbx_node *node = load->scene->nodes.data[i];
    if (!node->camera)
      continue;
    const ufbx_matrix *m = &node->node_to_world;
    /* Cameras are converted to look along -Z with +Y up. */
    for (int k = 0; k < 3; k++) {
      info->camera_position[k] = (float)m->cols[3].v[k];
      info->camera_forward[k] = -(float)m->cols[2].v[k];
      info->camera_up[k] = (float)m->cols[1].v[k];
    }
    normalize3(info->camera_forward);
    normalize3(info->camera_up);
    info->camera_fov_y =
        (float)node->camera->field_of_view_deg.y * 3.14159265f / 180.0f;
    info->has_camera = true;
    return;
  }
}

static ufbx_progress_result load_progress(void *user,
                                          const ufbx_progress *progress) {
  Asset *asset = user;
  if (progress->bytes_total > 0)
    set_progress(asset, 0.3f * (float)progress->bytes_read /
                            (float)progress->bytes_total);
  return cancelled(asset) ? UFBX_PROGRESS_CANCEL : UFBX_PROGRESS_CONTINUE;
}

static bool load_model(ModelLoad *load) {
  Asset *asset = load->asset;
  snprintf(load->directory, sizeof(load->directory), "%s", asset->path);
  char *slash = strrchr(load->directory, '/');
  char *backslash = strrchr(load->directory, '\\');
  char *separator = slash > backslash ? slash : backslash;
  if (separator)
    separator[1] = '\0';
  else
    load->directory[0] = '\0';

  ufbx_load_opts opts = {
      .target_axes = ufbx_axes_right_handed_y_up,
      .target_unit_meters = 1.0f,
      .space_conversion = UFBX_SPACE_CONVERSION_MODIFY_GEOMETRY,
      .target_camera_axes = ufbx_axes_right_handed_y_up,
      .generate_missing_normals = true,
      .ignore_animation = true,
      .progress_cb = {load_progress, asset},
  };
  ufbx_error error;
  load->scene = ufbx_load_file(asset->path, &opts, &error);
  if (!load->scene) {
    char message[1024];
    ufbx_format_error(message, sizeof(message), &error);
    log_message("[LOADER] cannot load %s: %s", asset->path, message);
    return false;
  }
  ufbx_scene *scene = load->scene;
  set_progress(asset, 0.3f);

  /* Materials, loading their textures. The last one is for parts without
     a material. */
  size_t texture_count = scene->textures.count + 1;
  load->srgb_textures = malloc(sizeof(uint32_t) * texture_count);
  load->linear_textures = malloc(sizeof(uint32_t) * texture_count);
  uint32_t material_count = (uint32_t)scene->materials.count + 1;
  CGF_MaterialData *materials =
      malloc(sizeof(CGF_MaterialData) * material_count);
  asset->materials = malloc(sizeof(CGF_Material) * material_count);
  if (!load->srgb_textures || !load->linear_textures || !materials ||
      !asset->materials) {
    free(materials);
    return false;
  }
  for (size_t i = 0; i < texture_count; i++)
    load->srgb_textures[i] = load->linear_textures[i] = UNTRIED;
  for (size_t i = 0; i < scene->materials.count; i++) {
    materials[i] = convert_material(load, scene->materials.data[i]);
    set_progress(asset, 0.3f + 0.3f * (float)(i + 1) /
                                   (float)scene->materials.count);
    if (cancelled(asset)) {
      free(materials);
      return false;
    }
  }
  uint32_t blend_counts[3] = {0, 0, 0};
  for (size_t i = 0; i < scene->materials.count; i++)
    blend_counts[materials[i].blend]++;
  log_message("[LOADER] %s: %u opaque, %u masked and %u transparent materials",
              asset->path, blend_counts[0], blend_counts[1], blend_counts[2]);
  materials[material_count - 1] = (CGF_MaterialData){
      .base_color = {0.6f, 0.6f, 0.6f, 1.0f},
      .roughness = 0.7f,
  };
  const CGF_EngineRendererAPI *r = begin_renderer();
  CGF_RenderResult result =
      r ? r->material_create(materials, material_count, asset->materials)
        : CGF_RENDER_ERROR_INVALID_STATE;
  if (r)
    end_renderer();
  free(materials);
  if (result != CGF_RENDER_OK) {
    log_message("[LOADER] cannot create materials (%d)", (int)result);
    return false;
  }
  asset->material_count = material_count;

  for (int k = 0; k < 3; k++) {
    load->bounds_min[k] = INFINITY;
    load->bounds_max[k] = -INFINITY;
  }
  if (!create_meshes(load) || cancelled(asset) ||
      !create_instances(load, material_count - 1) || !create_lights(load))
    return false;
  read_camera(load);

  CGF_ModelInfo *info = &asset->info;
  for (int k = 0; k < 3; k++) {
    info->bounds_min[k] = asset->instance_count ? load->bounds_min[k] : 0.0f;
    info->bounds_max[k] = asset->instance_count ? load->bounds_max[k] : 0.0f;
  }
  info->mesh_count = asset->mesh_count;
  info->material_count = asset->material_count;
  info->texture_count = asset->texture_count;
  info->instance_count = asset->instance_count;
  info->light_count = asset->light_count;
  return true;
}

static DWORD WINAPI model_thread(void *parameter) {
  Asset *asset = parameter;
  ModelLoad load = {.asset = asset};
  DWORD start = GetTickCount();
  bool ok = load_model(&load);
  if (load.scene)
    ufbx_free_scene(load.scene);
  free(load.srgb_textures);
  free(load.linear_textures);
  free(load.part_meshes);
  free(load.part_offsets);
  free(load.part_bounds);
  if (ok)
    log_message("[LOADER] loaded %s in %.1f s: %u meshes, %u materials, "
                "%u textures, %u instances, %u lights",
                asset->path, (double)(GetTickCount() - start) / 1000.0,
                asset->mesh_count, asset->material_count, asset->texture_count,
                asset->instance_count, asset->light_count);
  set_progress(asset, 1.0f);
  InterlockedExchange(&asset->status, ok ? CGF_ASSET_STATUS_READY
                                         : CGF_ASSET_STATUS_FAILED);
  return 0;
}

static DWORD WINAPI image_thread(void *parameter) {
  Asset *asset = parameter;
  bool srgb = !has_extension(asset->path, ".hdr");
  uint32_t index = acquire_texture(asset->path, srgb);
  bool ok = index != UINT32_MAX;
  if (ok) {
    asset->textures = malloc(sizeof(uint32_t));
    if (asset->textures) {
      asset->textures[0] = index;
      asset->texture_count = asset->texture_capacity = 1;
      asset->image = cached_texture(index).texture;
    } else {
      release_texture(index);
      ok = false;
    }
  }
  set_progress(asset, 1.0f);
  InterlockedExchange(&asset->status, ok ? CGF_ASSET_STATUS_READY
                                         : CGF_ASSET_STATUS_FAILED);
  return 0;
}

///////////////////////////////////////////////////////////////////////////////
// API
///////////////////////////////////////////////////////////////////////////////

/* Caller holds assets_lock. */
static Asset *find_asset(CGF_Asset handle) {
  if (handle.gen == 0 || handle.gen == UINT32_MAX || handle.id >= MAX_ASSETS ||
      assets[handle.id].generation != handle.gen)
    return NULL;
  return &assets[handle.id];
}

static bool start_asset(AssetType type, const char *path,
                        const CGF_ModelLoadDesc *desc, CGF_Asset *out_asset) {
  if (!path || !out_asset || strlen(path) >= PATH_SIZE)
    return false;
  AcquireSRWLockExclusive(&assets_lock);
  uint32_t slot = 0;
  while (slot < MAX_ASSETS && assets[slot].generation != 0)
    slot++;
  bool started = false;
  if (slot < MAX_ASSETS) {
    Asset *asset = &assets[slot];
    *asset = (Asset){
        .generation = next_generation++,
        .type = type,
        .status = CGF_ASSET_STATUS_LOADING,
    };
    if (next_generation == UINT32_MAX)
      next_generation = 1;
    snprintf(asset->path, sizeof(asset->path), "%s", path);
    if (desc) {
      asset->desc = *desc;
      asset->desc.path = asset->path;
    }
    asset->thread =
        CreateThread(NULL, 0, type == ASSET_MODEL ? model_thread : image_thread,
                     asset, 0, NULL);
    started = asset->thread != NULL;
    if (started)
      *out_asset = (CGF_Asset){slot, asset->generation};
    else
      asset->generation = 0;
  }
  ReleaseSRWLockExclusive(&assets_lock);
  return started;
}

static bool model_load(const CGF_ModelLoadDesc *desc, CGF_Asset *out_asset) {
  return desc && start_asset(ASSET_MODEL, desc->path, desc, out_asset);
}

static bool image_load(const char *path, CGF_Asset *out_asset) {
  return start_asset(ASSET_IMAGE, path, NULL, out_asset);
}

static CGF_AssetStatus status(CGF_Asset handle, float *out_progress) {
  AcquireSRWLockShared(&assets_lock);
  Asset *asset = find_asset(handle);
  CGF_AssetStatus result = CGF_ASSET_STATUS_INVALID;
  float progress = 0.0f;
  if (asset) {
    result = (CGF_AssetStatus)InterlockedCompareExchange(&asset->status, 0, 0);
    progress = (float)InterlockedCompareExchange(&asset->progress, 0, 0) /
               1000.0f;
  }
  ReleaseSRWLockShared(&assets_lock);
  if (out_progress)
    *out_progress = progress;
  return result;
}

/* A loaded asset of the given type, or NULL. Caller holds assets_lock. */
static Asset *ready_asset(CGF_Asset handle, AssetType type) {
  Asset *asset = find_asset(handle);
  if (!asset || asset->type != type ||
      InterlockedCompareExchange(&asset->status, 0, 0) !=
          CGF_ASSET_STATUS_READY)
    return NULL;
  return asset;
}

static bool model_info(CGF_Asset handle, CGF_ModelInfo *out_info) {
  if (!out_info)
    return false;
  AcquireSRWLockShared(&assets_lock);
  Asset *asset = ready_asset(handle, ASSET_MODEL);
  if (asset)
    *out_info = asset->info;
  ReleaseSRWLockShared(&assets_lock);
  return asset != NULL;
}

static CGF_RenderResult model_add_to_scene(CGF_Asset handle, CGF_Scene scene) {
  AcquireSRWLockShared(&assets_lock);
  CGF_RenderResult result = CGF_RENDER_ERROR_INVALID_HANDLE;
  Asset *asset = find_asset(handle);
  if (asset && asset->type == ASSET_MODEL) {
    result = CGF_RENDER_ERROR_INVALID_STATE;
    const CGF_EngineRendererAPI *r = renderer_api();
    if (ready_asset(handle, ASSET_MODEL) && r) {
      result = r->scene_add(scene, asset->instances, asset->instance_count);
      if (result == CGF_RENDER_OK) {
        result = r->scene_add_lights(scene, asset->lights, asset->light_count);
        if (result != CGF_RENDER_OK)
          r->scene_remove(scene, asset->instances, asset->instance_count);
      }
    }
  }
  ReleaseSRWLockShared(&assets_lock);
  return result;
}

static CGF_Texture image_texture(CGF_Asset handle) {
  AcquireSRWLockShared(&assets_lock);
  Asset *asset = ready_asset(handle, ASSET_IMAGE);
  CGF_Texture texture = asset ? asset->image : (CGF_Texture){0};
  ReleaseSRWLockShared(&assets_lock);
  return texture;
}

/* Stops the asset's thread and frees its memory. With release, also
   destroys the renderer resources it created. */
static void finish_asset(Asset *asset, bool release) {
  InterlockedExchange(&asset->cancel, 1);
  if (asset->thread) {
    WaitForSingleObject(asset->thread, INFINITE);
    CloseHandle(asset->thread);
  }
  if (release) {
    const CGF_EngineRendererAPI *r = begin_renderer();
    if (r) {
      r->instance_destroy(asset->instances, asset->instance_count);
      r->light_destroy(asset->lights, asset->light_count);
      for (uint32_t i = 0; i < asset->material_count; i++)
        r->material_destroy(asset->materials[i]);
      for (uint32_t i = 0; i < asset->mesh_count; i++)
        r->mesh_destroy(asset->meshes[i]);
      end_renderer();
    }
    for (uint32_t i = 0; i < asset->texture_count; i++)
      release_texture(asset->textures[i]);
  }
  free(asset->textures);
  free(asset->materials);
  free(asset->meshes);
  free(asset->instances);
  free(asset->lights);
}

static void destroy(CGF_Asset handle) {
  AcquireSRWLockExclusive(&assets_lock);
  Asset *asset = find_asset(handle);
  if (asset)
    asset->generation = UINT32_MAX; /* gone for other calls; slot reserved */
  ReleaseSRWLockExclusive(&assets_lock);
  if (!asset)
    return;
  finish_asset(asset, true);
  AcquireSRWLockExclusive(&assets_lock);
  *asset = (Asset){0};
  ReleaseSRWLockExclusive(&assets_lock);
}

static void shutdown_loader(void) {
  for (uint32_t i = 0; i < MAX_ASSETS; i++) {
    if (assets[i].generation) {
      finish_asset(&assets[i], false);
      assets[i] = (Asset){0};
    }
  }
  free(texture_cache);
  texture_cache = NULL;
  texture_cache_count = texture_cache_capacity = 0;
}

static void pause_loader(void) { AcquireSRWLockExclusive(&renderer_lock); }
static void resume_loader(void) { ReleaseSRWLockExclusive(&renderer_lock); }

CGF_PLUGIN_EXPORT bool cgf_loader_load(const CGF_APIRegistry *new_registry) {
  if (!new_registry)
    return false;
  const CGF_EngineAPI *new_engine =
      new_registry->get(CGF_ENGINE_API_NAME, sizeof(CGF_EngineAPI));
  if (!new_engine || new_engine->struct_size != sizeof(CGF_EngineAPI))
    return false;
  static const CGF_LoaderAPI api = {
      .model_load = model_load,
      .image_load = image_load,
      .status = status,
      .model_info = model_info,
      .model_add_to_scene = model_add_to_scene,
      .image_texture = image_texture,
      .destroy = destroy,
  };
  static const CGF_LoaderRuntimeAPI runtime_api = {
      .shutdown = shutdown_loader,
      .pause = pause_loader,
      .resume = resume_loader,
  };
  if (!new_registry->set(CGF_LOADER_API_NAME, sizeof(api), &api) ||
      !new_registry->set(CGF_LOADER_RUNTIME_API_NAME, sizeof(runtime_api),
                         &runtime_api))
    return false;
  registry = new_registry;
  engine = new_engine;
  return true;
}
