#include "../../engine/engine.h"
#include "include/game.h"
#include <stdio.h>

typedef struct State {
  float player_x;
  float player_y;
} State;

static const CGF_EngineAPI *engine;
static State *state;

static void game_update(float dt) {
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

static void game_render(void) {
  const State *s = state;
  char text[128];
  engine->draw_text("Arrows: move | F5: rebuild/reload | Esc: quit",
                    20, 15, 20, 0xE0E6F0);
  snprintf(text, sizeof(text), "player: (%.1f, %.1f)", s->player_x, s->player_y);
  engine->draw_text(text, 20, 45, 18, 0xA0B0C0);
  engine->draw_rect(s->player_x, s->player_y, 40, 40, 0.3f, 0.8f, 0.6f);
}

static void game_shutdown(void) {
  const State *s = state;
  char message[128];
  snprintf(message, sizeof(message), "[GAME] finished at (%.1f, %.1f)",
           s->player_x, s->player_y);
  engine->log(message);
}

CGF_PLUGIN_EXPORT bool cgf_sandbox_load(const CGF_APIRegistry *registry) {
  if (!registry) return false;
  const CGF_EngineAPI *new_engine = registry->get(
      CGF_ENGINE_API_NAME, sizeof(CGF_EngineAPI));
  if (!new_engine || new_engine->struct_size != sizeof(CGF_EngineAPI) ||
      !new_engine->log || !new_engine->key_down || !new_engine->draw_rect ||
      !new_engine->draw_text) return false;

  bool created = false;
  State *new_state = registry->get_or_create_state(
      "sandbox.main", sizeof(State), _Alignof(State), &created);
  if (!new_state) return false;
  if (created)
    *new_state = (State){.player_x = 380.0f, .player_y = 280.0f};

  engine = new_engine;
  state = new_state;
  static const SandboxAPI api = {
      .update = game_update,
      .render = game_render,
      .shutdown = game_shutdown,
  };
  /* Publish last. Never mutate existing state or fail after publication. */
  if (!registry->set(CGF_GAME_API_NAME, sizeof(api), &api)) return false;
  if (created) engine->log("[GAME] initialized");
  return true;
}

