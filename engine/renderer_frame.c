#include "renderer_internal.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define UPLOAD_ALIGNMENT 256u

static VkDeviceSize align_up(VkDeviceSize value) {
  return (value + UPLOAD_ALIGNMENT - 1) & ~(VkDeviceSize)(UPLOAD_ALIGNMENT - 1);
}

///////////////////////////////////////////////////////////////////////////////
// Matrices (column-major 4x4)
///////////////////////////////////////////////////////////////////////////////

static void mat4_multiply(const float a[16], const float b[16], float out[16]) {
  float result[16];
  for (int column = 0; column < 4; column++) {
    for (int row = 0; row < 4; row++) {
      float sum = 0.0f;
      for (int k = 0; k < 4; k++)
        sum += a[k * 4 + row] * b[column * 4 + k];
      result[column * 4 + row] = sum;
    }
  }
  memcpy(out, result, sizeof(result));
}

static bool mat4_inverse(const float m[16], float out[16]) {
  float inv[16];
  inv[0] = m[5] * m[10] * m[15] - m[5] * m[11] * m[14] - m[9] * m[6] * m[15] +
           m[9] * m[7] * m[14] + m[13] * m[6] * m[11] - m[13] * m[7] * m[10];
  inv[4] = -m[4] * m[10] * m[15] + m[4] * m[11] * m[14] + m[8] * m[6] * m[15] -
           m[8] * m[7] * m[14] - m[12] * m[6] * m[11] + m[12] * m[7] * m[10];
  inv[8] = m[4] * m[9] * m[15] - m[4] * m[11] * m[13] - m[8] * m[5] * m[15] +
           m[8] * m[7] * m[13] + m[12] * m[5] * m[11] - m[12] * m[7] * m[9];
  inv[12] = -m[4] * m[9] * m[14] + m[4] * m[10] * m[13] + m[8] * m[5] * m[14] -
            m[8] * m[6] * m[13] - m[12] * m[5] * m[10] + m[12] * m[6] * m[9];
  inv[1] = -m[1] * m[10] * m[15] + m[1] * m[11] * m[14] + m[9] * m[2] * m[15] -
           m[9] * m[3] * m[14] - m[13] * m[2] * m[11] + m[13] * m[3] * m[10];
  inv[5] = m[0] * m[10] * m[15] - m[0] * m[11] * m[14] - m[8] * m[2] * m[15] +
           m[8] * m[3] * m[14] + m[12] * m[2] * m[11] - m[12] * m[3] * m[10];
  inv[9] = -m[0] * m[9] * m[15] + m[0] * m[11] * m[13] + m[8] * m[1] * m[15] -
           m[8] * m[3] * m[13] - m[12] * m[1] * m[11] + m[12] * m[3] * m[9];
  inv[13] = m[0] * m[9] * m[14] - m[0] * m[10] * m[13] - m[8] * m[1] * m[14] +
            m[8] * m[2] * m[13] + m[12] * m[1] * m[10] - m[12] * m[2] * m[9];
  inv[2] = m[1] * m[6] * m[15] - m[1] * m[7] * m[14] - m[5] * m[2] * m[15] +
           m[5] * m[3] * m[14] + m[13] * m[2] * m[7] - m[13] * m[3] * m[6];
  inv[6] = -m[0] * m[6] * m[15] + m[0] * m[7] * m[14] + m[4] * m[2] * m[15] -
           m[4] * m[3] * m[14] - m[12] * m[2] * m[7] + m[12] * m[3] * m[6];
  inv[10] = m[0] * m[5] * m[15] - m[0] * m[7] * m[13] - m[4] * m[1] * m[15] +
            m[4] * m[3] * m[13] + m[12] * m[1] * m[7] - m[12] * m[3] * m[5];
  inv[14] = -m[0] * m[5] * m[14] + m[0] * m[6] * m[13] + m[4] * m[1] * m[14] -
            m[4] * m[2] * m[13] - m[12] * m[1] * m[6] + m[12] * m[2] * m[5];
  inv[3] = -m[1] * m[6] * m[11] + m[1] * m[7] * m[10] + m[5] * m[2] * m[11] -
           m[5] * m[3] * m[10] - m[9] * m[2] * m[7] + m[9] * m[3] * m[6];
  inv[7] = m[0] * m[6] * m[11] - m[0] * m[7] * m[10] - m[4] * m[2] * m[11] +
           m[4] * m[3] * m[10] + m[8] * m[2] * m[7] - m[8] * m[3] * m[6];
  inv[11] = -m[0] * m[5] * m[11] + m[0] * m[7] * m[9] + m[4] * m[1] * m[11] -
            m[4] * m[3] * m[9] - m[8] * m[1] * m[7] + m[8] * m[3] * m[5];
  inv[15] = m[0] * m[5] * m[10] - m[0] * m[6] * m[9] - m[4] * m[1] * m[10] +
            m[4] * m[2] * m[9] + m[8] * m[1] * m[6] - m[8] * m[2] * m[5];
  float det = m[0] * inv[0] + m[1] * inv[4] + m[2] * inv[8] + m[3] * inv[12];
  if (!isfinite(det) || fabsf(det) < 1e-20f)
    return false;
  for (int i = 0; i < 16; i++)
    out[i] = inv[i] / det;
  return true;
}

