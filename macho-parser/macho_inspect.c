#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

#include "../include/macho/loader.h"
#include "../include/macho/fat.h"

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

static const char *lc_name(uint32_t c) {
    switch(c) {
        case LC_SEGMENT_64: return "LC_SEGMENT_64";
        case LC_LOAD_DYLIB: return "LC_LOAD_DYLIB";
        case LC_LOAD_WEAK_DYLIB: return "LC_LOAD_WEAK_DYLIB";
        case LC_REEXPORT_DYLIB: return "LC_REEXPORT_DYLIB";
        case LC_MAIN: return "LC_MAIN";
        case LC_UUID: return "LC_UUID";
        case LC_RPATH: return "LC_RPATH";
        default: return "LC_OTHER";
    }
}

static int parse_thin_macho_64(const uint8_t *buf, size_t sz) {
    if (sz < sizeof(struct mach_header_64)) {
        fprintf(stderr, "error: file too small for mach_header_64\n");
        return 1;
    }

    const struct mach_header_64 *h = (const struct mach_header_64*)buf;

    if (h->magic != MH_MAGIC_64) {
        fprintf(stderr, "error: not MH_MAGIC_64 (0x%08x). (32-bit or other?)\n", h->magic);
        return 1;
    }

    printf("== Thin Mach-O (64-bit) ==\n");
    printf("CPU type: %d\n", h->cputype);
    printf("Load commands: %u\n", h->ncmds);

    // Load commands start immediately after header
    const uint8_t *p = buf + sizeof(struct mach_header_64);
    const uint8_t *end = buf + sz;

    for (uint32_t i = 0; i < h->ncmds; i++) {
        if (p + sizeof(struct load_command) > end) {
            fprintf(stderr, "error: truncated load command %u\n", i);
            return 1;
        }

        const struct load_command *lc = (const struct load_command*)p;
        if (lc->cmdsize < sizeof(struct load_command)) {
            fprintf(stderr, "error: invalid cmdsize at %u\n", i);
            return 1;
        }
        if (p + lc->cmdsize > end) {
            fprintf(stderr, "error: load command %u extends beyond file\n", i);
            return 1;
        }

        printf("[%2u] %-14s (0x%x) size=%u\n", i, lc_name(lc->cmd), lc->cmd, lc->cmdsize);

        if (lc->cmd == LC_SEGMENT_64) {
            const struct segment_command_64 *s = (const struct segment_command_64*)p;
            printf("     SEG %-16s vm=0x%llx size=0x%llx fileoff=0x%llx filesize=0x%llx nsects=%u\n",
                   s->segname,
                   (unsigned long long)s->vmaddr,
                   (unsigned long long)s->vmsize,
                   (unsigned long long)s->fileoff,
                   (unsigned long long)s->filesize,
                   s->nsects);
        }

        if (lc->cmd == LC_MAIN) {
            const struct entry_point_command *ep = (const struct entry_point_command*)p;
            printf("     entryoff=0x%llx stacksize=0x%llx\n",
                   (unsigned long long)ep->entryoff,
                   (unsigned long long)ep->stacksize);
        }

        p += lc->cmdsize;
    }

    return 0;
}

static int is_fat_magic(uint32_t m) {
    return (m == FAT_MAGIC || m == FAT_CIGAM || m == FAT_MAGIC_64 || m == FAT_CIGAM_64);
}

static int parse_fat(const uint8_t *buf, size_t sz) {
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

        // Pick a slice: prefer ARM64 if present, otherwise first slice.
        int pick = -1;
        for (uint32_t i = 0; i < nfat; i++) {
            uint32_t cputype = swapped ? bswap32_u(arch[i].cputype) : arch[i].cputype;
            if (cputype == (uint32_t)CPU_TYPE_ARM64) { pick = (int)i; break; }
        }
        if (pick < 0) pick = 0;

        uint32_t off  = swapped ? bswap32_u(arch[pick].offset) : arch[pick].offset;
        uint32_t size = swapped ? bswap32_u(arch[pick].size)   : arch[pick].size;
        uint32_t cputype = swapped ? bswap32_u(arch[pick].cputype) : arch[pick].cputype;

        printf("picked slice %d: cputype=%u offset=%u size=%u\n", pick, cputype, off, size);

        if ((uint64_t)off + (uint64_t)size > (uint64_t)sz) {
            fprintf(stderr, "error: slice out of bounds\n");
            return 1;
        }

        return parse_thin_macho_64(buf + off, size);
    }

    // FAT64
    if (magic == FAT_MAGIC_64 || magic == FAT_CIGAM_64) {
        size_t need = sizeof(struct fat_header) + (size_t)nfat * sizeof(struct fat_arch_64);
        if (sz < need) {
            fprintf(stderr, "error: truncated fat_arch_64 table\n");
            return 1;
        }

        const struct fat_arch_64 *arch = (const struct fat_arch_64*)(buf + sizeof(struct fat_header));

        int pick = -1;
        for (uint32_t i = 0; i < nfat; i++) {
            uint32_t cputype = swapped ? bswap32_u(arch[i].cputype) : arch[i].cputype;
            if (cputype == (uint32_t)CPU_TYPE_ARM64) { pick = (int)i; break; }
        }
        if (pick < 0) pick = 0;

        uint64_t off  = swapped ? bswap64_u(arch[pick].offset) : arch[pick].offset;
        uint64_t size = swapped ? bswap64_u(arch[pick].size)   : arch[pick].size;
        uint32_t cputype = swapped ? bswap32_u(arch[pick].cputype) : arch[pick].cputype;

        printf("picked slice %d: cputype=%u offset=%llu size=%llu\n",
               pick, cputype,
               (unsigned long long)off,
               (unsigned long long)size);

        if (off + size > (uint64_t)sz) {
            fprintf(stderr, "error: slice out of bounds\n");
            return 1;
        }

        // size might be > 4GB; we pass size_t after bounds check (ok on 64-bit hosts)
        return parse_thin_macho_64(buf + off, (size_t)size);
    }

    fprintf(stderr, "error: unknown fat magic 0x%08x\n", magic);
    return 1;
}

int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: %s <mach-o file>\n", argv[0]);
        return 2;
    }

    const char *path = argv[1];
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

    uint32_t magic = *(uint32_t*)buf;

    int rc;
    if (is_fat_magic(magic)) {
        rc = parse_fat(buf, (size_t)n);
    } else {
        rc = parse_thin_macho_64(buf, (size_t)n);
    }

    free(buf);
    return rc;
}
