#pragma once
#include <stdint.h>

/*
 * octo_compile - assemble an Octo (.8o) source string into CHIP-8 bytecode.
 *
 * out      : caller-allocated buffer to receive bytecode (loaded at 0x200)
 * out_max  : size of that buffer
 * errbuf   : filled with a human-readable error message on failure
 * Returns  : number of bytes written, or -1 on error
 */
int octo_compile(const char *source, uint8_t *out, int out_max,
                 char *errbuf, int errbuf_size);
