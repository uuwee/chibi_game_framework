#include "renderer_internal.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define STATE_NAME "cgf.engine.renderer.state"

RendererState *renderer;

///////////////////////////////////////////////////////////////////////////////
// Handle slots
///////////////////////////////////////////////////////////////////////////////

static bool pool_init(SlotPool *pool, uint32_t capacity) {
  pool->generations = calloc(capacity, sizeof(uint32_t));
  pool->free_slots = malloc(sizeof(uint32_t) * capacity);
  pool->capacity = capacity;
  pool->next_generation = 1;
  return pool->generations && pool->free_slots;
}

static void pool_release(SlotPool *pool) {
  free(pool->generations);
  free(pool->free_slots);
  *pool = (SlotPool){0};
}

static uint32_t pool_available(const SlotPool *pool) {
  return pool->free_count + (pool->capacity - pool->used);
}

/* The caller checks pool_available first. */
static CGF_ID pool_allocate(SlotPool *pool) {
  uint32_t slot = pool->free_count ? pool->free_slots[--pool->free_count]
                                   : pool->used++;
  uint32_t generation = pool->next_generation++;
  if (generation == 0)
    generation = pool->next_generation++;
  pool->generations[slot] = generation;
  return (CGF_ID){slot, generation};
}

static void pool_free(SlotPool *pool, CGF_ID id) {
  pool->generations[id.id] = 0;
  pool->free_slots[pool->free_count++] = id.id;
}

bool slot_valid(const SlotPool *pool, CGF_ID id) {
  return id.gen != 0 && id.id < pool->used &&
         pool->generations[id.id] == id.gen;
}

///////////////////////////////////////////////////////////////////////////////
// Vertex and index ranges
///////////////////////////////////////////////////////////////////////////////

static bool range_init(RangeAllocator *allocator, uint32_t capacity) {
  allocator->free_capacity = 64;
  allocator->free_ranges = malloc(sizeof(Range) * allocator->free_capacity);
  if (!allocator->free_ranges)
    return false;
  allocator->free_ranges[0] = (Range){0, capacity};
  allocator->free_count = 1;
  return true;
}

static bool range_allocate(RangeAllocator *allocator, uint32_t count,
                           Range *out) {
  for (uint32_t i = 0; i < allocator->free_count; i++) {
    Range *range = &allocator->free_ranges[i];
    if (range->count < count)
      continue;
    *out = (Range){range->first, count};
    range->first += count;
    range->count -= count;
    if (range->count == 0) {
      memmove(range, range + 1,
              sizeof(Range) * (allocator->free_count - i - 1));
      allocator->free_count--;
    }
    return true;
  }
  return false;
}

/* Returns a range, merging it with its neighbors. */
static void range_free(RangeAllocator *allocator, Range freed) {
  if (freed.count == 0)
    return;
  uint32_t i = 0;
  while (i < allocator->free_count &&
         allocator->free_ranges[i].first < freed.first)
    i++;
  Range *ranges = allocator->free_ranges;
  bool merge_previous =
      i > 0 && ranges[i - 1].first + ranges[i - 1].count == freed.first;
  bool merge_next = i < allocator->free_count &&
                    freed.first + freed.count == ranges[i].first;
  if (merge_previous && merge_next) {
    ranges[i - 1].count += freed.count + ranges[i].count;
    memmove(&ranges[i], &ranges[i + 1],
            sizeof(Range) * (allocator->free_count - i - 1));
    allocator->free_count--;
  } else if (merge_previous) {
    ranges[i - 1].count += freed.count;
  } else if (merge_next) {
    ranges[i].first = freed.first;
    ranges[i].count += freed.count;
  } else {
    if (allocator->free_count == allocator->free_capacity) {
      uint32_t capacity = allocator->free_capacity * 2;
      Range *grown = realloc(ranges, sizeof(Range) * capacity);
      if (!grown)
        return; /* the range leaks; allocation keeps working */
      allocator->free_ranges = ranges = grown;
      allocator->free_capacity = capacity;
    }
    memmove(&ranges[i + 1], &ranges[i],
            sizeof(Range) * (allocator->free_count - i));
    ranges[i] = freed;
    allocator->free_count++;
  }
}