/* Inverse transpose of the upper 3x3, for transforming normals. */
static void normal_matrix(const float m[16], float out[16]) {
  float a = m[0], b = m[4], c = m[8];
  float d = m[1], e = m[5], f = m[9];
  float g = m[2], h = m[6], i = m[10];
  float co00 = e * i - f * h, co01 = -(d * i - f * g), co02 = d * h - e * g;
  float co10 = -(b * i - c * h), co11 = a * i - c * g, co12 = -(a * h - b * g);
  float co20 = b * f - c * e, co21 = -(a * f - c * d), co22 = a * e - b * d;
  float det = a * co00 + b * co01 + c * co02;
  float s = fabsf(det) > 1e-20f ? 1.0f / det : 0.0f;
  /* (M^-1)^T is the cofactor matrix divided by det. Element (row, col) goes
     to out[col * 4 + row]. */
  float result[16] = {co00 * s, co10 * s, co20 * s, 0.0f,
                      co01 * s, co11 * s, co21 * s, 0.0f,
                      co02 * s, co12 * s, co22 * s, 0.0f,
                      0.0f,     0.0f,     0.0f,     1.0f};
  memcpy(out, result, sizeof(result));
}

static void frustum_planes(const float m[16], float planes[6][4]) {
  /* Rows of the view-projection matrix; clip depth is [0, w]. */
  float row[4][4];
  for (int r = 0; r < 4; r++) {
    for (int c = 0; c < 4; c++)
      row[r][c] = m[c * 4 + r];
  }
  for (int c = 0; c < 4; c++) {
    planes[0][c] = row[3][c] + row[0][c];
    planes[1][c] = row[3][c] - row[0][c];
    planes[2][c] = row[3][c] + row[1][c];
    planes[3][c] = row[3][c] - row[1][c];
    planes[4][c] = row[2][c];
    planes[5][c] = row[3][c] - row[2][c];
  }
  for (int p = 0; p < 6; p++) {
    float length = sqrtf(planes[p][0] * planes[p][0] +
                         planes[p][1] * planes[p][1] +
                         planes[p][2] * planes[p][2]);
    if (length > 0.0f) {
      for (int c = 0; c < 4; c++)
        planes[p][c] /= length;
    }
  }
}

///////////////////////////////////////////////////////////////////////////////
// Command helpers
///////////////////////////////////////////////////////////////////////////////

static void memory_barrier(VkCommandBuffer cmd, VkPipelineStageFlags2 src_stage,
                           VkAccessFlags2 src_access,
                           VkPipelineStageFlags2 dst_stage,
                           VkAccessFlags2 dst_access) {
  VkMemoryBarrier2 barrier = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
      .srcStageMask = src_stage,
      .srcAccessMask = src_access,
      .dstStageMask = dst_stage,
      .dstAccessMask = dst_access,
  };
  VkDependencyInfo dependency = {
      .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
      .memoryBarrierCount = 1,
      .pMemoryBarriers = &barrier,
  };
  vkCmdPipelineBarrier2(cmd, &dependency);
}

static VkImageMemoryBarrier2 image_barrier(VkImage image,
                                           VkImageAspectFlags aspect,
                                           VkImageLayout old_layout,
                                           VkImageLayout new_layout) {
  return (VkImageMemoryBarrier2){
      .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
      .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
      .srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT,
      .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
      .dstAccessMask =
          VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
      .oldLayout = old_layout,
      .newLayout = new_layout,
      .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .image = image,
      .subresourceRange = {aspect, 0, 1, 0, 1},
  };
}

static void image_barriers(VkCommandBuffer cmd,
                           const VkImageMemoryBarrier2 *barriers,
                           uint32_t count) {
  VkDependencyInfo dependency = {
      .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
      .imageMemoryBarrierCount = count,
      .pImageMemoryBarriers = barriers,
  };
  vkCmdPipelineBarrier2(cmd, &dependency);
}

static VkRenderingAttachmentInfo color_attachment(VkImageView view,
                                                  VkImageLayout layout,
                                                  bool clear,
                                                  const float color[4]) {
  VkRenderingAttachmentInfo attachment = {
      .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
      .imageView = view,
      .imageLayout = layout,
      .loadOp = clear ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD,
      .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
  };
  if (clear && color)
    memcpy(attachment.clearValue.color.float32, color, sizeof(float) * 4);
  return attachment;
}

static void begin_rendering(RendererState *s, VkCommandBuffer cmd,
                            const VkRenderingAttachmentInfo *colors,
                            uint32_t color_count,
                            const VkRenderingAttachmentInfo *depth) {
  VkRenderingInfo info = {
      .sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
      .renderArea = {{0, 0}, s->extent},
      .layerCount = 1,
      .colorAttachmentCount = color_count,
      .pColorAttachments = colors,
      .pDepthAttachment = depth,
  };
  vkCmdBeginRendering(cmd, &info);
  VkViewport viewport = {0.0f, 0.0f, (float)s->extent.width,
                         (float)s->extent.height, 0.0f, 1.0f};
  VkRect2D scissor = {{0, 0}, s->extent};
  vkCmdSetViewport(cmd, 0, 1, &viewport);
  vkCmdSetScissor(cmd, 0, 1, &scissor);
}

