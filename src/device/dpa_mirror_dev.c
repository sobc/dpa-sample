/*
 * dpa_mirror_dev.c - DPA Device-Side Packet Mirroring Handler
 *
 * This code runs directly on the BlueField-3 DPA RISC-V cores.
 * It receives incoming UDP packets from the RQ, swaps L2 (MACs),
 * L3 (IPv4), and L4 (UDP ports), and transmits the packet back out.
 */

#ifndef FLEXIO_DEV_VER_USED
#define FLEXIO_DEV_VER_USED FLEXIO_DEV_VER_LATEST
#endif

#include <libflexio-dev/flexio_dev.h>
#include <libflexio-dev/flexio_dev_err.h>
#include <libflexio-dev/flexio_dev_queue_access.h>
#include <libflexio-libc/string.h>
#include <stddef.h>
#include <dpaintrin.h>

#include "dpa_mirror_com.h"

/* Queue index masks */
#define CQ_IDX_MASK    ((1U << LOG_CQ_DEPTH) - 1U)
#define RQ_IDX_MASK    ((1U << LOG_RQ_DEPTH) - 1U)
#define SQ_IDX_MASK    ((1U << (LOG_SQ_DEPTH + LOG_SQE_NUM_SEGS)) - 1U)
#define DATA_IDX_MASK  ((1U << LOG_SQ_DEPTH) - 1U)

/* Context structures for managing queues directly on the DPA */
typedef struct {
    uint32_t cq_number;
    struct flexio_dev_cqe64 *cq_ring;
    struct flexio_dev_cqe64 *cqe;
    uint32_t cq_idx;
    uint8_t cq_hw_owner_bit;
    uint32_t *cq_dbr;
    uint32_t log_cq_depth;
} dpa_cq_ctx_t;

typedef struct {
    uint32_t rq_number;
    struct flexio_dev_wqe_rcv_data_seg *rq_ring;
    uint32_t *rq_dbr;
} dpa_rq_ctx_t;

typedef struct {
    uint32_t sq_number;
    union flexio_dev_sqe_seg *sq_ring;
    uint32_t sq_wqe_seg_idx;
    uint32_t sq_pi;
} dpa_sq_ctx_t;

typedef struct {
    void *sq_tx_buff;
    uint32_t tx_buff_idx;
} dpa_dt_ctx_t;

/* Global state for the DPA application instance */
static struct {
    uint64_t packets_count;
    uint32_t lkey;
    uint32_t target_udp_port;
    dpa_cq_ctx_t rq_cq_ctx;
    dpa_rq_ctx_t rq_ctx;
    dpa_sq_ctx_t sq_ctx;
    dpa_cq_ctx_t sq_cq_ctx;
    dpa_dt_ctx_t dt_ctx;
} app_ctx;

/* Helper: Swap two 32-bit values */
static inline void swap_u32(uint32_t *a, uint32_t *b)
{
    uint32_t tmp = *a;
    *a = *b;
    *b = tmp;
}

/* Helper: Swap two 16-bit values */
static inline void swap_u16(uint16_t *a, uint16_t *b)
{
    uint16_t tmp = *a;
    *a = *b;
    *b = tmp;
}

/* Helper: Swap 6-byte MAC addresses */
static inline void swap_mac_addresses(uint8_t *packet)
{
    uint8_t tmp[6];
    memcpy(tmp, packet, 6);              /* Save dst MAC */
    memcpy(packet, packet + 6, 6);       /* Copy src MAC to dst MAC */
    memcpy(packet + 6, tmp, 6);          /* Copy saved dst MAC to src MAC */
}

/* Advance CQ context */
static inline void step_cq(dpa_cq_ctx_t *cq_ctx)
{
    uint32_t mask = (1U << cq_ctx->log_cq_depth) - 1U;
    cq_ctx->cq_idx++;
    cq_ctx->cqe = &cq_ctx->cq_ring[cq_ctx->cq_idx & mask];
    if (!(cq_ctx->cq_idx & mask)) {
        cq_ctx->cq_hw_owner_bit = !cq_ctx->cq_hw_owner_bit;
    }
    flexio_dev_dbr_cq_set_ci(cq_ctx->cq_dbr, cq_ctx->cq_idx);
}

/* Get next SQ entry segment */
static inline union flexio_dev_sqe_seg *get_next_sqe(dpa_sq_ctx_t *sq_ctx)
{
    return &sq_ctx->sq_ring[sq_ctx->sq_wqe_seg_idx++ & SQ_IDX_MASK];
}

