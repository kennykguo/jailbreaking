#include <stdio.h>
#include <stdlib.h>

#ifdef __APPLE__
#include <objc/runtime.h>
#endif

#ifdef __APPLE__
static IMP g_orig = NULL;

static long long hooked_compute(id self, SEL _cmd, long long x) {
    printf("[hook] intercepted compute:, x=%lld\n", x);

    long long (*orig)(id, SEL, long long) = (void *)g_orig;
    long long out = 0;
    if (orig) {
        out = orig(self, _cmd, x);
    }

    printf("[hook] original returned %lld, patching to 1337\n", out);
    return 1337;
}

void hook_install(void) {
    Class cls = objc_getClass("RTTarget");
    if (!cls) {
        fprintf(stderr, "[hook] RTTarget not found\n");
        return;
    }

    SEL sel = sel_registerName("compute:");
    Method m = class_getInstanceMethod(cls, sel);
    if (!m) {
        fprintf(stderr, "[hook] compute: not found\n");
        return;
    }

    g_orig = method_setImplementation(m, (IMP)hooked_compute);
    printf("[hook] compute: hooked\n");
}
#endif

int dummy_export(void) {
    // Keep object non-empty for non-Apple builds.
    return 0;
}
