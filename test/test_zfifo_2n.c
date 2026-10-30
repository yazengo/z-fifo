/* Host unit + SPSC stress tests for zfifo. */
#include "zfifo_2n.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(c)                                                         \
    do {                                                                 \
        if (!(c)) {                                                      \
            fprintf(stderr, "%s:%d CHECK failed: %s\n", __FILE__, __LINE__, #c); \
            exit(1);                                                     \
        }                                                                \
    } while (0)

typedef struct {
    uint32_t seq;
    uint16_t a;
    uint8_t b;
} rec_t;

ZFIFO_2N_DEFINE(s_static, rec_t, 8);

static void test_init(void)
{
    zfifo_2n_t f;
    uint8_t buf[16];

    CHECK(zfifo_2n_init(&f, buf, 1, 0) == -1);
    CHECK(zfifo_2n_init(&f, buf, 1, 1) == -1);
    CHECK(zfifo_2n_init(&f, buf, 1, 12) == -1);
    CHECK(zfifo_2n_init(&f, buf, 0, 16) == -1);
    CHECK(zfifo_2n_init(&f, NULL, 1, 16) == -1);
    CHECK(zfifo_2n_init(&f, buf, 1, 16) == 0);
    CHECK(zfifo_2n_capacity(&f) == 16);
    CHECK(zfifo_2n_is_empty(&f) && !zfifo_2n_is_full(&f));
}

static void test_put_get_struct(void)
{
    rec_t r;
    uint32_t i;

    CHECK(zfifo_2n_capacity(&s_static) == 8);
    for (i = 0; i < 8; i++) {
        r.seq = i; r.a = (uint16_t)(i * 3); r.b = (uint8_t)i;
        CHECK(zfifo_2n_put(&s_static, &r));
    }
    CHECK(zfifo_2n_is_full(&s_static));
    CHECK(!zfifo_2n_put(&s_static, &r));
    CHECK(zfifo_2n_peek(&s_static, &r) && r.seq == 0);
    for (i = 0; i < 8; i++) {
        CHECK(zfifo_2n_get(&s_static, &r));
        CHECK(r.seq == i && r.a == i * 3 && r.b == i);
    }
    CHECK(!zfifo_2n_get(&s_static, &r));
    CHECK(!zfifo_2n_peek(&s_static, &r));
}

static void test_bulk_wrap(void)
{
    zfifo_2n_t f;
    uint8_t buf[8], in[20], out[20];
    uint32_t i;

    for (i = 0; i < sizeof(in); i++) {
        in[i] = (uint8_t)i;
    }
    zfifo_2n_init(&f, buf, 1, 8);
    CHECK(zfifo_2n_write(&f, in, 5) == 5);
    CHECK(zfifo_2n_read(&f, out, 3) == 3 && memcmp(out, in, 3) == 0);
    /* 2 queued, 6 free; write across the wrap point, truncated to space */
    CHECK(zfifo_2n_write(&f, in + 5, 10) == 6);
    CHECK(zfifo_2n_used(&f) == 8 && zfifo_2n_space(&f) == 0);
    CHECK(zfifo_2n_write(&f, in, 1) == 0);
    CHECK(zfifo_2n_read(&f, out, 20) == 8 && memcmp(out, in + 3, 8) == 0);
    CHECK(zfifo_2n_read(&f, out, 1) == 0);

    CHECK(zfifo_2n_write(&f, in, 4) == 4);
    CHECK(zfifo_2n_drop(&f, 3) == 3 && zfifo_2n_used(&f) == 1);
    CHECK(zfifo_2n_drop(&f, 9) == 1 && zfifo_2n_is_empty(&f));
    CHECK(zfifo_2n_write(&f, in, 4) == 4);
    zfifo_2n_flush(&f);
    CHECK(zfifo_2n_is_empty(&f));
}

static void test_counter_wrap(void)
{
    zfifo_2n_t f;
    uint32_t buf[4], v, i;

    zfifo_2n_init(&f, buf, sizeof(uint32_t), 4);
    f.in = f.out = 0xFFFFFFFEu; /* force 32-bit counter overflow */
    for (i = 0; i < 4; i++) {
        CHECK(zfifo_2n_put(&f, &i));
    }
    CHECK(zfifo_2n_is_full(&f) && zfifo_2n_used(&f) == 4);
    for (i = 0; i < 4; i++) {
        CHECK(zfifo_2n_get(&f, &v) && v == i);
    }
    CHECK(zfifo_2n_is_empty(&f));
}

static void test_zero_copy(void)
{
    zfifo_2n_t f;
    uint8_t buf[8];
    void *p;
    uint32_t n;

    zfifo_2n_init(&f, buf, 1, 8);
    zfifo_2n_write(&f, "abcdef", 6);
    zfifo_2n_drop(&f, 6);
    /* in = out = 6: only 2 contiguous slots until the wrap */
    n = zfifo_2n_write_acquire(&f, &p);
    CHECK(n == 2 && p == buf + 6);
    memcpy(p, "XY", 2);
    zfifo_2n_write_commit(&f, 2);
    n = zfifo_2n_write_acquire(&f, &p);
    CHECK(n == 6 && p == buf);
    memcpy(p, "Z", 1);
    zfifo_2n_write_commit(&f, 1);

    n = zfifo_2n_read_acquire(&f, &p);
    CHECK(n == 2 && memcmp(p, "XY", 2) == 0);
    zfifo_2n_read_release(&f, 2);
    n = zfifo_2n_read_acquire(&f, &p);
    CHECK(n == 1 && *(uint8_t *)p == 'Z');
    zfifo_2n_read_release(&f, 1);
    CHECK(zfifo_2n_read_acquire(&f, &p) == 0);
}

/* ---- SPSC stress: producer and consumer threads, mixed APIs ---- */

#define STRESS_TOTAL 2000000u
static zfifo_2n_t s_sf;
static uint32_t s_sbuf[64];

static uint32_t rnd(uint32_t *s)
{
    *s ^= *s << 13; *s ^= *s >> 17; *s ^= *s << 5;
    return *s;
}

static void *producer(void *arg)
{
    uint32_t seed = 0x1234567u, next = 0, tmp[37], i, n, k;
    void *p;

    (void)arg;
    while (next < STRESS_TOTAL) {
        switch (rnd(&seed) % 3) {
        case 0:
            if (zfifo_2n_put(&s_sf, &next)) {
                next++;
            }
            break;
        case 1:
            n = rnd(&seed) % 37 + 1;
            for (i = 0; i < n; i++) {
                tmp[i] = next + i;
            }
            next += zfifo_2n_write(&s_sf, tmp, n);
            break;
        default:
            n = zfifo_2n_write_acquire(&s_sf, &p);
            k = n ? rnd(&seed) % n + 1 : 0;
            for (i = 0; i < k; i++) {
                ((uint32_t *)p)[i] = next + i;
            }
            zfifo_2n_write_commit(&s_sf, k);
            next += k;
            break;
        }
        if (next > STRESS_TOTAL) {
            next = STRESS_TOTAL; /* bulk may overshoot; consumer stops at TOTAL */
        }
    }
    return NULL;
}

static void *consumer(void *arg)
{
    uint32_t seed = 0x89abcdefu, expect = 0, tmp[41], v, i, n, k;
    void *p;

    (void)arg;
    while (expect < STRESS_TOTAL) {
        switch (rnd(&seed) % 3) {
        case 0:
            if (zfifo_2n_get(&s_sf, &v)) {
                CHECK(v == expect);
                expect++;
            }
            break;
        case 1:
            n = zfifo_2n_read(&s_sf, tmp, rnd(&seed) % 41 + 1);
            for (i = 0; i < n; i++) {
                CHECK(tmp[i] == expect);
                expect++;
            }
            break;
        default:
            n = zfifo_2n_read_acquire(&s_sf, &p);
            k = n ? rnd(&seed) % n + 1 : 0;
            for (i = 0; i < k; i++) {
                CHECK(((uint32_t *)p)[i] == expect);
                expect++;
            }
            zfifo_2n_read_release(&s_sf, k);
            break;
        }
    }
    return NULL;
}

static void test_spsc_stress(void)
{
    pthread_t tp, tc;

    zfifo_2n_init(&s_sf, s_sbuf, sizeof(uint32_t), 64);
    pthread_create(&tc, NULL, consumer, NULL);
    pthread_create(&tp, NULL, producer, NULL);
    pthread_join(tp, NULL);
    pthread_join(tc, NULL);
}

int main(void)
{
    test_init();
    test_put_get_struct();
    test_bulk_wrap();
    test_counter_wrap();
    test_zero_copy();
    test_spsc_stress();
    printf("zfifo_2n: all tests passed\n");
    return 0;
}
