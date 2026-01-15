# Lab 1 Explanation - Mach-O Parser (Very Verbose, Apple-Beginner Friendly)

This document explains Lab 1 from first principles and defines every Apple-
specific term the first time it appears. It assumes you know low-level systems
programming in general, but *nothing* about Apple’s ecosystem. The goal is
maximum clarity, even if it feels repetitive.

Lab 1 objective: build a tool that can fully parse Apple executables (Mach-O
files), so we can reason about how Apple code is laid out on disk and how it is
mapped into memory at runtime. This is the foundation for later labs.

The tool we built is `macho-parser/macho_inspect.c`.

---

## 1) What is Mach-O? (and why we care)

**Mach-O** stands for *Mach Object* file format. It is Apple’s equivalent of:

- **ELF** on Linux
- **PE** on Windows

It is the standard binary format used on macOS and iOS. Every executable,
shared library, and bundle you run on Apple systems is a Mach-O file.

Why it matters: if you want to understand how Apple programs start, load
libraries, and map memory, you must understand Mach-O. This is the “native
assembly language” of Apple’s userland runtime.

**What you should understand after this section:** Mach-O is Apple’s binary
format, analogous to ELF/PE, and all later labs depend on being able to parse
it correctly.

---

## 2) What is a FAT / universal binary?

**FAT** (also called **universal**) is a container format Apple uses to ship
one file that supports multiple CPU architectures. Example: a single binary
that runs on both Intel x86_64 and Apple ARM64.

A FAT file is *not* a single executable. It is a wrapper that contains multiple
independent Mach-O files, called **slices**. Each slice is a full Mach-O for a
specific CPU type.

So a FAT file is like a directory:

- FAT header says how many slices exist.
- Each slice entry says: CPU type, file offset, size.

Our parser must:

1) Read the FAT header.
2) List slices (if asked).
3) Pick one slice and parse it as a normal Mach-O.

**What you should understand after this section:** FAT/universal binaries are
containers for multiple Mach-O slices; you must select a slice before parsing.

---

## 3) Apple’s loader and linker vocabulary (dyld, linker, loader, dylib)

This is Apple-specific terminology and is easy to confuse. Here is the mapping:

### Loader vs linker (general concepts)

- **Linker**: A build-time tool that combines object files and libraries into a
  final executable. On Apple, the main linker is `ld` (like on Linux), but it
  emits Mach-O.
- **Loader**: The runtime component that maps an executable into memory and
  starts it. On Linux, this is `ld-linux.so`. On Apple, this is **dyld**.

### dyld
**dyld** is Apple’s *dynamic loader*. It is the runtime program that:

- Maps Mach-O segments into memory.
- Loads dependent shared libraries.
- Applies fixups and relocations.
- Transfers control to the program’s entrypoint.

### dylib
A **dylib** is Apple’s shared library format. It is similar to a `.so` on Linux
or a `.dll` on Windows. Dylibs are Mach-O files themselves, but with a file
header that marks them as “shared libraries.”

### dyld vs kernel loader
On Apple systems, the kernel does an initial minimal mapping of the main
executable, then passes control to dyld, which completes loading (dependencies,
fixups, etc.). So dyld is a userland loader layered on top of the kernel.

**What you should understand after this section:**
- Linker = build-time; loader = runtime.
- dyld is Apple’s runtime loader.
- dylib is Apple’s shared library file.

---

## 4) Mach-O file structure from first principles

At a high level, a Mach-O file has:

1) A **header** that identifies the file and CPU type.
2) A list of **load commands** (metadata for the loader).
3) The **segment data** (actual bytes for code and data).

### The header
The header tells you:

- The CPU type (e.g., ARM64).
- The file type (executable, dylib, etc.).
- How many load commands exist.
- The total size of the load commands.

### Load commands
A **load command** is a structure that tells the loader what to do. Examples:

- Map a segment into memory.
- Load a dylib dependency.
- Define the entrypoint.

Each load command has:

- `cmd`: a numeric type.
- `cmdsize`: how big this command is.

The loader walks this list and executes the instructions described by each
command. Our parser does the same, but only prints the information.

**What you should understand after this section:** Mach-O is header + load
commands + data. Load commands are the “instructions” the loader follows.

---

## 5) Endianness and magic numbers

**Endianness** is byte order. Some Mach-O files are stored in big-endian
(swapped) order. The “magic number” at the file start tells you whether the
values are normal or swapped.

Examples:

- `MH_MAGIC_64`: 64-bit Mach-O, normal byte order.
- `MH_CIGAM_64`: 64-bit Mach-O, swapped byte order.
- `FAT_MAGIC`: FAT container, normal.
- `FAT_CIGAM`: FAT container, swapped.

When the magic indicates swapped order, every multi-byte field must be byte-
swapped. This is why the parser uses `read32_u` and `read64_u` helpers.

