/************************************************************\
 * Copyright 2018 Lawrence Livermore National Security, LLC
 * (c.f. AUTHORS, NOTICE.LLNS, COPYING)
 *
 * This file is part of the Flux resource manager framework.
 * For details, see https://github.com/flux-framework.
 *
 * SPDX-License-Identifier: LGPL-3.0
\************************************************************/

#if HAVE_CONFIG_H
#include "config.h"
#endif
#include <unistd.h>
#include <czmq.h>
#include <jansson.h>
#include <flux/core.h>
#if HAVE_FLUX_SECURITY
#include <flux/security/context.h>
#include <flux/security/sign.h>
#endif

#include "src/common/libutil/fluid.h"
#include "src/common/libjob/sign_none.h"
#include "src/common/libeventlog/eventlog.h"

#include "validate.h"

/* job-ingest takes in signed jobspec submitted through flux_job_submit(),
 * performing the following tasks for each job:
 *
 * 1) verify that submitting userid == userid that signed jobspec
 * 2) verify that enclosed jobspec is valid per RFC 14
 * 3) assign jobid using distributed 64-bit FLUID generator
 * 4) commit job data to KVS per RFC 16 (KVS Job Schema)
 * 5) make "job-manager.submit" request announcing new jobid
 *
 * For performance, the above actions are batched, so that if job requests
 * arrive within the 'batch_timeout' window, they are combined into one
 * KVS transaction and one job-manager request.
 *
 * The jobid is returned to the user in response to the job-ingest.submit RPC.
 * Responses are sent after the job has been successfully ingested.
 *
 * Currently all KVS data is committed under job.<fluid-dothex>,
 * where <fluid-dothex> is the jobid converted to 16-bit, 0-padded hex
 * strings delimited by periods, e.g.
 *   job.0000.0004.b200.0000
 *
 * The job-ingest module can be loaded on rank 0, or on many ranks across
 * the instance, rank < max FLUID id of 16384.  Each rank is relatively
 * independent and KVS commit scalability will ultimately limit the max
 * ingest rate for an instance.
 *
 * Security: any user with FLUX_ROLE_USER may submit jobs.  The jobspec
 * must be signed, but this module (running as the instance owner) doesn't
 * need to authenticate the signature.  It merely unwraps the contents,
 * and checks that the security envelope claims the same userid as the
 * userid stamped on the request message, which was authenticated by the
 * connector.
 */


/* The batch_timeout (seconds) is the maximum length of time
 * any given job request is delayed before initiating a KVS commit.
 * Too large, and individual job submit latency will suffer.
 * Too small, and KVS commit overhead will increase.
 */
const double batch_timeout = 0.01;

/* Timeout (seconds) to wait for validators to terminate when
 * stopped by closing their stdin.  If the timer pops, stop the reactor
 * and allow validate_destroy() to signal them.
 */
const double shutdown_timeout = 5.;


struct job_ingest_ctx {
    flux_t *h;
    struct validate *validate;
#if HAVE_FLUX_SECURITY
    flux_security_t *sec;
#endif
    struct fluid_generator gen;
    flux_msg_handler_t **handlers;

    struct batch *batch;
    flux_watcher_t *timer;

    bool shutdown;              // no new jobs are accepted in shutdown mode
    int shutdown_process_count; // number of validators executing at shutdown
    flux_watcher_t *shutdown_timer;
};

struct job {
    fluid_t id;         // jobid

    const flux_msg_t *msg; // submit request message
    const char *J;      // signed jobspec
    struct flux_msg_cred cred;    // submitting user's creds
    int priority;       // requested job priority
    int flags;          // submit flags

    char *jobspec;      // jobspec, not \0 terminated (unwrapped from signed)
    int jobspecsz;      // jobspec string length

    struct job_ingest_ctx *ctx;
};

struct batch {
    struct job_ingest_ctx *ctx;
    flux_kvs_txn_t *txn;
    zlist_t *jobs;
    json_t *joblist;
};

static int make_key (char *buf, int bufsz, struct job *job, const char *name);

/* Free decoded jobspec after it has been transferred to the batch txn,
 * to conserve memory.
 */
