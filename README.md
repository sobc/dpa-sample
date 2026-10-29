# BlueField-3 DPA Packet Mirroring (FlexIO + CMake + CPM)

High-performance hardware packet mirroring service running directly on the **Data Path Accelerator (DPA)** of the **NVIDIA BlueField-3 DPU**, built with the **FlexIO SDK**, modern **CMake**, and **CPM.cmake**.

---

## Overview

Incoming UDP packets matching a configured port (default `12345`) are steered directly into the BlueField-3 DPA Receive Queue (RQ). The DPA RISC-V cores process the packet entirely in hardware:
* **L2 Swap**: Swaps Ethernet Destination and Source MAC addresses.
* **L3 Swap**: Swaps IPv4 Source and Destination IP addresses.
* **L4 Swap**: Swaps UDP Source and Destination ports.
* **Payload & Checksums**: The payload is mirrored back untouched. Because 1's complement addition is commutative, swapping IP addresses and UDP ports preserves the exact checksum validity (RFC 1624).
* **Transmission**: Transmits the mirrored packet directly back onto the wire with line-rate sub-microsecond latency.

---

## Detailed Documentation

For in-depth architectural and implementation explanations, refer to:
* **[Host-Side Initialization & Lifecycle Guide](docs/HOST_INITIALIZATION.md)**: Comprehensive, step-by-step breakdown of every host-side queue, MKey, UAR, and Flow Steering initialization step.
* **[DPA Device-Side Implementation Guide](docs/DPA_DEVICE_IMPLEMENTATION.md)**: Deep dive into the DPA RISC-V execution model, DPACC compilation, memory barriers, WQE construction, and the mathematical proof of checksum invariance.
* **[Latency Benchmark Results (With vs. Without VMA)](docs/BENCHMARK_RESULTS.md)**: Detailed 500-packet latency measurements, statistical analysis, and the full C benchmark client source code.

---

## Architecture

```
                       +------------------------------------+
                       |           Host Controller          |
                       | (C++20: x86_64 host or BF3 ARM SoC)|
                       +-----------------+------------------+
                                         |
                       DevX Flow Steering| FlexIO Process / MKeys
                                         v
+-------------------------------------------------------------------------------+
| NVIDIA BlueField-3                                                            |
|                                                                               |
|  [ Ingress Packet ] ---> DevX Flow Steering (UDP Port 12345)                  |
|                                     |                                         |
|                                     v                                         |
|                         +-----------------------+                             |
|                         |  DPA RQ / CQ (Ring)   |                             |
|                         +-----------+-----------+                             |
|                                     | CQE Trigger                             |
|                                     v                                         |
|                         +-----------------------+                             |
|                         |    DPA Kernel (C)     |                             |
|                         |   dpa_mirror_dev.c    |                             |
|                         |   - Swap MACs         |                             |
|                         |   - Swap IPs          |                             |
|                         |   - Swap UDP Ports    |                             |
|                         +-----------+-----------+                             |
|                                     | SQ WQE                                  |
|                                     v                                         |
|                         +-----------------------+                             |
|                         |  DPA SQ / CQ (Ring)   |                             |
|                         +-----------+-----------+                             |
|                                     |                                         |
|  [ Mirrored Reply ] <---------------+ (Direct to Wire / vport)               |
+-------------------------------------------------------------------------------+
```

---

## Execution Modes

The host application supports both primary deployment modes:

1. **DPU Mode (ARM SoC - Recommended)**:
   * Runs directly on the BlueField-3 ARM Ubuntu OS.
   * Programs internal hardware flow steering to route mirrored packets directly back out to the wire (`vport`).
   * Command:
     ```bash
     sudo ./build/dpa_mirror -d mlx5_0 -p 12345 --dpu-mode
     ```
2. **NIC / Host Mode (x86_64 Server)**:
   * Runs on the x86 host server connected to the BlueField-3 via PCIe.
   * Automatically defaults to NIC mode when compiled/run on x86_64.
   * Command:
     ```bash
     sudo ./build/dpa_mirror -d mlx5_0 -p 12345 --nic-mode
     ```

---

## Prerequisites

* **NVIDIA DOCA & FlexIO SDK**:
  * Installed libraries: `libflexio`, `libibverbs`, `libmlx5`
  * DPA Compiler: `dpacc` (`/opt/mellanox/doca/tools/dpacc`)
* **Build Tools**:
  * CMake $\ge$ 3.20
  * C++20 compiler (`g++` or `clang++`)
  * `pkg-config`
  * Root privileges (`sudo`) for DevX hardware access

---

## Building the Project

The project uses CMake and [CPM.cmake](https://github.com/cpm-cmake/CPM.cmake) (no Meson required). Dependencies like `CLI11` are fetched automatically.

```bash
# 1. Configure the build
cmake -B build -S . -DCMAKE_BUILD_TYPE=Release

# 2. Compile host executable and DPA device archive
cmake --build build -j
```

---

## Running the Service

Start the DPA mirror service with root privileges:

```bash
# Basic run with default mlx5_0 and UDP port 12345:
sudo ./build/dpa_mirror -d mlx5_0 -p 12345

# Command line options:
#   -d, --device <dev>   InfiniBand / mlx5 device name (default: mlx5_0)
#   -p, --port <port>    UDP destination port to mirror (default: 12345)
#   --nic-mode           Host / NIC mode (default on x86_64)
#   --dpu-mode           DPU mode (default on ARM64)
```

---

## Testing and Verification

A test Python client is provided in `scripts/test_mirror_client.py`:

```bash
# Send test UDP packets and measure RTT:
python3 scripts/test_mirror_client.py --ip <BLUEFIELD_IP> --port 12345 --count 5
```

Example Output:
```
==================================================
  BlueField-3 DPA UDP Echo / Mirror Test Client   
  Target:  192.168.100.2:12345
  Packets: 5
  Timeout: 2.0s
==================================================
[1/5] Received 48 bytes from 192.168.100.2:12345 in 14.2 µs | Payload match: OK
[2/5] Received 48 bytes from 192.168.100.2:12345 in 12.8 µs | Payload match: OK
[3/5] Received 48 bytes from 192.168.100.2:12345 in 13.1 µs | Payload match: OK
[4/5] Received 48 bytes from 192.168.100.2:12345 in 12.9 µs | Payload match: OK
[5/5] Received 48 bytes from 192.168.100.2:12345 in 13.0 µs | Payload match: OK

----------------- Summary -----------------
Sent: 5, Received: 5, Lost: 0
RTT (min / avg / max): 12.8 / 13.2 / 14.2 µs
Test PASSED: All packets mirrored successfully.
```

Or test with standard `netcat`:
```bash
echo "Hello BlueField DPA" | nc -u -w1 <BLUEFIELD_IP> 12345
```

