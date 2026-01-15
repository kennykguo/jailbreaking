#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

#include "../include/macho/loader.h"
#include "../include/macho/fat.h"


// --- FAT64 compatibility shim ---
// Some fat.h variants omit FAT64 constants/structs.
// We define a minimal subset when missing to keep the parser portable.
#ifndef FAT_MAGIC_64
#define FAT_MAGIC_64  0xcafebabf
#endif

#ifndef FAT_CIGAM_64
#define FAT_CIGAM_64  0xbfbafeca
#endif

#ifndef FAT_MAGIC
// If FAT_MAGIC is missing entirely, something is very wrong with headers.
#error "FAT_MAGIC is missing; check your include/mach-o/fat.h"
#endif

// Define fat_arch_64 if the header doesn't provide a complete definition.
#ifndef _FAT_ARCH_64
#define _FAT_ARCH_64
struct fat_arch_64 {
    uint32_t cputype;
    uint32_t cpusubtype;
    uint64_t offset;
    uint64_t size;
    uint32_t align;
    uint32_t reserved;
};
#endif

// ARM64 thread state constants/structs are provided by vendored headers.



static uint32_t bswap32_u(uint32_t x) {
    return ((x & 0x000000FFu) << 24) |
           ((x & 0x0000FF00u) <<  8) |
           ((x & 0x00FF0000u) >>  8) |
           ((x & 0xFF000000u) >> 24);
}

static uint64_t bswap64_u(uint64_t x) {
    return ((uint64_t)bswap32_u((uint32_t)(x & 0xFFFFFFFFULL)) << 32) |
            (uint64_t)bswap32_u((uint32_t)(x >> 32));
}

static uint32_t read32_u(uint32_t x, int swapped) {
    return swapped ? bswap32_u(x) : x;
}

static uint64_t read64_u(uint64_t x, int swapped) {
    return swapped ? bswap64_u(x) : x;
}

static const char *cpu_type_name(uint32_t cputype) {
    switch (cputype) {
        case CPU_TYPE_ARM: return "ARM";
        case CPU_TYPE_ARM64: return "ARM64";
        case CPU_TYPE_X86: return "X86";
        case CPU_TYPE_X86_64: return "X86_64";
        case CPU_TYPE_POWERPC: return "PPC";
        case CPU_TYPE_POWERPC64: return "PPC64";
        default: return "UNKNOWN";
    }
}

static int parse_cputype(const char *s, uint32_t *out) {
    if (!s || !*s) return 0;
    if (strcmp(s, "arm") == 0) { *out = CPU_TYPE_ARM; return 1; }
    if (strcmp(s, "arm64") == 0) { *out = CPU_TYPE_ARM64; return 1; }
    if (strcmp(s, "x86") == 0 || strcmp(s, "i386") == 0) { *out = CPU_TYPE_X86; return 1; }
    if (strcmp(s, "x86_64") == 0 || strcmp(s, "amd64") == 0) { *out = CPU_TYPE_X86_64; return 1; }
    if (strcmp(s, "ppc") == 0) { *out = CPU_TYPE_POWERPC; return 1; }
    if (strcmp(s, "ppc64") == 0) { *out = CPU_TYPE_POWERPC64; return 1; }

    char *end = NULL;
    unsigned long v = strtoul(s, &end, 0);
    if (end && *end == '\0') {
        *out = (uint32_t)v;
        return 1;
    }
    return 0;
}

static const char *lc_name(uint32_t c) {
    switch(c) {
        case LC_SEGMENT: return "LC_SEGMENT";
        case LC_SEGMENT_64: return "LC_SEGMENT_64";
        case LC_LOAD_DYLIB: return "LC_LOAD_DYLIB";
        case LC_LOAD_WEAK_DYLIB: return "LC_LOAD_WEAK_DYLIB";
        case LC_REEXPORT_DYLIB: return "LC_REEXPORT_DYLIB";
        case LC_LOAD_UPWARD_DYLIB: return "LC_LOAD_UPWARD_DYLIB";
        case LC_ID_DYLIB: return "LC_ID_DYLIB";
        case LC_ID_DYLINKER: return "LC_ID_DYLINKER";
        case LC_LOAD_DYLINKER: return "LC_LOAD_DYLINKER";
        case LC_MAIN: return "LC_MAIN";
        case LC_THREAD: return "LC_THREAD";
        case LC_UNIXTHREAD: return "LC_UNIXTHREAD";
        case LC_UUID: return "LC_UUID";
        case LC_RPATH: return "LC_RPATH";
        case LC_DYLD_ENVIRONMENT: return "LC_DYLD_ENVIRONMENT";
        default: return "LC_OTHER";
    }
}

