#include <stdio.h>
#include <stdlib.h>

#ifdef __APPLE__
#include <objc/runtime.h>
#include <objc/message.h>
#include <dlfcn.h>
#endif

#ifdef __APPLE__
static long long compute_impl(id self, SEL _cmd, long long x) {
    (void)self;
    (void)_cmd;
    printf("[target] compute_impl called with x=%lld\n", x);
    return x + 1;
}
#endif

int main(void) {
#ifndef __APPLE__
    fprintf(stderr, "target: Obj-C runtime demo only works on macOS\n");
    return 1;
#else
    Class base = objc_getClass("NSObject");
    if (!base) {
        fprintf(stderr, "error: NSObject not found\n");
        return 1;
    }

    Class cls = objc_allocateClassPair(base, "RTTarget", 0);
    if (!cls) {
        fprintf(stderr, "error: objc_allocateClassPair failed\n");
        return 1;
    }

    SEL sel = sel_registerName("compute:");
    if (!class_addMethod(cls, sel, (IMP)compute_impl, "q@:q")) {
        fprintf(stderr, "error: class_addMethod failed\n");
        return 1;
    }

    objc_registerClassPair(cls);

    id obj = class_createInstance(cls, 0);
    if (!obj) {
        fprintf(stderr, "error: class_createInstance failed\n");
        return 1;
    }

    // If an injected dylib provides hook_install(), call it.
    void (*hook_install)(void) = NULL;
    hook_install = (void (*)(void))dlsym(RTLD_DEFAULT, "hook_install");
    if (hook_install) {
        hook_install();
    }

    long long (*msgSend)(id, SEL, long long) = (void *)objc_msgSend;
    long long result = msgSend(obj, sel, 41);
    printf("[target] result=%lld\n", result);

    return 0;
#endif
}
