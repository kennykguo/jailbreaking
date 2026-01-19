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

## 5) The demo program (what it does)

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

## 6) Where this is Apple-specific

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

## 7) How to build and run (macOS ARM64)

```
make -C arm64-patching
./arm64-patching/patch_demo
```

---

## 8) Lab 3 completion checklist

- ARM64 trampoline generator: done (`arm64_make_trampoline`)
- Prologue patcher: done (`arm64_patch_prologue`)
- Branch redirector: done (`arm64_encode_b` / `arm64_patch_b`)
- Live hot-patch engine: done (`arm64_hotpatch`)

Lab 3 is complete.
