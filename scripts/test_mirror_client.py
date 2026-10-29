#!/usr/bin/env python3
"""
test_mirror_client.py - Test client for BlueField-3 DPA packet mirroring service.

Sends UDP test packets to the DPA service, waits for the echoed response,
and validates payload integrity and Round-Trip Time (RTT).
"""

import argparse
import socket
import sys
import time


def main():
    parser = argparse.ArgumentParser(
        description="BlueField-3 DPA UDP Packet Mirror Verification Client"
    )
    parser.add_argument(
        "--ip",
        "-i",
        required=True,
        help="Target IP address (IP configured on the BlueField-3 interface)",
    )
    parser.add_argument(
        "--port",
        "-p",
        type=int,
        default=12345,
        help="Target UDP destination port (default: 12345)",
    )
    parser.add_argument(
        "--count",
        "-c",
        type=int,
        default=5,
        help="Number of test packets to send (default: 5)",
    )
    parser.add_argument(
        "--timeout",
        "-t",
        type=float,
        default=2.0,
        help="Receive timeout in seconds (default: 2.0)",
    )
    parser.add_argument(
        "--message",
        "-m",
        default="DPA-Echo-Test-Payload",
        help="Base test message payload",
    )

    args = parser.parse_args()

    print("==================================================")
    print("  BlueField-3 DPA UDP Echo / Mirror Test Client   ")
    print(f"  Target:  {args.ip}:{args.port}")
    print(f"  Packets: {args.count}")
    print(f"  Timeout: {args.timeout}s")
    print("==================================================")

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.settimeout(args.timeout)

    success_count = 0
    rtts = []

    for seq in range(1, args.count + 1):
        ts = time.time()
        payload = f"{args.message} seq={seq} timestamp={ts:.6f}".encode("utf-8")

        start_time = time.perf_counter()
        try:
            sock.sendto(payload, (args.ip, args.port))
            data, addr = sock.recvfrom(2048)
            end_time = time.perf_counter()

            rtt_us = (end_time - start_time) * 1_000_000
            rtts.append(rtt_us)

            if data == payload:
                success_count += 1
                print(
                    f"[{seq}/{args.count}] Received {len(data)} bytes from {addr[0]}:{addr[1]} "
                    f"in {rtt_us:.1f} µs | Payload match: OK"
                )
            else:
                print(
                    f"[{seq}/{args.count}] Received corrupted or modified payload from {addr}:"
                )
                print(f"  Sent: {payload}")
                print(f"  Recv: {data}")

        except socket.timeout:
            print(f"[{seq}/{args.count}] Request timed out after {args.timeout}s")
        except Exception as e:
            print(f"[{seq}/{args.count}] Error: {e}")

        if seq < args.count:
            time.sleep(0.1)

    sock.close()

    print("\n----------------- Summary -----------------")
    print(f"Sent: {args.count}, Received: {success_count}, Lost: {args.count - success_count}")
    if rtts:
        min_rtt = min(rtts)
        max_rtt = max(rtts)
        avg_rtt = sum(rtts) / len(rtts)
        print(f"RTT (min / avg / max): {min_rtt:.1f} / {avg_rtt:.1f} / {max_rtt:.1f} µs")

    if success_count == args.count:
        print("Test PASSED: All packets mirrored successfully.")
        sys.exit(0)
    else:
        print("Test FAILED or partial packet loss detected.")
        sys.exit(1)


if __name__ == "__main__":
    main()

