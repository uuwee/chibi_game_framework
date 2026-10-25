#include "../../engine/engine.h"
#include "../../engine/include/renderer.h"
#include "include/game.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#define PI 3.14159265358979f
#define ORBIT_LIGHT_COUNT 24
#define LIGHT_COUNT (ORBIT_LIGHT_COUNT + 1)
#define COLOR_COUNT 6
#define MAX_STATIC_INSTANCES 16

enum {
  MATERIAL_WHITE,
  MATERIAL_RED,
  MATERIAL_GREEN,
  MATERIAL_GOLD,
  MATERIAL_BLUE,
  MATERIAL_CEILING_LIGHT,
  MATERIAL_GLASS,
  MATERIAL_MARKER_FIRST, /* one emissive material per light color */
  MATERIAL_COUNT = MATERIAL_MARKER_FIRST + COLOR_COUNT,
};

/* Persistent across reloads of this plugin. Holds only plain values. */
typedef struct State {
  bool content_ready;
  bool content_failed;
  float camera_position[3];
  float yaw;
  float pitch;
  float time;
  bool paused; /* P toggles the animation */
  bool pause_key_down;
  float fps_time;
  uint32_t fps_frames;

  CGF_Scene scene;
  CGF_Mesh cube;
  CGF_Mesh sphere;
  CGF_Material materials[MATERIAL_COUNT];
  CGF_Instance statics[MAX_STATIC_INSTANCES];
  uint32_t static_count;
  CGF_Instance spinner;
  CGF_Instance markers[ORBIT_LIGHT_COUNT];
  CGF_Light lights[LIGHT_COUNT];
} State;

static const CGF_APIRegistry *registry;
static const CGF_EngineAPI *engine;
static State *state;

static const float light_colors[COLOR_COUNT][3] = {
    {1.0f, 0.35f, 0.2f}, {0.2f, 0.6f, 1.0f}, {0.3f, 1.0f, 0.4f},
    {1.0f, 0.85f, 0.3f}, {0.9f, 0.3f, 1.0f}, {0.2f, 1.0f, 0.9f},
};

///////////////////////////////////////////////////////////////////////////////
// Math (column-major 4x4)
///////////////////////////////////////////////////////////////////////////////

/* Translation * rotation about Y * scale. */
static void make_transform(float out[16], float x, float y, float z, float yaw,
                           float sx, float sy, float sz) {
  float c = cosf(yaw);
  float s = sinf(yaw);
  float m[16] = {c * sx, 0.0f, -s * sx, 0.0f, 0.0f, sy,   0.0f,   0.0f,
                 s * sz, 0.0f, c * sz,  0.0f, x,    y,    z,      1.0f};
  memcpy(out, m, sizeof(m));
}

static void camera_forward(float out[3]) {
  out[0] = cosf(state->pitch) * sinf(state->yaw);
  out[1] = sinf(state->pitch);
  out[2] = -cosf(state->pitch) * cosf(state->yaw);
}

static void make_camera(CGF_Camera *camera, float aspect) {
  const float *eye = state->camera_position;
  float f[3];
  camera_forward(f);
  /* right = normalize(cross(f, up)), up = cross(right, f) */
  float r[3] = {-f[2], 0.0f, f[0]};
  float length = sqrtf(r[0] * r[0] + r[2] * r[2]);
  r[0] /= length;
  r[2] /= length;
  float u[3] = {r[1] * f[2] - r[2] * f[1], r[2] * f[0] - r[0] * f[2],
                r[0] * f[1] - r[1] * f[0]};
  float view[16] = {
      r[0], u[0], -f[0], 0.0f, r[1], u[1], -f[1], 0.0f,
      r[2], u[2], -f[2], 0.0f,
      -(r[0] * eye[0] + r[1] * eye[1] + r[2] * eye[2]),
      -(u[0] * eye[0] + u[1] * eye[1] + u[2] * eye[2]),
      f[0] * eye[0] + f[1] * eye[1] + f[2] * eye[2], 1.0f};
  memcpy(camera->view, view, sizeof(view));

  /* Vulkan clip space: Y down, depth 0..1. */
  float near_plane = 0.05f;
  float far_plane = 100.0f;
  float focal = 1.0f / tanf(60.0f * PI / 180.0f * 0.5f);
  float projection[16] = {0};
  projection[0] = focal / aspect;
  projection[5] = -focal;
  projection[10] = far_plane / (near_plane - far_plane);
  projection[11] = -1.0f;
  projection[14] = near_plane * far_plane / (near_plane - far_plane);
  memcpy(camera->projection, projection, sizeof(projection));
}

