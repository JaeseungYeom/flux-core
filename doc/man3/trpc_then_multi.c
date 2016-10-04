#include <flux/core.h>
#include "src/common/libutil/shortjson.h"
#include "src/common/libutil/log.h"

void get_rank (flux_rpc_t *rpc, void *arg)
{
    const char *json_str;
    json_object *o;
    const char *rank;
    uint32_t nodeid;

    if (flux_rpc_get (rpc, &nodeid, &json_str) < 0)
        log_err_exit ("flux_rpc_get");
    if (!(o = Jfromstr (json_str)) || !Jget_str (o, "value", &rank))
        log_msg_exit ("response protocol error");
    printf ("[%" PRIu32 "] rank is %s\n", nodeid, rank);
    Jput (o);
}

int main (int argc, char **argv)
{
    flux_t h;
    flux_rpc_t *rpc;
    json_object *o = Jnew();

    Jadd_str (o, "name", "rank");
    if (!(h = flux_open (NULL, 0)))
        log_err_exit ("flux_open");
    if (!(rpc = flux_rpc_multi (h, "cmb.attrget", Jtostr (o), "all", 0)))
        log_err_exit ("flux_rpc");
    if (flux_rpc_then (rpc, get_rank, NULL) < 0)
        log_err_exit ("flux_rpc_then");
    if (flux_reactor_run (flux_get_reactor (h), 0) < 0)
        log_err_exit ("flux_reactor_run");

    flux_rpc_destroy (rpc);
    flux_close (h);
    Jput (o);
    return (0);
}
