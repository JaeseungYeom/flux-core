#ifndef PLUGIN_H
#define PLUGIN_H

#include "flux.h"

/* Plugins will be connected to these well-known shared memory zmq sockets.
 */
#define REQUEST_URI         "inproc://request"
#define EVENT_URI           "inproc://event"

/* A plugin defines 'const struct plugin_ops ops = {...}' containing
 * its implementations of one or more of the plugin operations.
 */
struct plugin_ops {
    int (*main)(flux_t h, zhash_t *args);
};

typedef struct plugin_ctx_struct *plugin_ctx_t;

/* Load the specified plugin by 'name' and return a handle for it,
 * or NULL on failure.  We dlopen() the file "<name>srv.so".
 * 'searchpath' is a colon-separated list of directories to search.
 * 'rank' is the rank of this cmbd instance.
 * 'id' is a session-wide unique socket id for this instance of the plugin,
 * used to form the return address when the plugin a request on its dealer
 * socket.  'args' is a hash of key-value pairs that may be NULL, or may
 * be used to pass arguments to the plugins's ops->main() function.
 */
plugin_ctx_t plugin_load (flux_t h, const char *searchpath,
                          char *name, char *id, zhash_t *args);

/* Unload a plugin by handle.
 * (FIXME: This is not used yet and is a work in progress)
 */
void plugin_unload (plugin_ctx_t p);

/* Accessors.
 */
const char *plugin_name (plugin_ctx_t p);
const char *plugin_id (plugin_ctx_t p);
void *plugin_sock (plugin_ctx_t p);

#endif /* !PLUGIN_H */

/*
 * vi:tabstop=4 shiftwidth=4 expandtab
 */