static uint32_t groups(uint32_t count, uint32_t size) {
  return (count + size - 1) / size;
}

///////////////////////////////////////////////////////////////////////////////
// frame_begin
///////////////////////////////////////////////////////////////////////////////

/* Bytes of upload space needed for this frame's snapshot. Caller holds the
   lock. */
static VkDeviceSize upload_size(const RendererState *s) {
  VkDeviceSize entries = 0;
  VkDeviceSize lights = 0;
  for (uint32_t i = 0; i < s->scene_pool.used; i++) {
    if (s->scene_pool.generations[i]) {
      entries += s->scenes[i].instance_count;
      lights += s->scenes[i].light_count;
    }
  }
  VkDeviceSize staging = (VkDeviceSize)s->instance_dirty_count *
                         sizeof(GpuInstance);
  if (s->meshes_dirty)
    staging += (VkDeviceSize)s->mesh_pool.used * sizeof(GpuMesh);
  if (s->materials_dirty)
    staging += (VkDeviceSize)s->material_pool.used * sizeof(GpuMaterial);
  for (uint32_t i = 0; i < s->mesh_upload_count; i++) {
    staging += (VkDeviceSize)s->mesh_uploads[i].vertex_count *
                   sizeof(CGF_Vertex) +
               (VkDeviceSize)s->mesh_uploads[i].index_count * sizeof(uint32_t);
  }
  return align_up(MAX_RENDERS_PER_FRAME * sizeof(GpuRenderData)) +
         align_up(entries * sizeof(uint32_t)) +
         align_up(lights * sizeof(GpuLight)) + align_up(staging) +
         UPLOAD_ALIGNMENT;
}

/* Packs the scene lists into the upload buffer, dropping destroyed
   instances and lights from the scenes. */
static void snapshot_scenes(RendererState *s, uint8_t *upload) {
  uint32_t *entries = (uint32_t *)(upload + s->entries_offset);
  GpuLight *lights = (GpuLight *)(upload + s->lights_offset);
  uint32_t entry_cursor = 0;
  uint32_t light_cursor = 0;

  for (uint32_t i = 0; i < s->scene_pool.used; i++) {
    SceneRecord *scene = &s->scenes[i];
    scene->frame_generation = s->scene_pool.generations[i];
    scene->entry_offset = entry_cursor;
    scene->light_offset = light_cursor;
    scene->entry_count = 0;
    scene->frame_light_count = 0;
    if (!scene->frame_generation)
      continue;

    for (uint32_t k = 0; k < scene->instance_count;) {
      CGF_Instance instance = scene->instances[k];
      if (!slot_valid(&s->instance_pool, instance)) {
        scene->instances[k] = scene->instances[--scene->instance_count];
        continue;
      }
      if (renderer_instance_drawable(s, instance.id) &&
          scene->entry_count < MAX_INSTANCES) {
        entries[entry_cursor++] = instance.id;
        scene->entry_count++;
      }
      k++;
    }

    for (uint32_t k = 0; k < scene->light_count;) {
      CGF_Light light = scene->lights[k];
      if (!slot_valid(&s->light_pool, light)) {
        scene->lights[k] = scene->lights[--scene->light_count];
        continue;
      }
      const CGF_LightData *data = &s->lights[light.id];
      GpuLight *gpu = &lights[light_cursor++];
      memcpy(gpu->position_radius, data->position, sizeof(float) * 3);
      gpu->position_radius[3] = data->radius;
      for (int c = 0; c < 3; c++)
        gpu->color[c] = data->color[c] * data->intensity;
      gpu->color[3] = 0.0f;
      scene->frame_light_count++;
      k++;
    }
  }
}

typedef struct CopyList {
  VkBufferCopy *regions;
  uint32_t count;
  uint32_t capacity;
} CopyList;

static bool copy_add(CopyList *list, VkDeviceSize src, VkDeviceSize dst,
                     VkDeviceSize size) {
  if (list->count > 0) {
    VkBufferCopy *last = &list->regions[list->count - 1];
    if (last->srcOffset + last->size == src &&
        last->dstOffset + last->size == dst) {
      last->size += size;
      return true;
    }
  }
  if (list->count == list->capacity) {
    uint32_t capacity = list->capacity ? list->capacity * 2 : 64;
    VkBufferCopy *grown = realloc(list->regions, sizeof(VkBufferCopy) * capacity);
    if (!grown)
      return false;
    list->regions = grown;
    list->capacity = capacity;
  }
  list->regions[list->count++] = (VkBufferCopy){src, dst, size};
  return true;
}

