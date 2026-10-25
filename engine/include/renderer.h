#ifndef CGF_ENGINE_RENDERER_H
#define CGF_ENGINE_RENDERER_H

#include "../../runtime/include/runtime.h"

#include <stdint.h>

#define CGF_ENGINE_RENDERER_API_NAME "cgf.engine.renderer"

typedef struct CGF_ID {
  uint32_t id, gen;
} CGF_ID;

typedef CGF_ID CGF_Scene;
typedef CGF_ID CGF_Instance;
typedef CGF_ID CGF_Mesh;
typedef CGF_ID CGF_Material;

typedef struct CGF_Camera {
  float view[16];
  float projection[16];
} CGF_Camera;

typedef struct CGF_InstanceData {
  float transform[16];
  CGF_Mesh mesh;
  CGF_Material material;
} CGF_InstanceData;

typedef enum CGF_RenderUpdate {
  CGF_RENDER_UPDATE_TRANSFORM = 1u << 0,
  CGF_RENDER_UPDATE_MESH = 1u << 1,
  CGF_RENDER_UPDATE_MATERIAL = 1u << 2,
  CGF_RENDER_UPDATE_FLAGS = 1u << 3,
  CGF_RENDER_UPDATE_COUNT,
} CGF_RenderUpdate;

typedef struct CGF_InstanceUpdate {
  CGF_Scene scene;
  CGF_RenderUpdate mask;
  CGF_InstanceData update;
} CGF_InstanceUpdate;

typedef struct CGF_MeshData {

} CGF_MeshData;

typedef struct CGF_MaterialData {

} CGF_MaterialData;

typedef struct CGF_EngineRendererAPI {
  void (*scene_create)(CGF_Scene *out_scene);
  void (*scene_destroy)(CGF_Scene scene);

  void (*allocate)(CGF_Scene scene, const CGF_InstanceData *data_list,
                   uint32_t count, CGF_Instance *out_instance_list);
  void (*free)(const CGF_Instance instance);
  void (*update)(const CGF_Instance instance, const CGF_InstanceData data);

  void (*mesh_create)(const CGF_MeshData *data_list, const uint32_t size,
                      CGF_Mesh *out_mesh_list);
  void (*mesh_destroy)(CGF_Mesh mesh);

  void (*material_create)(const CGF_MaterialData *data_list,
                          const uint32_t size, CGF_Material *out_material_list);
  void (*material_destroy)(CGF_Material material);
} CGF_EngineRendererAPI;

#endif