///////////////////////////////////////////////////////////////////////////////
// CPU state
///////////////////////////////////////////////////////////////////////////////

static bool cpu_init(RendererState *s) {
  s->instances = calloc(MAX_INSTANCES, sizeof(CGF_InstanceData));
  s->instance_dirty = calloc(MAX_INSTANCES, 1);
  s->instance_dirty_list = malloc(sizeof(uint32_t) * MAX_INSTANCES);
  s->mesh_records = calloc(MAX_MESHES, sizeof(MeshRecord));
  s->gpu_meshes = calloc(MAX_MESHES, sizeof(GpuMesh));
  s->gpu_materials = calloc(MAX_MATERIALS, sizeof(GpuMaterial));
  s->lights = calloc(MAX_LIGHTS, sizeof(CGF_LightData));
  s->scenes = calloc(MAX_SCENES, sizeof(SceneRecord));
  return s->instances && s->instance_dirty && s->instance_dirty_list &&
         s->mesh_records && s->gpu_meshes && s->gpu_materials && s->lights &&
         s->scenes && pool_init(&s->instance_pool, MAX_INSTANCES) &&
         pool_init(&s->mesh_pool, MAX_MESHES) &&
         pool_init(&s->material_pool, MAX_MATERIALS) &&
         pool_init(&s->light_pool, MAX_LIGHTS) &&
         pool_init(&s->scene_pool, MAX_SCENES) &&
         range_init(&s->vertex_allocator, VERTEX_CAPACITY) &&
         range_init(&s->index_allocator, INDEX_CAPACITY);
}

static void cpu_release(RendererState *s) {
  if (s->scenes) {
    for (uint32_t i = 0; i < s->scene_pool.used; i++) {
      free(s->scenes[i].instances);
      free(s->scenes[i].lights);
    }
  }
  for (uint32_t i = 0; i < s->mesh_upload_count; i++) {
    free(s->mesh_uploads[i].vertices);
    free(s->mesh_uploads[i].indices);
  }
  free(s->mesh_uploads);
  free(s->instances);
  free(s->instance_dirty);
  free(s->instance_dirty_list);
  free(s->mesh_records);
  free(s->gpu_meshes);
  free(s->gpu_materials);
  free(s->lights);
  free(s->scenes);
  free(s->vertex_allocator.free_ranges);
  free(s->index_allocator.free_ranges);
  pool_release(&s->instance_pool);
  pool_release(&s->mesh_pool);
  pool_release(&s->material_pool);
  pool_release(&s->light_pool);
  pool_release(&s->scene_pool);
}

static void lock(void) { AcquireSRWLockExclusive(&renderer->lock); }
static void unlock(void) { ReleaseSRWLockExclusive(&renderer->lock); }

bool renderer_instance_drawable(const RendererState *s, uint32_t slot) {
  const CGF_InstanceData *data = &s->instances[slot];
  return slot_valid(&s->mesh_pool, data->mesh) &&
         slot_valid(&s->material_pool, data->material);
}

static void mark_instance_dirty(RendererState *s, uint32_t slot) {
  if (!s->instance_dirty[slot]) {
    s->instance_dirty[slot] = 1;
    s->instance_dirty_list[s->instance_dirty_count++] = slot;
  }
}

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

///////////////////////////////////////////////////////////////////////////////
// Scenes
///////////////////////////////////////////////////////////////////////////////

static CGF_RenderResult scene_create(CGF_Scene *out_scene) {
  if (!out_scene)
    return CGF_RENDER_ERROR_INVALID_ARGUMENT;
  lock();
  CGF_RenderResult result = CGF_RENDER_ERROR_CAPACITY;
  if (pool_available(&renderer->scene_pool) > 0) {
    *out_scene = pool_allocate(&renderer->scene_pool);
    SceneRecord *scene = &renderer->scenes[out_scene->id];
    free(scene->instances);
    free(scene->lights);
    /* Keep the snapshot of the current frame; a new handle never matches it. */
    *scene = (SceneRecord){
        .frame_generation = scene->frame_generation,
        .entry_offset = scene->entry_offset,
        .entry_count = scene->entry_count,
        .light_offset = scene->light_offset,
        .frame_light_count = scene->frame_light_count,
    };
    result = CGF_RENDER_OK;
  }
  unlock();
  return result;
}

