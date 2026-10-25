#include "renderer_internal.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define UPLOAD_ALIGNMENT 256u
/* Staging offsets of buffer-to-image copies must be multiples of the
   texel block size. */
#define STAGING_ALIGNMENT 16u

static VkDeviceSize align_up(VkDeviceSize value) {
  return (value + UPLOAD_ALIGNMENT - 1) & ~(VkDeviceSize)(UPLOAD_ALIGNMENT - 1);
}

static VkDeviceSize align_staging(VkDeviceSize value) {
  return (value + STAGING_ALIGNMENT - 1) &
         ~(VkDeviceSize)(STAGING_ALIGNMENT - 1);
}

///////////////////////////////////////////////////////////////////////////////
// Vectors and matrices (column-major 4x4)
///////////////////////////////////////////////////////////////////////////////

static float dot3(const float a[3], const float b[3]) {
  return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

static void cross3(const float a[3], const float b[3], float out[3]) {
  float result[3] = {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2],
                     a[0] * b[1] - a[1] * b[0]};
  memcpy(out, result, sizeof(result));
}

static void normalize3(float v[3]) {
  float length = sqrtf(dot3(v, v));
  if (length > 0.0f) {
    for (int i = 0; i < 3; i++)
      v[i] /= length;
  }
}

static void mat4_transform(const float m[16], const float v[4], float out[4]) {
  float result[4];
  for (int row = 0; row < 4; row++)
    result[row] = m[row] * v[0] + m[4 + row] * v[1] + m[8 + row] * v[2] +
                  m[12 + row] * v[3];
  memcpy(out, result, sizeof(result));
}

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
// Shadow cascades
///////////////////////////////////////////////////////////////////////////////

/* View depth (positive in front of the camera) of an NDC depth at the screen
   center. */
static float ndc_to_view_depth(const float inverse_projection[16], float sign,
                               float ndc_z) {
  float p[4] = {0.0f, 0.0f, ndc_z, 1.0f};
  mat4_transform(inverse_projection, p, p);
  if (fabsf(p[3]) < 1e-12f)
    return INFINITY;
  return p[2] / p[3] * sign;
}

static float view_depth_to_ndc(const float projection[16], float sign,
                               float depth) {
  float p[4] = {0.0f, 0.0f, depth * sign, 1.0f};
  mat4_transform(projection, p, p);
  return p[2] / p[3];
}

/* Fits SHADOW_CASCADE_COUNT orthographic views around slices of the camera
   frustum. Each view bounds its slice with a sphere whose radius does not
   change as the camera turns, and moves in whole shadow map texels, so the
   shadows stay still while the camera moves. */