struct parse_opts {
    int list_only;
    int have_slice;
    uint32_t slice_index;
    int have_arch;
    uint32_t arch;
};

struct segment_map {
    char name[17];
    uint64_t vmaddr;
    uint64_t vmsize;
    uint64_t fileoff;
    uint64_t filesize;
};

static size_t lc_strnlen(const char *s, size_t maxlen) {
    size_t n = 0;
    while (n < maxlen && s[n] != '\0') n++;
    return n;
}

static void print_lc_string(const uint8_t *lc, uint32_t cmdsize, uint32_t off) {
    if (off >= cmdsize) {
        printf("<bad-offset>");
        return;
    }
    const char *s = (const char *)(lc + off);
    size_t maxlen = cmdsize - off;
    size_t n = lc_strnlen(s, maxlen);
    if (n == maxlen) {
        printf("<unterminated>");
        return;
    }
    printf("%.*s", (int)n, s);
}

static void map_entry_from_fileoff(const struct segment_map *segs, size_t segs_count,
                                   uint64_t entryoff) {
    for (size_t i = 0; i < segs_count; i++) {
        uint64_t start = segs[i].fileoff;
        uint64_t end = segs[i].fileoff + segs[i].filesize;
        if (entryoff >= start && entryoff < end) {
            uint64_t vm = segs[i].vmaddr + (entryoff - segs[i].fileoff);
            printf("     entry vmaddr=0x%llx (segment %s)\n",
                   (unsigned long long)vm, segs[i].name);
            return;
        }
    }
    printf("     entry vmaddr=<not mapped>\n");
}

static void print_uuid(const uint8_t *uuid) {
    printf("%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-",
           uuid[0], uuid[1], uuid[2], uuid[3],
           uuid[4], uuid[5],
           uuid[6], uuid[7],
           uuid[8], uuid[9]);
    printf("%02x%02x%02x%02x%02x%02x\n",
           uuid[10], uuid[11], uuid[12], uuid[13], uuid[14], uuid[15]);
}