///////////////////////////////////////////////////////////////////////////////
// Meshes
///////////////////////////////////////////////////////////////////////////////

static CGF_Mesh create_cube(const CGF_EngineRendererAPI *r) {
  /* Each face: normal n and axes u, v with cross(u, v) = n, so the corners
     are counter-clockwise seen from outside. */
  static const float faces[6][3][3] = {
      {{1, 0, 0}, {0, 0, -1}, {0, 1, 0}},  {{-1, 0, 0}, {0, 0, 1}, {0, 1, 0}},
      {{0, 1, 0}, {1, 0, 0}, {0, 0, -1}},  {{0, -1, 0}, {1, 0, 0}, {0, 0, 1}},
      {{0, 0, 1}, {1, 0, 0}, {0, 1, 0}},   {{0, 0, -1}, {-1, 0, 0}, {0, 1, 0}},
  };
  static const float corners[4][2] = {{-1, -1}, {1, -1}, {1, 1}, {-1, 1}};
  CGF_Vertex vertices[24];
  uint32_t indices[36];
  for (int f = 0; f < 6; f++) {
    const float *n = faces[f][0], *u = faces[f][1], *v = faces[f][2];
    for (int c = 0; c < 4; c++) {
      CGF_Vertex *vertex = &vertices[f * 4 + c];
      for (int k = 0; k < 3; k++) {
        vertex->position[k] =
            0.5f * (n[k] + corners[c][0] * u[k] + corners[c][1] * v[k]);
        vertex->normal[k] = n[k];
      }
      vertex->uv[0] = corners[c][0] * 0.5f + 0.5f;
      vertex->uv[1] = corners[c][1] * 0.5f + 0.5f;
    }
    uint32_t base = (uint32_t)f * 4;
    uint32_t face_indices[6] = {base, base + 1, base + 2,
                                base, base + 2, base + 3};
    memcpy(&indices[f * 6], face_indices, sizeof(face_indices));
  }
  CGF_MeshData data = {vertices, 24, indices, 36};
  CGF_Mesh mesh = {0};
  r->mesh_create(&data, 1, &mesh);
  return mesh;
}

static CGF_Mesh create_sphere(const CGF_EngineRendererAPI *r) {
  enum { RINGS = 16, SEGMENTS = 32 };
  static CGF_Vertex vertices[(RINGS + 1) * (SEGMENTS + 1)];
  static uint32_t indices[RINGS * SEGMENTS * 6];
  uint32_t vertex_count = 0;
  for (int i = 0; i <= RINGS; i++) {
    float theta = PI * (float)i / RINGS;
    for (int j = 0; j <= SEGMENTS; j++) {
      float phi = 2.0f * PI * (float)j / SEGMENTS;
      CGF_Vertex *v = &vertices[vertex_count++];
      v->normal[0] = sinf(theta) * cosf(phi);
      v->normal[1] = cosf(theta);
      v->normal[2] = sinf(theta) * sinf(phi);
      for (int k = 0; k < 3; k++)
        v->position[k] = v->normal[k] * 0.5f;
      v->uv[0] = (float)j / SEGMENTS;
      v->uv[1] = (float)i / RINGS;
    }
  }
  uint32_t index_count = 0;
  for (uint32_t i = 0; i < RINGS; i++) {
    for (uint32_t j = 0; j < SEGMENTS; j++) {
      uint32_t a = i * (SEGMENTS + 1) + j;
      uint32_t b = a + SEGMENTS + 1;
      uint32_t quad[6] = {a, a + 1, b, a + 1, b + 1, b};
      memcpy(&indices[index_count], quad, sizeof(quad));
      index_count += 6;
    }
  }
  CGF_MeshData data = {vertices, vertex_count, indices, index_count};
  CGF_Mesh mesh = {0};
  r->mesh_create(&data, 1, &mesh);
  return mesh;
}

///////////////////////////////////////////////////////////////////////////////
// Content
///////////////////////////////////////////////////////////////////////////////

static CGF_MaterialData material(float r, float g, float b, float a,
                                 CGF_MaterialBlend blend, float emission) {
  CGF_MaterialData data = {
      .blend = blend,
      .base_color = {r, g, b, a},
      .emissive = {r * emission, g * emission, b * emission},
  };
  return data;
}