/* Copies the changed CPU data to the device-local buffers. */
static bool record_uploads(RendererState *s, VkCommandBuffer cmd,
                           Buffer *upload, VkDeviceSize staging_offset) {
  uint8_t *base = upload->mapped;
  VkDeviceSize cursor = staging_offset;
  CopyList instance_copies = {0}, vertex_copies = {0}, index_copies = {0};
  VkBufferCopy mesh_copy = {0}, material_copy = {0};
  bool ok = true;

  for (uint32_t i = 0; i < s->instance_dirty_count && ok; i++) {
    uint32_t slot = s->instance_dirty_list[i];
    s->instance_dirty[slot] = 0;
    const CGF_InstanceData *data = &s->instances[slot];
    GpuInstance gpu = {.mesh = data->mesh.id, .material = data->material.id};
    memcpy(gpu.transform, data->transform, sizeof(gpu.transform));
    normal_matrix(data->transform, gpu.normal_transform);
    memcpy(base + cursor, &gpu, sizeof(gpu));
    ok = copy_add(&instance_copies, cursor,
                  (VkDeviceSize)slot * sizeof(GpuInstance), sizeof(gpu));
    cursor += sizeof(gpu);
  }
  s->instance_dirty_count = 0;

  if (s->meshes_dirty && s->mesh_pool.used) {
    VkDeviceSize size = (VkDeviceSize)s->mesh_pool.used * sizeof(GpuMesh);
    memcpy(base + cursor, s->gpu_meshes, size);
    mesh_copy = (VkBufferCopy){cursor, 0, size};
    cursor += size;
  }
  s->meshes_dirty = false;

  if (s->materials_dirty && s->material_pool.used) {
    VkDeviceSize size =
        (VkDeviceSize)s->material_pool.used * sizeof(GpuMaterial);
    memcpy(base + cursor, s->gpu_materials, size);
    material_copy = (VkBufferCopy){cursor, 0, size};
    cursor += size;
  }
  s->materials_dirty = false;

  for (uint32_t i = 0; i < s->mesh_upload_count; i++) {
    MeshUpload *mesh = &s->mesh_uploads[i];
    VkDeviceSize vertex_bytes = (VkDeviceSize)mesh->vertex_count *
                                sizeof(CGF_Vertex);
    VkDeviceSize index_bytes = (VkDeviceSize)mesh->index_count *
                               sizeof(uint32_t);
    memcpy(base + cursor, mesh->vertices, vertex_bytes);
    ok = ok && copy_add(&vertex_copies, cursor,
                        (VkDeviceSize)mesh->first_vertex * sizeof(CGF_Vertex),
                        vertex_bytes);
    cursor += vertex_bytes;
    memcpy(base + cursor, mesh->indices, index_bytes);
    ok = ok && copy_add(&index_copies, cursor,
                        (VkDeviceSize)mesh->first_index * sizeof(uint32_t),
                        index_bytes);
    cursor += index_bytes;
    free(mesh->vertices);
    free(mesh->indices);
  }
  s->mesh_upload_count = 0;

  bool any = instance_copies.count || vertex_copies.count ||
             index_copies.count || mesh_copy.size || material_copy.size;
  if (ok && any) {
    /* Earlier frames may still read these buffers. */
    memory_barrier(cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                   VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
                   VK_PIPELINE_STAGE_2_COPY_BIT,
                   VK_ACCESS_2_TRANSFER_WRITE_BIT);
    if (instance_copies.count)
      vkCmdCopyBuffer(cmd, upload->buffer, s->instance_buffer.buffer,
                      instance_copies.count, instance_copies.regions);
    if (vertex_copies.count)
      vkCmdCopyBuffer(cmd, upload->buffer, s->vertices.buffer,
                      vertex_copies.count, vertex_copies.regions);
    if (index_copies.count)
      vkCmdCopyBuffer(cmd, upload->buffer, s->indices.buffer,
                      index_copies.count, index_copies.regions);
    if (mesh_copy.size)
      vkCmdCopyBuffer(cmd, upload->buffer, s->mesh_buffer.buffer, 1,
                      &mesh_copy);
    if (material_copy.size)
      vkCmdCopyBuffer(cmd, upload->buffer, s->material_buffer.buffer, 1,
                      &material_copy);
    memory_barrier(cmd, VK_PIPELINE_STAGE_2_COPY_BIT,
                   VK_ACCESS_2_TRANSFER_WRITE_BIT,
                   VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                   VK_ACCESS_2_MEMORY_READ_BIT);
  }
  free(instance_copies.regions);
  free(vertex_copies.regions);
  free(index_copies.regions);
  return ok;
}

static bool ensure_upload_capacity(RendererState *s, Frame *frame,
                                   VkDeviceSize needed) {
  if (frame->upload.size >= needed)
    return true;
  VkDeviceSize size = frame->upload.size ? frame->upload.size : needed;
  while (size < needed)
    size *= 2;
  /* This frame's previous submission has completed (its fence was waited). */
  vk_destroy_buffer(s, &frame->upload);
  return vk_create_buffer(s, size,
                          VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                              VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                          true, &frame->upload);
}

static const char *const profile_labels[PROFILE_QUERY_COUNT] = {
    "total",   "cull",     "prepass",     "light cull", "forward",
    "cascades", "gi apply", "transparent", "tonemap",
};

/* Adds the timings of this frame slot's previous submission, which has
   completed, and prints averages every 120 frames. */
