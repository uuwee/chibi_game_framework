#define WIN32_LEAN_AND_MEAN
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <windows.h>

#include "../buildsys/build.h"
#include "../engine/engine.h"

static const BuildTarget target_script = {
    .type = BUILD_TYPE_DLL,
    .name = "sandbox",
    .source =
        (const char *[]){
            "examples/sandbox/game.c",
        },
    .source_count = 1,
    .output = ".build/bin/game.dll",
    .option = "/Iengine /DCGF_GAME_DLL",
};

typedef struct Script {
  HMODULE module;
  const CGF_ScriptAPI *api;
  void *state;
  wchar_t loaded_path[512];
} Script;

typedef const CGF_ScriptAPI *(*GetScriptAPI)(void);

static void windows_error(const char *operation) {
  fprintf(stderr, "[ERROR] %s (Windows error %lu)\n", operation,
          GetLastError());
}

static void unload_module(Script *script) {
  if (script->module && !FreeLibrary(script->module)) {
    windows_error("FreeLibrary");
    return;
  }
  if (script->loaded_path[0] && !DeleteFileW(script->loaded_path))
    windows_error("Delete loaded DLL copy");
  script->module = NULL;
  script->api = NULL;
  script->loaded_path[0] = L'\0';
}

static bool reload_script(Script *script, const CGF_API *engine) {
  static unsigned long generation;
  Script candidate = {0};
  if (build_target(&target_script)) {
    engine->log("[RELOAD] build failed; keeping current script");
    return false;
  }

  wchar_t relative[128];
  int length = swprintf(relative, sizeof(relative) / sizeof(relative[0]),
                        L".build/bin/game_%lu_%lu.dll", GetCurrentProcessId(),
                        ++generation);
  if (length < 0)
    return false;
  DWORD path_length =
      GetFullPathNameW(relative, 512, candidate.loaded_path, NULL);
  if (!path_length || path_length >= 512) {
    engine->log("[ERROR] loaded DLL path is invalid or too long");
    return false;
  }
  if (!CopyFileW(L".build/bin/game.dll", candidate.loaded_path, TRUE)) {
    windows_error("Copy script DLL");
    return false;
  }
  candidate.module = LoadLibraryW(candidate.loaded_path);
  if (!candidate.module) {
    windows_error("Load script DLL");
    goto fail;
  }

  FARPROC symbol = GetProcAddress(candidate.module, "script_get_api");
  if (!symbol) {
    windows_error("Find script_get_api");
    goto fail;
  }

  GetScriptAPI get_api;
  _Static_assert(sizeof(get_api) == sizeof(symbol), "function pointer size");
  memcpy(&get_api, &symbol, sizeof(get_api));
  candidate.api = get_api();
  const CGF_ScriptAPI *api = candidate.api;
  if (!api || api->struct_size != sizeof(CGF_ScriptAPI) ||
      api->abi_version != CGF_ABI_VERSION || !api->state_size || !api->init ||
      !api->update || !api->render || !api->fini) {
    engine->log("[RELOAD] invalid script API; keeping current script");
    goto fail;
  }
  if (script->module && (api->state_version != script->api->state_version ||
                         api->state_size != script->api->state_size)) {
    engine->log(
        "[RELOAD] incompatible state; restart runtime to use this script");
    goto fail;
  }

  if (script->module) {
    candidate.state = script->state;
  } else {
    candidate.state = calloc(1, api->state_size);
    if (!candidate.state) {
      engine->log("[ERROR] cannot allocate script state");
      goto fail;
    }
    api->init(candidate.state, engine);
  }

  Script previous = *script;
  *script = candidate;
  unload_module(&previous);
  engine->log("[RELOAD] script active (state preserved)");
  return true;
fail:
  unload_module(&candidate);
  return false;
}

static void close_script(Script *script, const CGF_API *engine) {
  if (script->module)
    script->api->fini(script->state, engine);
  free(script->state);
  script->state = NULL;
  unload_module(script);
}

int main() {
  const CGF_API *engine = engine_get_api();
  Script script = {0};
  if (!engine_open_window(800, 600, "Chibi sandbox - F5 reload")) {
    windows_error("Open window");
    return 1;
  }
  reload_script(&script, engine);
  engine_reset_clock();

  while (!engine_should_close()) {
    if (engine_key_pressed(CGF_KEY_RELOAD)) {
      reload_script(&script, engine);
      engine_reset_clock();
      continue;
    }
    float dt = engine_delta_time();
    engine_begin_frame(0.06f, 0.08f, 0.12f);
    if (script.module) {
      script.api->update(script.state, engine, dt);
      script.api->render(script.state, engine);
    } else {
      engine->draw_text("No script loaded. Fix game.c and press F5.", 20, 20,
                        20, 0xFFB080);
    }
    engine_end_frame();
    Sleep(1);
  }
  close_script(&script, engine);
  engine_close_window();
  return 0;
}
