# BlueField-3 DPA Packet Mirroring: DPA Device-Side Implementation Guide

This document describes the design, compilation, memory model, and packet-processing algorithms executing directly on the **Data Path Accelerator (DPA)** of the **NVIDIA BlueField-3**.

---

## 1. DPA Hardware & Execution Model

The BlueField-3 DPA contains an array of programmable **RISC-V cores** embedded adjacent to the ConnectX-7 network ASIC. These cores operate at extremely high packet rates with sub-microsecond latency and have direct memory access to internal on-chip SRAM/heap and hardware queue engines.

* **Event-Driven Execution**: DPA threads are not general-purpose OS threads with time-slicing. Instead, they operate as **hardware event handlers**.
* **Trigger Mechanism**: When a network packet arrives and the NIC generates a Completion Queue Entry (CQE) on the Receive CQ, the hardware scheduler immediately wakes up the registered DPA thread on an available RISC-V core.
* **Non-Blocking Run-to-Completion**: The handler drains available CQEs, executes the packet modification, dispatches outgoing Send WQEs, re-arms the CQ, and yields via `flexio_dev_thread_reschedule()`.

---

## 2. Compilation with DPACC

DPA device code is written in C and compiled using NVIDIA's dedicated compiler driver, **DPACC** (`/opt/mellanox/doca/tools/dpacc`):

```bash
dpacc src/device/dpa_mirror_dev.c -o build/dpa_mirror_dev.a \
    -hostcc=gcc \
    --mcpu=nv-dpa-bf3 \
    --devicecc-options="-DE_MODE_LE,-ffreestanding,-mcmodel=medany,-DFLEXIO_DEV_ALLOW_EXPERIMENTAL_API,-DFLEXIO_DEV_VER_USED=FLEXIO_DEV_VER_LATEST,-Wall,-Wextra,-O3,-Iinclude,-I/opt/mellanox/flexio/include" \
    -hostcc-options="-fPIC,-DFLEXIO_ALLOW_EXPERIMENTAL_API" \
    --flto \
    --app-name=dpa_mirror_app
```

### Key Compilation Flags:
* `--mcpu=nv-dpa-bf3`: Targets the BlueField-3 RISC-V instruction set.
* `-DE_MODE_LE`: Configures little-endian byte ordering.
* `-mcmodel=medany`: Generates code using the RISC-V Medium-Any code model, enabling relative addressing within $\pm 2$ GiB.
* `-ffreestanding`: Directs Clang to compile without standard C library runtime dependencies. FlexIO provides its own lightweight libc (`<libflexio-libc/string.h>`).
* `--app-name=dpa_mirror_app`: Emits embedded registration metadata into the host archive stub so `flexio_app_get()` can resolve the application by name at runtime.

---

## 3. Data Structures & Ring Buffers

The device code maintains lightweight context structures to track queues directly in DPA memory:

```
+---------------------------------------------------------------------------------+
|                                 DPA APP CONTEXT                                 |
|                                                                                 |
|   +-----------------------+     +-----------------------+                       |
|   |   RQ CQ (128 CQEs)    |     |   RQ Ring (128 WQEs)  |                       |
|   |   cq_ring, cqe, cq_ci |     |   rq_ring, rq_dbr     |                       |
|   +-----------+-----------+     +-----------+-----------+                       |
|               |                             |                                   |
|               | Receive Trigger             | Extracts Buffer                   |
|               v                             v                                   |
|   +-----------------------------------------------------+                       |
|   |             dpa_mirror_handler(thread_arg)          |                       |
|   |             process_and_mirror_packet()             |                       |
|   +---------------------------+-------------------------+                       |
|                               |                                                 |
|                               v Sends Out                                       |
|   +-----------------------+     +-----------------------+                       |
|   |   SQ Ring (128 WQEs)  |     | SQ Data (128 x 2048B) |                       |
|   |   sq_ring, sq_pi      |     |   sq_tx_buff, lkey    |                       |
|   +-----------------------+     +-----------------------+                       |
+---------------------------------------------------------------------------------+
```

### Context Breakdown:
* **`dpa_cq_ctx_t`**: Holds pointer to the CQE array (`struct flexio_dev_cqe64 *`), consumer index (`cq_idx`), and the hardware owner polarity bit (`cq_hw_owner_bit`).
* **`dpa_rq_ctx_t`**: Holds pointer to the Receive WQE array (`struct flexio_dev_wqe_rcv_data_seg *`) and the Doorbell Record address (`rq_dbr`).
* **`dpa_sq_ctx_t`**: Holds pointer to the Send WQE array (`union flexio_dev_sqe_seg *`), producer index (`sq_pi`), and segment offset.
* **`dpa_dt_ctx_t`**: Manages the cyclical transmit data buffer ring (`sq_tx_buff`).