static void scene_destroy(CGF_Scene scene) {
  lock();
  if (slot_valid(&renderer->scene_pool, scene)) {
    SceneRecord *record = &renderer->scenes[scene.id];
    free(record->instances);
    free(record->lights);
    record->instances = NULL;
    record->lights = NULL;
    record->instance_count = record->instance_capacity = 0;
    record->light_count = record->light_capacity = 0;
    pool_free(&renderer->scene_pool, scene);
  }
  unlock();
}

static CGF_RenderResult scene_add(CGF_Scene scene,
                                  const CGF_Instance *instance_list,
                                  uint32_t count) {
  if (count == 0)
    return CGF_RENDER_OK;
  if (!instance_list)
    return CGF_RENDER_ERROR_INVALID_ARGUMENT;
  lock();
  CGF_RenderResult result = CGF_RENDER_OK;
  if (!slot_valid(&renderer->scene_pool, scene)) {
    result = CGF_RENDER_ERROR_INVALID_HANDLE;
  } else {
    for (uint32_t i = 0; i < count; i++) {
      if (!slot_valid(&renderer->instance_pool, instance_list[i])) {
        result = CGF_RENDER_ERROR_INVALID_HANDLE;
        break;
      }
    }
  }
  if (result == CGF_RENDER_OK) {
    SceneRecord *record = &renderer->scenes[scene.id];
    if (!grow((void **)&record->instances, &record->instance_capacity,
              record->instance_count + count, sizeof(CGF_Instance))) {
      result = CGF_RENDER_ERROR_OUT_OF_MEMORY;
    } else {
      memcpy(record->instances + record->instance_count, instance_list,
             sizeof(CGF_Instance) * count);
      record->instance_count += count;
    }
  }
  unlock();
  return result;
}

static bool same_id(CGF_ID a, CGF_ID b) { return a.id == b.id && a.gen == b.gen; }

static void remove_ids(CGF_ID *list, uint32_t *list_count,
                       const CGF_ID *removed, uint32_t removed_count) {
  for (uint32_t r = 0; r < removed_count; r++) {
    for (uint32_t i = 0; i < *list_count;) {
      if (same_id(list[i], removed[r]))
        list[i] = list[--*list_count];
      else
        i++;
    }
  }
}

static void scene_remove(CGF_Scene scene, const CGF_Instance *instance_list,
                         uint32_t count) {
  if (!instance_list || count == 0)
    return;
  lock();
  if (slot_valid(&renderer->scene_pool, scene)) {
    SceneRecord *record = &renderer->scenes[scene.id];
    remove_ids(record->instances, &record->instance_count, instance_list,
               count);
  }
  unlock();
}

static CGF_RenderResult scene_add_lights(CGF_Scene scene,
                                         const CGF_Light *light_list,
                                         uint32_t count) {
  if (count == 0)
    return CGF_RENDER_OK;
  if (!light_list)
    return CGF_RENDER_ERROR_INVALID_ARGUMENT;
  lock();
  CGF_RenderResult result = CGF_RENDER_OK;
  if (!slot_valid(&renderer->scene_pool, scene)) {
    result = CGF_RENDER_ERROR_INVALID_HANDLE;
  } else {
    for (uint32_t i = 0; i < count; i++) {
      if (!slot_valid(&renderer->light_pool, light_list[i])) {
        result = CGF_RENDER_ERROR_INVALID_HANDLE;
        break;
      }
    }
  }
  if (result == CGF_RENDER_OK) {
    SceneRecord *record = &renderer->scenes[scene.id];
    if (!grow((void **)&record->lights, &record->light_capacity,
              record->light_count + count, sizeof(CGF_Light))) {
      result = CGF_RENDER_ERROR_OUT_OF_MEMORY;
    } else {
      memcpy(record->lights + record->light_count, light_list,
             sizeof(CGF_Light) * count);
      record->light_count += count;
    }
  }
  unlock();
  return result;
}