static void job_clean (struct job *job)
{
    if (job) {
        free (job->jobspec);
        job->jobspec = NULL;
    }
}

static void job_destroy (struct job *job)
{
    if (job) {
        int saved_errno = errno;
        free (job->jobspec);
        flux_msg_decref (job->msg);
        free (job);
        errno = saved_errno;
    }
}

static struct job *job_create (const flux_msg_t *msg,
                               struct job_ingest_ctx *ctx)
{
    struct job *job;

    if (!(job = calloc (1, sizeof (*job))))
        return NULL;
    job->msg = flux_msg_incref (msg);
    if (flux_request_unpack (job->msg, NULL, "{s:s s:i s:i}",
                             "J", &job->J,
                             "priority", &job->priority,
                             "flags", &job->flags) < 0)
        goto error;
    if (flux_msg_get_cred (job->msg, &job->cred) < 0)
        goto error;
    job->ctx = ctx;
    return job;
error:
    job_destroy (job);
    return NULL;
}

static void batch_destroy (struct batch *batch)
{
    if (batch) {
        int saved_errno = errno;
        if (batch->jobs) {
            struct job *job;
            while ((job = zlist_pop (batch->jobs)))
                job_destroy (job);
            zlist_destroy (&batch->jobs);
            json_decref (batch->joblist);
            flux_kvs_txn_destroy (batch->txn);
        }
        free (batch);
        errno = saved_errno;
    }
}

/* Create a 'struct batch', a container for a group of job submit
 * requests.  Prepare a KVS transaction and a json array of jobid's
 * to be used for job-manager.submit request.
 */
static struct batch *batch_create (struct job_ingest_ctx *ctx)
{
    struct batch *batch;

    if (!(batch = calloc (1, sizeof (*batch))))
        return NULL;
    if (!(batch->jobs = zlist_new ()))
        goto nomem;
    if (!(batch->txn = flux_kvs_txn_create ()))
        goto error;
    if (!(batch->joblist = json_array ()))
        goto nomem;
    batch->ctx = ctx;
    return batch;
nomem:
    errno = ENOMEM;
error:
    batch_destroy (batch);
    return NULL;
}

/* Respond to all requestors (for each job) with errnum and errstr (required).
 */
static void batch_respond_error (struct batch *batch,
                                 int errnum, const char *errstr)
{
    flux_t *h = batch->ctx->h;
    struct job *job = zlist_first (batch->jobs);
    while (job) {
        if (flux_respond_error (h, job->msg, errnum, errstr) < 0)
            flux_log_error (h, "%s: flux_respond_error", __FUNCTION__);
        job = zlist_next (batch->jobs);
    }
}

/* Respond to all requestors (for each job) with their id.
 */
static void batch_respond_success (struct batch *batch)
{
    flux_t *h = batch->ctx->h;
    struct job *job = zlist_first (batch->jobs);
    while (job) {
        if (flux_respond_pack (h, job->msg, "{s:I}", "id", job->id) < 0)
            flux_log_error (h, "%s: flux_respond_pack", __FUNCTION__);
        job = zlist_next (batch->jobs);
    }
}

static void batch_cleanup_continuation (flux_future_t *f, void *arg)
{
    flux_t *h = flux_future_get_flux (f);

    if (flux_future_get (f, NULL) < 0)
        flux_log_error (h, "%s: KVS commit failed", __FUNCTION__);
    flux_future_destroy (f);
}

/* Remove KVS job entries previously committed for all jobs in batch.
 */
static int batch_cleanup (struct batch *batch)
{
    flux_t *h = batch->ctx->h;
    flux_kvs_txn_t *txn;
    struct job *job;
    flux_future_t *f = NULL;
    char key[64];

    if (!(txn = flux_kvs_txn_create ()))
        return -1;
    job = zlist_first (batch->jobs);
    while (job) {
        if (make_key (key, sizeof (key), job, NULL) < 0)
            goto error;
        if (flux_kvs_txn_unlink (txn, 0, key) < 0)
            goto error;
        job = zlist_next (batch->jobs);
    }
    if (!(f = flux_kvs_commit (h, NULL, 0, txn)))
        goto error;
    if (flux_future_then (f, -1., batch_cleanup_continuation, NULL) < 0)
        goto error;
    flux_kvs_txn_destroy (txn);
    return 0;
error:
    flux_kvs_txn_destroy (txn);
    flux_future_destroy (f);
    return -1;
}

