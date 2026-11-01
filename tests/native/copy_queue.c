// Exercise the production queue with wraparound, dependent copies, CPU waits,
// scratch reuse and captured uniform plans whose metadata changes immediately.
#define VK_COPY_CAPACITY 17
#include "copy_queue.h"
#include "uniform_overlay.h"
#include <stdio.h>
#include <stdlib.h>

static vk_copy_queue queue = VK_COPY_QUEUE_INIT;
static uint32_t rng = 0x139b7f23;
static uint32_t next(void) { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return rng; }
static void enqueue(void *context, uint8_t *dst, const uint8_t *src, size_t size) {
    vk_copyq_push(context, dst, src, size);
}
int main(void) {
    uint8_t source[16][1024], got[8][1024], want[8][1024];
    for (int i = 0; i < 16; ++i) for (int j = 0; j < 1024; ++j) source[i][j] = (uint8_t)next();
    memset(got, 0, sizeof got); memset(want, 0, sizeof want);
    if (!vk_copyq_start(&queue, NULL)) return 1;
    for (int i = 0; i < 300000; ++i) {
        unsigned d = next() % 8, s = next() % 16, off = next() % 769, len = next() % 257;
        int dependency = s < 8 && s != d;
        const uint8_t *a = dependency ? got[s] : source[s];
        const uint8_t *b = dependency ? want[s] : source[s];
        vk_copyq_push(&queue, got[d] + off, a + off, len);
        memcpy(want[d] + off, b + off, len);
        if (!(i % 257)) {
            vk_copyq_wait(&queue, vk_copyq_head(&queue));
            if (memcmp(got, want, sizeof got)) { fprintf(stderr, "dependent copy mismatch %d\n", i); return 2; }
        }
    }
    vk_copyq_wait(&queue, vk_copyq_head(&queue));
    if (memcmp(got, want, sizeof got)) return 3;
    // One publication may wrap the ring and may exceed its entire capacity.
    vk_copy_op batch_ops[93];
    for (int i = 0; i < 93; ++i) {
        int d = i % 8, s = i % 16;
        batch_ops[i] = (vk_copy_op){got[d], source[s], 1024};
        memcpy(want[d], source[s], 1024);
    }
    vk_copyq_push_many(&queue, batch_ops, 93);
    vk_copyq_wait(&queue, vk_copyq_head(&queue));
    if (memcmp(got, want, sizeof got)) return 7;
    for (int batch = 0; batch < 200; ++batch) {
        uint8_t out[64][1056], expected[64][1056];
        for (int k = 0; k < 64; ++k) {
            vk_uniform_span in[12], pend[12]; uint64_t origin = 0x100000000ull;
            for (int j = 0; j < 24; ++j) {
                uint64_t lo = next() % 1024, hi = lo + next() % (1025 - lo);
                if (j % 7 == 0) { lo = 0; hi = 1024; }
                vk_uniform_span span = {origin + lo, origin + hi, next() % 16, source[j % 16]};
                if (j < 12) in[j] = span; else pend[j - 12] = span;
            }
            vk_uniform_list inflight = {in, sizeof *in, 12, 1, next() % 16};
            vk_uniform_list pending = {pend, sizeof *pend, 12, 0, 0};
            uint64_t lo = next() % 1024, n = next() % (1025 - lo);
            memset(out[k], 0xa5, sizeof out[k]); memset(expected[k], 0xa5, sizeof expected[k]);
            vk_uniform_read(expected[k] + 16, source[0] + lo, origin + lo, n, inflight, pending, 1);
            vk_uniform_emit(out[k] + 16, source[0] + lo, origin + lo, n, inflight, pending, 1, enqueue, &queue);
            // The queue must own its copy operations, not pointers to these lists.
            memset(in, 0, sizeof in); memset(pend, 0, sizeof pend);
        }
        vk_copyq_wait(&queue, vk_copyq_head(&queue));
        if (memcmp(out, expected, sizeof out)) { fprintf(stderr, "uniform plan mismatch %d\n", batch); return 4; }
    }
    vk_copyq_close(&queue);
    // A stopped queue can start again; sequence numbers and ordered data survive.
    if (!vk_copyq_start(&queue, NULL)) return 5;
    vk_copyq_push(&queue, got[0], source[0], sizeof got[0]);
    vk_copyq_wait(&queue, vk_copyq_head(&queue));
    if (memcmp(got[0], source[0], sizeof got[0])) return 6;
    vk_copyq_close(&queue);
    puts("300,000 dependent copies and 12,800 captured uniform plans match synchronous execution; queue wrap, waits, guards and restart passed");
    return 0;
}
