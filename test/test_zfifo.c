/* Host unit + SPSC stress tests for zfifo. */
#include "zfifo.h"

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

ZFIFO_DEFINE(s_static, rec_t, 5);

static void test_init(void)
{
    zfifo_t f;
    uint8_t buf[16];

    CHECK(zfifo_init(&f, buf, 1, 0) == -1);
    CHECK(zfifo_init(&f, buf, 1, 0x80000000u) == -1);
    CHECK(zfifo_init(&f, buf, 0, 3) == -1);
    CHECK(zfifo_init(&f, NULL, 1, 3) == -1);
    CHECK(zfifo_init(&f, buf, 1, 1) == 0);
    CHECK(zfifo_init(&f, buf, 1, 12) == 0);
    CHECK(zfifo_capacity(&f) == 12);
    CHECK(zfifo_is_empty(&f) && !zfifo_is_full(&f));
}

static void test_cap1(void)
{
    zfifo_t f;
    uint8_t buf[1], v = 0, i;

    zfifo_init(&f, buf, 1, 1);
    for (i = 0; i < 10; i++) {
        CHECK(zfifo_put(&f, &i));
        CHECK(zfifo_is_full(&f) && !zfifo_put(&f, &i));
        CHECK(zfifo_get(&f, &v) && v == i);
        CHECK(zfifo_is_empty(&f) && !zfifo_get(&f, &v));
    }
}

static void test_put_get_struct(void)
{
    rec_t r;
    uint32_t i, round;

    CHECK(zfifo_capacity(&s_static) == 5);
    /* several rounds so counters pass through the 2*cap wrap */
    for (round = 0; round < 7; round++) {
        for (i = 0; i < 5; i++) {
            r.seq = i; r.a = (uint16_t)(i * 3); r.b = (uint8_t)round;
            CHECK(zfifo_put(&s_static, &r));
        }
        CHECK(zfifo_is_full(&s_static) && !zfifo_put(&s_static, &r));
        CHECK(zfifo_peek(&s_static, &r) && r.seq == 0);
        for (i = 0; i < 5; i++) {
            CHECK(zfifo_get(&s_static, &r));
            CHECK(r.seq == i && r.a == i * 3 && r.b == round);
        }
        CHECK(!zfifo_get(&s_static, &r));
    }
}

static void test_bulk_wrap(void)
{
    zfifo_t f;
    uint8_t buf[7], in[20], out[20];
    uint32_t i;

    for (i = 0; i < sizeof(in); i++) {
        in[i] = (uint8_t)i;
    }
    zfifo_init(&f, buf, 1, 7);
    CHECK(zfifo_write(&f, in, 5) == 5);
    CHECK(zfifo_read(&f, out, 3) == 3 && memcmp(out, in, 3) == 0);
    /* 2 queued, 5 free; write across the wrap point, truncated to space */
    CHECK(zfifo_write(&f, in + 5, 10) == 5);
    CHECK(zfifo_used(&f) == 7 && zfifo_space(&f) == 0);
    CHECK(zfifo_write(&f, in, 1) == 0);
    CHECK(zfifo_read(&f, out, 20) == 7 && memcmp(out, in + 3, 7) == 0);
    CHECK(zfifo_read(&f, out, 1) == 0);

    CHECK(zfifo_write(&f, in, 4) == 4);
    CHECK(zfifo_drop(&f, 3) == 3 && zfifo_used(&f) == 1);
    CHECK(zfifo_drop(&f, 9) == 1 && zfifo_is_empty(&f));
    CHECK(zfifo_write(&f, in, 4) == 4);
    zfifo_flush(&f);
    CHECK(zfifo_is_empty(&f));
}

static void test_counter_wrap(void)
{
    zfifo_t f;
    uint32_t buf[3], v, i;

    zfifo_init(&f, buf, sizeof(uint32_t), 3);
    f.in = f.out = 5; /* 2*cap - 1: next advance wraps to 0 */
    for (i = 0; i < 3; i++) {
        CHECK(zfifo_put(&f, &i));
    }
    CHECK(f.in == 2 && zfifo_is_full(&f) && zfifo_used(&f) == 3);
    for (i = 0; i < 3; i++) {
        CHECK(zfifo_get(&f, &v) && v == i);
    }
    CHECK(zfifo_is_empty(&f));
}

