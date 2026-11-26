/*
 * 86Box v6.0's mem.h, the memory mappings, for GEMU's 86Box layer
 * (compat/86box).
 *
 * A mem_mapping_t puts a driver's handlers (and/or its memory, `exec`) on a
 * range of the machine's ISA memory: the space "isa-memory" if the machine
 * has one (the 5150: what its own decode doesn't claim), else "program".
 * Handlers get the absolute address, as in 86Box.  Reads use read_b/w/l,
 * or `exec` when there are none; writes use write_b/w/l (none: the write
 * goes to what's below).  Where mappings overlap, the one added (or moved,
 * enabled, given handlers) last wins, as in 86Box.  The mapping follows the
 * structure: changing base/size/enable through these calls - or a load
 * restoring the structure - moves it.  The CPU-side calls (readmemb, ...),
 * paging and shadow RAM are not here.
 *
 * From 86Box (src/include/86box/mem.h), GPLv2 or later:
 *   Copyright 2008-2020 Sarah Walker.  Copyright 2016-2020 Miran Grca.
 *   Copyright 2017-2020 Fred N. van Kempen.
 */
#ifndef EMU_MEM_H
#define EMU_MEM_H
#include <stdint.h>

#define MEM_MAPPING_EXTERNAL  1  /* On external bus (ISA/PCI). */
#define MEM_MAPPING_INTERNAL  2  /* On internal bus (RAM). */
#define MEM_MAPPING_ROM_WS    4  /* Executing from ROM may involve additional wait states. */
#define MEM_MAPPING_IS_ROM    8  /* Responds to ROMCS#. */
#define MEM_MAPPING_ROM       (MEM_MAPPING_ROM_WS | MEM_MAPPING_IS_ROM)
#define MEM_MAPPING_ROMCS     16 /* If it responds to ROMCS#, it requires ROMCS# asserted. */

#define MEM_GRANULARITY_BITS   12
#define MEM_GRANULARITY_SIZE   (1 << MEM_GRANULARITY_BITS)
#define MEM_GRANULARITY_HBOUND (MEM_GRANULARITY_SIZE - 2)
#define MEM_GRANULARITY_QBOUND (MEM_GRANULARITY_SIZE - 4)
#define MEM_GRANULARITY_MASK   (MEM_GRANULARITY_SIZE - 1)
#define MEM_GRANULARITY_PAGE   (MEM_GRANULARITY_MASK & ~0xfff)
#define MEM_GRANULARITY_BASE   (~MEM_GRANULARITY_MASK)

typedef struct _mem_mapping_ {
    struct _mem_mapping_ *prev;
    struct _mem_mapping_ *next;

    int enable;

    uint32_t base;
    uint32_t size;

    uint32_t base_ignore;
    uint32_t mask;

    uint8_t (*read_b)(uint32_t addr, void *priv);
    uint16_t (*read_w)(uint32_t addr, void *priv);
    uint32_t (*read_l)(uint32_t addr, void *priv);
    void (*write_b)(uint32_t addr, uint8_t val, void *priv);
    void (*write_w)(uint32_t addr, uint16_t val, void *priv);
    void (*write_l)(uint32_t addr, uint32_t val, void *priv);

    uint8_t *exec;

    uint32_t flags;

    void *priv; /* backpointer to device */

    void *gemu; /* GEMU: the layer's record (its core mapping); not v6.0's */
} mem_mapping_t;

#ifdef __cplusplus
extern "C" {
#endif

extern void mem_mapping_add(mem_mapping_t *, uint32_t base, uint32_t size,
                            uint8_t (*read_b)(uint32_t addr, void *priv),
                            uint16_t (*read_w)(uint32_t addr, void *priv),
                            uint32_t (*read_l)(uint32_t addr, void *priv),
                            void (*write_b)(uint32_t addr, uint8_t val, void *priv),
                            void (*write_w)(uint32_t addr, uint16_t val, void *priv),
                            void (*write_l)(uint32_t addr, uint32_t val, void *priv),
                            uint8_t *exec, uint32_t flags, void *priv);
extern void mem_mapping_set(mem_mapping_t *, uint32_t base, uint32_t size,
                            uint8_t (*read_b)(uint32_t addr, void *priv),
                            uint16_t (*read_w)(uint32_t addr, void *priv),
                            uint32_t (*read_l)(uint32_t addr, void *priv),
                            void (*write_b)(uint32_t addr, uint8_t val, void *priv),
                            void (*write_w)(uint32_t addr, uint16_t val, void *priv),
                            void (*write_l)(uint32_t addr, uint32_t val, void *priv),
                            uint8_t *exec, uint32_t flags, void *priv);
extern void mem_mapping_set_handler(mem_mapping_t *,
                                    uint8_t (*read_b)(uint32_t addr, void *priv),
                                    uint16_t (*read_w)(uint32_t addr, void *priv),
                                    uint32_t (*read_l)(uint32_t addr, void *priv),
                                    void (*write_b)(uint32_t addr, uint8_t val, void *priv),
                                    void (*write_w)(uint32_t addr, uint16_t val, void *priv),
                                    void (*write_l)(uint32_t addr, uint32_t val, void *priv));
extern void mem_mapping_set_write_handler(mem_mapping_t *,
                                          void (*write_b)(uint32_t addr, uint8_t val, void *priv),
                                          void (*write_w)(uint32_t addr, uint16_t val, void *priv),
                                          void (*write_l)(uint32_t addr, uint32_t val, void *priv));
extern void mem_mapping_set_p(mem_mapping_t *, void *priv);
extern void mem_mapping_set_addr(mem_mapping_t *, uint32_t base, uint32_t size);
extern void mem_mapping_set_exec(mem_mapping_t *, uint8_t *exec);
extern void mem_mapping_disable(mem_mapping_t *);
extern void mem_mapping_enable(mem_mapping_t *);

#ifdef __cplusplus
}
#endif

#endif /*EMU_MEM_H*/
