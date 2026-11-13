/* Executable memory for gemu's JIT backends (see gemu/jit_x64.h). */
#include "gemu/jit_x64.h"

#ifdef _WIN32
#include <windows.h>

void *gemu_jit_alloc_exec(size_t size) {
    return VirtualAlloc(NULL, size, MEM_COMMIT | MEM_RESERVE,
                        PAGE_EXECUTE_READWRITE);
}

void gemu_jit_free_exec(void *p, size_t size) {
    (void)size;
    if (p) VirtualFree(p, 0, MEM_RELEASE);
}

#else
#include <sys/mman.h>

void *gemu_jit_alloc_exec(size_t size) {
    void *p = mmap(NULL, size, PROT_READ | PROT_WRITE | PROT_EXEC,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    return p == MAP_FAILED ? NULL : p;
}

void gemu_jit_free_exec(void *p, size_t size) {
    if (p) munmap(p, size);
}
#endif