/* Get result of announcing job(s) to job manager,
 * and respond to submit request(s).
 */
static void batch_announce_continuation (flux_future_t *f, void *arg)
{
    struct batch *batch = arg;
    flux_t *h = batch->ctx->h;

    if (flux_future_get (f, NULL) < 0) {
        batch_respond_error (batch, errno, future_strerror (f, errno));
        if (batch_cleanup (batch) < 0)
            flux_log_error (h, "%s: KVS cleanup failure", __FUNCTION__);
    }
    else
        batch_respond_success (batch);

    batch_destroy (batch);
    flux_future_destroy (f);
}

/* Announce job(s) to job manager.
 */
static void batch_announce (struct batch *batch)
{
    flux_t *h = batch->ctx->h;
    flux_future_t *f;

    if (!(f = flux_rpc_pack (h, "job-manager.submit", FLUX_NODEID_ANY, 0,
                             "{s:O}",
                             "jobs", batch->joblist)))
        goto error;
    if (flux_future_then (f, -1., batch_announce_continuation, batch) < 0)
        goto error;
    return;
error:
    flux_log_error (h, "%s: error sending RPC", __FUNCTION__);
    batch_respond_error (batch, errno, "error sending job-manager.submit RPC");
    if (batch_cleanup (batch) < 0)
        flux_log_error (h, "%s: KVS cleanup failure", __FUNCTION__);
    batch_destroy (batch);
    flux_future_destroy (f);
}

/* Get result of KVS commit.
 * If successful, announce job(s) to job-manager.
 */
static void batch_flush_continuation (flux_future_t *f, void *arg)
{
    struct batch *batch = arg;

    if (flux_future_get (f, NULL) < 0) {
        batch_respond_error (batch, errno, "KVS commit failed");
        batch_destroy (batch);
    }
    else {
        batch_announce (batch);
    }
    flux_future_destroy (f);
}

/* batch timer - expires 'batch_timeout' seconds after batch was created.
 * Replace ctx->batch with a NULL, and pass 'batch' off to a chain of
 * continuations that commit its data to the KVS, respond to requestors,
 * and announce the new jobids.
 */
static void batch_flush (flux_reactor_t *r, flux_watcher_t *w,
                         int revents, void *arg)
{
    struct job_ingest_ctx *ctx = arg;
    struct batch *batch;
    flux_future_t *f;

    batch = ctx->batch;
    ctx->batch = NULL;

    if (!(f = flux_kvs_commit (ctx->h, NULL, 0, batch->txn))) {
        batch_respond_error (batch, errno, "flux_kvs_commit failed");
        goto error;
    }
    if (flux_future_then (f, -1., batch_flush_continuation, batch) < 0) {
        batch_respond_error (batch, errno, "flux_future_then (kvs) failed");
        flux_future_destroy (f);
        if (batch_cleanup (batch) < 0)
            flux_log_error (ctx->h, "%s: KVS cleanup failure", __FUNCTION__);
        goto error;
    }
    return;
error:
    batch_destroy (batch);
}

/* Format key within the KVS directory of 'job'.
 */
static int make_key (char *buf, int bufsz, struct job *job, const char *name)
{
    if (flux_job_kvs_key (buf, bufsz, job->id, name) < 0) {
        errno = EINVAL;
        return -1;
    }
    return 0;
}

/* Add 'job' to 'batch'.
 * On error, ensure that no remnants of job made into KVS transaction.
 */
