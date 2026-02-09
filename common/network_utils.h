#pragma once

/*
 * ThunderLink Cross-Platform Socket Helpers
 * Abstracts Winsock2 (Windows) vs POSIX sockets (macOS) differences.
 */

#include <stdint.h>
#include "protocol.h"

#ifdef _WIN32
  #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
  #endif
  #include <winsock2.h>
  #include <ws2tcpip.h>
  #pragma comment(lib, "ws2_32.lib")

  typedef SOCKET tlnk_socket_t;
  #define TLNK_INVALID_SOCKET INVALID_SOCKET
  #define TLNK_SOCKET_ERROR   SOCKET_ERROR

  static inline int tlnk_close_socket(tlnk_socket_t s) {
      return closesocket(s);
  }

  static inline int tlnk_get_error(void) {
      return WSAGetLastError();
  }

  static inline int tlnk_init_sockets(void) {
      WSADATA wsa;
      return WSAStartup(MAKEWORD(2, 2), &wsa);
  }

  static inline void tlnk_cleanup_sockets(void) {
      WSACleanup();
  }

#else
  #include <sys/socket.h>
  #include <sys/types.h>
  #include <netinet/in.h>
  #include <netinet/tcp.h>
  #include <arpa/inet.h>
  #include <unistd.h>
  #include <errno.h>
  #include <fcntl.h>
  #include <string.h>

  typedef int tlnk_socket_t;
  #define TLNK_INVALID_SOCKET (-1)
  #define TLNK_SOCKET_ERROR   (-1)

  static inline int tlnk_close_socket(tlnk_socket_t s) {
      return close(s);
  }

  static inline int tlnk_get_error(void) {
      return errno;
  }

  static inline int tlnk_init_sockets(void) {
      return 0; /* No-op on POSIX */
  }

  static inline void tlnk_cleanup_sockets(void) {
      /* No-op on POSIX */
  }
#endif

/* Fill a sockaddr_in for the given IP string and port */
static inline struct sockaddr_in tlnk_make_addr(const char* ip, uint16_t port) {
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    inet_pton(AF_INET, ip, &addr.sin_addr);
    return addr;
}

/* Create a UDP socket configured for high-throughput video frames */
static inline tlnk_socket_t tlnk_create_udp_socket(void) {
    tlnk_socket_t s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == TLNK_INVALID_SOCKET) return s;

    int reuse = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (const char*)&reuse, sizeof(reuse));

    int recv_buf = TLNK_RECV_BUFFER_SIZE;
    setsockopt(s, SOL_SOCKET, SO_RCVBUF, (const char*)&recv_buf, sizeof(recv_buf));

    int send_buf = TLNK_SEND_BUFFER_SIZE;
    setsockopt(s, SOL_SOCKET, SO_SNDBUF, (const char*)&send_buf, sizeof(send_buf));

    return s;
}

/* Create a TCP socket configured for low-latency control/input */
static inline tlnk_socket_t tlnk_create_tcp_socket(void) {
    tlnk_socket_t s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == TLNK_INVALID_SOCKET) return s;

    int reuse = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (const char*)&reuse, sizeof(reuse));

    /* Disable Nagle's algorithm for minimum latency */
    int nodelay = 1;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char*)&nodelay, sizeof(nodelay));

    return s;
}

/* Set socket receive buffer size */
static inline int tlnk_set_recv_buffer(tlnk_socket_t s, int size) {
    return setsockopt(s, SOL_SOCKET, SO_RCVBUF, (const char*)&size, sizeof(size));
}

/* Set socket send buffer size */
static inline int tlnk_set_send_buffer(tlnk_socket_t s, int size) {
    return setsockopt(s, SOL_SOCKET, SO_SNDBUF, (const char*)&size, sizeof(size));
}
