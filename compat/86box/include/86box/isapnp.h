/*
 * 86Box v6.0's isapnp.h, the part cards use, for GEMU's 86Box layer
 * (compat/86box).  No GEMU machine has ISA Plug and Play: isapnp_add_card()
 * stops GEMU with a message (only the PnP models of a card call it).
 *
 * From 86Box (src/include/86box/isapnp.h), GPLv2 or later.
 */
#ifndef EMU_ISAPNP_H
#define EMU_ISAPNP_H
#include <stdint.h>

#define ISAPNP_IO_DISABLED  0
#define ISAPNP_IRQ_DISABLED 0
#define ISAPNP_DMA_DISABLED 4

typedef struct isapnp_device_config_t {
    uint8_t activate;
    struct pnp_mem_t {
        uint32_t base : 24;
        uint32_t size : 24;
    } mem[4];
    struct pnp_mem32_t {
        uint32_t base;
        uint32_t size;
    } mem32[4];
    struct pnp_io_t {
        uint16_t base;
    } io[8];
    struct pnp_irq_t {
        uint8_t irq : 4;
        uint8_t level : 1;
        uint8_t type : 1;
    } irq[2];
    struct pnp_dma_t {
        uint8_t dma : 3;
    } dma[2];
} isapnp_device_config_t;

extern void *isapnp_add_card(uint8_t *rom, uint16_t rom_size,
                             void (*config_changed)(uint8_t ld, isapnp_device_config_t *config, void *priv),
                             void (*csn_changed)(uint8_t csn, void *priv),
                             uint8_t (*read_vendor_reg)(uint8_t ld, uint8_t reg, void *priv),
                             void (*write_vendor_reg)(uint8_t ld, uint8_t reg, uint8_t val, void *priv),
                             void *priv);

#endif /*EMU_ISAPNP_H*/
