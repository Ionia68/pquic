#include "bpf.h"

/* Special wake up decision logic in initial state */
static void cnx_set_next_wake_time_init(picoquic_cnx_t* cnx, uint64_t current_time)
{
    uint64_t start_time = (uint64_t) get_cnx(cnx, AK_CNX_START_TIME, 0);
    uint64_t next_time = start_time + PICOQUIC_MICROSEC_HANDSHAKE_MAX;
    int timer_based = 0;
    int blocked = 1;
    int pacing = 0;
    picoquic_path_t *path_x = (picoquic_path_t *) get_cnx(cnx, AK_CNX_PATH, 0);
    picoquic_packet_context_t *pkt_ctx;
    int pc_ready_flag = 1 << picoquic_packet_context_initial;
    bpf_data *bpfd = get_bpf_data(cnx);
    uniflow_data_t *ud = NULL;
    picoquic_stream_head *tls_stream_0 = (picoquic_stream_head *) get_cnx(cnx, AK_CNX_TLS_STREAM, 0);

    picoquic_crypto_context_t *crypto_context_1 = (picoquic_crypto_context_t *) get_cnx(cnx, AK_CNX_CRYPTO_CONTEXT, 1);
    picoquic_crypto_context_t *crypto_context_2 = (picoquic_crypto_context_t *) get_cnx(cnx, AK_CNX_CRYPTO_CONTEXT, 2);
    void *crypto_context_1_aead_encrypt = (void *) get_crypto_context(crypto_context_1, AK_CRYPTOCONTEXT_AEAD_ENCRYPTION);

    picoquic_stream_data *tls_stream_0_send_queue = (picoquic_stream_data *) get_stream_head(tls_stream_0, AK_STREAMHEAD_SEND_QUEUE);

    if (tls_stream_0_send_queue == NULL) {
        picoquic_stream_head *tls_stream_1 = (picoquic_stream_head *) get_cnx(cnx, AK_CNX_TLS_STREAM, 1);
        picoquic_stream_data *tls_stream_1_send_queue = (picoquic_stream_data *) get_stream_head(tls_stream_1, AK_STREAMHEAD_SEND_QUEUE);
        void *crypto_context_2_aead_encrypt = (void *) get_crypto_context(crypto_context_2, AK_CRYPTOCONTEXT_AEAD_ENCRYPTION);
        if (crypto_context_1_aead_encrypt != NULL &&
            tls_stream_1_send_queue != NULL) {
            pc_ready_flag |= 1 << picoquic_packet_context_application;
        }
        else if (crypto_context_2_aead_encrypt != NULL &&
            tls_stream_1_send_queue == NULL) {
            pc_ready_flag |= 1 << picoquic_packet_context_handshake;
        }
    }

    if (next_time < current_time) {
        next_time = current_time;
        blocked = 0;
    } else {
        for (picoquic_packet_context_enum pc = 0; blocked == 1 && pc < picoquic_nb_packet_context; pc++) {
            pkt_ctx = (picoquic_packet_context_t *) get_path(path_x, AK_PATH_PKT_CTX, pc);
            ud = mp_get_uniflow_data(bpfd, true, path_x);
            /* If the path is not active, don't expect anything! */
            if ((ud != NULL && ud->state != uniflow_active) || get_path(path_x, AK_PATH_CHALLENGE_VERIFIED, 0) == 0) {
                continue;
            }

            picoquic_packet_t* p = (picoquic_packet_t *) get_pkt_ctx(pkt_ctx, AK_PKTCTX_RETRANSMIT_OLDEST);

            if ((pc_ready_flag & (1 << pc)) == 0) {
                continue;
            }

            while (p != NULL)
            {
                picoquic_packet_type_enum ptype = (picoquic_packet_type_enum) get_pkt(p, AK_PKT_TYPE);
                if (ptype < picoquic_packet_0rtt_protected) {
                    if (helper_retransmit_needed_by_packet(cnx, p, current_time, &timer_based, NULL, NULL)) {
                        blocked = 0;
                    }
                    break;
                }
                p = (picoquic_packet_t *) get_pkt(p, AK_PKT_NEXT_PACKET);
            }

            if (blocked != 0)
            {
                if (helper_is_ack_needed(cnx, current_time, pc, path_x)) {
                    blocked = 0;
                }
            }
        }

        if (blocked != 0 && pacing == 0) {
            uint64_t cwin_x = (uint64_t) get_path(path_x, AK_PATH_CWIN, 0);
            uint64_t bytes_in_transit_x = (uint64_t) get_path(path_x, AK_PATH_BYTES_IN_TRANSIT, 0);
            int challenge_verified_x = (int) get_path(path_x, AK_PATH_CHALLENGE_VERIFIED, 0);
            if (challenge_verified_x == 1) {
                if (helper_should_send_max_data(cnx) ||
                    helper_is_tls_stream_ready(cnx) ||
                    (crypto_context_1_aead_encrypt != NULL && (helper_find_ready_stream(cnx)) != NULL)) {
                    if (cwin_x > bytes_in_transit_x) {
#ifdef PACING
                        if (picoquic_is_sending_authorized_by_pacing(path_x, current_time, &next_time)) {
#endif
                        blocked = 0;
#ifdef PACING
                        } else {
                            pacing = 1;
                        }
#endif
                    } else {
                        helper_congestion_algorithm_notify(cnx, path_x, picoquic_congestion_notification_cwin_blocked, 0, 0, 0, current_time);
                    }
                }
            }
        }

        if (blocked == 0) {
            next_time = current_time;
        } else if (pacing == 0) {
            for (picoquic_packet_context_enum pc = 0; pc < picoquic_nb_packet_context; pc++) {
                pkt_ctx = (picoquic_packet_context_t *) get_path(path_x, AK_PATH_PKT_CTX, pc);
                picoquic_packet_t* p = (picoquic_packet_t *) get_pkt_ctx(pkt_ctx, AK_PKTCTX_RETRANSMIT_OLDEST);

                if ((pc_ready_flag & (1 << pc)) == 0) {
                    continue;
                }

                /* Consider delayed ACK */
                int ack_needed = (int) get_pkt_ctx(pkt_ctx, AK_PKTCTX_ACK_NEEDED);
                if (ack_needed) {
                    next_time = get_pkt_ctx(pkt_ctx, AK_PKTCTX_HIGHEST_ACK_TIME) + get_pkt_ctx(pkt_ctx, AK_PKTCTX_ACK_DELAY_LOCAL);
                }

                if (p != NULL) {
                    picoquic_packet_type_enum ptype = (picoquic_packet_type_enum) get_pkt(p, AK_PKT_TYPE);
                    int pcontains_crypto = (int) get_pkt(p, AK_PKT_CONTAINS_CRYPTO);
                    while (p != NULL &&
                        ptype == picoquic_packet_0rtt_protected &&
                        pcontains_crypto == 0) {
                        p = (picoquic_packet_t *) get_pkt(p, AK_PKT_NEXT_PACKET);
                        if (p != NULL) {
                            ptype = (picoquic_packet_type_enum) get_pkt(p, AK_PKT_TYPE);
                            pcontains_crypto = (int) get_pkt(p, AK_PKT_CONTAINS_CRYPTO);
                        }
                    }
                }

                if (p != NULL) {
                    uint64_t nb_retransmit = (uint64_t) get_pkt_ctx(pkt_ctx, AK_PKTCTX_NB_RETRANSMIT);
                    uint64_t send_time = (uint64_t) get_pkt(p, AK_PKT_SEND_TIME);
                    if (nb_retransmit == 0) {
                        uint64_t retransmit_timer_x = (uint64_t) get_path(path_x, AK_PATH_RETRANSMIT_TIMER, 0);
                        if (send_time + retransmit_timer_x < next_time) {
                            next_time = send_time + retransmit_timer_x;
                        }
                    }
                    else {
                        if (send_time + (1000000ull << (nb_retransmit - 1)) < next_time) {
                            next_time = send_time + (1000000ull << (nb_retransmit - 1));
                        }
                    }
                }
            }
        }
    }

    /* Consider path challenges */
    int challenge_verified_x = (int) get_path(path_x, AK_PATH_CHALLENGE_VERIFIED, 0);
    int challenge_repeat_count_x = (int) get_path(path_x, AK_PATH_CHALLENGE_REPEAT_COUNT, 0);
    if (blocked != 0 && challenge_verified_x == 0 && challenge_repeat_count_x < PICOQUIC_CHALLENGE_REPEAT_MAX) {
        uint64_t challenge_time_x = (uint64_t) get_path(path_x, AK_PATH_CHALLENGE_TIME, 0);
        uint64_t retransmit_timer_x = (uint64_t) get_path(path_x, AK_PATH_RETRANSMIT_TIMER, 0);
        uint64_t next_challenge_time = challenge_time_x + retransmit_timer_x;
        if (next_challenge_time <= current_time) {
            next_time = current_time;
        } else if (next_challenge_time < next_time) {
            next_time = next_challenge_time;
        }
    }

    /* reset the connection at its new logical position */
    picoquic_reinsert_cnx_by_wake_time(cnx, next_time);
}