---

## 4. The Packet Processing Flow

### Step 1: First-Run Handshake
```c
if (!data_from_host->initialized) {
    app_ctx_init(data_from_host);
    data_from_host->initialized = 1;
}
```
On the first packet trigger, the DPA thread reads the `host2dev_mirror_data` structure located at `thread_arg`, initializes its local queue pointers, and sets `initialized = 1`.

---

### Step 2: Draining the CQE Ring
The DPA polls the current CQE owner bit against its expected software owner bit:
```c
while (flexio_dev_cqe_get_owner(app_ctx.rq_cq_ctx.cqe) !=
       app_ctx.rq_cq_ctx.cq_hw_owner_bit) {
    ...
    process_and_mirror_packet();
    step_cq(&app_ctx.rq_cq_ctx);
}
```
* **CQ Advance (`step_cq`)**:
  * Advances `cq_idx`.
  * If the index wraps around (`cq_idx & CQ_IDX_MASK == 0`), flips `cq_hw_owner_bit` polarity.
  * Updates the Consumer Index in the Doorbell Record (`flexio_dev_dbr_cq_set_ci`).

---

### Step 3: Buffer Retrieval & Payload Copy
1. The CQE counter gives the index of the RQ WQE:
   ```c
   rq_wqe_idx = flexio_dev_cqe_get_wqe_counter(app_ctx.rq_cq_ctx.cqe);
   data_sz    = flexio_dev_cqe_get_byte_cnt(app_ctx.rq_cq_ctx.cqe);
   ```
2. The WQE holds the direct DPA memory address of the incoming packet:
   ```c
   rwqe = &app_ctx.rq_ctx.rq_ring[rq_wqe_idx & RQ_IDX_MASK];
   rq_data = (uint8_t *)flexio_dev_rwqe_get_addr(rwqe);
   ```
3. A chunk is leased from the transmit data ring:
   ```c
   sq_data = (uint8_t *)app_ctx.dt_ctx.sq_tx_buff +
             ((app_ctx.dt_ctx.tx_buff_idx++ & DATA_IDX_MASK) << LOG_WQD_CHUNK_BSIZE);
   ```
4. The entire frame is copied to the transmit buffer:
   ```c
   memcpy(sq_data, rq_data, data_sz);
   ```

---

### Step 4: Header Swapping & Checksum Invariance

The packet is parsed and modified in-place:

```
Packet Layout (Ethernet + IPv4 + UDP):
0                   6                  12  14       26      30      34      36     38   40   42
+-------------------+-------------------+---+--------+-------+-------+-------+------+----+----+-------------------+
|  Destination MAC  |    Source MAC     |Typ|  IPv4  |Src IP |Dst IP |SrcPort|DstPort|Len |Csum|    UDP Payload    |
|      (6 bytes)    |     (6 bytes)     |0800| Header|4 bytes|4 bytes|2 bytes|2 bytes|2 B |2 B |     (N bytes)     |
+-------------------+-------------------+---+--------+-------+-------+-------+------+----+----+-------------------+
          |                   |                          |       |       |       |                 |
          +---- SWAP (L2) ----+                          +-SWAP -+       +-SWAP -+                 |
                                                           (L3)            (L4)                    |
                                                                                                   v
                                                                                           Payload Unchanged
```

#### 1. L2 Swap (Ethernet MAC Addresses):
```c
uint8_t tmp[6];
memcpy(tmp, sq_data, 6);
memcpy(sq_data, sq_data + 6, 6);
memcpy(sq_data + 6, tmp, 6);
```

#### 2. L3 Swap (IPv4 Addresses):
```c
uint32_t *src_ip = (uint32_t *)(sq_data + 26);
uint32_t *dst_ip = (uint32_t *)(sq_data + 30);
swap_u32(src_ip, dst_ip);
```

#### 3. L4 Swap (UDP Ports):
```c
uint8_t *udp_hdr = sq_data + 14 + ihl;
swap_u16((uint16_t *)(udp_hdr + 0), (uint16_t *)(udp_hdr + 2));
```

---

## 5. Mathematical Proof of Checksum Invariance

A common performance pitfall in packet processors is the need to recalculate IPv4 and UDP checksums after modifying headers. However, because our mirror operation strictly swaps symmetric fields, **no checksum recalculation is required**.

