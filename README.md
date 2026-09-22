# Redis from scratch
This is the project of redis implementation from scratch.

This project is based on https://build-your-own.org/redis/.

Each folder is one stage of the same server: from a blocking TCP echo, through `poll()` and Redis-like commands, to TTL timers and a thread pool for expensive cleanup.

# Redis-like Server

A simple Redis-inspired TCP server implemented in C++ to understand how event-driven servers handle multiple client connections using **non-blocking I/O** and `poll()`.

The networking core (see `event_loop/`) is an **echo server** using a length-prefixed protocol. Each client connection maintains its own read/write buffers and connection state. Later folders replace the echo behavior with `SET` / `GET` / `DEL`, idle timeouts, key TTL, sorted sets, and a thread pool.

---

## Folder Structure

```text
redis_from_scratch/
├── basic/                      blocking TCP: one client at a time
├── protocol_parsing/           length-prefixed request / response protocol
├── event_loop/                 non-blocking I/O + poll() event loop (echo)
├── command/                    SET / GET / DEL on an in-memory map
├── fixed_connection_timeout/   idle connection timeout (intrusive linked list)
├── arbitrary_ttl_timeout/      per-key TTL with a min-heap of timers
├── thread_pool/                ZADD / ZRANGE + async delete of large zsets
│   ├── server.cpp
│   ├── client.cpp
│   ├── thread_pool.h
│   ├── thread_pool.cpp
│   ├── list.h
│   └── zset.h
└── README.md
```

| Folder                       | What it adds                                                |
| ---------------------------- | ----------------------------------------------------------- |
| `basic/`                     | `socket` / `bind` / `listen` / `accept`, blocking `read`    |
| `protocol_parsing/`          | 4-byte length prefix so TCP streams become messages         |
| `event_loop/`                | `O_NONBLOCK` + `poll()`, one thread serving many clients    |
| `command/`                   | parse argv-style commands, store strings in a hash map      |
| `fixed_connection_timeout/`  | close idle connections; doubly linked list of `Conn`        |
| `arbitrary_ttl_timeout/`     | `pexpire`; min-heap of key timers, lazy deletion            |
| `thread_pool/`               | `zadd` / `zrange`; worker threads free large zsets off-loop |

---

## Setup

macOS or Linux, with `g++` or `clang++` (C++17). The latest stage also needs POSIX threads.

The server listens on `0.0.0.0:1234`. Stop any previous `./server` on that port before starting a new one.

### Latest stage (`thread_pool/`)

```bash
cd thread_pool

g++ -std=c++17 -o server server.cpp thread_pool.cpp -pthread
g++ -std=c++17 -o client client.cpp
```

Terminal 1:

```bash
./server
```

Terminal 2:

```bash
./client set name Miki
./client get name
./client del name
./client zadd scores 1.0 alice
./client zrange scores
./client pexpire name 5000
```

`thread_pool.cpp` must be linked with the server, and `-pthread` is required for `pthread_create` / mutex / condvar.

### Earlier stages

Compile the `server.cpp` and `client.cpp` in that folder. No extra source file, and no `-pthread`.

```bash
cd event_loop   # or basic, protocol_parsing, command, ...

g++ -std=c++17 -o server server.cpp
g++ -std=c++17 -o client client.cpp

./server
```

`fixed_connection_timeout/` and `arbitrary_ttl_timeout/` only include `list.h`; they still compile as a single `.cpp`.

`basic/` and `protocol_parsing/` use a hardcoded client message. From `command/` onward, the client forwards argv, for example `./client set k v`.

---

## Architecture

The server uses a **single-threaded event loop** with non-blocking sockets.

```text
                         +------------------+
                         | Listening Socket |
                         +--------+---------+
                                  |
                               accept()
                                  |
                                  v
                         +------------------+
                         |       Conn       |
                         |------------------|
                         | fd               |
                         | state            |
                         | rbuf             |
                         | wbuf             |
                         +------------------+
                                  |
                                  v
+----------------------------------------------------------+
|                        Event Loop                        |
|                                                          |
|   Build pollfd list                                      |
|          |                                               |
|          v                                               |
|        poll()                                            |
|          |                                               |
|          +---- POLLIN  ----> Read request                |
|          |                                               |
|          +---- POLLOUT ----> Write response              |
|          |                                               |
|          +---- Listen fd --> Accept new connection       |
+----------------------------------------------------------+
```

Unlike a blocking server, the server does not wait indefinitely for one client.

