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
#include <string>
#include <vector>
#include <unordered_map>

#include "list.h"

// 内部のメンバへのポインタから、そのメンバを持っている親オブジェクトのポインタを取得する
#define container_of(ptr, type, member) ({ \
    const __typeof__(((type *)0)->member) *__mptr = (ptr); \
    (type *)((char *)__mptr - offsetof(type, member)); \
})

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
const size_t k_max_args = 200 * 1000;

// monotonic (never reverse) timer
// get the time since the OS has started
static uint64_t get_monotonic_usec() {
  timespec tv = {0, 0};
  clock_gettime(CLOCK_MONOTONIC, &tv);
  return uint64_t(tv.tv_sec) * 1000000 + tv.tv_nsec / 1000;
}

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

  // timer
  uint64_t idle_start = 0;
  DListNode idle_node;
};

static struct {
  std::unordered_map<std::string, std::string> db;

  std::vector<Conn *> fd2conn;

  // head of list node
  DListNode idle_list;
} g_data;

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

static int32_t parse_req(const uint8_t *body, uint32_t bodylen, std::vector<std::string> &out) {
  if (bodylen < 4) {
    return -1;
  }

  uint32_t n = 0;

  // nstr: how many args they have
  // SET name MIMI: nstr = 3
  memcpy(&n, &body[0], 4);
  if (n > k_max_args) {
    return -1;
  }

  size_t pos = 4;
  // check nstr + len1
  while(n--) {
    if (pos + 4 > bodylen) {
      return -1;
    }

    uint32_t sz = 0;
    memcpy(&sz, &body[pos], 4);
    // nstr, len1, str1
    if (pos + 4 + sz > bodylen) {
      return -1;
    }

    out.push_back(std::string((char *)&body[pos + 4], sz));
    pos += 4 + sz;
  }

  if (pos != bodylen) {
    return -1;
  }

  return 0;
}

enum {
  RES_OK = 0,
  RES_ERR = 1,
  RES_NX = 2,
};

static uint32_t do_get(const std::vector<std::string> &cmd, uint8_t *res, uint32_t *reslen) {
  if (!g_data.db.count(cmd[1])) {
    return RES_NX;
  }
  std::string &val = g_data.db[cmd[1]];
  assert(val.size() <= k_max_msg);
  std::string msg = "get " + val;
  memcpy(res, msg.data(), msg.size());
  *reslen = (uint32_t)msg.size();
  return RES_OK;
}

static uint32_t do_set(const std::vector<std::string> &cmd, uint8_t *res, uint32_t *reslen) {
  std::string msg = "set " + cmd[2];
  memcpy(res, msg.data(), msg.size());
  *reslen = (uint32_t)msg.size();
  g_data.db[cmd[1]] = cmd[2];
  return RES_OK;
}

static uint32_t do_del(const std::vector<std::string> &cmd, uint8_t *res, uint32_t *reslen) {
  g_data.db.erase(cmd[1]);
  return RES_OK;
}

static bool do_request(const uint8_t *req, uint32_t reqlen, uint32_t *rescode, uint8_t *res, uint32_t *reslen) {
  std::vector<std::string> cmd;
  if (0 != parse_req(req, reqlen, cmd)) {
    msg("bad req");
    return -1;
  }

  printf("cmd %zu, %s\n", cmd.size(), cmd[0].c_str());
  // cmd example: SET name MIMI
  if (cmd.size() == 2 && cmd[0] == "get") {
    *rescode = do_get(cmd, res, reslen);
  } else if (cmd.size() == 3 && cmd[0] == "set") {
    *rescode = do_set(cmd, res, reslen);
  } else if (cmd.size() == 2 && cmd[0] == "del") {
    *rescode = do_del(cmd, res, reslen);
  } else {
    // cmd is not recognized
    *rescode = RES_ERR;
    const char *msg = "Unknown cmd";
    strcpy((char *)res, msg);
    *reslen = strlen(msg);
    return 0;
  }
  return 0;
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

  // got one request, generate the response.
  uint32_t rescode = 0;
  uint32_t wlen = 0;

  // response
  // len=8 | RESCODE | response
  int32_t err = do_request(
    &conn->rbuf[4], len,
    &rescode, &conn->wbuf[4 + 4], &wlen
  );

  if (err) {
    conn->state = STATE_END;
    return false;
  }

  // rescode + response
  wlen += 4;

  memcpy(&conn->wbuf[0], &wlen, 4);
  memcpy(&conn->wbuf[4], &rescode, 4);

  // len + rescode + response
  conn->wbuf_size = 4 + wlen;

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
  // reset timer, move to the last node
  conn->idle_start = get_monotonic_usec();
  dlist_detach(&conn->idle_node);
  // 循環リストのためheadの前に入れる
  dlist_insert_before(&g_data.idle_list, &conn->idle_node);

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
  conn->idle_start = get_monotonic_usec();
  dlist_insert_before(&g_data.idle_list, &conn->idle_node);
  conn_put(fd2conn, conn);
  return 0;
}

const uint64_t k_idle_timeout_ms = 5 * 1000;

static void conn_done(Conn *conn) {
  g_data.fd2conn[conn->fd] = NULL;
  (void)close(conn->fd);
  dlist_detach(&conn->idle_node);
  free(conn);
}

// get the nearest time to expiration
static uint32_t next_timer_ms() {

  // default is 10 sec
  if (dlist_empty(&g_data.idle_list)) {
    return 10000; // no timer, the value doesn't matter
  }

  uint64_t now_us = get_monotonic_usec();

  // the head of the list is the nearest expire
  Conn *next = container_of(g_data.idle_list.next, Conn, idle_node);

  // next timeout
  uint64_t next_us = next->idle_start + k_idle_timeout_ms * 1000;
  if (next_us <= now_us) {
    // missed?
    return 0;
  }
  return (uint32_t)((next_us - now_us) / 1000);
}

// check if already expires
// disconnect
static void process_timers() {
  uint64_t now_us = get_monotonic_usec();
  while (!dlist_empty(&g_data.idle_list)) {
    Conn *next = container_of(g_data.idle_list.next, Conn, idle_node);
    uint64_t next_us = next->idle_start + k_idle_timeout_ms * 1000;
    if (next_us >= now_us + 1000) {
      // not ready, the extra 1000us is for the ms resolution of poll()
      break;
    }
    printf("removing idle connection: %d\n", next->fd);
    conn_done(next);
  }
}

int main() {
  // data initialization
  dlist_init(&g_data.idle_list);

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
    for (Conn *conn: g_data.fd2conn) {
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

    // Calculate the timeout until the nearest timer expires.
    int timeout_ms = (int)next_timer_ms();

    printf("timeout ms %d\n", timeout_ms);

    // Wait until an fd becomes ready or the nearest timer expires.
    int rv = poll(poll_args.data(), (nfds_t) poll_args.size(), timeout_ms);
    if (rv < 0) {
      die("poll");
    }

    // skip listening fd
    for (size_t i = 1; i < poll_args.size(); i++) {
      // if fd is active
      if (poll_args[i].revents) {
        Conn *conn = g_data.fd2conn[poll_args[i].fd];

        // update timer is included here
        connection_io(conn);
        if (conn -> state == STATE_END) {
          conn_done(conn);
        }
      }
    }

    // handle timer
    // expire the connection if any
    process_timers();

    // if listening fd is active, connect new fd
    // start new timer for new connection
    if (poll_args[0].revents) {
      (void) accept_new_conn(g_data.fd2conn, fd);
    }
  }

  return 0;
}