/* Initialize DPA application context from host configuration */
static void app_ctx_init(struct host2dev_mirror_data *data_from_host)
{
    app_ctx.packets_count = 0;
    app_ctx.lkey = data_from_host->sq_transf.wqd_mkey_id;
    app_ctx.target_udp_port = data_from_host->target_udp_port;

    /* Initialize RQ CQ context */
    app_ctx.rq_cq_ctx.cq_number = data_from_host->rq_cq_transf.cq_num;
    app_ctx.rq_cq_ctx.cq_ring = (struct flexio_dev_cqe64 *)data_from_host->rq_cq_transf.cq_ring_daddr;
    app_ctx.rq_cq_ctx.cq_dbr = (uint32_t *)data_from_host->rq_cq_transf.cq_dbr_daddr;
    app_ctx.rq_cq_ctx.cqe = app_ctx.rq_cq_ctx.cq_ring;
    app_ctx.rq_cq_ctx.cq_idx = 0;
    app_ctx.rq_cq_ctx.cq_hw_owner_bit = 0x1;
    app_ctx.rq_cq_ctx.log_cq_depth = data_from_host->rq_cq_transf.log_cq_depth;

    /* Initialize RQ context */
    app_ctx.rq_ctx.rq_number = data_from_host->rq_transf.wq_num;
    app_ctx.rq_ctx.rq_ring = (struct flexio_dev_wqe_rcv_data_seg *)data_from_host->rq_transf.wq_ring_daddr;
    app_ctx.rq_ctx.rq_dbr = (uint32_t *)data_from_host->rq_transf.wq_dbr_daddr;

    /* Initialize SQ context */
    app_ctx.sq_ctx.sq_number = data_from_host->sq_transf.wq_num;
    app_ctx.sq_ctx.sq_ring = (union flexio_dev_sqe_seg *)data_from_host->sq_transf.wq_ring_daddr;
    app_ctx.sq_ctx.sq_wqe_seg_idx = 0;
    app_ctx.sq_ctx.sq_pi = 0;

    /* Initialize SQ CQ context */
    app_ctx.sq_cq_ctx.cq_number = data_from_host->sq_cq_transf.cq_num;
    app_ctx.sq_cq_ctx.cq_ring = (struct flexio_dev_cqe64 *)data_from_host->sq_cq_transf.cq_ring_daddr;
    app_ctx.sq_cq_ctx.cq_dbr = (uint32_t *)data_from_host->sq_cq_transf.cq_dbr_daddr;
    app_ctx.sq_cq_ctx.cqe = app_ctx.sq_cq_ctx.cq_ring;
    app_ctx.sq_cq_ctx.cq_idx = 0;
    app_ctx.sq_cq_ctx.cq_hw_owner_bit = 0x1;
    app_ctx.sq_cq_ctx.log_cq_depth = data_from_host->sq_cq_transf.log_cq_depth;

    /* Initialize SQ data ring */
    app_ctx.dt_ctx.sq_tx_buff = (void *)data_from_host->sq_transf.wqd_daddr;
    app_ctx.dt_ctx.tx_buff_idx = 0;
}

