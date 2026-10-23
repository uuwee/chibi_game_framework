#ifndef SANDBOX_H
#define SANDBOX_H

#include "../../../runtime/include/runtime.h"

#define CGF_GAME_API_NAME "cgf.game"

typedef struct SandboxAPI {
  void (*update)(float dt);
  void (*render)(void);
  void (*shutdown)(void);
} SandboxAPI;

#endif
