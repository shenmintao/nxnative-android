// A bounded, single-producer copy queue. Sources and destinations stay alive
// until their sequence completes; dependent copies execute in producer order.
#pragma once
#include <pthread.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#ifndef VK_COPY_CAPACITY
#define VK_COPY_CAPACITY 8192
#endif
typedef struct { void *dst; const void *src; size_t size; } vk_copy_op;
typedef struct {
    vk_copy_op ops[VK_COPY_CAPACITY];
    _Atomic uint64_t head, done;
    pthread_mutex_t mu;
    pthread_cond_t wake, finished;
    pthread_t worker;
    void (*begin)(void);
    int started, failed, stop;
} vk_copy_queue;
#define VK_COPY_QUEUE_INIT { .mu = PTHREAD_MUTEX_INITIALIZER, .wake = PTHREAD_COND_INITIALIZER, .finished = PTHREAD_COND_INITIALIZER }

static void *vk_copyq_worker(void *arg) {
    vk_copy_queue *q = arg;
    if (q->begin) q->begin();
    uint64_t at = atomic_load_explicit(&q->done, memory_order_relaxed);
    for (;;) {
        uint64_t head = atomic_load_explicit(&q->head, memory_order_acquire);
        if (at == head) {
            pthread_mutex_lock(&q->mu);
            while (at == (head = atomic_load_explicit(&q->head, memory_order_acquire)) && !q->stop)
                pthread_cond_wait(&q->wake, &q->mu);
            int stop = q->stop && at == head;
            pthread_mutex_unlock(&q->mu);
            if (stop) return NULL;
        }
        for (; at < head; ++at) {
            const vk_copy_op *op = &q->ops[at % VK_COPY_CAPACITY];
            memcpy(op->dst, op->src, op->size);
        }
        atomic_store_explicit(&q->done, at, memory_order_release);
        pthread_mutex_lock(&q->mu);
        pthread_cond_broadcast(&q->finished);
        pthread_mutex_unlock(&q->mu);
    }
}
static inline int vk_copyq_start(vk_copy_queue *q, void (*begin)(void)) {
    if (q->started || q->failed) return q->started;
    q->begin = begin; q->stop = 0;
    if (pthread_create(&q->worker, NULL, vk_copyq_worker, q)) { q->failed = 1; return 0; }
    q->started = 1;
    return 1;
}
static inline uint64_t vk_copyq_head(const vk_copy_queue *q) { return atomic_load_explicit(&q->head, memory_order_acquire); }
static inline void vk_copyq_wait(vk_copy_queue *q, uint64_t sequence) {
    if (atomic_load_explicit(&q->done, memory_order_acquire) >= sequence) return;
    pthread_mutex_lock(&q->mu);
    while (atomic_load_explicit(&q->done, memory_order_acquire) < sequence)
        pthread_cond_wait(&q->finished, &q->mu);
    pthread_mutex_unlock(&q->mu);
}
static inline uint64_t vk_copyq_push_many(vk_copy_queue *q, const vk_copy_op *ops, size_t count) {
    uint64_t head = atomic_load_explicit(&q->head, memory_order_relaxed);
    while (count) {
        size_t room = VK_COPY_CAPACITY - (size_t)(head - atomic_load_explicit(&q->done, memory_order_acquire));
        if (!room) { vk_copyq_wait(q, head - VK_COPY_CAPACITY + 1); continue; }
        size_t take = count < room ? count : room, at = (size_t)(head % VK_COPY_CAPACITY);
        size_t first = take < VK_COPY_CAPACITY - at ? take : VK_COPY_CAPACITY - at;
        memcpy(q->ops + at, ops, first * sizeof *ops);
        if (take > first) memcpy(q->ops, ops + first, (take - first) * sizeof *ops);
        head += take; ops += take; count -= take;
        atomic_store_explicit(&q->head, head, memory_order_release);
        // Pair every publication with the consumer's predicate mutex. Skipping
        // this lock based on a sleeping flag can lose a wakeup on either CPU.
        pthread_mutex_lock(&q->mu); pthread_cond_signal(&q->wake); pthread_mutex_unlock(&q->mu);
    }
    return head;
}
static inline uint64_t vk_copyq_push(vk_copy_queue *q, void *dst, const void *src, size_t size) {
    vk_copy_op op = {dst, src, size};
    return vk_copyq_push_many(q, &op, 1);
}
static inline void vk_copyq_close(vk_copy_queue *q) {
    if (!q->started) return;
    vk_copyq_wait(q, vk_copyq_head(q));
    pthread_mutex_lock(&q->mu); q->stop = 1; pthread_cond_signal(&q->wake); pthread_mutex_unlock(&q->mu);
    pthread_join(q->worker, NULL); q->started = 0;
}
