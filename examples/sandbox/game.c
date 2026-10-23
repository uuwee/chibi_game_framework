#include "../../engine/engine.h"
#include <stdio.h>

typedef struct State {
  float player_x;
  float player_y;
} State;

static void game_init(void *state, const CGF_EngineAPI *engine) {
  State *s = state;
  *s = (State){.player_x = 380.0f, .player_y = 280.0f};
  engine->log("[GAME] initialized");
}

static void game_update(void *state, const CGF_EngineAPI *engine, float dt) {
  State *s = state;
  const float speed = 180.0f;
  s->player_x += ((float)engine->key_down(CGF_KEY_RIGHT) -
                  (float)engine->key_down(CGF_KEY_LEFT)) * speed * dt;
  s->player_y += ((float)engine->key_down(CGF_KEY_DOWN) -
                  (float)engine->key_down(CGF_KEY_UP)) * speed * dt;
  if (s->player_x < 0) s->player_x = 0;
  if (s->player_x > 760) s->player_x = 760;
  if (s->player_y < 80) s->player_y = 80;
  if (s->player_y > 560) s->player_y = 560;
}

static void game_render(void *state, const CGF_EngineAPI *engine) {
  const State *s = state;
  char text[128];
  engine->draw_text("Arrows: move | F5: rebuild/reload | Esc: quit",
                    20, 15, 20, 0xE0E6F0);
  snprintf(text, sizeof(text), "player: (%.1f, %.1f)", s->player_x, s->player_y);
  engine->draw_text(text, 20, 45, 18, 0xA0B0C0);
  engine->draw_rect(s->player_x, s->player_y, 40, 40, 0.3f, 0.8f, 0.6f);
}

static void game_fini(void *state, const CGF_EngineAPI *engine) {
  const State *s = state;
  char message[128];
  snprintf(message, sizeof(message), "[GAME] finished at (%.1f, %.1f)",
           s->player_x, s->player_y);
  engine->log(message);
}

CGF_EXPORT const CGF_ScriptAPI *script_get_api(void) {
  static const CGF_ScriptAPI api = {
      .struct_size = sizeof(CGF_ScriptAPI),
      .state_version = 1,
      .state_size = sizeof(State),
      .init = game_init,
      .update = game_update,
      .render = game_render,
      .fini = game_fini,
  };
  return &api;
}

