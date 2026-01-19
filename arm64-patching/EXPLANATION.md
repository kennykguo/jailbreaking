# Lab 3 Explanation - ARM64 Inline Patching (Very Verbose)

This document explains Lab 3 from first principles and defines every Apple-
specific term the first time it appears. It assumes you know general operating
systems concepts but are new to Apple’s ecosystem.

Lab 3 goal: control instruction-level execution flow on Apple ARM64 by
rewriting machine code *in memory*. This is the foundation for hot-patching and
runtime instrumentation.

Artifacts required by Lab 3:

- ARM64 trampoline generator
- Prologue patcher
- Branch redirector
- Live hot-patch engine

All of these are implemented in `arm64-patching/`.

---

## 1) Apple + ARM64 terms (defined explicitly)

### ARM64 (AArch64)
ARM64 is the 64-bit ARM instruction set used by Apple silicon (A-series,
M-series). Apple iOS devices and modern macOS machines execute ARM64 machine
code.

### Mach-O (from Lab 1, repeated)
Mach-O is Apple’s executable format. On Apple silicon, Mach-O contains ARM64
code.

### dyld (context only)
dyld is Apple’s dynamic loader. In Lab 3 we are not using dyld directly, but
we are modifying code that dyld has already mapped into memory.

### Apple runtime and security terms used later (quick definitions)

These terms appear later in this document. They are Apple-specific and are
defined here so you can refer back while reading the run logs.

### Mach (the kernel subsystem Apple uses for processes and memory)
Mach is the microkernel subsystem inside Apple’s kernel (XNU). It defines
tasks (processes), threads, messages, and virtual memory operations. When you
see `mach_*` APIs, you are interacting with Mach’s kernel services.

### XNU / Darwin
XNU is Apple’s kernel (the “X is Not Unix” kernel). Darwin is the open-source
core of macOS/iOS that includes the XNU kernel and userland basics.
The `uname -a` output shows “Darwin … arm64,” which identifies the OS kernel.

### Code signing (and ad hoc / linker-signed)
Apple requires executables to be code signed. The **CodeDirectory** is the
data structure in the binary that stores hashes of code pages.
- **ad hoc** means a local signature with no identity or certificate chain.
- **linker-signed** means the linker emitted the signature at build time.
These affect what kinds of memory changes the kernel allows at runtime.

### W^X (Write XOR Execute)
W^X is Apple’s policy that a memory page cannot be writable and executable at
the same time. It is enforced by the kernel and impacts JIT and patching.

### JIT and MAP_JIT
JIT means “just-in-time compilation,” i.e., generating code at runtime.
`MAP_JIT` is an Apple-specific `mmap` flag that marks a region as JIT code so
the kernel allows controlled write/execute transitions.

### pthread_jit_write_protect_np
An Apple-specific API that toggles whether JIT pages are writable. You must
enable writes before copying code bytes and disable writes afterward.

### mach_vm_protect and VM_PROT_COPY
`mach_vm_protect` is the Mach API to change page protections in a task.
`VM_PROT_COPY` asks the kernel to give you a private copy of the page
(copy-on-write) so you can modify it without altering the original signed
code page.

### sys_icache_invalidate
Apple’s API to flush the CPU instruction cache for a memory range. Without
this, the CPU might keep executing stale instructions.

### EXC_BAD_ACCESS (Mach exception)
An Apple/Mach crash reason meaning the CPU tried to access memory in an
illegal way (bad permissions or invalid address). LLDB reports this when a
process faults.

### LLDB
Apple’s debugger. It runs a program under control and can inspect registers,
memory, and call stacks at the exact point of a crash.

### Copy-on-write (COW)
Copy-on-write is a memory optimization where multiple mappings share the same
physical page until one of them tries to write. At that moment, the kernel
creates a private copy for the writer. In our case, `VM_PROT_COPY` asks the
kernel to give us a private copy of a code page so we can safely modify it
without mutating the original signed page.

### CodeDirectory flags
The **CodeDirectory** is a structure inside a code-signed Mach-O that lists
hashes for code pages and includes flags describing how it was signed.
When you see `flags=0x20002(adhoc,linker-signed)` it means:
- **adhoc**: signed without an identity (no certificate chain).
- **linker-signed**: the linker produced the signature during the build.
These flags matter because the kernel uses the CodeDirectory to enforce
what kinds of runtime code modifications are allowed.

### dyld`start`
`dyld` is the dynamic loader. The `dyld` function named `start` is the entry
point of the dynamic loader itself. It runs before your program’s `main`.
When a backtrace shows `dyld`’s `start`, it just means this is the earliest
userland frame below your code.

### Mach task vs thread
In Mach terminology:
- A **task** is a process (an address space plus resource accounting).
- A **thread** is a schedulable execution context inside a task.
`mach_task_self()` returns a handle to the current task so you can operate on
your own virtual memory mappings.

### `mprotect` vs `mach_vm_protect`
Both change memory protections:
- `mprotect` is the POSIX API and operates in the process address space.
- `mach_vm_protect` is the Mach API and offers Apple-specific flags like
  `VM_PROT_COPY`. We use `mach_vm_protect` on macOS to respect code signing.

### `mmap`, `MAP_ANON`, `MAP_PRIVATE`
`mmap` creates a new virtual memory mapping.
- `MAP_ANON` (aka `MAP_ANONYMOUS`) means the mapping is not backed by a file.
- `MAP_PRIVATE` means copy-on-write behavior if the mapping is shared.
On macOS, we add `MAP_JIT` to allow executable pages for JIT/patching.

### `PROT_READ`, `PROT_WRITE`, `PROT_EXEC`
These are the standard page permission bits:
- `PROT_READ`: the CPU may read from the page.
- `PROT_WRITE`: the CPU may write to the page.
- `PROT_EXEC`: the CPU may execute instructions from the page.
W^X means you should not have `PROT_WRITE` and `PROT_EXEC` at the same time.

### Page size (4 KB vs 16 KB)
The **page size** is the unit of memory protection. macOS on ARM64 commonly
uses 16 KB pages. That means any protection change applies to a 16 KB region,
not just 4 KB. This matters for patching because two functions can share the
same 16 KB page even if they are 4 KB apart.

### ASLR (Address Space Layout Randomization)
ASLR randomizes the base addresses of code and data at program launch to make
exploitation harder. This is why function addresses like `0x100008000` can
change between runs. We always compute branch offsets at runtime to handle
ASLR.

### ARM64 `B` instruction and `imm26`
The ARM64 unconditional branch instruction `B` encodes a 26-bit signed offset
(`imm26`) from the current instruction. The offset is measured in 4-byte
units, so the actual byte delta is `imm26 * 4`.
This is why our encoder checks alignment and range before writing the patch.

### Instruction cache and `sys_icache_invalidate`
Modern CPUs keep a separate instruction cache. When you write new code bytes,
the CPU might still execute the old cached instructions. `sys_icache_invalidate`
flushes the instruction cache for the modified range so the new instructions
take effect immediately.

### Mach exceptions and `EXC_BAD_ACCESS` code=2
Mach exceptions are how the kernel reports crashes to debuggers. `EXC_BAD_ACCESS`
means the CPU tried to access memory in a way that was not allowed. The
subcode `code=2` typically indicates a protection fault (permission issue),
not an unmapped address. In our case, this matched pages losing execute
permission while the CPU was still running code there.

### SIGBUS / “bus error”
“Bus error” is the Unix signal name for a hardware access fault. On macOS,
this often corresponds to a Mach exception like `EXC_BAD_ACCESS`. It usually
means the CPU tried to access memory with invalid permissions or alignment.

### What `mmap: Permission denied` means
This error indicates the kernel refused a memory mapping request. On macOS
ARM64, requesting an RWX mapping without `MAP_JIT` typically fails due to W^X
policy. That is why the trampoline allocation must use `MAP_JIT`.

### What `mprotect: Permission denied` means
This error indicates the kernel refused a protection change on a page. For
code pages in signed binaries, macOS does not allow arbitrary writable/executable
transitions. Using `mach_vm_protect` with `VM_PROT_COPY` requests a private,
writable copy instead.

### `getconf PAGESIZE`
`getconf` is a Unix tool that queries system configuration values. `PAGESIZE`
returns the OS memory page size in bytes (e.g., 16384 on Apple ARM64). We use
this to avoid assuming 4 KB pages.