static void test_max_cap_no_overflow(void)
{
    zfifo_t f;
    uint8_t dummy;
    void *p;

    /* counter math only; the buffer is never dereferenced here */
    CHECK(zfifo_init(&f, &dummy, 1, 0x7FFFFFFFu) == 0);
    f.in = f.out = 0xFFFFFFFCu; /* 2*cap - 2 */
    zfifo_write_commit(&f, 0x7FFFFFFFu);
    CHECK(f.in == 0x7FFFFFFDu && zfifo_is_full(&f));
    CHECK(zfifo_read_acquire(&f, &p) == 2); /* slots cap-2, cap-1 before wrap */
    CHECK(zfifo_drop(&f, 0x7FFFFFFFu) == 0x7FFFFFFFu && zfifo_is_empty(&f));
    CHECK(f.out == f.in);
}

static void test_zero_copy(void)
{
    zfifo_t f;
    uint8_t buf[6];
    void *p;
    uint32_t n;

    zfifo_init(&f, buf, 1, 6);
    zfifo_write(&f, "abcd", 4);
    zfifo_drop(&f, 4);
    /* in = out = 4: only 2 contiguous slots until the wrap */
    n = zfifo_write_acquire(&f, &p);
    CHECK(n == 2 && p == buf + 4);
    memcpy(p, "XY", 2);
    zfifo_write_commit(&f, 2);
    n = zfifo_write_acquire(&f, &p);
    CHECK(n == 4 && p == buf);
    memcpy(p, "Z", 1);
    zfifo_write_commit(&f, 1);

    n = zfifo_read_acquire(&f, &p);
    CHECK(n == 2 && memcmp(p, "XY", 2) == 0);
    zfifo_read_release(&f, 2);
    n = zfifo_read_acquire(&f, &p);
    CHECK(n == 1 && *(uint8_t *)p == 'Z');
    zfifo_read_release(&f, 1);
    CHECK(zfifo_read_acquire(&f, &p) == 0);
}

/* ---- SPSC stress: producer and consumer threads, mixed APIs ---- */

#define STRESS_TOTAL 1000000u
static zfifo_t s_sf;
static uint32_t s_sbuf[100];

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
            if (zfifo_put(&s_sf, &next)) {
                next++;
            }
            break;
        case 1:
            n = rnd(&seed) % 37 + 1;
            for (i = 0; i < n; i++) {
                tmp[i] = next + i;
            }
            next += zfifo_write(&s_sf, tmp, n);
            break;
        default:
            n = zfifo_write_acquire(&s_sf, &p);
            k = n ? rnd(&seed) % n + 1 : 0;
            for (i = 0; i < k; i++) {
                ((uint32_t *)p)[i] = next + i;
            }
            zfifo_write_commit(&s_sf, k);
            next += k;
            break;
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
            if (zfifo_get(&s_sf, &v)) {
                CHECK(v == expect);
                expect++;
            }
            break;
        case 1:
            n = zfifo_read(&s_sf, tmp, rnd(&seed) % 41 + 1);
            for (i = 0; i < n; i++) {
                CHECK(tmp[i] == expect);
                expect++;
            }
            break;
        default:
            n = zfifo_read_acquire(&s_sf, &p);
            k = n ? rnd(&seed) % n + 1 : 0;
            for (i = 0; i < k; i++) {
                CHECK(((uint32_t *)p)[i] == expect);
                expect++;
            }
            zfifo_read_release(&s_sf, k);
            break;
        }
    }
    return NULL;
}

static void test_spsc_stress(uint32_t cap)
{
    pthread_t tp, tc;

    zfifo_init(&s_sf, s_sbuf, sizeof(uint32_t), cap);
    pthread_create(&tc, NULL, consumer, NULL);
    pthread_create(&tp, NULL, producer, NULL);
    pthread_join(tp, NULL);
    pthread_join(tc, NULL);
}

int main(void)
{
    static const uint32_t caps[] = { 1, 3, 61, 64, 100 };
    size_t i;

    test_init();
    test_cap1();
    test_put_get_struct();
    test_bulk_wrap();
    test_counter_wrap();
    test_max_cap_no_overflow();
    test_zero_copy();
    for (i = 0; i < sizeof(caps) / sizeof(caps[0]); i++) {
        test_spsc_stress(caps[i]);
    }
    printf("zfifo: all tests passed\n");
    return 0;
}