/* Process incoming packet: Swap L2/L3/L4 headers and send back to wire */
static void process_and_mirror_packet(void)
{
    uint32_t rq_wqe_idx;
    uint32_t data_sz;
    struct flexio_dev_wqe_rcv_data_seg *rwqe;
    uint8_t *rq_data;
    uint8_t *sq_data;
    union flexio_dev_sqe_seg *swqe;

    /* Extract WQE counter and byte size from current CQE */
    rq_wqe_idx = flexio_dev_cqe_get_wqe_counter(app_ctx.rq_cq_ctx.cqe);
    data_sz = flexio_dev_cqe_get_byte_cnt(app_ctx.rq_cq_ctx.cqe);

    /* Locate the RQ WQE */
    rwqe = &app_ctx.rq_ctx.rq_ring[rq_wqe_idx & RQ_IDX_MASK];
    rq_data = (uint8_t *)flexio_dev_rwqe_get_addr(rwqe);

    /* Allocate next buffer chunk in the SQ TX data ring */
    sq_data = (uint8_t *)app_ctx.dt_ctx.sq_tx_buff +
              ((app_ctx.dt_ctx.tx_buff_idx++ & DATA_IDX_MASK) << LOG_WQD_CHUNK_BSIZE);

    /* Copy received payload into TX buffer */
    memcpy(sq_data, rq_data, data_sz);

    /* Verify packet size: Ethernet (14) + IPv4 (min 20) + UDP (8) = 42 bytes minimum */
    if (data_sz >= 42) {
        /* 1. L2 Swap: Destination MAC <-> Source MAC */
        swap_mac_addresses(sq_data);

        /* 2. Check EtherType (0x0800 = IPv4) */
        uint16_t ethertype = ((uint16_t)sq_data[12] << 8) | sq_data[13];
        if (ethertype == 0x0800) {
            uint8_t ihl = (sq_data[14] & 0x0FU) * 4U;
            uint8_t protocol = sq_data[23];

            /* Check protocol (0x11 = UDP) and valid header length */
            if (protocol == 0x11 && ihl >= 20 && data_sz >= (uint32_t)(14 + ihl + 8)) {
                /* 3. L3 Swap: IPv4 Source IP <-> Destination IP */
                /* IPv4 src IP is at offset 26, dst IP at offset 30 */
                swap_u32((uint32_t *)(sq_data + 26), (uint32_t *)(sq_data + 30));

                /* Note on IPv4 Header Checksum:
                 * Swapping two 32-bit words in 1's complement arithmetic leaves
                 * the sum unchanged: (A + B == B + A).
                 * Therefore, the IPv4 header checksum remains valid!
                 */

                /* 4. L4 Swap: UDP Source Port <-> Destination Port */
                uint8_t *udp_hdr = sq_data + 14 + ihl;
                swap_u16((uint16_t *)(udp_hdr + 0), (uint16_t *)(udp_hdr + 2));

                /* Note on UDP Checksum:
                 * The UDP checksum covers:
                 * - Pseudo header: Src IP, Dst IP, Protocol, UDP Length
                 * - UDP header: Src Port, Dst Port, UDP Length
                 * - UDP payload
                 * Since Src/Dst IP and Src/Dst Port are both swapped, the 16-bit
                 * 1's complement sum is mathematically invariant.
                 * If the original UDP checksum was non-zero, it remains 100% valid.
                 * If 0 (disabled), it remains 0.
                 */
            }
        }
    }

    /* Prepare Send WQE (Control seg, Ethernet seg, Data seg) */
    swqe = get_next_sqe(&app_ctx.sq_ctx);
    flexio_dev_swqe_seg_ctrl_set(swqe, app_ctx.sq_ctx.sq_pi, app_ctx.sq_ctx.sq_number,
                                 FLEXIO_CTRL_SEG_CE_CQE_ON_CQE_ERROR,
                                 FLEXIO_CTRL_SEG_TYPE_SEND_EN);

    swqe = get_next_sqe(&app_ctx.sq_ctx);
    flexio_dev_swqe_seg_eth_set(swqe, 0, 0, 0, NULL);

    swqe = get_next_sqe(&app_ctx.sq_ctx);
    flexio_dev_swqe_seg_mem_ptr_data_set(swqe, data_sz, app_ctx.lkey, (uint64_t)sq_data);

    /* Send WQE takes 4 segments: skip the 4th segment */
    (void)get_next_sqe(&app_ctx.sq_ctx);

    /* Memory barrier and ring SQ doorbell */
    __dpa_thread_fence(__DPA_MEMORY, __DPA_W, __DPA_W);
    flexio_dev_qp_sq_ring_db(++app_ctx.sq_ctx.sq_pi, app_ctx.sq_ctx.sq_number);

    /* Memory barrier and increment RQ Producer Index */
    __dpa_thread_fence(__DPA_MEMORY, __DPA_W, __DPA_W);
    flexio_dev_dbr_rq_inc_pi(app_ctx.rq_ctx.rq_dbr);
}

/*
 * Entry point for DPA event handler.
 * Called when an event arrives on the RQ CQ.
 */
flexio_dev_event_handler_t dpa_mirror_handler;
__dpa_global__ void dpa_mirror_handler(uint64_t thread_arg)
{
    struct host2dev_mirror_data *data_from_host = (struct host2dev_mirror_data *)thread_arg;

    /* Initialize context on first invocation */
    if (!data_from_host->initialized) {
        app_ctx_init(data_from_host);
        data_from_host->initialized = 1;
    }

    /* Process all available CQEs */
    while (flexio_dev_cqe_get_owner(app_ctx.rq_cq_ctx.cqe) !=
           app_ctx.rq_cq_ctx.cq_hw_owner_bit) {
        
        flexio_dev_print("[DPA] Mirrored packet #%lu\n", (unsigned long)++app_ctx.packets_count);

        __dpa_thread_fence(__DPA_MEMORY, __DPA_R, __DPA_R);
        process_and_mirror_packet();
        step_cq(&app_ctx.rq_cq_ctx);
    }

    /* Memory barrier before arming CQ */
    __dpa_thread_fence(__DPA_MEMORY, __DPA_W, __DPA_W);

    /* Arm CQ for the next incoming packet */
    flexio_dev_cq_arm(app_ctx.rq_cq_ctx.cq_idx, app_ctx.rq_cq_ctx.cq_number);

    /* Reschedule thread to wait for next event */
    flexio_dev_thread_reschedule();
}