static int parse_thin_macho_32(const uint8_t *buf, size_t sz) {
    if (sz < sizeof(struct mach_header)) {
        fprintf(stderr, "error: file too small for mach_header\n");
        return 1;
    }

    const struct mach_header *h = (const struct mach_header*)buf;
    uint32_t magic = h->magic;
    int swapped = 0;
    if (magic == MH_MAGIC) swapped = 0;
    else if (magic == MH_CIGAM) swapped = 1;
    else {
        fprintf(stderr, "error: not MH_MAGIC/MH_CIGAM (0x%08x)\n", magic);
        return 1;
    }

    uint32_t ncmds = read32_u(h->ncmds, swapped);
    uint32_t sizeofcmds = read32_u(h->sizeofcmds, swapped);
    uint32_t cputype = read32_u((uint32_t)h->cputype, swapped);

    printf("== Thin Mach-O (32-bit) ==\n");
    printf("CPU type: %u (%s)\n", cputype, cpu_type_name(cputype));
    printf("Load commands: %u  sizeofcmds=%u\n", ncmds, sizeofcmds);

    const uint8_t *p = buf + sizeof(struct mach_header);
    const uint8_t *end = buf + sz;

    struct segment_map *segs = calloc(ncmds, sizeof(*segs));
    if (!segs) { perror("calloc"); return 1; }
    size_t segs_count = 0;
    uint64_t entryoff = 0;
    int have_entryoff = 0;
    uint64_t entry_pc = 0;
    int have_entry_pc = 0;

    for (uint32_t i = 0; i < ncmds; i++) {
        if (p + sizeof(struct load_command) > end) {
            fprintf(stderr, "error: truncated load command %u\n", i);
            free(segs);
            return 1;
        }

        const struct load_command *lc = (const struct load_command*)p;
        uint32_t cmd = read32_u(lc->cmd, swapped);
        uint32_t cmdsize = read32_u(lc->cmdsize, swapped);
        if (cmdsize < sizeof(struct load_command)) {
            fprintf(stderr, "error: invalid cmdsize at %u\n", i);
            free(segs);
            return 1;
        }
        if (p + cmdsize > end) {
            fprintf(stderr, "error: load command %u extends beyond file\n", i);
            free(segs);
            return 1;
        }

        printf("[%2u] %-18s (0x%x) size=%u\n", i, lc_name(cmd), cmd, cmdsize);

        if (cmd == LC_SEGMENT) {
            if (cmdsize < sizeof(struct segment_command)) {
                fprintf(stderr, "error: LC_SEGMENT too small\n");
                free(segs);
                return 1;
            }
            const struct segment_command *s = (const struct segment_command*)p;
            uint32_t nsects = read32_u(s->nsects, swapped);
            printf("     SEG %-16s vm=0x%08x size=0x%08x fileoff=0x%08x filesize=0x%08x nsects=%u\n",
                   s->segname,
                   read32_u(s->vmaddr, swapped),
                   read32_u(s->vmsize, swapped),
                   read32_u(s->fileoff, swapped),
                   read32_u(s->filesize, swapped),
                   nsects);

            if (segs_count < ncmds) {
                struct segment_map *m = &segs[segs_count++];
                memset(m, 0, sizeof(*m));
                memcpy(m->name, s->segname, 16);
                m->name[16] = '\0';
                m->vmaddr = read32_u(s->vmaddr, swapped);
                m->vmsize = read32_u(s->vmsize, swapped);
                m->fileoff = read32_u(s->fileoff, swapped);
                m->filesize = read32_u(s->filesize, swapped);
            }

            size_t need = sizeof(struct segment_command) +
                          (size_t)nsects * sizeof(struct section);
            if (cmdsize < need) {
                fprintf(stderr, "error: LC_SEGMENT sections truncated\n");
                free(segs);
                return 1;
            }
            const struct section *sec = (const struct section*)(p + sizeof(struct segment_command));
            for (uint32_t sidx = 0; sidx < nsects; sidx++) {
                printf("         SECT %-16s seg=%-16s addr=0x%08x size=0x%08x off=0x%08x align=%u flags=0x%x\n",
                       sec[sidx].sectname,
                       sec[sidx].segname,
                       read32_u(sec[sidx].addr, swapped),
                       read32_u(sec[sidx].size, swapped),
                       read32_u(sec[sidx].offset, swapped),
                       read32_u(sec[sidx].align, swapped),
                       read32_u(sec[sidx].flags, swapped));
            }
        } else if (cmd == LC_MAIN) {
            if (cmdsize < sizeof(struct entry_point_command)) {
                fprintf(stderr, "error: LC_MAIN too small\n");
                free(segs);
                return 1;
            }
            const struct entry_point_command *ep = (const struct entry_point_command*)p;
            entryoff = read64_u(ep->entryoff, swapped);
            have_entryoff = 1;
            printf("     entryoff=0x%llx stacksize=0x%llx\n",
                   (unsigned long long)entryoff,
                   (unsigned long long)read64_u(ep->stacksize, swapped));
        } else if (cmd == LC_UUID) {
            const struct uuid_command *uc = (const struct uuid_command*)p;
            printf("     uuid=");
            print_uuid(uc->uuid);
        } else if (cmd == LC_LOAD_DYLIB || cmd == LC_LOAD_WEAK_DYLIB ||
                   cmd == LC_REEXPORT_DYLIB || cmd == LC_LOAD_UPWARD_DYLIB ||
                   cmd == LC_ID_DYLIB) {
            const struct dylib_command *dc = (const struct dylib_command*)p;
            uint32_t name_off = read32_u(dc->dylib.name.offset, swapped);
            printf("     dylib=");
            print_lc_string(p, cmdsize, name_off);
            printf(" current=0x%x compat=0x%x\n",
                   read32_u(dc->dylib.current_version, swapped),
                   read32_u(dc->dylib.compatibility_version, swapped));
        } else if (cmd == LC_RPATH) {
            const struct rpath_command *rc = (const struct rpath_command*)p;
            uint32_t name_off = read32_u(rc->path.offset, swapped);
            printf("     rpath=");
            print_lc_string(p, cmdsize, name_off);
            printf("\n");
        } else if (cmd == LC_LOAD_DYLINKER || cmd == LC_ID_DYLINKER ||
                   cmd == LC_DYLD_ENVIRONMENT) {
            const struct dylinker_command *dc = (const struct dylinker_command*)p;
            uint32_t name_off = read32_u(dc->name.offset, swapped);
            printf("     dyld=");
            print_lc_string(p, cmdsize, name_off);
            printf("\n");
        } else if (cmd == LC_UNIXTHREAD || cmd == LC_THREAD) {
            if (cmdsize < sizeof(struct thread_command)) {
                fprintf(stderr, "error: LC_THREAD too small\n");
                free(segs);
                return 1;
            }
            const uint8_t *tp = p + sizeof(struct thread_command);
            const uint8_t *tend = p + cmdsize;
            while (tp + 8 <= tend) {
                uint32_t flavor = read32_u(*(const uint32_t*)tp, swapped);
                uint32_t count = read32_u(*(const uint32_t*)(tp + 4), swapped);
                tp += 8;
                size_t bytes = (size_t)count * sizeof(uint32_t);
                if (tp + bytes > tend) break;
                if (flavor == ARM_THREAD_STATE64 && count >= ARM_THREAD_STATE64_COUNT) {
                    const struct arm_thread_state64 *ts =
                        (const struct arm_thread_state64*)tp;
                    entry_pc = read64_u(ts->pc, swapped);
                    have_entry_pc = 1;
                }
                tp += bytes;
            }
            if (have_entry_pc) {
                printf("     entry pc=0x%llx (from LC_UNIXTHREAD)\n",
                       (unsigned long long)entry_pc);
            }
        }

        p += cmdsize;
    }

    if (have_entryoff) {
        map_entry_from_fileoff(segs, segs_count, entryoff);
    }

    free(segs);
    return 0;
}

