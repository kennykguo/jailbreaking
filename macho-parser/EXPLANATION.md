# Lab 1 Explanation - Mach-O Parser

This document explains the Lab 1 changes from first principles and defines the
key terms used in the output. The work focuses on a Mach-O parser that can:

- Select slices in FAT (universal) binaries.
- Map segments and sections.
- Resolve dylib dependencies.
- Map entrypoints.
- Handle 32-bit and 64-bit Mach-O.

The implementation lives in `macho-parser/macho_inspect.c`.

## 1) Mach-O and FAT from first principles

### Mach-O
Mach-O (Mach Object) is the native binary format used by Apple platforms. A
Mach-O file starts with a fixed-size header followed by a list of load
commands. The load commands describe how the kernel and dyld (the dynamic
linker/loader) should map the file into memory and what dependencies or
metadata it has.

At a minimum:

- The header identifies the file as Mach-O and declares the CPU type and the
  number/size of load commands.
- Load commands tell the loader which segments to map, which dynamic libraries
  to load, and where execution should begin.

### FAT / Universal binaries
A FAT (universal) binary is a wrapper that contains multiple Mach-O slices for
multiple CPU types. Each slice is its own Mach-O file.

The FAT file starts with a `fat_header`, then a table of `fat_arch` or
`fat_arch_64` entries. Each entry says:

- CPU type + subtype
- File offset to that slice
- Size of that slice

So the FAT file is a directory of embedded Mach-O files.

## 2) Endianness and magic numbers

The first 4 bytes are the magic number. They determine:

- Whether the file is Mach-O or FAT
- Whether the byte order is swapped (big-endian vs little-endian)

Examples:

- `MH_MAGIC_64` / `MH_CIGAM_64` for 64-bit Mach-O
- `MH_MAGIC` / `MH_CIGAM` for 32-bit Mach-O
- `FAT_MAGIC` / `FAT_CIGAM` for FAT

When the magic is a swapped variant (CIGAM), all multi-byte fields in the
header and load commands must be byte-swapped to be interpreted correctly.

## 3) Headers and load commands

Both 32-bit and 64-bit Mach-O headers contain:

- CPU type
- File type (executable, dylib, bundle, etc.)
- `ncmds`: number of load commands
- `sizeofcmds`: total size of the load command region

Load commands are a packed array right after the header:

```
+---------------------------+
| Mach-O header             |
+---------------------------+
| load_command #0           |
+---------------------------+
| load_command #1           |
+---------------------------+
| ...                       |
+---------------------------+
```

Each load command starts with:

- `cmd`: the type (LC_SEGMENT_64, LC_LOAD_DYLIB, etc.)
- `cmdsize`: the size of the command

The parser walks these commands, bounds-checks each command by `cmdsize`, and
prints meaning-specific details when the command type is recognized.

## 4) Segment and section mapping

### Segments
A segment is a contiguous region of virtual memory the loader maps from the
file. The segment command describes:

- `vmaddr` / `vmsize`: virtual address range
- `fileoff` / `filesize`: file byte range
- Memory protections

This establishes the file-to-memory mapping:

```
file offset -> memory address
vmaddr = segment.vmaddr + (file_offset - segment.fileoff)
```

If `vmsize` is larger than `filesize`, the extra memory is zero-filled (BSS).

### Sections
Segments contain sections. A section is a named subdivision (e.g. `__text`,
`__data`, `__cstring`). The parser prints each section's address, size, file
offset, alignment, and flags.

By printing both segments and sections, we get a complete map of how bytes on
storage turn into addressable memory ranges in a process.

## 5) Dylib dependency resolution

Dynamic libraries are recorded using `LC_LOAD_DYLIB`, `LC_LOAD_WEAK_DYLIB`,
`LC_REEXPORT_DYLIB`, `LC_LOAD_UPWARD_DYLIB`, and `LC_ID_DYLIB`.

Each of these commands contains a `dylib_command` with:

- A string offset (relative to the command) for the path
- Compatibility and current version numbers

The parser now prints each dylib path and its versions, which is the raw
information dyld uses to load dependencies and enforce ABI compatibility.

It also prints `LC_RPATH` and `LC_LOAD_DYLINKER`/`LC_ID_DYLINKER` so the search
paths and linker identity are visible.

## 6) Entrypoint mapping

There are two ways a Mach-O can declare its entrypoint:

- `LC_MAIN`: modern, preferred. Contains `entryoff` (file offset) and
  `stacksize`.
- `LC_UNIXTHREAD`: legacy. Contains a serialized CPU thread state; the PC (program
  counter) is the entrypoint.

The parser records `entryoff` and then maps it to a virtual address using the
segment table:

```
entry_vmaddr = segment.vmaddr + (entryoff - segment.fileoff)
```

If a `LC_UNIXTHREAD` command contains an ARM64 thread state, the parser also
extracts the `pc` and reports it as an entrypoint.

This produces an explicit mapping from file offset -> VM address, which is the
real execution address inside a process after dyld mapping.

## 7) FAT slice selection

The tool now supports explicit FAT slice selection:

- `--list` prints all slices in a FAT file.
- `--slice N` picks a specific slice index.
- `--arch NAME|CPU` picks a slice by CPU type (e.g. `arm64`).

If no selection is provided, the parser prefers ARM64 when available, otherwise
falls back to the first slice. This gives deterministic, explicit control over
which architecture is inspected.

## 8) What changed in the parser

Key behavior added to `macho_inspect`:

- 32-bit Mach-O parsing (headers, load commands, segments/sections).
- Byte-swapped Mach-O support (MH_CIGAM / MH_CIGAM_64).
- Dylib dependency and rpath output.
- Segment + section mapping with explicit fileoff/vmaddr details.
- Entrypoint mapping from LC_MAIN and ARM64 LC_UNIXTHREAD.
- FAT slice listing and selection via CLI.

This now covers all Lab 1 artifacts:

- Mach-O parser
- FAT / universal slice selector
- Segment and load-command mapper
- Dylib dependency resolver
- Entrypoint mapper

