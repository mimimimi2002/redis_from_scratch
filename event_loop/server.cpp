// stdlib
#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
// system
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/ip.h>
// C++
#include <vector>


static void msg(const char* msg) {
  fprintf(stderr, "%s\n", msg);
}

static void die(const char* msg) {
  int err = errno;
  fprintf(stderr, "[%d] %s", err, msg);
  abort();
}

// set fd setting to non blocking mode
static void fd_set_nb(int fd) {
  errno = 0;

  // fd control
  // get current fd flag
  int flags = fcntl(fd, F_GETFL, 0);
  if (errno) {
    die("get fcntl error");
    return;
  }

  // only change the flag bit
  flags |= O_NONBLOCK;

  errno = 0;

  // set current fd flag
  (void)fcntl(fd, F_SETFL, flags);
  if (errno) {
    die("set fcntl error");
  }
}

// 4096 bytes message
static constexpr size_t k_max_msg = 4096;

enum {
  STATE_REQ = 0,
  STATE_RES = 1,
  STATE_END = 2,
};

// state represents whether server receive or send to the client
// receive -> read, send -> write
struct Conn {
  int fd = -1;
  uint32_t state = 0;
  size_t rbuf_size = 0;
  uint8_t rbuf[4 + k_max_msg];

  size_t wbuf_size = 0;
  size_t wbuf_sent = 0;
  uint8_t wbuf[4 + k_max_msg];
};

static bool try_flush_buffer(Conn *conn) {
    size_t remain = conn->wbuf_size - conn->wbuf_sent;

    ssize_t rv = write(
        conn->fd,
        &conn->wbuf[conn->wbuf_sent],
        remain
    );

    if (rv < 0 && errno == EAGAIN) {
        // Cannot write more right now.
        return false;
    }

    if (rv < 0) {
        msg("write() error");
        conn->state = STATE_END;
        return false;
    }

    conn->wbuf_sent += (size_t)rv;
    assert(conn->wbuf_sent <= conn->wbuf_size);

    if (conn->wbuf_sent == conn->wbuf_size) {
        // Entire response has been sent.
        conn->state = STATE_REQ;
        conn->wbuf_sent = 0;
        conn->wbuf_size = 0;
        return false;
    }

    // Some data remains, so try write() again.
    return true;
}

static void state_res(Conn *conn) {
    while (try_flush_buffer(conn)) {}
}

static bool try_one_request(Conn *conn) {
  // try to parse a request from the buffer
  if (conn->rbuf_size < 4) {
    // not enough data in the buffer. Will retry in the next iteration
    return false;
  }
  uint32_t len = 0;
  memcpy(&len, &conn->rbuf[0], 4);
  if (len > k_max_msg) {
    msg("too long");
    conn->state = STATE_END;
    return false;
  }
  if (4 + len > conn->rbuf_size) {
    // not enough data in the buffer. Will retry in the next iteration
    return false;
  }
  // guarantee that the request is received
  printf(
      "fd=%d client says: %.*s\n",
      conn->fd,
      (int)len,
      &conn->rbuf[4]
  );

  // echo server
  memcpy(&conn->wbuf[0], &len, 4);
  memcpy(&conn->wbuf[4], &conn->rbuf[4], len);
  conn->wbuf_size = 4 + len;

  // remove the request from the buffer.
  // note: frequent memmove is inefficient.
  // note: need better handling for production code.
  size_t remain = conn->rbuf_size - 4 - len;
  if (remain) {
    memmove(conn->rbuf, &conn->rbuf[4 + len], remain);
  }
  conn->rbuf_size = remain;
  // change state
  conn->state = STATE_RES;
  state_res(conn);
  // continue the outer loop if the request was fully processed
  return (conn->state == STATE_REQ);
}

static bool try_fill_buffer(Conn *conn) {
  assert(conn->rbuf_size < sizeof(conn->rbuf));
  ssize_t rv = 0;
  do {
    // remained cap
    size_t cap = sizeof(conn->rbuf) - conn->rbuf_size;
    // read maximum cap from socket to rbuf
    rv = read(conn->fd, &conn->rbuf[conn->rbuf_size], cap);
  } while (rv < 0 && errno == EINTR); // when the read was interrupt, try again

  // no more data to read
  if (rv < 0 && errno == EAGAIN) {
  // got EAGAIN, stop.
    return false;
  }
  if (rv < 0) {
    msg("read() error");
    conn->state = STATE_END;
    return false;
  }
  if (rv == 0) {
    if (conn->rbuf_size > 0) {
      msg("unexpected EOF");
    } else {
      msg("EOF");
    }
    conn->state = STATE_END;
    return false;
  }
  conn->rbuf_size += (size_t)rv;
  assert(conn->rbuf_size <= sizeof(conn->rbuf) - conn->rbuf_size);
  // Try to process requests one by one.
  // Why is there a loop? Please read the explanation of "pipelining".

  // parse the current rbuf to see if buff is completed as length + msg
  // while is used to parse multiple request
  while (try_one_request(conn)) {}
  return (conn->state == STATE_REQ);
}


