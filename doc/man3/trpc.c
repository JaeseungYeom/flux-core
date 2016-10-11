#include <flux/core.h>
#include "src/common/libutil/shortjson.h"
#include "src/common/libutil/log.h"

void get_rank (flux_rpc_t *rpc)
{
    const char *json_str;
    json_object *o;
    const char *rank;

    if (flux_rpc_get (rpc, NULL, &json_str) < 0)
        log_err_exit ("flux_rpc_get");
    if (!(o = Jfromstr (json_str)) || !Jget_str (o, "value", &rank))
        log_msg_exit ("response protocol error");
    printf ("rank is %s\n", rank);
    Jput (o);
}

int main (int argc, char **argv)
{
    flux_t *h;
    flux_rpc_t *rpc;
    json_object *o = Jnew();

    if (!(h = flux_open (NULL, 0)))
        log_err_exit ("flux_open");
    Jadd_str (o, "name", "rank");
    if (!(rpc = flux_rpc (h, "cmb.attrget", Jtostr (o), FLUX_NODEID_ANY, 0)))
        log_err_exit ("flux_rpc");
    get_rank (rpc);
    flux_rpc_destroy (rpc);
    flux_close (h);
    Jput (o);
    return (0);
}
