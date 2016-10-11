/*****************************************************************************\
 *  Copyright (c) 2014 Lawrence Livermore National Security, LLC.  Produced at
 *  the Lawrence Livermore National Laboratory (cf, AUTHORS, DISCLAIMER.LLNS).
 *  LLNL-CODE-658032 All rights reserved.
 *
 *  This file is part of the Flux resource manager framework.
 *  For details, see https://github.com/flux-framework.
 *
 *  This program is free software; you can redistribute it and/or modify it
 *  under the terms of the GNU General Public License as published by the Free
 *  Software Foundation; either version 2 of the license, or (at your option)
 *  any later version.
 *
 *  Flux is distributed in the hope that it will be useful, but WITHOUT
 *  ANY WARRANTY; without even the IMPLIED WARRANTY OF MERCHANTABILITY or
 *  FITNESS FOR A PARTICULAR PURPOSE.  See the terms and conditions of the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License along
 *  with this program; if not, write to the Free Software Foundation, Inc.,
 *  59 Temple Place, Suite 330, Boston, MA 02111-1307 USA.
 *  See also:  http://www.gnu.org/licenses/
\*****************************************************************************/

#if HAVE_CONFIG_H
#include "config.h"
#endif
#include <errno.h>
#include <stdbool.h>
#include <czmq.h>

#include "attr.h"
#include "rpc.h"

#include "src/common/libutil/shortjson.h"
#include "src/common/libutil/xzmalloc.h"

typedef struct {
    zhash_t *hash;
    zlist_t *names;
    flux_t *h;
} ctx_t;

typedef struct {
    char *val;
    int flags;
} attr_t;

static void freectx (void *arg)
{
    ctx_t *ctx = arg;
    zhash_destroy (&ctx->hash);
    zlist_destroy (&ctx->names);
    free (ctx);
}

static ctx_t *getctx (flux_t *h)
{
    ctx_t *ctx = flux_aux_get (h, "flux::attr");

    if (!ctx) {
        ctx = xzmalloc (sizeof (*ctx));
        if (!(ctx->hash = zhash_new ()))
            oom ();
        ctx->h = h;
        flux_aux_set (h, "flux::attr", ctx, freectx);
    }
    return ctx;
}

static void attr_destroy (void *arg)
{
    attr_t *attr = arg;
    free (attr->val);
    free (attr);
}

static attr_t *attr_create (const char *val, int flags)
{
    attr_t *attr = xzmalloc (sizeof (*attr));
    attr->val = xstrdup (val);
    attr->flags = flags;
    return attr;
}

static int attr_get_rpc (ctx_t *ctx, const char *name, attr_t **attrp)
{
    flux_rpc_t *r;
    json_object *in = Jnew ();
    json_object *out = NULL;
    const char *json_str, *val;
    int flags;
    attr_t *attr;
    int rc = -1;

    Jadd_str (in, "name", name);
    if (!(r = flux_rpc (ctx->h, "cmb.attrget", Jtostr (in),
                        FLUX_NODEID_ANY, 0)))
        goto done;
    if (flux_rpc_get (r, &json_str) < 0)
        goto done;
    if (!(out = Jfromstr (json_str)) || !Jget_str (out, "value", &val)
                                     || !Jget_int (out, "flags", &flags)) {
        errno = EPROTO;
        goto done;
    }
    attr = attr_create (val, flags);
    zhash_update (ctx->hash, name, attr);
    zhash_freefn (ctx->hash, name, attr_destroy);
    *attrp = attr;
    rc = 0;
done:
    Jput (in);
    Jput (out);
    flux_rpc_destroy (r);
    return rc;
}

static int attr_set_rpc (ctx_t *ctx, const char *name, const char *val)
{
    flux_rpc_t *r;
    json_object *in = Jnew ();
    attr_t *attr;
    int rc = -1;

    Jadd_str (in, "name", name);
    if (val)
        Jadd_str (in, "value", val);
    else
        Jadd_obj (in, "value", NULL);
    if (!(r = flux_rpc (ctx->h, "cmb.attrset", Jtostr (in),
                        FLUX_NODEID_ANY, 0)))
        goto done;
    if (flux_rpc_get (r, NULL) < 0)
        goto done;
    if (val) {
        attr = attr_create (val, 0);
        zhash_update (ctx->hash, name, attr);
        zhash_freefn (ctx->hash, name, attr_destroy);
    } else
        zhash_delete (ctx->hash, name);
    rc = 0;
done:
    Jput (in);
    flux_rpc_destroy (r);
    return rc;
}

#if CZMQ_VERSION < CZMQ_MAKE_VERSION(3,0,1)
static bool attr_strcmp (const char *s1, const char *s2)
{
    return (strcmp (s1, s2) > 0);
}
#else
static int attr_strcmp (const char *s1, const char *s2)
{
    return strcmp (s1, s2);
}
#endif

static int attr_list_rpc (ctx_t *ctx)
{
    flux_rpc_t *r;
    const char *json_str;
    json_object *array;
    json_object *out = NULL;
    int len, i, rc = -1;

    if (!(r = flux_rpc (ctx->h, "cmb.attrlist", NULL, FLUX_NODEID_ANY, 0)))
        goto done;
    if (flux_rpc_get (r, &json_str) < 0)
        goto done;
    if (!(out = Jfromstr (json_str)) || !Jget_obj (out, "names", &array)
                                     || !Jget_ar_len (array, &len)) {
        errno = EPROTO;
        goto done;
    }
    zlist_destroy (&ctx->names);
    if (!(ctx->names = zlist_new ()))
        oom ();
    for (i = 0; i < len; i++) {
        const char *name;
        if (!Jget_ar_str (array, i, &name)) {
            errno = EPROTO;
            goto done;
        }
        if (zlist_append (ctx->names, xstrdup (name)) < 0)
            oom ();
    }
    zlist_sort (ctx->names, (zlist_compare_fn *)attr_strcmp);
    rc = 0;
done:
    Jput (out);
    flux_rpc_destroy (r);
    return rc;
}

const char *flux_attr_get (flux_t *h, const char *name, int *flags)
{
    ctx_t *ctx = getctx (h);
    attr_t *attr;

    if (!(attr = zhash_lookup (ctx->hash, name))
                        || !(attr->flags & FLUX_ATTRFLAG_IMMUTABLE))
        if (attr_get_rpc (ctx, name, &attr) < 0)
            return NULL;
    if (flags && attr)
        *flags = attr->flags;
    return attr ? attr->val : NULL;
}

int flux_attr_set (flux_t *h, const char *name, const char *val)
{
    ctx_t *ctx = getctx (h);

    if (attr_set_rpc (ctx, name, val) < 0)
        return -1;
    return 0;
}

int flux_attr_fake (flux_t *h, const char *name, const char *val, int flags)
{
    ctx_t *ctx = getctx (h);
    attr_t *attr = attr_create (val, flags);
    zhash_update (ctx->hash, name, attr);
    zhash_freefn (ctx->hash, name, attr_destroy);
    return 0;
}

const char *flux_attr_first (flux_t *h)
{
    ctx_t *ctx = getctx (h);

    if (attr_list_rpc (ctx) < 0)
        return NULL;
    return ctx->names ? zlist_first (ctx->names) : NULL;
}

const char *flux_attr_next (flux_t *h)
{
    ctx_t *ctx = flux_aux_get (h, "flux::attr");

    return ctx->names ? zlist_next (ctx->names) : NULL;
}

/*
 * vi:tabstop=4 shiftwidth=4 expandtab
 */