static void scene_remove_lights(CGF_Scene scene, const CGF_Light *light_list,
                                uint32_t count) {
  if (!light_list || count == 0)
    return;
  lock();
  if (slot_valid(&renderer->scene_pool, scene)) {
    SceneRecord *record = &renderer->scenes[scene.id];
    remove_ids(record->lights, &record->light_count, light_list, count);
  }
  unlock();
}

///////////////////////////////////////////////////////////////////////////////
// Instances
///////////////////////////////////////////////////////////////////////////////

static bool transform_finite(const float transform[16]) {
  for (int i = 0; i < 16; i++) {
    if (!isfinite(transform[i]))
      return false;
  }
  return true;
}

static CGF_RenderResult instance_create(const CGF_InstanceData *data_list,
                                        uint32_t count,
                                        CGF_Instance *out_instance_list) {
  if (count == 0)
    return CGF_RENDER_OK;
  if (!data_list || !out_instance_list)
    return CGF_RENDER_ERROR_INVALID_ARGUMENT;
  lock();
  CGF_RenderResult result = CGF_RENDER_OK;
  for (uint32_t i = 0; i < count && result == CGF_RENDER_OK; i++) {
    if (!transform_finite(data_list[i].transform))
      result = CGF_RENDER_ERROR_INVALID_ARGUMENT;
    else if (!slot_valid(&renderer->mesh_pool, data_list[i].mesh) ||
             !slot_valid(&renderer->material_pool, data_list[i].material))
      result = CGF_RENDER_ERROR_INVALID_HANDLE;
  }
  if (result == CGF_RENDER_OK &&
      pool_available(&renderer->instance_pool) < count)
    result = CGF_RENDER_ERROR_CAPACITY;
  if (result == CGF_RENDER_OK) {
    for (uint32_t i = 0; i < count; i++) {
      CGF_Instance instance = pool_allocate(&renderer->instance_pool);
      renderer->instances[instance.id] = data_list[i];
      mark_instance_dirty(renderer, instance.id);
      out_instance_list[i] = instance;
    }
  }
  unlock();
  return result;
}

static void instance_destroy(const CGF_Instance *instance_list,
                             uint32_t count) {
  if (!instance_list)
    return;
  lock();
  for (uint32_t i = 0; i < count; i++) {
    if (slot_valid(&renderer->instance_pool, instance_list[i]))
      pool_free(&renderer->instance_pool, instance_list[i]);
  }
  unlock();
}

static CGF_RenderResult instance_update(const CGF_InstanceUpdate *update_list,
                                        uint32_t count) {
  if (count == 0)
    return CGF_RENDER_OK;
  if (!update_list)
    return CGF_RENDER_ERROR_INVALID_ARGUMENT;
  lock();
  CGF_RenderResult result = CGF_RENDER_OK;
  for (uint32_t i = 0; i < count && result == CGF_RENDER_OK; i++) {
    const CGF_InstanceUpdate *update = &update_list[i];
    if (!slot_valid(&renderer->instance_pool, update->instance))
      result = CGF_RENDER_ERROR_INVALID_HANDLE;
    else if ((update->mask & CGF_RENDER_UPDATE_TRANSFORM) &&
             !transform_finite(update->data.transform))
      result = CGF_RENDER_ERROR_INVALID_ARGUMENT;
    else if ((update->mask & CGF_RENDER_UPDATE_MESH) &&
             !slot_valid(&renderer->mesh_pool, update->data.mesh))
      result = CGF_RENDER_ERROR_INVALID_HANDLE;
    else if ((update->mask & CGF_RENDER_UPDATE_MATERIAL) &&
             !slot_valid(&renderer->material_pool, update->data.material))
      result = CGF_RENDER_ERROR_INVALID_HANDLE;
  }
  if (result == CGF_RENDER_OK) {
    for (uint32_t i = 0; i < count; i++) {
      const CGF_InstanceUpdate *update = &update_list[i];
      CGF_InstanceData *data = &renderer->instances[update->instance.id];
      if (update->mask & CGF_RENDER_UPDATE_TRANSFORM)
        memcpy(data->transform, update->data.transform,
               sizeof(data->transform));
      if (update->mask & CGF_RENDER_UPDATE_MESH)
        data->mesh = update->data.mesh;
      if (update->mask & CGF_RENDER_UPDATE_MATERIAL)
        data->material = update->data.material;
      mark_instance_dirty(renderer, update->instance.id);
    }
  }
  unlock();
  return result;
}

