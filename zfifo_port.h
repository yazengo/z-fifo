/* Acquire/release accessors shared by zfifo and zfifo_2n (internal header). */
#ifndef ZFIFO_PORT_H
#define ZFIFO_PORT_H

#include <stdint.h>

/* Override both macros for toolchains not covered below. */
#if !defined(ZFIFO_LOAD_ACQ) || !defined(ZFIFO_STORE_REL)
#if defined(__GNUC__) || defined(__clang__)
#define ZFIFO_LOAD_ACQ(p)     __atomic_load_n((p), __ATOMIC_ACQUIRE)
#define ZFIFO_STORE_REL(p, v) __atomic_store_n((p), (v), __ATOMIC_RELEASE)
#elif defined(__CC_ARM) /* Keil ARMCC 5 */
static inline uint32_t zfifo_load_acq(const uint32_t *p)
{
    uint32_t v = *(const volatile uint32_t *)p;
    __dmb(0xF);
    return v;
}
static inline void zfifo_store_rel(uint32_t *p, uint32_t v)
{
    __dmb(0xF);
    *(volatile uint32_t *)p = v;
}
#define ZFIFO_LOAD_ACQ(p)     zfifo_load_acq(p)
#define ZFIFO_STORE_REL(p, v) zfifo_store_rel((p), (v))
#else
#error "zfifo: define ZFIFO_LOAD_ACQ / ZFIFO_STORE_REL for this toolchain"
#endif
#endif

#endif /* ZFIFO_PORT_H */
