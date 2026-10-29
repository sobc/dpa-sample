/*
 * dpa_mirror_com.h - Shared definitions between Host and DPA Device
 */

#ifndef DPA_MIRROR_COM_H
#define DPA_MIRROR_COM_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Logarithms for queue depths */
#define LOG_CQ_DEPTH 7       /* 128 CQEs */
#define LOG_RQ_DEPTH 7       /* 128 WQEs */
#define LOG_SQ_DEPTH 7       /* 128 WQEs */

/* SQE consists of multiple 16-byte segments (WQEBBs) */
#define LOG_SQE_NUM_SEGS 2

/* Size of WQD chunks: 2^11 = 2048 bytes (standard MTU / frame buffer) */
#define LOG_WQD_CHUNK_BSIZE 11
#define WQD_CHUNK_BSIZE (1U << LOG_WQD_CHUNK_BSIZE)

/* Type for 64-bit DPA addresses */
#ifndef FLEXIO_UINTPTR_T_DEFINED
typedef uint64_t flexio_uintptr_t;
#define FLEXIO_UINTPTR_T_DEFINED
#endif

/* Structure for transferring CQ configuration from Host to DPA */
struct app_transfer_cq {
    uint32_t cq_num;
    uint32_t log_cq_depth;
    flexio_uintptr_t cq_ring_daddr;
    flexio_uintptr_t cq_dbr_daddr;
} __attribute__((__packed__, aligned(8)));

/* Structure for transferring WQ (RQ / SQ) configuration from Host to DPA */
struct app_transfer_wq {
    uint32_t wq_num;
    uint32_t wqd_mkey_id;
    flexio_uintptr_t wq_ring_daddr;
    flexio_uintptr_t wq_dbr_daddr;
    flexio_uintptr_t wqd_daddr;
} __attribute__((__packed__, aligned(8)));

/* Primary shared context passed to the DPA event handler */
struct host2dev_mirror_data {
    struct app_transfer_cq rq_cq_transf;
    struct app_transfer_wq rq_transf;
    struct app_transfer_cq sq_cq_transf;
    struct app_transfer_wq sq_transf;
    uint32_t target_udp_port;  /* Configured UDP port */
    uint8_t initialized;       /* Flag to check if DPA context was initialized */
    uint8_t pad[3];
} __attribute__((__packed__, aligned(8)));

#ifdef __cplusplus
}
#endif

#endif /* DPA_MIRROR_COM_H */
