#include "zfifo.h"
#include "zfifo_port.h"

#include <string.h>

/* Elements between out and in; both in [0, 2*cap). */
static inline uint32_t count(const zfifo_t *f, uint32_t in, uint32_t out)
{
    return in >= out ? in - out : in + 2u * f->cap - out;
}

/* idx + n modulo 2*cap, written to avoid uint32 overflow for large cap. */
static inline uint32_t advance(const zfifo_t *f, uint32_t idx, uint32_t n)
{
    uint32_t lim = 2u * f->cap - n;

    return idx >= lim ? idx - lim : idx + n;
}

static inline uint32_t slot(const zfifo_t *f, uint32_t idx)
{
    return idx >= f->cap ? idx - f->cap : idx;
}

#define ELEM(f, idx) ((f)->buf + (size_t)slot((f), (idx)) * (f)->esize)

int zfifo_init(zfifo_t *f, void *buf, uint32_t esize, uint32_t cap)
{
    /* cap < 2^31 so that 2*cap fits in uint32 */
    if (!f || !buf || esize == 0u || cap == 0u || cap >= 0x80000000u) {
        return -1;
    }
    f->in = 0u;
    f->out = 0u;
    f->cap = cap;
    f->esize = esize;
    f->buf = (uint8_t *)buf;
    return 0;
}

void zfifo_reset(zfifo_t *f)
{
    f->in = 0u;
    f->out = 0u;
}

uint32_t zfifo_used(const zfifo_t *f)
{
    uint32_t out = ZFIFO_LOAD_ACQ(&f->out);
    uint32_t used = count(f, ZFIFO_LOAD_ACQ(&f->in), out);

    return used > f->cap ? f->cap : used;
}

uint32_t zfifo_space(const zfifo_t *f)
{
    return f->cap - zfifo_used(f);
}

static void ring_copy_in(zfifo_t *f, uint32_t idx, const uint8_t *src, uint32_t n)
{
    uint32_t off = slot(f, idx);
    uint32_t first = f->cap - off;

    if (first > n) {
        first = n;
    }
    memcpy(f->buf + (size_t)off * f->esize, src, (size_t)first * f->esize);
    if (n > first) {
        memcpy(f->buf, src + (size_t)first * f->esize, (size_t)(n - first) * f->esize);
    }
}

static void ring_copy_out(const zfifo_t *f, uint32_t idx, uint8_t *dst, uint32_t n)
{
    uint32_t off = slot(f, idx);
    uint32_t first = f->cap - off;

    if (first > n) {
        first = n;
    }
    memcpy(dst, f->buf + (size_t)off * f->esize, (size_t)first * f->esize);
    if (n > first) {
        memcpy(dst + (size_t)first * f->esize, f->buf, (size_t)(n - first) * f->esize);
    }
}

/* ---- producer side ---- */

bool zfifo_put(zfifo_t *f, const void *elem)
{
    uint32_t in = f->in;

    /* acquire on out: consumer has finished reading the slot we may reuse */
    if (count(f, in, ZFIFO_LOAD_ACQ(&f->out)) >= f->cap) {
        return false;
    }
    memcpy(ELEM(f, in), elem, f->esize);
    ZFIFO_STORE_REL(&f->in, advance(f, in, 1u));
    return true;
}

uint32_t zfifo_write(zfifo_t *f, const void *src, uint32_t n)
{
    uint32_t in = f->in;
    uint32_t space = f->cap - count(f, in, ZFIFO_LOAD_ACQ(&f->out));

    if (n > space) {
        n = space;
    }
    if (n == 0u) {
        return 0u;
    }
    ring_copy_in(f, in, (const uint8_t *)src, n);
    ZFIFO_STORE_REL(&f->in, advance(f, in, n));
    return n;
}

uint32_t zfifo_write_acquire(zfifo_t *f, void **ptr)
{
    uint32_t in = f->in;
    uint32_t space = f->cap - count(f, in, ZFIFO_LOAD_ACQ(&f->out));
    uint32_t contig = f->cap - slot(f, in);

    *ptr = ELEM(f, in);
    return space < contig ? space : contig;
}

void zfifo_write_commit(zfifo_t *f, uint32_t n)
{
    ZFIFO_STORE_REL(&f->in, advance(f, f->in, n));
}

/* ---- consumer side ---- */

bool zfifo_get(zfifo_t *f, void *elem)
{
    uint32_t out = f->out;

    /* acquire on in: producer's slot data is visible */
    if (ZFIFO_LOAD_ACQ(&f->in) == out) {
        return false;
    }
    memcpy(elem, ELEM(f, out), f->esize);
    ZFIFO_STORE_REL(&f->out, advance(f, out, 1u));
    return true;
}

bool zfifo_peek(const zfifo_t *f, void *elem)
{
    uint32_t out = f->out;

    if (ZFIFO_LOAD_ACQ(&f->in) == out) {
        return false;
    }
    memcpy(elem, ELEM(f, out), f->esize);
    return true;
}

uint32_t zfifo_read(zfifo_t *f, void *dst, uint32_t n)
{
    uint32_t out = f->out;
    uint32_t used = count(f, ZFIFO_LOAD_ACQ(&f->in), out);

    if (n > used) {
        n = used;
    }
    if (n == 0u) {
        return 0u;
    }
    ring_copy_out(f, out, (uint8_t *)dst, n);
    ZFIFO_STORE_REL(&f->out, advance(f, out, n));
    return n;
}

uint32_t zfifo_drop(zfifo_t *f, uint32_t n)
{
    uint32_t out = f->out;
    uint32_t used = count(f, ZFIFO_LOAD_ACQ(&f->in), out);

    if (n > used) {
        n = used;
    }
    ZFIFO_STORE_REL(&f->out, advance(f, out, n));
    return n;
}

void zfifo_flush(zfifo_t *f)
{
    ZFIFO_STORE_REL(&f->out, ZFIFO_LOAD_ACQ(&f->in));
}

uint32_t zfifo_read_acquire(zfifo_t *f, void **ptr)
{
    uint32_t out = f->out;
    uint32_t used = count(f, ZFIFO_LOAD_ACQ(&f->in), out);
    uint32_t contig = f->cap - slot(f, out);

    *ptr = ELEM(f, out);
    return used < contig ? used : contig;
}

void zfifo_read_release(zfifo_t *f, uint32_t n)
{
    ZFIFO_STORE_REL(&f->out, advance(f, f->out, n));
}
