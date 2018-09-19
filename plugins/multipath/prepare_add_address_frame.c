#include "picoquic_internal.h"
#include "plugin.h"
#include "../helpers.h"
#include "bpf.h"
#include "memory.h"

/**
 * cnx->protoop_inputv[0] = uint8_t* bytes
 * cnx->protoop_inputv[1] = size_t bytes_max
 * size_t consumed = cnx->protoop_inputv[2]
 *
 * Output: int ret
 * cnx->protoop_outputv[0] = size_t consumed
 */
protoop_arg_t prepare_add_address_frame(picoquic_cnx_t* cnx)
{
    uint8_t* bytes = (uint8_t *) cnx->protoop_inputv[0]; 
    size_t bytes_max = (size_t) cnx->protoop_inputv[1];
    size_t consumed = (size_t) cnx->protoop_inputv[2];

    picoquic_path_t *path_0 = cnx->path[0];
    uint16_t port;

    if (path_0->dest_addr_len == sizeof(struct sockaddr_in)) {
        struct sockaddr_in *si = (struct sockaddr_in *) &path_0->dest_addr;
        my_memcpy(&port, &si->sin_port, 2);
    } else {
        /* v6 */
        struct sockaddr_in6 *si6 = (struct sockaddr_in6 *) &path_0->dest_addr;
        my_memcpy(&port, &si6->sin6_port, 2);
    }

    int ret = 0;
    bpf_data *bpfd = get_bpf_data(cnx);

    /* Only cope with v4 so far, v6 for later */
    struct sockaddr_in sas[4];
    int nb_addrs = picoquic_getaddrs_v4(sas, 4);

    int frame_size_v4 = 9;

    if (bytes_max < nb_addrs * frame_size_v4) {
        /* A valid frame, with our encoding, uses at least 13 bytes.
         * If there is not enough space, don't attempt to encode it.
         */
        consumed = 0;
        ret = PICOQUIC_ERROR_FRAME_BUFFER_TOO_SMALL;
    }
    else {
        /* Create local address IDs. */
        size_t byte_index = 0;
        int addr_index = 0;
        int addr_id = 0;
        struct sockaddr_in *sa;

        for (int i = 0; i < nb_addrs; i++) {
            /* First record the address */
            addr_index = bpfd->nb_loc_addrs;
            addr_id = addr_index + 1;
            sa = (struct sockaddr_in *) my_malloc(cnx, sizeof(struct sockaddr_in));
            if (!sa) {
                ret = PICOQUIC_ERROR_MEMORY;
                break;
            }
            my_memcpy(sa, &sas[i], sizeof(struct sockaddr_in));
            /* Take the port from the current path 0 */
            my_memcpy(&sa->sin_port, &port, 2);
            bpfd->loc_addrs[addr_index].id = addr_id;
            bpfd->loc_addrs[addr_index].sa = (struct sockaddr *) sa;
            bpfd->loc_addrs[addr_index].is_v6 = false;

            /* Encode the first byte */
            bytes[byte_index++] = ADD_ADDRESS_TYPE;
            if (port != 0) {
                /* Encode port flag with v4 */
                bytes[byte_index++] = 0x14;
            } else {
                /* Otherwisen only v4 value */
                bytes[byte_index++] = 0x04;
            }
            /* Encode address ID */
            bytes[byte_index++] = addr_id;
            /* Encode IP address */
            my_memcpy(&bytes[byte_index], &sa->sin_addr.s_addr, 4);
            byte_index += 4;
            if (port != 0) {
                /* Encode port */
                my_memcpy(&bytes[byte_index], &sa->sin_port, 2);
                byte_index += 2;
            }
            
            bpfd->nb_loc_addrs++;
        }

        consumed = byte_index;
    }
    
    cnx->protoop_outputc_callee = 1;
    cnx->protoop_outputv[0] = (protoop_arg_t) consumed;

    return (protoop_arg_t) ret;
}