static int batch_add_job (struct batch *batch, struct job *job)
{
    char key[64];
    int saved_errno;
    json_t *jobentry;
    json_t *entry = NULL;
    char *entrystr = NULL;
    double t;

    if (zlist_append (batch->jobs, job) < 0) {
        errno = ENOMEM;
        return -1;
    }
    if (make_key (key, sizeof (key), job, "J") < 0)
        goto error;
    if (flux_kvs_txn_put (batch->txn, 0, key, job->J) < 0)
        goto error;
    if (make_key (key, sizeof (key), job, "jobspec") < 0)
        goto error;
    if (flux_kvs_txn_put_raw (batch->txn, 0, key,
                              job->jobspec, job->jobspecsz) < 0)
        goto error;
    entry = eventlog_entry_pack (0., "submit",
                                 "{ s:i s:i s:i }",
                                 "userid", job->cred.userid,
                                 "priority", job->priority,
                                 "flags", job->flags);
    if (!entry)
        goto error;
    if (!(entrystr = eventlog_entry_encode (entry)))
        goto error;
    if (make_key (key, sizeof (key), job, "eventlog") < 0)
        goto error;
    if (flux_kvs_txn_put (batch->txn, FLUX_KVS_APPEND, key, entrystr) < 0)
        goto error;
    /* get created timestamp in eventlog entry for job */
    if (eventlog_entry_parse (entry, &t, NULL, NULL) < 0)
        goto error;
    if (!(jobentry = json_pack ("{s:I s:i s:i s:f s:i}",
                                "id", job->id,
                                "userid", job->cred.userid,
                                "priority", job->priority,
                                "t_submit", t,
                                "flags", job->flags)))
        goto nomem;
    if (json_array_append_new (batch->joblist, jobentry) < 0) {
        json_decref (jobentry);
        goto nomem;
    }
    free (entrystr);
    json_decref (entry);
    job_clean (job); // batch->txn now holds this info
    return 0;
nomem:
    errno = ENOMEM;
error:
    saved_errno = errno;
    free (entrystr);
    json_decref (entry);
    zlist_remove (batch->jobs, job);
    if (make_key (key, sizeof (key), job, NULL) == 0)
        (void)flux_kvs_txn_unlink (batch->txn, 0, key);
    errno = saved_errno;
    return -1;
}

void validate_continuation (flux_future_t *f, void *arg)
{
    struct job *job = arg;
    struct job_ingest_ctx *ctx = job->ctx;
    flux_t *h = flux_future_get_flux (f);
    const char *errmsg = NULL;

    /* If jobspec validation failed, respond immediately to the user.
     */
    if (flux_future_get (f, NULL) < 0) {
        errmsg = future_strerror (f, errno);
        goto error;
    }
    if (fluid_generate (&ctx->gen, &job->id) < 0)
        goto error;
    /* Add job to the current "batch" of new jobs, creating the batch if
     * one doesn't exist already.  Submit is finalized upon timer expiration.
     */
    if (!ctx->batch) {
        if (!(ctx->batch = batch_create (ctx)))
            goto error;
        flux_timer_watcher_reset (ctx->timer, batch_timeout, 0.);
        flux_watcher_start (ctx->timer);
    }
    if (batch_add_job (ctx->batch, job) < 0)
        goto error;
    flux_future_destroy (f);
    return;
error:
    if (flux_respond_error (h, job->msg, errno, errmsg) < 0)
        flux_log_error (h, "%s: flux_respond_error", __FUNCTION__);
    job_destroy (job);
    flux_future_destroy (f);
}

static int valid_flags (int flags)
{
    int allowed = FLUX_JOB_DEBUG | FLUX_JOB_WAITABLE;
    if ((flags & ~allowed)) {
        errno = EPROTO;
        return -1;
    }
    return 0;
}

/* Handle "job-ingest.submit" request to add a new job.
 */
