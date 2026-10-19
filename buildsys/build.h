#ifndef CGF_BUILDSYS_BUILD_H
#define CGF_BUILDSYS_BUILD_H

#include <stdint.h>

typedef enum BuildType {
  BUILD_TYPE_EXE,
  BUILD_TYPE_DLL,
} BuildType;

typedef struct BuildTarget {
  const BuildType type;
  const char *name;
  const char **source;
  const uint32_t source_count;
  const char *output;
  const char *option;
} BuildTarget;

int build_target(const BuildTarget *target);

#endif
