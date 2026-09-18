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

    printf("server says: %.*s\n", (int)len, rbuf);

    return 0;
}

static int32_t send_req(int fd, const char *text) {
    uint32_t len = strlen(text);

    if (len > k_max_msg) {
        return -1;
    }

    char wbuf[4 + k_max_msg];

    memcpy(wbuf, &len, 4);
    memcpy(&wbuf[4], text, len);

    return write_all(fd, wbuf, 4 + len);
}

static int32_t send_req_slow(int fd, const char *text) {
    uint32_t len = strlen(text);

    if (len > k_max_msg) {
        return -1;
    }

    char wbuf[4 + k_max_msg];
    memcpy(wbuf, &len, 4);
    memcpy(&wbuf[4], text, len);

    // [length][hel] まで送る
    size_t first_part = 4 + 3;

    ssize_t rv = write(fd, wbuf, first_part);
    if (rv < 0) {
        return -1;
    }

    printf(
        "pid=%d sent first %zd bytes, sleeping 5 sec...\n",
        getpid(),
        rv
    );

    sleep(5);

    // 残りの "lo1" を送る
    size_t remain = 4 + len - first_part;

    printf("pid=%d sending remaining bytes\n", getpid());

    return write_all(
        fd,
        &wbuf[first_part],
        remain
    );
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

    if (argc > 1 && strcmp(argv[1], "slow") == 0) {
        // Slow client
        send_req_slow(fd, "hello1");
        read_res(fd);
    } else {
        // Normal client
        const char *query_list[3] = {
            "hello1",
            "hello2",
            "hello3"
        };

        for (size_t i = 0; i < 3; ++i) {
            send_req(fd, query_list[i]);
        }

        for (size_t i = 0; i < 3; ++i) {
            read_res(fd);
        }
    }

    close(fd);
    return 0;
}