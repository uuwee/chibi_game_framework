#include "include/runtime.h"

#include <string.h>


typedef struct PluginEntry {
  char *name;

  size_t api_size;
  void *api;

  size_t state_size;
  size_t state_alignment;
  void *state;
} PluginEntry;

#define PLUGIN_ENTRY_MAX 64
PluginEntry plugin_entries[PLUGIN_ENTRY_MAX];
uint32_t plugin_entries_count = 0;

const void *get(const char *name, uint32_t version, size_t size) {
  if (name == NULL) {
    return NULL;
  }

  for (uint32_t i = 0; i < plugin_entries_count; i++) {
    const PluginEntry *entry = &plugin_entries[i];

    if (entry->name == NULL)
      continue;

    if (strcmp(name, entry->name) == 0) {
      return entry->api;
    }
  }

  return NULL;
}

void *get_or_create_state(const char *name, size_t size, size_t alignment,
                          bool *created) {
  if (name == NULL)
    return NULL;

  for (uint32_t i = 0; i < plugin_entries_count; i++) {
    PluginEntry *entry = &plugin_entries[i];

    if (entry->name == NULL)
      continue;

    if (strcmp(name, entry->name) == 0) {
      if (entry->state == NULL) {
        // create
        entry->state = malloc(size);
      }

      return entry->state;
    }
  }

  return NULL;
}

