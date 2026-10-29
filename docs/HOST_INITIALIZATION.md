# BlueField-3 DPA Packet Mirroring: Host-Side Architecture & Lifecycle Guide

This document provides a comprehensive, step-by-step breakdown of everything initialized, configured, and executed on the **Host side** (running either on an x86_64 host in NIC mode or on the BlueField-3 ARM SoC in DPU mode).

---

## 1. High-Level Architecture Overview

The Host application acts as the **control plane** and **resource allocator** for the DPA:
1. It queries the NIC hardware and opens the InfiniBand/DevX driver context.
2. It allocates memory spaces directly on the **DPA internal heap**.
3. It sets up queues (SQ, RQ, and their associated CQs).
4. It loads the compiled DPA kernel binary and links it to an **Event Handler**.
5. It configures the hardware **Flow Steering** (Direct Rules) so that incoming traffic bypasses the OS network stack and lands directly into the DPA Receive Queue.
6. It triggers the event handler and monitors stdout messages streamed from the DPA.

```
+-----------------------------------------------------------------------------------------+
|                                      HOST CONTROLLER                                    |
|                                                                                         |
|  1. FlexIO Version Negotiation                                                          |
|  2. Open IBV Device Context (mlx5_0)                                                    |
|  3. Load DPA Binary into DPA Process                                                    |
|  4. Extract Process PD & UAR                                                            |
|  5. Allocate DPA Heap Buffers (CQ Rings, DBRs, SQ/RQ Rings, Packet Buffers)              |
|  6. Register Memory Keys (MKeys) for DPA Local Write Access                             |
|  7. Create DPA Event Handler (Points to dpa_mirror_handler)                             |
|  8. Create Queues: SQ + SQ CQ, RQ + RQ CQ (Connected to Event Handler Thread)          |
|  9. Populate RQ Ring with initial Receive WQEs & Arm DBR                                |
| 10. Install DevX Flow Steering Rules (NIC_RX TIR steering + FDB TX forward to wire)     |
| 11. Copy Runtime Configuration Struct to DPA Heap                                       |
| 12. Run Event Handler & Enter Monitor Loop                                              |
| 13. Graceful Teardown (Reverse Order)                                                   |
+-----------------------------------------------------------------------------------------+
```

---

## 2. Step-by-Step Initialization Sequence

### Step 1: FlexIO Version Handshake
* **Function**: `flexio_version_set(FLEXIO_VER_USED)`
* **Purpose**: FlexIO binaries and host libraries maintain strict ABI compatibility. The host specifies the version macro (`FLEXIO_VER_LATEST`), ensuring that the runtime library structures and driver ioctl definitions match.
* **Failure Mode**: Returns non-zero if the driver or installed `libflexio` version is incompatible.

---

### Step 2: IB Device Discovery & Context Acquisition
* **Functions**: `ibv_get_device_list()`, `ibv_open_device()`
* **Purpose**: Enumerates the InfiniBand/RDMA devices installed in the system. The application selects the requested device (default `mlx5_0`) and opens a low-level driver context (`struct ibv_context *`).
* **Context**: This context exposes direct hardware registers via the `mlx5` user-space driver.

---

### Step 3: Loading the DPA Application & Creating the DPA Process
* **Functions**: `flexio_app_get()`, `flexio_process_create()`
* **How DPA Code is Linked**:
  * The DPA kernel (`dpa_mirror_dev.c`) is compiled ahead-of-time by `dpacc` into a static archive (`dpa_mirror_dev.a`).
  * The linker embeds this archive into the host binary using `--whole-archive`. DPACC generates static initialization tables that register the DPA ELF image under the application name `"dpa_mirror_app"`.
* **Execution**:
  * `flexio_app_get(&sel_attr, &flexio_app)` queries the internal registry for the application named `"dpa_mirror_app"` compiled for the target hardware model (`FLEXIO_HW_MODEL_DEF` $\rightarrow$ BlueField-3 / `nv-dpa-bf3`).
  * `flexio_process_create(ibv_ctx, flexio_app, NULL, &flexio_process)` uploads the DPA program code into the DPA instruction and data memories, creating an isolated **DPA Process** sandbox.

---

