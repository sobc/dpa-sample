/*
 * test_mirror_client.c - High-Performance C Benchmark Client for DPA Packet
 * Mirroring
 *
 * Designed for sub-microsecond latency measurements with kernel bypass (e.g.,
 * libvma).
 */

#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <getopt.h>
#include <netinet/in.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

static inline double get_time_us(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
  return (double)ts.tv_sec * 1000000.0 + (double)ts.tv_nsec / 1000.0;
}

static int compare_doubles(const void *a, const void *b) {
  double da = *(const double *)a;
  double db = *(const double *)b;
  return (da > db) - (da < db);
}

static void print_usage(const char *prog) {
  printf("Usage: %s [OPTIONS]\n", prog);
  printf("Options:\n");
  printf("  -i, --ip <ip>          Target IP address (default: 10.3.10.50)\n");
  printf("  -p, --port <port>      Target UDP port (default: 12345)\n");
  printf(
      "  -c, --count <num>      Number of benchmark packets (default: 50)\n");
  printf("  -w, --warmup <num>     Number of warmup packets (default: 5)\n");
  printf("  -s, --size <bytes>     Payload size in bytes (default: 64)\n");
  printf(
      "  -t, --timeout <sec>    Receive timeout in seconds (default: 1.0)\n");
  printf("  -h, --help             Print this help message\n");
}

