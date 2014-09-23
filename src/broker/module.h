#ifndef _BROKER_MODULE_H
#define _BROKER_MODULE_H

/* Plugins will be connected to these well-known shared memory zmq sockets.
 */
#define REQUEST_URI         "inproc://request"
#define EVENT_URI           "inproc://event"

/* Create, start, stop, destroy a plugin.
 * Termination:  plugin_stop (), read EOF on sock, plugin_destroy ()
 */
typedef struct plugin_ctx_struct *plugin_ctx_t;
plugin_ctx_t plugin_create (flux_t h, const char *path, zhash_t *args);
void plugin_start (plugin_ctx_t p);
void plugin_stop (plugin_ctx_t p);
void plugin_destroy (plugin_ctx_t p);


/* Accessors.
 */
const char *plugin_name (plugin_ctx_t p);
const char *plugin_uuid (plugin_ctx_t p);
const char *plugin_digest (plugin_ctx_t p);
int plugin_size (plugin_ctx_t p);
void *plugin_sock (plugin_ctx_t p);

#endif /* !_BROKER_MODULE_H */

/*
 * vi:tabstop=4 shiftwidth=4 expandtab
 */
