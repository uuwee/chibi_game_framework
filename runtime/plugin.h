#ifndef CGF_PLUGIN_H
#define CGF_PLUGIN_H

#include <stdbool.h>
#include <stddef.h>

typedef struct CGF_Plugin CGF_Plugin;

/* Creates an unloaded plugin. Copies the path and entry name; resolves the
   source path now so later working-directory changes do not affect reloads. */
CGF_Plugin *cgf_plugin_create(const wchar_t *path, const char *entry_name);

/* Load a private DLL copy and commit its registrations before unloading the
   previous copy. Failure leaves the previous DLL and registrations active.
   Call on the runtime thread, outside plugin callbacks. */
bool cgf_plugin_reload(CGF_Plugin *plugin);

/* Call plugin shutdown callbacks first. Registered function pointers become
   invalid after close; persistent state is released by registry shutdown. */
void cgf_plugin_close(CGF_Plugin *plugin);

#endif