static void collect_profile(RendererState *s) {
  uint32_t slot = s->frame_index;
  if (!s->profile || !s->queries_written[slot])
    return;
  s->queries_written[slot] = false;
  uint64_t times[PROFILE_QUERY_COUNT];
  if (vkGetQueryPoolResults(s->device, s->query_pool,
                            slot * PROFILE_QUERY_COUNT, PROFILE_QUERY_COUNT,
                            sizeof(times), times, sizeof(uint64_t),
                            VK_QUERY_RESULT_64_BIT) != VK_SUCCESS)
    return;
  double to_ms = s->timestamp_period / 1.0e6;
  for (uint32_t i = 1; i < PROFILE_QUERY_COUNT; i++)
    s->profile_sums[i] += (double)(times[i] - times[i - 1]) * to_ms;
  s->profile_sums[0] +=
      (double)(times[PROFILE_QUERY_COUNT - 1] - times[0]) * to_ms;
  if (++s->profile_frames < 120)
    return;
  char line[512];
  int length = snprintf(line, sizeof(line), "[RENDERER] GPU ms:");
  for (uint32_t i = 0; i < PROFILE_QUERY_COUNT && length > 0 &&
                       (size_t)length < sizeof(line);
       i++) {
    length += snprintf(line + length, sizeof(line) - (size_t)length,
                       " %s %.2f", profile_labels[i],
                       s->profile_sums[i] / s->profile_frames);
    s->profile_sums[i] = 0.0;
  }
  puts(line);
  fflush(stdout);
  s->profile_frames = 0;
}

static void timestamp(RendererState *s, VkCommandBuffer cmd, bool timed,
                      uint32_t index) {
  if (timed)
    vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                         s->query_pool,
                         s->frame_index * PROFILE_QUERY_COUNT + index);
}

