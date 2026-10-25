#ifndef CGF_BUILDSYS_BUILD_H
#define CGF_BUILDSYS_BUILD_H

typedef enum BuildType {
  BUILD_TYPE_EXE,
  BUILD_TYPE_DLL,
  /* Compiles each GLSL source with glslc into <output>/<file name>.inc, a C
     initializer list of SPIR-V words. output names a directory. */
  BUILD_TYPE_SHADERS,
} BuildType;

/* String lists are NULL-terminated; optional lists may be NULL.
   Every string except name may contain $(NAME), which is replaced with the
   environment variable NAME. An unset variable fails the build. */
typedef struct BuildTarget {
  BuildType type;
  const char *name; /* also names the object directory .build/obj/<name> */
  const char *output;
  const char *const *sources;
  const char *const *include_dirs; /* optional */
  const char *const *defines;      /* optional, NAME or NAME=VALUE */
  const char *const *link_options; /* optional */
  const struct BuildTarget *const *dependencies; /* optional, built first */
} BuildTarget;

int build_target(const BuildTarget *target);

#endif
