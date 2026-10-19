#ifndef CGF_ENGINE_H
#define CGF_ENGINE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define CGF_ABI_VERSION 1u

typedef enum CGF_Key {
  CGF_KEY_LEFT,
  CGF_KEY_RIGHT,
  CGF_KEY_UP,
  CGF_KEY_DOWN,
  CGF_KEY_RELOAD,
  CGF_KEY_COUNT,
} CGF_Key;

typedef struct CGF_EngineAPI {
  uint32_t struct_size;
  uint32_t abi_version;

  void (*log)(const char *message);
  bool (*key_down)(CGF_Key key);
  void (*draw_rect)(float x, float y, float w, float h, float r, float g,
                    float b);
  void (*draw_text)(const char *text, int x, int y, int size, uint32_t color);
} CGF_API;

typedef struct CGF_ScriptAPI {
  uint32_t struct_size;
  uint32_t abi_version;
  uint32_t state_version;
  size_t state_size;

  /* State is owned by runtime. init/fini run only on first load/shutdown.
     Compatible reloads keep state; do not store DLL function/static pointers
     or DLL-owned resources in it. get_api must have no side effects. */
  void (*init)(void *state, const CGF_API *engine);
  void (*update)(void *state, const CGF_API *engine, float dt);
  void (*render)(void *state, const CGF_API *engine);
  void (*fini)(void *state, const CGF_API *engine);
} CGF_ScriptAPI;

const CGF_API *engine_get_api(void);
bool engine_open_window(int width, int height, const char *title);

bool engine_should_close(void);
float engine_delta_time(void);
void engine_reset_clock(void);
bool engine_key_pressed(CGF_Key key);

void engine_begin_frame(float r, float g, float b);
void engine_end_frame(void);
void engine_close_window(void);

#if defined(_WIN32) && defined(CGF_GAME_DLL)
#define CGF_EXPORT __declspec(dllexport)
#else
#define CGF_EXPORT
#endif

/* Both API tables must change CGF_ABI_VERSION when their ABI changes. */
CGF_EXPORT const CGF_ScriptAPI *script_get_api(void);

#endif