static void submit_cb (flux_t *h, flux_msg_handler_t *mh,
                       const flux_msg_t *msg, void *arg)
{
    struct job_ingest_ctx *ctx = arg;
    struct job *job = NULL;
    const char *errmsg = NULL;
    char errbuf[256];
    int64_t userid_signer;
    const char *mech_type;
    flux_future_t *f = NULL;

    if (ctx->shutdown) {
        errno = ENOSYS;
        goto error;
    }

    /* Parse request.
     */
    if (!(job = job_create (msg, ctx)))
        goto error;
    /* Validate submit flags.
     */
    if (valid_flags (job->flags) < 0)
        goto error;
    /* Validate requested job priority.
     */
    if (job->priority < FLUX_JOB_PRIORITY_MIN
            || job->priority > FLUX_JOB_PRIORITY_MAX) {
        snprintf (errbuf, sizeof (errbuf), "priority range is [%d:%d]",
                  FLUX_JOB_PRIORITY_MIN, FLUX_JOB_PRIORITY_MAX);
        errmsg = errbuf;
        errno = EINVAL;
        goto error;
    }
    if (!(job->cred.rolemask & FLUX_ROLE_OWNER)
           && job->priority > FLUX_JOB_PRIORITY_DEFAULT) {
        snprintf (errbuf, sizeof (errbuf),
                  "only the instance owner can submit with priority >%d",
                  FLUX_JOB_PRIORITY_DEFAULT);
        errmsg = errbuf;
        errno = EINVAL;
        goto error;
    }
    /* Only owner can set FLUX_JOB_WAITABLE.
     */
    if (!(job->cred.rolemask & FLUX_ROLE_OWNER)
            && (job->flags & FLUX_JOB_WAITABLE)) {
        snprintf (errbuf,
                  sizeof (errbuf),
                  "only the instance onwer can submit with FLUX_JOB_WAITABLE");
        errmsg = errbuf;
        errno = EINVAL;
        goto error;
    }
    /* Validate jobspec signature, and unwrap(J) -> jobspec,  jobspecsz.
     * Userid claimed by signature must match authenticated job->cred.userid.
     * If not the instance owner, a strong signature is required
     * to give the IMP permission to launch processes on behalf of the user.
     */
#if HAVE_FLUX_SECURITY
    const void *jobspec;
    if (flux_sign_unwrap_anymech (ctx->sec, job->J, &jobspec, &job->jobspecsz,
                                  &mech_type, &userid_signer,
                                  FLUX_SIGN_NOVERIFY) < 0) {
        errmsg = flux_security_last_error (ctx->sec);
        goto error;
    }
    if (!(job->jobspec = malloc (job->jobspecsz)))
        goto error;
    memcpy (job->jobspec, jobspec, job->jobspecsz);
#else
    uint32_t userid_signer_u32;
    /* Simplified unwrap only understands mech=none.
     * Unlike flux-security version, returned payload must be freed,
     * and returned userid is a uint32_t.
     */
    if (sign_none_unwrap (job->J, (void **)&job->jobspec, &job->jobspecsz,
                          &userid_signer_u32) < 0) {
        errmsg = "could not unwrap jobspec";
        goto error;
    }
    mech_type = "none";
    userid_signer = userid_signer_u32;
#endif
    if (userid_signer != job->cred.userid) {
        snprintf (errbuf, sizeof (errbuf),
                  "signer=%lu != requestor=%lu",
                  (unsigned long)userid_signer,
                  (unsigned long)job->cred.userid);
        errmsg = errbuf;
        errno = EPERM;
        goto error;
    }
    if (!(job->cred.rolemask & FLUX_ROLE_OWNER)
                                && !strcmp (mech_type, "none")) {
        snprintf (errbuf, sizeof (errbuf),
                  "only instance owner can use sign-type=none");
        errmsg = errbuf;
        errno = EPERM;
        goto error;
    }
    /* Validate jobspec asynchronously.
     * Continue submission process in validate_continuation().
     */
    if (!(f = validate_jobspec (ctx->validate, job->jobspec, job->jobspecsz)))
        goto error;
    if (flux_future_then (f, -1., validate_continuation, job) < 0)
        goto error;
    return;
error:
    if (flux_respond_error (h, msg, errno, errmsg) < 0)
        flux_log_error (h, "%s: flux_respond_error", __FUNCTION__);
    job_destroy (job);
    flux_future_destroy (f);
}

static void exit_cb (void *arg)
{
    struct job_ingest_ctx *ctx = arg;

    if (--ctx->shutdown_process_count == 0) {
        flux_watcher_stop (ctx->shutdown_timer);
        flux_reactor_stop (flux_get_reactor (ctx->h));
    }
}

