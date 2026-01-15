#ifndef _MACH_MACHINE_THREAD_STATUS_H_
#define _MACH_MACHINE_THREAD_STATUS_H_

#include <stdint.h>

#ifndef ARM_THREAD_STATE64
#define ARM_THREAD_STATE64 6
#endif

#ifndef ARM_THREAD_STATE64_COUNT
#define ARM_THREAD_STATE64_COUNT 68
#endif

struct arm_thread_state64 {
    uint64_t x[29];
    uint64_t fp;
    uint64_t lr;
    uint64_t sp;
    uint64_t pc;
    uint32_t cpsr;
    uint32_t pad;
};

#endif /* _MACH_MACHINE_THREAD_STATUS_H_ */