### LLDB commands used
These are the LLDB commands we used and what they do:
- `target create <path>`: loads the executable into LLDB.
- `run`: starts the program.
- `breakpoint set -n <symbol>`: sets a breakpoint on a function by name.
- `register read x0 x1 x2`: prints register values (function arguments).
- `memory read --format x --size 4 --count 2 <addr>`: shows raw instruction bytes.
- `finish`: runs until the current function returns (so we can inspect after).
- `bt`: prints a backtrace (call stack).

### `nm` and symbol addresses
`nm` is a tool that prints symbol addresses from a binary. We used it to show
where `target_function`, `hook_function`, and `arm64_hotpatch` live in memory.
This helped prove whether they shared the same page and whether the branch
target was correct.

### Function prologue
The **prologue** is the first few instructions of a function. It typically
sets up the stack frame and saves registers. By overwriting the prologue, we
redirect control flow before the function body executes.

### Hook function vs target function
The **target function** is the function we want to intercept.
The **hook function** is the replacement we want to run instead. In Lab 3,
`target_function` is the original, and `hook_function` is the replacement.

### Trampoline (in more detail)
The trampoline is a tiny code stub that contains the original bytes we
overwrote plus a branch back to the original function after those bytes.
It lets a hook call the "real" function even after we redirect the prologue.

### Inlining and `__attribute__((noinline))`
Inlining is a compiler optimization where a function call is replaced by the
function body directly at the call site. That removes the real call and makes
patching the function entry ineffective. `__attribute__((noinline))` tells
the compiler to keep a real call so the prologue patch can intercept it.

### `static` functions and why it matters
`static` at file scope makes a function visible only inside that file. The
compiler can be more aggressive about inlining and reordering because it knows
no external code will call it. That is why `static` functions often need
`noinline` for patching demos.

### AArch64 calling convention (why `x0` matters)
On ARM64, the first function argument is passed in register `x0`, the second
in `x1`, and so on. In `arm64_hotpatch`, `x0` holds the address we will patch,
`x1` points to the bytes we will write, and `x2` holds the length.

### Instruction alignment on ARM64
ARM64 instructions are always 4 bytes and must be 4-byte aligned. If a branch
target is not 4-byte aligned, it is invalid. Our encoder checks alignment and
rejects unaligned branches.

### Branch range limits
The ARM64 `B` instruction has a signed 26-bit offset. That means it can reach
roughly +/-128 MB from the current instruction. If the target is farther, you
must use a different sequence (e.g., load a full address into a register).

### Object files and linking
Compiling produces `.o` object files (`patch_demo.o`, `patcher.o`). Linking
combines those into the final executable (`patch_demo`). Link-time decisions
affect layout, which matters for patching and page alignment.

### `-O2` and layout changes
`-O2` enables many optimizations that can change function placement and
inlining decisions. That is why we explicitly control alignment and inlining.

### Page alignment attributes
`__attribute__((aligned(PATCHER_PAGE_SIZE)))` tells the compiler and linker to
place a symbol at a page boundary. This is how we ensure the patching code and
the target code do not share a page.

### `arm64_encode_b` and branch encoding math (explicit)
The ARM64 `B` instruction encodes a signed **26-bit immediate** called
`imm26`. The hardware interprets it like this:

```
target_address = current_instruction_address + (imm26 << 2)
```

That means:
- The offset is measured in 4-byte units (`<< 2`).
- The offset range is about ±128 MB (`2^25 * 4`).

In our code:

```
intptr_t delta = (intptr_t)dst - (intptr_t)src;
if ((delta & 0x3) != 0) return -1;
intptr_t imm26 = delta >> 2;
if (imm26 < -(1 << 25) || imm26 > ((1 << 25) - 1)) return -1;
uint32_t insn = 0x14000000u | ((uint32_t)imm26 & 0x03FFFFFFu);
```

`0x14000000` is the base encoding for the unconditional branch instruction.
We OR in the 26-bit immediate.

### Why `arm64_hotpatch` flushes the instruction cache
When we write new bytes to memory, the CPU’s instruction cache might still
contain the old instructions. If we do not flush, the CPU can keep executing
the stale code even though memory now contains new bytes.

On macOS we use:
`sys_icache_invalidate(addr, len)`

On GCC/Clang we use:
`__builtin___clear_cache(...)`

### `mach_vm_protect` vs `vm_protect`
`mach_vm_protect` is the 64-bit capable Mach API. It takes 64-bit addresses
and sizes (`mach_vm_address_t`, `mach_vm_size_t`) and is preferred on modern
systems. `vm_protect` is the older API that uses 32-bit types on some
platforms. We use `mach_vm_protect` to avoid truncation on 64-bit addresses.

### `mach_task_self()`
`mach_task_self()` returns the Mach task port for the current process. A
Mach “task port” is a capability that authorizes operations on that task’s
memory. We pass it to `mach_vm_protect` to change protections on our own pages.

### What “linker-signed” implies
When the linker produces the signature, the kernel still expects code pages
to match the signature at load time. Copy-on-write (`VM_PROT_COPY`) is the
supported way to create writable private pages without violating the original
signature.

### Why we used `MAP_JIT` only for the trampoline
The trampoline is *new* executable memory created at runtime, so it must be
created with `MAP_JIT`. The target function is existing code already mapped
by dyld, so we do not re-map it; we only change its protections.

### Why we do not use an absolute jump
We use a single `B` instruction because the hook is in the same binary and
within ±128 MB, which fits the `B` range. If the hook were farther away, we
would need a longer sequence (load a 64-bit address into a register and
branch via `BR`).

### Why we align both `target_function` and `arm64_hotpatch`
We align both to avoid sharing a page. The patching code must never remove
execute permission from the page it is currently executing. Aligning both
symbols to page boundaries ensures they land on distinct pages even after the
linker lays out the final binary.

### Why we check page size at runtime
Even if we compile with a detected page size, a mismatch could happen if the
binary is moved to a system with a different page size. The runtime check
prevents silent corruption by forcing a clear error if the assumption is wrong.

### Why `arm64_hotpatch` refuses to patch its own page
If the patch target shares a page with `arm64_hotpatch`, we would remove
execute permission from the page while still running inside it, which causes
`EXC_BAD_ACCESS`. The explicit check prevents this class of failure.

### Why the trampoline copies exactly one instruction
For the demo, we only need to overwrite the first instruction with a branch.
Copying one instruction keeps the trampoline simple. Real patching systems
decode multiple instructions and relocate any PC-relative ones.

### What “PC-relative” means
PC-relative instructions compute addresses based on the current program
counter (PC). If you copy them elsewhere, they compute the wrong address. That
is why proper trampolines need relocation logic.

### Why `-O2` can change function addresses
The optimizer can reorder functions, inline code, and remove unused symbols.
This changes where functions land in the binary and which calls exist. We
control this with alignment and `noinline` so the patch remains meaningful.

### Why `static` affects visibility and patching
Because `static` functions are not visible outside the file, the linker can
perform more aggressive optimizations. For patching demos, that means you must
force a real call if you want to intercept the function entry.

### Why `VM_PROT_COPY` is safe for code signing
Code signing validates the original file-backed code pages. `VM_PROT_COPY`
creates a private copy so modifications do not alter the original signed page.
The kernel allows this because the signed code remains intact; only your
process sees the modified copy.

### Why “bus error” stopped after alignment fixes
The original crash was caused by executing in a page that had execute
permission removed. Once `arm64_hotpatch` moved to a separate page, the CPU
never executed from a non-executable page during patching, so the bus error
stopped.

### Why `test_patch_demo.sh` is silent on success
The test script uses `grep` checks and prints nothing when all checks pass.
A non-zero exit indicates failure. Silence means success.

### What the hook proves in Lab 3 (conceptual)
Seeing `[hook] hook_function called...` after the patch shows that we have
successfully redirected control flow at the machine-instruction level. That is
the core capability needed for runtime instrumentation (e.g., tracing every
call to a function without modifying source code).

### Why this lab is “inline patching,” not symbol interposition
Symbol interposition (like `DYLD_INSERT_LIBRARIES`) replaces symbols at link
or load time. Inline patching modifies the already-loaded instructions. That
means it works even if symbols are resolved or stripped, and it can target any
instruction address, not just named functions.

