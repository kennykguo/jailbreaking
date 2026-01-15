#ifndef _ARCHITECTURE_BYTE_ORDER_H_
#define _ARCHITECTURE_BYTE_ORDER_H_

#include <stdint.h>

static inline uint16_t _nx_swap16(uint16_t x) {
    return (uint16_t)((x << 8) | (x >> 8));
}

static inline uint32_t _nx_swap32(uint32_t x) {
    return ((x & 0x000000FFu) << 24) |
           ((x & 0x0000FF00u) <<  8) |
           ((x & 0x00FF0000u) >>  8) |
           ((x & 0xFF000000u) >> 24);
}

static inline uint64_t _nx_swap64(uint64_t x) {
    return ((uint64_t)_nx_swap32((uint32_t)(x & 0xFFFFFFFFULL)) << 32) |
            (uint64_t)_nx_swap32((uint32_t)(x >> 32));
}

#define NXSwapShort(x) _nx_swap16((uint16_t)(x))
#define NXSwapInt(x)   _nx_swap32((uint32_t)(x))
#define NXSwapLong(x)  _nx_swap32((uint32_t)(x))
#define NXSwapLongLong(x) _nx_swap64((uint64_t)(x))

#endif /* _ARCHITECTURE_BYTE_ORDER_H_ */
