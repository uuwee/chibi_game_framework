#ifndef CGF_RUNTIME_H
#define CGF_RUNTIME_H

#include <stdbool.h>
#include <stdint.h>

typedef struct CGF_APIRegistry {
  bool (*set)(const char *name, size_t size, const void *api);
  const void *(*get)(const char *name, size_t size);

  void *(*get_or_create_state)(const char *name, size_t size, size_t alignment,
                               bool *created);
} CGF_APIRegistry;

const CGF_APIRegistry *cgf_api_registry_get_api(void);
#endif
