#include "core_capture.h"

struct ether_fc_frame {
    uint16_t opcode;
    uint16_t param;
} __rte_packed;

static inline void prepare_pause_frame(uint16_t port, struct rte_mbuf *mbuf) {
    struct ether_fc_frame *pause_frame;
    struct rte_ether_hdr *hdr;

    /* Prepare a PAUSE frame */
    hdr = rte_pktmbuf_mtod(mbuf, struct rte_ether_hdr *);
    pause_frame = (struct ether_fc_frame *) &hdr[1];
    rte_eth_macaddr_get(port, &hdr->s_addr);

    void *tmp = &hdr->d_addr.addr_bytes[0];
    *((uint64_t *)tmp) = 0x010000C28001ULL;

    hdr->ether_type = rte_cpu_to_be_16(ETHER_TYPE_FLOW_CONTROL);
    pause_frame->opcode = rte_cpu_to_be_16(OPCODE_PAUSE);
    pause_frame->param  = rte_cpu_to_be_16(PAUSE_TIME);
    mbuf->pkt_len  = 60;
    mbuf->data_len = 60;
}

static inline void clone_pause_frames(const struct rte_mbuf *original,
            struct rte_mbuf **clones, int num, struct rte_mempool *pool) {
    if(unlikely(rte_pktmbuf_alloc_bulk(pool, clones, num)))
        rte_exit(EXIT_FAILURE, "Error: Could not allocate pause frame buffers " \
                                    "for cloning on Core %d\n", rte_lcore_id());
    for (int i=0; i<num; i++) {
        clones[i]->pkt_len  = 60;
        clones[i]->data_len  = 60;
        rte_mov64(rte_pktmbuf_mtod(clones[i], void*), rte_pktmbuf_mtod(original, void*));
    }
}

static inline uint16_t send_pause_frames(uint16_t port, uint16_t queue,
            const struct rte_mbuf *pause_frame, struct rte_mbuf **pause_mbufs,
            uint16_t num, struct rte_mempool *pool) {
    uint16_t nb_tx = rte_eth_tx_burst(port, queue, pause_mbufs, num);
    if(likely(nb_tx))
        clone_pause_frames(pause_frame, pause_mbufs, nb_tx, pool);
    return nb_tx;
}

/*
 * Capture the traffic from the given port/queue tuple
 */
