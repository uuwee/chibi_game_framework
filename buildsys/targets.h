#ifndef CGF_BUILDSYS_TARGETS_H
#define CGF_BUILDSYS_TARGETS_H

#include "build.h"

/* Shared by the build runner and the runtime, which rebuilds plugins. */
extern const BuildTarget target_self;
extern const BuildTarget target_runtime;
extern const BuildTarget target_sandbox;
extern const BuildTarget target_renderer_shaders;
extern const BuildTarget target_renderer; /* builds its shaders first */

#endif
