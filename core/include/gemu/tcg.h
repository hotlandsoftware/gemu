#pragma once
#include <stdint.h>
#include <stddef.h>

/*
 * Translation Block Cache - core of gemu's dynamic code translation.
 *
 * Guest code is divided into basic blocks (TBs): a linear sequence of
 * instructions from a starting PC up to and including the first branch.
 * TBs are decoded once and cached in a hash table keyed on guest PC.
 *
 * Roadmap: today TBs hold pre-decoded structs (fast interpreter dispatch).
 * In a future JIT tier, TBs will hold native host code in mmap'd executable
 * memory. The cache management layer stays the same either way.
 */

#define GEMU_TB_HASH_BITS 10
#define GEMU_TB_HASH_SIZE (1u << GEMU_TB_HASH_BITS)
#define GEMU_TB_HASH_MASK (GEMU_TB_HASH_SIZE - 1u)

#define GEMU_TB_MAX_INSNS 64

typedef struct GemuTb {
    uint32_t        guest_pc;   /* first guest instruction address */
    uint32_t        guest_size; /* covered bytes (n_insns * insn_width) */
    uint32_t        n_insns;    /* number of decoded instructions */
    void           *insns;      /* platform-allocated decoded insn array */
    uint32_t        exec_count; /* hot-path hit counter */
    struct GemuTb  *hash_next;  /* chaining within hash bucket */
} GemuTb;

typedef struct GemuTbCache {
    GemuTb  *buckets[GEMU_TB_HASH_SIZE];
    uint32_t n_tbs;
    uint32_t n_insns_total;
    void    (*free_insns)(void *insns); /* platform-provided destructor */
} GemuTbCache;

void    gemu_tb_cache_init(GemuTbCache *cache, void (*free_insns)(void *));
void    gemu_tb_cache_flush(GemuTbCache *cache);

GemuTb *gemu_tb_lookup(const GemuTbCache *cache, uint32_t pc);
GemuTb *gemu_tb_insert(GemuTbCache *cache, uint32_t pc,
                       uint32_t guest_size, uint32_t n_insns, void *insns);
