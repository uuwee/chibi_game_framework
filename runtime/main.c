#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>

#include "../buildsys/build.h"
#include "../engine/engine.h"
#include "../examples/sandbox/include/game.h"
#include "api_registry.h"
#include "plugin.h"

static const BuildTarget target_sandbox = {
    .type = BUILD_TYPE_DLL,
    .name = "sandbox",
    .source = (const char *[]){"examples/sandbox/game.c"},
    .source_count = 1,
    .output = ".build/bin/game.dll",
    .option = "/Iengine /DCGF_PLUGIN_DLL",
};

static bool rebuild_plugin(CGF_Plugin *plugin, const CGF_EngineAPI *engine) {
  if (build_target(&target_sandbox)) {
    engine->log("[RELOAD] build failed; keeping current plugin");
    return false;
  }
  return cgf_plugin_reload(plugin);
}

int main(void) {
  const CGF_APIRegistry *registry = cgf_api_registry_get_api();
  if (!engine_register_api(registry)) {
    fprintf(stderr, "[ERROR] cannot register engine API\n");
    return 1;
  }
  const CGF_EngineAPI *engine =
      registry->get(CGF_ENGINE_API_NAME, sizeof(CGF_EngineAPI));

  if (!engine_open_window(800, 600, "Chibi sandbox - F5 reload")) {
    fprintf(stderr, "[ERROR] cannot open window (Windows error %lu)\n", GetLastError());
    cgf_api_registry_shutdown();
    return 1;
  }
  CGF_Plugin *sandbox = cgf_plugin_create(L".build/bin/game.dll", "cgf_sandbox_load");
  if (!sandbox) {
    engine->log("[ERROR] cannot create sandbox plugin");
    engine_close_window();
    cgf_api_registry_shutdown();
    return 1;
  }
  rebuild_plugin(sandbox, engine);
  engine_reset_clock();

  while (!engine_should_close()) {
    if (engine_key_pressed(CGF_KEY_RELOAD)) {
      rebuild_plugin(sandbox, engine);
      engine_reset_clock();
      continue;
    }
    float dt = engine_delta_time();
    engine_begin_frame(0.06f, 0.08f, 0.12f);
    const SandboxAPI *game = registry->get(CGF_GAME_API_NAME, sizeof(SandboxAPI));
    if (game) {
      game->update(dt);
      game->render();
    } else {
      engine->draw_text("No plugin loaded. Fix game.c and press F5.",
                        20, 20, 20, 0xFFB080);
    }
    engine_end_frame();
    Sleep(1);
  }

  const SandboxAPI *game = registry->get(CGF_GAME_API_NAME, sizeof(SandboxAPI));
  if (game) game->shutdown();
  cgf_plugin_close(sandbox);
  engine_close_window();
  cgf_api_registry_shutdown();
  return 0;
}