### What “hot-patching” means
Hot-patching means applying patches while the program is running, without
restarting it. In our demo, we patch the function while the program is alive
and immediately observe changed behavior.

### Why we use a demo program instead of a real system binary
macOS system binaries are often protected by hardened runtime and SIP, which
block injection and code modification. A local demo binary is under our
control and is the safest place to validate the mechanics.

### Where this leads (next lab)
Lab 3 establishes the primitive we need for Lab 4 (system-wide
instrumentation). Once we can safely patch instructions, we can build
automated patchers and tracers that scale across many functions or processes.

### What “hardened runtime” means (Apple-specific)
Hardened runtime is an Apple security policy enforced on signed binaries. It
restricts behaviors like injection, debugging, and JIT unless explicitly
entitled. This is why we avoid system binaries during these labs.

### What SIP (System Integrity Protection) means
SIP is Apple’s kernel-level protection system that prevents even root
processes from modifying certain system resources and processes. It blocks
many forms of code injection and memory patching in protected processes.

### Why we still can patch our own process
We are patching a self-built, ad hoc signed binary in user space. The kernel
allows these kinds of modifications within the process because they do not
violate protected system policy.

### What “dyld shared cache” is (Apple-specific)
On macOS, many system libraries are not loaded from individual files at
runtime. Instead, they are packed into a shared cache file called the **dyld
shared cache**. This speeds up launches. It matters for patching because
system code may come from the shared cache, and system protections can be
stricter there than for your own binaries.

### What “AMFI” means (Apple-specific)
AMFI stands for Apple Mobile File Integrity. It is the subsystem that enforces
code signing and executable integrity across Apple platforms. AMFI is the
reason code pages are protected and why the kernel refuses certain memory
permission changes.

### What “entitlements” are
Entitlements are special permissions embedded in a code signature. They grant
privileges like debugging other processes, JIT, or accessing restricted
resources. Without the correct entitlements, the kernel denies certain
operations.

### What “task port” access implies
To modify another process, you need that process’s task port (a Mach
capability). On modern macOS, getting a task port for other processes is
restricted by SIP, AMFI, and entitlements. That is why Lab 3 only patches the
current process.

### What “JIT entitlement” means
On iOS and some macOS contexts, using JIT may require the
`com.apple.security.cs.allow-jit` entitlement. Our local macOS demo works
without it because it is a developer-controlled binary, but system apps may
require explicit entitlements.

### What “ABI” means in this context
ABI is the Application Binary Interface: calling conventions, register usage,
stack layout, and binary formats. Inline patching must respect the ABI because
we are redirecting execution at the machine-code level.

### Why we use C instead of Objective-C here
Lab 3 is about raw instruction-level control. C keeps the demo small and
predictable. Objective-C would add runtime indirection and complicate the
patching story at this stage.

### What “hooking” means at the machine level
Hooking means diverting control flow so the CPU executes your code instead of
the original. In Lab 3, the hook is a branch at the function entry that sends
execution to `hook_function`.

### Why we print outputs instead of using a debugger only
Printed output gives us a direct, deterministic trace of control flow without
depending on debugger state. It is also the simplest way to validate the
patched path in automated tests.

### What “XNU VM protections” means
XNU’s virtual memory subsystem enforces page permissions and handles faults.
When we call `mprotect` or `mach_vm_protect`, we are requesting changes to the
VM map that XNU enforces at the hardware level via page tables.

### What “page tables” are (minimal)
Page tables are CPU-managed data structures that map virtual addresses to
physical memory and include permission bits (read/write/execute). When we
change protections, the kernel updates these tables.

### Why permissions are enforced at the CPU level
Even if user code tries to execute a write-protected page, the CPU will raise
an exception. The kernel then reports it as `EXC_BAD_ACCESS`. This is why
incorrect permissions cause immediate crashes.

### Why “kernel authority” matters for patching
User processes cannot arbitrarily change all memory permissions. The kernel
decides whether a requested change is allowed (e.g., for code-signed pages).
This is why our patching code must follow Apple’s allowed paths.

### What “ASLR slide” means (Apple-specific detail)
macOS randomizes the load address of the main executable by applying a
“slide” (offset) at runtime. Symbol addresses like `0x100008000` already
include this slide. That is why addresses change between runs.

### Why we patch at runtime instead of editing the binary
Editing the binary on disk would require re-signing and is static. Inline
patching lets us change behavior of a running process immediately without
modifying the file on disk.

### What “instruction boundary” means
An instruction boundary is the start address of a CPU instruction. On ARM64,
all instruction boundaries are 4 bytes apart. Overwriting at non-boundaries
corrupts execution.

### Why the trampoline needs executable permissions
The trampoline is code that the CPU will execute. It must be mapped with
execute permission (and toggled writable only when updating it).

### What “dyld mapped the code” means (Apple-specific)
When a macOS program starts, dyld maps the Mach-O segments from disk into the
process address space. Code segments are mapped as read+execute. We are
modifying those in-memory mappings, not the file on disk.

### What “segment” means in Mach-O
A segment is a large region of virtual memory described in the Mach-O header
(e.g., `__TEXT`, `__DATA`). The code we patch lives in the `__TEXT` segment.

### Why `__TEXT` is read+execute
The `__TEXT` segment contains code and read-only data. It is mapped RX to
prevent accidental or malicious writes.

### What “text relocations” are (and why we avoid them)
Text relocations are runtime modifications to code sections. Apple discourages
them because they break code signing assumptions. Our patching uses
copy‑on‑write to avoid modifying the original signed code page.

### What “Mach-O code page hash” implies
Code signing stores hashes of each code page. If the original page is modified
in place, the hash no longer matches and the kernel may kill the process. That
is why `VM_PROT_COPY` matters: it keeps the original page intact.

### What “page-aligned symbol” means in practice
The linker chooses where functions live. The alignment attribute forces the
linker to start the function on a page boundary so it occupies a predictable
page.

### Why we do not patch the hook function
We only need to redirect execution to the hook; the hook itself can remain
unaltered. Patching only the target minimizes the number of modified pages.

### Why the demo uses a single thread
Multi-threaded patching is harder because another thread could execute the
target while it is being modified. The demo is single-threaded to avoid
race conditions.

### What “race condition” means here
A race condition would occur if one thread executes the function while another
thread is in the middle of overwriting its first instruction. That could lead
to crashes or inconsistent behavior.

### What “instruction atomicity” means
ARM64 writes to aligned 4-byte words are atomic at the instruction level. We
write exactly one aligned instruction so no thread can see a half-written
instruction.

### Why we patch only one instruction
Writing one instruction is the smallest possible change and avoids having to
handle multiple instruction boundaries or partial writes. It also reduces the
chance of races in a simple demo.

### Why we flush the cache *after* writing
The CPU must see the new instruction bytes. We flush after writing so the CPU
does not execute stale instructions.

### What “branch target” means
The branch target is the address we want execution to jump to. In Lab 3, this
is `hook_function`.

### What “instruction stream” means
The instruction stream is the sequence of bytes the CPU fetches and decodes
as instructions. Inline patching changes this stream in memory.

### Why we avoid deeper disassembly here
A full patching system must decode instructions and handle relocations. That
is out of scope for Lab 3, so we keep the demo minimal and document the
limitations clearly.

### What “instruction relocation” means (in more detail)
When you copy instructions into a trampoline, any instruction that encodes a
PC-relative address (like a branch or load literal) will point to the wrong
location unless you adjust it. That adjustment is called relocation. Correct
relocation requires decoding the instruction, understanding its semantics,
and re-encoding it for the new location. We avoid this complexity by copying
only one instruction in a controlled demo.

### What a “literal load” is (ARM64 example)
ARM64 has instructions like `LDR Xt, [PC, #imm]` that load a value from a
literal pool near the instruction. If you move that instruction, the literal
pool address changes, so the load would read the wrong data. This is a common
reason why trampolines need relocation.

### What “dyld shared cache mapping” implies
When a library is in the shared cache, its code pages are mapped from a shared
file into many processes. Modifying those pages is heavily restricted because
it would affect other processes. That is why system-level patching requires
privileged techniques and is out of scope here.