### Step 4: Extracting Protection Domain (PD) and User Access Region (UAR)
* **Functions**: `flexio_process_get_pd()`, `flexio_process_get_uar()`
* **Protection Domain (PD)**: Grants permission to associate queues, memory keys, and buffers within the same hardware isolation boundary.
* **User Access Region (UAR)**: The memory-mapped I/O (MMIO) register space used by both the host and the DPA to ring hardware doorbells (triggering queue processing directly without kernel traps).

---

### Step 5: Setting Up the DPA Message Stream
* **Function**: `flexio_msg_stream_create()`
* **Attributes**:
  * `transport_mode = FLEXIO_MSG_TRANSPORT_QP_RC` (Reliable Connection queue pair)
  * `sync_mode = FLEXIO_MSG_DEV_SYNC_MODE_SYNC`
  * `level = FLEXIO_MSG_DEV_INFO`
* **Purpose**: The DPA cores cannot directly access Linux `stdout`. This function sets up a dedicated high-speed internal ring buffer and RDMA connection so that `flexio_dev_print(...)` calls on the DPA are automatically streamed to the host's `stdout`.

---

### Step 6: Creating the DPA Event Handler
* **Function**: `flexio_event_handler_create()`
* **Attributes**:
  * `host_stub_func = dpa_mirror_handler`: Symbol stub generated by `dpacc` indicating the entry-point function inside the DPA RISC-V code.
  * `affinity.type = FLEXIO_AFFINITY_NONE`: Permits the hardware scheduler to execute the handler on any available DPA RISC-V execution core.
* **Thread Extraction**: `flexio_event_handler_get_thread(event_handler)` yields a DPA thread handle used in Step 8 to link incoming network packets directly to this event handler.

---

### Step 7: Send Queue (SQ) & SQ Completion Queue (CQ) Allocation
* **Components Allocated**:
  1. **SQ CQ Ring & DBR**:
     * `flexio_copy_from_host()` allocates a DPA heap buffer for the 64-byte CQEs (128 CQEs = 8192 bytes) and sets the hardware ownership bit (`mlx5dv_set_cqe_owner(cqe, 1)`).
     * Allocates a 64-bit Doorbell Record (DBR) on the DPA heap.
     * `flexio_cq_create()` creates the SQ CQ with `element_type = FLEXIO_CQ_ELEMENT_TYPE_NON_DPA_CQ` (the SQ completion does not trigger an event handler).
  2. **SQ Work Ring & Data Buffer**:
     * `flexio_buf_dev_alloc()` allocates the SQ WQE ring (128 WQEs $\times$ 64 bytes = 8192 bytes) on the DPA heap.
     * `flexio_buf_dev_alloc()` allocates the SQ packet payload ring (128 chunks $\times$ 2048 bytes = 256 KB) on the DPA heap.
  3. **Memory Key (MKey)**:
     * `flexio_device_mkey_create()` creates a hardware Memory Key spanning the 256 KB SQ packet buffer with `IBV_ACCESS_LOCAL_WRITE` permissions. The DPA will write this `lkey` into the send WQEs.
  4. **SQ Creation**:
     * `flexio_sq_create()` registers the SQ in hardware, binding it to the SQ CQ, the DPA UAR, and the DPA-allocated ring memory.

---

### Step 8: Receive Queue (RQ) & RQ CQ Allocation
* **Components Allocated**:
  1. **RQ CQ**:
     * `flexio_cq_create()` creates the RQ CQ with:
       * `element_type = FLEXIO_CQ_ELEMENT_TYPE_DPA_THREAD`
       * `thread = flexio_event_handler_get_thread(event_handler)`
       * **Key Mechanism**: Whenever a packet arrives in the RQ, the hardware automatically generates a CQE and triggers the DPA Event Handler!
  2. **RQ Ring & Data Buffer**:
     * Allocates the RQ WQE ring (8192 bytes) and the RQ packet data buffer (256 KB) on the DPA heap.
     * Creates an MKey for the RQ data buffer.
  3. **WQE Initialization (Pre-Posting Buffers)**:
     * An RQ cannot receive packets until Work Queue Elements (WQEs) are posted to point to available memory buffers.
     * The host prepares 128 `mlx5_wqe_data_seg` entries locally on the CPU:
       ```cpp
       mlx5dv_set_data_seg(&rx_wqes[i], 2048, rq_mkey_id, curr_dpa_addr);
       ```
     * Copies all 128 WQEs to the DPA heap using `flexio_host2dev_memcpy()`.
  4. **Doorbell Record Arming**:
     * Writes the producer index (`Q_DEPTH & 0xFFFF` = 128) into the DPA heap DBR. The NIC now knows 128 receive slots are ready to accept traffic.
  5. **RQ Creation**:
     * `flexio_rq_create()` activates the hardware RQ.