static void shutdown_timeout_cb (flux_reactor_t *r,
                                 flux_watcher_t *w,
                                 int revents,
                                 void *arg)
{
    struct job_ingest_ctx *ctx = arg;

    flux_log (ctx->h,
              LOG_ERR,
              "shutdown timed out with %d validators active",
              ctx->shutdown_process_count);
    flux_reactor_stop (flux_get_reactor (ctx->h));
}

/* Override built-in shutdown handler that calls flux_reactor_stop().
 * Since libsubprocess client is not able ot run outside of the reactor,
 * take care of cleaning up validator before exiting reactor.
 */
static void shutdown_cb (flux_t *h,
                         flux_msg_handler_t *mh,
                         const flux_msg_t *msg,
                         void *arg)
{
    struct job_ingest_ctx *ctx = arg;

    ctx->shutdown = true; // fail any new submit requests
    ctx->shutdown_process_count = validate_stop_notify (ctx->validate,
                                                        exit_cb,
                                                        ctx);
    if (ctx->shutdown_process_count == 0)
        flux_reactor_stop (flux_get_reactor (h));
    else {
        flux_timer_watcher_reset (ctx->shutdown_timer, shutdown_timeout, 0.);
        flux_watcher_start (ctx->shutdown_timer);
    }
}

static void getinfo_cb (flux_t *h,
                        flux_msg_handler_t *mh,
                        const flux_msg_t *msg,
                        void *arg)
{
    struct job_ingest_ctx *ctx = arg;
    uint64_t timestamp;

    if (flux_request_decode (msg, NULL, NULL) < 0)
        goto error;
    if (fluid_save_timestamp (&ctx->gen, &timestamp) < 0) {
        errno = EOVERFLOW; // punt: which is more likely, clock_gettime()
                           //       failure or flux running for 35 years?
        goto error;
    }
    if (flux_respond_pack (h, msg, "{s:I}", "timestamp", timestamp) < 0)
        flux_log_error (h, "%s: flux_respond_pack", __FUNCTION__);
    return;
error:
    if (flux_respond_error (h, msg, errno, NULL) < 0)
        flux_log_error (h, "%s: flux_respond_error", __FUNCTION__);
}

static const struct flux_msg_handler_spec htab[] = {
    { FLUX_MSGTYPE_REQUEST,  "job-ingest.getinfo", getinfo_cb, 0},
    { FLUX_MSGTYPE_REQUEST,  "job-ingest.submit", submit_cb, FLUX_ROLE_USER },
    { FLUX_MSGTYPE_REQUEST,  "job-ingest.shutdown", shutdown_cb, 0 },
    FLUX_MSGHANDLER_TABLE_END,
};

/* Configure the validator.
 * Use compiled in path and string for validator program and args,
 * unless overridden with validator=path or schema=path on module load
 * command line.
 */
int validate_initialize (flux_t *h,
                         int argc,
                         char **argv,
                         struct validate **validate)
{
    const char *usage_message = "Usage: flux module load [OPTIONS] job-ingest "
                                " [validator-args=ARGS] [validator=PATH]";
    const char *valpath;
    const char *valargs;
    struct validate *v;
    int i;

    valpath = flux_conf_builtin_get ("jobspec_validate_path", FLUX_CONF_AUTO);
    valargs = flux_conf_builtin_get ("jobspec_validator_args", FLUX_CONF_AUTO);
    for (i = 0; i < argc; i++) {
        if (!strncmp (argv[i], "validator-args=", 15)) {
            valargs = argv[i] + 15;
        }
        else if (!strncmp (argv[i], "validator=", 10)) {
            valpath = argv[i] + 10;
            if (access (valpath, X_OK) < 0) {
                flux_log_error (h, "validator %s", valpath);
                return -1;
            }
        }
        else {
            flux_log (h, LOG_ERR, "invalid option %s", argv[i]);
            flux_log (h, LOG_ERR, "%s", usage_message);
            errno = EINVAL;
            return -1;
        }
    }
    if (!(v = validate_create (h, valpath, valargs))) {
        flux_log_error (h, "validate_create");
        return -1;
    }
    *validate = v;
    return 0;
}

