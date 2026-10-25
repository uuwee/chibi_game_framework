#ifndef CGF_ENGINE_H
#define CGF_ENGINE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "../runtime/include/runtime.h"

typedef enum CGF_Key {
  CGF_KEY_LEFT,
  CGF_KEY_RIGHT,
  CGF_KEY_UP,
  CGF_KEY_DOWN,
  CGF_KEY_F5,
  CGF_KEY_F6,
  CGF_KEY_W,
  CGF_KEY_A,
  CGF_KEY_S,
  CGF_KEY_D,
  CGF_KEY_Q,
  CGF_KEY_E,
  CGF_KEY_SHIFT,
  CGF_KEY_P,
  CGF_KEY_COUNT,
} CGF_Key;

typedef struct CGF_EngineAPI {
  uint32_t struct_size;

  void (*log)(const char *message);
  bool (*key_down)(CGF_Key key);
  void (*draw_rect)(float x, float y, float w, float h, float r, float g,
                    float b);
  void (*draw_text)(const char *text, int x, int y, int size, uint32_t color);

  /* Client area size in pixels. Both are 0 while the window is minimized or
     closed. */
  void (*window_size)(uint32_t *width, uint32_t *height);
  /* Win32 handles (HINSTANCE, HWND) of the window, or NULL when closed. */
  void (*native_window)(void **instance, void **window);
} CGF_EngineAPI;

bool engine_register_api(const CGF_APIRegistry *registry);
bool engine_open_window(int width, int height, const char *title);

bool engine_should_close(void);
float engine_delta_time(void);
void engine_reset_clock(void);
bool engine_key_pressed(CGF_Key key);

void engine_begin_frame(float r, float g, float b);
void engine_end_frame(void);
void engine_close_window(void);

///////////////////////////////////////////////////////////////////////////////
// API REGISTRY
///////////////////////////////////////////////////////////////////////////////
#define CGF_ENGINE_API_NAME "cgf.engine"

#endif