### Why we use a local binary for deterministic addresses
System binaries can be updated and re-signed by Apple, and their internal
layout can change across OS versions. A local binary built from our sources
gives deterministic structure and behavior, which is crucial for learning.

### What “instruction cache coherence” means
Instruction cache coherence refers to the CPU seeing the updated instructions
after a write. On some architectures, you must explicitly invalidate the
instruction cache. ARM64 requires this for self-modifying code. That is why
`sys_icache_invalidate` is mandatory after patching.

### What “data cache” vs “instruction cache” means
Modern CPUs have separate caches for data and instructions. Writing to memory
updates the data cache, but the instruction cache can still contain old code.
Flushing the instruction cache resolves this mismatch.

### What “self-modifying code” means
Self-modifying code is any program that changes its own instructions in
memory. Inline patching is a controlled form of self-modifying code.

### Why we keep the demo single-file and minimal
Each extra dependency adds noise. This lab is about the mechanics of patching,
so we keep the code small and explicit to make the flow easy to trace.

### What “hot-patch engine” would look like in a larger system
A real hot-patch engine would include:
- A disassembler to understand instruction boundaries.
- Relocation logic for PC-relative instructions.
- Thread suspension to avoid races.
- Permission management and cache flushing.
Our `arm64_hotpatch` is the smallest viable subset.

### Why we avoid multi-instruction patching here
Patching multiple instructions increases risk: you must preserve function
prologues correctly and handle partial updates. The single-instruction patch
keeps the demo focused and safe.

### Why we did not use function pointers or PLT/GOT hooks
Function pointers and PLT/GOT hooks operate at a higher level (symbol
resolution). Lab 3 intentionally stays at the raw instruction level to show
how to redirect control flow without relying on symbol indirection.

### What “exception level” means on ARM64
ARM64 defines privilege levels called Exception Levels (EL0–EL3). User-space
code runs at EL0. The kernel runs at EL1. When a user process triggers a
fault (like executing non-executable memory), the CPU traps to EL1 and the
kernel reports an exception (e.g., `EXC_BAD_ACCESS`).

### What “user space” vs “kernel space” means on macOS
User space is where normal applications run, with limited privileges. Kernel
space is where the OS kernel runs with full hardware privilege. Inline
patching in this lab is strictly in user space; we are not modifying the
kernel.

### What “VM map” means
The VM map is the kernel’s internal structure that tracks all memory regions
for a process: their addresses, sizes, and permissions. `mprotect` and
`mach_vm_protect` update entries in this map.

### What “code region” vs “data region” means
Code regions contain executable instructions (`__TEXT`). Data regions contain
mutable data (`__DATA`). Code regions are RX; data regions are RW. Inline
patching temporarily alters a code region.

### What “copy of a page” means in VM_PROT_COPY
When `VM_PROT_COPY` is used, the kernel makes a private copy of the page for
the current process. The original page (which is backed by the on-disk binary)
remains unchanged. The process now sees its own modified version.

### What “ad hoc signature” implies for experimentation
An ad hoc signature allows the binary to run but does not grant special
entitlements. That’s why our demo can run but cannot bypass system protections
for other processes.

### Why instruction patching works even if symbols are stripped
Symbols are for humans and debuggers. The CPU executes addresses. Inline
patching modifies addresses directly, so symbol stripping does not stop it.

### Why we log outputs at each step
We log outputs to observe control flow transitions without relying on
disassembly or debugger-only visibility. This keeps the learning loop clear.

### Why this lab is safe on macOS but dangerous on production software
Inline patching can destabilize a process if applied to the wrong address or
without proper synchronization. We constrain it to a controlled demo binary
to learn the mechanics safely.

### Why we avoid patching system processes
System processes are protected by SIP and hardened runtime policies and may
have additional integrity checks. Attempting to patch them would fail or risk
system instability.

### What “fault registers” are (ARM64 detail)
When an exception occurs, the CPU records details in registers like:
- `ESR_EL1` (Exception Syndrome Register): describes the type of fault.
- `FAR_EL1` (Fault Address Register): stores the faulting address.
LLDB abstracts this and reports `EXC_BAD_ACCESS` with the address.

### What “ASLR slide” means for branch encoding
ASLR shifts the base address of the binary at runtime. That means absolute
addresses change each run, but **relative offsets** between functions in the
same binary stay constant. Our branch encoding uses the relative offset, so it
remains correct despite ASLR.

### What “instruction pipeline” means (minimal)
CPUs fetch, decode, and execute instructions in a pipeline. When we modify
code, the pipeline can contain old instructions. Flushing the instruction
cache ensures the pipeline fetches new bytes.

### What “execute-only memory” means (context)
Some systems support execute-only pages that cannot be read. Apple has explored
XOM in some contexts. If a page were execute-only, reading its bytes would
fault. Our demo assumes code pages are readable, which is true for the local
binary.

### Why `sys_icache_invalidate` is Apple-specific
Apple provides `sys_icache_invalidate` in `libkern` as the supported way to
flush instruction caches for self-modifying code. On other platforms,
different APIs exist.

### Why “bus error” can appear instead of “segmentation fault”
On macOS, the Unix signal for certain protection faults is SIGBUS (bus error)
instead of SIGSEGV. Both indicate invalid memory access, but SIGBUS is often
used for alignment or protection errors.

### What `__attribute__` does in C
`__attribute__` is a compiler extension that lets you control alignment,
inlining, and other code-generation details. We use it to enforce page
alignment and to disable inlining where needed.

### Why we avoid rewriting more than one instruction in the demo
Multiple-instruction patches require carefully managing instruction boundaries
and possibly rewriting PC-relative instructions. The one-instruction patch is
the smallest safe unit for a teaching demo.

### What “symbol visibility” means (context)
Symbol visibility controls whether a function name is exposed outside the
object file. `static` functions have internal visibility. This affects
debugging and inlining but not the CPU’s ability to execute the code.

### Why LLDB shows `dyld` in the backtrace
The backtrace includes `dyld` because `dyld` starts your program before `main`.
Seeing it in the stack does not mean dyld caused the crash; it is just the
bottom frame.

### What “XNU VM map internals” refers to (high-level)
Internally, XNU tracks memory regions in a VM map structure. Each entry records
start/end addresses, protections, and backing objects (file or anonymous
memory). When we call `mach_vm_protect`, XNU updates the VM map entry for the
page we are modifying.

### What “AMFI enforcement pipeline” refers to (high-level)
AMFI integrates with code signing and the kernel’s VM system. When a process
tries to change protections or execute code, AMFI policies are consulted. If
the change would violate code-signing rules, the kernel denies the request.
That is why RWX mappings fail without `MAP_JIT`, and why `VM_PROT_COPY` is
needed for code pages.

### What “dyld shared cache format” refers to (high-level)
The dyld shared cache is a single file that contains many system libraries.
dyld maps portions of this cache directly into processes. Its internal format
is complex (multiple regions, rebasing, bindings). We do not parse it in Lab 3,
but it explains why system code behaves differently from local binaries.

### What “exception syndrome decoding” would show
On ARM64, the exception syndrome register (`ESR_EL1`) encodes the fault type,
access size, and cause. A full debugger could decode this into precise reasons
like “instruction abort due to permission.” We rely on LLDB’s `EXC_BAD_ACCESS`
summary instead.

### Why we stop at this depth
Lab 3’s goal is to build a working inline patching primitive, not to reverse
engineer Apple’s kernel. The current explanations cover all concepts needed
to understand and reproduce the behavior we observed.

### What “Mach ports” are (conceptual)
Mach uses capability-style handles called **ports** to grant rights to kernel
objects (like tasks and threads). A task port is required to manipulate a
process’s memory. In this lab, we only use `mach_task_self()` to get our own
task port.

### What “MIG” is (Mach Interface Generator)
Many Mach APIs are defined via MIG, which generates client/server stubs for
Mach messages. Functions like `mach_vm_protect` are wrappers around these
message calls. You don’t see the message passing directly, but it’s how the
kernel interface is implemented.

### Why `mach_vm_protect` needs a task port
All virtual memory operations are scoped to a task (process). The task port is
the capability that authorizes those operations. Without it, you cannot change
protections.

### What “RX page” means
RX means **read + execute**. Code pages are typically RX: you can read the
instructions and execute them, but you cannot write to them.