static int get_nb_uniflows(bpf_data *bpfd, bool for_sending) {
    return for_sending ? bpfd->nb_sending_proposed : bpfd->nb_receiving_proposed;
}

static picoquic_path_t *_get_path(picoquic_cnx_t *cnx, bpf_data *bpfd, bool for_sending, int index, uniflow_data_t **ud) {
    uniflow_data_t **uds = for_sending ? bpfd->sending_uniflows : bpfd->receiving_uniflows;
    *ud = uds[index];
    return uds[index]->path;
}

static picoquic_path_t *get_sending_path(picoquic_cnx_t *cnx, bpf_data *bpfd, int index, uniflow_data_t **ud) {
    return _get_path(cnx, bpfd, true, index, ud);
}

static picoquic_path_t *get_receiving_path(picoquic_cnx_t *cnx, bpf_data *bpfd, int index, uniflow_data_t **ud) {
    return _get_path(cnx, bpfd, false, index, ud);
}


/**
 * See PROTOOP_NOPARAM_SET_NEXT_WAKE_TIME
 */
protoop_arg_t set_nxt_wake_time(picoquic_cnx_t *cnx)
{
    uint64_t current_time = (uint64_t) get_cnx(cnx, AK_CNX_INPUT, 0);
    uint64_t latest_progress_time = (uint64_t) get_cnx(cnx, AK_CNX_LATEST_PROGRESS_TIME, 0);
    uint64_t client_mode = (int) get_cnx(cnx, AK_CNX_CLIENT_MODE, 0);
    uint64_t next_time = latest_progress_time + PICOQUIC_MICROSEC_SILENCE_MAX * (2 - client_mode);
    picoquic_stream_head* stream = NULL;
    picoquic_state_enum cnx_state = (picoquic_state_enum) get_cnx(cnx, AK_CNX_STATE, 0);
    int timer_based = 0;
    int blocked = 1;
    int pacing = 0;
    int ret = 0;

    bpf_data *bpfd = get_bpf_data(cnx);
    uniflow_data_t *ud = NULL;

    char *reason = NULL;


    if (cnx_state < picoquic_state_client_ready)
    {
        cnx_set_next_wake_time_init(cnx, current_time);
        bpfd->next_sending_uniflow = NULL;
        return 0;
    }

    manage_paths(cnx);

    if (cnx_state == picoquic_state_disconnecting || cnx_state == picoquic_state_handshake_failure || cnx_state == picoquic_state_closing_received) {
        blocked = 0;
        bpfd->next_sending_uniflow = bpfd->sending_uniflows[0];
        reason = "Special state";
    }

    int nb_snd_uniflows = get_nb_uniflows(bpfd, true);
    int nb_rcv_uniflows = get_nb_uniflows(bpfd, false);
    picoquic_path_t *path_x = NULL;

    /* If any receive path requires path response, do it now! */
    for (int i = 0; blocked != 0 && i < nb_rcv_uniflows; i++) {
        path_x = get_receiving_path(cnx, bpfd, i, &ud);
        if (ud != NULL && ud->state != uniflow_active) continue;
        if (get_path(path_x, AK_PATH_CHALLENGE_RESPONSE_TO_SEND, 0) != 0) {
            blocked = 0;
            bpfd->next_sending_uniflow = NULL;
            reason = "Path response to send";
        }
        for (picoquic_packet_context_enum pc = 0; pc < picoquic_nb_packet_context && (i == 0 || pc == picoquic_packet_context_application); pc++) {
            if (helper_is_ack_needed(cnx, current_time, pc, path_x)) {
                blocked = 0;
                bpfd->next_sending_uniflow = NULL;
                reason = "Ack needed";
            } else if (get_path(path_x, AK_PATH_PING_RECEIVED, 0)) {
                blocked = 0;
                bpfd->next_sending_uniflow = NULL;
                reason = "Ping received";
            }
        }
    }

    picoquic_packet_context_t *pkt_ctx;
    if (blocked != 0) {
        bpf_duplicate_data *bpfdd = get_bpf_duplicate_data(cnx);
        bool should_send_max_data = helper_should_send_max_data(cnx);
        bool is_tls_stream_ready = helper_is_tls_stream_ready(cnx);
        bool has_cc_to_send = run_noparam(cnx, PROTOOPID_NOPARAM_HAS_CONGESTION_CONTROLLED_PLUGIN_FRAMEMS_TO_SEND, 0, NULL, NULL);
        bool handshake_done_to_send = !get_cnx(cnx, AK_CNX_CLIENT_MODE, 0) && get_cnx(cnx, AK_CNX_HANDSHAKE_DONE, 0) && !get_cnx(cnx, AK_CNX_HANDSHAKE_DONE_SENT, 0);

        for (int i = 0; blocked != 0 && pacing == 0 && i < nb_snd_uniflows; i++) {
            path_x = get_sending_path(cnx, bpfd, i, &ud);
            /* If the path is not active, don't expect anything! */
            if (ud != NULL && ud->state != uniflow_active) {
                continue;
            }
            for (picoquic_packet_context_enum pc = 0; pc < picoquic_nb_packet_context && (i == 0 || pc == picoquic_packet_context_application); pc++) {
                pkt_ctx = (picoquic_packet_context_t *) get_path(path_x, AK_PATH_PKT_CTX, pc);
                picoquic_packet_t* p = (picoquic_packet_t *) get_pkt_ctx(pkt_ctx, AK_PKTCTX_RETRANSMIT_OLDEST);

                if (p != NULL && ret == 0 && helper_retransmit_needed_by_packet(cnx, p, current_time, &timer_based, NULL, NULL)) {
                    blocked = 0;
                    bpfd->next_sending_uniflow = NULL;
                    reason = "Retransmit needed";
                }
                if (get_cnx(cnx, AK_CNX_HANDSHAKE_DONE, 0) && (get_cnx(cnx, AK_CNX_CLIENT_MODE, 0) || get_cnx(cnx, AK_CNX_HANDSHAKE_DONE_ACKED, 0))) {
                    break;
                }
            }

            if (blocked != 0) {
                uint64_t cwin_x = (uint64_t) get_path(path_x, AK_PATH_CWIN, 0);
                uint64_t bytes_in_transit_x = (uint64_t) get_path(path_x, AK_PATH_BYTES_IN_TRANSIT, 0);
                int is_validated = get_path(path_x, AK_PATH_CHALLENGE_VERIFIED, 0);
                if (is_validated) {
                   if (should_send_max_data ||
                        is_tls_stream_ready ||
                        handshake_done_to_send ||
                        ((cnx_state == picoquic_state_client_ready || cnx_state == picoquic_state_server_ready) &&
                        ((stream = helper_find_ready_stream(cnx)) != NULL || helper_find_ready_plugin_stream(cnx) || has_cc_to_send || bpfdd->requires_duplication))) {
                        if (cwin_x > bytes_in_transit_x) {
#ifdef PACING
                            if (picoquic_is_sending_authorized_by_pacing(path_x, current_time, &next_time)) {
#endif
                            //PROTOOP_PRINTF(cnx, "Not blocked because path %p has should max data %d tls ready %d cnx_state %d stream %p has_cc %d cwin %d BIF %d\n", (protoop_arg_t) path_x, should_send_max_data, is_tls_stream_ready, cnx_state, (protoop_arg_t) stream, has_cc_to_send, cwin_x, bytes_in_transit_x);
                            blocked = 0;
                            bpfd->next_sending_uniflow = NULL;
                            reason = "App data can be sent";
#ifdef PACING
                            } else {
                                 pacing = 1;
                            }
#endif
                        } else {
                            helper_congestion_algorithm_notify(cnx, path_x, picoquic_congestion_notification_cwin_blocked, 0, 0, 0, current_time);
                        }
                    }
                }
            }
        }
    }

    bool has_booked_frames = picoquic_has_booked_plugin_frames(cnx);
    for (int i = 0; blocked != 0 && pacing == 0 && i < nb_snd_uniflows; i++) {
        path_x = get_sending_path(cnx, bpfd, i, &ud);
        /* If the path is not active, don't expect anything! */
        if ((ud != NULL && ud->state != uniflow_active) || get_path(path_x, AK_PATH_CHALLENGE_VERIFIED, 0) == 0) {
            continue;
        }
        uint64_t cwin_x = (uint64_t) get_path(path_x, AK_PATH_CWIN, 0);
        uint64_t bytes_in_transit_x = (uint64_t) get_path(path_x, AK_PATH_BYTES_IN_TRANSIT, 0);
        if (helper_is_mtu_probe_needed(cnx, path_x) && ud->has_sent_uniflows_frame) {
            blocked = 0;
            bpfd->next_sending_uniflow = ud;
            reason = "MTU probe needed";
        }
        if (cwin_x > bytes_in_transit_x && has_booked_frames) {
            blocked = 0;
            bpfd->next_sending_uniflow = NULL;
            reason = "Plugin frames can be sent";
        }
    }

    if (blocked == 0) {
        next_time = current_time;
    } else if (pacing == 0) {
        for (picoquic_packet_context_enum pc = 0; pc < picoquic_nb_packet_context; pc++) {
            /* First consider receive paths */
            for (int i = 0; i < nb_rcv_uniflows; i++) {
                path_x = get_receiving_path(cnx, bpfd, i, &ud);
                /* If the path is not active, don't expect anything! */
                if (ud != NULL && ud->state != uniflow_active) {
                    continue;
                }
                pkt_ctx = (picoquic_packet_context_t *) get_path(path_x, AK_PATH_PKT_CTX, pc);
                /* Consider delayed ACK */
                int ack_needed = (int) get_pkt_ctx(pkt_ctx, AK_PKTCTX_ACK_NEEDED);
                if (ack_needed) {
                    uint64_t ack_time = get_pkt_ctx(pkt_ctx, AK_PKTCTX_HIGHEST_ACK_TIME) + get_pkt_ctx(pkt_ctx, AK_PKTCTX_ACK_DELAY_LOCAL);

                    if (ack_time < next_time) {
                        next_time = ack_time;
                        bpfd->next_sending_uniflow = NULL;
                        reason = "Delayed ACK for receive uniflow";
                    }
                }
            }

            for (int i = 0; i < nb_snd_uniflows; i++) {
                path_x = get_sending_path(cnx, bpfd, i, &ud);
                /* If the path is not active, don't expect anything! */
                if ((ud != NULL && ud->state != uniflow_active) || get_path(path_x, AK_PATH_CHALLENGE_VERIFIED, 0) == 0) {
                    continue;
                }
                pkt_ctx = (picoquic_packet_context_t *) get_path(path_x, AK_PATH_PKT_CTX, pc);
                picoquic_packet_t* p = (picoquic_packet_t *) get_pkt_ctx(pkt_ctx, AK_PKTCTX_RETRANSMIT_OLDEST);
                /* Consider delayed ACK */
                int ack_needed = (int) get_pkt_ctx(pkt_ctx, AK_PKTCTX_ACK_NEEDED);
                if (ack_needed) {
                    uint64_t ack_time = get_pkt_ctx(pkt_ctx, AK_PKTCTX_HIGHEST_ACK_TIME) + get_pkt_ctx(pkt_ctx, AK_PKTCTX_ACK_DELAY_LOCAL);

                    if (ack_time < next_time) {
                        next_time = ack_time;
                        bpfd->next_sending_uniflow = NULL;
                        reason = "Delayed ACK for sending uniflow";
                    }
                }

                p = (picoquic_packet_t *) get_pkt_ctx(pkt_ctx, AK_PKTCTX_RETRANSMIT_OLDEST);
                if (p != NULL) {
                    uint64_t retransmit_time = UINT64_MAX;
                    char *retransmit_reason = NULL;
                    helper_retransmit_needed_by_packet(cnx, p, current_time, &timer_based, &retransmit_reason, &retransmit_time);

                    if (retransmit_time < next_time) {
                        next_time = retransmit_time;
                        bpfd->next_sending_uniflow = NULL;
                        reason = "Next retransmit time";
                    }
                }
            }
            if (get_cnx(cnx, AK_CNX_HANDSHAKE_DONE, 0) && (get_cnx(cnx, AK_CNX_CLIENT_MODE, 0) || get_cnx(cnx, AK_CNX_HANDSHAKE_DONE_ACKED, 0))) {
                break;
            }
        }

        for (int i = 0; i < nb_snd_uniflows; i++) {
            path_x = get_sending_path(cnx, bpfd, i, &ud);
            
            /* If the path is not active, don't expect anything! */
            if (ud != NULL && ud->state != uniflow_active) {
                continue;
            }
            int challenge_verified_x = (int) get_path(path_x, AK_PATH_CHALLENGE_VERIFIED, 0);
            int challenge_repeat_count_x = (int) get_path(path_x, AK_PATH_CHALLENGE_REPEAT_COUNT, 0);
            /* Consider path challenges */
            if (challenge_verified_x == 0 && challenge_repeat_count_x < PICOQUIC_CHALLENGE_REPEAT_MAX) {
                uint64_t challenge_time_x = (uint64_t) get_path(path_x, AK_PATH_CHALLENGE_TIME, 0);
                uint64_t retransmit_timer_x = (uint64_t) get_path(path_x, AK_PATH_RETRANSMIT_TIMER, 0);
                uint64_t next_challenge_time = challenge_time_x + retransmit_timer_x;
                if (next_challenge_time <= current_time) {
                    next_time = current_time;
                    bpfd->next_sending_uniflow = ud;
                    reason = "Path challenge to send now";
                } else if (next_challenge_time < next_time) {
                    next_time = next_challenge_time;
                    bpfd->next_sending_uniflow = ud;
                    reason = "Next path challenge time";
                }
            }
        }

        /* Consider keep alive */
        uint64_t keep_alive_interval = (uint64_t) get_cnx(cnx, AK_CNX_KEEP_ALIVE_INTERVAL, 0);
        if (keep_alive_interval != 0 && next_time > (latest_progress_time + keep_alive_interval)) {
            next_time = latest_progress_time + keep_alive_interval;
            bpfd->next_sending_uniflow = NULL;
            reason = "Next keep alive";
        }
    }

    PROTOOP_PRINTF(cnx, "Current time %" PRIu64 ", wake at %" PRIu64 " on uniflow: %p, reason: %s\n", current_time, next_time, (protoop_arg_t) bpfd->next_sending_uniflow, reason);

    /* reset the connection at its new logical position */
    picoquic_reinsert_cnx_by_wake_time(cnx, next_time);

    return 0;
}