---

### Step 9: Flow Steering (Direct Rules / DevX) Configuration
Flow steering ensures that targeted network packets are diverted to the DPA before the Linux kernel or OVS can touch them.

#### A. Ingress (RX) Rule:
* **Domain**: `MLX5DV_DR_DOMAIN_TYPE_NIC_RX`
* **Matcher Mask**:
  * Outer Ethernet `ethertype = 0xFFFF`
  * Outer IP `ip_protocol = 0xFF`
  * Outer UDP `udp_dport = 0xFFFF`
* **Matcher Value**:
  * `ethertype = 0x0800` (IPv4)
  * `ip_protocol = 0x11` (UDP)
  * `udp_dport = <target_port>` (e.g. 12345)
* **Action**: `mlx5dv_dr_action_create_dest_devx_tir(flexio_rq_get_tir(rq))`
  * Diverts the packet directly to the DPA RQ's Transport Interface Receive (TIR) object.

#### B. Egress (TX) Rule (DPU Mode / BlueField ARM SoC):
* In DPU mode, the card functions as an embedded switch (eSwitch / FDB domain). Packets sent by the DPA SQ land on the internal switch and are dropped by default unless forwarded to the physical uplink (`vport 0xFFFF`).
* **Domain**: `MLX5DV_DR_DOMAIN_TYPE_FDB`
* **Matcher Mask**:
  * Outer `dmac = 0xFFFFFFFFFFFF`
* **Matcher Value**:
  * `dmac = <client_mac>` (the MAC of `fips1`, e.g. `a0:88:c2:b6:14:1a`)
* **Action Hierarchy**:
  * Rule 1: Root Table $\rightarrow$ SWS Table (`mlx5dv_dr_action_create_dest_table`)
  * Rule 2: SWS Table $\rightarrow$ Wire Vport (`mlx5dv_dr_action_create_dest_vport(dom, 0xFFFF)`)

---

### Step 10: Copying Runtime Configuration to DPA Heap
* **Struct**: `struct host2dev_mirror_data`
  * Transfers CQ/WQ numbers, log depths, DPA ring addresses, and DBR addresses for both SQ and RQ.
* **Function**: `flexio_copy_from_host(process, &config, sizeof(config), &app_data_daddr)`
* The allocated DPA memory pointer (`app_data_daddr`) is passed as the argument to the DPA event handler.

---

### Step 11: Starting the DPA Event Handler
* **Function**: `flexio_event_handler_run(event_handler, app_data_daddr)`
* **Effect**: Transitions the DPA Event Handler from the `INIT` state to the `RUNNING` state.
* From this instant, every incoming UDP packet on port 12345 triggers execution of `dpa_mirror_handler` on the DPA RISC-V cores.

---

### Step 12: Monitor Loop & Graceful Teardown
* The host process sleeps and monitors POSIX signals (`SIGINT`, `SIGTERM`).
* **Teardown Sequence (Strict Reverse Order)**:
  1. Destroy Flow Rules (`mlx5dv_dr_rule_destroy`)
  2. Destroy Flow Matchers (`mlx5dv_dr_matcher_destroy`)
  3. Free DPA Configuration Struct (`flexio_buf_dev_free`)
  4. Destroy Memory Keys (`flexio_device_mkey_destroy`)
  5. Destroy Queues: SQ, RQ (`flexio_sq_destroy`, `flexio_rq_destroy`)
  6. Free Queue Rings and Data Buffers on DPA Heap (`flexio_buf_dev_free`)
  7. Destroy CQs (`flexio_cq_destroy`)
  8. Destroy Event Handler (`flexio_event_handler_destroy`)
  9. Destroy Message Stream (`flexio_msg_stream_destroy`)
  10. Destroy FlexIO Process (`flexio_process_destroy`)
  11. Close IBV Device Context (`ibv_close_device`)

