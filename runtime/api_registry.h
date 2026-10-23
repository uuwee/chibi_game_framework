#ifndef CGF_API_REGISTRY_H
#define CGF_API_REGISTRY_H

#include "include/runtime.h"

/* Runtime-only operations. A plugin load stages API registrations and new
   state allocations; existing state must not be modified until load succeeds. */
bool cgf_api_registry_begin(void);
void cgf_api_registry_commit(void);
void cgf_api_registry_cancel(void);
void cgf_api_registry_shutdown(void);

#endif