static int parse_thin_macho_64(const uint8_t *buf, size_t sz) {
    if (sz < sizeof(struct mach_header_64)) {
        fprintf(stderr, "error: file too small for mach_header_64\n");
        return 1;
    }

    const struct mach_header_64 *h = (const struct mach_header_64*)buf;
    uint32_t magic = h->magic;
    int swapped = 0;
    if (magic == MH_MAGIC_64) swapped = 0;
    else if (magic == MH_CIGAM_64) swapped = 1;
    else {
        fprintf(stderr, "error: not MH_MAGIC_64/MH_CIGAM_64 (0x%08x)\n", magic);
        return 1;
    }

    uint32_t ncmds = read32_u(h->ncmds, swapped);
    uint32_t sizeofcmds = read32_u(h->sizeofcmds, swapped);
    uint32_t cputype = read32_u((uint32_t)h->cputype, swapped);

    printf("== Thin Mach-O (64-bit) ==\n");
    printf("CPU type: %u (%s)\n", cputype, cpu_type_name(cputype));
    printf("Load commands: %u  sizeofcmds=%u\n", ncmds, sizeofcmds);

    const uint8_t *p = buf + sizeof(struct mach_header_64);
    const uint8_t *end = buf + sz;

    struct segment_map *segs = calloc(ncmds, sizeof(*segs));
    if (!segs) { perror("calloc"); return 1; }
    size_t segs_count = 0;
    uint64_t entryoff = 0;
    int have_entryoff = 0;
    uint64_t entry_pc = 0;
    int have_entry_pc = 0;

    for (uint32_t i = 0; i < ncmds; i++) {
        if (p + sizeof(struct load_command) > end) {
            fprintf(stderr, "error: truncated load command %u\n", i);
            free(segs);
            return 1;
        }

        const struct load_command *lc = (const struct load_command*)p;
        uint32_t cmd = read32_u(lc->cmd, swapped);
        uint32_t cmdsize = read32_u(lc->cmdsize, swapped);
        if (cmdsize < sizeof(struct load_command)) {
            fprintf(stderr, "error: invalid cmdsize at %u\n", i);
            free(segs);
            return 1;
        }
        if (p + cmdsize > end) {
            fprintf(stderr, "error: load command %u extends beyond file\n", i);
            free(segs);
            return 1;
        }

        printf("[%2u] %-18s (0x%x) size=%u\n", i, lc_name(cmd), cmd, cmdsize);

        if (cmd == LC_SEGMENT_64) {
            if (cmdsize < sizeof(struct segment_command_64)) {
                fprintf(stderr, "error: LC_SEGMENT_64 too small\n");
                free(segs);
                return 1;
            }
            const struct segment_command_64 *s = (const struct segment_command_64*)p;
            uint32_t nsects = read32_u(s->nsects, swapped);
            printf("     SEG %-16s vm=0x%llx size=0x%llx fileoff=0x%llx filesize=0x%llx nsects=%u\n",
                   s->segname,
                   (unsigned long long)read64_u(s->vmaddr, swapped),
                   (unsigned long long)read64_u(s->vmsize, swapped),
                   (unsigned long long)read64_u(s->fileoff, swapped),
                   (unsigned long long)read64_u(s->filesize, swapped),
                   nsects);

            if (segs_count < ncmds) {
                struct segment_map *m = &segs[segs_count++];
                memset(m, 0, sizeof(*m));
                memcpy(m->name, s->segname, 16);
                m->name[16] = '\0';
                m->vmaddr = read64_u(s->vmaddr, swapped);
                m->vmsize = read64_u(s->vmsize, swapped);
                m->fileoff = read64_u(s->fileoff, swapped);
                m->filesize = read64_u(s->filesize, swapped);
            }

            size_t need = sizeof(struct segment_command_64) +
                          (size_t)nsects * sizeof(struct section_64);
            if (cmdsize < need) {
                fprintf(stderr, "error: LC_SEGMENT_64 sections truncated\n");
                free(segs);
                return 1;
            }
            const struct section_64 *sec = (const struct section_64*)(p + sizeof(struct segment_command_64));
            for (uint32_t sidx = 0; sidx < nsects; sidx++) {
                printf("         SECT %-16s seg=%-16s addr=0x%llx size=0x%llx off=0x%x align=%u flags=0x%x\n",
                       sec[sidx].sectname,
                       sec[sidx].segname,
                       (unsigned long long)read64_u(sec[sidx].addr, swapped),
                       (unsigned long long)read64_u(sec[sidx].size, swapped),
                       read32_u(sec[sidx].offset, swapped),
                       read32_u(sec[sidx].align, swapped),
                       read32_u(sec[sidx].flags, swapped));
            }
        } else if (cmd == LC_MAIN) {
            if (cmdsize < sizeof(struct entry_point_command)) {
                fprintf(stderr, "error: LC_MAIN too small\n");
                free(segs);
                return 1;
            }
            const struct entry_point_command *ep = (const struct entry_point_command*)p;
            entryoff = read64_u(ep->entryoff, swapped);
            have_entryoff = 1;
            printf("     entryoff=0x%llx stacksize=0x%llx\n",
                   (unsigned long long)entryoff,
                   (unsigned long long)read64_u(ep->stacksize, swapped));
        } else if (cmd == LC_UUID) {
            const struct uuid_command *uc = (const struct uuid_command*)p;
            printf("     uuid=");
            print_uuid(uc->uuid);
        } else if (cmd == LC_LOAD_DYLIB || cmd == LC_LOAD_WEAK_DYLIB ||
                   cmd == LC_REEXPORT_DYLIB || cmd == LC_LOAD_UPWARD_DYLIB ||
                   cmd == LC_ID_DYLIB) {
            const struct dylib_command *dc = (const struct dylib_command*)p;
            uint32_t name_off = read32_u(dc->dylib.name.offset, swapped);
            printf("     dylib=");
            print_lc_string(p, cmdsize, name_off);
            printf(" current=0x%x compat=0x%x\n",
                   read32_u(dc->dylib.current_version, swapped),
                   read32_u(dc->dylib.compatibility_version, swapped));
        } else if (cmd == LC_RPATH) {
            const struct rpath_command *rc = (const struct rpath_command*)p;
            uint32_t name_off = read32_u(rc->path.offset, swapped);
            printf("     rpath=");
            print_lc_string(p, cmdsize, name_off);
            printf("\n");
        } else if (cmd == LC_LOAD_DYLINKER || cmd == LC_ID_DYLINKER ||
                   cmd == LC_DYLD_ENVIRONMENT) {
            const struct dylinker_command *dc = (const struct dylinker_command*)p;
            uint32_t name_off = read32_u(dc->name.offset, swapped);
            printf("     dyld=");
            print_lc_string(p, cmdsize, name_off);
            printf("\n");
        } else if (cmd == LC_UNIXTHREAD || cmd == LC_THREAD) {
            if (cmdsize < sizeof(struct thread_command)) {
                fprintf(stderr, "error: LC_THREAD too small\n");
                free(segs);
                return 1;
            }
            const uint8_t *tp = p + sizeof(struct thread_command);
            const uint8_t *tend = p + cmdsize;
            while (tp + 8 <= tend) {
                uint32_t flavor = read32_u(*(const uint32_t*)tp, swapped);
                uint32_t count = read32_u(*(const uint32_t*)(tp + 4), swapped);
                tp += 8;
                size_t bytes = (size_t)count * sizeof(uint32_t);
                if (tp + bytes > tend) break;
                if (flavor == ARM_THREAD_STATE64 && count >= ARM_THREAD_STATE64_COUNT) {
                    const struct arm_thread_state64 *ts =
                        (const struct arm_thread_state64*)tp;
                    entry_pc = read64_u(ts->pc, swapped);
                    have_entry_pc = 1;
                }
                tp += bytes;
            }
            if (have_entry_pc) {
                printf("     entry pc=0x%llx (from LC_UNIXTHREAD)\n",
                       (unsigned long long)entry_pc);
            }
        }

        p += cmdsize;
    }

    if (have_entryoff) {
        map_entry_from_fileoff(segs, segs_count, entryoff);
    }

    free(segs);
    return 0;
}

