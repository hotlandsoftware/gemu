#pragma once
#include <stddef.h>

/* Executable memory for the JIT backends (jit_mem.c).  NULL when the host
 * refuses it. */
void *gemu_jit_alloc_exec(size_t size);
void  gemu_jit_free_exec(void *p, size_t size);