static void state_req(Conn *conn) {
  while(try_fill_buffer(conn)) {}
}

static void connection_io(Conn *conn) {
  if (conn->state == STATE_REQ) {
    state_req(conn);
  } else if (conn->state == STATE_RES) {
    state_res(conn);
  } else {
    assert(0);
  }
}

// add new conn to cd2conn vector
static void conn_put(std::vector<Conn *> &fd2conn, struct Conn *conn) {
  if (fd2conn.size() <= (size_t)conn->fd) {
    fd2conn.resize(conn->fd + 1);
  }
  fd2conn[conn->fd] = conn;
}

static int32_t accept_new_conn(std::vector<Conn *> &fd2conn, int fd) {
  // accept
  struct sockaddr_in client_addr = {};
  socklen_t socklen = sizeof(client_addr);
  int connfd = accept(fd, (struct sockaddr *)&client_addr, &socklen);
  if (connfd < 0) {
    msg("accept() error");
    return -1; // error
  }
  // set the new connection fd to nonblocking mode
  fd_set_nb(connfd);
  // creating the struct Conn
  struct Conn *conn = (struct Conn *)malloc(sizeof(struct Conn));
  if (!conn) {
    close(connfd);
    return -1;
  }
  conn->fd = connfd;
  conn->state = STATE_REQ;
  conn->rbuf_size = 0;
  conn->wbuf_size = 0;
  conn->wbuf_sent = 0;
  conn_put(fd2conn, conn);
  return 0;
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

  // set the listen fd to nonblocking mode
  fd_set_nb(fd);

  // listen
  // SOMAXCONN: max queue num of connections waiting to accept
  rv = listen(fd, SOMAXCONN);

  if (rv < 0) {
    die("listen()");
  }

  // create a map of all client connections, keyed by fd
  std::vector<Conn *> fd2conn;
  // the event loop
  // the vector of pollfd structs
  // all fds
  std::vector<struct pollfd> poll_args;

  fprintf(stderr, "listen start at %s:%s\n", "0:0:0:0", "1234");

  // 1. add listening fd to poll args
  // 2. add current connection's fds to poll args
  // 3. poll to get active fds : can be blocked in while loop
  // 4. for each active fd, read / write
  // 5. if listening fd is active, add new connection to fd2conn
  while (true) {
    poll_args.clear();

    // poll fd: fd, event to supervise, revents where the poll result is written
    // POLLIN: when the fd is readable
    struct pollfd pfd = {fd, POLLIN, 0};
    // add listening fd as first in poll_args
    poll_args.push_back(pfd);

    // create a vector of fds for the existing connections
    for (Conn *conn: fd2conn) {
      if (!conn) {
        continue;
      }
      struct pollfd pfd = {};
      pfd.fd = conn->fd;
      pfd.events = (conn->state == STATE_REQ) ? POLLIN : POLLOUT;
      // add POLLERR state to events
      pfd.events = pfd.events | POLLERR;
      poll_args.push_back(pfd);
    }

    // wait maximum 1 sec to get the ready fd
    // return the num of active fd and rewrite revents for active fds
    // blocking operation
    int rv = poll(poll_args.data(), (nfds_t) poll_args.size(), 1000);
    if (rv < 0) {
      die("poll");
    }

    // skip listening fd
    for (size_t i = 1; i < poll_args.size(); i++) {
      // if fd is active
      if (poll_args[i].revents) {
        Conn *conn = fd2conn[poll_args[i].fd];
        connection_io(conn);
        if (conn -> state == STATE_END) {
          fd2conn[conn->fd] = NULL;
          (void)close(conn->fd);
          free(conn);
        }
      }
    }

    // if listening fd is active, connect new fd
    if (poll_args[0].revents) {
      (void) accept_new_conn(fd2conn, fd);
    }
  }

  return 0;
}