If a socket cannot currently be read from or written to, the operation returns with `EAGAIN`, allowing the event loop to continue processing other clients.

---

## Main Components

| Component            | Role                                               |
| -------------------- | -------------------------------------------------- |
| `fd_set_nb()`        | Sets a socket to non-blocking mode using `fcntl()` |
| `Conn`               | Stores the state and buffers of one client         |
| `fd2conn`            | Maps a file descriptor to its corresponding `Conn` |
| `poll_args`          | Stores file descriptors monitored by `poll()`      |
| `accept_new_conn()`  | Accepts and initializes a new client               |
| `connection_io()`    | Dispatches processing based on connection state    |
| `state_req()`        | Handles request receiving                          |
| `try_fill_buffer()`  | Reads available bytes from the socket into `rbuf`  |
| `try_one_request()`  | Parses and processes one complete request          |
| `state_res()`        | Handles response sending                           |
| `try_flush_buffer()` | Writes response bytes from `wbuf` to the socket    |

---

## Connection State

Each client has its own `Conn` structure.

```cpp
struct Conn {
    int fd = -1;
    uint32_t state = STATE_REQ;

    size_t rbuf_size = 0;
    uint8_t rbuf[4 + k_max_msg];

    size_t wbuf_size = 0;
    size_t wbuf_sent = 0;
    uint8_t wbuf[4 + k_max_msg];
};
```

There are three connection states.

| State       | Meaning                     | Monitored Event |
| ----------- | --------------------------- | --------------- |
| `STATE_REQ` | Receiving a request         | `POLLIN`        |
| `STATE_RES` | Sending a response          | `POLLOUT`       |
| `STATE_END` | Connection should be closed | —               |

Normal state transition:

```text
STATE_REQ
    |
    | read()
    | complete request
    v
STATE_RES
    |
    | write()
    | complete response
    v
STATE_REQ
```

If EOF or an error occurs:

```text
STATE_REQ / STATE_RES
          |
          v
      STATE_END
```

---

## Event Loop

The main server loop repeatedly performs the following operations:

1. Add the listening socket to the `poll()` list.
2. Add all existing client sockets.
3. Wait for socket events using `poll()`.
4. Process readable/writable clients.
5. Close connections in `STATE_END`.
6. Accept new clients.
7. Repeat.

Simplified:

```cpp
while (true) {

    build_poll_list();

    poll(...);

    for (ready client : clients) {
        connection_io(client);
    }

    if (listening_socket_is_ready) {
        accept_new_connection();
    }
}
```

Conceptually:

```text
Client A ── fd 4 ──┐
Client B ── fd 5 ──┤
Client C ── fd 6 ──┼──> poll() ──> Ready sockets
Listen   ── fd 3 ──┘
```

Only sockets that are ready for I/O are processed.

---

## Non-Blocking I/O

Both the listening socket and client sockets are configured with `O_NONBLOCK`.

```cpp
int flags = fcntl(fd, F_GETFL, 0);
flags |= O_NONBLOCK;
fcntl(fd, F_SETFL, flags);
```

With blocking I/O:

```text
read()
  |
  | No data
  v
WAIT...
```

The server could become stuck waiting for one client.

With non-blocking I/O:

```text
read()
  |
  | No data available
  v
-1 / EAGAIN
  |
  v
Return to event loop
```

This allows the server to process another client instead.

---

## Request Protocol

TCP is a **byte stream** and does not preserve application-level message boundaries.

Therefore, requests use a simple **length-prefixed protocol**.

```text
+----------------------+----------------------+
| Length               | Message              |
| uint32_t / 4 bytes   | length bytes         |
+----------------------+----------------------+
```

For example:

```text
+----------+-------------------+
|    5     | h e l l o         |
+----------+-------------------+
   4 B           5 B
```

The first four bytes tell the server how many bytes belong to the request body.

---

## Reading Data

`try_fill_buffer()` reads currently available data from the socket into `rbuf`.

```cpp
size_t cap = sizeof(conn->rbuf) - conn->rbuf_size;

rv = read(
    conn->fd,
    &conn->rbuf[conn->rbuf_size],
    cap
);
```

`rbuf_size` represents how many bytes are currently stored in the buffer.

```text
Before read():

rbuf

+----------------------+----------------------+
| Already received     | Free space           |
+----------------------+----------------------+
                       ^
                    rbuf_size
```

New data is written after the existing data.