static void shadow_cascades(GpuRenderData *data, const float direction[3]) {
  float sign = ndc_to_view_depth(data->inverse_projection, 1.0f, 0.5f) < 0.0f
                   ? -1.0f
                   : 1.0f;
  float a = ndc_to_view_depth(data->inverse_projection, sign, 0.0f);
  float b = ndc_to_view_depth(data->inverse_projection, sign, 1.0f);
  float near_depth = fminf(a, b);
  float far_depth = fmaxf(a, b);
  if (!isfinite(far_depth) || far_depth > SHADOW_DISTANCE)
    far_depth = SHADOW_DISTANCE;
  if (!(near_depth > 0.0f) || near_depth >= far_depth)
    near_depth = far_depth * 1e-3f;

  float splits[SHADOW_CASCADE_COUNT + 1];
  for (int i = 0; i <= SHADOW_CASCADE_COUNT; i++) {
    float t = (float)i / SHADOW_CASCADE_COUNT;
    float logarithmic = near_depth * powf(far_depth / near_depth, t);
    float linear = near_depth + (far_depth - near_depth) * t;
    splits[i] = 0.85f * logarithmic + 0.15f * linear;
  }

  float forward[3] = {direction[0], direction[1], direction[2]};
  normalize3(forward);
  float up[3] = {0.0f, 1.0f, 0.0f};
  if (fabsf(forward[1]) > 0.99f) {
    up[0] = 1.0f;
    up[1] = 0.0f;
  }
  float right[3];
  cross3(forward, up, right);
  normalize3(right);
  cross3(right, forward, up);

  for (int c = 0; c < SHADOW_CASCADE_COUNT; c++) {
    float corners[8][3];
    float center[3] = {0.0f, 0.0f, 0.0f};
    for (int i = 0; i < 8; i++) {
      float depth = splits[c + (i >> 2)];
      float p[4] = {(i & 1) ? 1.0f : -1.0f, (i & 2) ? 1.0f : -1.0f,
                    view_depth_to_ndc(data->projection, sign, depth), 1.0f};
      mat4_transform(data->inverse_view_projection, p, p);
      for (int k = 0; k < 3; k++) {
        corners[i][k] = p[k] / p[3];
        center[k] += corners[i][k] / 8.0f;
      }
    }
    float radius = 0.0f;
    for (int i = 0; i < 8; i++) {
      float d[3] = {corners[i][0] - center[0], corners[i][1] - center[1],
                    corners[i][2] - center[2]};
      radius = fmaxf(radius, sqrtf(dot3(d, d)));
    }
    radius = ceilf(radius * 16.0f) / 16.0f;
    float texel = 2.0f * radius / SHADOW_MAP_SIZE;
    float cx = floorf(dot3(right, center) / texel) * texel;
    float cy = floorf(dot3(up, center) / texel) * texel;
    float cz = dot3(forward, center);

    /* Rows: x and y across the light, depth along it. Casters in front of
       the near plane are clamped to depth 0 when rendered. */
    float *m = data->shadow_view_projection[c];
    memset(m, 0, sizeof(float) * 16);
    for (int k = 0; k < 3; k++) {
      m[k * 4 + 0] = right[k] / radius;
      m[k * 4 + 1] = up[k] / radius;
      m[k * 4 + 2] = forward[k] / (2.0f * radius);
    }
    m[12] = -cx / radius;
    m[13] = -cy / radius;
    m[14] = -(cz - radius) / (2.0f * radius);
    m[15] = 1.0f;

    float (*planes)[4] = data->frustums[1 + c];
    frustum_planes(m, planes);
    /* No near plane: casters toward the light are kept. */
    planes[4][0] = planes[4][1] = planes[4][2] = 0.0f;
    planes[4][3] = 1e30f;
    data->shadow_splits[c] = splits[c + 1];
    data->shadow_texel[c] = texel;
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
      .subresourceRange = {aspect, 0, VK_REMAINING_MIP_LEVELS, 0,
                           VK_REMAINING_ARRAY_LAYERS},
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

static VkRenderingAttachmentInfo depth_attachment(VkImageView view,
                                                  bool clear) {
  return (VkRenderingAttachmentInfo){
      .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
      .imageView = view,
      .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
      .loadOp = clear ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD,
      .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
      .clearValue.depthStencil = {1.0f, 0},
  };
}

static void begin_rendering_area(VkCommandBuffer cmd, VkExtent2D extent,
                                 const VkRenderingAttachmentInfo *colors,
                                 uint32_t color_count,
                                 const VkRenderingAttachmentInfo *depth) {
  VkRenderingInfo info = {
      .sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
      .renderArea = {{0, 0}, extent},
      .layerCount = 1,
      .colorAttachmentCount = color_count,
      .pColorAttachments = colors,
      .pDepthAttachment = depth,
  };
  vkCmdBeginRendering(cmd, &info);
  VkViewport viewport = {0.0f, 0.0f, (float)extent.width,
                         (float)extent.height, 0.0f, 1.0f};
  VkRect2D scissor = {{0, 0}, extent};
  vkCmdSetViewport(cmd, 0, 1, &viewport);
  vkCmdSetScissor(cmd, 0, 1, &scissor);
}

static void begin_rendering(RendererState *s, VkCommandBuffer cmd,
                            const VkRenderingAttachmentInfo *colors,
                            uint32_t color_count,
                            const VkRenderingAttachmentInfo *depth) {
  begin_rendering_area(cmd, s->extent, colors, color_count, depth);
}

static uint32_t groups(uint32_t count, uint32_t size) {
  return (count + size - 1) / size;
}

static void push(RendererState *s, VkCommandBuffer cmd, uint32_t render,
                 uint32_t view, uint32_t value) {
  GpuPushConstants constants = {.render = render, .view = view, .value = value};
  vkCmdPushConstants(cmd, s->pipeline_layout, VK_SHADER_STAGE_ALL, 0,
                     sizeof(constants), &constants);
}

static void bind_sets(RendererState *s, VkCommandBuffer cmd, Frame *frame) {
  VkDescriptorSet sets[3] = {s->descriptor_set, s->texture_set,
                             frame->descriptor_set};
  vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                          s->pipeline_layout, 0, 3, sets, 0, NULL);
  vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                          s->pipeline_layout, 0, 3, sets, 0, NULL);
}

static void compute_to_compute(VkCommandBuffer cmd) {
  memory_barrier(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                 VK_ACCESS_2_SHADER_WRITE_BIT,
                 VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                 VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT);
}

///////////////////////////////////////////////////////////////////////////////
// frame_begin: uploads
///////////////////////////////////////////////////////////////////////////////

static VkDeviceSize mesh_upload_bytes(const MeshUpload *mesh) {
  return align_staging((VkDeviceSize)mesh->vertex_count * sizeof(CGF_Vertex)) +
         align_staging((VkDeviceSize)mesh->index_count * sizeof(uint32_t));
}

/* Chooses the mesh and texture data sent this frame: whole meshes first,
   then texture mips in order, within UPLOAD_BUDGET bytes. Returns the staging
   bytes; the chosen meshes are the first *mesh_count of the queue and each
   chosen texture has planned_mip_end set. Caller holds the lock. */
static VkDeviceSize plan_uploads(RendererState *s, uint32_t *mesh_count,
                                 uint32_t *texture_count) {
  VkDeviceSize used = 0;
  *mesh_count = 0;
  *texture_count = 0;
  bool full = false;
  for (uint32_t i = 0; i < s->mesh_upload_count && !full; i++) {
    VkDeviceSize bytes = mesh_upload_bytes(&s->mesh_uploads[i]);
    if (used > 0 && used + bytes > UPLOAD_BUDGET) {
      full = true;
      break;
    }
    used += bytes;
    (*mesh_count)++;
  }
  for (uint32_t i = 0; i < s->texture_upload_count && !full; i++) {
    TextureUpload *upload = &s->texture_uploads[i];
    const TextureRecord *texture = &s->textures[upload->slot];
    uint32_t mip = upload->next_mip;
    while (mip < texture->mip_count) {
      VkDeviceSize bytes = align_staging(texture_mip_size(
          texture->source_format, texture->width, texture->height, mip));
      if (used > 0 && used + bytes > UPLOAD_BUDGET) {
        full = true;
        break;
      }
      used += bytes;
      mip++;
    }
    upload->planned_mip_end = mip;
    if (mip > upload->next_mip)
      (*texture_count)++;
  }
  return used;
}