### What “RW page” means
RW means **read + write**. Data pages are typically RW: you can read and
write data but should not execute code from them.

### What “RWX” means (and why it is blocked)
RWX means a page that is readable, writable, and executable. Apple blocks this
because it makes code injection trivial. That is why the kernel denies RWX
`mmap` or `mprotect` in most cases.

### What “MAP_JIT + write toggle” actually enforces
`MAP_JIT` does not allow simultaneous write+execute. It allows a page to be
**temporarily** writable while a separate toggle is enabled, and otherwise
executable. This enforces a strict “write then execute” discipline.

### What “instruction fetch” means
Instruction fetch is the CPU reading bytes from memory as code. If a page is
not executable, instruction fetch will fault even if the bytes are readable.

### Why the branch overwrite is safe in this demo
We overwrite a single, aligned 4-byte instruction. ARM64 guarantees aligned
32-bit instruction stores are atomic, so no thread sees half an instruction.

### What “atomic” means here
Atomic means the write appears to happen all at once. For a single 4-byte
store on ARM64, other cores will not observe a partially written instruction.

### Why `hook_function` returns 1337
We chose a distinct value (1337) to make the behavior change obvious. Any
constant would work; the point is to make the patch visible in output.

### Why “PC-relative range” matters
If the hook were farther than ±128 MB, the `B` instruction could not reach
it. That would require a different patch strategy (e.g., loading a full
address into a register).

### What “literal pool” means (ARM64)
A literal pool is a region of data in code sections used by PC-relative load
instructions. Moving those instructions without relocating the pool breaks
their semantics.

### What “symbol stripping” means
Symbol stripping removes names from a binary to reduce size and hinder
analysis. Inline patching doesn’t rely on symbols; it operates on addresses.

### Why the demo uses printf for visibility
`printf` provides a clear, ordered trace of what code executed. It’s the
simplest observable side effect for a teaching lab.

### What “pointer authentication (PAC)” is (Apple-specific)
Pointer Authentication is an ARMv8.3+ security feature used by Apple on ARM64.
It signs pointers with a cryptographic tag so corrupted pointers are detected.
In some contexts, PAC can interfere with indirect branches or function
pointers if you do not handle the signature correctly. Our demo uses direct
branching with `B`, so PAC does not affect us here.

### What “branch target identification (BTI)” is (context)
BTI is an ARM security feature that restricts which targets are valid for
indirect branches. macOS uses BTI selectively. Because we use a direct `B`
branch and target a normal function entry, BTI is not an issue in this demo.

### What “code signing enforcement on pages” means
Code signing divides executable code into pages and stores hashes in the
CodeDirectory. If a signed page is modified in place, the hash no longer
matches. The kernel enforces this by rejecting certain permission changes or
killing the process if it detects invalid pages. `VM_PROT_COPY` avoids this by
creating a private copy for our process.

### What “dyld rebase and bind” means (high-level)
dyld performs **rebasing** (fixing up pointers that depend on where the binary
was loaded) and **binding** (resolving references to external symbols). These
operations happen at load time. Inline patching happens *after* this phase.

### What “PLT/GOT” means in other systems (context)
On ELF systems, PLT/GOT provide indirection for dynamic linking. Mach-O uses
different mechanisms (lazy binding stubs, symbol pointers). We avoid these in
Lab 3 to stay at the raw instruction level.

### What “lazy binding stubs” are (Apple-specific)
Mach-O binaries can use stubs that call into dyld the first time a symbol is
used, then patch a pointer for subsequent calls. This is a high-level
indirection mechanism, distinct from inline patching.

### Why “function alignment” affects branch targets
Alignment changes the exact address of a function, which changes the branch
offset. Because we calculate the offset at runtime, this is safe, but it is
why we re-check addresses after changes.

### Why we rely on `nm` for ground truth
`nm` shows the actual symbol addresses in the linked binary. This confirms
where functions landed and whether our branch target math matches reality.

### What “link-time layout” means
The linker decides the final addresses of functions and sections in the
binary. That layout determines page boundaries and branch offsets. Small code
changes can shift layout, which is why we validate addresses in debugging.

### What “PAC edge cases” could look like (context)
PAC signs return addresses and some function pointers. If you patch a return
address or use indirect calls with unsigned pointers, the CPU may fault. Since
we use a direct branch to a normal function entry, PAC does not interfere in
this lab.

### What “BTI landing pads” are (context)
BTI requires specific instructions at valid indirect branch targets. If you
jump indirectly to a target without a proper landing pad, the CPU can fault.
We do not use indirect branches here, so BTI is not triggered.

### What “Mach port rights” mean (high-level)
Mach ports have rights like send, receive, and send‑once. A task port gives
you the ability to operate on a task. Without the right port, the kernel
rejects operations. This is why patching other processes requires special
privileges.

### What “AMFI policy structures” refer to (high-level)
AMFI uses internal policy structures to decide whether code pages can be
modified or executed. These policies integrate with code-signing metadata and
entitlements. We stay inside the allowed policy for our own process.

### Why we didn’t explore these internals in Lab 3
They are kernel‑internal and vary across OS versions. Lab 3 focuses on
reliable, user‑space patching mechanics, which are already demonstrated.

### AMFI enforcement flow (high-level, concrete sequence)
At a high level, AMFI enforces:
1) **Load-time validation**: dyld maps code pages and the kernel checks the
   code signature’s CodeDirectory hashes.
2) **Runtime policy checks**: when a process requests a permission change
   (e.g., via `mach_vm_protect`), AMFI policies decide whether the transition
   is allowed.
3) **Execute-time checks**: if a page is modified in a way that violates
   signing policy, the process may be denied execution or terminated.
Our use of `VM_PROT_COPY` keeps the signed pages intact while giving our
process a private, writable copy.

### Hardened runtime details (why it matters)
The hardened runtime is a code‑signing option that enables stricter checks:
- Blocks `DYLD_INSERT_LIBRARIES` for protected binaries.
- Restricts JIT unless the `allow-jit` entitlement is present.
- Restricts debugging/injection without `get-task-allow`.
Our demo binary is ad hoc signed, not hardened, so we can modify our own pages.

### Entitlements used in practice (examples)
Common Apple entitlements related to injection/patching include:
- `com.apple.security.cs.allow-jit`
- `com.apple.security.cs.allow-unsigned-executable-memory`
- `com.apple.security.get-task-allow`
We do not require these for the local demo, but they matter for system apps.

### Mach VM map internals (deeper but still high-level)
The VM map tracks regions as entries (start/end, protections, backing object).
When we call `mach_vm_protect`, the kernel:
1) Locates the VM map entry covering the address.
2) Verifies the requested protection transition.
3) Splits the entry if only part of the region changes.
4) Updates page table permissions for the region.

### Mach port rights taxonomy (brief)
Rights include:
- **send**: ability to send messages to a port (most common).
- **receive**: ability to receive messages; effectively controls the port.
- **send‑once**: single‑use send right.
Task ports are highly privileged receive rights to a task object.

### dyld shared cache layout (high-level)
The shared cache contains:
- Multiple **regions** (text, data, linkedit).
- Pre‑rebased code to speed up launch.
- Shared mappings across processes.
Because it is shared, modifying those pages would affect multiple processes,
so macOS strongly restricts changes.

### Mach exception delivery (high-level)
When a fault occurs:
1) CPU traps into the kernel (EL1).
2) Kernel constructs a Mach exception message.
3) If a debugger is attached, it intercepts the exception.
4) LLDB reports the exception as `EXC_BAD_ACCESS`.

### ARM64 fault types (high-level)
Faults are categorized as:
- **instruction abort**: trying to execute non‑executable memory.
- **data abort**: invalid data access (read/write).
Our crashes were consistent with instruction aborts when execute permission
was removed from the active page.

### dyld vs kernel responsibility (clarified)
dyld maps binaries and resolves symbols. The kernel enforces memory
permissions. dyld cannot override kernel W^X policies; it must operate within
them.

### Why this level is sufficient for Lab 3
At this depth, you can:
- Predict which memory operations will be allowed.
- Diagnose common patching failures.
- Understand the constraints imposed by Apple’s security model.
Deeper kernel internals are not needed to build or reason about the Lab 3
artifacts.

