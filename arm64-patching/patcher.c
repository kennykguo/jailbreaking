#include "patcher.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <sys/mman.h>

#ifdef __APPLE__
#include <libkern/OSCacheControl.h>
#include <mach/mach_init.h>
#include <mach/mach_vm.h>
#include <mach/mach_error.h>
#include <pthread.h>
#endif

static size_t page_size(void) {
    long ps = sysconf(_SC_PAGESIZE);
    if (ps <= 0) return 4096;
    return (size_t)ps;
}

static void *page_align(void *p) {
    size_t ps = page_size();
    uintptr_t u = (uintptr_t)p;
    return (void *)(u & ~(ps - 1));
}

#if defined(__APPLE__) && defined(__aarch64__)
static void jit_write_enable(void) {
    pthread_jit_write_protect_np(0);
}

static void jit_write_disable(void) {
    pthread_jit_write_protect_np(1);
}
#else
static void jit_write_enable(void) {
}

static void jit_write_disable(void) {
}
#endif

int arm64_encode_b(uint8_t *src, const uint8_t *dst, uint32_t *out_insn) {
    if (!src || !dst || !out_insn) return -1;
    // PC is the address of the current instruction.
    intptr_t delta = (intptr_t)dst - (intptr_t)src;
    // B uses imm26 in units of 4 bytes.
    if ((delta & 0x3) != 0) return -1;
    intptr_t imm26 = delta >> 2;
    // Range is signed 26 bits.
    if (imm26 < -(1 << 25) || imm26 > ((1 << 25) - 1)) return -1;
    uint32_t insn = 0x14000000u | ((uint32_t)imm26 & 0x03FFFFFFu);
    *out_insn = insn;
    return 0;
}

__attribute__((aligned(PATCHER_PAGE_SIZE)))
int arm64_hotpatch(void *addr, const uint8_t *bytes, size_t len) {
    if (!addr || !bytes || len == 0) return -1;

    if (page_size() != PATCHER_PAGE_SIZE) {
        fprintf(stderr, "error: page size %zu != PATCHER_PAGE_SIZE %d\n",
                page_size(), PATCHER_PAGE_SIZE);
        return -1;
    }

    void *page = page_align(addr);
    size_t ps = page_size();

    if (page == page_align((void *)arm64_hotpatch)) {
        fprintf(stderr, "error: patching from same page as arm64_hotpatch\n");
        return -1;
    }

#if defined(__APPLE__)
    kern_return_t kr = mach_vm_protect(mach_task_self(),
                                       (mach_vm_address_t)page,
                                       (mach_vm_size_t)ps,
                                       FALSE,
                                       VM_PROT_READ | VM_PROT_WRITE | VM_PROT_COPY);
    if (kr != KERN_SUCCESS) {
        fprintf(stderr, "mach_vm_protect: %s\n", mach_error_string(kr));
        return -1;
    }
#else
    if (mprotect(page, ps, PROT_READ | PROT_WRITE) != 0) {
        perror("mprotect");
        return -1;
    }
#endif

    memcpy(addr, bytes, len);

#if defined(__APPLE__)
    sys_icache_invalidate(addr, len);
#elif defined(__GNUC__)
    __builtin___clear_cache((char *)addr, (char *)addr + len);
#endif

#if defined(__APPLE__)
    kr = mach_vm_protect(mach_task_self(),
                         (mach_vm_address_t)page,
                         (mach_vm_size_t)ps,
                         FALSE,
                         VM_PROT_READ | VM_PROT_EXECUTE);
    if (kr != KERN_SUCCESS) {
        fprintf(stderr, "mach_vm_protect: %s\n", mach_error_string(kr));
        return -1;
    }
#else
    if (mprotect(page, ps, PROT_READ | PROT_EXEC) != 0) {
        perror("mprotect");
        return -1;
    }
#endif

    return 0;
}

int arm64_patch_b(void *src, const void *dst) {
    uint32_t insn = 0;
    if (arm64_encode_b((uint8_t *)src, (const uint8_t *)dst, &insn) != 0) {
        return -1;
    }
    return arm64_hotpatch(src, (const uint8_t *)&insn, sizeof(insn));
}

void *arm64_make_trampoline(void *src, size_t insn_bytes) {
    if (!src || insn_bytes == 0 || (insn_bytes % 4) != 0) return NULL;

    size_t tramp_size = insn_bytes + 4; // copied insns + branch back
    int map_flags = MAP_PRIVATE | MAP_ANON;
#if defined(__APPLE__) && defined(__aarch64__)
    map_flags |= MAP_JIT;
#endif
    void *tramp = mmap(NULL, tramp_size,
                       PROT_READ | PROT_WRITE | PROT_EXEC,
                       map_flags, -1, 0);
    if (tramp == MAP_FAILED) {
        perror("mmap");
        return NULL;
    }

    jit_write_enable();
    memcpy(tramp, src, insn_bytes);

    uint8_t *tramp_branch = (uint8_t *)tramp + insn_bytes;
    uint32_t br = 0;
    if (arm64_encode_b(tramp_branch, (const uint8_t *)src + insn_bytes, &br) != 0) {
        fprintf(stderr, "error: trampoline branch out of range\n");
        jit_write_disable();
        munmap(tramp, tramp_size);
        return NULL;
    }
    memcpy(tramp_branch, &br, sizeof(br));
    jit_write_disable();

#if defined(__APPLE__)
    sys_icache_invalidate(tramp, tramp_size);
#elif defined(__GNUC__)
    __builtin___clear_cache((char *)tramp, (char *)tramp + tramp_size);
#endif

    return tramp;
}

void *arm64_patch_prologue(void *src, void *hook, size_t insn_bytes) {
    void *tramp = arm64_make_trampoline(src, insn_bytes);
    if (!tramp) return NULL;

    if (arm64_patch_b(src, hook) != 0) {
        return NULL;
    }

    return tramp;
}