```text
After read():

+----------------------+-------------+--------+
| Already received     | New bytes   | Free   |
+----------------------+-------------+--------+
                                     ^
                                new rbuf_size
```

If `read()` returns `EAGAIN`, no more data is currently available.

The server then returns to the event loop instead of waiting.

---

## Parsing a Request

After receiving data, `try_one_request()` checks whether `rbuf` contains a complete request.

### 1. Check the header

```cpp
if (conn->rbuf_size < 4) {
    return false;
}
```

At least four bytes are required to determine the message length.

### 2. Read the message length

```cpp
uint32_t len = 0;
memcpy(&len, &conn->rbuf[0], 4);
```

### 3. Check whether the complete body has arrived

```cpp
if (4 + len > conn->rbuf_size) {
    return false;
}
```

For example:

```text
Expected:

[len = 5][h e l l o]

Received:

[len = 5][h e l]

→ Request is incomplete
→ Wait for more data
```

Once all bytes have arrived, the request can be processed.

---

## Generating a Response

The current implementation is an **echo server**.

The request body is copied directly into the response buffer.

```cpp
memcpy(&conn->wbuf[0], &len, 4);
memcpy(&conn->wbuf[4], &conn->rbuf[4], len);

conn->wbuf_size = 4 + len;
```

For example:

```text
Client

[len=5][hello]
        |
        v
      Server
        |
        | copy request
        v
[len=5][hello]

        |
        v

Client receives "hello"
```

After creating the response:

```cpp
conn->state = STATE_RES;
```

The connection switches from receiving to sending.

---

## Writing a Response

`try_flush_buffer()` sends data from `wbuf` to the client.

```cpp
size_t remain = conn->wbuf_size - conn->wbuf_sent;

write(
    conn->fd,
    &conn->wbuf[conn->wbuf_sent],
    remain
);
```

A single `write()` is not guaranteed to send the entire response.

For example:

```text
1000 byte response

[--------------------]

First write sends 300 bytes:

[######--------------]
       ^
       wbuf_sent = 300
```

If the next `write()` returns `EAGAIN`, the server stops processing this client and returns to the event loop.

Later, when `poll()` reports `POLLOUT`, writing continues from the saved position.

```text
[######--------------]
       ^
       Continue writing here
```

When the complete response has been sent:

```cpp
conn->state = STATE_REQ;
conn->wbuf_sent = 0;
conn->wbuf_size = 0;
```

The connection becomes ready to receive another request.

---

## Request Pipelining

A single TCP `read()` may contain multiple requests.

For example:

```text
rbuf

+----------------+----------------+
| Request A      | Request B      |
+----------------+----------------+
```

After processing Request A, it is removed from the buffer.

```cpp
size_t remain = conn->rbuf_size - 4 - len;

memmove(
    conn->rbuf,
    &conn->rbuf[4 + len],
    remain
);
```

Before:

```text
+----------------+----------------+
| Request A      | Request B      |
+----------------+----------------+
```

After:

```text
+----------------+
| Request B      |
+----------------+
```

The server can immediately attempt to process the next request.

This is why request processing uses:

```cpp
while (try_one_request(conn)) {}
```

---

## Handling Multiple Clients

Each client has an independent `Conn`.

```text
fd2conn

index
  3       -
  4       ---> Conn A
  5       ---> Conn B
  6       ---> Conn C
```

Suppose Client A cannot currently finish sending its response.

```text
Client A

read
  |
process
  |
write
  |
EAGAIN
  |
  v
Return to event loop
```

The server can then process another client.

```text
A: read → process → write → EAGAIN
                         |
                         v

B: read → process → write

C: read → process → write

                         |
                         v
                       poll()

A becomes writable
                         |
                         v

A: continue write
```

The server does not switch clients in the middle of arbitrary code execution.

Instead, it performs as much work as possible for one client **without blocking**, saves its state, and then returns to the event loop.

Fields such as:

```text
state
rbuf_size
wbuf_size
wbuf_sent
```

allow the server to continue the operation later.

---

## Complete Request Lifecycle

The complete flow of one request is:

```text
                    poll()
                      |
                   POLLIN
                      |
                      v
              try_fill_buffer()
                      |
                    read()
                      |
                      v
                   rbuf
                      |
                      v
             try_one_request()
                      |
                 Parse request
                      |
                      v
              Generate response
                      |
                      v
                   wbuf
                      |
                 STATE_RES
                      |
                      v
                state_res()
                      |
                    write()
                      |
          +-----------+-----------+
          |                       |
       Complete                  EAGAIN
          |                       |
          v                       v
      STATE_REQ              Event loop
          |                       |
          |                  Wait POLLOUT
          |                       |
          +<----------------------+
```

