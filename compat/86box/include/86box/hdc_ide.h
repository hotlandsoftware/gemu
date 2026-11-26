/*
 * 86Box v6.0's hdc_ide.h, the part sound cards use, for GEMU's 86Box layer
 * (compat/86box).  No 86Box IDE here: these stop GEMU with a message (only
 * the models of a card with an IDE interface call them).
 *
 * From 86Box (src/include/86box/hdc_ide.h), GPLv2 or later.
 */
#ifndef EMU_HDC_IDE_H
#define EMU_HDC_IDE_H
#include <stdint.h>
#include <86box/isapnp.h>

extern void ide_set_base_addr(int board, int base, uint16_t port);
extern void ide_set_irq(int board, int irq);
extern void ide_handlers(uint8_t board, int set);
extern void ide_pnp_config_changed(uint8_t ld, isapnp_device_config_t *config, void *priv);
extern void ide_pnp_config_changed_1addr(uint8_t ld, isapnp_device_config_t *config, void *priv);

#define ide_set_base(board, port)  ide_set_base_addr(board, 0, port)
#define ide_set_side(board, port)  ide_set_base_addr(board, 1, port)
#define ide_remove_handlers(board) ide_handlers(board, 0)
#define ide_set_handlers(board)    ide_handlers(board, 1)

#endif /*EMU_HDC_IDE_H*/