### Proof for IPv4 Header Checksum:
The IPv4 checksum $C_{\text{IP}}$ is defined by RFC 791 as the 16-bit 1's complement sum of all 16-bit words in the IPv4 header:
$$S_{\text{IP}} = \sum_{i=1}^{n} W_i$$
Let the 32-bit Source IP be represented as two 16-bit words $(S_1, S_2)$, and Destination IP as $(D_1, D_2)$. The header sum is:
$$S_{\text{original}} = \text{Rest} + S_1 + S_2 + D_1 + D_2 \pmod{2^{16} - 1}$$
When swapping Source and Destination IP addresses:
$$S_{\text{modified}} = \text{Rest} + D_1 + D_2 + S_1 + S_2 \pmod{2^{16} - 1}$$
Because 1's complement addition is **commutative and associative**:
$$S_{\text{original}} \equiv S_{\text{modified}}$$
Therefore, **the IPv4 header checksum remains 100% valid**.

### Proof for UDP Checksum:
The UDP checksum covers:
1. The IPv4 Pseudo-Header: $\text{Src IP} + \text{Dst IP} + \text{Protocol (17)} + \text{UDP Length}$
2. The UDP Header: $\text{Src Port} + \text{Dst Port} + \text{UDP Length} + \text{Payload Words}$

Under our transformation:
* $\text{Src IP} \leftrightarrow \text{Dst IP}$ (sum is unchanged)
* $\text{Src Port} \leftrightarrow \text{Dst Port}$ (sum is unchanged)
* $\text{Payload}$ (untouched)

Because all swapped values are summed together in the same 16-bit accumulator:
$$\Sigma_{\text{UDP, modified}} = \Sigma_{\text{UDP, original}}$$
* If the incoming checksum was non-zero, it remains **valid without modification**.
* If the incoming checksum was `0x0000` (checksum disabled per RFC 768), it remains `0x0000`.

---

## 6. Transmitting the Mirrored Packet (SQ WQE Construction)

An mlx5 Send Work Queue Element (SQE) consists of four 16-byte segments (WQEBBs):

1. **Segment 0: Control Segment (`flexio_dev_swqe_seg_ctrl_set`)**:
   * Sets SQ number, producer index (`sq_pi`).
   * Configures completion type: `FLEXIO_CTRL_SEG_CE_CQE_ON_CQE_ERROR` (only report on error).
   * Sets operation code: `FLEXIO_CTRL_SEG_TYPE_SEND_EN`.
2. **Segment 1: Ethernet Segment (`flexio_dev_swqe_seg_eth_set`)**:
   * Specifies L3/L4 checksum offload flags and VLAN insertion if applicable.
3. **Segment 2: Data Segment (`flexio_dev_swqe_seg_mem_ptr_data_set`)**:
   * Points to `sq_data` in DPA memory.
   * Specifies byte length `data_sz` and memory key `lkey`.
4. **Segment 3: Padding**:
   * Skipped via `get_next_sqe(&app_ctx.sq_ctx)` to maintain 64-byte alignment.

### Doorbell Ringing & Barriers:
```c
/* Ensure all writes to the SQ WQE are flushed to memory */
__dpa_thread_fence(__DPA_MEMORY, __DPA_W, __DPA_W);

/* Ring SQ Doorbell to dispatch packet to the network ASIC */
flexio_dev_qp_sq_ring_db(++app_ctx.sq_ctx.sq_pi, app_ctx.sq_ctx.sq_number);

/* Memory barrier before updating RQ Doorbell */
__dpa_thread_fence(__DPA_MEMORY, __DPA_W, __DPA_W);

/* Increment RQ Producer Index to replenish the consumed receive slot */
flexio_dev_dbr_rq_inc_pi(app_ctx.rq_ctx.rq_dbr);
```

---

## 7. Re-Arming & Thread Yield

Once all packets in the CQ ring are processed:

1. **Memory Barrier**:
   ```c
   __dpa_thread_fence(__DPA_MEMORY, __DPA_W, __DPA_W);
   ```
2. **Arm CQ**:
   ```c
   flexio_dev_cq_arm(app_ctx.rq_cq_ctx.cq_idx, app_ctx.rq_cq_ctx.cq_number);
   ```
   Informs the hardware CQ engine to generate an event notification when the next CQE arrives.
3. **Yield execution**:
   ```c
   flexio_dev_thread_reschedule();
   ```
   Suspends the DPA thread, freeing the execution unit until the next network packet triggers it.

