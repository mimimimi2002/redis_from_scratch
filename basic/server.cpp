#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/ip.h>

static void msg(const char* msg) {
  fprintf(stderr, "%s\n", msg);
}

static void do_something(int connfd) {
  // initialize with '\0'
  char rbuf[64] = {};

  // give space for the last '\0'
  ssize_t n = read(connfd, rbuf, sizeof(rbuf) - 1);
  if (n < 0) {
    msg("read() error");
    return;
  }

  fprintf(stderr, "client says: %s\n", rbuf);

  char wbuf[] = "world";
  write(connfd, wbuf, strlen(wbuf));
}

static void die(const char* msg) {
  int err = errno;
  fprintf(stderr, "[%d] %s", err, msg);
  abort();
}

int main() {
  // IPv4, stream, protocol
  int fd = socket(AF_INET, SOCK_STREAM, 0);

  if (fd < 0) {
    die("socket()");
  }

  // fix the same addr to the same socket
  // prevent to block the addr soon after the bind()
  // setsockpot(fd, socket_setting, re_user_addr, valid, byte of valid)
  int val = 1;
  setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &val, sizeof(val));

  // bind, 0.0.0.0: accept all the ip address this pc has
  struct sockaddr_in addr = {};
  addr.sin_family = AF_INET; // IPv4
  addr.sin_port = htons(1234); // port 1234, network to host short, 16 bit, little endian to big endian
  addr.sin_addr.s_addr = htonl(0); // IP address, network to host long, 32 bit, little endian to big endian
  int rv = bind(fd, (const struct sockaddr *)&addr, sizeof(addr)); // bind take sockaddr_in and sockaddr_inv6 both, cast to sockaddr

  if (rv < 0) {
    die("bind()");
  }

  // listen
  // SOMAXCONN: max queue num of connections waiting to accept
  rv = listen(fd, SOMAXCONN);

  if (rv < 0) {
    die("listen()");
  }

  fprintf(stderr, "listen start at %s:%s\n", "0:0:0:0", "1234");

  while (true) {
    // accept
    struct sockaddr_in client_addr = {};
    // len of socket addr
    socklen_t addrlen = sizeof(client_addr);
    int connfd = accept(fd, (struct sockaddr *)&client_addr, &addrlen);

    if (connfd < 0) {
      continue;
    }

    do_something(connfd);
    close(connfd);
  }

  return 0;
}