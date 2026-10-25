#define WIN32_LEAN_AND_MEAN
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

#include "../buildsys/build.h"
#include "../buildsys/targets.h"
#include "../engine/engine.h"
#include "../engine/include/renderer.h"
#include "../engine/include/renderer_runtime.h"
#include "../examples/sandbox/include/game.h"
#include "api_registry.h"
#include "plugin.h"

static bool rebuild_plugin(CGF_Plugin *plugin, const BuildTarget *target,
                           const CGF_EngineAPI *engine) {
  if (build_target(target)) {
    engine->log("[RELOAD] build failed; keeping current plugin");
    return false;
  }
  return cgf_plugin_reload(plugin);
}

static void draw_message(const CGF_EngineAPI *engine, const char *message) {
  engine_begin_frame(0.06f, 0.08f, 0.12f);
  engine->draw_text(message, 20, 20, 20, 0xFFB080);
  engine_end_frame();
  Sleep(1);
}

/* Usage: runtime [--frames N]. --frames exits after N frames. */
static long parse_frame_limit(int argc, char **argv) {
  for (int i = 1; i + 1 < argc; i++) {
    if (strcmp(argv[i], "--frames") == 0)
      return strtol(argv[i + 1], NULL, 10);
  }
  return 0;
}

int main(int argc, char **argv) {
  long frame_limit = parse_frame_limit(argc, argv);
  const CGF_APIRegistry *registry = cgf_api_registry_get_api();
  if (!engine_register_api(registry)) {
    fprintf(stderr, "[ERROR] cannot register engine API\n");
    return 1;
  }
  const CGF_EngineAPI *engine =
      registry->get(CGF_ENGINE_API_NAME, sizeof(CGF_EngineAPI));

  if (!engine_open_window(1280, 720,
                          "Chibi sandbox - F5: reload game, F6: reload "
                          "renderer")) {
    fprintf(stderr, "[ERROR] cannot open window (Windows error %lu)\n",
            GetLastError());
    cgf_api_registry_shutdown();
    return 1;
  }
  CGF_Plugin *renderer =
      cgf_plugin_create(L".build/bin/renderer.dll", "cgf_renderer_load");
  CGF_Plugin *sandbox =
      cgf_plugin_create(L".build/bin/game.dll", "cgf_sandbox_load");
  if (!renderer || !sandbox) {
    engine->log("[ERROR] cannot create plugins");
    cgf_plugin_close(sandbox);
    cgf_plugin_close(renderer);
    engine_close_window();
    cgf_api_registry_shutdown();
    return 1;
  }
  /* renderer.dll is built by build.exe; load what is there. */
  cgf_plugin_reload(renderer);
  rebuild_plugin(sandbox, &target_sandbox, engine);
  engine_reset_clock();

  long frames = 0;
  while (!engine_should_close()) {
    if (engine_key_pressed(CGF_KEY_F5)) {
      rebuild_plugin(sandbox, &target_sandbox, engine);
      engine_reset_clock();
      continue;
    }
    if (engine_key_pressed(CGF_KEY_F6)) {
      rebuild_plugin(renderer, &target_renderer, engine);
      engine_reset_clock();
      continue;
    }
    float dt = engine_delta_time();
    const SandboxAPI *game =
        registry->get(CGF_GAME_API_NAME, sizeof(SandboxAPI));
    const CGF_EngineRendererAPI *render_api = registry->get(
        CGF_ENGINE_RENDERER_API_NAME, sizeof(CGF_EngineRendererAPI));
    if (!render_api) {
      draw_message(engine, "No renderer loaded. Fix the renderer and press F6.");
    } else if (!game) {
      draw_message(engine, "No game loaded. Fix game.c and press F5.");
    } else {
      game->update(dt);
      game->render();
    }
    uint32_t width = 0;
    uint32_t height = 0;
    engine->window_size(&width, &height);
    if (width == 0 || height == 0)
      Sleep(16); /* minimized: no presentation paces the loop */
    if (frame_limit > 0 && ++frames >= frame_limit)
      break;
  }

  const SandboxAPI *game = registry->get(CGF_GAME_API_NAME, sizeof(SandboxAPI));
  if (game)
    game->shutdown();
  cgf_plugin_close(sandbox);
  const CGF_EngineRendererRuntimeAPI *renderer_runtime =
      registry->get(CGF_ENGINE_RENDERER_RUNTIME_API_NAME,
                    sizeof(CGF_EngineRendererRuntimeAPI));
  if (renderer_runtime)
    renderer_runtime->shutdown();
  cgf_plugin_close(renderer);
  engine_close_window();
  cgf_api_registry_shutdown();
  return 0;
}