int capture_core(const struct capture_core_config * config) {
    const unsigned socket_id = rte_socket_id();
    unsigned dev_socket_id;

    volatile bool * stop_condition = config->stop_condition;

    const uint16_t port = config->port;
    const uint16_t queue = config->queue;

    const uint16_t burst_size = config->burst_size;
    struct rte_mbuf * bufs[burst_size];
    struct rte_mbuf * bufptr;

    struct rte_mempool * pause_mbuf_pool = config->pause_mbuf_pool;
    const uint16_t flow_control = config->flow_control;
    const uint16_t pause_burst_size = config->pause_burst_size;
    struct rte_mbuf *pause_mbufs[pause_burst_size];
    struct rte_mbuf *pause_frame = NULL;

    struct rte_ring * pbuf_free_ring = config->pbuf_free_ring;
    struct rte_ring * pbuf_full_ring = config->pbuf_full_ring;
    const uint32_t snaplen = config->snaplen;
    const uint32_t watermark = config->watermark;

    struct pcap_buffer * buffer = NULL;
    struct pcap_packet_header header;
    size_t header_size = sizeof(header);
    struct timeval tv;

    const uint16_t disk_blk_size = config->disk_blk_size;
    uint16_t i, nb_rx, bytes_to_write;
    unsigned int overrun = 0, overrun_start = 0, flush = 0;
    unsigned char * oldbuf = NULL;

    LOG_INFO("Core %u is capturing packets for port %u\n", rte_lcore_id(), port);

    dev_socket_id = rte_eth_dev_socket_id(port);
    if (dev_socket_id != socket_id)
        LOG_WARN("Port %u on different socket from worker; performance will suffer\n", port);

    /* Init stats */
    config->stats->core_id = rte_lcore_id();
    config->stats->pbuf_free_ring = config->pbuf_free_ring;

    if (flow_control) {
        pause_frame = rte_pktmbuf_alloc(pause_mbuf_pool);
        if(!pause_frame)
            rte_exit(EXIT_FAILURE, "Error: Could not allocate pause frame buffer " \
                                                "on Core %d\n", rte_lcore_id());
        prepare_pause_frame(port, pause_frame);
        clone_pause_frames(pause_frame, pause_mbufs, pause_burst_size, pause_mbuf_pool);
    }
    else
        config->stats->pause_frames = ~0UL;

    if(!rte_ring_sc_dequeue_bulk(pbuf_free_ring, (void **)&buffer, 1, NULL))
        rte_exit(EXIT_FAILURE, "Error: Could not obtain an empty packet buffer (PBUF) " \
                                                "on Core %d\n", rte_lcore_id());

    /* Run until the application is quit or killed. */
    while(likely(!(*stop_condition))) {

        /* Retrieve packets and put them into the ring */
        nb_rx = rte_eth_rx_burst(port, queue, bufs, burst_size);

        if (likely(nb_rx > 0)) {

            gettimeofday(&tv, NULL);
            header.timestamp = (uint32_t) tv.tv_sec;
            header.microseconds = (uint32_t) tv.tv_usec;

            for (i=0; i < nb_rx; i++) {
                bufptr = bufs[i];
                rte_prefetch0(rte_pktmbuf_mtod(bufptr, void *));

                header.packet_length = MIN(snaplen, bufptr->pkt_len);
                header.packet_length_wire = bufptr->pkt_len;
                rte_mov16(buffer->buffer + buffer->offset, (void*)&header);
                buffer->offset += header_size;

                if(unlikely(bufptr->nb_segs > 1)) {
                    if(header.packet_length <= bufptr->data_len) {
                        rte_memcpy(buffer->buffer + buffer->offset, rte_pktmbuf_mtod(bufptr, void*), header.packet_length);
                        buffer->offset += header.packet_length;
                    }
                    else {
                        rte_prefetch0(rte_pktmbuf_mtod(bufptr->next, void *));
                        bytes_to_write = header.packet_length;
                        do {
                            rte_memcpy(buffer->buffer + buffer->offset, rte_pktmbuf_mtod(bufptr, void*), bufptr->data_len);
                            bytes_to_write -= bufptr->data_len;
                            buffer->offset += bufptr->data_len;
                            bufptr = bufptr->next;
                        } while (bufptr && bytes_to_write > 0);
                        bufptr = bufs[i];
                    }
                }
                else {
                    rte_memcpy(buffer->buffer + buffer->offset, rte_pktmbuf_mtod(bufptr, void*), header.packet_length);
                    buffer->offset += header.packet_length;
                }
            rte_pktmbuf_free(bufptr);
            }

            /* Update stats */
            config->stats->packets+=nb_rx;
            config->stats->buffer_packets+=nb_rx;
            flush = 0;
        }
        else
            flush++;

        /* Enqueue buffer to be flushed if full and get a new one */
        if (buffer->offset > watermark || (flush > 9999999 && buffer->offset > disk_blk_size)) {
            buffer->packets = config->stats->buffer_packets;
            overrun = buffer->offset % disk_blk_size;
            if (overrun) {
                buffer->offset -= overrun;
                overrun_start = buffer->offset;
                oldbuf = buffer->buffer;
            }

            while(!(rte_ring_sp_enqueue_bulk(pbuf_full_ring, (void **)&buffer, 1, NULL) || unlikely(*stop_condition))){
                if (flow_control)
                    config->stats->pause_frames += send_pause_frames(port, queue, pause_frame,
                                        pause_mbufs, pause_burst_size, pause_mbuf_pool);
            }

            config->stats->buffer_packets = 0;

            while(!(rte_ring_sc_dequeue_bulk(pbuf_free_ring, (void **)&buffer, 1, NULL) || unlikely(*stop_condition))){
                if (flow_control)
                    config->stats->pause_frames += send_pause_frames(port, queue, pause_frame,
                                        pause_mbufs, pause_burst_size, pause_mbuf_pool);
            }

            if (overrun) {
                rte_memcpy(buffer->buffer, oldbuf + overrun_start, overrun);
                buffer->offset += overrun;
            }
        }
    }

    if (buffer->offset) {
        buffer->packets = config->stats->buffer_packets;
        unsigned int underrun = disk_blk_size - (buffer->offset % disk_blk_size);
        memset(buffer->buffer + buffer->offset, 0, underrun);
        if (underrun > header_size)
            add_pad_packet((struct pcap_packet_header *)(buffer->buffer + buffer->offset),
                                                                underrun);
        buffer->offset += underrun;
        rte_ring_sp_enqueue_bulk(pbuf_full_ring, (void **)&buffer, 1, NULL);
    }

    LOG_INFO("Closed capture core %d (port %d)\n", rte_lcore_id(), port);

    return 0;
}
