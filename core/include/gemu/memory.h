#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

typedef struct GemuMemory {
    uint8_t *data;
    size_t   size;
} GemuMemory;

static inline uint8_t gemu_mem_read8(const GemuMemory *m, uint32_t addr) {
    return m->data[addr & (m->size - 1)];
}

static inline void gemu_mem_write8(GemuMemory *m, uint32_t addr, uint8_t val) {
    m->data[addr & (m->size - 1)] = val;
}

bool gemu_mem_load_file(GemuMemory *mem, uint32_t addr, const char *path, size_t *out_len);
