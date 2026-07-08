#include "gemu/tcg.h"
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

static uint32_t tb_hash(uint32_t pc) {
    /* Knuth multiplicative hash, shift down to index width */
    return (pc * 2654435761u) >> (32 - GEMU_TB_HASH_BITS);
}

void gemu_tb_cache_init(GemuTbCache *cache, void (*free_insns)(void *)) {
    memset(cache, 0, sizeof(*cache));
    cache->free_insns = free_insns;
}

void gemu_tb_cache_flush(GemuTbCache *cache) {
    for (uint32_t i = 0; i < GEMU_TB_HASH_SIZE; i++) {
        GemuTb *tb = cache->buckets[i];
        while (tb) {
            GemuTb *next = tb->hash_next;
            if (cache->free_insns && tb->insns)
                cache->free_insns(tb->insns);
            free(tb);
            tb = next;
        }
        cache->buckets[i] = NULL;
    }
    cache->n_tbs = 0;
    cache->n_insns_total = 0;
}

GemuTb *gemu_tb_lookup(const GemuTbCache *cache, uint32_t pc) {
    GemuTb *tb = cache->buckets[tb_hash(pc)];
    while (tb) {
        if (tb->guest_pc == pc) return tb;
        tb = tb->hash_next;
    }
    return NULL;
}

GemuTb *gemu_tb_insert(GemuTbCache *cache, uint32_t pc,
                       uint32_t guest_size, uint32_t n_insns, void *insns) {
    GemuTb *tb = malloc(sizeof(*tb));
    if (!tb) return NULL;

    tb->guest_pc   = pc;
    tb->guest_size = guest_size;
    tb->n_insns    = n_insns;
    tb->insns      = insns;
    tb->exec_count = 0;

    uint32_t bucket  = tb_hash(pc);
    tb->hash_next    = cache->buckets[bucket];
    cache->buckets[bucket] = tb;

    cache->n_tbs++;
    cache->n_insns_total += n_insns;
    return tb;
}