/* Bytes of upload space needed for this frame's snapshot. Caller holds the
   lock. */
static VkDeviceSize upload_size(const RendererState *s,
                                VkDeviceSize data_bytes) {
  VkDeviceSize entries = 0;
  VkDeviceSize lights = 0;
  for (uint32_t i = 0; i < s->scene_pool.used; i++) {
    if (s->scene_pool.generations[i]) {
      entries += s->scenes[i].instance_count;
      lights += s->scenes[i].light_count;
    }
  }
  VkDeviceSize staging =
      (VkDeviceSize)s->instance_dirty_count * sizeof(GpuInstance) +
      (VkDeviceSize)s->mesh_pool.used * sizeof(GpuMesh) +
      (VkDeviceSize)s->material_pool.used * sizeof(GpuMaterial) + data_bytes;
  return align_up(MAX_RENDERS_PER_FRAME * sizeof(GpuRenderData)) +
         align_up((entries + 1) * sizeof(uint32_t)) +
         align_up((lights + 1) * sizeof(GpuLight)) + align_up(staging) +
         UPLOAD_ALIGNMENT;
}

static float luminance(const float color[3]) {
  return 0.2126f * color[0] + 0.7152f * color[1] + 0.0722f * color[2];
}

static void pack_light(const CGF_LightData *data, GpuLight *gpu) {
  if (data->type == CGF_LIGHT_TYPE_DIRECTIONAL) {
    float toward[3] = {-data->direction[0], -data->direction[1],
                       -data->direction[2]};
    normalize3(toward);
    memcpy(gpu->position_radius, toward, sizeof(toward));
    gpu->position_radius[3] = 0.0f;
  } else {
    memcpy(gpu->position_radius, data->position, sizeof(float) * 3);
    gpu->position_radius[3] = data->radius;
  }
  for (int c = 0; c < 3; c++)
    gpu->color[c] = data->color[c] * data->intensity;
  gpu->color[3] = 0.0f;
}

/* Assigns the scene's environment to a cube map slot, preparing a new slot
   when the texture changed. Caller holds the lock. */
static uint32_t environment_slot(RendererState *s, const SceneRecord *scene) {
  if (!scene->has_environment ||
      !slot_valid(&s->texture_pool, scene->environment.texture))
    return ENVIRONMENT_NONE;
  CGF_Texture texture = scene->environment.texture;
  uint32_t chosen = ENVIRONMENT_NONE;
  for (uint32_t i = 0; i < MAX_ENVIRONMENTS && chosen == ENVIRONMENT_NONE;
       i++) {
    const EnvironmentSlot *slot = &s->environments[i];
    if (slot->state != ENVIRONMENT_EMPTY && slot->texture.id == texture.id &&
        slot->texture.gen == texture.gen)
      chosen = i;
  }
  for (uint32_t i = 0; i < MAX_ENVIRONMENTS && chosen == ENVIRONMENT_NONE;
       i++) {
    EnvironmentSlot *slot = &s->environments[i];
    if (slot->state == ENVIRONMENT_EMPTY ||
        slot->last_used < s->frame_number) {
      slot->state = ENVIRONMENT_WAITING;
      slot->texture = texture;
      chosen = i;
    }
  }
  if (chosen == ENVIRONMENT_NONE)
    return ENVIRONMENT_NONE;
  EnvironmentSlot *slot = &s->environments[chosen];
  slot->last_used = s->frame_number;
  if (slot->state == ENVIRONMENT_WAITING && s->textures[texture.id].resident)
    slot->process = true;
  return chosen;
}

/* Packs the scene lists into the upload buffer, dropping destroyed
   instances and lights from the scenes. Directional lights come first in
   each scene's light list. */
