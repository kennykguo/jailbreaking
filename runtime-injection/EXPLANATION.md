# Lab 2 Explanation - Runtime Injection and Obj-C Control (Very Verbose)

This document explains Lab 2 from first principles and defines every Apple-
specific term the first time it appears. It assumes you understand general
operating systems concepts, but you are new to Apple’s ecosystem.

Lab 2 goal: gain live control of a running process by injecting a dynamic
library and intercepting Objective-C method calls at runtime.

Artifacts required by Lab 2:

- Minimal dylib injector
- Obj-C method interceptor
- Runtime tracer
- Live return-value patcher

All of these are implemented in the `runtime-injection/` folder.

---

## 1) New Apple-specific terms (defined explicitly)

### Mach-O (from Lab 1, repeated here)
Mach-O is Apple’s executable format. Everything we inject or load is a Mach-O
file.

### dylib
A **dylib** is a dynamic library on Apple systems. It is like a shared library:
code that can be loaded into a running process. A dylib is itself a Mach-O
file with a special file type.

### dyld
**dyld** is Apple’s dynamic loader. It is the runtime component that maps
Mach-O files into memory and resolves their dependencies. It is responsible for
loading dylibs into a process.

### Objective-C runtime (Obj-C runtime)
The **Objective-C runtime** is the system library that implements the
Objective-C language’s dynamic features. It owns the class registry, method
lookup, and message dispatch. It allows us to replace method implementations at
runtime.

### Selector (SEL)
A **selector** is the runtime representation of a method name. In Objective-C,
methods are looked up by selector (not by direct function pointer).

### IMP
An **IMP** is a function pointer to a method implementation. If you replace an
IMP, you effectively replace the method’s code at runtime.

### Message dispatch (`objc_msgSend`)
Objective-C does not call methods directly. Instead, it sends a **message** to
an object using `objc_msgSend`. The runtime looks up the selector in the
object’s class and then calls the corresponding IMP. If you change the IMP, you
change what happens when the message is sent.

**What you should understand after this section:** dyld loads dylibs; the
Objective-C runtime lets us replace methods at runtime by swapping IMPs.

---

## 2) Lab 2 architecture (what we built)

We created three small programs:

1) **target** (`target.c`)
   - A minimal process that creates an Objective-C class at runtime and calls
     a method on it.

2) **hook** (`hook.c` compiled into `hook.dylib`)
   - A dylib that replaces the method implementation, logs calls (tracing), and
     changes the return value (patching).

3) **injector** (`injector.c`)
   - A minimal launcher that starts the target with a dylib injected using
     `DYLD_INSERT_LIBRARIES`.

This gives us:

- **Injection**: the dylib is injected at process start.
- **Interception**: the dylib replaces a method’s IMP.
- **Tracing**: the replacement logs inputs/outputs.
- **Patching**: the replacement changes the return value.

**What you should understand after this section:** these three pieces are the
minimal system to gain runtime control of a process using dyld + Obj-C runtime.

---

## 3) How injection works on Apple (in plain terms)

On Apple systems, dyld decides which dylibs to load when a process starts. One
supported mechanism is an environment variable called:

- `DYLD_INSERT_LIBRARIES`

If this variable is set, dyld loads the listed dylibs *before* running the
program’s `main` function.

In our injector:

- We spawn the target program.
- We add `DYLD_INSERT_LIBRARIES=<path-to-hook.dylib>` to the child’s
  environment.
- dyld loads the hook dylib before the program starts.

**Important limitation:**

- Apple blocks this for system-protected or “hardened” binaries. This is
  enforced by code signing and SIP (System Integrity Protection). That is why
  we inject into our own local test program, not into system binaries.

**What you should understand after this section:** the minimal injection path
is to start a process with DYLD_INSERT_LIBRARIES so dyld loads your dylib.

---

## 4) The target program (why it exists and what it does)

The target program is a simple executable that creates an Objective-C class at
runtime (using C-only code). This avoids requiring Objective-C source files
while still exercising the Obj-C runtime.

Key steps in `target.c`:

1) **Create a class** using `objc_allocateClassPair`.
2) **Add a method** named `compute:` with an IMP (`compute_impl`).
3) **Register the class** so it exists in the runtime.
4) **Create an instance** and call the method using `objc_msgSend`.

We also look for an injected symbol called `hook_install`. If it exists (because
our dylib is injected), we call it before sending the message. This makes the
hook installation explicit and reliable.

**What you should understand after this section:** the target creates a known
method at runtime so the hook can replace it. The `hook_install` call provides
reliable timing for the demo.

---

## 5) The hook dylib (method interception + tracing + patching)

The injected dylib provides a function called `hook_install`:

- It finds the class `RTTarget` in the Obj-C runtime.
- It finds the method `compute:`.
- It replaces the method’s IMP with `hooked_compute`.

`hooked_compute` does three things:

1) Logs the input (`x`). (This is the runtime tracer.)
2) Calls the original implementation. (So we can see real behavior.)
3) Patches the return value to `1337`. (This is the return-value patcher.)

This demonstrates exactly how Obj-C method interception works:

- You are not patching machine code directly yet.
- You are swapping the function pointer the runtime uses to call the method.

**What you should understand after this section:** Objective-C methods are
indirect calls through the runtime; swapping an IMP is the core interception
mechanism.

---

## 6) The injector (minimal dylib injector)

The injector is a very small launcher:

- It checks that the dylib and target are accessible.
- It sets the DYLD environment variables for the child.
- It spawns the target process with those variables.

This is not “inject into an already-running process.” It is *start-time
injection*. That is intentional because it is the simplest reliable form on
modern macOS without special entitlements.

**What you should understand after this section:** start-time injection via
dyld is the simplest way to load your code into a process on Apple systems.

---

## 7) What the output should look like (what we learn)

If you run the target normally, you should see:

- `compute_impl` called.
- result = 42.

If you run with the injector, you should see:

- The hook logging the call.
- The original method still running.
- The returned value patched to 1337.

This demonstrates that:

- Your code is running inside the target process.
- You can intercept method calls.
- You can observe and modify behavior at runtime.

**What you should understand after this section:** the injected dylib has full
control over Obj-C method calls in the process.

---

## 8) Build and run (macOS)

From `runtime-injection/`:

```
make
./target
./injector ./hook.dylib ./target
```

Expected behavior:

- Without injection, result is 42.
- With injection, result is 1337 and hook logs appear.

---

## 9) Arch Linux support (build-only)

The code builds on Arch, but the injection is not functional there because:

- There is no dyld on Arch.
- There is no Apple Objective-C runtime by default.

So on Arch you can compile, but you cannot run the injection demo. This is
intentional to keep the repo cross-platform and allow editing/building on Arch.

**What you should understand after this section:** Lab 2 runtime behavior
requires macOS, but the codebase remains buildable on Arch.

---

## 10) Lab 2 completion checklist

- Minimal dylib injector: done (`injector`)
- Obj-C method interceptor: done (`hook_install` + IMP swap)
- Runtime tracer: done (hook logs inputs/outputs)
- Live return-value patcher: done (return 1337)

Lab 2 is complete.