int mod_main (flux_t *h, int argc, char **argv)
{
    flux_reactor_t *r = flux_get_reactor (h);
    int rc = -1;
    struct job_ingest_ctx ctx;
    uint32_t rank;

    memset (&ctx, 0, sizeof (ctx));
    ctx.h = h;
#if HAVE_FLUX_SECURITY
    if (!(ctx.sec = flux_security_create (0))) {
        flux_log_error (h, "flux_security_create");
        goto done;
    }
    if (flux_security_configure (ctx.sec, NULL) < 0) {
        flux_log_error (h, "flux_security_configure: %s",
                        flux_security_last_error (ctx.sec));
        goto done;
    }
#endif
    if (flux_msg_handler_addvec (h, htab, &ctx, &ctx.handlers) < 0) {
        flux_log_error (h, "flux_msghandler_add");
        goto done;
    }
    if (!(ctx.timer = flux_timer_watcher_create (r, 0., 0.,
                                                 batch_flush, &ctx))) {
        flux_log_error (h, "flux_timer_watcher_create");
        goto done;
    }
    if (!(ctx.shutdown_timer = flux_timer_watcher_create (r,
                                                          0.,
                                                          0.,
                                                          shutdown_timeout_cb,
                                                          &ctx))) {
        flux_log_error (h, "flux_timer_watcher_create");
        goto done;
    }
    if (flux_get_rank (h, &rank) < 0) {
        flux_log_error (h, "flux_get_rank");
        goto done;
    }
    /* Initialize FLUID generator.
     * On rank 0, derive the starting timestamp from the job manager's
     * 'max_jobid' plus one.  On other ranks, ask upstream job-ingest.
     */
    if (rank == 0) {
        flux_future_t *f;
        flux_jobid_t max_jobid;

        if (!(f = flux_rpc (h, "job-manager.getinfo", NULL, 0, 0))) {
            flux_log_error (h, "flux_rpc");
            goto done;
        }
        if (flux_rpc_get_unpack (f, "{s:I}", "max_jobid", &max_jobid) < 0) {
            if (errno == ENOSYS)
                flux_log_error (h, "job-manager must be loaded first");
            else
                flux_log_error (h, "job-manager.getinfo");
            flux_future_destroy (f);
            goto done;
        }
        flux_future_destroy (f);
        if (fluid_init (&ctx.gen, 0, fluid_get_timestamp (max_jobid) + 1) < 0) {
            flux_log (h, LOG_ERR, "fluid_init failed");
            errno = EINVAL;
        }
    }
    else {
        flux_future_t *f;
        uint64_t timestamp;

        if (!(f = flux_rpc (h, "job-ingest.getinfo", NULL, 0, 0))) {
            flux_log_error (h, "flux_rpc");
            goto done;
        }
        if (flux_rpc_get_unpack (f, "{s:I}", "timestamp", &timestamp) < 0) {
            if (errno == ENOSYS)
                flux_log_error (h, "job-ingest must be loaded on rank 0 first");
            else
                flux_log_error (h, "job-ingest.getinfo");
            flux_future_destroy (f);
            goto done;
        }
        flux_future_destroy (f);
        /* fluid_init() will fail on rank > 16K.
         * Just skip loading the job module on those ranks.
         */
        if (fluid_init (&ctx.gen, rank, timestamp) < 0) {
            flux_log (h, LOG_ERR, "fluid_init failed");
            errno = EINVAL;
        }
    }
    flux_log (h, LOG_DEBUG, "fluid ts=%jums", (uint64_t)ctx.gen.timestamp);
    if (validate_initialize (h, argc, argv, &ctx.validate) < 0)
        goto done;
    if (flux_reactor_run (r, 0) < 0) {
        flux_log_error (h, "flux_reactor_run");
        goto done;
    }
    rc = 0;
done:
    flux_msg_handler_delvec (ctx.handlers);
    flux_watcher_destroy (ctx.timer);
    flux_watcher_destroy (ctx.shutdown_timer);
#if HAVE_FLUX_SECURITY
    flux_security_destroy (ctx.sec);
#endif
    validate_destroy (ctx.validate);
    return rc;
}

MOD_NAME ("job-ingest");

/*
 * vi:tabstop=4 shiftwidth=4 expandtab
 */