At a high level, the server repeatedly performs:

```text
read
  ↓
parse / process
  ↓
write
  ↓
read
  ↓
parse / process
  ↓
write
  ↓
...
```

The event loop and non-blocking I/O allow this process to be shared efficiently across multiple clients.

---

## Thread Pool

The event loop is **single-threaded**. That is the right model for sockets: `poll()`, `read()`, and `write()` stay on one thread so connection state is never shared.

It is the wrong model for a long CPU or allocator burst. If `DEL` frees a huge sorted set on the main thread, `poll()` does not run, and every other client waits.

`thread_pool/` keeps I/O on the event loop and moves **large zset destruction** to worker threads.

### Why a pool, not a new thread per delete

Creating a `pthread` per `DEL` would still work for a toy server. A fixed pool reuses 4 workers, bounds concurrency, and avoids thread-create cost on the hot path.

### Data structures

```cpp
struct Work {
    void (*f)(void *) = NULL;
    void *arg = NULL;
};

struct ThreadPool {
    std::vector<pthread_t> threads;
    std::deque<Work> queue;
    pthread_mutex_t mu;
    pthread_cond_t not_empty;
};
```

| Piece        | Role                                              |
| ------------ | ------------------------------------------------- |
| `threads`    | Worker threads started once in `thread_pool_init` |
| `queue`      | FIFO of `{function, argument}` jobs               |
| `mu`         | Protects `queue`                                  |
| `not_empty`  | Wakes a worker when the producer pushes work      |

The pool is stored on `g_data` next to the hash map and timers. `main()` starts 4 workers before `listen()`:

```cpp
thread_pool_init(&g_data.tp, 4);
```

### Producer / consumer

The **event loop is the producer**. Workers are consumers.

```text
                    Event loop (producer)
                              |
                              | thread_pool_queue(f, arg)
                              v
                    +-------------------+
                    |  deque<Work>      |
                    |  mutex + condvar  |
                    +---------+---------+
                              |
              +---------------+---------------+
              |               |               |
              v               v               v
          Worker 0        Worker 1   ...   Worker 3
              |               |               |
              +-------+-------+-------+-------+
                      |
                      v
                 f(arg)   e.g. delete a large ZSet
```

Producer (`thread_pool_queue`):

1. Lock `mu`.
2. Push `Work {f, arg}` onto the deque.
3. `pthread_cond_signal(&not_empty)` so one sleeper wakes.
4. Unlock.

Consumer (`worker`):

```text
lock mu
    while queue is empty:
        pthread_cond_wait(&not_empty, &mu)   # unlocks, sleeps, relocks
    pop front Work
unlock mu
run w.f(w.arg)
repeat
```

`pthread_cond_wait` must sit in a `while`, not an `if`. After a wakeup, another worker may have taken the job, or the wakeup may be spurious. The loop re-checks `queue.empty()` under the mutex.

The worker **unlocks before running `f`**. Holding the mutex during `delete` would serialize every job and stall the producer (the event loop) on `thread_pool_queue`.

### When the server uses it

`entry_del` unlinks the key from `g_data.db` first, so later `GET` / `SET` do not see a half-destroyed entry. Then it decides where to free the zset:

```text
DEL key
  |
  v
entry_del
  |
  | unlink key from g_data.db
  |
  +-- zset size <= 10000 --> delete on the event loop
  |
  +-- zset size >  10000 --> thread_pool_queue(zset_del_async, zset)
                                  |
                                  v
                             worker: delete zset
```

Small containers stay on the main thread: queueing would cost more than `delete`. Large ones would stall `poll()`, so they go to the pool.

The payload is only a pointer. The map no longer owns that `ZSet`; the worker is the last owner and calls `zset_dispose`.

This is the same pattern Redis uses for large-object unlink: return to the client quickly, reclaim memory in the background.

---

## Next Steps

The later folders already cover commands, idle timeouts, key TTL, sorted sets, and async delete.

Possible follow-ups:

* Real skip-list / B-tree zset instead of `std::sort` on every `zadd`
* Cache eviction (maxmemory)
* A second background job type (not only `delete`)
* Integration with a web backend
* Concurrent performance benchmarking

