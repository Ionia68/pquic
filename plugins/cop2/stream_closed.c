#include "picoquic.h"
#include "plugin.h"
#include "util.h"
#include "../helpers.h"
#include "bpf.h"


/**
 * See PROTOOP_NOPARAM_STREAM_CLOSED
 */
protoop_arg_t stream_closed(picoquic_cnx_t *cnx)
{
    get_cop2_metrics(cnx)->quic_metrics.streams_closed++;
    return 0;
}