static bool add_static(const CGF_EngineRendererAPI *r, CGF_Mesh mesh,
                       uint32_t material_index, float x, float y, float z,
                       float yaw, float sx, float sy, float sz) {
  CGF_InstanceData data = {.mesh = mesh,
                           .material = state->materials[material_index]};
  make_transform(data.transform, x, y, z, yaw, sx, sy, sz);
  return state->static_count < MAX_STATIC_INSTANCES &&
         r->instance_create(&data, 1, &state->statics[state->static_count++]) ==
             CGF_RENDER_OK;
}

static void orbit_light(uint32_t i, float time, CGF_LightData *out) {
  float phase = 2.0f * PI * (float)i / ORBIT_LIGHT_COUNT;
  float speed = 0.35f + 0.1f * (float)(i % 3);
  float angle = phase + time * speed;
  float radius = 2.2f + 1.6f * (float)(i % 2);
  const float *color = light_colors[i % COLOR_COUNT];
  *out = (CGF_LightData){
      .position = {radius * cosf(angle), 0.35f + 0.25f * sinf(time * 1.7f + phase),
                   radius * sinf(angle)},
      .radius = 3.0f,
      .color = {color[0], color[1], color[2]},
      .intensity = 1.5f,
  };
}

static bool create_content(const CGF_EngineRendererAPI *r) {
  state->cube = create_cube(r);
  state->sphere = create_sphere(r);

  CGF_MaterialData materials[MATERIAL_COUNT] = {
      [MATERIAL_WHITE] = material(0.75f, 0.75f, 0.75f, 1, CGF_MATERIAL_BLEND_OPAQUE, 0),
      [MATERIAL_RED] = material(0.75f, 0.12f, 0.1f, 1, CGF_MATERIAL_BLEND_OPAQUE, 0),
      [MATERIAL_GREEN] = material(0.12f, 0.6f, 0.15f, 1, CGF_MATERIAL_BLEND_OPAQUE, 0),
      [MATERIAL_GOLD] = material(0.9f, 0.65f, 0.25f, 1, CGF_MATERIAL_BLEND_OPAQUE, 0),
      [MATERIAL_BLUE] = material(0.15f, 0.3f, 0.85f, 1, CGF_MATERIAL_BLEND_OPAQUE, 0),
      [MATERIAL_CEILING_LIGHT] = material(1.0f, 0.93f, 0.8f, 1, CGF_MATERIAL_BLEND_OPAQUE, 6.0f),
      [MATERIAL_GLASS] = material(0.4f, 0.8f, 1.0f, 0.35f, CGF_MATERIAL_BLEND_ALPHA, 0),
  };
  materials[MATERIAL_GLASS].double_sided = true;
  for (int i = 0; i < COLOR_COUNT; i++) {
    const float *c = light_colors[i];
    materials[MATERIAL_MARKER_FIRST + i] =
        material(c[0], c[1], c[2], 1, CGF_MATERIAL_BLEND_OPAQUE, 4.0f);
  }
  if (r->material_create(materials, MATERIAL_COUNT, state->materials) !=
      CGF_RENDER_OK)
    return false;

  /* A room open toward the camera, Cornell-box style. */
  CGF_Mesh cube = state->cube;
  CGF_Mesh sphere = state->sphere;
  state->static_count = 0;
  bool ok = add_static(r, cube, MATERIAL_WHITE, 0, -0.1f, 0, 0, 10.4f, 0.2f, 10.4f) &&
            add_static(r, cube, MATERIAL_WHITE, 0, 6.1f, 0, 0, 10.4f, 0.2f, 10.4f) &&
            add_static(r, cube, MATERIAL_WHITE, 0, 3, -5.1f, 0, 10.4f, 6.4f, 0.2f) &&
            add_static(r, cube, MATERIAL_RED, -5.1f, 3, 0, 0, 0.2f, 6.4f, 10.4f) &&
            add_static(r, cube, MATERIAL_GREEN, 5.1f, 3, 0, 0, 0.2f, 6.4f, 10.4f) &&
            add_static(r, cube, MATERIAL_CEILING_LIGHT, 0, 5.98f, -1.0f, 0, 2.5f, 0.05f, 2.5f) &&
            add_static(r, cube, MATERIAL_WHITE, -1.9f, 1.6f, -2.0f, 0.4f, 1.4f, 3.2f, 1.4f) &&
            add_static(r, sphere, MATERIAL_GOLD, 1.9f, 1.0f, -0.6f, 0, 2.0f, 2.0f, 2.0f) &&
            add_static(r, cube, MATERIAL_GLASS, 2.6f, 0.8f, 2.2f, 0.6f, 1.2f, 1.6f, 1.2f);
  if (!ok)
    return false;

  CGF_InstanceData spinner = {.mesh = cube,
                              .material = state->materials[MATERIAL_BLUE]};
  make_transform(spinner.transform, -1.0f, 0.6f, 2.0f, 0, 1.2f, 1.2f, 1.2f);
  if (r->instance_create(&spinner, 1, &state->spinner) != CGF_RENDER_OK)
    return false;

  CGF_LightData lights[LIGHT_COUNT];
  CGF_InstanceData markers[ORBIT_LIGHT_COUNT];
  for (uint32_t i = 0; i < ORBIT_LIGHT_COUNT; i++) {
    orbit_light(i, 0.0f, &lights[i]);
    markers[i] = (CGF_InstanceData){
        .mesh = sphere,
        .material = state->materials[MATERIAL_MARKER_FIRST + i % COLOR_COUNT],
    };
    const float *p = lights[i].position;
    make_transform(markers[i].transform, p[0], p[1], p[2], 0, 0.12f, 0.12f,
                   0.12f);
  }
  /* A warm light under the ceiling panel. */
  lights[ORBIT_LIGHT_COUNT] = (CGF_LightData){
      .position = {0.0f, 5.4f, -1.0f},
      .radius = 14.0f,
      .color = {1.0f, 0.9f, 0.75f},
      .intensity = 18.0f,
  };
  if (r->light_create(lights, LIGHT_COUNT, state->lights) != CGF_RENDER_OK ||
      r->instance_create(markers, ORBIT_LIGHT_COUNT, state->markers) !=
          CGF_RENDER_OK)
    return false;

  if (r->scene_create(&state->scene) != CGF_RENDER_OK ||
      r->scene_add(state->scene, state->statics, state->static_count) !=
          CGF_RENDER_OK ||
      r->scene_add(state->scene, &state->spinner, 1) != CGF_RENDER_OK ||
      r->scene_add(state->scene, state->markers, ORBIT_LIGHT_COUNT) !=
          CGF_RENDER_OK ||
      r->scene_add_lights(state->scene, state->lights, LIGHT_COUNT) !=
          CGF_RENDER_OK)
    return false;
  return true;
}