**What you should understand after this section:** Magic numbers are the file
signature; they tell us Mach-O vs FAT and whether to byte-swap.

---

## 6) Segments (the loader’s actual mapping units)

A **segment** is the unit the loader maps into memory. Each segment describes:

- **Virtual address range** in memory (`vmaddr`, `vmsize`).
- **File byte range** on disk (`fileoff`, `filesize`).
- **Permissions** (read/write/execute).

Mapping rule:

```
vmaddr = segment.vmaddr + (file_offset - segment.fileoff)
```

If `vmsize > filesize`, the extra memory is zero-filled. This is how BSS (zero-
initialized data) appears without taking disk space.

Our parser prints each segment and records it, because we later need segments to
map the entrypoint offset to a real virtual address.

**What you should understand after this section:** Segments define how the file
maps into memory; they are the core of the loader’s work.

---

## 7) Sections (linker-level organization)

A **section** is a named subdivision inside a segment. Examples:

- `__text`: machine code.
- `__data`: initialized data.
- `__bss`: zero-initialized data.

Sections are mostly for linkers, debuggers, and tooling. The loader uses
segments, not sections. But sections are useful for understanding where code
and data live inside a segment.

Our parser prints sections to give you a fine-grained map of memory.

**What you should understand after this section:** Sections are link-time
subdivisions inside segments; useful for analysis but not the loader’s unit.

---

## 8) Dylib dependencies (what dyld will load)

Mach-O files list their dynamic dependencies using load commands:

- `LC_LOAD_DYLIB`: required dylib.
- `LC_LOAD_WEAK_DYLIB`: optional dylib (load if present).
- `LC_REEXPORT_DYLIB`: re-exported symbols.
- `LC_LOAD_UPWARD_DYLIB`: special lookup semantics.
- `LC_ID_DYLIB`: the identity of the dylib file itself.

Each of these commands contains a path string to the dylib. This path is what
**dyld** uses to locate and load the library at runtime. The command also stores
version numbers used for compatibility checks.

We also parse:

- `LC_RPATH`: runtime search prefixes.
- `LC_LOAD_DYLINKER` / `LC_ID_DYLINKER`: which dynamic loader is in use.

**What you should understand after this section:** The dylib load commands are
literally the dependency list dyld uses at runtime.

---

## 9) Entrypoints (where execution begins)

There are two ways a Mach-O defines its entrypoint:

### 9.1 LC_MAIN (modern)

- `entryoff`: file offset to the entry function.
- `stacksize`: requested stack size.

We convert `entryoff` to a virtual address using the segment mapping table:

```
entry_vmaddr = segment.vmaddr + (entryoff - segment.fileoff)
```

### 9.2 LC_UNIXTHREAD (legacy)

This command encodes a CPU thread state. For ARM64, the program counter (`pc`)
inside that thread state is the entrypoint. We parse and print it if present.

**What you should understand after this section:** Entrypoint mapping converts
file offsets into actual runtime addresses; this is how we know where code starts.

---

## 10) What our code does (explicit walkthrough)

This is how the parser is structured:

- `parse_fat`: reads FAT headers, lists slices, selects one by index or arch,
  then dispatches to `parse_thin_macho_32` or `parse_thin_macho_64` based on the
  slice’s magic.

- `parse_thin_macho_32`: parses 32-bit Mach-O header, load commands, segments,
  sections, dylibs, rpaths, dyld path, UUID, and entrypoints.

- `parse_thin_macho_64`: same as above for 64-bit files.

- Helpers like `read32_u`, `read64_u` ensure swapped-endian correctness.

- A small segment table is collected so we can map `entryoff` to `vmaddr`.

**What you should understand after this section:** The parser walks exactly the
same metadata the loader uses, but in a safe, read-only way.

---

## 11) What we learned (findings from the tool)

From `macho_inspect` output you can now see:

- Which segments exist, their memory ranges, and their file offsets.
- Which sections contain code or data.
- Which dylibs will be loaded and from where.
- The exact entrypoint address as it will exist in memory.
- Which architecture slice you are parsing in a FAT file.

This is the minimal foundation for all later labs (runtime injection, patching,
tracing, and loader manipulation).

**What you should understand after this section:** You can now translate a
Mach-O file into a full memory map and dependency graph.

---

## 12) How to use the tool (examples)

```
./macho_inspect <mach-o file>
```

List slices in a FAT file:

```
./macho_inspect --list <fat file>
```

Pick a slice by index:

```
./macho_inspect --slice 1 <fat file>
```

Pick a slice by arch:

```
./macho_inspect --arch arm64 <fat file>
```

---

## 13) Lab 1 completion checklist

- Mach-O parser: done
- FAT / universal slice selector: done
- Segment and load-command mapper: done
- Dylib dependency resolver: done
- Entrypoint mapper: done

Lab 1 is complete.
