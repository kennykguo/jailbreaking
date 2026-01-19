#include <stdio.h>
#include <stdint.h>

#include "patcher.h"

__attribute__((aligned(PATCHER_PAGE_SIZE)))
__attribute__((noinline))
static int target_function(int x) {
    printf("[target] target_function called with x=%d\n", x);
    return x + 1;
}

static int hook_function(int x) {
    printf("[hook] hook_function called with x=%d\n", x);
    return 1337;
}

int main(void) {
#if !defined(__APPLE__) || !defined(__aarch64__)
    fprintf(stderr, "patch_demo: runtime patching requires macOS ARM64\n");
    fprintf(stderr, "build is supported on Arch, but execution is macOS ARM64 only\n");
    return 1;
#else
    printf("[demo] calling target_function before patch\n");
    int a = target_function(41);
    printf("[demo] result=%d\n", a);

    printf("[demo] patching prologue\n");
    void *tramp = arm64_patch_prologue((void *)target_function, (void *)hook_function, 4);
    if (!tramp) {
        fprintf(stderr, "error: patch failed\n");
        return 1;
    }

    printf("[demo] calling target_function after patch\n");
    int b = target_function(41);
    printf("[demo] result=%d\n", b);

    (void)tramp; // trampoline unused in this minimal demo
    return 0;
#endif
}