///////////////////////////////////////////////////////////////////////////////
// Game API
///////////////////////////////////////////////////////////////////////////////

static float key_axis(CGF_Key positive, CGF_Key negative) {
  return (float)engine->key_down(positive) - (float)engine->key_down(negative);
}

static void game_update(float dt) {
  State *s = state;
  bool pause_key_down = engine->key_down(CGF_KEY_P);
  if (pause_key_down && !s->pause_key_down)
    s->paused = !s->paused;
  s->pause_key_down = pause_key_down;
  if (!s->paused)
    s->time += dt;

  s->yaw += key_axis(CGF_KEY_RIGHT, CGF_KEY_LEFT) * 1.5f * dt;
  s->pitch += key_axis(CGF_KEY_UP, CGF_KEY_DOWN) * 1.2f * dt;
  if (s->pitch > 1.4f)
    s->pitch = 1.4f;
  if (s->pitch < -1.4f)
    s->pitch = -1.4f;

  float speed = (engine->key_down(CGF_KEY_SHIFT) ? 12.0f : 4.0f) * dt;
  float forward[3];
  camera_forward(forward);
  float right[3] = {cosf(s->yaw), 0.0f, sinf(s->yaw)};
  float move = key_axis(CGF_KEY_W, CGF_KEY_S);
  float strafe = key_axis(CGF_KEY_D, CGF_KEY_A);
  float lift = key_axis(CGF_KEY_E, CGF_KEY_Q);
  for (int k = 0; k < 3; k++)
    s->camera_position[k] += (forward[k] * move + right[k] * strafe) * speed;
  s->camera_position[1] += lift * speed;

  s->fps_time += dt;
  s->fps_frames++;
  if (s->fps_time >= 2.0f) {
    char message[64];
    snprintf(message, sizeof(message), "[GAME] %.1f fps",
             (double)((float)s->fps_frames / s->fps_time));
    engine->log(message);
    s->fps_time = 0.0f;
    s->fps_frames = 0;
  }
}

