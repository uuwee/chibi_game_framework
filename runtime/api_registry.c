#include "api_registry.h"

#include <malloc.h>
#include <stdlib.h>
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

typedef struct Registry {
  PluginEntry entries[PLUGIN_ENTRY_MAX];
  size_t count;
} Registry;

static Registry live, pending;
static bool transaction;
static bool changed_api[PLUGIN_ENTRY_MAX];

static Registry *current(void) { return transaction ? &pending : &live; }

static PluginEntry *find_entry(Registry *registry, const char *name) {
  for (size_t i = 0; i < registry->count; ++i)
    if (strcmp(registry->entries[i].name, name) == 0)
      return &registry->entries[i];
  return NULL;
}

static PluginEntry *add_entry(Registry *registry, const char *name) {
  if (registry->count == PLUGIN_ENTRY_MAX) return NULL;
  size_t length = strlen(name) + 1;
  char *copy = malloc(length);
  if (!copy) return NULL;
  memcpy(copy, name, length);
  PluginEntry *entry = &registry->entries[registry->count++];
  *entry = (PluginEntry){.name = copy};
  return entry;
}

static const void *get(const char *name, size_t size) {
  if (!name || !name[0] || !size) return NULL;
  Registry *registry = current();
  PluginEntry *entry = find_entry(registry, name);
  if (!entry || entry->api_size != size) return NULL;
  size_t index = (size_t)(entry - registry->entries);
  if (transaction && index < live.count && live.entries[index].api)
    return live.entries[index].api;
  return entry->api;
}

static bool set(const char *name, size_t size, const void *api) {
  if (!name || !name[0] || !size || !api) return false;
  Registry *registry = current();
  PluginEntry *entry = find_entry(registry, name);
  if (entry && entry->api && entry->api_size != size) return false;

  if (entry && entry->api) {
    size_t index = (size_t)(entry - registry->entries);
    if (!transaction || changed_api[index]) {
      memmove(entry->api, api, size);
      return true;
    }
  }

  void *copy = malloc(size);
  if (!copy) return false;
  memcpy(copy, api, size);
  if (!entry) entry = add_entry(registry, name);
  if (!entry) { free(copy); return false; }
  entry->api = copy;
  entry->api_size = size;
  if (transaction) changed_api[entry - registry->entries] = true;
  return true;
}

static void *get_or_create_state(const char *name, size_t size, size_t alignment,
                                  bool *created) {
  if (created) *created = false;
  if (!created || !name || !name[0] || !size || !alignment ||
      (alignment & (alignment - 1)) != 0) return NULL;
  Registry *registry = current();
  PluginEntry *entry = find_entry(registry, name);
  if (entry && entry->state) {
    if (entry->state_size != size || entry->state_alignment != alignment)
      return NULL;
    return entry->state;
  }

  size_t actual_alignment = alignment < sizeof(void *) ? sizeof(void *) : alignment;
  void *state = _aligned_malloc(size, actual_alignment);
  if (!state) return NULL;
  memset(state, 0, size);
  if (!entry) entry = add_entry(registry, name);
  if (!entry) { _aligned_free(state); return NULL; }
  entry->state = state;
  entry->state_size = size;
  entry->state_alignment = alignment;
  *created = true;
  return state;
}

bool cgf_api_registry_begin(void) {
  if (transaction) return false;
  pending = live;
  memset(changed_api, 0, sizeof(changed_api));
  transaction = true;
  return true;
}

void cgf_api_registry_commit(void) {
  if (!transaction) return;
  for (size_t i = 0; i < live.count; ++i) {
    if (changed_api[i] && live.entries[i].api) {
      memcpy(live.entries[i].api, pending.entries[i].api, live.entries[i].api_size);
      free(pending.entries[i].api);
      pending.entries[i].api = live.entries[i].api;
    }
  }
  live = pending;
  memset(&pending, 0, sizeof(pending));
  transaction = false;
}

static void discard_changes(const PluginEntry *entry, bool api, bool state,
                             bool name) {
  if (api) free(entry->api);
  if (state) _aligned_free(entry->state);
  if (name) free(entry->name);
}

void cgf_api_registry_cancel(void) {
  if (!transaction) return;
  Registry discarded = pending;
  memset(&pending, 0, sizeof(pending));
  transaction = false;
  for (size_t i = 0; i < discarded.count && i < PLUGIN_ENTRY_MAX; ++i) {
    PluginEntry entry = discarded.entries[i];
    bool new_entry = i >= live.count;
    bool new_state = new_entry || entry.state != live.entries[i].state;
    discard_changes(&entry, changed_api[i], new_state, new_entry);
  }
}

void cgf_api_registry_shutdown(void) {
  cgf_api_registry_cancel();
  for (size_t i = 0; i < live.count; ++i)
    discard_changes(&live.entries[i], true, true, true);
  memset(&live, 0, sizeof(live));
}

static const CGF_APIRegistry api_registry = {
    .set = set,
    .get = get,
    .get_or_create_state = get_or_create_state,
};

const CGF_APIRegistry *cgf_api_registry_get_api(void) { return &api_registry; }