int main(int argc, char **argv) {
  const char *target_ip = "10.3.10.50";
  uint16_t target_port = 12345;
  int count = 50;
  int warmup = 5;
  size_t payload_size = 64;
  double timeout_sec = 1.0;

  static struct option long_options[] = {{"ip", required_argument, 0, 'i'},
                                         {"port", required_argument, 0, 'p'},
                                         {"count", required_argument, 0, 'c'},
                                         {"warmup", required_argument, 0, 'w'},
                                         {"size", required_argument, 0, 's'},
                                         {"timeout", required_argument, 0, 't'},
                                         {"help", no_argument, 0, 'h'},
                                         {0, 0, 0, 0}};

  int opt;
  while ((opt = getopt_long(argc, argv, "i:p:c:w:s:t:h", long_options, NULL)) !=
         -1) {
    switch (opt) {
    case 'i':
      target_ip = optarg;
      break;
    case 'p':
      target_port = (uint16_t)atoi(optarg);
      break;
    case 'c':
      count = atoi(optarg);
      break;
    case 'w':
      warmup = atoi(optarg);
      break;
    case 's':
      payload_size = (size_t)atoi(optarg);
      break;
    case 't':
      timeout_sec = atof(optarg);
      break;
    case 'h':
      print_usage(argv[0]);
      return 0;
    default:
      print_usage(argv[0]);
      return 1;
    }
  }

  if (payload_size < 16) {
    payload_size = 16;
  }

  printf("==============================================================\n");
  printf("  High-Performance C Benchmark Client for DPA UDP Mirroring   \n");
  printf("  Target:     %s:%u\n", target_ip, target_port);
  printf("  Warmup:     %d packets\n", warmup);
  printf("  Benchmark:  %d packets\n", count);
  printf("  Size:       %zu bytes\n", payload_size);
  printf("==============================================================\n\n");

  int sockfd = socket(AF_INET, SOCK_DGRAM, 0);
  if (sockfd < 0) {
    perror("socket");
    return 1;
  }

  struct timeval tv;
  tv.tv_sec = (time_t)timeout_sec;
  tv.tv_usec = (suseconds_t)((timeout_sec - (double)tv.tv_sec) * 1000000.0);
  if (setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) < 0) {
    perror("setsockopt SO_RCVTIMEO");
  }

  struct sockaddr_in dest_addr;
  memset(&dest_addr, 0, sizeof(dest_addr));
  dest_addr.sin_family = AF_INET;
  dest_addr.sin_port = htons(target_port);
  if (inet_pton(AF_INET, target_ip, &dest_addr.sin_addr) <= 0) {
    fprintf(stderr, "Invalid target IP address: %s\n", target_ip);
    close(sockfd);
    return 1;
  }

  uint8_t *send_buf = malloc(payload_size);
  uint8_t *recv_buf = malloc(payload_size + 128);
  if (!send_buf || !recv_buf) {
    fprintf(stderr, "Out of memory\n");
    return 1;
  }

  // Warmup phase (let hardware queues and VMA initialize)
  if (warmup > 0) {
    printf("[Warmup] Sending %d warmup packets...\n", warmup);
    for (int i = 0; i < warmup; ++i) {
      memset(send_buf, 0xAA + (i & 0x0F), payload_size);
      snprintf((char *)send_buf, payload_size, "WARMUP-%04d", i);

      sendto(sockfd, send_buf, payload_size, 0, (struct sockaddr *)&dest_addr,
             sizeof(dest_addr));

      struct sockaddr_in from_addr;
      socklen_t from_len = sizeof(from_addr);
      recvfrom(sockfd, recv_buf, payload_size + 128, 0,
               (struct sockaddr *)&from_addr, &from_len);
      usleep(2000);
    }
    printf("[Warmup] Complete. Starting benchmark...\n\n");
  }

  double *rtt_records = malloc(count * sizeof(double));
  int success_count = 0;

  for (int i = 0; i < count; ++i) {
    // Construct unique payload
    memset(send_buf, 0x55, payload_size);
    snprintf((char *)send_buf, payload_size, "DPA-BENCH-%06d-LEN%04zu", i,
             payload_size);

    double t_start = get_time_us();

    ssize_t sent = sendto(sockfd, send_buf, payload_size, 0,
                          (struct sockaddr *)&dest_addr, sizeof(dest_addr));
    if (sent < 0) {
      perror("sendto");
      continue;
    }

    struct sockaddr_in from_addr;
    socklen_t from_len = sizeof(from_addr);
    ssize_t recvd = recvfrom(sockfd, recv_buf, payload_size + 128, 0,
                             (struct sockaddr *)&from_addr, &from_len);

    double t_end = get_time_us();
    double rtt = t_end - t_start;

    if (recvd < 0) {
      printf("[%3d/%3d] Timeout after %.2f sec\n", i + 1, count, timeout_sec);
    } else if (recvd != (ssize_t)payload_size ||
               memcmp(send_buf, recv_buf, payload_size) != 0) {
      printf("[%3d/%3d] Mismatch! Sent %zd B, Recv %zd B\n", i + 1, count, sent,
             recvd);
    } else {
      rtt_records[success_count++] = rtt;
      if (i < 10 || i % 10 == 0 || i == count - 1) {
        printf("[%3d/%3d] %zd bytes echoed in %6.2f µs (Payload Match: OK)\n",
               i + 1, count, recvd, rtt);
      }
    }

    // Small inter-packet pacing (500 µs)
    usleep(500);
  }

  close(sockfd);
  free(send_buf);
  free(recv_buf);

  printf("\n==================== Results ====================\n");
  printf("Sent:     %d\n", count);
  printf("Received: %d (Loss: %.1f%%)\n", success_count,
         (double)(count - success_count) * 100.0 / (double)count);

  if (success_count > 0) {
    qsort(rtt_records, success_count, sizeof(double), compare_doubles);

    double sum = 0;
    for (int i = 0; i < success_count; ++i) {
      sum += rtt_records[i];
    }
    double avg = sum / success_count;
    double min = rtt_records[0];
    double max = rtt_records[success_count - 1];
    double p50 = rtt_records[success_count / 2];
    double p99 = rtt_records[(int)(success_count * 0.99)];

    printf("\nRTT Latency Statistics:\n");
    printf("  Min:     %6.2f µs\n", min);
    printf("  Median:  %6.2f µs\n", p50);
    printf("  Average: %6.2f µs\n", avg);
    printf("  p99:     %6.2f µs\n", p99);
    printf("  Max:     %6.2f µs\n", max);
    printf("=================================================\n");
  }

  free(rtt_records);
  return (success_count == count) ? 0 : 1;
}
