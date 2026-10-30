#include "zfifo_2n.h"
#include "zfifo_port.h"

#include <string.h>

#define ELEM(f, idx) ((f)->buf + (size_t)((idx) & (f)->mask) * (f)->esize)

int zfifo_2n_init(zfifo_2n_t *f, void *buf, uint32_t esize, uint32_t cap)
{
    /* cap <= 2^31 keeps (in - out) unambiguous across counter wrap */
    if (!f || !buf || esize == 0u || cap < 2u || cap > 0x80000000u || (cap & (cap - 1u)) != 0u) {
        return -1;
    }
    f->in = 0u;
    f->out = 0u;
    f->mask = cap - 1u;
    f->esize = esize;
    f->buf = (uint8_t *)buf;
    return 0;
}

void zfifo_2n_reset(zfifo_2n_t *f)
{
    f->in = 0u;
    f->out = 0u;
}

uint32_t zfifo_2n_used(const zfifo_2n_t *f)
{
    /* out before in keeps the diff non-negative; clamp covers a racing consumer */
    uint32_t out = ZFIFO_LOAD_ACQ(&f->out);
    uint32_t used = ZFIFO_LOAD_ACQ(&f->in) - out;

    return used > zfifo_2n_capacity(f) ? zfifo_2n_capacity(f) : used;
}

uint32_t zfifo_2n_space(const zfifo_2n_t *f)
{
    return zfifo_2n_capacity(f) - zfifo_2n_used(f);
}

/* Copy n elements between linear memory and the ring starting at idx, handling wrap. */
static void ring_copy_in(zfifo_2n_t *f, uint32_t idx, const uint8_t *src, uint32_t n)
{
    uint32_t off = idx & f->mask;
    uint32_t first = zfifo_2n_capacity(f) - off;

    if (first > n) {
        first = n;
    }
    memcpy(f->buf + (size_t)off * f->esize, src, (size_t)first * f->esize);
    if (n > first) {
        memcpy(f->buf, src + (size_t)first * f->esize, (size_t)(n - first) * f->esize);
    }
}

static void ring_copy_out(const zfifo_2n_t *f, uint32_t idx, uint8_t *dst, uint32_t n)
{
    uint32_t off = idx & f->mask;
    uint32_t first = zfifo_2n_capacity(f) - off;

    if (first > n) {
        first = n;
    }
    memcpy(dst, f->buf + (size_t)off * f->esize, (size_t)first * f->esize);
    if (n > first) {
        memcpy(dst + (size_t)first * f->esize, f->buf, (size_t)(n - first) * f->esize);
    }
}

/* ---- producer side ---- */

bool zfifo_2n_put(zfifo_2n_t *f, const void *elem)
{
    uint32_t in = f->in;

    /* acquire on out: consumer has finished reading the slot we may reuse */
    if (in - ZFIFO_LOAD_ACQ(&f->out) >= zfifo_2n_capacity(f)) {
        return false;
    }
    memcpy(ELEM(f, in), elem, f->esize);
    ZFIFO_STORE_REL(&f->in, in + 1u);
    return true;
}

uint32_t zfifo_2n_write(zfifo_2n_t *f, const void *src, uint32_t n)
{
    uint32_t in = f->in;
    uint32_t space = zfifo_2n_capacity(f) - (in - ZFIFO_LOAD_ACQ(&f->out));

    if (n > space) {
        n = space;
    }
    if (n == 0u) {
        return 0u;
    }
    ring_copy_in(f, in, (const uint8_t *)src, n);
    ZFIFO_STORE_REL(&f->in, in + n);
    return n;
}

uint32_t zfifo_2n_write_acquire(zfifo_2n_t *f, void **ptr)
{
    uint32_t in = f->in;
    uint32_t space = zfifo_2n_capacity(f) - (in - ZFIFO_LOAD_ACQ(&f->out));
    uint32_t contig = zfifo_2n_capacity(f) - (in & f->mask);

    *ptr = ELEM(f, in);
    return space < contig ? space : contig;
}

void zfifo_2n_write_commit(zfifo_2n_t *f, uint32_t n)
{
    ZFIFO_STORE_REL(&f->in, f->in + n);
}

/* ---- consumer side ---- */

bool zfifo_2n_get(zfifo_2n_t *f, void *elem)
{
    uint32_t out = f->out;

    /* acquire on in: producer's slot data is visible */
    if (ZFIFO_LOAD_ACQ(&f->in) == out) {
        return false;
    }
    memcpy(elem, ELEM(f, out), f->esize);
    ZFIFO_STORE_REL(&f->out, out + 1u);
    return true;
}

bool zfifo_2n_peek(const zfifo_2n_t *f, void *elem)
{
    uint32_t out = f->out;

    if (ZFIFO_LOAD_ACQ(&f->in) == out) {
        return false;
    }
    memcpy(elem, ELEM(f, out), f->esize);
    return true;
}

uint32_t zfifo_2n_read(zfifo_2n_t *f, void *dst, uint32_t n)
{
    uint32_t out = f->out;
    uint32_t used = ZFIFO_LOAD_ACQ(&f->in) - out;

    if (n > used) {
        n = used;
    }
    if (n == 0u) {
        return 0u;
    }
    ring_copy_out(f, out, (uint8_t *)dst, n);
    ZFIFO_STORE_REL(&f->out, out + n);
    return n;
}

uint32_t zfifo_2n_drop(zfifo_2n_t *f, uint32_t n)
{
    uint32_t out = f->out;
    uint32_t used = ZFIFO_LOAD_ACQ(&f->in) - out;

    if (n > used) {
        n = used;
    }
    ZFIFO_STORE_REL(&f->out, out + n);
    return n;
}

void zfifo_2n_flush(zfifo_2n_t *f)
{
    ZFIFO_STORE_REL(&f->out, ZFIFO_LOAD_ACQ(&f->in));
}

uint32_t zfifo_2n_read_acquire(zfifo_2n_t *f, void **ptr)
{
    uint32_t out = f->out;
    uint32_t used = ZFIFO_LOAD_ACQ(&f->in) - out;
    uint32_t contig = zfifo_2n_capacity(f) - (out & f->mask);

    *ptr = ELEM(f, out);
    return used < contig ? used : contig;
}

void zfifo_2n_read_release(zfifo_2n_t *f, uint32_t n)
{
    ZFIFO_STORE_REL(&f->out, f->out + n);
}
