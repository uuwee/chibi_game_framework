#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "api_registry.h"
#include "plugin.h"

#define PLUGIN_PATH_MAX 512

typedef struct LoadedModule {
  HMODULE module;
  wchar_t path[PLUGIN_PATH_MAX];
} LoadedModule;

struct CGF_Plugin {
  wchar_t source_path[PLUGIN_PATH_MAX];
  char *entry_name;
  LoadedModule loaded;
};

static void windows_error(const char *operation) {
  fprintf(stderr, "[ERROR] %s (Windows error %lu)\n", operation,
          GetLastError());
}

static void unload_module(LoadedModule *loaded) {
  if (loaded->module && !FreeLibrary(loaded->module)) {
    windows_error("FreeLibrary");
    return;
  }
  if (loaded->path[0] && !DeleteFileW(loaded->path))
    windows_error("Delete loaded DLL copy");
  *loaded = (LoadedModule){0};
}

CGF_Plugin *cgf_plugin_create(const wchar_t *path, const char *entry_name) {
  if (!path || !path[0] || !entry_name || !entry_name[0]) return NULL;
  CGF_Plugin *plugin = calloc(1, sizeof(*plugin));
  if (!plugin) return NULL;
  DWORD length = GetFullPathNameW(path, PLUGIN_PATH_MAX, plugin->source_path, NULL);
  if (!length || length >= PLUGIN_PATH_MAX) {
    free(plugin);
    return NULL;
  }
  size_t size = strlen(entry_name) + 1;
  plugin->entry_name = malloc(size);
  if (!plugin->entry_name) {
    free(plugin);
    return NULL;
  }
  memcpy(plugin->entry_name, entry_name, size);
  return plugin;
}

bool cgf_plugin_reload(CGF_Plugin *plugin) {
  static unsigned long generation;
  if (!plugin) return false;
  LoadedModule candidate = {0};
  /* Keep copies beside the source DLL, with process/instance-unique names.
     Never load the build output itself: the linker must be able to replace it. */
  for (;;) {
    int length = swprintf(candidate.path, PLUGIN_PATH_MAX, L"%ls.%lu.%lu.dll",
                          plugin->source_path, GetCurrentProcessId(), ++generation);
    if (length < 0 || length >= PLUGIN_PATH_MAX) return false;
    if (CopyFileW(plugin->source_path, candidate.path, TRUE)) break;
    DWORD error = GetLastError();
    if (error == ERROR_FILE_EXISTS || error == ERROR_ALREADY_EXISTS) continue;
    windows_error("Copy plugin DLL");
    return false;
  }
  candidate.module = LoadLibraryW(candidate.path);
  if (!candidate.module) {
    windows_error("Load plugin DLL");
    goto fail;
  }
  FARPROC symbol = GetProcAddress(candidate.module, plugin->entry_name);
  if (!symbol) {
    windows_error("Find plugin entry");
    goto fail;
  }
  CGF_PluginLoad load;
  _Static_assert(sizeof(load) == sizeof(symbol), "function pointer size");
  memcpy(&load, &symbol, sizeof(load));

  if (!cgf_api_registry_begin()) {
    fprintf(stderr, "[RELOAD] registry transaction already active\n");
    goto fail;
  }
  if (!load(cgf_api_registry_get_api())) {
    cgf_api_registry_cancel();
    fprintf(stderr, "[RELOAD] plugin rejected load; keeping current plugin\n");
    goto fail;
  }
  cgf_api_registry_commit();
  LoadedModule previous = plugin->loaded;
  plugin->loaded = candidate;
  unload_module(&previous);
  puts("[RELOAD] plugin active (state preserved)");
  return true;
fail:
  unload_module(&candidate);
  return false;
}

void cgf_plugin_close(CGF_Plugin *plugin) {
  if (!plugin) return;
  unload_module(&plugin->loaded);
  free(plugin->entry_name);
  free(plugin);
}