static int is_fat_magic(uint32_t m) {
    return (m == FAT_MAGIC || m == FAT_CIGAM || m == FAT_MAGIC_64 || m == FAT_CIGAM_64);
}

static int parse_fat(const uint8_t *buf, size_t sz, const struct parse_opts *opts) {
    if (sz < sizeof(struct fat_header)) {
        fprintf(stderr, "error: file too small for fat_header\n");
        return 1;
    }

    const struct fat_header *fh = (const struct fat_header*)buf;
    uint32_t magic = fh->magic;
    int swapped = (magic == FAT_CIGAM || magic == FAT_CIGAM_64);

    uint32_t nfat = swapped ? bswap32_u(fh->nfat_arch) : fh->nfat_arch;

    printf("== FAT / Universal Mach-O ==\n");
    printf("fat magic: 0x%08x  nfat_arch=%u  (swapped=%d)\n", magic, nfat, swapped);

    // Support FAT32 (fat_arch) and FAT64 (fat_arch_64)
    if (magic == FAT_MAGIC || magic == FAT_CIGAM) {
        size_t need = sizeof(struct fat_header) + (size_t)nfat * sizeof(struct fat_arch);
        if (sz < need) {
            fprintf(stderr, "error: truncated fat_arch table\n");
            return 1;
        }

        const struct fat_arch *arch = (const struct fat_arch*)(buf + sizeof(struct fat_header));

        for (uint32_t i = 0; opts && opts->list_only && i < nfat; i++) {
            uint32_t cputype = read32_u(arch[i].cputype, swapped);
            uint32_t cpusub = read32_u(arch[i].cpusubtype, swapped);
            uint32_t off = read32_u(arch[i].offset, swapped);
            uint32_t size = read32_u(arch[i].size, swapped);
            printf("slice[%u]: cputype=%u (%s) subtype=%u off=%u size=%u\n",
                   i, cputype, cpu_type_name(cputype), cpusub, off, size);
        }
        if (opts && opts->list_only) return 0;

        int pick = -1;
        if (opts && opts->have_slice) {
            if (opts->slice_index >= nfat) {
                fprintf(stderr, "error: slice index out of range\n");
                return 1;
            }
            pick = (int)opts->slice_index;
        } else if (opts && opts->have_arch) {
            for (uint32_t i = 0; i < nfat; i++) {
                uint32_t cputype = read32_u(arch[i].cputype, swapped);
                if (cputype == opts->arch) { pick = (int)i; break; }
            }
            if (pick < 0) {
                fprintf(stderr, "error: requested arch not found in fat file\n");
                return 1;
            }
        } else {
            // Pick a slice: prefer ARM64 if present, otherwise first slice.
            for (uint32_t i = 0; i < nfat; i++) {
                uint32_t cputype = read32_u(arch[i].cputype, swapped);
                if (cputype == (uint32_t)CPU_TYPE_ARM64) { pick = (int)i; break; }
            }
            if (pick < 0) pick = 0;
        }

        uint32_t off  = read32_u(arch[pick].offset, swapped);
        uint32_t size = read32_u(arch[pick].size, swapped);
        uint32_t cputype = read32_u(arch[pick].cputype, swapped);

        printf("picked slice %d: cputype=%u (%s) offset=%u size=%u\n",
               pick, cputype, cpu_type_name(cputype), off, size);

        if ((uint64_t)off + (uint64_t)size > (uint64_t)sz) {
            fprintf(stderr, "error: slice out of bounds\n");
            return 1;
        }

        if (size < sizeof(uint32_t)) {
            fprintf(stderr, "error: slice too small\n");
            return 1;
        }
        uint32_t smagic = 0;
        memcpy(&smagic, buf + off, sizeof(smagic));
        if (smagic == MH_MAGIC_64 || smagic == MH_CIGAM_64) {
            return parse_thin_macho_64(buf + off, size);
        }
        return parse_thin_macho_32(buf + off, size);
    }

    // FAT64
    if (magic == FAT_MAGIC_64 || magic == FAT_CIGAM_64) {
        size_t need = sizeof(struct fat_header) + (size_t)nfat * sizeof(struct fat_arch_64);
        if (sz < need) {
            fprintf(stderr, "error: truncated fat_arch_64 table\n");
            return 1;
        }

        const struct fat_arch_64 *arch = (const struct fat_arch_64*)(buf + sizeof(struct fat_header));

        for (uint32_t i = 0; opts && opts->list_only && i < nfat; i++) {
            uint32_t cputype = read32_u(arch[i].cputype, swapped);
            uint32_t cpusub = read32_u(arch[i].cpusubtype, swapped);
            uint64_t off = read64_u(arch[i].offset, swapped);
            uint64_t size = read64_u(arch[i].size, swapped);
            printf("slice[%u]: cputype=%u (%s) subtype=%u off=%llu size=%llu\n",
                   i, cputype, cpu_type_name(cputype), cpusub,
                   (unsigned long long)off, (unsigned long long)size);
        }
        if (opts && opts->list_only) return 0;

        int pick = -1;
        if (opts && opts->have_slice) {
            if (opts->slice_index >= nfat) {
                fprintf(stderr, "error: slice index out of range\n");
                return 1;
            }
            pick = (int)opts->slice_index;
        } else if (opts && opts->have_arch) {
            for (uint32_t i = 0; i < nfat; i++) {
                uint32_t cputype = read32_u(arch[i].cputype, swapped);
                if (cputype == opts->arch) { pick = (int)i; break; }
            }
            if (pick < 0) {
                fprintf(stderr, "error: requested arch not found in fat file\n");
                return 1;
            }
        } else {
            for (uint32_t i = 0; i < nfat; i++) {
                uint32_t cputype = read32_u(arch[i].cputype, swapped);
                if (cputype == (uint32_t)CPU_TYPE_ARM64) { pick = (int)i; break; }
            }
            if (pick < 0) pick = 0;
        }

        uint64_t off  = read64_u(arch[pick].offset, swapped);
        uint64_t size = read64_u(arch[pick].size, swapped);
        uint32_t cputype = read32_u(arch[pick].cputype, swapped);

        printf("picked slice %d: cputype=%u (%s) offset=%llu size=%llu\n",
               pick, cputype, cpu_type_name(cputype),
               (unsigned long long)off,
               (unsigned long long)size);

        if (off + size > (uint64_t)sz) {
            fprintf(stderr, "error: slice out of bounds\n");
            return 1;
        }

        // size might be > 4GB; we pass size_t after bounds check (ok on 64-bit hosts)
        if (size < sizeof(uint32_t)) {
            fprintf(stderr, "error: slice too small\n");
            return 1;
        }
        uint32_t smagic = 0;
        memcpy(&smagic, buf + off, sizeof(smagic));
        if (smagic == MH_MAGIC_64 || smagic == MH_CIGAM_64) {
            return parse_thin_macho_64(buf + off, (size_t)size);
        }
        return parse_thin_macho_32(buf + off, (size_t)size);
    }

    fprintf(stderr, "error: unknown fat magic 0x%08x\n", magic);
    return 1;
}

