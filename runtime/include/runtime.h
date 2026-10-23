#ifndef CGF_RUNTIME_H
#define CGF_RUNTIME_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#if defined(_WIN32) && defined(CGF_PLUGIN_DLL)
#define CGF_PLUGIN_EXPORT __declspec(dllexport)
#else
#define CGF_PLUGIN_EXPORT
#endif

typedef struct CGF_APIRegistry {
  /* Copies the table. Replacing it requires the same size and keeps its address.
     Same-size ABI changes are not detected: keep names/layouts compatible. */
  bool (*set)(const char *name, size_t size, const void *api);
  /* NULL for an absent name or a size mismatch. Do not cache individual methods. */
  const void *(*get)(const char *name, size_t size);

  /* Zeroed on first creation; address remains valid until registry shutdown.
     A size/alignment mismatch fails. created is required and false on failure.
     Use namespaced names. Never keep DLL code/static pointers in persistent state. */
  void *(*get_or_create_state)(const char *name, size_t size, size_t alignment,
                               bool *created);
} CGF_APIRegistry;

const CGF_APIRegistry *cgf_api_registry_get_api(void);

/* Each plugin exports a uniquely named function of this type, so the same
   entry can also be linked statically. Return false before modifying existing
   state or external resources. Registration is committed by the loader. */
typedef bool (*CGF_PluginLoad)(const CGF_APIRegistry *registry);
#endif
