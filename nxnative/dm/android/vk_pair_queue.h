// Bounded program-key ownership. Called under the pair worker mutex; a full
// queue leaves demand creation unchanged and does not mark the key as seen.
#pragma once
#include <stdint.h>
#include <string.h>
enum { PAIR_QUEUE_N = 1024, PAIR_SEEN_N = 8192 };
typedef struct { uint64_t key[3]; } vk_pair_key;
typedef struct {
    vk_pair_key queue[PAIR_QUEUE_N], seen[PAIR_SEEN_N];
    unsigned head, tail, high;
} vk_pair_queue;
static inline int vk_pair_push(vk_pair_queue *q, const vk_pair_key *key) {
    uint64_t hash = key->key[0] ^ (key->key[1] * UINT64_C(0x9e3779b97f4a7c15)) ^ key->key[2];
    unsigned slot = (unsigned)(hash ^ (hash >> 32)) & (PAIR_SEEN_N - 1), i;
    if (!key->key[0]) return -1;
    for (i = 0; i < PAIR_SEEN_N; i++, slot = (slot + 1) & (PAIR_SEEN_N - 1)) {
        if (!q->seen[slot].key[0]) break;
        if (!memcmp(&q->seen[slot], key, sizeof *key)) return 0;
    }
    if (i == PAIR_SEEN_N || q->head - q->tail == PAIR_QUEUE_N) return -1;
    q->seen[slot] = *key; q->queue[q->head++ % PAIR_QUEUE_N] = *key;
    if (q->head - q->tail > q->high) q->high = q->head - q->tail;
    return 1;
}
static inline int vk_pair_pop(vk_pair_queue *q, vk_pair_key *key) {
    if (q->head == q->tail) return 0;
    *key = q->queue[q->tail++ % PAIR_QUEUE_N]; return 1;
}
// Stages are resolved from the bundle, never from SetShaders' array order.
static inline int vk_pair_stages(const uint64_t *keys, const int *stages, int n, int gs, vk_pair_key *pair) {
    memset(pair, 0, sizeof *pair);
    if (n < 2 || n > 3) return 0;
    for (int i = 0; i < n; i++) {
        int st = stages[i];
        if (!keys[i] || st < 0 || st > 2 || (st == 2 && !gs) || pair->key[st]) return 0;
        pair->key[st] = keys[i];
    }
    return pair->key[0] && pair->key[1];
}
