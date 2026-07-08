#include "gemu/memory.h"
#include <stdio.h>

bool gemu_mem_load_file(GemuMemory *m, uint32_t addr, const char *path, size_t *out_len) {
    FILE *f = fopen(path, "rb");
    if (!f) return false;

    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    rewind(f);

    if (sz < 0 || (size_t)(addr + sz) > m->size) {
        fclose(f);
        return false;
    }

    size_t read = fread(m->data + addr, 1, (size_t)sz, f);
    fclose(f);

    if (out_len) *out_len = read;
    return read == (size_t)sz;
}
