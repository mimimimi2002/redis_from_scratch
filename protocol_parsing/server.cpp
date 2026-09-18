#include <assert.h>
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

static void die(const char* msg) {
  int err = errno;
  fprintf(stderr, "[%d] %s", err, msg);
  abort();
}

// 4096 bytes message
static size_t k_max_msg = 4096;

static int32_t read_full(int fd, char* buf, size_t n) {
  while (n > 0) {

    // this waits until the next query come
    ssize_t rv = read(fd, buf, n);

    // rv == 0: EOF (peer closed the connection)
    // rv < 0:  read error
    if (rv <= 0) {
      return -1; // error or unexpected EOF
    }

    assert((size_t)rv<=n);
    n -= (size_t)rv;
    buf += rv;
  }

  return 0;
}

static int32_t write_all(int fd, char* buf, size_t n) {
  while (n > 0) {
    ssize_t rv = write(fd, buf, n);

    if (rv <= 0) {
      return -1;
    }

    assert((size_t) rv <= n);

    n -= (size_t)rv;
    buf += rv;
  }

  return 0;
}

static int32_t one_request(int connfd) {
  // 4 bytes header + max 4096 bytes message
  char rbuf[4 + k_max_msg];
  errno = 0;

  // read the message len by 4 bytes
  int32_t err = read_full(connfd, rbuf, 4);

  if (err) {
    msg(errno == 0 ? "EOF": "read() error");
    return err;
  }

  // copy the 4bytes header of rbuf to len
  uint32_t len = 0;
  memcpy(&len, rbuf, 4);
  if (len > k_max_msg) {
    msg("too long");
    return -1;
  }

  // request body
  // read the msg with len bytes
  err = read_full(connfd, &rbuf[4], len);
  if (err) {
    msg("read() error");
    return err;
  }

  // do something
  fprintf(stderr, "client says: %.*s\n", len, &rbuf[4]);

  // reply using the sample protocol
  const char reply[] = "world";
  char wbuf[4 + sizeof(reply)];
  len = (uint32_t)strlen(reply);
  memcpy(wbuf, &len, 4);
  memcpy(&wbuf[4], reply, len);
  return write_all(connfd, wbuf, 4 + len);
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

    // Process multiple requests from the same connection until it is closed.
    while(true) {
      int32_t err = one_request(connfd);
      if (err) {
        break;
      }
    }
    close(connfd);
  }

  return 0;
}