**What you should understand after this section:** Lab 3 is about ARM64
machine code on Apple silicon, *after* it has been loaded into memory.

---

## 2) What “inline patching” means

**Inline patching** means modifying the machine code instructions *in place*
inside a live process. The code you patch is already mapped into memory and
may already be running. That is why it is powerful (and dangerous).

In practice, inline patching is done by:

1) Making the target code page writable.
2) Overwriting one or more instructions.
3) Flushing the CPU instruction cache so the new bytes take effect.

**What you should understand after this section:** inline patching is literal
runtime modification of instructions in memory.

---

## 3) Why ARM64 patching requires special care

ARM64 instructions are fixed-width (4 bytes). That simplifies patching because
instructions are aligned and you always write in 4-byte chunks.

However:

- A single instruction may reference PC-relative addresses.
- If you copy an instruction elsewhere (into a trampoline), it might break if
  that instruction depends on the original PC.

For this lab, we use a **minimal** trampoline approach that copies just one
instruction. This is good enough for a controlled demo, but real systems use
an instruction decoder and relocation logic.

**What you should understand after this section:** ARM64 patching is easy to
write but tricky to get fully correct without instruction relocation.

---

## 4) The four Lab 3 artifacts (in plain terms)

### 4.1 Branch redirector
A **branch redirector** overwrites a function’s first instruction with a branch
that jumps to a new address.

On ARM64, an unconditional branch (`B`) is encoded with a 26-bit immediate
offset. The offset is relative to the current instruction and is measured in
4-byte units.

We implement this in `arm64_encode_b` and `arm64_patch_b`.

### 4.2 Prologue patcher
A **prologue patcher** means: patch the *beginning* of a function (its
prologue) so control flow redirects immediately.

We implement this in `arm64_patch_prologue`, which:

1) Builds a trampoline (so the original code can still run).
2) Overwrites the first instruction with a branch to the hook.

### 4.3 Trampoline generator
A **trampoline** is a small block of code that contains the original
instruction(s) you overwrote, plus a branch back to the original function
after those instructions.

We implement this in `arm64_make_trampoline`:

- Copy N bytes of original instructions.
- Append a branch back to `original + N`.

This allows you to call the original function from your hook if you want to.

### 4.4 Live hot-patch engine
A **hot-patch engine** is the safe mechanism that actually writes bytes into
code memory. It must:

- Temporarily make the page writable (`mprotect`).
- Write the new bytes.
- Flush instruction cache.
- Restore original page protections.

We implement this in `arm64_hotpatch`.

**What you should understand after this section:** the four artifacts are
small pieces that together let you redirect and restore control flow safely.

---

## 5) Apple-specific memory protection rules (why “permission denied” happens)

On macOS ARM64, Apple enforces **W^X** (Write XOR Execute). This means a memory
page must not be writable and executable at the same time. Code pages are
usually **read + execute**. If you try to make them **read + write + execute**
in one step, the kernel denies it.

This affects two parts of our lab:

1) **Trampoline memory** (new executable code we allocate):
   - We must allocate it with `MAP_JIT` so macOS knows this is JIT-style code.
   - We must explicitly enable writes when we copy instructions, then disable
     writes again. That uses `pthread_jit_write_protect_np`.

2) **Existing code pages** (the function we patch in place):
   - We must temporarily remove execute permission while writing.
   - We use `mach_vm_protect` with `VM_PROT_COPY` so the kernel can give us a
     private copy of the page to modify, which is allowed under code signing.

If these rules are not followed, you will see:

- `mmap: Permission denied` when creating the trampoline.
- `mprotect: Permission denied` or similar when patching the prologue.

**What you should understand after this section:** macOS protects executable
memory. Inline patching must respect W^X and code-signing rules, so we use
`MAP_JIT`, `pthread_jit_write_protect_np`, and `mach_vm_protect` with
`VM_PROT_COPY`.

---

## 6) Findings and fixes from the failed run

We attempted to run the demo and saw two concrete failures:

1) `mmap: Permission denied` when allocating the trampoline.
2) `mprotect: Permission denied` when trying to make the target code page
   writable.

These failures tell us:

- macOS rejected an RWX allocation for new executable code.
- macOS rejected changing a signed code page to writable in-place.

Fixes applied:

- **Trampoline allocation** now uses `MAP_JIT` on macOS ARM64. This tells the
  kernel the allocation is intended for JIT-style code and is permitted when
  combined with explicit write toggling. We also added
  `pthread_jit_write_protect_np` calls to enable writing while copying bytes
  and disable writing afterward.

- **Inline patching** now uses `mach_vm_protect` with `VM_PROT_COPY` for the
  page that contains the target function. This requests a private copy of the
  code page that can be made writable. After patching, we restore RX.

Supporting fixes for building on macOS:

- Added missing CPU and VM constants to the vendored headers so the Apple
  SDK headers compile cleanly on both macOS and Arch:
  - `include/mach/machine.h`: `cpu_threadtype_t`, `CPU_THREADTYPE_*`,
    `CPU_STATE_MAX`.
  - `include/mach/vm_prot.h`: `VM_PROT_COPY`.

Optional verification:

- `arm64-patching/test_patch_demo.sh` checks for the expected output lines on
  macOS ARM64. It is a minimal regression test for the patch flow.

**What you should understand after this section:** the errors were caused by
Apple's executable-memory protections (W^X and code signing). The fixes respect
those rules while still enabling inline patching for the demo.

---

## 7) Additional failure on macOS ARM64 (bus error) and the root cause

After addressing W^X/JIT protections, the demo still crashed with a bus error.
Here is the exact crash output from LLDB:

```
Process 67313 stopped
* thread #1, queue = 'com.apple.main-thread', stop reason = EXC_BAD_ACCESS (code=2, address=0x100003b30)
    frame #0: 0x0000000100003b30 patch_demo`arm64_hotpatch + 128
patch_demo`arm64_hotpatch:
->  0x100003b30 <+128>: cbnz   w0, 0x100003b6c           ; <+188>
    0x100003b34 <+132>: mov    x0, x20
    0x100003b38 <+136>: mov    x1, x21
    0x100003b3c <+140>: mov    x2, x19
```

Backtrace:

```
* frame #0: 0x0000000100003b30 patch_demo`arm64_hotpatch + 128
  frame #1: 0x0000000100003d9c patch_demo`arm64_patch_prologue + 112
  frame #2: 0x0000000100003988 patch_demo`main + 104
  frame #3: 0x0000000186001d54 dyld`start + 7184
