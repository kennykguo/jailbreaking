# =========================
# AGENT.md
# =========================

# Agent Context — Apple OS Internals Lab Track

## Purpose

This repository is a structured Apple silicon OS internals research track.

The goal is to turn an Apple A7 iPad into a full-stack ARM64 + GPU research machine by replacing Apple’s high-level stacks with custom runtimes, loaders, schedulers, allocators, and compute systems.

This is not mobile app development.
This is OS, runtime, kernel, secure-boot, GPU driver, and ML runtime engineering.

---

## Research Tier

This track assumes the device is unlocked in **full-stack secure-boot research tier**, meaning:

• Control below the kernel  
• Pre-AMFI trust insertion  
• iBoot / early kernel visibility  
• Kernel memory authority  
• dyld / sandbox / entitlement authority  
• Boot-flow persistence  

The device is treated as a **secure-boot & kernel research platform**, not a phone.

---

## Current Track

We are following a structured ladder:

1. Mach-O and dyld foundations  
2. Runtime injection & Obj-C control  
3. ARM64 inline patching  
4. System-wide instrumentation  
5. Sandbox & entitlement modeling  
6. Kernel & Mach IPC tracing  
7. Secure boot & early kernel modeling  
8. GPU & deep learning runtime engineering  

Current active lab: **LAB 1 – Mach-O Parser**

---

## User Skill Profile

The user:

• Is fluent in C and low-level systems programming  
• Understands operating systems, kernels, IPC, memory management  
• Has GPU, CUDA, FPGA, allocator, and runtime engineering experience  
• Is new only to Apple’s ecosystem  

No beginner programming explanations are needed.  
Apple-specific architecture must be explained deeply.

---

## Development Environment

The user uses both:

| OS | Role |
|----|-----|
| Arch Linux | Main build, automation, reproducible systems lab |
| macOS | Apple-native reference, Mach-O corpus, dyld validation |

All tooling must:

• Build on both Arch and macOS  
• Vendor Apple headers under `include/`  
• Prefer Makefiles  
• Avoid SDK-locked frameworks  
• Compile under gcc or clang  

---

## Code Style

• Language: C only  
• Unix-like small tools  
• No frameworks, no UI  
• Explicit memory ownership  
• Explicit bounds checks  
• Vendored headers  
• Portable, reproducible builds  

Directory style:

repo/  
  include/  
  macho-parser/  
  … more to come as we finish more labs

---

## Explanation Preferences

Explain in:

• Loader / ABI / kernel / silicon terms  
• With memory layouts and diagrams  
• No UIKit / Swift / mobile abstractions  
• Real binaries, real execution flow  

Complete the lab work first, then provide first-principles, verbose
explanations of what was done and define the terms used. Write these
explanations to a markdown file in the active lab folder (e.g.,
`macho-parser/EXPLANATION.md` for Lab 1).

The explanation file must be written as a learning guide: define every
Apple-specific term at first use, explain why each step is performed, relate
concepts back to general OS/runtime principles, and explicitly state what the
reader should understand after each section.

Assume the reader is new to Apple internals: define core terms like Mach-O,
FAT/universal, dylib, dyld, linker/loader, and other Apple-specific concepts
explicitly and verbosely. Favor clarity and redundancy over brevity.

---

## End Goal

Replace Apple’s ML stack:

CoreML → MPS → Metal → GPU

with:

User Runtime → User Metal Kernels → User Memory System → Apple GPU

Turning the iPad into a **custom ARM64 + GPU ML compute node**.

---
