#pragma once

#include <stddef.h>
#include <pthread.h>
#include <vector>
#include <deque>


struct Work {
    void (*f)(void *) = NULL;
    void *arg = NULL;
};

struct ThreadPool {
  // workers
  std::vector<pthread_t> threads;

  // task queues
  std::deque<Work> queue;

  // mutual exclusive
  pthread_mutex_t mu;

  // condition variables
  pthread_cond_t not_empty;
};

void thread_pool_init(ThreadPool *tp, size_t num_threads);
void thread_pool_queue(ThreadPool *tp, void (*f)(void *), void *arg);
