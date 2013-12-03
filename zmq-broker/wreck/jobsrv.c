
#define _GNU_SOURCE
#include <stdio.h>
#include <zmq.h>
#include <czmq.h>
#include <inttypes.h>
#include <json/json.h>

#include "zmsg.h"
#include "log.h"
#include "util.h"
#include "plugin.h"

#if 0
zlist_t * kvs_jobid_list (plugin_ctx_t *p)
{
    zlist_t *zl = NULL;
    json_object_iter i;
    json_object *val = NULL;
    json_object *reply = NULL;
    json_object *request = util_json_object_new_object ();

    json_object_object_add (request, "lwj.id-list", NULL);

    reply = flux_rpc (p, request, "kvs.get.dir");
    if (!reply) {
        err ("kvs_job_list: plugin request failed!");
        goto done;
    }
    if (!(val = json_object_object_get (reply, "lwj.ids"))) {
        errno = ENOENT;
        err ("kvs_job_list: %s", strerror (errno));
        goto done;
    }
    if (!(zl = zlist_new ())) {
        errno = ENOMEM;
        goto done;
    }

    json_object_object_foreachC (val, i) {
        json_object *co;
        if ((co = json_object_object_get (i.val, "FILEVAL"))) {
            zlist_append (zl,
                strdup (json_object_to_json_string_ext (co,
                            JSON_C_TO_STRING_PLAIN)));
        }
    }

done:
    if (request)
        json_object_put (request);
    if (reply)
        json_object_put (reply);

    return (zl);
}

void jobid_list_free (zlist_t *list)
{
    void *s;
    while ((s = zlist_pop (list)))
        free (s);
    zlist_destroy (&list);
}

#endif

static int kvs_job_new (flux_t h, unsigned long jobid)
{
    int rc;
    char *key;

    if (asprintf (&key, "lwj.%lu.state", jobid) < 0)
        return (-1);

    rc = kvs_put_string (h, key, "reserved");
    kvs_commit (h);

    free (key);
    return rc;
}

/*
 *  Set a new value for lwj.next-id and commit
 */
static int set_next_jobid (flux_t h, unsigned long jobid)
{
    int rc;
    if ((rc = kvs_put_int64 (h, "lwj.next-id", jobid)) < 0) {
        err ("kvs_put: %s", strerror (errno));
        return -1;
    }
    return kvs_commit (h);
}

/*
 *  Get and increment lwj.next-id (called from treeroot only)
 */
static unsigned long increment_jobid (flux_t h)
{
    int64_t ret;
    int rc;
    rc = kvs_get_int64 (h, "lwj.next-id", &ret);
    if (rc < 0 && (errno == ENOENT))
        ret = 1;
    set_next_jobid (h, ret+1);
    return (unsigned long) ret;
}

/*
 *  Tree-wide call for lwj_next_id. If not treeroot forward request
 *   up the tree. Otherwise increment jobid and return result.
 */
static unsigned long lwj_next_id (flux_t h)
{
    unsigned long ret;
    if (flux_treeroot (h))
        ret = increment_jobid (h);
    else {
        json_object *na = json_object_new_object ();
        json_object *o = flux_rpc (h, na, "job.next-id");
        if (util_json_object_get_int64 (o, "id", (int64_t *) &ret) < 0) {
            err ("lwj_next_id: Bad object!");
            ret = 0;
        }
        json_object_put (o);
        json_object_put (na);
    }
    return (ret);
}

/*
 *  create json object : { id: <int64>id }
 */
static json_object *json_id (int id)
{
    json_object *o = json_object_new_object ();
    util_json_object_add_int64 (o, "id", id);
    return (o);
}

static char * ctime_iso8601_now (char *buf, size_t sz)
{
    struct tm tm;
    time_t now = time (NULL);

    memset (buf, 0, sz);

    if (!localtime_r (&now, &tm))
        err_exit ("localtime");
    strftime (buf, sz, "%FT%T", &tm);

    return (buf);
}

static void add_jobinfo (flux_t h, int64_t id, json_object *req)
{
    char buf [64];
    json_object_iter i;
    json_object *o;
    kvsdir_t dir;

    if (kvs_get_dir (h, &dir, "lwj.%lu", id) < 0)
        err_exit ("kvs_get_dir (id=%lu)", id);

    json_object_object_foreachC (req, i)
        kvsdir_put (dir, i.key, i.val);

    o = json_object_new_string (ctime_iso8601_now (buf, sizeof (buf)));
    kvsdir_put (dir, "create-time", o);
    json_object_put (o);

    kvsdir_destroy (dir);
}

static int handle_recv (flux_t h, zmsg_t **zmsg, int typemask)
{
    json_object *o = NULL;
    char *tag;

    if (cmb_msg_decode (*zmsg, &tag, &o) >= 0) {
        if (strcmp (tag, "job.next-id") == 0) {
            if (flux_treeroot (h)) {
                unsigned long id = lwj_next_id (h);
                json_object *ox = json_id (id);
                flux_respond (h, zmsg, ox);
                json_object_put (o);
            }
            else {
                fprintf (stderr, "%s: forwarding request\n", tag);
                flux_request_send (h, o, tag);
            }
        }
        if (strcmp (tag, "job.create") == 0) {
            json_object *jobinfo = NULL;
            unsigned long id = lwj_next_id (h);
            int rc = kvs_job_new (h, id);
            if (rc < 0) {
                flux_respond_errnum (h, zmsg, errno);
                goto out;
            }
            add_jobinfo (h, id, o);

            kvs_commit (h);

            /* Generate reply with new jobid */
            jobinfo = util_json_object_new_object ();
            util_json_object_add_int64 (jobinfo, "jobid", id);
            flux_respond (h, zmsg, jobinfo);
            json_object_put (jobinfo);
        }
    }

out:
    if (o)
        json_object_put (o);
    zmsg_destroy (zmsg);
    return 0;
}

const struct plugin_ops ops = {
    .recv = handle_recv,
};

/*
 * vi: ts=4 sw=4 expandtab
 */
