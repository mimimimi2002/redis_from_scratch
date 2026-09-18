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

static void msg(const char *msg) {
    fprintf(stderr, "%s\n", msg);
}

static void die(const char *msg) {
    int err = errno;
    fprintf(stderr, "[%d] %s\n", err, msg);
    abort();
}

static size_t k_max_msg = 4096;

static int32_t read_full(int fd, char* buf, size_t n){
  while (n > 0) {
    ssize_t rv = read(fd, buf, n);

    if (rv <= 0) {
      return -1;
    }

    assert((size_t)rv <= n);
    n -= (size_t)rv;
    buf += rv;
  }

  return 0;
}

static int32_t write_all(int fd, char *buf, size_t n) {
  printf("%.*s is about to be written\n", (int)n, buf);
  while (n > 0) {
    ssize_t rv = write(fd, buf, n);
    if (rv <= 0) {
        return -1;  // error
    }
    assert((size_t)rv <= n);
    n -= (size_t)rv;
    buf += rv;
  }

  return 0;
}

static int32_t query(int fd, const char *text) {
  uint32_t len = strlen(text);
  if (len > k_max_msg) {
    return -1;
  }

  char wbuf[4 + k_max_msg];
  memcpy(wbuf, &len, 4);
  memcpy(&wbuf[4], text, len);
  if (int32_t err = write_all(fd, wbuf, 4 + len)) {
    return err;
  }

  char rbuf[4 + k_max_msg];
  errno = 0;
  int32_t err = read_full(fd, rbuf, 4);

  // when return value is not 0
  if (err) {
      msg(errno == 0 ? "EOF" : "read() error");
      return err;
  }

  memcpy(&len, rbuf, 4);
  if (len > k_max_msg) {
    msg("too long");
    return -1;
  }

  // reply body
  err = read_full(fd, &rbuf[4], len);
  if (err) {
    msg("read() error");
    return err;
  }

  printf("server says %.*s\n", len, &rbuf[4]);

  return 0;
}

int main() {
  int fd = socket(AF_INET, SOCK_STREAM, 0);

  if (fd < 0) {
    die("socket()");
  }

  struct sockaddr_in addr = {};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(1234);
  // localhost
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

  int rv = connect(fd, (const struct sockaddr*) &addr, sizeof(addr));

  if (rv < 0) {
    die("connect()");
  }

  // multiple request
  int32_t err = query(fd, "hello1");
  if (err) {
    msg("query1");
    close(fd);
    return 0;
  }

  err = query(fd, "hello2");
  if (err) {
    msg("query2");
    close(fd);
    return 0;
  }

  err = query(fd, "hello3");
  if (err) {
    msg("query3");
    close(fd);
    return 0;
  }
}
