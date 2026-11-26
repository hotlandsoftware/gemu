/*
 * 86Box v6.0's io.h for GEMU's 86Box layer (compat/86box).
 *
 * io_sethandler() puts a driver's handlers on ports of the machine's "io"
 * space as SHARED mappings, so they stack with other handlers on the same
 * port as in 86Box (writes go to all, reads are ANDed).  A width the driver
 * doesn't handle is made of narrower accesses.
 * io_removehandler() takes off, on each port, the first handler with the
 * same functions and priv.  The CPU-side calls (inb(), outb(), ...) and the
 * I/O traps are not here.
 *
 * From 86Box (src/include/86box/io.h), GPLv2 or later:
 *   Copyright 2008-2017 Sarah Walker.  Copyright 2016-2017 Miran Grca.
 */
#ifndef EMU_IO_H
#define EMU_IO_H
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

extern void io_sethandler_common(uint16_t base, uint16_t size,
                                 uint8_t (*inb)(uint16_t port, void *priv),
                                 uint16_t (*inw)(uint16_t port, void *priv),
                                 uint32_t (*inl)(uint16_t port, void *priv),
                                 void (*outb)(uint16_t port, uint8_t val, void *priv),
                                 void (*outw)(uint16_t port, uint16_t val, void *priv),
                                 void (*outl)(uint16_t port, uint32_t val, void *priv),
                                 void *priv, uint8_t step);

extern void io_removehandler_common(uint16_t base, uint16_t size,
                                    uint8_t (*inb)(uint16_t port, void *priv),
                                    uint16_t (*inw)(uint16_t port, void *priv),
                                    uint32_t (*inl)(uint16_t port, void *priv),
                                    void (*outb)(uint16_t port, uint8_t val, void *priv),
                                    void (*outw)(uint16_t port, uint16_t val, void *priv),
                                    void (*outl)(uint16_t port, uint32_t val, void *priv),
                                    void *priv, uint8_t step);

extern void io_handler_common(uint8_t set, uint16_t base, uint16_t size,
                              uint8_t (*inb)(uint16_t port, void *priv),
                              uint16_t (*inw)(uint16_t port, void *priv),
                              uint32_t (*inl)(uint16_t port, void *priv),
                              void (*outb)(uint16_t port, uint8_t val, void *priv),
                              void (*outw)(uint16_t port, uint16_t val, void *priv),
                              void (*outl)(uint16_t port, uint32_t val, void *priv),
                              void *priv, uint8_t step);

extern void io_sethandler(uint16_t base, uint16_t size,
                          uint8_t (*inb)(uint16_t port, void *priv),
                          uint16_t (*inw)(uint16_t port, void *priv),
                          uint32_t (*inl)(uint16_t port, void *priv),
                          void (*outb)(uint16_t port, uint8_t val, void *priv),
                          void (*outw)(uint16_t port, uint16_t val, void *priv),
                          void (*outl)(uint16_t port, uint32_t val, void *priv),
                          void *priv);

extern void io_removehandler(uint16_t base, uint16_t size,
                             uint8_t (*inb)(uint16_t port, void *priv),
                             uint16_t (*inw)(uint16_t port, void *priv),
                             uint32_t (*inl)(uint16_t port, void *priv),
                             void (*outb)(uint16_t port, uint8_t val, void *priv),
                             void (*outw)(uint16_t port, uint16_t val, void *priv),
                             void (*outl)(uint16_t port, uint32_t val, void *priv),
                             void *priv);

extern void io_handler(uint8_t set, uint16_t base, uint16_t size,
                       uint8_t (*inb)(uint16_t port, void *priv),
                       uint16_t (*inw)(uint16_t port, void *priv),
                       uint32_t (*inl)(uint16_t port, void *priv),
                       void (*outb)(uint16_t port, uint8_t val, void *priv),
                       void (*outw)(uint16_t port, uint16_t val, void *priv),
                       void (*outl)(uint16_t port, uint32_t val, void *priv),
                       void *priv);

#ifdef __cplusplus
}
#endif

#endif /*EMU_IO_H*/