///////////////////////////////////////////////////////////////////////////////
// Meshes
///////////////////////////////////////////////////////////////////////////////

static bool mesh_data_valid(const CGF_MeshData *data) {
  if (!data->vertex_list || !data->index_list || data->vertex_count == 0 ||
      data->index_count == 0 || data->index_count % 3 != 0)
    return false;
  for (uint32_t i = 0; i < data->index_count; i++) {
    if (data->index_list[i] >= data->vertex_count)
      return false;
  }
  for (uint32_t i = 0; i < data->vertex_count; i++) {
    for (int k = 0; k < 3; k++) {
      if (!isfinite(data->vertex_list[i].position[k]))
        return false;
    }
  }
  return true;
}

static void bounding_sphere(const CGF_MeshData *data, float out[4]) {
  float lo[3], hi[3];
  for (int k = 0; k < 3; k++)
    lo[k] = hi[k] = data->vertex_list[0].position[k];
  for (uint32_t i = 1; i < data->vertex_count; i++) {
    for (int k = 0; k < 3; k++) {
      float v = data->vertex_list[i].position[k];
      lo[k] = v < lo[k] ? v : lo[k];
      hi[k] = v > hi[k] ? v : hi[k];
    }
  }
  float radius = 0.0f;
  for (int k = 0; k < 3; k++)
    out[k] = (lo[k] + hi[k]) * 0.5f;
  for (uint32_t i = 0; i < data->vertex_count; i++) {
    const float *p = data->vertex_list[i].position;
    float dx = p[0] - out[0], dy = p[1] - out[1], dz = p[2] - out[2];
    float d = sqrtf(dx * dx + dy * dy + dz * dz);
    radius = d > radius ? d : radius;
  }
  out[3] = radius;
}

static CGF_RenderResult mesh_create(const CGF_MeshData *data_list,
                                    uint32_t count, CGF_Mesh *out_mesh_list) {
  if (count == 0)
    return CGF_RENDER_OK;
  if (!data_list || !out_mesh_list)
    return CGF_RENDER_ERROR_INVALID_ARGUMENT;
  for (uint32_t i = 0; i < count; i++) {
    if (!mesh_data_valid(&data_list[i]))
      return CGF_RENDER_ERROR_INVALID_ARGUMENT;
  }

  /* Copy the data outside the lock. */
  MeshUpload *uploads = calloc(count, sizeof(MeshUpload));
  bool copied = uploads != NULL;
  for (uint32_t i = 0; copied && i < count; i++) {
    const CGF_MeshData *data = &data_list[i];
    uploads[i].vertex_count = data->vertex_count;
    uploads[i].index_count = data->index_count;
    uploads[i].vertices = malloc(sizeof(CGF_Vertex) * data->vertex_count);
    uploads[i].indices = malloc(sizeof(uint32_t) * data->index_count);
    copied = uploads[i].vertices && uploads[i].indices;
    if (copied) {
      memcpy(uploads[i].vertices, data->vertex_list,
             sizeof(CGF_Vertex) * data->vertex_count);
      memcpy(uploads[i].indices, data->index_list,
             sizeof(uint32_t) * data->index_count);
    }
  }

  CGF_RenderResult result = CGF_RENDER_OK;
  if (!copied)
    result = CGF_RENDER_ERROR_OUT_OF_MEMORY;

  if (result == CGF_RENDER_OK) {
    lock();
    RendererState *s = renderer;
    if (pool_available(&s->mesh_pool) < count) {
      result = CGF_RENDER_ERROR_CAPACITY;
    } else if (!grow((void **)&s->mesh_uploads, &s->mesh_upload_capacity,
                     s->mesh_upload_count + count, sizeof(MeshUpload))) {
      result = CGF_RENDER_ERROR_OUT_OF_MEMORY;
    } else {
      uint32_t allocated = 0;
      for (; allocated < count; allocated++) {
        Range vertices, indices;
        if (!range_allocate(&s->vertex_allocator,
                            uploads[allocated].vertex_count, &vertices))
          break;
        if (!range_allocate(&s->index_allocator,
                            uploads[allocated].index_count, &indices)) {
          range_free(&s->vertex_allocator, vertices);
          break;
        }
        uploads[allocated].first_vertex = vertices.first;
        uploads[allocated].first_index = indices.first;
      }
      if (allocated < count) {
        for (uint32_t i = 0; i < allocated; i++) {
          range_free(&s->vertex_allocator,
                     (Range){uploads[i].first_vertex,
                             uploads[i].vertex_count});
          range_free(&s->index_allocator,
                     (Range){uploads[i].first_index, uploads[i].index_count});
        }
        result = CGF_RENDER_ERROR_CAPACITY;
      } else {
        for (uint32_t i = 0; i < count; i++) {
          CGF_Mesh mesh = pool_allocate(&s->mesh_pool);
          s->mesh_records[mesh.id] = (MeshRecord){
              .vertices = {uploads[i].first_vertex, uploads[i].vertex_count},
              .indices = {uploads[i].first_index, uploads[i].index_count},
          };
          GpuMesh *gpu = &s->gpu_meshes[mesh.id];
          *gpu = (GpuMesh){
              .first_index = uploads[i].first_index,
              .index_count = uploads[i].index_count,
              .vertex_offset = (int32_t)uploads[i].first_vertex,
          };
          bounding_sphere(&data_list[i], gpu->sphere);
          s->mesh_uploads[s->mesh_upload_count++] = uploads[i];
          out_mesh_list[i] = mesh;
        }
        s->meshes_dirty = true;
      }
    }
    unlock();
  }

  if (result != CGF_RENDER_OK && uploads) {
    for (uint32_t i = 0; i < count; i++) {
      free(uploads[i].vertices);
      free(uploads[i].indices);
    }
  }
  free(uploads);
  return result;
}