static void initialize_targets(RendererState *s, VkCommandBuffer cmd) {
  VkImageMemoryBarrier2 barriers[7 + GI_CASCADE_COUNT];
  uint32_t count = 0;
  barriers[count++] =
      image_barrier(s->depth.image, VK_IMAGE_ASPECT_DEPTH_BIT,
                    VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
  Image *colors[] = {&s->normal, &s->albedo, &s->hdr};
  for (uint32_t i = 0; i < 3; i++)
    barriers[count++] =
        image_barrier(colors[i]->image, VK_IMAGE_ASPECT_COLOR_BIT,
                      VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
  for (uint32_t c = 0; c < GI_CASCADE_COUNT; c++)
    barriers[count++] =
        image_barrier(s->cascades[c].image, VK_IMAGE_ASPECT_COLOR_BIT,
                      VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
  barriers[count] =
      image_barrier(s->pyramid, VK_IMAGE_ASPECT_COLOR_BIT,
                    VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
  barriers[count++].subresourceRange.levelCount = GI_PYRAMID_LEVELS;
  image_barriers(cmd, barriers, count);
  s->targets_need_init = false;
}

CGF_RenderResult renderer_frame_begin(const float clear_color[4]) {
  RendererState *s = renderer;
  if (!clear_color)
    return CGF_RENDER_ERROR_INVALID_ARGUMENT;
  for (int i = 0; i < 4; i++) {
    if (!isfinite(clear_color[i]))
      return CGF_RENDER_ERROR_INVALID_ARGUMENT;
  }
  if (s->in_frame)
    return CGF_RENDER_ERROR_INVALID_STATE;

  uint32_t width = 0;
  uint32_t height = 0;
  s->engine->window_size(&width, &height);
  if (width == 0 || height == 0)
    return CGF_RENDER_NOT_VISIBLE;
  if (s->swapchain_dirty || !s->swapchain || width != s->extent.width ||
      height != s->extent.height) {
    if (!vk_resize(s, width, height)) {
      s->swapchain_dirty = true;
      return CGF_RENDER_NOT_VISIBLE;
    }
  }

  Frame *frame = &s->frames[s->frame_index];
  if (vkWaitForFences(s->device, 1, &frame->fence, VK_TRUE, UINT64_MAX) !=
      VK_SUCCESS)
    return CGF_RENDER_ERROR_DEVICE;

  AcquireSRWLockExclusive(&s->lock);
  VkDeviceSize render_bytes =
      align_up(MAX_RENDERS_PER_FRAME * sizeof(GpuRenderData));
  if (!ensure_upload_capacity(s, frame, upload_size(s))) {
    ReleaseSRWLockExclusive(&s->lock);
    return CGF_RENDER_ERROR_OUT_OF_MEMORY;
  }

  VkResult acquired =
      vkAcquireNextImageKHR(s->device, s->swapchain, UINT64_MAX,
                            frame->image_available, VK_NULL_HANDLE,
                            &s->image_index);
  if (acquired == VK_ERROR_OUT_OF_DATE_KHR) {
    s->swapchain_dirty = true;
    ReleaseSRWLockExclusive(&s->lock);
    return CGF_RENDER_NOT_VISIBLE;
  }
  if (acquired != VK_SUCCESS && acquired != VK_SUBOPTIMAL_KHR) {
    ReleaseSRWLockExclusive(&s->lock);
    return CGF_RENDER_ERROR_DEVICE;
  }
  if (acquired == VK_SUBOPTIMAL_KHR)
    s->swapchain_dirty = true;

  vkResetFences(s->device, 1, &frame->fence);
  VkCommandBuffer cmd = frame->command_buffer;
  vkResetCommandBuffer(cmd, 0);
  VkCommandBufferBeginInfo begin = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
      .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
  };
  vkBeginCommandBuffer(cmd, &begin);
  collect_profile(s);
  if (s->profile)
    vkCmdResetQueryPool(cmd, s->query_pool,
                        s->frame_index * PROFILE_QUERY_COUNT,
                        PROFILE_QUERY_COUNT);

  /* Snapshot: everything changed before this point is drawn this frame. */
  VkDeviceSize entry_bytes = 0;
  VkDeviceSize light_bytes = 0;
  for (uint32_t i = 0; i < s->scene_pool.used; i++) {
    if (s->scene_pool.generations[i]) {
      entry_bytes += (VkDeviceSize)s->scenes[i].instance_count *
                     sizeof(uint32_t);
      light_bytes += (VkDeviceSize)s->scenes[i].light_count * sizeof(GpuLight);
    }
  }
  s->entries_offset = render_bytes;
  s->lights_offset = s->entries_offset + align_up(entry_bytes);
  VkDeviceSize staging_offset = s->lights_offset + align_up(light_bytes);
  snapshot_scenes(s, frame->upload.mapped);
  bool uploaded = record_uploads(s, cmd, &frame->upload, staging_offset);
  s->frame_mesh_count = s->mesh_pool.used;
  ReleaseSRWLockExclusive(&s->lock);
  if (!uploaded)
    fprintf(stderr, "[RENDERER] out of memory while uploading\n");

  if (s->targets_need_init)
    initialize_targets(s, cmd);

  VkImageMemoryBarrier2 to_attachment = image_barrier(
      s->images[s->image_index], VK_IMAGE_ASPECT_COLOR_BIT,
      VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
  to_attachment.srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
  to_attachment.srcAccessMask = VK_ACCESS_2_NONE;
  to_attachment.dstStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
  to_attachment.dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT |
                                VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT;
  image_barriers(cmd, &to_attachment, 1);

  VkRenderingAttachmentInfo target =
      color_attachment(s->image_views[s->image_index],
                       VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, true,
                       clear_color);
  begin_rendering(s, cmd, &target, 1, NULL);
  vkCmdEndRendering(cmd);

  memcpy(s->sky, clear_color, sizeof(s->sky));
  s->render_count = 0;
  s->in_frame = true;
  return CGF_RENDER_OK;
}

///////////////////////////////////////////////////////////////////////////////
// render
///////////////////////////////////////////////////////////////////////////////

static void draw_kind(RendererState *s, VkCommandBuffer cmd, uint32_t kind) {
  if (s->frame_mesh_count == 0)
    return;
  vkCmdDrawIndexedIndirect(cmd, s->draws.buffer,
                           (VkDeviceSize)kind * MAX_MESHES *
                               GPU_DRAW_COMMAND_SIZE,
                           s->frame_mesh_count, GPU_DRAW_COMMAND_SIZE);
}

static void push(RendererState *s, VkCommandBuffer cmd, VkDeviceAddress data,
                 uint32_t cascade) {
  GpuPushConstants constants = {.render = data, .cascade = cascade};
  vkCmdPushConstants(cmd, s->pipeline_layout, VK_SHADER_STAGE_ALL, 0,
                     sizeof(constants), &constants);
}

static void compute_to_compute(VkCommandBuffer cmd) {
  memory_barrier(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                 VK_ACCESS_2_SHADER_WRITE_BIT,
                 VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                 VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT);
}

CGF_RenderResult renderer_render(CGF_Scene scene, const CGF_Camera *camera) {
  RendererState *s = renderer;
  if (!s->in_frame)
    return CGF_RENDER_ERROR_INVALID_STATE;
  if (!camera)
    return CGF_RENDER_ERROR_INVALID_ARGUMENT;
  if (scene.gen == 0 || scene.id >= s->scene_pool.used ||
      s->scenes[scene.id].frame_generation != scene.gen)
    return CGF_RENDER_ERROR_INVALID_HANDLE;
  if (s->render_count >= MAX_RENDERS_PER_FRAME)
    return CGF_RENDER_ERROR_CAPACITY;

  const SceneRecord *record = &s->scenes[scene.id];
  Frame *frame = &s->frames[s->frame_index];
  GpuRenderData data = {0};
  memcpy(data.view, camera->view, sizeof(data.view));
  memcpy(data.projection, camera->projection, sizeof(data.projection));
  mat4_multiply(camera->projection, camera->view, data.view_projection);
  float inverse_view[16];
  if (!mat4_inverse(data.view_projection, data.inverse_view_projection) ||
      !mat4_inverse(camera->projection, data.inverse_projection) ||
      !mat4_inverse(camera->view, inverse_view))
    return CGF_RENDER_ERROR_INVALID_ARGUMENT;
  data.camera_position[0] = inverse_view[12];
  data.camera_position[1] = inverse_view[13];
  data.camera_position[2] = inverse_view[14];
  data.camera_position[3] = 1.0f;
  memcpy(data.sky, s->sky, sizeof(data.sky));
  frustum_planes(data.view_projection, data.frustum);
  data.screen_size[0] = s->extent.width;
  data.screen_size[1] = s->extent.height;
  data.tile_count[0] = groups(s->extent.width, TILE_SIZE);
  data.tile_count[1] = groups(s->extent.height, TILE_SIZE);
  data.entry_count = record->entry_count;
  data.light_count = record->frame_light_count;
  data.mesh_count = s->frame_mesh_count;
  data.mesh_capacity = MAX_MESHES;
  data.cascade_count = GI_CASCADE_COUNT;
  data.gi_interval = GI_INTERVAL;
  data.debug_mode = s->debug_mode;
  data.instances = s->instance_buffer.address;
  data.meshes = s->mesh_buffer.address;
  data.materials = s->material_buffer.address;
  data.entries = frame->upload.address + s->entries_offset +
                 (VkDeviceSize)record->entry_offset * sizeof(uint32_t);
  data.visible = s->visible.address;
  data.draws = s->draws.address;
  data.lights = frame->upload.address + s->lights_offset +
                (VkDeviceSize)record->light_offset * sizeof(GpuLight);
  data.tile_lights = s->tile_lights.address;
  data.probes = s->probes.address;

  VkDeviceSize data_offset =
      (VkDeviceSize)s->render_count * sizeof(GpuRenderData);
  memcpy((uint8_t *)frame->upload.mapped + data_offset, &data, sizeof(data));
  VkDeviceAddress data_address = frame->upload.address + data_offset;
  bool timed = s->profile && s->render_count == 0;
  if (timed)
    s->queries_written[s->frame_index] = true;
  s->render_count++;

  VkCommandBuffer cmd = frame->command_buffer;
  const Pipelines *p = &s->pipelines;
  vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                          s->pipeline_layout, 0, 1, &s->descriptor_set, 0,
                          NULL);
  vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                          s->pipeline_layout, 0, 1, &s->descriptor_set, 0,
                          NULL);
  push(s, cmd, data_address, 0);

  /* Previous renders may still use the shared buffers and images. */
  memory_barrier(cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                 VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
                 VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                 VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT);

  timestamp(s, cmd, timed, 0);

  /* GPU culling: build one indirect draw per (pipeline, mesh). */
  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, p->cull_reset);
  vkCmdDispatch(cmd, groups(PIPELINE_KIND_COUNT * data.mesh_count, 64), 1, 1);
  compute_to_compute(cmd);
  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, p->cull_count);
  vkCmdDispatch(cmd, groups(data.entry_count, 64), 1, 1);
  compute_to_compute(cmd);
  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, p->cull_prefix);
  vkCmdDispatch(cmd, 1, 1, 1);
  compute_to_compute(cmd);
  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, p->cull_write);
  vkCmdDispatch(cmd, groups(data.entry_count, 64), 1, 1);
  memory_barrier(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                 VK_ACCESS_2_SHADER_WRITE_BIT,
                 VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT |
                     VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT,
                 VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT |
                     VK_ACCESS_2_SHADER_READ_BIT);

  timestamp(s, cmd, timed, 1);

  VkDeviceSize zero = 0;
  vkCmdBindVertexBuffers(cmd, 0, 1, &s->vertices.buffer, &zero);
  vkCmdBindIndexBuffer(cmd, s->indices.buffer, 0, VK_INDEX_TYPE_UINT32);

  /* Depth prepass with normals and albedo (opaque only). */
  const float zero_color[4] = {0.0f, 0.0f, 0.0f, 0.0f};
  VkRenderingAttachmentInfo prepass_colors[2] = {
      color_attachment(s->normal.view, VK_IMAGE_LAYOUT_GENERAL, true,
                       zero_color),
      color_attachment(s->albedo.view, VK_IMAGE_LAYOUT_GENERAL, true,
                       zero_color),
  };
  VkRenderingAttachmentInfo depth_clear = {
      .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
      .imageView = s->depth.view,
      .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
      .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
      .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
      .clearValue.depthStencil = {1.0f, 0},
  };
  begin_rendering(s, cmd, prepass_colors, 2, &depth_clear);
  for (uint32_t double_sided = 0; double_sided < 2; double_sided++) {
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                      p->prepass[double_sided]);
    draw_kind(s, cmd, double_sided);
  }
  vkCmdEndRendering(cmd);
  memory_barrier(cmd,
                 VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT |
                     VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                 VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT |
                     VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                 VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT |
                     VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT,
                 VK_ACCESS_2_SHADER_READ_BIT |
                     VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT);

  timestamp(s, cmd, timed, 2);

  /* Forward+ light culling per tile. */
  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, p->light_cull);
  vkCmdDispatch(cmd, data.tile_count[0], data.tile_count[1], 1);
  memory_barrier(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                 VK_ACCESS_2_SHADER_WRITE_BIT,
                 VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                 VK_ACCESS_2_SHADER_READ_BIT);

  timestamp(s, cmd, timed, 3);

  /* Opaque forward shading. */
  VkRenderingAttachmentInfo hdr_clear = color_attachment(
      s->hdr.view, VK_IMAGE_LAYOUT_GENERAL, true, zero_color);
  VkRenderingAttachmentInfo depth_load = {
      .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
      .imageView = s->depth.view,
      .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
      .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD,
      .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
  };
  begin_rendering(s, cmd, &hdr_clear, 1, &depth_load);
  for (uint32_t kind = 0; kind < 2; kind++) {
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, p->forward[kind]);
    draw_kind(s, cmd, kind);
  }
  vkCmdEndRendering(cmd);
  memory_barrier(cmd, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                 VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                 VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                 VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT);

  timestamp(s, cmd, timed, 4);

  /* Radiance and depth pyramid that the cascades trace against. */
  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, p->gi_pyramid);
  for (uint32_t level = 0; level < GI_PYRAMID_LEVELS; level++) {
    push(s, cmd, data_address, level);
    vkCmdDispatch(cmd, groups(s->pyramid_width[level], 8),
                  groups(s->pyramid_height[level], 8), 1);
    compute_to_compute(cmd);
  }

  /* Radiance cascades, top cascade first, then indirect lighting. */
  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, p->gi_cascade);
  for (int c = GI_CASCADE_COUNT - 1; c >= 0; c--) {
    push(s, cmd, data_address, (uint32_t)c);
    vkCmdDispatch(cmd, groups(s->cascades[c].width, 8),
                  groups(s->cascades[c].height, 8), 1);
    compute_to_compute(cmd);
  }
  timestamp(s, cmd, timed, 5);
  push(s, cmd, data_address, 0);
  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, p->gi_probe);
  vkCmdDispatch(cmd, groups(s->probe_count, 64), 1, 1);
  compute_to_compute(cmd);
  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, p->gi_probe_filter);
  vkCmdDispatch(cmd, groups(s->probe_count, 64), 1, 1);
  compute_to_compute(cmd);
  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, p->gi_apply);
  vkCmdDispatch(cmd, groups(s->extent.width, 8), groups(s->extent.height, 8),
                1);
  memory_barrier(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                 VK_ACCESS_2_SHADER_WRITE_BIT,
                 VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                 VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT |
                     VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);

  timestamp(s, cmd, timed, 6);

  /* Transparent forward shading over the lit image. */
  VkRenderingAttachmentInfo hdr_load = color_attachment(
      s->hdr.view, VK_IMAGE_LAYOUT_GENERAL, false, NULL);
  begin_rendering(s, cmd, &hdr_load, 1, &depth_load);
  for (uint32_t kind = 2; kind < PIPELINE_KIND_COUNT; kind++) {
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, p->forward[kind]);
    draw_kind(s, cmd, kind);
  }
  vkCmdEndRendering(cmd);
  memory_barrier(cmd, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                 VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                 VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                 VK_ACCESS_2_SHADER_READ_BIT);

  timestamp(s, cmd, timed, 7);

  /* Tone map over the window contents. */
  VkRenderingAttachmentInfo target =
      color_attachment(s->image_views[s->image_index],
                       VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, false, NULL);
  begin_rendering(s, cmd, &target, 1, NULL);
  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, p->tonemap);
  vkCmdDraw(cmd, 3, 1, 0, 0);
  vkCmdEndRendering(cmd);
  timestamp(s, cmd, timed, 8);
  return CGF_RENDER_OK;
}