```

Environment confirmation:

```
Executable=/Users/kenny/jailbreaking/arm64-patching/patch_demo
CodeDirectory flags=0x20002(adhoc,linker-signed)
Darwin Kernel Version 25.1.0: ... arm64
```

Root cause:

- The page we were modifying contained **both** the target function **and**
  the patching code (`arm64_hotpatch`).
- We temporarily removed execute permission from that page to write the patch.
- The CPU then tried to execute `arm64_hotpatch` from a non-executable page,
  causing `EXC_BAD_ACCESS` at the instruction inside `arm64_hotpatch`.

This was confirmed with symbol addresses from `nm`:

```
0000000100003a00 t _target_function
0000000100003a38 t _hook_function
0000000100003ab0 T _arm64_hotpatch
```

All three symbols were on the same 4 KB page (`0x100003000`).

Fix applied:

- We forced `arm64_hotpatch` onto its own 4 KB page so it is never executing
  from the page we are temporarily making non-executable.
- This is done with `__attribute__((aligned(4096)))` on `arm64_hotpatch`.
- We also aligned `target_function` to a 4 KB boundary for consistency.

After the change, the symbols are separated:

```
0000000100005000 t _target_function
0000000100005038 t _hook_function
0000000100007000 T _arm64_hotpatch
```

**What you should understand after this section:** inline patching can crash
if you remove execute permission from the page you are currently executing.
We fixed this by ensuring the patching code and the target code live on
different pages.

---

## 8) The demo program (what it does)

`patch_demo.c` defines two functions:

- `target_function`: prints and returns `x + 1`.
- `hook_function`: prints and returns `1337`.

The demo does:

1) Call `target_function` normally.
2) Patch the prologue of `target_function` to branch to `hook_function`.
3) Call `target_function` again (now it runs the hook).

Expected output:

- Before patch: result = 42.
- After patch: result = 1337.

**What you should understand after this section:** the patch is real; the
second call executes different machine code without recompiling.

---

## 9) Where this is Apple-specific

The patching itself is pure ARM64 logic, but Apple-specific considerations are:

- We only run the demo on **macOS ARM64** (Apple silicon). This ensures the
  instruction encoding matches the CPU.
- We use Apple’s cache flush API (`sys_icache_invalidate`) when available.

On Arch, the code builds but the runtime demo is disabled because:

- You are not on an ARM64 Apple CPU.
- There is no guarantee the same instruction cache APIs exist.

**What you should understand after this section:** runtime patching is tied to
Apple silicon; builds are cross-platform but execution is macOS ARM64.

---

## 10) How to build and run (macOS ARM64)

```
make -C arm64-patching
./arm64-patching/patch_demo
```

---

## 11) Lab 3 completion checklist

- ARM64 trampoline generator: done (`arm64_make_trampoline`)
- Prologue patcher: done (`arm64_patch_prologue`)
- Branch redirector: done (`arm64_encode_b` / `arm64_patch_b`)
- Live hot-patch engine: done (`arm64_hotpatch`)

Lab 3 is complete.

---

## 12) Full run log and tooling (latest macOS run)

This section documents **everything we ran**, what each tool does, and the
exact output observed. This is the raw evidence trail we use to debug.

### 12.1 Build step (`make`)

Command executed from `arm64-patching/`:

```
make
```

What `make` does here:

- Reads `arm64-patching/Makefile`.
- Compiles `patch_demo.c` into `patch_demo.o` (object file).
- Compiles `patcher.c` into `patcher.o`.
- Links both object files into the executable `patch_demo`.

Output:

```
cc -I../include -O2 -Wall -Wextra -Wpedantic -std=c11 -c patch_demo.c -o patch_demo.o
cc -I../include -O2 -Wall -Wextra -Wpedantic -std=c11 -c patcher.c -o patcher.o
cc -O2 -Wall -Wextra -Wpedantic -std=c11 patch_demo.o patcher.o -o patch_demo
```

Key details in the compiler flags:

- `-I../include`: use vendored headers (Apple types/constants).
- `-O2`: optimize, which can change code layout (important for patching).
- `-Wall -Wextra -Wpedantic`: strict warnings (helps catch mistakes).
- `-std=c11`: C language version.

**What you should understand:** the build completes cleanly and produces a
fresh `patch_demo` binary, so any runtime error happens after compilation.

---

### 12.2 Run step (patch demo)

Command:

```
./patch_demo
```

Observed output:

```
[demo] calling target_function before patch
[target] target_function called with x=41
[demo] result=42
[demo] patching prologue
[1]    72473 bus error  ./patch_demo
```

What each line means:

- `[demo] calling target_function before patch`  
  The demo is about to run the original function before any patching.

- `[target] target_function called with x=41`  
  The original function ran, showing the pre-patch behavior.

- `[demo] result=42`  
  The original behavior is correct: `x + 1` returned 42.

- `[demo] patching prologue`  
  We entered the patching sequence that rewrites the first instruction of
  `target_function`.

- `bus error`  
  The process crashed during the patching step. This indicates a low-level
  memory access or instruction fault while modifying executable memory.

**What you should understand:** the crash is in the **patching step**, not in
the hook itself. The first call works; the failure happens when we try to
modify code in memory.

---

### 12.3 Test script run

Command:

```
./test_patch_demo.sh
```

Observed output:

```
(no output)
```

What this script does:

- Runs `./patch_demo`.
- Greps for a set of expected output lines.
- Exits non-zero if any line is missing.

Because `patch_demo` crashes before producing the "after patch" output, the
script fails silently (non-zero exit) without printing anything.

**What you should understand:** the test script is behaving correctly because
the demo still crashes before completing the patched call.

---

### 12.4 LLDB crash capture (latest run)

Command:

```
lldb -- ./arm64-patching/patch_demo
run
bt
```

What LLDB is:

- LLDB is Apple's debugger. It runs the program under supervision and stops
  exactly where a crash happens so we can see the faulting instruction and
  call stack.

Output (verbatim):

```
(lldb) target create "./arm64-patching/patch_demo"
Current executable set to '/Users/kenny/jailbreaking/arm64-patching/patch_demo' (arm64).
(lldb) run
Process 73655 launched: '/Users/kenny/jailbreaking/arm64-patching/patch_demo' (arm64)
[demo] calling target_function before patch
[target] target_function called with x=41
[demo] result=42
[demo] patching prologue
Process 73655 stopped
* thread #1, queue = 'com.apple.main-thread', stop reason = EXC_BAD_ACCESS (code=2, address=0x100007080)
    frame #0: 0x0000000100007080 patch_demo`arm64_hotpatch + 128
patch_demo`arm64_hotpatch:
->  0x100007080 <+128>: cbnz   w0, 0x1000070bc           ; <+188>
    0x100007084 <+132>: mov    x0, x20
    0x100007088 <+136>: mov    x1, x21
    0x10000708c <+140>: mov    x2, x19
Target 0: (patch_demo) stopped.
(lldb) bt
* thread #1, queue = 'com.apple.main-thread', stop reason = EXC_BAD_ACCESS (code=2, address=0x100007080)
  * frame #0: 0x0000000100007080 patch_demo`arm64_hotpatch + 128
    frame #1: 0x00000001000072ec patch_demo`arm64_patch_prologue + 112
    frame #2: 0x0000000100004068 patch_demo`main + 104
    frame #3: 0x0000000186001d54 dyld`start + 7184
```

What this tells us:

- The crash happens **inside `arm64_hotpatch`**, not inside the target or hook.
- The fault is `EXC_BAD_ACCESS` at the exact instruction address of
  `arm64_hotpatch + 128`, which means the CPU could not execute or access
  memory at that point.
- The call stack is: `main` → `arm64_patch_prologue` → `arm64_hotpatch`.

Current hypothesis (to be tested next):

- The patching code is still removing execute permission from the page that
  contains `arm64_hotpatch`, likely because the page size is 16 KB on macOS
  ARM64 and our 4 KB alignment is not sufficient to split the functions into
  separate pages.

**What you should understand:** the debugger confirms the crash is in the
patching routine itself, so the failure is about executable memory handling,
not about the branch or the hook logic.

---

### 12.5 Page size checks and alignment fix (no assumptions)

We changed the build to *detect the actual page size* and pass it into the
code as a compile-time constant, instead of guessing.

What we changed:

- `arm64-patching/Makefile` now runs `getconf PAGESIZE` and passes it as
  `-DPATCHER_PAGE_SIZE=<value>`.
- `arm64-patching/patcher.h` defines a fallback `PATCHER_PAGE_SIZE` of 4096
  only if the build system did not provide one.
- `arm64-patching/patch_demo.c` aligns `target_function` to
  `PATCHER_PAGE_SIZE`.
- `arm64-patching/patcher.c` aligns `arm64_hotpatch` to
  `PATCHER_PAGE_SIZE`.
- `arm64_hotpatch` now **checks** that the runtime page size equals the
  compile-time `PATCHER_PAGE_SIZE`. If it does not match, we abort with a
  clear error.
- `arm64_hotpatch` also checks whether the target page is the same as the
  page containing `arm64_hotpatch`. If they match, it aborts to avoid
  self-modifying the page it is currently executing on.

Build output showing the detected page size:

```
cc -I../include -DPATCHER_PAGE_SIZE=16384 -O2 -Wall -Wextra -Wpedantic -std=c11 -c patch_demo.c -o patch_demo.o
cc -I../include -DPATCHER_PAGE_SIZE=16384 -O2 -Wall -Wextra -Wpedantic -std=c11 -c patcher.c -o patcher.o
cc -O2 -Wall -Wextra -Wpedantic -std=c11 patch_demo.o patcher.o -o patch_demo
```

This shows the build system detected a **16 KB page size** on this macOS
ARM64 machine, which is why our earlier 4 KB alignment was insufficient.

**What you should understand:** we now check the real page size and align
functions to it, so the patcher does not assume 4 KB pages.

---

### 12.6 Latest run output (post-fix)

Command sequence (from `arm64-patching/`):

```
make
./patch_demo
./test_patch_demo.sh
```

Observed output:

```
make: Nothing to be done for `all'.
[demo] calling target_function before patch
[target] target_function called with x=41
[demo] result=42
[demo] patching prologue
[demo] calling target_function after patch
[target] target_function called with x=41
[demo] result=42
```