static void mesh_destroy(CGF_Mesh mesh) {
  lock();
  RendererState *s = renderer;
  if (slot_valid(&s->mesh_pool, mesh)) {
    /* GPU copies are ordered after earlier frames, so the ranges can be
       reused right away. */
    range_free(&s->vertex_allocator, s->mesh_records[mesh.id].vertices);
    range_free(&s->index_allocator, s->mesh_records[mesh.id].indices);
    s->mesh_records[mesh.id] = (MeshRecord){0};
    s->gpu_meshes[mesh.id] = (GpuMesh){0};
    s->meshes_dirty = true;
    pool_free(&s->mesh_pool, mesh);
  }
  unlock();
}

///////////////////////////////////////////////////////////////////////////////
// Materials
///////////////////////////////////////////////////////////////////////////////

static bool material_data_valid(const CGF_MaterialData *data) {
  if (data->blend != CGF_MATERIAL_BLEND_OPAQUE &&
      data->blend != CGF_MATERIAL_BLEND_ALPHA)
    return false;
  for (int i = 0; i < 4; i++) {
    if (!isfinite(data->base_color[i]))
      return false;
  }
  for (int i = 0; i < 3; i++) {
    if (!isfinite(data->emissive[i]) || data->emissive[i] < 0.0f)
      return false;
  }
  return true;
}

static CGF_RenderResult material_create(const CGF_MaterialData *data_list,
                                        uint32_t count,
                                        CGF_Material *out_material_list) {
  if (count == 0)
    return CGF_RENDER_OK;
  if (!data_list || !out_material_list)
    return CGF_RENDER_ERROR_INVALID_ARGUMENT;
  for (uint32_t i = 0; i < count; i++) {
    if (!material_data_valid(&data_list[i]))
      return CGF_RENDER_ERROR_INVALID_ARGUMENT;
  }
  lock();
  CGF_RenderResult result = CGF_RENDER_ERROR_CAPACITY;
  if (pool_available(&renderer->material_pool) >= count) {
    for (uint32_t i = 0; i < count; i++) {
      const CGF_MaterialData *data = &data_list[i];
      CGF_Material material = pool_allocate(&renderer->material_pool);
      GpuMaterial *gpu = &renderer->gpu_materials[material.id];
      *gpu = (GpuMaterial){
          .pipeline = (data->blend == CGF_MATERIAL_BLEND_ALPHA ? 2u : 0u) +
                      (data->double_sided ? 1u : 0u),
      };
      memcpy(gpu->base_color, data->base_color, sizeof(gpu->base_color));
      memcpy(gpu->emissive, data->emissive, sizeof(data->emissive));
      out_material_list[i] = material;
    }
    renderer->materials_dirty = true;
    result = CGF_RENDER_OK;
  }
  unlock();
  return result;
}

