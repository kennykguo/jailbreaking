#ifndef ARM64_PATCHER_H
#define ARM64_PATCHER_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// Encode a 26-bit immediate branch (B) from src to dst. Returns 0 on success.
int arm64_encode_b(uint8_t *src, const uint8_t *dst, uint32_t *out_insn);

// Patch one instruction at src with an unconditional branch to dst.
int arm64_patch_b(void *src, const void *dst);

// Build a minimal trampoline: copy one instruction and branch back.
// Returns pointer to trampoline or NULL on failure.
void *arm64_make_trampoline(void *src, size_t insn_bytes);

// High-level prologue patch: redirect src to hook and return trampoline.
void *arm64_patch_prologue(void *src, void *hook, size_t insn_bytes);

// Hot-patch engine: make page writable, apply patch, flush cache.
int arm64_hotpatch(void *addr, const uint8_t *bytes, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* ARM64_PATCHER_H */