What this tells us:

- The build is already up to date.
- The demo no longer crashes during patching (no bus error).
- The patching step completes and the second call runs.
- However, the second call still executes the original function, which means
  the branch redirection did not take effect yet.

This is now a **behavioral bug** (patch not taking effect), not a crash.
The next step is to inspect the patched instruction bytes in memory and
verify the branch encoding and write location.

**What you should understand:** the memory protection and alignment issues are
resolved, but the patch is not yet redirecting control flow.

---

### 12.7 LLDB breakpoint data (before the write happens)

We used LLDB to break at the **start** of `arm64_hotpatch` and inspect the
arguments and memory *before* the patch is applied.

Command sequence:

```
lldb -- ./arm64-patching/patch_demo
breakpoint set -n arm64_hotpatch
run
register read x0 x1 x2
memory read --format x --size 4 --count 2 $x0
bt
```

Output (verbatim):

```
(lldb) target create "./arm64-patching/patch_demo"
Current executable set to '/Users/kenny/jailbreaking/arm64-patching/patch_demo' (arm64).
(lldb) breakpoint set -n arm64_hotpatch
Breakpoint 1: where = patch_demo`arm64_hotpatch, address = 0x0000000100010000
(lldb) run
Process 80191 launched: '/Users/kenny/jailbreaking/arm64-patching/patch_demo' (arm64)
[demo] calling target_function before patch
[target] target_function called with x=41
[demo] result=42
[demo] patching prologue
Process 80191 stopped
* thread #1, queue = 'com.apple.main-thread', stop reason = breakpoint 1.1
    frame #0: 0x0000000100010000 patch_demo`arm64_hotpatch
patch_demo`arm64_hotpatch:
->  0x100010000 <+0>:  sub    sp, sp, #0x50
    0x100010004 <+4>:  stp    x24, x23, [sp, #0x10]
    0x100010008 <+8>:  stp    x22, x21, [sp, #0x20]
    0x10001000c <+12>: stp    x20, x19, [sp, #0x30]
Target 0: (patch_demo) stopped.
(lldb) register read x0 x1 x2
      x0 = 0x0000000100008000  patch_demo`target_function
      x1 = 0x000000016fdfe07c
      x2 = 0x0000000000000004
(lldb) memory read --format x --size 4 --count 2 $x0
0x100008000: 0xd100c3ff 0xa9014ff4
(lldb) bt
* thread #1, queue = 'com.apple.main-thread', stop reason = breakpoint 1.1
  * frame #0: 0x0000000100010000 patch_demo`arm64_hotpatch
    frame #1: 0x000000010001037c patch_demo`arm64_patch_prologue + 112
    frame #2: 0x0000000100004068 patch_demo`main + 104
    frame #3: 0x0000000186001d54 dyld`start + 7184
```

What this tells us:

- `x0` is the **patch target address** (the first instruction of
  `target_function`).
- `x2 = 4` means we are patching exactly one 4-byte instruction.
- The memory at `x0` is still the **original** instruction bytes at the moment
  we break, which is expected because we stopped **before** the write.

**Why this matters:** we have not yet observed the memory *after* the patch
is written. To confirm whether the patch is actually applied, we must step
through or finish the function and then re-read the bytes at `x0`.

---

### 12.8 LLDB memory check (after the write happens)

We ran the patch, then inspected the same address **after** the write:

Command sequence:

```
lldb -- ./arm64-patching/patch_demo
breakpoint set -n arm64_hotpatch
run
register read x0
expr void* $p = (void*)$x0
finish
memory read --format x --size 4 --count 2 $p
bt
```

Output (verbatim):

```
(lldb) target create "./arm64-patching/patch_demo"
Current executable set to '/Users/kenny/jailbreaking/arm64-patching/patch_demo' (arm64).
(lldb) breakpoint set -n arm64_hotpatch
Breakpoint 1: where = patch_demo`arm64_hotpatch, address = 0x0000000100010000
(lldb) run
Process 82434 launched: '/Users/kenny/jailbreaking/arm64-patching/patch_demo' (arm64)
[demo] calling target_function before patch
[target] target_function called with x=41
[demo] result=42
[demo] patching prologue
Process 82434 stopped
* thread #1, queue = 'com.apple.main-thread', stop reason = breakpoint 1.1
    frame #0: 0x0000000100010000 patch_demo`arm64_hotpatch
patch_demo`arm64_hotpatch:
->  0x100010000 <+0>:  sub    sp, sp, #0x50
    0x100010004 <+4>:  stp    x24, x23, [sp, #0x10]
    0x100010008 <+8>:  stp    x22, x21, [sp, #0x20]
    0x10001000c <+12>: stp    x20, x19, [sp, #0x30]
Target 0: (patch_demo) stopped.
(lldb) register read x0
      x0 = 0x0000000100008000  patch_demo`target_function
(lldb) expr void* $p = (void*)$x0
(lldb) finish
Process 82434 stopped
* thread #1, queue = 'com.apple.main-thread', stop reason = step out
    frame #0: 0x000000010001037c patch_demo`arm64_patch_prologue + 112
patch_demo`arm64_patch_prologue:
->  0x10001037c <+112>: cmp    w0, #0x0
    0x100010380 <+116>: csel   x0, x19, xzr, eq
    0x100010384 <+120>: b      0x10001038c               ; <+128>
    0x100010388 <+124>: mov    x0, #0x0
Target 0: (patch_demo) stopped.
(lldb) memory read --format x --size 4 --count 2 $p
0x100008000: 0x1400000e 0xa9014ff4
(lldb) bt
* thread #1, queue = 'com.apple.main-thread', stop reason = step out
  * frame #0: 0x000000010001037c patch_demo`arm64_patch_prologue + 112
    frame #1: 0x0000000100004068 patch_demo`main + 104
    frame #2: 0x0000000186001d54 dyld`start + 7184
```

What this tells us:

- The first instruction at `target_function` has changed to `0x1400000e`,
  which is an ARM64 unconditional branch (`B`).
- This proves the patch write **did** happen.

We verified the destination with `nm`:

```
0000000100008000 t _target_function
0000000100008038 t _hook_function
```

`0x1400000e` encodes a branch of 0x38 bytes (14 * 4) from `0x100008000` to
`0x100008038`, which is exactly `hook_function`.

**What you should understand:** the patch is correctly written and points to
the hook. The remaining issue is not the branch encoding or the write path.

---

### 12.9 Root cause for “patch has no effect” (compiler inlining)

Even though the patch is correct, the runtime output still shows
`target_function` executing after patching. The most likely cause is that the
compiler **inlined** `target_function` into `main` at `-O2`, so the second call
does not actually jump to the function entry point.

Evidence that supports this hypothesis:

- The branch instruction at the entry is correct.
- The output still shows the original function body executing.
- The function is small and `static`, which makes it a strong inline candidate.

Fix applied:

- We marked `target_function` as `__attribute__((noinline))` to force the
  compiler to emit a real call to the function entry so the patch can take
  effect.

**What you should understand:** inline patching can only affect **real call
sites** that go through the function’s entry point. If the compiler inlines
the function, the patch does nothing.

---

### 12.10 Final run output (patch works)

Command sequence:

```
make
./patch_demo
./test_patch_demo.sh
```

Output (verbatim):

```
cc -I../include -DPATCHER_PAGE_SIZE=16384 -O2 -Wall -Wextra -Wpedantic -std=c11 -c patch_demo.c -o patch_demo.o
cc -O2 -Wall -Wextra -Wpedantic -std=c11 patch_demo.o patcher.o -o patch_demo
[demo] calling target_function before patch
[target] target_function called with x=41
[demo] result=42
[demo] patching prologue
[demo] calling target_function after patch
[hook] hook_function called with x=41
[demo] result=1337
```

`./test_patch_demo.sh` produced no output in this run, which is expected when
all its checks pass.

What this proves:

- The patching pipeline completes without errors.
- The branch redirection takes effect.
- The hook function runs and returns 1337.

**What you should understand:** Lab 3 now works end-to-end on macOS ARM64.