static void material_destroy(CGF_Material material) {
  lock();
  if (slot_valid(&renderer->material_pool, material)) {
    renderer->gpu_materials[material.id] = (GpuMaterial){0};
    renderer->materials_dirty = true;
    pool_free(&renderer->material_pool, material);
  }
  unlock();
}

///////////////////////////////////////////////////////////////////////////////
// Lights
///////////////////////////////////////////////////////////////////////////////

static bool light_data_valid(const CGF_LightData *data) {
  for (int i = 0; i < 3; i++) {
    if (!isfinite(data->position[i]) || !isfinite(data->color[i]))
      return false;
  }
  return isfinite(data->radius) && data->radius > 0.0f &&
         isfinite(data->intensity) && data->intensity >= 0.0f;
}

static CGF_RenderResult light_create(const CGF_LightData *data_list,
                                     uint32_t count,
                                     CGF_Light *out_light_list) {
  if (count == 0)
    return CGF_RENDER_OK;
  if (!data_list || !out_light_list)
    return CGF_RENDER_ERROR_INVALID_ARGUMENT;
  for (uint32_t i = 0; i < count; i++) {
    if (!light_data_valid(&data_list[i]))
      return CGF_RENDER_ERROR_INVALID_ARGUMENT;
  }
  lock();
  CGF_RenderResult result = CGF_RENDER_ERROR_CAPACITY;
  if (pool_available(&renderer->light_pool) >= count) {
    for (uint32_t i = 0; i < count; i++) {
      CGF_Light light = pool_allocate(&renderer->light_pool);
      renderer->lights[light.id] = data_list[i];
      out_light_list[i] = light;
    }
    result = CGF_RENDER_OK;
  }
  unlock();
  return result;
}

static void light_destroy(const CGF_Light *light_list, uint32_t count) {
  if (!light_list)
    return;
  lock();
  for (uint32_t i = 0; i < count; i++) {
    if (slot_valid(&renderer->light_pool, light_list[i]))
      pool_free(&renderer->light_pool, light_list[i]);
  }
  unlock();
}

static CGF_RenderResult light_update(const CGF_LightUpdate *update_list,
                                     uint32_t count) {
  if (count == 0)
    return CGF_RENDER_OK;
  if (!update_list)
    return CGF_RENDER_ERROR_INVALID_ARGUMENT;
  lock();
  CGF_RenderResult result = CGF_RENDER_OK;
  for (uint32_t i = 0; i < count && result == CGF_RENDER_OK; i++) {
    if (!slot_valid(&renderer->light_pool, update_list[i].light))
      result = CGF_RENDER_ERROR_INVALID_HANDLE;
    else if (!light_data_valid(&update_list[i].data))
      result = CGF_RENDER_ERROR_INVALID_ARGUMENT;
  }
  if (result == CGF_RENDER_OK) {
    for (uint32_t i = 0; i < count; i++)
      renderer->lights[update_list[i].light.id] = update_list[i].data;
  }
  unlock();
  return result;
}

///////////////////////////////////////////////////////////////////////////////
// Plugin
///////////////////////////////////////////////////////////////////////////////

static void renderer_shutdown(void) {
  RendererState *s = renderer;
  if (!s || !s->initialized)
    return;
  vk_shutdown(s);
  cpu_release(s);
  memset(s, 0, sizeof(*s));
}

