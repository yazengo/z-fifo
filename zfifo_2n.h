/* zfifo_2n - lock-free SPSC ring buffer, power-of-2 capacity (Zephyr spsc_lockfree style). */
/* One producer + one consumer (thread or ISR) need no lock; more of either do. */
#ifndef ZFIFO_2N_H
#define ZFIFO_2N_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t in;     /* written only by producer */
    uint32_t out;    /* written only by consumer */
    uint32_t mask;   /* capacity - 1 */
    uint32_t esize;  /* element size in bytes */
    uint8_t *buf;
} zfifo_2n_t;

#define ZFIFO_2N_INITIALIZER(_buf, _esize, _cap) \
    { 0u, 0u, (uint32_t)(_cap) - 1u, (uint32_t)(_esize), (uint8_t *)(_buf) }

/* Statically define a fifo of `cap` elements of `type`; cap must be power of 2. */
#define ZFIFO_2N_DEFINE(name, type, cap)                                          \
    typedef char name##_cap_must_be_pow2[((cap) > 1 && ((cap) & ((cap) - 1)) == 0) ? 1 : -1]; \
    static type name##_buf[cap];                                               \
    static zfifo_2n_t name = ZFIFO_2N_INITIALIZER(name##_buf, sizeof(type), cap)

/* Returns 0 on success, -1 if args are invalid (cap must be power of 2, >= 2). */
int zfifo_2n_init(zfifo_2n_t *f, void *buf, uint32_t esize, uint32_t cap);

/* Not concurrency-safe: call only while both sides are idle. */
void zfifo_2n_reset(zfifo_2n_t *f);

static inline uint32_t zfifo_2n_capacity(const zfifo_2n_t *f) { return f->mask + 1u; }

/* Safe from either side; result is a snapshot. */
uint32_t zfifo_2n_used(const zfifo_2n_t *f);
uint32_t zfifo_2n_space(const zfifo_2n_t *f);
static inline bool zfifo_2n_is_empty(const zfifo_2n_t *f) { return zfifo_2n_used(f) == 0u; }
static inline bool zfifo_2n_is_full(const zfifo_2n_t *f) { return zfifo_2n_space(f) == 0u; }

/* ---- producer side ---- */
bool zfifo_2n_put(zfifo_2n_t *f, const void *elem);
/* Writes up to n elements, returns number actually written. */
uint32_t zfifo_2n_write(zfifo_2n_t *f, const void *src, uint32_t n);
/* Zero-copy: get contiguous free span, fill it, then commit n <= returned count. */
uint32_t zfifo_2n_write_acquire(zfifo_2n_t *f, void **ptr);
void zfifo_2n_write_commit(zfifo_2n_t *f, uint32_t n);

/* ---- consumer side ---- */
bool zfifo_2n_get(zfifo_2n_t *f, void *elem);
bool zfifo_2n_peek(const zfifo_2n_t *f, void *elem);
/* Reads up to n elements, returns number actually read. */
uint32_t zfifo_2n_read(zfifo_2n_t *f, void *dst, uint32_t n);
/* Discards up to n elements, returns number discarded. */
uint32_t zfifo_2n_drop(zfifo_2n_t *f, uint32_t n);
/* Discards everything currently queued. */
void zfifo_2n_flush(zfifo_2n_t *f);
/* Zero-copy: get contiguous filled span, consume it, then release n <= returned count. */
uint32_t zfifo_2n_read_acquire(zfifo_2n_t *f, void **ptr);
void zfifo_2n_read_release(zfifo_2n_t *f, uint32_t n);

#ifdef __cplusplus
}
#endif

#endif /* ZFIFO_2N_H */
