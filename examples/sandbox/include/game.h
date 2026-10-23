#ifndef SANDBOX_H
#define SANDBOX_H

#include "../../../runtime/include/runtime.h"

#define CGF_GAME_API_NAME "cgf.game"

typedef struct SandboxAPI {
  void (*update)(float dt);
  void (*render)(void);
  void (*shutdown)(void);
} SandboxAPI;

/* Registers the game API and reconnects this DLL to persistent state.
   Repeated loads preserve state; shutdown is called only at runtime exit. */
CGF_PLUGIN_EXPORT bool cgf_sandbox_load(const CGF_APIRegistry *registry);

#endif
