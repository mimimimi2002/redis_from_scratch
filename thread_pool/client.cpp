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
#include <string>
#include <vector>

static void msg(const char *msg) {
    fprintf(stderr, "%s\n", msg);
}

static void die(const char *msg) {
    int err = errno;
    fprintf(stderr, "[%d] %s\n", err, msg);
    abort();
}

static size_t k_max_msg = 4096;

// when reading, the position of buf also moves
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

static int32_t read_res(int fd) {
    char rbuf[4 + k_max_msg];

    uint32_t len = 0;

    int32_t err = read_full(fd, (char *)&len, 4);
    if (err) {
        msg("read() error");
        return err;
    }

    if (len > k_max_msg) {
        msg("too long");
        return -1;
    }

    err = read_full(fd, rbuf, len);
    if (err) {
        msg("read() error");
        return err;
    }

    uint32_t rescode = 0;
    if (len < 4) {
        msg("bad response");
        return -1;
    }

    memcpy(&rescode, &rbuf[0], 4);

    printf(
        "server says: %.*s\n",
        (int)(len - 4),
        &rbuf[4]
    );

    return 0;
}

static int32_t send_req(int fd, const std::vector<std::string> &cmd) {
    uint32_t len = 4;

    for (const std::string &s :cmd) {
        len += 4 + s.size();
    }

    if (len > k_max_msg) {
        return -1;
    }

    char wbuf[4 + k_max_msg];

    // copy length of n_str, len1, str1, len2, str2 to the first buffer
    memcpy(wbuf, &len, 4);
    uint32_t n = cmd.size();
    memcpy(&wbuf[4], &n, 4);

    size_t cur = 8;

    for (const std::string &s : cmd) {
        uint32_t p = (uint32_t)s.size();
        memcpy(&wbuf[cur], &p, 4);
        memcpy(&wbuf[cur + 4], s.data(), s.size());
        cur += 4 + s.size();
    }

    return write_all(fd, wbuf, 4 + len);
}

int main(int argc, char **argv) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);

    if (fd < 0) {
        die("socket()");
    }

    struct sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(1234);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    int rv = connect(
        fd,
        (const struct sockaddr *)&addr,
        sizeof(addr)
    );

    if (rv < 0) {
        die("connect()");
    }

    std::vector<std::string> cmd;
    for (int i = 1; i < argc; i++) {
        cmd.push_back(argv[i]);
    }

    int32_t err = send_req(fd, cmd);


    if (err) {
        goto L_DONE;
    }

    err = read_res(fd);
    if (err) {
        goto L_DONE;
    }

    L_DONE:
        close(fd);
        return 0;
}