///////////////////////////////////////////////////////////////////////////////
// frame_end
///////////////////////////////////////////////////////////////////////////////

CGF_RenderResult renderer_frame_end(void) {
  RendererState *s = renderer;
  if (!s->in_frame)
    return CGF_RENDER_ERROR_INVALID_STATE;
  Frame *frame = &s->frames[s->frame_index];
  VkCommandBuffer cmd = frame->command_buffer;

  VkImageMemoryBarrier2 to_present = image_barrier(
      s->images[s->image_index], VK_IMAGE_ASPECT_COLOR_BIT,
      VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
  to_present.srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
  to_present.srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
  to_present.dstStageMask = VK_PIPELINE_STAGE_2_NONE;
  to_present.dstAccessMask = VK_ACCESS_2_NONE;
  image_barriers(cmd, &to_present, 1);

  s->in_frame = false;
  s->frame_index = (s->frame_index + 1) % FRAMES_IN_FLIGHT;
  s->frame_number++;
  if (vkEndCommandBuffer(cmd) != VK_SUCCESS)
    return CGF_RENDER_ERROR_DEVICE;

  VkSemaphoreSubmitInfo wait = {
      .sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
      .semaphore = frame->image_available,
      .stageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
  };
  VkSemaphoreSubmitInfo signal = {
      .sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
      .semaphore = s->render_finished[s->image_index],
      .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
  };
  VkCommandBufferSubmitInfo command = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO,
      .commandBuffer = cmd,
  };
  VkSubmitInfo2 submit = {
      .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2,
      .waitSemaphoreInfoCount = 1,
      .pWaitSemaphoreInfos = &wait,
      .commandBufferInfoCount = 1,
      .pCommandBufferInfos = &command,
      .signalSemaphoreInfoCount = 1,
      .pSignalSemaphoreInfos = &signal,
  };
  if (vkQueueSubmit2(s->queue, 1, &submit, frame->fence) != VK_SUCCESS)
    return CGF_RENDER_ERROR_DEVICE;

  VkPresentInfoKHR present = {
      .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
      .waitSemaphoreCount = 1,
      .pWaitSemaphores = &s->render_finished[s->image_index],
      .swapchainCount = 1,
      .pSwapchains = &s->swapchain,
      .pImageIndices = &s->image_index,
  };
  VkResult presented = vkQueuePresentKHR(s->queue, &present);
  if (presented == VK_ERROR_OUT_OF_DATE_KHR || presented == VK_SUBOPTIMAL_KHR)
    s->swapchain_dirty = true;
  else if (presented != VK_SUCCESS)
    return CGF_RENDER_ERROR_DEVICE;
  return CGF_RENDER_OK;
}