static void animate(const CGF_EngineRendererAPI *r) {
  State *s = state;
  CGF_LightUpdate light_updates[ORBIT_LIGHT_COUNT];
  CGF_InstanceUpdate marker_updates[ORBIT_LIGHT_COUNT + 1];
  for (uint32_t i = 0; i < ORBIT_LIGHT_COUNT; i++) {
    light_updates[i].light = s->lights[i];
    orbit_light(i, s->time, &light_updates[i].data);
    const float *p = light_updates[i].data.position;
    marker_updates[i] = (CGF_InstanceUpdate){
        .instance = s->markers[i],
        .mask = CGF_RENDER_UPDATE_TRANSFORM,
    };
    make_transform(marker_updates[i].data.transform, p[0], p[1], p[2], 0,
                   0.12f, 0.12f, 0.12f);
  }
  marker_updates[ORBIT_LIGHT_COUNT] = (CGF_InstanceUpdate){
      .instance = s->spinner,
      .mask = CGF_RENDER_UPDATE_TRANSFORM,
  };
  make_transform(marker_updates[ORBIT_LIGHT_COUNT].data.transform, -1.0f,
                 0.6f, 2.0f, s->time * 0.8f, 1.2f, 1.2f, 1.2f);
  r->light_update(light_updates, ORBIT_LIGHT_COUNT);
  r->instance_update(marker_updates, ORBIT_LIGHT_COUNT + 1);
}

static void game_render(void) {
  const CGF_EngineRendererAPI *r =
      registry->get(CGF_ENGINE_RENDERER_API_NAME, sizeof(CGF_EngineRendererAPI));
  if (!r)
    return;
  State *s = state;
  if (!s->content_ready && !s->content_failed) {
    s->content_ready = create_content(r);
    s->content_failed = !s->content_ready;
    engine->log(s->content_ready ? "[GAME] scene created"
                                 : "[GAME] failed to create the scene");
  }
  if (!s->content_ready)
    return;

  animate(r);

  const float sky[4] = {0.02f, 0.025f, 0.04f, 1.0f};
  CGF_RenderResult result = r->frame_begin(sky);
  if (result != CGF_RENDER_OK)
    return;
  uint32_t width = 0, height = 0;
  engine->window_size(&width, &height);
  CGF_Camera camera;
  make_camera(&camera, height ? (float)width / (float)height : 1.0f);
  result = r->render(s->scene, &camera);
  if (result != CGF_RENDER_OK) {
    char message[64];
    snprintf(message, sizeof(message), "[GAME] render failed: %d",
             (int)result);
    engine->log(message);
  }
  r->frame_end();
}

static void game_shutdown(void) {
  const State *s = state;
  char message[128];
  snprintf(message, sizeof(message), "[GAME] finished at (%.1f, %.1f, %.1f)",
           (double)s->camera_position[0], (double)s->camera_position[1],
           (double)s->camera_position[2]);
  engine->log(message);
}

CGF_PLUGIN_EXPORT bool cgf_sandbox_load(const CGF_APIRegistry *new_registry) {
  if (!new_registry)
    return false;
  const CGF_EngineAPI *new_engine =
      new_registry->get(CGF_ENGINE_API_NAME, sizeof(CGF_EngineAPI));
  if (!new_engine || new_engine->struct_size != sizeof(CGF_EngineAPI) ||
      !new_engine->log || !new_engine->key_down || !new_engine->window_size)
    return false;

  bool created = false;
  State *new_state = new_registry->get_or_create_state(
      "sandbox.main", sizeof(State), _Alignof(State), &created);
  if (!new_state)
    return false;
  if (created) {
    *new_state = (State){
        .camera_position = {0.0f, 2.6f, 9.5f},
        .pitch = -0.08f,
    };
  }

  registry = new_registry;
  engine = new_engine;
  state = new_state;
  static const SandboxAPI api = {
      .update = game_update,
      .render = game_render,
      .shutdown = game_shutdown,
  };
  /* Publish last. Never mutate existing state or fail after publication. */
  if (!new_registry->set(CGF_GAME_API_NAME, sizeof(api), &api))
    return false;
  if (created)
    engine->log("[GAME] initialized");
  return true;
}