static const CGF_EngineRendererAPI api = {
    .scene_create = scene_create,
    .scene_destroy = scene_destroy,
    .scene_add = scene_add,
    .scene_remove = scene_remove,
    .scene_add_lights = scene_add_lights,
    .scene_remove_lights = scene_remove_lights,
    .instance_create = instance_create,
    .instance_destroy = instance_destroy,
    .instance_update = instance_update,
    .mesh_create = mesh_create,
    .mesh_destroy = mesh_destroy,
    .material_create = material_create,
    .material_destroy = material_destroy,
    .light_create = light_create,
    .light_destroy = light_destroy,
    .light_update = light_update,
    .frame_begin = renderer_frame_begin,
    .render = renderer_render,
    .frame_end = renderer_frame_end,
};

static const CGF_EngineRendererRuntimeAPI runtime_api = {
    .shutdown = renderer_shutdown,
};

static bool publish(const CGF_APIRegistry *registry) {
  return registry->set(CGF_ENGINE_RENDERER_API_NAME, sizeof(api), &api) &&
         registry->set(CGF_ENGINE_RENDERER_RUNTIME_API_NAME,
                       sizeof(runtime_api), &runtime_api);
}

static DebugMode read_debug_mode(void) {
  char value[16];
  DWORD length = GetEnvironmentVariableA("CGF_RENDERER_DEBUG", value,
                                         (DWORD)sizeof(value));
  if (length == 0 || length >= sizeof(value))
    return DEBUG_MODE_NONE;
  if (strcmp(value, "direct") == 0)
    return DEBUG_MODE_DIRECT;
  if (strcmp(value, "indirect") == 0)
    return DEBUG_MODE_INDIRECT;
  if (strcmp(value, "lights") == 0)
    return DEBUG_MODE_LIGHTS;
  return DEBUG_MODE_NONE;
}

static bool first_load(const CGF_APIRegistry *registry, RendererState *s,
                       const CGF_EngineAPI *engine) {
  s->engine = engine;
  s->debug_mode = read_debug_mode();
  InitializeSRWLock(&s->lock);
  renderer = s;
  if (!cpu_init(s) || !vk_initialize(s) || !publish(registry)) {
    vk_shutdown(s);
    cpu_release(s);
    memset(s, 0, sizeof(*s));
    renderer = NULL;
    return false;
  }
  s->initialized = true;
  puts("[RENDERER] initialized");
  return true;
}

/* Reconnects this DLL to the existing device and rebuilds the pipelines from
   its shaders. Everything that can fail happens before the state changes. */
static bool reload(const CGF_APIRegistry *registry, RendererState *s,
                   const CGF_EngineAPI *engine) {
  if (s->in_frame || volkInitialize() != VK_SUCCESS)
    return false;
  volkLoadInstanceOnly(s->instance);
  volkLoadDevice(s->device);
  vkDeviceWaitIdle(s->device);

  Pipelines pipelines;
  if (!vk_create_pipelines(s, &pipelines))
    return false;
  if (!publish(registry)) {
    vk_destroy_pipelines(s, &pipelines);
    return false;
  }

  vk_destroy_pipelines(s, &s->pipelines);
  s->pipelines = pipelines;
  /* The debug callback lives in the DLL; point the messenger at this one. */
  if (s->messenger) {
    vkDestroyDebugUtilsMessengerEXT(s->instance, s->messenger, NULL);
    vk_create_messenger(s, &s->messenger);
  }
  s->engine = engine;
  s->debug_mode = read_debug_mode();
  renderer = s;
  puts("[RENDERER] reloaded");
  return true;
}

CGF_PLUGIN_EXPORT bool cgf_renderer_load(const CGF_APIRegistry *registry) {
  if (!registry)
    return false;
  const CGF_EngineAPI *engine =
      registry->get(CGF_ENGINE_API_NAME, sizeof(CGF_EngineAPI));
  if (!engine || engine->struct_size != sizeof(CGF_EngineAPI) ||
      !engine->window_size || !engine->native_window)
    return false;

  bool created = false;
  RendererState *s = registry->get_or_create_state(
      STATE_NAME, sizeof(RendererState), _Alignof(RendererState), &created);
  if (!s)
    return false;
  return s->initialized ? reload(registry, s, engine)
                        : first_load(registry, s, engine);
}