int main(int argc, char **argv) {
    struct parse_opts opts;
    memset(&opts, 0, sizeof(opts));
    const char *path = NULL;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--list") == 0) {
            opts.list_only = 1;
        } else if (strcmp(argv[i], "--slice") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "error: --slice requires an argument\n");
                return 2;
            }
            opts.have_slice = 1;
            opts.slice_index = (uint32_t)strtoul(argv[++i], NULL, 0);
        } else if (strcmp(argv[i], "--arch") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "error: --arch requires an argument\n");
                return 2;
            }
            if (!parse_cputype(argv[i + 1], &opts.arch)) {
                fprintf(stderr, "error: unknown arch '%s'\n", argv[i + 1]);
                return 2;
            }
            opts.have_arch = 1;
            i++;
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            printf("usage: %s [--list] [--slice N | --arch NAME|CPU] <mach-o file>\n", argv[0]);
            return 0;
        } else if (argv[i][0] == '-') {
            fprintf(stderr, "error: unknown option '%s'\n", argv[i]);
            return 2;
        } else {
            path = argv[i];
        }
    }

    if (!path) {
        fprintf(stderr, "usage: %s [--list] [--slice N | --arch NAME|CPU] <mach-o file>\n", argv[0]);
        return 2;
    }

    FILE *f = fopen(path, "rb");
    if (!f) { perror("open"); return 1; }

    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    rewind(f);

    if (n <= 0) {
        fprintf(stderr, "error: empty file\n");
        fclose(f);
        return 1;
    }

    uint8_t *buf = (uint8_t*)malloc((size_t)n);
    if (!buf) { perror("malloc"); fclose(f); return 1; }

    if (fread(buf, 1, (size_t)n, f) != (size_t)n) {
        perror("fread");
        fclose(f);
        free(buf);
        return 1;
    }
    fclose(f);

    uint32_t magic = 0;
    memcpy(&magic, buf, sizeof(magic));

    int rc;
    if (is_fat_magic(magic)) {
        rc = parse_fat(buf, (size_t)n, &opts);
    } else if (magic == MH_MAGIC || magic == MH_CIGAM) {
        rc = parse_thin_macho_32(buf, (size_t)n);
    } else {
        rc = parse_thin_macho_64(buf, (size_t)n);
    }

    free(buf);
    return rc;
}
