#ifndef _MACH_MACHINE_H_
#define _MACH_MACHINE_H_

#include <stdint.h>

typedef int cpu_type_t;
typedef int cpu_subtype_t;

#define CPU_ARCH_ABI64 0x01000000

#define CPU_TYPE_X86      7
#define CPU_TYPE_ARM      12
#define CPU_TYPE_POWERPC  18

#define CPU_TYPE_X86_64     (CPU_TYPE_X86 | CPU_ARCH_ABI64)
#define CPU_TYPE_ARM64      (CPU_TYPE_ARM | CPU_ARCH_ABI64)
#define CPU_TYPE_POWERPC64  (CPU_TYPE_POWERPC | CPU_ARCH_ABI64)

#endif /* _MACH_MACHINE_H_ */