static void snapshot_scenes(RendererState *s, uint8_t *upload) {
  uint32_t *entries = (uint32_t *)(upload + s->entries_offset);
  GpuLight *lights = (GpuLight *)(upload + s->lights_offset);
  uint32_t entry_cursor = 0;
  uint32_t light_cursor = 0;

  for (uint32_t i = 0; i < MAX_ENVIRONMENTS; i++) {
    EnvironmentSlot *slot = &s->environments[i];
    slot->process = false;
    if (slot->state != ENVIRONMENT_EMPTY &&
        !slot_valid(&s->texture_pool, slot->texture))
      slot->state = ENVIRONMENT_EMPTY;
  }

  for (uint32_t i = 0; i < s->scene_pool.used; i++) {
    SceneRecord *scene = &s->scenes[i];
    scene->frame_generation = s->scene_pool.generations[i];
    scene->entry_offset = entry_cursor;
    scene->light_offset = light_cursor;
    scene->entry_count = 0;
    scene->frame_point_count = 0;
    scene->frame_directional_count = 0;
    scene->frame_shadow_light = UINT32_MAX;
    scene->frame_environment = ENVIRONMENT_NONE;
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
      if (!slot_valid(&s->light_pool, scene->lights[k]))
        scene->lights[k] = scene->lights[--scene->light_count];
      else
        k++;
    }
    float brightest = -1.0f;
    for (int pass = 0; pass < 2; pass++) {
      CGF_LightType type =
          pass == 0 ? CGF_LIGHT_TYPE_DIRECTIONAL : CGF_LIGHT_TYPE_POINT;
      for (uint32_t k = 0; k < scene->light_count; k++) {
        const CGF_LightData *data = &s->lights[scene->lights[k].id];
        if (data->type != type)
          continue;
        pack_light(data, &lights[light_cursor++]);
        if (type == CGF_LIGHT_TYPE_POINT) {
          scene->frame_point_count++;
          continue;
        }
        float brightness = luminance(data->color) * data->intensity;
        if (data->cast_shadow && brightness > brightest) {
          brightest = brightness;
          scene->frame_shadow_light = scene->frame_directional_count;
          memcpy(scene->frame_shadow_direction, data->direction,
                 sizeof(scene->frame_shadow_direction));
        }
        scene->frame_directional_count++;
      }
    }

    scene->frame_environment = environment_slot(s, scene);
    scene->frame_environment_intensity = scene->environment.intensity;
    scene->frame_environment_rotation = scene->environment.rotation;
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

/* Copies of one texture's mips from the staging buffer. */
typedef struct TextureCopy {
  uint32_t slot;
  bool first; /* the image has no data yet */
  bool last;  /* the image is complete after these copies */
  uint32_t region_count;
  VkBufferImageCopy regions[16];
} TextureCopy;

/* Copies the changed CPU data and this frame's share of the mesh and
   texture data to the device-local buffers and images. */
static bool record_uploads(RendererState *s, VkCommandBuffer cmd,
                           Buffer *upload, VkDeviceSize staging_offset,
                           uint32_t mesh_count, uint32_t texture_count) {
  uint8_t *base = upload->mapped;
  VkDeviceSize cursor = staging_offset;
  CopyList instance_copies = {0}, vertex_copies = {0}, index_copies = {0};
  VkBufferCopy mesh_copy = {0}, material_copy = {0};
  TextureCopy *texture_copies =
      texture_count ? calloc(texture_count, sizeof(TextureCopy)) : NULL;
  bool ok = texture_count == 0 || texture_copies;

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

  /* Mesh data; the meshes become drawable with this frame. */
  for (uint32_t i = 0; i < mesh_count && ok; i++) {
    MeshUpload *mesh = &s->mesh_uploads[i];
    VkDeviceSize vertex_bytes = (VkDeviceSize)mesh->vertex_count *
                                sizeof(CGF_Vertex);
    VkDeviceSize index_bytes = (VkDeviceSize)mesh->index_count *
                               sizeof(uint32_t);
    cursor = align_staging(cursor);
    memcpy(base + cursor, mesh->vertices, vertex_bytes);
    ok = ok && copy_add(&vertex_copies, cursor,
                        (VkDeviceSize)mesh->first_vertex * sizeof(CGF_Vertex),
                        vertex_bytes);
    cursor = align_staging(cursor + vertex_bytes);
    memcpy(base + cursor, mesh->indices, index_bytes);
    ok = ok && copy_add(&index_copies, cursor,
                        (VkDeviceSize)mesh->first_index * sizeof(uint32_t),
                        index_bytes);
    cursor += index_bytes;
    s->gpu_meshes[mesh->slot] = s->mesh_records[mesh->slot].gpu;
    s->meshes_dirty = true;
  }

  /* Texture mips. */
  uint32_t copy_count = 0;
  for (uint32_t i = 0; i < s->texture_upload_count && ok; i++) {
    TextureUpload *upload_job = &s->texture_uploads[i];
    if (upload_job->planned_mip_end <= upload_job->next_mip)
      break;
    const TextureRecord *texture = &s->textures[upload_job->slot];
    TextureCopy *copy = &texture_copies[copy_count++];
    copy->slot = upload_job->slot;
    copy->first = upload_job->next_mip == 0;
    copy->last = upload_job->planned_mip_end == texture->mip_count;
    size_t source = 0;
    for (uint32_t mip = 0; mip < upload_job->planned_mip_end; mip++) {
      size_t size = texture_mip_size(texture->source_format, texture->width,
                                     texture->height, mip);
      if (mip >= upload_job->next_mip && copy->region_count < 16) {
        cursor = align_staging(cursor);
        memcpy(base + cursor, upload_job->data + source, size);
        copy->regions[copy->region_count++] = (VkBufferImageCopy){
            .bufferOffset = cursor,
            .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, mip, 0, 1},
            .imageExtent = {texture->width >> mip ? texture->width >> mip : 1,
                            texture->height >> mip ? texture->height >> mip
                                                   : 1,
                            1},
        };
        cursor += size;
      }
      source += size;
    }
    upload_job->next_mip = upload_job->planned_mip_end;
  }

  if (s->meshes_dirty && s->mesh_pool.used) {
    VkDeviceSize size = (VkDeviceSize)s->mesh_pool.used * sizeof(GpuMesh);
    cursor = align_staging(cursor);
    memcpy(base + cursor, s->gpu_meshes, size);
    mesh_copy = (VkBufferCopy){cursor, 0, size};
    cursor += size;
  }
  s->meshes_dirty = false;

  /* Textures completed this frame can be used by materials right away. */
  for (uint32_t i = 0; i < copy_count; i++) {
    if (texture_copies[i].last) {
      s->textures[texture_copies[i].slot].resident = true;
      vk_write_texture_descriptor(s, texture_copies[i].slot,
                                  &s->textures[texture_copies[i].slot]);
      s->materials_dirty = true;
    }
  }
  if (s->materials_dirty && s->material_pool.used) {
    renderer_build_materials(s);
    VkDeviceSize size =
        (VkDeviceSize)s->material_pool.used * sizeof(GpuMaterial);
    cursor = align_staging(cursor);
    memcpy(base + cursor, s->gpu_materials, size);
    material_copy = (VkBufferCopy){cursor, 0, size};
    cursor += size;
  }
  s->materials_dirty = false;

  /* Drop the finished items from the queues. */
  for (uint32_t i = 0; i < mesh_count; i++) {
    free(s->mesh_uploads[i].vertices);
    free(s->mesh_uploads[i].indices);
  }
  memmove(s->mesh_uploads, s->mesh_uploads + mesh_count,
          sizeof(MeshUpload) * (s->mesh_upload_count - mesh_count));
  s->mesh_upload_count -= mesh_count;
  uint32_t kept = 0;
  for (uint32_t i = 0; i < s->texture_upload_count; i++) {
    TextureUpload *job = &s->texture_uploads[i];
    if (job->next_mip >= s->textures[job->slot].mip_count)
      free(job->data);
    else
      s->texture_uploads[kept++] = *job;
  }
  s->texture_upload_count = kept;

  VkImageMemoryBarrier2 *image_barriers_list =
      copy_count ? calloc(copy_count, sizeof(VkImageMemoryBarrier2)) : NULL;
  ok = ok && (copy_count == 0 || image_barriers_list);
  bool any = instance_copies.count || vertex_copies.count ||
             index_copies.count || mesh_copy.size || material_copy.size ||
             copy_count;
  if (ok && any) {
    /* Earlier frames may still read these buffers. New images go to the
       transfer layout; images in progress already are in it. */
    uint32_t barrier_count = 0;
    for (uint32_t i = 0; i < copy_count; i++) {
      if (!texture_copies[i].first)
        continue;
      VkImageMemoryBarrier2 *b = &image_barriers_list[barrier_count++];
      *b = image_barrier(s->textures[texture_copies[i].slot].image,
                         VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                         VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
      b->srcStageMask = VK_PIPELINE_STAGE_2_NONE;
      b->srcAccessMask = VK_ACCESS_2_NONE;
      b->dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
      b->dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    }
    VkMemoryBarrier2 before = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
        .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
        .srcAccessMask =
            VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
        .dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT,
        .dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
    };
    VkDependencyInfo dependency = {
        .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .memoryBarrierCount = 1,
        .pMemoryBarriers = &before,
        .imageMemoryBarrierCount = barrier_count,
        .pImageMemoryBarriers = image_barriers_list,
    };
    vkCmdPipelineBarrier2(cmd, &dependency);

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
    for (uint32_t i = 0; i < copy_count; i++)
      vkCmdCopyBufferToImage(cmd, upload->buffer,
                             s->textures[texture_copies[i].slot].image,
                             VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                             texture_copies[i].region_count,
                             texture_copies[i].regions);

    barrier_count = 0;
    for (uint32_t i = 0; i < copy_count; i++) {
      if (!texture_copies[i].last)
        continue;
      VkImageMemoryBarrier2 *b = &image_barriers_list[barrier_count++];
      *b = image_barrier(s->textures[texture_copies[i].slot].image,
                         VK_IMAGE_ASPECT_COLOR_BIT,
                         VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                         VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
      b->srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
      b->srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
      b->dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
      b->dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT;
    }
    VkMemoryBarrier2 after = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
        .srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT,
        .srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
        .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
        .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT,
    };
    dependency.pMemoryBarriers = &after;
    dependency.imageMemoryBarrierCount = barrier_count;
    vkCmdPipelineBarrier2(cmd, &dependency);
  }
  free(image_barriers_list);
  free(texture_copies);
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
    "total",   "cull",     "shadows",  "prepass",     "light cull",
    "forward", "gi trace", "gi apply", "transparent", "tonemap",
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
  VkImageMemoryBarrier2 barriers[5 + GI_CASCADE_COUNT];
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
  barriers[count++] =
      image_barrier(s->pyramid, VK_IMAGE_ASPECT_COLOR_BIT,
                    VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
  image_barriers(cmd, barriers, count);
  s->targets_need_init = false;
}

static void initialize_static_images(RendererState *s, VkCommandBuffer cmd) {
  VkImageMemoryBarrier2 barriers[1 + 2 * MAX_ENVIRONMENTS];
  uint32_t count = 0;
  barriers[count++] =
      image_barrier(s->shadow_map, VK_IMAGE_ASPECT_DEPTH_BIT,
                    VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
  for (uint32_t i = 0; i < MAX_ENVIRONMENTS; i++) {
    barriers[count++] =
        image_barrier(s->environments[i].radiance, VK_IMAGE_ASPECT_COLOR_BIT,
                      VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
    barriers[count++] = image_barrier(
        s->environments[i].irradiance, VK_IMAGE_ASPECT_COLOR_BIT,
        VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
  }
  image_barriers(cmd, barriers, count);
  s->static_images_need_init = false;
}

/* Builds the prefiltered radiance and the irradiance cube maps of
   environments whose texture has just become usable. */
static void process_environments(RendererState *s, VkCommandBuffer cmd) {
  bool any = false;
  for (uint32_t i = 0; i < MAX_ENVIRONMENTS; i++)
    any = any || s->environments[i].process;
  if (!any)
    return;
  const Pipelines *p = &s->pipelines;
  VkPipelineLayout layout = s->env_pipeline_layout;
  vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 1, 1,
                          &s->texture_set, 0, NULL);
  /* Earlier frames may still sample the slot. */
  memory_barrier(cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                 VK_ACCESS_2_MEMORY_READ_BIT,
                 VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                 VK_ACCESS_2_SHADER_WRITE_BIT);
  for (uint32_t i = 0; i < MAX_ENVIRONMENTS; i++) {
    EnvironmentSlot *slot = &s->environments[i];
    if (!slot->process)
      continue;
    /* render: unused, view: source texture, value: cube map level. */
    GpuPushConstants constants = {.view = slot->texture.id};
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, p->env_prefilter);
    for (uint32_t mip = 0; mip < ENV_CUBE_MIPS; mip++) {
      uint32_t size = ENV_CUBE_SIZE >> mip;
      constants.value = mip;
      vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0,
                              1, &s->env_sets[i][mip], 0, NULL);
      vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_ALL, 0,
                         sizeof(constants), &constants);
      vkCmdDispatch(cmd, groups(size, 8), groups(size, 8), 6);
    }
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, p->env_irradiance);
    constants.value = 0;
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 1,
                            &s->env_sets[i][ENV_CUBE_MIPS], 0, NULL);
    vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_ALL, 0, sizeof(constants),
                       &constants);
    vkCmdDispatch(cmd, groups(ENV_IRRADIANCE_SIZE, 8),
                  groups(ENV_IRRADIANCE_SIZE, 8), 6);
    slot->process = false;
    slot->state = ENVIRONMENT_READY;
  }
  memory_barrier(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                 VK_ACCESS_2_SHADER_WRITE_BIT,
                 VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                 VK_ACCESS_2_SHADER_READ_BIT);
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
  /* Frames before this slot's previous one have completed. */
  if (s->frame_number >= FRAMES_IN_FLIGHT)
    renderer_release_retired(s, s->frame_number - FRAMES_IN_FLIGHT);
  uint32_t mesh_count = 0;
  uint32_t texture_count = 0;
  VkDeviceSize data_bytes = plan_uploads(s, &mesh_count, &texture_count);
  if (!ensure_upload_capacity(s, frame, upload_size(s, data_bytes))) {
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
  if (s->static_images_need_init)
    initialize_static_images(s, cmd);

  /* Snapshot: everything changed before this point is drawn this frame. */
  VkDeviceSize entry_count = 0;
  VkDeviceSize light_count = 0;
  for (uint32_t i = 0; i < s->scene_pool.used; i++) {
    if (s->scene_pool.generations[i]) {
      entry_count += s->scenes[i].instance_count;
      light_count += s->scenes[i].light_count;
    }
  }
  s->entries_offset = align_up(MAX_RENDERS_PER_FRAME * sizeof(GpuRenderData));
  s->lights_offset =
      s->entries_offset + align_up((entry_count + 1) * sizeof(uint32_t));
  VkDeviceSize staging_offset =
      s->lights_offset + align_up((light_count + 1) * sizeof(GpuLight));
  snapshot_scenes(s, frame->upload.mapped);
  bool uploaded = record_uploads(s, cmd, &frame->upload, staging_offset,
                                 mesh_count, texture_count);
  s->frame_mesh_count = s->mesh_pool.used;
  vk_write_frame_set(s, frame, s->entries_offset, s->lights_offset);
  ReleaseSRWLockExclusive(&s->lock);
  if (!uploaded)
    fprintf(stderr, "[RENDERER] out of memory while uploading\n");

  if (s->targets_need_init)
    initialize_targets(s, cmd);
  process_environments(s, cmd);

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

static void draw_kind(RendererState *s, VkCommandBuffer cmd, uint32_t view,
                      uint32_t kind) {
  if (s->frame_mesh_count == 0)
    return;
  uint32_t list = view * DRAW_KIND_COUNT + kind;
  vkCmdDrawIndexedIndirectCount(
      cmd, s->draws.buffer,
      (VkDeviceSize)list * MAX_MESHES * GPU_DRAW_COMMAND_SIZE,
      s->cull_counters.buffer, (VkDeviceSize)list * sizeof(uint32_t),
      s->frame_mesh_count, GPU_DRAW_COMMAND_SIZE);
}

static void draw_transparent(RendererState *s, VkCommandBuffer cmd) {
  vkCmdDrawIndexedIndirectCount(
      cmd, s->draws.buffer,
      (VkDeviceSize)TRANSPARENT_DRAW_BASE * GPU_DRAW_COMMAND_SIZE,
      s->cull_counters.buffer,
      (VkDeviceSize)COUNTER_TRANSPARENT_DRAWS * sizeof(uint32_t),
      TRANSPARENT_CAPACITY, GPU_DRAW_COMMAND_SIZE);
}

static bool fill_render_data(RendererState *s, const SceneRecord *record,
                             const CGF_Camera *camera, GpuRenderData *data) {
  *data = (GpuRenderData){0};
  memcpy(data->view, camera->view, sizeof(data->view));
  memcpy(data->projection, camera->projection, sizeof(data->projection));
  mat4_multiply(camera->projection, camera->view, data->view_projection);
  float inverse_view[16];
  if (!mat4_inverse(data->view_projection, data->inverse_view_projection) ||
      !mat4_inverse(camera->projection, data->inverse_projection) ||
      !mat4_inverse(camera->view, inverse_view))
    return false;
  data->camera_position[0] = inverse_view[12];
  data->camera_position[1] = inverse_view[13];
  data->camera_position[2] = inverse_view[14];
  data->camera_position[3] = 1.0f;
  memcpy(data->sky, s->sky, sizeof(data->sky));
  frustum_planes(data->view_projection, data->frustums[0]);
  data->screen_size[0] = s->extent.width;
  data->screen_size[1] = s->extent.height;
  data->tile_count[0] = groups(s->extent.width, TILE_SIZE);
  data->tile_count[1] = groups(s->extent.height, TILE_SIZE);
  data->entry_offset = record->entry_offset;
  data->entry_count = record->entry_count;
  data->light_offset = record->light_offset;
  data->point_light_count = record->frame_point_count;
  data->directional_count = record->frame_directional_count;
  data->shadow_light = record->frame_shadow_light;
  data->view_count = 1;
  if (record->frame_shadow_light != UINT32_MAX) {
    shadow_cascades(data, record->frame_shadow_direction);
    data->view_count = VIEW_COUNT;
  }
  data->mesh_count = s->frame_mesh_count;
  data->cascade_count = GI_CASCADE_COUNT;
  data->gi_interval = GI_INTERVAL;
  data->debug_mode = s->debug_mode;
  data->environment_slot = ENVIRONMENT_NONE;
  uint32_t environment = record->frame_environment;
  if (environment != ENVIRONMENT_NONE &&
      s->environments[environment].state == ENVIRONMENT_READY) {
    data->environment_slot = environment;
    data->environment[0] = record->frame_environment_intensity;
    data->environment[1] = cosf(record->frame_environment_rotation);
    data->environment[2] = sinf(record->frame_environment_rotation);
  }
  data->exposure = exp2f(camera->exposure);
  return true;
}

CGF_RenderResult renderer_render(CGF_Scene scene, const CGF_Camera *camera) {
  RendererState *s = renderer;
  if (!s->in_frame)
    return CGF_RENDER_ERROR_INVALID_STATE;
  if (!camera || !isfinite(camera->exposure))
    return CGF_RENDER_ERROR_INVALID_ARGUMENT;
  if (scene.gen == 0 || scene.id >= s->scene_pool.used ||
      s->scenes[scene.id].frame_generation != scene.gen)
    return CGF_RENDER_ERROR_INVALID_HANDLE;
  if (s->render_count >= MAX_RENDERS_PER_FRAME)
    return CGF_RENDER_ERROR_CAPACITY;

  const SceneRecord *record = &s->scenes[scene.id];
  Frame *frame = &s->frames[s->frame_index];
  GpuRenderData data;
  if (!fill_render_data(s, record, camera, &data))
    return CGF_RENDER_ERROR_INVALID_ARGUMENT;

  uint32_t render = s->render_count++;
  memcpy((uint8_t *)frame->upload.mapped + render * sizeof(GpuRenderData),
         &data, sizeof(data));
  bool timed = s->profile && render == 0;
  if (timed)
    s->queries_written[s->frame_index] = true;

  VkCommandBuffer cmd = frame->command_buffer;
  const Pipelines *p = &s->pipelines;
  bind_sets(s, cmd, frame);
  push(s, cmd, render, 0, 0);

  /* Previous renders may still use the shared buffers and images. */
  memory_barrier(cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                 VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
                 VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                 VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT);

  timestamp(s, cmd, timed, 0);

  /* GPU culling for the camera and each shadow cascade: one indirect draw
     per visible (pipeline, mesh), and a sorted list of transparent
     instances. */
  uint32_t views = data.view_count;
  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, p->cull_reset);
  vkCmdDispatch(cmd,
                groups(views * DRAW_KIND_COUNT * data.mesh_count +
                           COUNTER_COUNT,
                       64),
                1, 1);
  compute_to_compute(cmd);
  if (data.entry_count > 0) {
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, p->cull_count);
    vkCmdDispatch(cmd, groups(data.entry_count, 64), views, 1);
    compute_to_compute(cmd);
  }
  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, p->cull_prefix);
  vkCmdDispatch(cmd, views, 1, 1);
  compute_to_compute(cmd);
  if (data.entry_count > 0) {
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, p->cull_write);
    vkCmdDispatch(cmd, groups(data.entry_count, 64), views, 1);
  }
  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, p->transparent_sort);
  vkCmdDispatch(cmd, 1, 1, 1);
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

  /* Shadow cascades of the scene's main directional light. */
  if (views > 1) {
    VkExtent2D shadow_extent = {SHADOW_MAP_SIZE, SHADOW_MAP_SIZE};
    for (uint32_t c = 0; c < SHADOW_CASCADE_COUNT; c++) {
      VkRenderingAttachmentInfo depth =
          depth_attachment(s->shadow_layers[c], true);
      begin_rendering_area(cmd, shadow_extent, NULL, 0, &depth);
      push(s, cmd, render, 1 + c, 0);
      for (uint32_t kind = 0; kind < DRAW_KIND_COUNT; kind++) {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                          p->shadow[kind >= 2]);
        draw_kind(s, cmd, 1 + c, kind);
      }
      vkCmdEndRendering(cmd);
    }
    memory_barrier(cmd, VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                   VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                   VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                   VK_ACCESS_2_SHADER_READ_BIT);
  }
  push(s, cmd, render, 0, 0);

  timestamp(s, cmd, timed, 2);

  /* Depth prepass with the surface parameters (opaque and masked only). */
  const float zero_color[4] = {0.0f, 0.0f, 0.0f, 0.0f};
  VkRenderingAttachmentInfo prepass_colors[2] = {
      color_attachment(s->normal.view, VK_IMAGE_LAYOUT_GENERAL, true,
                       zero_color),
      color_attachment(s->albedo.view, VK_IMAGE_LAYOUT_GENERAL, true,
                       zero_color),
  };
  VkRenderingAttachmentInfo depth_clear = depth_attachment(s->depth.view, true);
  begin_rendering(s, cmd, prepass_colors, 2, &depth_clear);
  for (uint32_t kind = 0; kind < DRAW_KIND_COUNT; kind++) {
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, p->prepass[kind]);
    draw_kind(s, cmd, 0, kind);
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

  timestamp(s, cmd, timed, 3);

  /* Forward+ light culling per tile. */
  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, p->light_cull);
  vkCmdDispatch(cmd, data.tile_count[0], data.tile_count[1], 1);
  memory_barrier(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                 VK_ACCESS_2_SHADER_WRITE_BIT,
                 VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                 VK_ACCESS_2_SHADER_READ_BIT);

  timestamp(s, cmd, timed, 4);

  /* Opaque forward shading. */
  VkRenderingAttachmentInfo hdr_clear = color_attachment(
      s->hdr.view, VK_IMAGE_LAYOUT_GENERAL, true, zero_color);
  VkRenderingAttachmentInfo depth_load =
      depth_attachment(s->depth.view, false);
  begin_rendering(s, cmd, &hdr_clear, 1, &depth_load);
  for (uint32_t kind = 0; kind < DRAW_KIND_COUNT; kind++) {
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, p->forward[kind]);
    draw_kind(s, cmd, 0, kind);
  }
  vkCmdEndRendering(cmd);
  memory_barrier(cmd, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                 VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                 VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                 VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT);

  timestamp(s, cmd, timed, 5);

  /* Radiance and depth pyramid that the cascades trace against. */
  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, p->gi_pyramid);
  for (uint32_t level = 0; level < GI_PYRAMID_LEVELS; level++) {
    push(s, cmd, render, 0, level);
    vkCmdDispatch(cmd, groups(s->pyramid_width[level], 8),
                  groups(s->pyramid_height[level], 8), 1);
    compute_to_compute(cmd);
  }

  /* Radiance cascades, top cascade first, then indirect lighting. */
  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, p->gi_cascade);
  for (int c = GI_CASCADE_COUNT - 1; c >= 0; c--) {
    push(s, cmd, render, 0, (uint32_t)c);
    vkCmdDispatch(cmd, groups(s->cascades[c].width, 8),
                  groups(s->cascades[c].height, 8), 1);
    compute_to_compute(cmd);
  }
  timestamp(s, cmd, timed, 6);
  push(s, cmd, render, 0, 0);
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
                 VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT |
                     VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                 VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT |
                     VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT |
                     VK_ACCESS_2_SHADER_READ_BIT);

  timestamp(s, cmd, timed, 7);

  /* Transparent forward shading over the lit image, back to front. */
  VkRenderingAttachmentInfo hdr_load = color_attachment(
      s->hdr.view, VK_IMAGE_LAYOUT_GENERAL, false, NULL);
  begin_rendering(s, cmd, &hdr_load, 1, &depth_load);
  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, p->transparent);
  draw_transparent(s, cmd);
  vkCmdEndRendering(cmd);
  memory_barrier(cmd, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                 VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                 VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                 VK_ACCESS_2_SHADER_READ_BIT);

  timestamp(s, cmd, timed, 8);

  /* Tone map over the window contents. */
  VkRenderingAttachmentInfo target =
      color_attachment(s->image_views[s->image_index],
                       VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, false, NULL);
  begin_rendering(s, cmd, &target, 1, NULL);
  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, p->tonemap);
  vkCmdDraw(cmd, 3, 1, 0, 0);
  vkCmdEndRendering(cmd);
  timestamp(s, cmd, timed, 9);
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
