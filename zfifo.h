/* zfifo - lock-free SPSC ring buffer with arbitrary capacity; zfifo_2n is the power-of-2 variant. */
/* Counters wrap in [0, 2*cap) so all cap slots are usable without a mask. */
#ifndef ZFIFO_H
#define ZFIFO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t in;     /* written only by producer, range [0, 2*cap) */
    uint32_t out;    /* written only by consumer, range [0, 2*cap) */
    uint32_t cap;
    uint32_t esize;  /* element size in bytes */
    uint8_t *buf;
} zfifo_t;

#define ZFIFO_INITIALIZER(_buf, _esize, _cap) \
    { 0u, 0u, (uint32_t)(_cap), (uint32_t)(_esize), (uint8_t *)(_buf) }

/* Statically define a fifo of `cap` elements of `type`; 1 <= cap < 2^31. */
#define ZFIFO_DEFINE(name, type, cap)                                      \
    typedef char name##_cap_invalid[((cap) >= 1 && (cap) < 0x80000000u) ? 1 : -1]; \
    static type name##_buf[cap];                                               \
    static zfifo_t name = ZFIFO_INITIALIZER(name##_buf, sizeof(type), cap)

/* Returns 0 on success, -1 if args are invalid (1 <= cap < 2^31). */
int zfifo_init(zfifo_t *f, void *buf, uint32_t esize, uint32_t cap);

/* Not concurrency-safe: call only while both sides are idle. */
void zfifo_reset(zfifo_t *f);

static inline uint32_t zfifo_capacity(const zfifo_t *f) { return f->cap; }

/* Exact when called by producer or consumer; approximate from a third context. */
uint32_t zfifo_used(const zfifo_t *f);
uint32_t zfifo_space(const zfifo_t *f);
static inline bool zfifo_is_empty(const zfifo_t *f) { return zfifo_used(f) == 0u; }
static inline bool zfifo_is_full(const zfifo_t *f) { return zfifo_space(f) == 0u; }

/* ---- producer side ---- */
bool zfifo_put(zfifo_t *f, const void *elem);
/* Writes up to n elements, returns number actually written. */
uint32_t zfifo_write(zfifo_t *f, const void *src, uint32_t n);
/* Zero-copy: get contiguous free span, fill it, then commit n <= returned count. */
uint32_t zfifo_write_acquire(zfifo_t *f, void **ptr);
void zfifo_write_commit(zfifo_t *f, uint32_t n);

/* ---- consumer side ---- */
bool zfifo_get(zfifo_t *f, void *elem);
bool zfifo_peek(const zfifo_t *f, void *elem);
/* Reads up to n elements, returns number actually read. */
uint32_t zfifo_read(zfifo_t *f, void *dst, uint32_t n);
/* Discards up to n elements, returns number discarded. */
uint32_t zfifo_drop(zfifo_t *f, uint32_t n);
/* Discards everything currently queued. */
void zfifo_flush(zfifo_t *f);
/* Zero-copy: get contiguous filled span, consume it, then release n <= returned count. */
uint32_t zfifo_read_acquire(zfifo_t *f, void **ptr);
void zfifo_read_release(zfifo_t *f, uint32_t n);

#ifdef __cplusplus
}
#endif

#endif /* ZFIFO_H */
